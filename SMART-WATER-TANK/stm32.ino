/* ============================================================
   SMART WATER TANK - STM32F446RE (Nucleo-64)
   Function : Ultrasonic level sensing + Relay pump control
              + UART bridge to ESP32 (using Serial only)
   Board    : Nucleo F446RE (Arduino IDE, STM32duino core)
   ============================================================ */

#define TRIG_PIN      PB4
#define ECHO_PIN      PB5
#define RELAY_PIN     PB3
#define STATUS_LED    PA5      // onboard LED (LD2)

// ---- Tank calibration (edit for your tank) ----
const float TANK_HEIGHT_CM   = 100.0;
const float SENSOR_OFFSET_CM = 2.0;
const int   LOW_THRESHOLD    = 25;
const int   HIGH_THRESHOLD   = 90;
const unsigned long SEND_INTERVAL = 2000;

bool pumpState = false;
unsigned long lastSend = 0;

float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  return (duration * 0.0343) / 2.0;
}

int distanceToLevelPercent(float distanceCM) {
  if (distanceCM < 0) return -1;
  float waterColumn = TANK_HEIGHT_CM - (distanceCM - SENSOR_OFFSET_CM);
  float percent = (waterColumn / TANK_HEIGHT_CM) * 100.0;
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;
  return (int)percent;
}

void setup() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);

  Serial.begin(9600);   // used to talk to the ESP32
}

void loop() {
  float distance = readDistanceCM();
  int level = distanceToLevelPercent(distance);

  if (level >= 0) {
    if (!pumpState && level <= LOW_THRESHOLD) {
      pumpState = true;
    } else if (pumpState && level >= HIGH_THRESHOLD) {
      pumpState = false;
    }
    digitalWrite(RELAY_PIN, pumpState ? HIGH : LOW);
    digitalWrite(STATUS_LED, pumpState ? HIGH : LOW);
  }

  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();
    Serial.print("L:"); Serial.print(level < 0 ? 0 : level);
    Serial.print(",P:"); Serial.print(pumpState ? 1 : 0);
    Serial.print(",E:"); Serial.println(level < 0 ? 1 : 0);
  }

  delay(300);
}