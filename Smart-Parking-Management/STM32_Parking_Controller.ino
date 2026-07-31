/*
 * ============================================================
 *  Smart Parking Management System — STM32F446RE CONTROLLER
 * ============================================================
 *
 *  ROLE  : Main controller
 *          - Reads ultrasonic sensors (entry + exit)
 *          - Controls servo gate (entry + exit)
 *          - Manages 10 parking slots (assigns slot numbers)
 *          - Drives 16x2 I2C LCD (slot status display)
 *          - Talks to ESP32 over UART (slot data + gate cmds)
 *
 *  Board : STM32 Nucleo-F446RE
 *  IDE   : Arduino IDE  (board package: STM32duino)
 *
 *  Libraries (Library Manager):
 *    - LiquidCrystal_I2C by Frank de Brabander
 *    - Servo  (built-in Arduino library — works on STM32duino)
 *
 * ============================================================
 *  ──────────────────────────────────────────────
 *  FULL PIN CONNECTION TABLE  (STM32F446RE)
 *  ──────────────────────────────────────────────
 *
 *  ── ENTRY Ultrasonic (HC-SR04) ──
 *  HC-SR04 VCC  →  5V  (CN7 pin 18)
 *  HC-SR04 GND  →  GND (CN7 pin 20)
 *  HC-SR04 TRIG →  PA8   (D7 on morpho header)
 *  HC-SR04 ECHO →  PA9   (D8) — 3.3V safe via 1kΩ + 2kΩ voltage divider
 *
 *  ── EXIT Ultrasonic (HC-SR04) ──
 *  HC-SR04 VCC  →  5V
 *  HC-SR04 GND  →  GND
 *  HC-SR04 TRIG →  PC6   (D12)
 *  HC-SR04 ECHO →  PC7   (D13) — 3.3V safe via 1kΩ + 2kΩ divider
 *
 *  ── ENTRY Servo (SG90) ──
 *  Servo VCC    →  5V (external supply recommended, 500mA+)
 *  Servo GND    →  GND (common ground with STM32)
 *  Servo Signal →  PA6   (D12 / TIM3_CH1 — PWM capable)
 *
 *  ── EXIT Servo (SG90) ──
 *  Servo VCC    →  5V (same external supply)
 *  Servo GND    →  GND
 *  Servo Signal →  PA7   (D11 / TIM3_CH2 — PWM capable)
 *
 *  ── I2C LCD 16x2 (PCF8574 backpack, addr 0x27) ──
 *  LCD VCC  →  5V
 *  LCD GND  →  GND
 *  LCD SDA  →  PB9   (I2C1_SDA — CN10 pin 5)
 *  LCD SCL  →  PB8   (I2C1_SCL — CN10 pin 3)
 *
 *  ── UART to ESP32 ──
 *  STM32 PA2 (TX2) →  ESP32 GPIO16 (RX2)  [with 1kΩ series on ESP32 RX]
 *  STM32 PA3 (RX2) →  ESP32 GPIO17 (TX2)
 *  Common GND      →  GND (both boards share ground)
 *  NOTE: STM32 is 3.3V; ESP32 is 3.3V — direct connection is safe.
 *
 *  ── VOLTAGE DIVIDER for 5V ECHO → 3.3V ──
 *  ECHO pin → 1kΩ → STM32 pin
 *                 ↓
 *               2kΩ
 *                 ↓
 *               GND
 *  (divides 5V × 2/(1+2) = 3.3V)
 *
 * ============================================================
 *  UART PROTOCOL (STM32 ↔ ESP32, 115200 8N1)
 *
 *  STM32 → ESP32 messages (newline terminated):
 *    SLOT:n:IN      car entered slot n (n = 1..10)
 *    SLOT:n:OUT     slot n cleared
 *    SLOTS:s1,s2,…  full occupied slot list (comma separated)
 *    GATE:ENTRY:OPEN
 *    GATE:ENTRY:CLOSE
 *    GATE:EXIT:OPEN
 *    GATE:EXIT:CLOSE
 *
 *  ESP32 → STM32 commands (from website/Firebase):
 *    CMD:CLEAR:n    clear slot n (website button press)
 *    CMD:OPEN:ENTRY open entry gate remotely
 *    CMD:OPEN:EXIT  open exit gate remotely
 *
 * ============================================================
 */

#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <Wire.h>

// ── I2C LCD ───────────────────────────────────────────────────
// Address 0x27 is standard for PCF8574-based backpacks.
// If LCD shows nothing, try 0x3F.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── Servo objects ─────────────────────────────────────────────
Servo entryServo;
Servo exitServo;

// ── Pin Definitions ───────────────────────────────────────────
// Entry ultrasonic
#define ENTRY_TRIG_PIN PA8
#define ENTRY_ECHO_PIN PA9

// Exit ultrasonic
#define EXIT_TRIG_PIN PC6
#define EXIT_ECHO_PIN PC7

// Servo signal pins
#define ENTRY_SERVO_PIN PA6
#define EXIT_SERVO_PIN PA7

// ── Servo angles ──────────────────────────────────────────────
#define GATE_OPEN_ANGLE 90 // degrees — barrier lifts
#define GATE_CLOSE_ANGLE 0 // degrees — barrier down

// ── Detection threshold ───────────────────────────────────────
#define DETECT_DISTANCE_CM 15 // car detected if < 15 cm

// ── Parking state ─────────────────────────────────────────────
#define MAX_SLOTS 10
bool slotOccupied[MAX_SLOTS + 1]; // index 1–10 (index 0 unused)
int totalCars = 0;                // current count

// ── Gate state ────────────────────────────────────────────────
bool entryGateOpen = false;
bool exitGateOpen = false;

// ── Timing ────────────────────────────────────────────────────
#define SENSOR_POLL_MS 200UL       // poll ultrasonic every 200 ms
#define GATE_CLOSE_DELAY_MS 3000UL // keep gate open 3 s after detection clears
#define LCD_REFRESH_MS 1000UL      // refresh LCD every 1 s
#define SLOT_SEND_INTERVAL 5000UL  // send full slot list to ESP32 every 5 s

unsigned long lastSensorPoll = 0;
unsigned long lastLcdRefresh = 0;
unsigned long lastSlotSend = 0;
unsigned long entryGateOpenAt = 0;
unsigned long exitGateOpenAt = 0;

// ── UART to ESP32 via Serial2 (PA2/PA3) ──────────────────────
#define ESP32_SERIAL Serial2 // STM32duino maps USART2 to Serial2

// ─────────────────────────────────────────────────────────────
//  Helper: measure distance with HC-SR04 (returns cm)
// ─────────────────────────────────────────────────────────────
float measureDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // pulseIn timeout = 30 ms (distance ~5 m max)
  long duration = pulseIn(echoPin, HIGH, 30000UL);
  if (duration == 0)
    return 999.0;                  // timeout = nothing detected
  return (duration * 0.034) / 2.0; // cm
}

// ─────────────────────────────────────────────────────────────
//  Helper: assign the first free slot; returns slot number or 0
// ─────────────────────────────────────────────────────────────
int assignNextFreeSlot() {
  for (int i = 1; i <= MAX_SLOTS; i++) {
    if (!slotOccupied[i])
      return i;
  }
  return 0; // parking full
}

// ─────────────────────────────────────────────────────────────
//  Gate control
// ─────────────────────────────────────────────────────────────
void openEntryGate() {
  if (!entryGateOpen) {
    entryServo.write(GATE_OPEN_ANGLE);
    entryGateOpen = true;
    entryGateOpenAt = millis();
    ESP32_SERIAL.println("GATE:ENTRY:OPEN");
    Serial.println("[STM32] Entry gate OPENED");
  }
}

void closeEntryGate() {
  if (entryGateOpen) {
    entryServo.write(GATE_CLOSE_ANGLE);
    entryGateOpen = false;
    ESP32_SERIAL.println("GATE:ENTRY:CLOSE");
    Serial.println("[STM32] Entry gate CLOSED");
  }
}

void openExitGate() {
  if (!exitGateOpen) {
    exitServo.write(GATE_OPEN_ANGLE);
    exitGateOpen = true;
    exitGateOpenAt = millis();
    ESP32_SERIAL.println("GATE:EXIT:OPEN");
    Serial.println("[STM32] Exit gate OPENED");
  }
}

void closeExitGate() {
  if (exitGateOpen) {
    exitServo.write(GATE_CLOSE_ANGLE);
    exitGateOpen = false;
    ESP32_SERIAL.println("GATE:EXIT:CLOSE");
    Serial.println("[STM32] Exit gate CLOSED");
  }
}

// ─────────────────────────────────────────────────────────────
//  Car enters — assign slot, update state, notify ESP32
// ─────────────────────────────────────────────────────────────
void carEntered() {
  int slot = assignNextFreeSlot();
  if (slot == 0) {
    Serial.println("[STM32] Parking FULL — entry denied");
    updateLCDFull();
    return;
  }
  slotOccupied[slot] = true;
  totalCars++;
  openEntryGate();

  // Notify ESP32
  ESP32_SERIAL.print("SLOT:");
  ESP32_SERIAL.print(slot);
  ESP32_SERIAL.println(":IN");

  Serial.print("[STM32] Car entered → Slot ");
  Serial.println(slot);
  updateLCD();
}

// ─────────────────────────────────────────────────────────────
//  Car exits — free the last occupied slot, notify ESP32
//  (Physical exit: simply open exit gate; slot cleared by
//   website button CMD:CLEAR:n, or last-in/first-out here)
// ─────────────────────────────────────────────────────────────
void carExited() {
  openExitGate();
  Serial.println("[STM32] Exit sensor triggered — gate opened");
  // Actual slot clearing is handled via website button → ESP32 → CMD:CLEAR:n
}

// ─────────────────────────────────────────────────────────────
//  Clear a specific slot (called from website button command)
// ─────────────────────────────────────────────────────────────
void clearSlot(int slot) {
  if (slot < 1 || slot > MAX_SLOTS)
    return;
  if (!slotOccupied[slot])
    return; // already free

  slotOccupied[slot] = false;
  totalCars = max(0, totalCars - 1);

  ESP32_SERIAL.print("SLOT:");
  ESP32_SERIAL.print(slot);
  ESP32_SERIAL.println(":OUT");

  Serial.print("[STM32] Slot ");
  Serial.print(slot);
  Serial.println(" CLEARED by remote command");
  updateLCD();
}

// ─────────────────────────────────────────────────────────────
//  Clear ALL slots (website "Clear All" button)
// ─────────────────────────────────────────────────────────────
void clearAllSlots() {
  for (int i = 1; i <= MAX_SLOTS; i++)
    slotOccupied[i] = false;
  totalCars = 0;
  ESP32_SERIAL.println("SLOTS:CLEARED");
  Serial.println("[STM32] ALL slots cleared by remote command");
  updateLCD();
}

// ─────────────────────────────────────────────────────────────
//  LCD update — Line 1: "Cars: X  Free: Y"
//               Line 2: slot numbers of occupied slots
// ─────────────────────────────────────────────────────────────
void updateLCD() {
  lcd.clear();

  // Line 1: summary
  lcd.setCursor(0, 0);
  lcd.print("Cars:");
  lcd.print(totalCars);
  lcd.print(" Free:");
  lcd.print(MAX_SLOTS - totalCars);

  // Line 2: list occupied slot numbers (up to 16 chars)
  lcd.setCursor(0, 1);
  String slots = "Sl:";
  bool any = false;
  for (int i = 1; i <= MAX_SLOTS; i++) {
    if (slotOccupied[i]) {
      if (any)
        slots += ",";
      slots += String(i);
      any = true;
    }
  }
  if (!any)
    slots = "Parking Empty";
  // trim to 16 chars
  if (slots.length() > 16)
    slots = slots.substring(0, 16);
  lcd.print(slots);
}

void updateLCDFull() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  PARKING FULL  ");
  lcd.setCursor(0, 1);
  lcd.print(" No Free Slots! ");
}

// ─────────────────────────────────────────────────────────────
//  Send full slot list to ESP32 (periodic sync)
//  Format: SLOTS:1,3,7  (occupied slot numbers)
// ─────────────────────────────────────────────────────────────
void sendFullSlotList() {
  ESP32_SERIAL.print("SLOTS:");
  bool any = false;
  for (int i = 1; i <= MAX_SLOTS; i++) {
    if (slotOccupied[i]) {
      if (any)
        ESP32_SERIAL.print(",");
      ESP32_SERIAL.print(i);
      any = true;
    }
  }
  if (!any)
    ESP32_SERIAL.print("EMPTY");
  ESP32_SERIAL.println();
}

// ─────────────────────────────────────────────────────────────
//  Parse commands arriving from ESP32 (from website/Firebase)
//  CMD:CLEAR:n      — clear slot n
//  CMD:CLEAR:ALL    — clear all slots
//  CMD:OPEN:ENTRY   — open entry gate remotely
//  CMD:OPEN:EXIT    — open exit gate remotely
// ─────────────────────────────────────────────────────────────
void handleESP32Command(String cmd) {
  cmd.trim();
  Serial.print("[STM32] CMD from ESP32: ");
  Serial.println(cmd);

  if (cmd.startsWith("CMD:CLEAR:ALL")) {
    clearAllSlots();
  } else if (cmd.startsWith("CMD:CLEAR:")) {
    int slot = cmd.substring(10).toInt();
    clearSlot(slot);
  } else if (cmd == "CMD:OPEN:ENTRY") {
    openEntryGate();
  } else if (cmd == "CMD:OPEN:EXIT") {
    openExitGate();
  }
}

// ─────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  // Debug serial (USB / UART1)
  Serial.begin(115200);
  while (!Serial && millis() < 3000)
    ;
  Serial.println("[STM32] Smart Parking Controller booting...");

  // UART2 → ESP32
  ESP32_SERIAL.begin(115200);

  // Ultrasonic pins
  pinMode(ENTRY_TRIG_PIN, OUTPUT);
  pinMode(ENTRY_ECHO_PIN, INPUT);
  pinMode(EXIT_TRIG_PIN, OUTPUT);
  pinMode(EXIT_ECHO_PIN, INPUT);

  // Servos — attach and close both gates at start
  entryServo.attach(ENTRY_SERVO_PIN);
  exitServo.attach(EXIT_SERVO_PIN);
  entryServo.write(GATE_CLOSE_ANGLE);
  exitServo.write(GATE_CLOSE_ANGLE);

  // Init slot array
  for (int i = 0; i <= MAX_SLOTS; i++)
    slotOccupied[i] = false;

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Parking");
  lcd.setCursor(0, 1);
  lcd.print("System Ready");
  delay(2000);
  updateLCD();

  Serial.println("[STM32] Setup complete.");
}

// ─────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── 1. Read commands from ESP32 ──────────────────────────────
  while (ESP32_SERIAL.available()) {
    String line = ESP32_SERIAL.readStringUntil('\n');
    if (line.length() > 0)
      handleESP32Command(line);
  }

  // ── 2. Poll ultrasonic sensors ───────────────────────────────
  if (now - lastSensorPoll >= SENSOR_POLL_MS) {
    lastSensorPoll = now;

    float entryDist = measureDistance(ENTRY_TRIG_PIN, ENTRY_ECHO_PIN);
    float exitDist = measureDistance(EXIT_TRIG_PIN, EXIT_ECHO_PIN);

    Serial.print("[STM32] Entry: ");
    Serial.print(entryDist, 1);
    Serial.print(" cm  |  Exit: ");
    Serial.print(exitDist, 1);
    Serial.println(" cm");

    // Car at entry sensor and parking not full
    if (entryDist < DETECT_DISTANCE_CM && totalCars < MAX_SLOTS) {
      if (!entryGateOpen)
        carEntered();
      else
        entryGateOpenAt = now; // extend open time
    }

    // Car at exit sensor
    if (exitDist < DETECT_DISTANCE_CM) {
      if (!exitGateOpen)
        carExited();
      else
        exitGateOpenAt = now; // extend open time
    }
  }

  // ── 3. Auto-close gates after delay ─────────────────────────
  if (entryGateOpen && (now - entryGateOpenAt) >= GATE_CLOSE_DELAY_MS) {
    closeEntryGate();
  }
  if (exitGateOpen && (now - exitGateOpenAt) >= GATE_CLOSE_DELAY_MS) {
    closeExitGate();
  }

  // ── 4. Periodic LCD refresh ──────────────────────────────────
  if (now - lastLcdRefresh >= LCD_REFRESH_MS) {
    lastLcdRefresh = now;
    if (totalCars >= MAX_SLOTS)
      updateLCDFull();
    else
      updateLCD();
  }

  // ── 5. Periodic slot sync to ESP32 ───────────────────────────
  if (now - lastSlotSend >= SLOT_SEND_INTERVAL) {
    lastSlotSend = now;
    sendFullSlotList();
  }
}
