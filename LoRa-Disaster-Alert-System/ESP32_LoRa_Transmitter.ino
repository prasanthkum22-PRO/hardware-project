/*
 * ============================================================
 *  LoRa Disaster Alert System — TRANSMITTER (ESP32)
 *  Hardware : ESP32 DevKit + SX1278 LoRa + DHT11 + SOS Button
 *  IDE      : Arduino IDE  (board: ESP32 Dev Module)
 *
 *  Libraries required (install via Library Manager):
 *    - LoRa  by Sandeep Mistry   (search "LoRa")
 *    - DHT11 by Dhruba Saha      (search "DHT11", NOT Adafruit DHT)
 *
 *  Serial port usage:  Serial  (USB / UART0)  — for debug only
 *  NOTE: Serial2 / Serial1 are NOT used in this sketch.
 * ============================================================
 *
 *  ─── SX1278 LoRa Module ↔ ESP32 Wiring ───
 *  SX1278  →  ESP32
 *  VCC     →  3.3V
 *  GND     →  GND
 *  SCK     →  GPIO 18   (VSPI CLK)
 *  MISO    →  GPIO 19   (VSPI MISO)
 *  MOSI    →  GPIO 23   (VSPI MOSI)
 *  NSS/CS  →  GPIO 5
 *  RESET   →  GPIO 14
 *  DIO0    →  GPIO 2    (strapping pin — see note above)
 *
 *  ─── DHT11 ↔ ESP32 ───
 *  VCC  →  3.3V (or 5V)
 *  GND  →  GND
 *  DATA →  GPIO 4   (with 10kΩ pull-up to VCC)
 *
 *  ─── SOS Button ↔ ESP32 ───
 *  One leg  →  GPIO 15
 *  Other    →  GND
 *  (internal pull-up enabled, press = LOW)
 * ============================================================
 */

#include <DHT11.h>
#include <LoRa.h>
#include <SPI.h>

// ── Pin definitions ──────────────────────────────────────────
#define LORA_SS 5
#define LORA_RST 14
#define LORA_DIO0 2

#define DHT_PIN 4

#define SOS_BTN_PIN 15 // active LOW (internal pull-up)
#define SOS_LED_PIN 13 // onboard LED — lights while SOS sent

// ── LoRa radio parameters ────────────────────────────────────
// All nodes MUST share the same frequency, BW, SF, CR
#define LORA_FREQ 433E6 // 433 MHz  (match your module's band)
#define LORA_BW 125E3   // 125 kHz bandwidth
#define LORA_SF 9       // Spreading factor 9  (range vs. speed trade-off)
#define LORA_TX_POW 17  // dBm  (max 20 for SX1278)

// ── Timing ───────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS 5000UL // send sensor data every 5 s
#define DEBOUNCE_MS 50UL          // button debounce window

// ── Node identity (change per device) ────────────────────────
#define NODE_ID "TX-ESP32-01"

// ── Objects ──────────────────────────────────────────────────
DHT11 dht11(DHT_PIN); // FIX: this library takes only the pin

// ── State ────────────────────────────────────────────────────
unsigned long lastSensorSend = 0;
unsigned long lastBtnChange = 0;
bool prevBtnState = HIGH;
bool sosActive = false;

// ─────────────────────────────────────────────────────────────
void setup() {
  // ── Debug serial (USB) ──────────────────────────────────────
  Serial.begin(115200);
  while (!Serial && millis() < 3000)
    ; // wait up to 3 s for USB CDC
  Serial.println(F("\n[ESP32] LoRa Disaster Alert — Transmitter booting..."));

  // ── GPIO ────────────────────────────────────────────────────
  pinMode(SOS_BTN_PIN, INPUT_PULLUP);
  pinMode(SOS_LED_PIN, OUTPUT);
  digitalWrite(SOS_LED_PIN, LOW);

  // ── DHT sensor ──────────────────────────────────────────────
  // FIX: DHT11 (this library) has no begin() — nothing to call here.
  Serial.println(F("[ESP32] DHT11 ready"));

  // ── LoRa init ───────────────────────────────────────────────
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("[ESP32] ERROR: LoRa init FAILED — check wiring!"));
    while (true) {
      digitalWrite(SOS_LED_PIN, !digitalRead(SOS_LED_PIN));
      delay(200); // fast blink = hardware fault
    }
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setTxPower(LORA_TX_POW);
  LoRa.setSyncWord(0xF3); // private network sync word
  Serial.println(F("[ESP32] LoRa radio ready"));
}

// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. SOS button handling (debounced) ──────────────────────
  bool btnNow = digitalRead(SOS_BTN_PIN);
  if (btnNow != prevBtnState && (now - lastBtnChange) > DEBOUNCE_MS) {
    lastBtnChange = now;
    prevBtnState = btnNow;
    if (btnNow == LOW) {
      // button just pressed → send SOS immediately
      sosActive = true;
      sendSOS();
    }
  }

  // ── 2. Periodic sensor broadcast ────────────────────────────
  if (now - lastSensorSend >= SENSOR_INTERVAL_MS) {
    lastSensorSend = now;
    sendSensorData();
  }
}

// ─────────────────────────────────────────────────────────────
// Read DHT11 and broadcast a sensor packet
// Packet format (CSV):
//   NODE,TEMP=xx,HUM=xx,SOS=0|1
// ─────────────────────────────────────────────────────────────
void sendSensorData() {
  int temperature = dht11.readTemperature(); // FIX: int, not float
  int humidity = dht11.readHumidity();       // FIX: int, not float

  // FIX: this library signals errors via negative return codes,
  // not NAN — isnan() would never catch a failed read.
  if (temperature < 0 || humidity < 0) {
    Serial.print(F("[ESP32] DHT11 read failed — "));
    Serial.println(
        DHT11::getErrorString(temperature < 0 ? temperature : humidity));
    // Send 0 instead of returning so we don't break the LoRa link!
    if (temperature < 0)
      temperature = 0;
    if (humidity < 0)
      humidity = 0;
  }

  String packet = buildPacket(temperature, humidity, sosActive);

  Serial.print(F("[ESP32] TX SENSOR → "));
  Serial.println(packet);

  transmitPacket(packet);

  sosActive = false; // clear SOS flag after one sensor-cycle with it set
  digitalWrite(SOS_LED_PIN, LOW);
}

// ─────────────────────────────────────────────────────────────
// Send a dedicated SOS packet immediately (highest priority)
// ─────────────────────────────────────────────────────────────
void sendSOS() {
  digitalWrite(SOS_LED_PIN, HIGH);

  // Read sensor if available, else send 0s
  int temperature = dht11.readTemperature();
  int humidity = dht11.readHumidity();
  if (temperature < 0)
    temperature = 0;
  if (humidity < 0)
    humidity = 0;

  String packet = buildPacket(temperature, humidity, true);

  Serial.print(F("[ESP32] *** SOS TX *** → "));
  Serial.println(packet);

  // Transmit SOS 3 times for reliability
  for (int i = 0; i < 3; i++) {
    transmitPacket(packet);
    delay(200);
  }
}

// ─────────────────────────────────────────────────────────────
// Build the ASCII CSV packet string
// ─────────────────────────────────────────────────────────────
String buildPacket(int temp, int hum, bool sos) {
  String p = String(NODE_ID);
  p += ",TEMP=";
  p += String(temp); // FIX: int formatting, no decimal arg
  p += ",HUM=";
  p += String(hum); // FIX: int formatting, no decimal args
  p += ",SOS=";
  p += (sos ? "1" : "0");
  return p;
}

// ─────────────────────────────────────────────────────────────
// Low-level LoRa transmit
// ─────────────────────────────────────────────────────────────
void transmitPacket(const String &payload) {
  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket(); // blocking — waits for TX done
}