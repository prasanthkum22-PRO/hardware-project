/*
 * ============================================================
 *  Smart Parking Management System — ESP32 IoT BRIDGE
 * ============================================================
 *
 *  ROLE  : Wi-Fi / IoT middleman
 *          - Receives slot data from STM32 over UART
 *          - Pushes live slot status to Firebase Realtime DB
 *          - Listens to Firebase for website button commands
 *          - Forwards website commands to STM32 over UART
 *
 *  Board : ESP32 DevKit V1 (or any ESP32 with UART2)
 *  IDE   : Arduino IDE  (board: "ESP32 Dev Module")
 *
 *  Libraries (Library Manager):
 *    - Firebase ESP32 Client by Mobizt  (search "Firebase ESP32 Client")
 *    - ArduinoJson by Benoit Blanchon   (search "ArduinoJson")
 *
 * ============================================================
 *  FULL PIN CONNECTION TABLE  (ESP32 side)
 *  ──────────────────────────────────────────────────────────
 *
 *  ── UART to STM32 ──
 *  ESP32 GPIO16 (RX2)  →  STM32 PA2 (TX/USART2) — via 1kΩ series on this pin
 *  ESP32 GPIO17 (TX2)  →  STM32 PA3 (RX/USART2)
 *  ESP32 GND           →  STM32 GND  (COMMON GROUND — mandatory)
 *
 *  ── Power ──
 *  ESP32 powered via USB or 3.3V regulated supply
 *  Do NOT connect ESP32 5V pin to STM32 3.3V pin
 *
 *  NOTE: Both boards run at 3.3V logic — no level shifting needed
 *        for TX lines, but a 1kΩ series resistor on ESP32 RX2
 *        protects against any brief overvoltage transient.
 *
 * ============================================================
 *  FIREBASE REALTIME DATABASE STRUCTURE
 *
 *  parking/
 *    totalCars        : 3
 *    freeSlots        : 7
 *    slots/
 *      slot1          : true   (true=occupied, false=free)
 *      slot2          : false
 *      ...
 *      slot10         : true
 *    commands/
 *      clearSlot      : 0      (website writes slot number here)
 *      clearAll       : false  (website writes true here)
 *      openEntry      : false
 *      openExit       : false
 *    gates/
 *      entryState     : "CLOSED"
 *      exitState      : "CLOSED"
 *
 * ============================================================
 *  FIREBASE AUTH NOTE
 *  This sketch uses email/password auth (legacy database-secret
 *  auth is deprecated). You must:
 *    1. Firebase Console → Authentication → Sign-in method →
 *       enable "Email/Password"
 *    2. Firebase Console → Authentication → Users → Add user,
 *       with the exact email/password defined below
 *  Otherwise sign-in will fail at runtime.
 * ============================================================
 */

#include <ArduinoJson.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ─────────────────────────────────────────────────────────────
//  !! CHANGE THESE to your credentials !!
// ─────────────────────────────────────────────────────────────
#define WIFI_SSID     "prasan"
#define WIFI_PASSWORD "123456789"

// Firebase project settings
// API_KEY      -> Project settings -> General -> Web API Key
// DATABASE_URL -> Realtime Database -> the https://xxx.firebasedatabase.app URL
// USER_EMAIL / USER_PASSWORD -> a user under Authentication -> Email/Password
#define API_KEY       "AIzaSyBF2wGjMcwagnvjLw6x8pOsvLJqA6F3r0Q"
#define DATABASE_URL  "https://finger-attendance--esp-32-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define USER_EMAIL    "prasanth.kum22@gmail.com"
#define USER_PASSWORD "project8"
// ─────────────────────────────────────────────────────────────

// ── UART2 pins for STM32 communication ───────────────────────
#define STM32_RX2 16           // ESP32 GPIO16 receives from STM32 TX
#define STM32_TX2 17           // ESP32 GPIO17 sends to STM32 RX
HardwareSerial stm32Serial(2); // UART2

// ── Firebase objects ──────────────────────────────────────────
FirebaseData fbData;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;
bool firebaseReady = false;

// ── Local parking state mirror ────────────────────────────────
#define MAX_SLOTS 10
bool slotState[MAX_SLOTS + 1]; // index 1–10
int totalCars = 0;

// ── Timing ────────────────────────────────────────────────────
#define FIREBASE_POLL_MS 2000UL // check Firebase commands every 2 s
#define FIREBASE_PUSH_MS 3000UL // push slot status every 3 s
unsigned long lastFbPoll = 0;
unsigned long lastFbPush = 0;

// ─────────────────────────────────────────────────────────────
//  Connect to Wi-Fi
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[ESP32] Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[ESP32] WiFi connected. IP: " +
                    WiFi.localIP().toString());
  } else {
    Serial.println("\n[ESP32] WiFi FAILED — will retry in loop");
  }
}

// ─────────────────────────────────────────────────────────────
//  Push full parking state to Firebase
// ─────────────────────────────────────────────────────────────
void pushStateToFirebase() {
  if (WiFi.status() != WL_CONNECTED || !firebaseReady)
    return;

  Firebase.RTDB.setInt(&fbData, "/parking/totalCars", totalCars);
  Firebase.RTDB.setInt(&fbData, "/parking/freeSlots", MAX_SLOTS - totalCars);

  for (int i = 1; i <= MAX_SLOTS; i++) {
    String path = "/parking/slots/slot" + String(i);
    Firebase.RTDB.setBool(&fbData, path, slotState[i]);
  }
  Serial.println("[ESP32] Slot state pushed to Firebase");
}

// ─────────────────────────────────────────────────────────────
//  Push gate state update to Firebase
// ─────────────────────────────────────────────────────────────
void pushGateState(String gate, String state) {
  if (WiFi.status() != WL_CONNECTED || !firebaseReady)
    return;
  String path = "/parking/gates/" + gate + "State";
  Firebase.RTDB.setString(&fbData, path, state);
}

// ─────────────────────────────────────────────────────────────
//  Read and act on Firebase command fields (set by website)
//  Commands are one-shot flags: website writes value, ESP32
//  reads → forwards to STM32 → resets the flag back to idle.
// ─────────────────────────────────────────────────────────────
void pollFirebaseCommands() {
  if (WiFi.status() != WL_CONNECTED || !firebaseReady)
    return;

  // ── Check clearAll flag ───────────────────────────────────
  if (Firebase.RTDB.getBool(&fbData, "/parking/commands/clearAll")) {
    if (fbData.boolData() == true) {
      Serial.println("[ESP32] Firebase CMD: CLEAR ALL");
      stm32Serial.println("CMD:CLEAR:ALL");
      Firebase.RTDB.setBool(&fbData, "/parking/commands/clearAll", false);
    }
  }

  // ── Check clearSlot (slot number, 0 = no command) ─────────
  if (Firebase.RTDB.getInt(&fbData, "/parking/commands/clearSlot")) {
    int slot = fbData.intData();
    if (slot > 0 && slot <= MAX_SLOTS) {
      Serial.print("[ESP32] Firebase CMD: CLEAR SLOT ");
      Serial.println(slot);
      stm32Serial.print("CMD:CLEAR:");
      stm32Serial.println(slot);
      Firebase.RTDB.setInt(&fbData, "/parking/commands/clearSlot", 0);
    }
  }

  // ── Check openEntry flag ──────────────────────────────────
  if (Firebase.RTDB.getBool(&fbData, "/parking/commands/openEntry")) {
    if (fbData.boolData() == true) {
      Serial.println("[ESP32] Firebase CMD: OPEN ENTRY GATE");
      stm32Serial.println("CMD:OPEN:ENTRY");
      Firebase.RTDB.setBool(&fbData, "/parking/commands/openEntry", false);
    }
  }

  // ── Check openExit flag ───────────────────────────────────
  if (Firebase.RTDB.getBool(&fbData, "/parking/commands/openExit")) {
    if (fbData.boolData() == true) {
      Serial.println("[ESP32] Firebase CMD: OPEN EXIT GATE");
      stm32Serial.println("CMD:OPEN:EXIT");
      Firebase.RTDB.setBool(&fbData, "/parking/commands/openExit", false);
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  Parse messages arriving from STM32 over UART
// ─────────────────────────────────────────────────────────────
void handleSTM32Message(String msg) {
  msg.trim();
  Serial.print("[ESP32] STM32 MSG: ");
  Serial.println(msg);

  // SLOT:n:IN — car entered slot n
  if (msg.startsWith("SLOT:") && msg.endsWith(":IN")) {
    int slot = msg.substring(5, msg.indexOf(":IN")).toInt();
    if (slot >= 1 && slot <= MAX_SLOTS) {
      slotState[slot] = true;
      totalCars = min(totalCars + 1, MAX_SLOTS);
      pushStateToFirebase();
    }
  }
  // SLOT:n:OUT — slot n cleared
  else if (msg.startsWith("SLOT:") && msg.endsWith(":OUT")) {
    int colonPos = msg.lastIndexOf(":OUT");
    int slot = msg.substring(5, colonPos).toInt();
    if (slot >= 1 && slot <= MAX_SLOTS) {
      slotState[slot] = false;
      totalCars = max(totalCars - 1, 0);
      pushStateToFirebase();
    }
  }
  // SLOTS:n,n,n — full occupied list (periodic sync)
  else if (msg.startsWith("SLOTS:")) {
    String data = msg.substring(6);
    for (int i = 1; i <= MAX_SLOTS; i++)
      slotState[i] = false;
    totalCars = 0;

    if (data != "EMPTY" && data != "CLEARED") {
      int start = 0;
      while (start < (int)data.length()) {
        int comma = data.indexOf(',', start);
        if (comma == -1)
          comma = data.length();
        int slot = data.substring(start, comma).toInt();
        if (slot >= 1 && slot <= MAX_SLOTS) {
          slotState[slot] = true;
          totalCars++;
        }
        start = comma + 1;
      }
    }
    pushStateToFirebase();
  }
  // GATE:ENTRY:OPEN / CLOSE
  else if (msg.startsWith("GATE:ENTRY:")) {
    String state = msg.substring(11);
    pushGateState("entry", state);
  }
  // GATE:EXIT:OPEN / CLOSE
  else if (msg.startsWith("GATE:EXIT:")) {
    String state = msg.substring(10);
    pushGateState("exit", state);
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[ESP32] Smart Parking IoT Bridge booting...");

  // UART2 to STM32
  stm32Serial.begin(115200, SERIAL_8N1, STM32_RX2, STM32_TX2);

  // Initialize slot state
  for (int i = 0; i <= MAX_SLOTS; i++)
    slotState[i] = false;

  // WiFi
  connectWiFi();

  // ── Firebase config (email/password auth) ──────────────────
  fbConfig.api_key = API_KEY;
  fbConfig.database_url = DATABASE_URL;

  fbAuth.user.email = USER_EMAIL;
  fbAuth.user.password = USER_PASSWORD;

  fbConfig.token_status_callback = tokenStatusCallback; // from TokenHelper.h

  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  fbData.setResponseSize(4096);

  Serial.println("[ESP32] Waiting for Firebase sign-in...");
  unsigned long signinStart = millis();
  while (!Firebase.ready() && millis() - signinStart < 15000) {
    delay(200);
    Serial.print(".");
  }

  if (Firebase.ready()) {
    firebaseReady = true;
    Serial.println("\n[ESP32] Firebase signed in OK.");

    // Initialize Firebase command flags to idle
    Firebase.RTDB.setInt(&fbData, "/parking/commands/clearSlot", 0);
    Firebase.RTDB.setBool(&fbData, "/parking/commands/clearAll", false);
    Firebase.RTDB.setBool(&fbData, "/parking/commands/openEntry", false);
    Firebase.RTDB.setBool(&fbData, "/parking/commands/openExit", false);
    Firebase.RTDB.setInt(&fbData, "/parking/totalCars", 0);
    Firebase.RTDB.setInt(&fbData, "/parking/freeSlots", MAX_SLOTS);
    for (int i = 1; i <= MAX_SLOTS; i++) {
      Firebase.RTDB.setBool(&fbData, "/parking/slots/slot" + String(i), false);
    }
    Serial.println("[ESP32] Firebase ready. System online.");
  } else {
    firebaseReady = false;
    Serial.println("\n[ESP32] Firebase sign-in FAILED — check "
                    "API_KEY / DATABASE_URL / USER_EMAIL / USER_PASSWORD, "
                    "and that Email/Password sign-in is enabled with this "
                    "user created in Firebase Console.");
  }
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. Read UART from STM32 ──────────────────────────────────
  while (stm32Serial.available()) {
    String line = stm32Serial.readStringUntil('\n');
    if (line.length() > 0)
      handleSTM32Message(line);
  }

  // ── 2. Poll Firebase for website commands ─────────────────────
  if (firebaseReady && now - lastFbPoll >= FIREBASE_POLL_MS) {
    lastFbPoll = now;
    pollFirebaseCommands();
  }

  // ── 3. Periodic Firebase state push (keep website in sync) ───
  if (firebaseReady && now - lastFbPush >= FIREBASE_PUSH_MS) {
    lastFbPush = now;
    pushStateToFirebase();
  }

  // ── 4. WiFi reconnect if dropped ──────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ESP32] WiFi disconnected — reconnecting...");
    connectWiFi();
  }

  // ── 5. Retry Firebase sign-in if it never succeeded ───────────
  if (!firebaseReady && WiFi.status() == WL_CONNECTED && Firebase.ready()) {
    firebaseReady = true;
    Serial.println("[ESP32] Firebase signed in (late).");
  }
}
