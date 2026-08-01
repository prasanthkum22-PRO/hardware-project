/*
 * ============================================================
 *  INTELLIGENT HOME SECURITY SYSTEM — ESP32 (CLOUD & NOTIFICATION)
 * ============================================================
 *
 *  WORKFLOW:
 *  Line 3: ESP32 receives INTRUDER alert from STM32 over UART →
 *          Sends instant Telegram message to owner's phone →
 *          Uploads event to Firebase Realtime Database →
 *          Owner can send /reset or /unlock commands back via Telegram.
 *
 * ============================================================
 *  PIN CONNECTIONS — ESP32 Dev Module
 * ============================================================
 *
 *  ── UART from STM32 ──
 *  ESP32 GPIO16 (RX2) ← STM32 PA2 (TX)  [1kΩ series resistor on this line]
 *  ESP32 GPIO17 (TX2) → STM32 PA3 (RX)
 *  GND                ←→ STM32 GND  (MUST share common ground)
 *
 *  ── No other hardware on ESP32 side ──
 *     All sensors, servo, buzzer, LED are on STM32.
 *     ESP32 is purely the cloud/notification bridge.
 *
 * ============================================================
 *  TELEGRAM BOT SETUP (one-time):
 *    1. Open Telegram → search @BotFather → /newbot
 *    2. Choose a name and username → copy the BOT TOKEN
 *    3. Start a chat with your new bot
 *    4. Open: https://api.telegram.org/bot<YOUR_TOKEN>/getUpdates
 *    5. Send any message to your bot, refresh the URL
 *    6. Find "chat":{"id": XXXXXXX} — that is your CHAT_ID
 *    7. Fill BOT_TOKEN and CHAT_ID below
 *
 *  TELEGRAM COMMANDS HANDLED:
 *    /status   → replies with current system status
 *    /reset    → sends CMD:RESET to STM32 (disarms alarm)
 *    /unlock   → sends CMD:UNLOCK to STM32 (opens door)
 *    /lock     → sends CMD:LOCK to STM32 (locks door)
 *    /arm      → re-arms the system (same as reset)
 *
 * ============================================================
 *  FIREBASE SETUP:
 *    1. console.firebase.google.com → New project
 *    2. Realtime Database → Create → Test mode
 *    3. Copy Database URL (https://xxx-default-rtdb.firebaseio.com)
 *    4. Project Settings → Service Accounts → Database secrets → copy key
 *    5. Fill FIREBASE_HOST (without https://) and FIREBASE_AUTH below
 *
 *  DATABASE STRUCTURE:
 *    /security/status          "ARMED" | "INTRUDER" | "ALARM_HOLD"
 *    /security/lastAlert       distance string e.g. "15.3 cm"
 *    /security/alertCount      int (total intrusions)
 *    /security/lastAlertTime   millis uptime string
 *    /security/door            "LOCKED" | "UNLOCKED"
 *
 * ============================================================
 *  Libraries — install ALL via Library Manager:
 *    1. UniversalTelegramBot   by Brian Lough
 *    2. ArduinoJson            by Benoit Blanchon  (v6.x)
 *    3. Firebase ESP32 Client  by Mobizt
 *
 *  Board: ESP32 Dev Module
 *  (Tools → Board → ESP32 Arduino → ESP32 Dev Module)
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <FirebaseESP32.h>

// ─────────────────────────────────────────────────────────────
//  ★ CONFIGURE THESE BEFORE UPLOADING
// ─────────────────────────────────────────────────────────────
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

#define BOT_TOKEN        "YOUR_TELEGRAM_BOT_TOKEN"    // from @BotFather
#define CHAT_ID          "YOUR_TELEGRAM_CHAT_ID"      // your numeric chat ID

#define FIREBASE_HOST    "YOUR_PROJECT-default-rtdb.firebaseio.com"  // no https://
#define FIREBASE_AUTH    "YOUR_DATABASE_SECRET_KEY"

// ─────────────────────────────────────────────────────────────
//  Hardware — UART to STM32
// ─────────────────────────────────────────────────────────────
#define STM32_RX_PIN  16    // ESP32 RX2 ← STM32 TX (PA2)
#define STM32_TX_PIN  17    // ESP32 TX2 → STM32 RX (PA3)
#define STM32_BAUD    115200

// ─────────────────────────────────────────────────────────────
//  Telegram poll interval — check for new messages every 2 s
//  (keep >= 2000 ms to avoid Telegram rate-limit)
// ─────────────────────────────────────────────────────────────
#define TELEGRAM_POLL_MS  2000UL
#define FIREBASE_PUSH_MS  3000UL

// ─────────────────────────────────────────────────────────────
//  Objects
// ─────────────────────────────────────────────────────────────
WiFiClientSecure  secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

FirebaseData   fbData;
FirebaseAuth   fbAuth;
FirebaseConfig fbConfig;

// ─────────────────────────────────────────────────────────────
//  System state (mirrored from STM32)
// ─────────────────────────────────────────────────────────────
String systemStatus  = "ARMED";
String doorStatus    = "LOCKED";
String lastAlert     = "None";
int    alertCount    = 0;
unsigned long lastAlertTime = 0;

// ─────────────────────────────────────────────────────────────
//  Timing
// ─────────────────────────────────────────────────────────────
unsigned long lastTelegramPoll  = 0;
unsigned long lastFirebasePush  = 0;

// ─────────────────────────────────────────────────────────────
//  Send command to STM32 over Serial2
// ─────────────────────────────────────────────────────────────
void sendToSTM32(String cmd) {
  Serial2.println(cmd);
  Serial.print("[ESP32→STM32] ");
  Serial.println(cmd);
}

// ─────────────────────────────────────────────────────────────
//  Send Telegram message (with WiFi check)
// ─────────────────────────────────────────────────────────────
void sendTelegram(String msg) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ESP32] WiFi not connected — Telegram skipped");
    return;
  }
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  if (ok) Serial.println("[ESP32] Telegram sent ✓");
  else    Serial.println("[ESP32] Telegram FAILED");
}

// ─────────────────────────────────────────────────────────────
//  Push state to Firebase
// ─────────────────────────────────────────────────────────────
void pushToFirebase() {
  if (!Firebase.ready()) return;

  Firebase.setString(fbData, "/security/status",        systemStatus);
  Firebase.setString(fbData, "/security/door",          doorStatus);
  Firebase.setString(fbData, "/security/lastAlert",     lastAlert);
  Firebase.setInt   (fbData, "/security/alertCount",    alertCount);
  Firebase.setString(fbData, "/security/lastAlertTime",
                     alertCount > 0 ? String(lastAlertTime / 1000) + "s uptime" : "Never");

  Serial.println("[ESP32] Firebase updated");
}

// ─────────────────────────────────────────────────────────────
//  Handle Telegram commands from the owner
// ─────────────────────────────────────────────────────────────
void handleTelegramMessages(int msgCount) {
  for (int i = 0; i < msgCount; i++) {
    String fromID = bot.messages[i].chat_id;
    String text   = bot.messages[i].text;
    text.trim();

    Serial.print("[ESP32] Telegram msg from ");
    Serial.print(fromID);
    Serial.print(": ");
    Serial.println(text);

    // Only respond to the authorised CHAT_ID
    if (fromID != String(CHAT_ID)) {
      bot.sendMessage(fromID, "Unauthorized.", "");
      continue;
    }

    if (text == "/status") {
      String reply = "🏠 Security Status\n";
      reply += "System : " + systemStatus + "\n";
      reply += "Door   : " + doorStatus   + "\n";
      reply += "Alerts : " + String(alertCount) + "\n";
      reply += "Last   : " + lastAlert;
      sendTelegram(reply);

    } else if (text == "/reset" || text == "/arm") {
      sendToSTM32("CMD:RESET");
      systemStatus = "ARMED";
      sendTelegram("✅ System RESET — monitoring resumed.");
      pushToFirebase();

    } else if (text == "/unlock") {
      sendToSTM32("CMD:UNLOCK");
      doorStatus = "UNLOCKED";
      sendTelegram("🔓 Door UNLOCKED.");
      pushToFirebase();

    } else if (text == "/lock") {
      sendToSTM32("CMD:LOCK");
      doorStatus = "LOCKED";
      sendTelegram("🔒 Door LOCKED.");
      pushToFirebase();

    } else if (text == "/start" || text == "/help") {
      String help = "🔐 Home Security Bot\n\n";
      help += "/status  — current system status\n";
      help += "/reset   — disarm alarm & re-arm monitoring\n";
      help += "/unlock  — unlock the door remotely\n";
      help += "/lock    — lock the door remotely\n";
      help += "/arm     — same as /reset";
      sendTelegram(help);

    } else {
      sendTelegram("Unknown command. Send /help for list.");
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  Parse messages arriving FROM STM32 over Serial2
//
//  Messages:
//    INTRUDER:<dist>    intruder detected at <dist> cm
//    RESET              system disarmed/reset
//    ARMED              system armed (boot or after reset)
//    DOOR:LOCKED        door locked
//    DOOR:UNLOCKED      door unlocked
// ─────────────────────────────────────────────────────────────
void handleSTM32Message(String msg) {
  msg.trim();
  if (msg.length() == 0) return;

  Serial.print("[STM32→ESP32] ");
  Serial.println(msg);

  // ── INTRUDER:<distance> ──
  if (msg.startsWith("INTRUDER:")) {
    String dist = msg.substring(9);     // e.g. "15.3"
    systemStatus  = "INTRUDER";
    lastAlert     = dist + " cm";
    alertCount++;
    lastAlertTime = millis();

    // ★ Line 3 of workflow: send Telegram notification
    String alert = "🚨 INTRUDER DETECTED!\n";
    alert += "📏 Distance : " + dist + " cm\n";
    alert += "🚪 Door     : LOCKED\n";
    alert += "⚠️ Alert #"  + String(alertCount) + "\n\n";
    alert += "Reply /reset to disarm\nReply /unlock to open door";
    sendTelegram(alert);

    // ★ Line 3: upload to Firebase
    pushToFirebase();
  }

  // ── RESET ──
  else if (msg == "RESET") {
    systemStatus = "ARMED";
    pushToFirebase();
  }

  // ── ARMED ──
  else if (msg == "ARMED") {
    systemStatus = "ARMED";
    pushToFirebase();
  }

  // ── DOOR:LOCKED ──
  else if (msg == "DOOR:LOCKED") {
    doorStatus = "LOCKED";
  }

  // ── DOOR:UNLOCKED ──
  else if (msg == "DOOR:UNLOCKED") {
    doorStatus = "UNLOCKED";
  }
}

// ─────────────────────────────────────────────────────────────
//  WiFi connect with retry
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[ESP32] Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t > 20000UL) {
      Serial.println("\n[ESP32] WiFi timeout — retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t = millis();
    }
  }
  Serial.println("\n[ESP32] WiFi connected: " + WiFi.localIP().toString());
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  // USB debug serial
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[ESP32] Home Security System — ESP32 booting...");

  // Serial2 → STM32
  Serial2.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
  Serial.println("[ESP32] Serial2 (STM32 link) ready on GPIO16/17");

  // WiFi
  connectWiFi();

  // Telegram TLS — setInsecure is fine for prototyping
  // For production: use a proper root CA certificate bundle
  secureClient.setInsecure();

  // Firebase
  fbConfig.host = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("[ESP32] Firebase initialized");

  // Push initial armed state
  pushToFirebase();

  // Notify owner bot is online
  sendTelegram("✅ Home Security System ONLINE\nMonitoring started.\nSend /help for commands.");

  Serial.println("[ESP32] Setup complete — waiting for STM32 events...");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. Read messages from STM32 ──────────────────────────────
  while (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) handleSTM32Message(line);
  }

  // ── 2. Poll Telegram for owner commands ──────────────────────
  if (now - lastTelegramPoll >= TELEGRAM_POLL_MS) {
    lastTelegramPoll = now;
    if (WiFi.status() == WL_CONNECTED) {
      int msgCount = bot.getUpdates(bot.last_message_received + 1);
      if (msgCount > 0) handleTelegramMessages(msgCount);
    } else {
      connectWiFi();   // reconnect if dropped
    }
  }

  // ── 3. Periodic Firebase sync ─────────────────────────────────
  if (now - lastFirebasePush >= FIREBASE_PUSH_MS) {
    lastFirebasePush = now;
    pushToFirebase();
  }
}
