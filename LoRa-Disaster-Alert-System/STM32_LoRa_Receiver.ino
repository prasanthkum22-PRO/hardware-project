/*
 * ============================================================
 *  LoRa Disaster Alert System — RECEIVER (STM32F446RE Nucleo)
 *  Hardware : Nucleo-F446RE + SX1278 LoRa + 16×2 I2C LCD
 *             + SOS Indicator LED
 *  IDE      : Arduino IDE  (board: STM32 Nucleo-64  F446RE)
 *             Package: STM32duino (stm32duino.github.io/Arduino_Core_STM32)
 *
 *  Libraries required (install via Library Manager):
 *    - LoRa  by Sandeep Mistry
 *    - LiquidCrystal_I2C  by Frank de Brabander  (or Marco Schwartz)
 *
 *  Serial port usage:  Serial  (USART2, routed to ST-Link VCP)
 *  NOTE: No Serial1 / Serial2 used here — all debug on Serial only.
 * ============================================================
 *
 *  ─── SX1278 LoRa Module ↔ Nucleo-F446RE Wiring ───
 *  SX1278   →  Nucleo Pin  (Arduino header label)
 *  VCC      →  3V3         (CN6 pin 4)
 *  GND      →  GND         (CN6 pin 6)
 *  SCK      →  PA5         (D13 — SPI1 CLK)
 *  MISO     →  PA6         (D12 — SPI1 MISO)
 *  MOSI     →  PA7         (D11 — SPI1 MOSI)
 *  NSS/CS   →  PB6         (D10)
 *  RESET    →  PA9         (D8)
 *  DIO0     →  PA8         (D7)
 *
 *  ─── 16×2 I2C LCD (PCF8574 backpack) ↔ Nucleo ───
 *  LCD VCC  →  5V  (CN7 pin 18)
 *  LCD GND  →  GND
 *  LCD SDA  →  PB9  (D14 / I2C1 SDA)
 *  LCD SCL  →  PB8  (D15 / I2C1 SCL)
 *  Default I2C address: 0x27  (change to 0x3F if LCD shows nothing)
 *
 *  ─── SOS Indicator LED ↔ Nucleo ───
 *  LED+ (via 330Ω resistor)  →  PC7  (D9)
 *  LED−                      →  GND
 *  (Onboard LD2 green LED is also driven for SOS — PA5 / D13)
 *
 * ============================================================
 *  PACKET FORMAT received from ESP32 transmitter:
 *    NODE_ID,TEMP=xx.x,HUM=xx.x,SOS=0|1
 *  Example:
 *    TX-ESP32-01,TEMP=32.5,HUM=65.0,SOS=0
 *    TX-ESP32-01,TEMP=31.0,HUM=67.0,SOS=1
 * ============================================================
 */

#include <LiquidCrystal_I2C.h>
#include <LoRa.h>
#include <SPI.h>
#include <Wire.h>

// ── LoRa pin map (Arduino header names for Nucleo-F446RE) ────
#define LORA_SS PB6
#define LORA_RST PA9
#define LORA_DIO0 PA8

// ── LCD I2C address & dimensions ────────────────────────────
#define LCD_ADDR 0x27 // change to 0x3F if display blank
#define LCD_COLS 16
#define LCD_ROWS 2

// ── SOS indicator LED ────────────────────────────────────────
#define SOS_LED_PIN PC7 // external LED with 330Ω
// PA5 is the Nucleo-F446RE onboard LD2 (green)
// NOTE: PA5 is shared with LORA SCK — do NOT redefine it here.
// Use PC7 for the external SOS LED only.

// ── LoRa radio parameters (MUST match transmitter) ──────────
#define LORA_FREQ 433E6
#define LORA_BW 125E3
#define LORA_SF 9
#define LORA_SYNC 0xF3

// ── Timing ───────────────────────────────────────────────────
#define SOS_LED_BLINK_MS 300UL  // blink period while SOS active
#define SOS_LED_HOLD_MS 10000UL // keep SOS state visible for 10 s

// ── Objects ──────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ── State ────────────────────────────────────────────────────
float lastTemp = 0.0;
float lastHum = 0.0;
bool sosState = false;
unsigned long sosTimestamp = 0;
unsigned long lastBlink = 0;
bool ledState = false;

// ── Custom LCD characters ─────────────────────────────────────
// Thermometer icon (char 0)
byte thermIcon[8] = {B00100, B01010, B01010, B01110,
                     B01110, B11111, B11111, B01110};
// Drop / humidity icon (char 1)
byte dropIcon[8] = {B00100, B01110, B11111, B11111,
                    B11111, B11111, B01110, B00000};
// Alert / SOS icon (char 2)
byte alertIcon[8] = {B00100, B01110, B01010, B11011,
                     B10001, B11111, B01110, B00100};

// ─────────────────────────────────────────────────────────────
void setup() {
  // ── Debug serial (ST-Link VCP on USART2) ────────────────────
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n[STM32] LoRa Disaster Alert — Receiver booting..."));

  // ── SOS indicator LED ────────────────────────────────────────
  pinMode(SOS_LED_PIN, OUTPUT);
  digitalWrite(SOS_LED_PIN, LOW);

  // ── I2C LCD ─────────────────────────────────────────────────
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, thermIcon);
  lcd.createChar(1, dropIcon);
  lcd.createChar(2, alertIcon);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("LoRa Disaster"));
  lcd.setCursor(0, 1);
  lcd.print(F("Alert  System"));
  delay(2000);
  lcd.clear();
  Serial.println(F("[STM32] LCD initialised"));

  // ── LoRa init ───────────────────────────────────────────────
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("[STM32] ERROR: LoRa init FAILED — check wiring!"));
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("LoRa INIT FAIL"));
    lcd.setCursor(0, 1);
    lcd.print(F("Check wiring!"));
    while (true) {
      digitalWrite(SOS_LED_PIN, !digitalRead(SOS_LED_PIN));
      delay(200);
    }
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSyncWord(LORA_SYNC);
  Serial.println(F("[STM32] LoRa radio ready — listening..."));

  showIdle();
}

// ─────────────────────────────────────────────────────────────
void loop() {
  // ── Check for incoming LoRa packet ──────────────────────────
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    String raw = "";
    while (LoRa.available()) {
      raw += (char)LoRa.read();
    }
    int rssi = LoRa.packetRssi();
    Serial.print(F("[STM32] RX packet (RSSI="));
    Serial.print(rssi);
    Serial.print(F(" dBm): "));
    Serial.println(raw);

    parseAndDisplay(raw, rssi);
  }

  // ── SOS LED blinking / auto-clear ───────────────────────────
  if (sosState) {
    unsigned long now = millis();
    // Blink the SOS LED
    if (now - lastBlink >= SOS_LED_BLINK_MS) {
      lastBlink = now;
      ledState = !ledState;
      digitalWrite(SOS_LED_PIN, ledState ? HIGH : LOW);
    }
    // Auto-clear SOS after hold period
    if (now - sosTimestamp >= SOS_LED_HOLD_MS) {
      clearSOS();
    }
  }
}

// ─────────────────────────────────────────────────────────────
// Parse the CSV packet and update LCD + SOS state
// Expected format:  NODE,TEMP=xx.x,HUM=xx.x,SOS=0|1
// ─────────────────────────────────────────────────────────────
void parseAndDisplay(const String &raw, int rssi) {
  // --- extract TEMP ---
  int tIdx = raw.indexOf("TEMP=");
  if (tIdx != -1) {
    int commaAfterT = raw.indexOf(',', tIdx);
    String tStr = (commaAfterT != -1) ? raw.substring(tIdx + 5, commaAfterT)
                                      : raw.substring(tIdx + 5);
    lastTemp = tStr.toFloat();
  }

  // --- extract HUM ---
  int hIdx = raw.indexOf("HUM=");
  if (hIdx != -1) {
    int commaAfterH = raw.indexOf(',', hIdx);
    String hStr = (commaAfterH != -1) ? raw.substring(hIdx + 4, commaAfterH)
                                      : raw.substring(hIdx + 4);
    lastHum = hStr.toFloat();
  }

  // --- extract SOS ---
  int sIdx = raw.indexOf("SOS=");
  bool newSOS = false;
  if (sIdx != -1) {
    char sosChar = raw.charAt(sIdx + 4);
    newSOS = (sosChar == '1');
  }

  // --- update display ---
  if (newSOS) {
    triggerSOS(rssi);
  } else {
    showSensorData(rssi);
  }
}

// ─────────────────────────────────────────────────────────────
// Normal sensor display
//  Row0:  [therm] T:xx.xC  [drop] H:xx%
//  Row1:  RSSI:-xxx dBm
// ─────────────────────────────────────────────────────────────
void showSensorData(int rssi) {
  lcd.clear();

  // Row 0 — temperature & humidity
  lcd.setCursor(0, 0);
  lcd.write(byte(0)); // thermometer icon
  lcd.print(F("T:"));
  lcd.print(lastTemp, 1);
  lcd.print(F("C "));
  lcd.write(byte(1)); // drop icon
  lcd.print(F("H:"));
  lcd.print((int)lastHum);
  lcd.print(F("%"));

  // Row 1 — RSSI
  lcd.setCursor(0, 1);
  lcd.print(F("RSSI:"));
  lcd.print(rssi);
  lcd.print(F(" dBm  "));

  digitalWrite(SOS_LED_PIN, LOW);
}

// ─────────────────────────────────────────────────────────────
// SOS alert display
//  Row0:  [alert] SOS ALERT!
//  Row1:  T:xx.x H:xx% !!
// ─────────────────────────────────────────────────────────────
void triggerSOS(int rssi) {
  sosState = true;
  sosTimestamp = millis();
  lastBlink = millis();
  ledState = true;
  digitalWrite(SOS_LED_PIN, HIGH);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(byte(2)); // alert icon
  lcd.print(F(" *** SOS! ***"));

  lcd.setCursor(0, 1);
  lcd.print(F("T:"));
  lcd.print(lastTemp, 1);
  lcd.print(F("C H:"));
  lcd.print((int)lastHum);
  lcd.print(F("%!"));

  Serial.println(F("[STM32] *** SOS RECEIVED — ALERT ACTIVE ***"));
}

// ─────────────────────────────────────────────────────────────
// Clear SOS state after hold period
// ─────────────────────────────────────────────────────────────
void clearSOS() {
  sosState = false;
  digitalWrite(SOS_LED_PIN, LOW);
  showSensorData(-999); // refresh with last known values
  Serial.println(F("[STM32] SOS cleared — back to normal monitoring"));
}

// ─────────────────────────────────────────────────────────────
// Show idle screen while waiting for first packet
// ─────────────────────────────────────────────────────────────
void showIdle() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Waiting for TX"));
  lcd.setCursor(0, 1);
  lcd.print(F("433MHz  SF9..."));
}
