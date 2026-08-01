/*
 * ============================================================
 *  INTELLIGENT HOME SECURITY SYSTEM — STM32F446RE (MAIN CONTROLLER)
 * ============================================================
 *
 *  WORKFLOW:
 *  Line 1: Power ON → STM32 initializes → Servo locks door →
 *          Ultrasonic sensor continuously monitors entrance.
 *
 *  Line 2: Distance < threshold → STM32 detects intruder →
 *          Activates buzzer + LED → Locks door via servo →
 *          Sends INTRUDER alert to ESP32 over UART.
 *
 *  Line 3: ESP32 sends Telegram notification + Firebase upload →
 *          Owner receives alert → Reset locally or remotely.
 *
 * ============================================================
 *  PIN CONNECTIONS — STM32F446RE Nucleo
 * ============================================================
 *
 *  ── HC-SR04 Ultrasonic Sensor ──
 *  HC-SR04 VCC   →  5V  (CN7 pin 18)
 *  HC-SR04 GND   →  GND (CN7 pin 20)
 *  HC-SR04 TRIG  →  PB0   (clean GPIO, no peripheral conflict)
 *  HC-SR04 ECHO  →  PB1   (clean GPIO)
 *                    ** VOLTAGE DIVIDER MANDATORY on ECHO **
 *                    ECHO(5V) → 1kΩ → PB1
 *                                   ↓
 *                                 2kΩ → GND
 *                    Result = 5 × 2/(1+2) = 3.33V ✓ safe for STM32
 *
 *  ── SG90 Servo Motor (door lock) ──
 *  Servo VCC     →  5V external supply (500mA+, share GND with STM32)
 *  Servo GND     →  GND (common ground)
 *  Servo Signal  →  PA6   (TIM3_CH1 — hardware PWM)
 *
 *  ── LED (intrusion indicator) ──
 *  LED Anode     →  220Ω resistor → PC0
 *  LED Cathode   →  GND
 *
 *  ── Active Buzzer (2-pin) ──
 *  Buzzer +      →  PC1
 *  Buzzer -      →  GND
 *
 *  ── Reset Push Button (manual reset) ──
 *  One leg       →  PC2
 *  Other leg     →  GND
 *  (internal pull-up used — no external resistor needed)
 *
 *  ── UART to ESP32 ──
 *  STM32 PA2 (TX) →  ESP32 GPIO16 (RX2)  [add 1kΩ series on ESP32 RX]
 *  STM32 PA3 (RX) ←  ESP32 GPIO17 (TX2)
 *  GND            ←→ GND (MUST share common ground)
 *  NOTE: Both boards are 3.3V logic — direct connection safe.
 *        Serial on STM32duino/Nucleo = USART2 = PA2/PA3 = USB bridge.
 *
 * ============================================================
 *  UART PROTOCOL (115200 8N1)
 *
 *  STM32 → ESP32:
 *    INTRUDER:<distance>    intrusion detected, distance in cm
 *    RESET                  system reset/disarmed
 *    ARMED                  system monitoring (sent on boot + after reset)
 *    DOOR:LOCKED            servo moved to locked position
 *    DOOR:UNLOCKED          servo moved to unlocked position
 *
 *  ESP32 → STM32:
 *    CMD:RESET              remote reset from Telegram/web
 *    CMD:UNLOCK             remote door unlock
 *    CMD:LOCK               remote door lock
 *
 * ============================================================
 *  Libraries (Library Manager — install only this):
 *    LiquidCrystal_I2C  by Frank de Brabander  (if LCD added later)
 *    Servo              built-in STM32duino — no install needed
 * ============================================================
 */

#include <Servo.h>

// ─────────────────────────────────────────────────────────────
//  Pin Definitions
// ─────────────────────────────────────────────────────────────
#define TRIG_PIN       PB0   // Ultrasonic trigger (clean GPIO)
#define ECHO_PIN       PB1   // Ultrasonic echo   (clean GPIO, use voltage divider!)
#define SERVO_PIN      PA6   // SG90 signal       (TIM3_CH1 — hardware PWM)
#define LED_PIN        PC0   // Intrusion LED     (via 220Ω to GND)
#define BUZZER_PIN     PC1   // Active buzzer     (+ to pin, - to GND)
#define RESET_BTN_PIN  PC2   // Manual reset button (to GND, INPUT_PULLUP)

// ─────────────────────────────────────────────────────────────
//  Servo Angles
// ─────────────────────────────────────────────────────────────
#define DOOR_LOCKED_ANGLE    0    // door shut / locked
#define DOOR_UNLOCKED_ANGLE  90   // door open / unlocked

// ─────────────────────────────────────────────────────────────
//  Detection Thresholds
// ─────────────────────────────────────────────────────────────
#define TRIGGER_DISTANCE_CM  20.0f   // intruder if closer than this
#define RESET_DISTANCE_CM    30.0f   // auto re-arm when clear beyond this
#define SAMPLES              3        // readings averaged per poll (debounce)

// ─────────────────────────────────────────────────────────────
//  Timing
// ─────────────────────────────────────────────────────────────
#define SENSOR_POLL_MS       500UL    // poll sensor every 500 ms
#define ALARM_DURATION_MS   5000UL   // buzzer/LED active for 5 s then stays alert
#define DEBOUNCE_MS          50UL    // button debounce

// ─────────────────────────────────────────────────────────────
//  System State
// ─────────────────────────────────────────────────────────────
enum SystemState {
  STATE_ARMED,       // monitoring, all clear
  STATE_INTRUDER,    // intruder detected — alarm active
  STATE_ALARM_HOLD   // alarm triggered, waiting for reset
};
SystemState sysState = STATE_ARMED;

Servo doorServo;
bool  alarmActive   = false;
unsigned long alarmStartedAt  = 0;
unsigned long lastSensorPoll  = 0;
unsigned long lastBtnCheck    = 0;
bool   lastBtnState   = HIGH;

// ─────────────────────────────────────────────────────────────
//  ★ micros()-based distance measurement
//    (pulseIn() is unreliable on STM32 at 180 MHz — replaced)
// ─────────────────────────────────────────────────────────────
float measureOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Wait for ECHO HIGH (30 ms timeout)
  unsigned long t0 = micros();
  while (digitalRead(ECHO_PIN) == LOW) {
    if (micros() - t0 > 30000UL) return -1.0f;
  }
  unsigned long t1 = micros();
  while (digitalRead(ECHO_PIN) == HIGH) {
    if (micros() - t1 > 30000UL) return -1.0f;
  }
  unsigned long t2 = micros();

  return ((float)(t2 - t1) * 0.034f) / 2.0f;
}

// ─────────────────────────────────────────────────────────────
//  Averaged reading over SAMPLES — filters noise/false triggers
// ─────────────────────────────────────────────────────────────
float getDistance() {
  float sum = 0;
  int   valid = 0;
  for (int i = 0; i < SAMPLES; i++) {
    float d = measureOnce();
    if (d > 0) { sum += d; valid++; }
    delayMicroseconds(500);
  }
  return (valid == 0) ? -1.0f : sum / valid;
}

// ─────────────────────────────────────────────────────────────
//  Door control — sends UART message to ESP32
// ─────────────────────────────────────────────────────────────
void lockDoor() {
  doorServo.write(DOOR_LOCKED_ANGLE);
  Serial.println("DOOR:LOCKED");                  // → ESP32
}

void unlockDoor() {
  doorServo.write(DOOR_UNLOCKED_ANGLE);
  Serial.println("DOOR:UNLOCKED");               // → ESP32
}

// ─────────────────────────────────────────────────────────────
//  Alarm ON — buzzer + LED
// ─────────────────────────────────────────────────────────────
void activateAlarm() {
  digitalWrite(LED_PIN,    HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  alarmActive    = true;
  alarmStartedAt = millis();
}

void deactivateAlarm() {
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);
  alarmActive = false;
}

// ─────────────────────────────────────────────────────────────
//  Intruder detected — trigger full response
// ─────────────────────────────────────────────────────────────
void handleIntruder(float dist) {
  sysState = STATE_INTRUDER;
  activateAlarm();
  lockDoor();                           // lock door on intrusion

  // Notify ESP32 with distance
  Serial.print("INTRUDER:");
  Serial.println(dist, 1);             // → ESP32 (triggers Telegram + Firebase)

  Serial.print("[STM32] *** INTRUDER DETECTED *** Distance: ");
  Serial.print(dist, 1);
  Serial.println(" cm");
}

// ─────────────────────────────────────────────────────────────
//  Reset system — can be called locally (button) or remotely (ESP32 CMD)
// ─────────────────────────────────────────────────────────────
void resetSystem() {
  sysState = STATE_ARMED;
  deactivateAlarm();
  lockDoor();                           // ensure door re-locked after reset
  Serial.println("RESET");             // → ESP32 (updates Firebase/Telegram)
  Serial.println("[STM32] System RESET — monitoring resumed");

  // Small visual confirmation: blink LED twice
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
}

// ─────────────────────────────────────────────────────────────
//  Parse commands arriving FROM ESP32 (remote reset/unlock)
// ─────────────────────────────────────────────────────────────
void handleESP32Command(String cmd) {
  cmd.trim();
  if (cmd.length() == 0 || !cmd.startsWith("CMD:")) return;

  if (cmd == "CMD:RESET") {
    resetSystem();
  } else if (cmd == "CMD:UNLOCK") {
    unlockDoor();
    Serial.println("[STM32] Door UNLOCKED by remote command");
  } else if (cmd == "CMD:LOCK") {
    lockDoor();
    Serial.println("[STM32] Door LOCKED by remote command");
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  // Serial = USART2 = PA2(TX)/PA3(RX) = USB bridge + ESP32 link
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  Serial.println("[STM32] Intelligent Home Security System booting...");

  // Ultrasonic
  pinMode(TRIG_PIN,  OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN,  INPUT_PULLDOWN);   // pulldown prevents float noise

  // Outputs
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Reset button — active LOW with internal pull-up
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  // Servo — attach and lock door immediately
  doorServo.attach(SERVO_PIN);
  doorServo.write(DOOR_LOCKED_ANGLE);
  delay(500);                           // let servo reach position

  // Announce armed state to ESP32
  delay(1000);                          // wait for ESP32 to boot
  Serial.println("ARMED");

  // Startup sensor self-test
  Serial.println("[STM32] === SENSOR SELF-TEST (5 readings) ===");
  for (int i = 0; i < 5; i++) {
    float d = getDistance();
    Serial.print("[STM32] Distance: ");
    if (d < 0) Serial.println("--- (no echo)");
    else { Serial.print(d, 1); Serial.println(" cm"); }
    delay(300);
  }
  Serial.println("[STM32] === SETUP COMPLETE — MONITORING ===");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. Read remote commands from ESP32 ───────────────────────
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleESP32Command(line);
  }

  // ── 2. Manual reset button (debounced) ───────────────────────
  if (now - lastBtnCheck >= DEBOUNCE_MS) {
    lastBtnCheck = now;
    bool btnNow = digitalRead(RESET_BTN_PIN);
    if (btnNow == LOW && lastBtnState == HIGH) {   // falling edge = press
      if (sysState != STATE_ARMED) {
        resetSystem();
      }
    }
    lastBtnState = btnNow;
  }

  // ── 3. Auto-silence buzzer after ALARM_DURATION_MS ───────────
  //    (LED stays ON to indicate alert-hold; buzzer stops)
  if (alarmActive && (now - alarmStartedAt >= ALARM_DURATION_MS)) {
    digitalWrite(BUZZER_PIN, LOW);    // silence buzzer
    alarmActive = false;
    sysState = STATE_ALARM_HOLD;
    Serial.println("[STM32] Buzzer silenced — waiting for reset");
  }

  // ── 4. Poll ultrasonic sensor ─────────────────────────────────
  if (now - lastSensorPoll >= SENSOR_POLL_MS) {
    lastSensorPoll = now;

    float dist = getDistance();

    // Print live reading to Serial Monitor
    Serial.print("[STM32] Dist: ");
    if (dist < 0) Serial.print("N/A");
    else          { Serial.print(dist, 1); Serial.print(" cm"); }
    Serial.print(" | State: ");
    Serial.println(sysState == STATE_ARMED      ? "ARMED" :
                   sysState == STATE_INTRUDER   ? "INTRUDER" :
                                                  "ALARM_HOLD");

    if (sysState == STATE_ARMED) {
      // Only trigger if valid reading AND within threshold
      if (dist > 0 && dist < TRIGGER_DISTANCE_CM) {
        handleIntruder(dist);
      }
    } else if (sysState == STATE_ALARM_HOLD) {
      // Auto re-arm if area is clear again
      if (dist > 0 && dist > RESET_DISTANCE_CM) {
        Serial.println("[STM32] Area clear — auto re-arming");
        resetSystem();
      }
    }
  }
}
