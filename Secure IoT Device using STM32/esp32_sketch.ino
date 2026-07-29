/*
  ESP32 — WiFi / Firebase Bridge
  --------------------------------------------------------
  - Connects to WiFi
  - Connects to Firebase Realtime Database
  - Receives sensor data from STM32 over UART (Serial2)
        format from STM32:  T25.3H60.2M1\n
  - Uploads readings to Firebase:
        /sensor/temperature
        /sensor/humidity
        /sensor/motion
  - Polls Firebase for light command:
        /control/light   (0 or 1)
    and forwards it to STM32 as a single character '0' / '1'

  LIBRARY NEEDED (Library Manager):
    - "Firebase ESP Client" by mobizt   (search: Firebase ESP Client)

  BOARD: Tools -> Board -> ESP32 Dev Module
*/

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ---------------- WiFi credentials ----------------
#define WIFI_SSID     "prasan"
#define WIFI_PASSWORD "123456789"

// ---------------- Firebase credentials ----------------
// Get these from Firebase Console:
//   API_KEY      -> Project settings -> General -> Web API Key
//   DATABASE_URL -> Realtime Database -> the https://xxx.firebaseio.com URL
//   USER_EMAIL / USER_PASSWORD -> a user you create under Authentication -> Email/Password
#define API_KEY       "AIzaSyBF2wGjMcwagnvjLw6x8pOsvLJqA6F3r0Q"

#define DATABASE_URL  "https://finger-attendance--esp-32-default-rtdb.asia-southeast1.firebasedatabase.app/"

#define USER_EMAIL    "prasanth.kum22@gmail.com"

#define USER_PASSWORD "project8"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------------- UART to STM32 ----------------
#define STM_RX 16   // ESP32 RX2 <- connect to STM32 TX (PA9)
#define STM_TX 17   // ESP32 TX2 -> connect to STM32 RX (PA10)

unsigned long lastFirebaseCheck = 0;
const unsigned long FB_CHECK_INTERVAL = 1000;

String incoming = "";

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nBooting ESP32 bridge...");

  Serial2.begin(9600, SERIAL_8N1, STM_RX, STM_TX);

  // ---- WiFi ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(300);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi FAILED - check SSID/password (2.4GHz only!)");
  }

  // ---- Firebase ----
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Firebase initialized.");
}

void loop() {
  // ---- 1. Read sensor data streaming in from STM32 ----
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      parseAndUpload(incoming);
      incoming = "";
    } else {
      incoming += c;
    }
  }

  // ---- 2. Poll Firebase for light on/off command ----
  if (Firebase.ready() && millis() - lastFirebaseCheck > FB_CHECK_INTERVAL) {
    lastFirebaseCheck = millis();
    if (Firebase.RTDB.getInt(&fbdo, "/control/light")) {
      int val = fbdo.intData();
      Serial2.print(val == 1 ? '1' : '0');
    }
  }
}

void parseAndUpload(String data) {
  // Expected: T25.3H60.2M1
  int tIdx = data.indexOf('T');
  int hIdx = data.indexOf('H');
  int mIdx = data.indexOf('M');
  if (tIdx == -1 || hIdx == -1 || mIdx == -1) return;

  float temp = data.substring(tIdx + 1, hIdx).toFloat();
  float hum  = data.substring(hIdx + 1, mIdx).toFloat();
  int motion = data.substring(mIdx + 1).toInt();

  Serial.print("Temp: "); Serial.print(temp);
  Serial.print("  Hum: "); Serial.print(hum);
  Serial.print("  Motion: "); Serial.println(motion);

  if (Firebase.ready()) {
    Firebase.RTDB.setFloat(&fbdo, "/sensor/temperature", temp);
    Firebase.RTDB.setFloat(&fbdo, "/sensor/humidity", hum);
    Firebase.RTDB.setInt(&fbdo, "/sensor/motion", motion);
  }
}
