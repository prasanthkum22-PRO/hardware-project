#include <SoftwareSerial.h>
#include <DHT11.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define DHTPIN     PB0
#define PIR_PIN    PB1
#define RELAY_PIN  PA5

#define ESP_RX PA10
#define ESP_TX PA9

DHT11 dht11(DHTPIN);
SoftwareSerial espSerial(ESP_RX, ESP_TX);
LiquidCrystal_I2C lcd(0x27, 16, 2);

bool lightState = false;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000;

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);

  pinMode(PIR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Home Init");
  delay(1200);
  lcd.clear();

  Serial.println("STM32 ready.");
}

void loop() {
  if (espSerial.available()) {
    char c = espSerial.read();
    if (c == '1') {
      lightState = true;
      digitalWrite(RELAY_PIN, HIGH);
      Serial.println("Light ON (via Firebase)");
    } else if (c == '0') {
      lightState = false;
      digitalWrite(RELAY_PIN, LOW);
      Serial.println("Light OFF (via Firebase)");
    }
  }

  if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = millis();

    int temperature = 0;
    int humidity = 0;
    int result = dht11.readTemperatureHumidity(temperature, humidity);

    if (result != 0) {
      Serial.println("DHT11 read failed - check wiring");
      return;
    }

    int motion = digitalRead(PIR_PIN);

    espSerial.print("T");
    espSerial.print(temperature);
    espSerial.print("H");
    espSerial.print(humidity);
    espSerial.print("M");
    espSerial.print(motion);
    espSerial.print("\n");

    lcd.setCursor(0, 0);
    lcd.print("T:");
    lcd.print(temperature);
    lcd.print("C H:");
    lcd.print(humidity);
    lcd.print("%  ");

    lcd.setCursor(0, 1);
    lcd.print("Mot:");
    lcd.print(motion ? "YES" : "NO ");
    lcd.print(" L:");
    lcd.print(lightState ? "ON " : "OFF");

    Serial.print("Temp: "); Serial.print(temperature);
    Serial.print("C  Hum: "); Serial.print(humidity);
    Serial.print("%  Motion: "); Serial.println(motion);
  }
}