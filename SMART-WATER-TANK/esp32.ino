/* ============================================================
   SMART WATER TANK - ESP32 DevKit (Telegram "Tank Full" Alert Only)
   DEBUG VERSION - extra Serial prints to diagnose why the
   Telegram message isn't sending.
   ============================================================ */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

const char* ssid     = "prasan";
const char* password = "123456789";

#define BOT_TOKEN "8902121825:AAF4KxEF-0kyMeDAuFBLTlaB2URs113uG5Y"
#define CHAT_ID   "8762693342"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

#define RXD2 16
#define TXD2 17

const int FULL_LEVEL_THRESHOLD = 88;

int  waterLevel    = -1;
bool pumpOn        = false;
bool sensorError   = false;
bool lastPumpState = false;
bool fullAlertSent = false;
bool firstFrameReceived = false;

String rxBuffer = "";

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connect timed out.");
  }
}

void sendTelegram(String msg) {
  Serial.println(">>> sendTelegram() called with: " + msg);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(">>> ABORTED: WiFi not connected.");
    return;
  }
  bool sent = bot.sendMessage(CHAT_ID, msg, "");
  Serial.println(sent ? ">>> Telegram SUCCESS" : ">>> Telegram FAILED (check token/chat id)");
}

bool parseFrame(String frame) {
  int lIdx = frame.indexOf("L:");
  int pIdx = frame.indexOf(",P:");
  int eIdx = frame.indexOf(",E:");
  if (lIdx == -1 || pIdx == -1 || eIdx == -1) {
    Serial.println("PARSE FAILED on frame: [" + frame + "]");
    return false;
  }
  waterLevel  = frame.substring(lIdx + 2, pIdx).toInt();
  pumpOn      = frame.substring(pIdx + 3, eIdx).toInt() == 1;
  sensorError = frame.substring(eIdx + 3).toInt() == 1;
  return true;
}

void checkTankFull() {
  if (sensorError) {
    Serial.println("checkTankFull: skipped, sensorError=true");
    return;
  }

  bool pumpJustTurnedOff = (lastPumpState == true && pumpOn == false);

  // ---- DEBUG: show every value the decision depends on ----
  Serial.print("checkTankFull -> level="); Serial.print(waterLevel);
  Serial.print(" threshold="); Serial.print(FULL_LEVEL_THRESHOLD);
  Serial.print(" lastPump="); Serial.print(lastPumpState);
  Serial.print(" pumpOn="); Serial.print(pumpOn);
  Serial.print(" pumpJustTurnedOff="); Serial.print(pumpJustTurnedOff);
  Serial.print(" fullAlertSent="); Serial.println(fullAlertSent);

  if ((waterLevel >= FULL_LEVEL_THRESHOLD || pumpJustTurnedOff) && !fullAlertSent) {
    String msg = "Tank Full\n";
    msg += "Level: " + String(waterLevel) + "%\n";
    msg += "Pump: " + String(pumpOn ? "ON" : "OFF");
    sendTelegram(msg);
    fullAlertSent = true;
  }

  if (waterLevel < FULL_LEVEL_THRESHOLD - 10) {
    fullAlertSent = false;
  }

  lastPumpState = pumpOn;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  secured_client.setInsecure();
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      Serial.println("RAW FRAME: [" + rxBuffer + "]");
      if (parseFrame(rxBuffer)) {
        Serial.print("Level: "); Serial.print(waterLevel);
        Serial.print("% | Pump: "); Serial.print(pumpOn ? "ON" : "OFF");
        Serial.print(" | Error: "); Serial.println(sensorError);

        if (!firstFrameReceived) {
          lastPumpState = pumpOn;
          firstFrameReceived = true;
        }
        checkTankFull();
      }
      rxBuffer = "";
    } else {
      rxBuffer += c;
    }
  }
}