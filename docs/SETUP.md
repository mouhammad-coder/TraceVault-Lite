# Setup and upload

## Requirements

- Arduino IDE 2.x or Arduino CLI
- ESP32 board package (`esp32:esp32`)
- ESP32 Dev Module target (`esp32:esp32:esp32`)
- MFRC522
- Keypad
- LiquidCrystal I2C
- ESP32Servo
- UniversalTelegramBot
- ArduinoJson (dependency of UniversalTelegramBot)

The sketch also uses ESP32-core libraries for Wi-Fi, TLS, SPI, I2C, time, and
FreeRTOS support.

## Private configuration

Copy `secrets.example.h` to `secrets.h` in the repository root. Replace the
placeholder values in the copy:

```cpp
#define TRACEVAULT_WIFI_SSID "YOUR_WIFI_SSID"
#define TRACEVAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define TRACEVAULT_BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define TRACEVAULT_CHAT_ID "YOUR_TELEGRAM_CHAT_ID"
#define TRACEVAULT_ACCESS_PIN "1234"
#define TRACEVAULT_RFID_UID {0xDE, 0xAD, 0xBE, 0xEF}
```

`secrets.h` is ignored by Git. Keep the access PIN at exactly four digits. To
discover a card UID, temporarily set `rfidLearningMode` to `true`, scan the
card, read the UID from the serial monitor, then return learning mode to
`false`.

## Compile with Arduino CLI

Arduino CLI expects the primary `.ino` filename to match its containing sketch
directory. From the repository root, create an ignored verification copy:

```powershell
New-Item -ItemType Directory -Force build/TraceVault_Lite
Copy-Item TraceVault_Lite.ino,secrets.example.h build/TraceVault_Lite/
arduino-cli compile --fqbn esp32:esp32:esp32 build/TraceVault_Lite
```

If you want to validate your private configuration too, copy `secrets.h` into
that temporary sketch directory before compiling. The `build` directory is
ignored by Git.

## Upload

Select **ESP32 Dev Module**, choose the connected serial port, and upload from
Arduino IDE. With Arduino CLI, replace `COMx` with the detected port:

```powershell
arduino-cli upload --fqbn esp32:esp32:esp32 --port COMx build/TraceVault_Lite
```

Open the serial monitor at 115200 baud for startup diagnostics and RFID
learning output.

## Hardware checks before power-on

- Confirm every GPIO against the table in the main README.
- Confirm the RC522 is powered at 3.3 V.
- Confirm the HC-SR04 ECHO divider reaches GPIO 39 at the resistor junction.
- Confirm all four keypad read lines have individual 10 kΩ pull-ups to 3.3 V.
- Confirm each LED has a current-limiting resistor.
- Use a suitable 5 V servo supply and connect all grounds together.
