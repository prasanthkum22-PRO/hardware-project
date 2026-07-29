# Smart Room Panel — STM32 + ESP32 + Firebase

DHT11 (temp/humidity) + PIR (motion) + Relay (light) + I2C LCD on an
STM32 Nucleo F446RE, bridged to Firebase Realtime Database through an
ESP32, with a web dashboard to view sensor data and toggle the light.

```
Web Dashboard  <-->  Firebase RTDB  <-->  ESP32 (WiFi)  <-- UART -->  STM32 F446RE
                                                                        |- DHT11
                                                                        |- PIR
                                                                        |- Relay (light)
                                                                        |- I2C LCD
```

## Files

- `stm32_sketch/stm32_sketch.ino` — upload to the Nucleo F446RE
- `esp32_sketch/esp32_sketch.ino` — upload to the ESP32
- `web_dashboard/index.html` — open in a browser, or host on Firebase Hosting / GitHub Pages

## 1. Wiring

### DHT11 → STM32
| DHT11 | STM32 |
|---|---|
| VCC | 5V |
| GND | GND |
| DATA | PB0 |

### PIR sensor → STM32
| PIR | STM32 |
|---|---|
| VCC | 5V |
| GND | GND |
| OUT | PB1 |

### Relay module (light) → STM32
| Relay | STM32 |
|---|---|
| VCC | 5V |
| GND | GND |
| IN | PA5 |

⚠️ Never wire mains voltage directly to the STM32. Use a relay module rated
for your light's voltage/current, and keep the mains side fully isolated.

### 16x2 I2C LCD → STM32
| LCD | STM32 |
|---|---|
| VCC | 5V |
| GND | GND |
| SDA | PB9 |
| SCL | PB8 |

### ESP32 ↔ STM32 (UART bridge)
| ESP32 | STM32 |
|---|---|
| GND | GND |
| TX2 (GPIO17) | PA10 (RX) |
| RX2 (GPIO16) | PA9 (TX) |

## 2. Arduino IDE setup

**STM32 side**
- Board: `Nucleo-64`, Board part number: `Nucleo F446RE`, Upload method: `STLink`
- Libraries to install (Library Manager):
  - `DHT sensor library` (Adafruit) + `Adafruit Unified Sensor`
  - `LiquidCrystal I2C` (Frank de Brabander)
  - `SoftwareSerial` (STM32duino-compatible version)

**ESP32 side**
- Boards Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- Board: `ESP32 Dev Module`
- Library to install: `Firebase ESP Client` (by mobizt)

## 3. Firebase project setup

1. Go to [Firebase Console](https://console.firebase.google.com) → Create project
2. **Realtime Database** → Create database → start in test mode (tighten rules later)
3. **Authentication** → Sign-in method → enable **Email/Password**, and enable **Anonymous** too (the web dashboard uses anonymous sign-in by default)
4. Authentication → Users → add one user (email/password) — this is what the ESP32 sketch uses
5. Project settings → General → copy the **Web API Key** and the **Database URL**
6. Fill these into both `esp32_sketch.ino` and `web_dashboard/index.html`:
   - `API_KEY` / `apiKey`
   - `DATABASE_URL` / `databaseURL`
   - `USER_EMAIL` / `USER_PASSWORD` (ESP32 only)

### Suggested Realtime Database structure
```
/sensor/temperature   (number, written by ESP32)
/sensor/humidity      (number, written by ESP32)
/sensor/motion        (0 or 1, written by ESP32)
/control/light        (0 or 1, written by the web dashboard, read by ESP32)
```

### Example security rules (tighten once it's working)
```json
{
  "rules": {
    "sensor": { ".read": "auth != null", ".write": "auth != null" },
    "control": { ".read": "auth != null", ".write": "auth != null" }
  }
}
```

## 4. Running it

1. Upload `stm32_sketch.ino` to the Nucleo (Serial Monitor at 9600 baud shows debug logs)
2. Upload `esp32_sketch.ino` to the ESP32 (Serial Monitor at 115200 baud — wait for the IP address and "Firebase initialized")
3. Wire the ESP32 and STM32 together as above
4. Open `web_dashboard/index.html` in a browser (double-click works, no server needed)
5. You should see live temperature/humidity/motion, and the rocker switch should toggle your light

## 5. Putting it on GitHub

```bash
cd project
git init
git add .
git commit -m "Smart room panel: STM32 + ESP32 + Firebase"
git branch -M main
git remote add origin https://github.com/<your-username>/<your-repo>.git
git push -u origin main
```

To host the dashboard for free:
- **GitHub Pages**: repo → Settings → Pages → deploy from `main` branch, folder `/web_dashboard` (or move `index.html` to repo root)
- **Firebase Hosting** (keeps everything in one place): `firebase init hosting` → point it at `web_dashboard` → `firebase deploy`

⚠️ Don't commit real WiFi passwords or Firebase credentials to a public repo.
Consider using placeholder values in the committed code and keeping your real
values in a local, gitignored config, or making the repo private.
