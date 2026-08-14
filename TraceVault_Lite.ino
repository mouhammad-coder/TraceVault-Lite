/*
  TraceVault Lite - IoT Cybersecurity Safe
  Board: ESP32 DevKit V1 / ESP32-WROOM-32

  Unlocking is decided only by the local RFID and PIN checks.
  Wi-Fi and Telegram are used only for security alerts.

  Corrected keypad wiring:
  - Physical pin 1 -> GPIO 13
  - Physical pin 2 -> GPIO 14
  - Physical pin 3 -> GPIO 16
  - Physical pin 4 -> GPIO 17
  - Physical pin 5 -> GPIO 32
  - Physical pin 6 -> GPIO 34
  - Physical pin 7 -> GPIO 35
  - Physical pin 8 -> GPIO 36 / VP
  - Connect one 10 kOhm pull-up resistor from each keypad input
    pin (GPIO 32, 34, 35 and 36) to 3.3 V.
  - HC-SR04 TRIG: GPIO 26
  - HC-SR04 ECHO: through a 1 kOhm / 2 kOhm voltage divider
    to GPIO 39. Connect ECHO to 1 kOhm, the resistor junction to
    GPIO 39, and 2 kOhm from the junction to GND.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <time.h>
#include <esp_system.h>

// ==================================================
// USER CONFIGURATION
// ==================================================

// Copy secrets.example.h to secrets.h and customize it locally. The ignored
// secrets.h file is used when present; safe placeholders keep public builds
// compilable when it is absent.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define TRACEVAULT_WIFI_SSID "YOUR_WIFI_SSID"
  #define TRACEVAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
  #define TRACEVAULT_BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
  #define TRACEVAULT_CHAT_ID "YOUR_TELEGRAM_CHAT_ID"
  #define TRACEVAULT_ACCESS_PIN "1234"
  #define TRACEVAULT_RFID_UID {0xDE, 0xAD, 0xBE, 0xEF}
#endif

const char* wifiName = TRACEVAULT_WIFI_SSID;
const char* wifiPassword = TRACEVAULT_WIFI_PASSWORD;

#define BOT_TOKEN TRACEVAULT_BOT_TOKEN
#define CHAT_ID TRACEVAULT_CHAT_ID

const char* buildIdentifier =
  "TRACEVAULT-STABLE-ALARM-V3";

const char* beirutTimeZone = "EET-2EEST,M3.5.0/0,M10.5.0/0";
const char* primaryNtpServer = "pool.ntp.org";
const char* secondaryNtpServer = "time.nist.gov";

String correctPin = TRACEVAULT_ACCESS_PIN;

// This is the only UID allowed to continue to PIN entry.
byte authorizedRFID[] = TRACEVAULT_RFID_UID;

int lockedAngle = 15;
int unlockedAngle = 165;
int maximumFailedAttempts = 3;
int lockdownSeconds = 30;
int detectionDistanceCm = 60;
const int loudestBuzzerFrequency = 2800;

bool debugMode = true;
bool rfidLearningMode = false;
bool sendSuccessfulAccessAlerts = true;

// ==================================================
// PIN DEFINITIONS
// ==================================================

// RC522 RFID reader
const int rfidSckPin = 18;
const int rfidMosiPin = 23;
const int rfidMisoPin = 19;
const int rfidSsPin = 5;
const int rfidResetPin = 27;

// LCD, servo and ultrasonic sensor
const int lcdSdaPin = 21;
const int lcdSclPin = 22;
const int servoPin = 25;
const int ultrasonicTrigPin = 26;
const int ultrasonicEchoPin = 39;

// LEDs and passive / tone-capable buzzer
const int yellowLedPin = 12;
const int greenLedPin = 4;
const int redLedPin = 15;
const int buzzerPin = 33;
const int buzzerLedcChannel = 6;

// ==================================================
// KEYPAD CONFIGURATION
// ==================================================

const byte keypadRows = 4;
const byte keypadColumns = 4;

char keypadButtons[keypadRows][keypadColumns] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Physical keypad pins 1 to 4 are the output drive lines.
const byte keypadDrivePins[4] = {
  13, 14, 16, 17
};

// Physical keypad pins 5 to 8 are the input read lines.
// GPIO 34, 35 and 36 are input-only ESP32 pins.
const byte keypadReadPins[4] = {
  32, 34, 35, 36
};

// Use an external 10 kOhm pull-up from EACH input pin to 3.3 V:
// GPIO 32, GPIO 34, GPIO 35 and GPIO 36.
// The scanner drives only keypadDrivePins and reads only keypadReadPins.
// GPIO 32, 34, 35 and 36 are never driven.

// ==================================================
// HARDWARE OBJECTS AND SYSTEM VARIABLES
// ==================================================

MFRC522 rfidReader(rfidSsPin, rfidResetPin);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo doorServo;
WiFiClientSecure telegramClient;
UniversalTelegramBot telegramBot(BOT_TOKEN, telegramClient);

bool waitingForPin = false;
bool doorIsUnlocked = false;
bool personIsNearby = false;
bool lockdownActive = false;
bool lockdownAlertSent = false;

int failedAttempts = 0;
String enteredPin = "";

byte scannedRFID[10];
byte scannedRFIDLength = 0;

unsigned long lockdownStartTime = 0;
unsigned long lastDistanceTime = 0;
unsigned long lastWiFiTime = 0;
unsigned long lastRFIDTime = 0;
unsigned long lastAlarmToneChange = 0;

int lastCountdownValue = -1;
int alarmLowFrequency = 2650;
int alarmHighFrequency = 2950;
int wrongPinFailedAttempts = 0;
int unauthorizedRfidFailedAttempts = 0;
bool alarmUsingHighFrequency = false;
bool systemStartedAlertAttempted = false;
bool clockSyncStarted = false;
bool servoDetachPending = false;

TaskHandle_t lockdownTelegramTaskHandle = nullptr;

// The program remembers the last commanded servo position.
int currentServoAngle = lockedAngle;
bool servoIsAttached = false;
unsigned long servoDetachTime = 0;

// ==================================================
// BASIC DISPLAY, LIGHT AND SOUND FUNCTIONS
// ==================================================

void debugPrint(String message) {
  if (debugMode == true) {
    Serial.println(message);
  }
}

void displayMessage(String line1, String line2) {
  if (line1.length() > 16) {
    line1 = line1.substring(0, 16);
  }
  if (line2.length() > 16) {
    line2 = line2.substring(0, 16);
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// Lockdown uses a stable display and updates only the countdown line.
// Both lines are padded to 16 characters, so lcd.clear() is not needed.
void displayLockdownCountdown(int secondsRemaining) {
  lcd.setCursor(0, 0);
  lcd.print("!!! BREACH !!!  ");

  String secondLine =
    "LOCKED: " + String(secondsRemaining) + " SEC";

  while (secondLine.length() < 16) {
    secondLine += " ";
  }

  if (secondLine.length() > 16) {
    secondLine = secondLine.substring(0, 16);
  }

  lcd.setCursor(0, 1);
  lcd.print(secondLine);
}

void setStatusLights(bool yellow, bool green, bool red) {
  if (yellow == true) {
    digitalWrite(yellowLedPin, HIGH);
  } else {
    digitalWrite(yellowLedPin, LOW);
  }

  if (green == true) {
    digitalWrite(greenLedPin, HIGH);
  } else {
    digitalWrite(greenLedPin, LOW);
  }

  if (red == true) {
    digitalWrite(redLedPin, HIGH);
  } else {
    digitalWrite(redLedPin, LOW);
  }
}

void startupLedSelfTest() {
  Serial.println("STARTUP LED TEST: YELLOW");
  setStatusLights(true, false, false);
  delay(400);

  Serial.println("STARTUP LED TEST: GREEN");
  setStatusLights(false, true, false);
  delay(700);

  Serial.println("STARTUP LED TEST: RED");
  setStatusLights(false, false, true);
  delay(400);

  setStatusLights(true, false, false);
}

void showRFIDScanningAnimation() {
  displayMessage("RFID SCANNING", "[>         ]");
  delay(65);

  displayMessage("RFID SCANNING", "[====>     ]");
  delay(65);

  displayMessage("RFID SCANNING", "[=========>]");
  delay(65);
}

void flashGreenForAccess() {
  for (int count = 0; count < 2; count++) {
    setStatusLights(false, true, false);
    delay(90);

    setStatusLights(false, false, false);
    delay(50);
  }
}

void flashYellowAfterLock() {
  for (int count = 0; count < 2; count++) {
    setStatusLights(true, false, false);
    delay(100);

    setStatusLights(false, false, false);
    delay(70);
  }

  setStatusLights(true, false, false);
}

// Stop the tone without detaching the LEDC output.
void stopTone() {
  ledcWriteTone(buzzerPin, 0);
}

void playToneStrong(
  int frequency,
  int durationMs,
  int pauseMs = 15
) {
  ledcWriteTone(buzzerPin, frequency);
  delay(durationMs);
  stopTone();
  delay(pauseMs);
}

int safeBuzzerFrequency(int frequency) {
  if (frequency < 500) {
    return 500;
  }

  if (frequency > 5000) {
    return 5000;
  }

  return frequency;
}

void soundKeyPress() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 250),
    18,
    2
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency + 300),
    28,
    0
  );
}

void soundUserDetected() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 900),
    45,
    12
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency),
    75,
    0
  );
}

void soundRFIDAccepted() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 1200),
    55,
    10
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 650),
    65,
    10
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 150),
    80,
    10
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency + 400),
    160,
    0
  );
}

void soundAccessGranted() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 1500),
    55,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 1000),
    60,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 500),
    70,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency),
    90,
    10
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency + 500),
    130,
    15
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency),
    80,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency + 500),
    260,
    0
  );
}

void soundAccessDenied() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency + 400),
    100,
    18
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 200),
    130,
    18
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 900),
    180,
    20
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 1700),
    450,
    0
  );
}

void startLockdownAlarm() {
  alarmLowFrequency =
    safeBuzzerFrequency(loudestBuzzerFrequency - 150);

  alarmHighFrequency =
    safeBuzzerFrequency(loudestBuzzerFrequency + 150);

  alarmUsingHighFrequency = false;
  lastAlarmToneChange = millis();

  ledcWriteTone(buzzerPin, alarmLowFrequency);
}

void updateLockdownAlarm() {
  if (
    millis() - lastAlarmToneChange <
    260
  ) {
    return;
  }

  lastAlarmToneChange = millis();

  alarmUsingHighFrequency =
    !alarmUsingHighFrequency;

  if (alarmUsingHighFrequency == true) {
    ledcWriteTone(buzzerPin, alarmHighFrequency);
  } else {
    ledcWriteTone(buzzerPin, alarmLowFrequency);
  }
}

void soundDoorLocked() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency + 350),
    45,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 200),
    60,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 800),
    85,
    8
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 1500),
    180,
    0
  );
}

void soundSystemReset() {
  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 1200),
    70,
    12
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency - 500),
    85,
    12
  );

  playToneStrong(
    safeBuzzerFrequency(loudestBuzzerFrequency),
    150,
    0
  );
}

void flashRedLight() {
  setStatusLights(false, false, false);

  for (int count = 0; count < 3; count++) {
    setStatusLights(false, false, true);
    delay(100);
    setStatusLights(false, false, false);
    delay(70);
  }
}

// ==================================================
// DOOR FUNCTIONS
// ==================================================

void attachServoIfNeeded() {
  if (servoIsAttached == false) {
    doorServo.setPeriodHertz(50);
    doorServo.attach(servoPin, 500, 2400);
    servoIsAttached = true;

    // Immediately command the last known position after attaching.
    doorServo.write(currentServoAngle);
    delay(20);
  }
}

void moveServoFast(int targetAngle) {
  attachServoIfNeeded();

  // Send the final target immediately.
  doorServo.write(targetAngle);
  currentServoAngle = targetAngle;

  // Keep control pulses active for 600 ms, then detach from loop().
  servoDetachTime = millis() + 600;
  servoDetachPending = true;
}

void updateServoDetach() {
  if (
    servoDetachPending == true &&
    millis() >= servoDetachTime
  ) {
    doorServo.detach();

    servoIsAttached = false;
    servoDetachPending = false;
  }
}

void lockDoor() {
  moveServoFast(lockedAngle);
  doorIsUnlocked = false;
  waitingForPin = false;
  enteredPin = "";
  setStatusLights(true, false, false);
  Serial.println("GREEN LED OFF - YELLOW ARMED LED ON");
  debugPrint("Door is locked.");
}

void unlockDoor() {
  // The servo releases only the latch. It does not push the door.
  moveServoFast(unlockedAngle);
  doorIsUnlocked = true;
  waitingForPin = false;
  enteredPin = "";
  setStatusLights(false, true, false);
  debugPrint("Door latch is released.");
}

// ==================================================
// WI-FI AND TELEGRAM FUNCTIONS
// ==================================================

void startClockSynchronization() {
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(
      beirutTimeZone,
      primaryNtpServer,
      secondaryNtpServer
    );

    clockSyncStarted = true;
    debugPrint("Clock synchronization started.");
  }
}

String getEventDateTime() {
  struct tm localTime;

  // Use a short timeout so missing NTP never delays the physical vault.
  if (getLocalTime(&localTime, 100) == true) {
    char dateTimeText[40];
    strftime(
      dateTimeText,
      sizeof(dateTimeText),
      "%b %d, %Y • %H:%M:%S",
      &localTime
    );

    return String(dateTimeText);
  }

  return "Time unavailable";
}

void connectToWiFi() {
  debugPrint("Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiName, wifiPassword);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 6000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    telegramClient.setInsecure();
    telegramClient.setTimeout(5000);
    startClockSynchronization();
    debugPrint("Wi-Fi connected. IP: " + WiFi.localIP().toString());
  } else {
    debugPrint("Wi-Fi offline. The physical safe still works.");
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiTime >= 30000) {
      lastWiFiTime = millis();
      debugPrint("Retrying Wi-Fi connection.");
      WiFi.disconnect();
      WiFi.begin(wifiName, wifiPassword);
    }
  } else if (clockSyncStarted == false) {
    // This covers a connection that succeeds after the startup timeout.
    telegramClient.setInsecure();
    telegramClient.setTimeout(5000);
    startClockSynchronization();
  }
}

bool sendTelegramAlert(
  String eventType,
  int attemptNumber,
  String doorStatus
) {
  if (WiFi.status() != WL_CONNECTED) {
    debugPrint("Telegram message failed.");
    return false;
  }

  telegramClient.setInsecure();
  telegramClient.setTimeout(5000);

  String message = "";

  if (eventType == "SYSTEM_STARTED") {
    message = "🟢 TraceVault system started\n";
  } else if (eventType == "UNAUTHORIZED_RFID") {
    message = "🚨 Unauthorized RFID detected\n";
  } else if (eventType == "WRONG_PIN") {
    message = "🔢 Incorrect PIN entered\n";
  } else if (eventType == "LOCKDOWN") {
    message = "🔒 System entered lockdown mode\n";
  } else if (eventType == "ACCESS_GRANTED") {
    message = "✅ Authorized access granted\n";
  } else if (eventType == "DOOR_LOCKED") {
    message = "🔐 Vault locked again\n";
  } else {
    debugPrint("Telegram message failed.");
    return false;
  }

  message += getEventDateTime();

  bool messageSent = telegramBot.sendMessage(
    CHAT_ID,
    message,
    ""
  );

  if (messageSent == true) {
    debugPrint("Telegram message succeeded.");
    return true;
  }

  debugPrint("Telegram message failed.");
  return false;
}

void sendSystemStartedAlertIfNeeded() {
  if (
    WiFi.status() == WL_CONNECTED &&
    systemStartedAlertAttempted == false
  ) {
    // Set this before sending so a Telegram failure cannot cause a loop flood.
    systemStartedAlertAttempted = true;
    sendTelegramAlert("SYSTEM_STARTED", 0, "LOCKED");
  }
}

// This task sends the critical alert without pausing loop().
// It does not control the LCD, LEDs, buzzer, servo or counters.
void sendLockdownTelegramTask(void* parameter) {
  sendTelegramAlert(
    "LOCKDOWN",
    maximumFailedAttempts,
    "LOCKED"
  );

  lockdownTelegramTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ==================================================
// ULTRASONIC AND KEYPAD FUNCTIONS
// ==================================================

float readDistanceCm() {
  digitalWrite(ultrasonicTrigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(ultrasonicTrigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(ultrasonicTrigPin, LOW);

  unsigned long echoTime = pulseIn(ultrasonicEchoPin, HIGH, 25000);
  if (echoTime == 0) {
    return -1.0;
  }

  return echoTime * 0.0343 / 2.0;
}

char readRawKeypad() {
  // Drive one physical keypad row LOW and read all four columns.
  for (int row = 0; row < 4; row++) {
    digitalWrite(keypadDrivePins[row], LOW);
    delayMicroseconds(5);

    for (int column = 0; column < 4; column++) {
      if (digitalRead(keypadReadPins[column]) == LOW) {
        char pressedKey = keypadButtons[row][column];
        digitalWrite(keypadDrivePins[row], HIGH);
        return pressedKey;
      }
    }

    digitalWrite(keypadDrivePins[row], HIGH);
  }

  return NO_KEY;
}

char readKeypad() {
  static char lastRawKey = NO_KEY;
  static char reportedKey = NO_KEY;
  static unsigned long keyChangedTime = 0;

  char rawKey = readRawKeypad();

  if (rawKey != lastRawKey) {
    lastRawKey = rawKey;
    keyChangedTime = millis();
  }

  // Wait until the electrical contacts have been stable for 35 ms.
  if (millis() - keyChangedTime < 35) {
    return NO_KEY;
  }

  if (rawKey == NO_KEY) {
    reportedKey = NO_KEY;
    return NO_KEY;
  }

  // Report a held key only once. It must be released before another report.
  if (reportedKey == NO_KEY) {
    reportedKey = rawKey;

    if (debugMode == true) {
      Serial.print("KEY PRESSED: ");
      Serial.println(rawKey);
    }

    return rawKey;
  }

  return NO_KEY;
}

void showPinStars() {
  String stars = "";
  for (unsigned int index = 0; index < enteredPin.length(); index++) {
    stars += "*";
  }
  displayMessage("ENTER PIN", stars);
}

// ==================================================
// RFID FUNCTIONS
// ==================================================

bool readRFID() {
  if (millis() - lastRFIDTime < 1200) {
    return false;
  }
  if (rfidReader.PICC_IsNewCardPresent() == false) {
    return false;
  }
  if (rfidReader.PICC_ReadCardSerial() == false) {
    return false;
  }

  lastRFIDTime = millis();
  scannedRFIDLength = rfidReader.uid.size;
  if (scannedRFIDLength > 10) {
    scannedRFIDLength = 10;
  }

  for (byte index = 0; index < scannedRFIDLength; index++) {
    scannedRFID[index] = rfidReader.uid.uidByte[index];
  }

  if (debugMode == true) {
    Serial.println("RFID CARD DETECTED");
    printScannedRFID();
  }

  rfidReader.PICC_HaltA();
  rfidReader.PCD_StopCrypto1();
  return true;
}

void printScannedRFID() {
  Serial.print("RFID UID: {");
  for (byte index = 0; index < scannedRFIDLength; index++) {
    Serial.print("0x");
    if (scannedRFID[index] < 0x10) {
      Serial.print("0");
    }
    Serial.print(scannedRFID[index], HEX);
    if (index < scannedRFIDLength - 1) {
      Serial.print(", ");
    }
  }
  Serial.println("}");
}

bool checkRFID() {
  int authorizedLength = sizeof(authorizedRFID) / sizeof(authorizedRFID[0]);

  if (scannedRFIDLength != authorizedLength) {
    return false;
  }

  for (int index = 0; index < authorizedLength; index++) {
    if (scannedRFID[index] != authorizedRFID[index]) {
      return false;
    }
  }

  return true;
}

// ==================================================
// FAILED ATTEMPTS AND LOCKDOWN
// ==================================================

void startLockdown() {
  // Enter lockdown before starting any presentation or network work.
  lockdownActive = true;
  lockdownAlertSent = true;
  lockdownStartTime = millis();
  lastCountdownValue = lockdownSeconds;

  // A normal failed-attempt lockdown is already physically locked.
  // This safety branch runs only if lockdown is entered while unlocked.
  if (doorIsUnlocked == true) {
    moveServoFast(lockedAngle);
  }

  doorIsUnlocked = false;
  waitingForPin = false;
  enteredPin = "";

  // Keep red solid. Yellow and green remain off.
  setStatusLights(false, false, true);
  displayLockdownCountdown(lockdownSeconds);
  startLockdownAlarm();

  // Telegram runs on core 0 and cannot pause the main alarm loop.
  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    sendLockdownTelegramTask,
    "LockdownTelegram",
    8192,
    nullptr,
    1,
    &lockdownTelegramTaskHandle,
    0
  );

  if (taskCreated != pdPASS) {
    lockdownTelegramTaskHandle = nullptr;
    Serial.println(
      "Failed to create lockdown Telegram task."
    );
  }
}

void registerFailedAttempt(String eventType) {
  // The latch is already locked during authentication.
  // Update only the software state; do not command the servo.
  doorIsUnlocked = false;
  waitingForPin = false;
  enteredPin = "";

  failedAttempts++;

  if (eventType == "WRONG_PIN") {
    wrongPinFailedAttempts++;
  } else if (eventType == "UNAUTHORIZED_RFID") {
    unauthorizedRfidFailedAttempts++;
  }

  // Attempt three goes directly to lockdown without the ordinary
  // denial animation, melody or Telegram warning.
  if (failedAttempts >= maximumFailedAttempts) {
    startLockdown();
    return;
  }

  if (eventType == "WRONG_PIN") {
    displayMessage(
      "ACCESS DENIED",
      "WRONG PIN " +
      String(failedAttempts) +
      "/" +
      String(maximumFailedAttempts)
    );
  } else {
    displayMessage(
      "ACCESS DENIED",
      "BAD RFID " +
      String(failedAttempts) +
      "/" +
      String(maximumFailedAttempts)
    );
  }

  setStatusLights(false, false, false);
  flashRedLight();
  soundAccessDenied();
  setStatusLights(true, false, false);

  // Only attempts 1 and 2 send this normal warning.
  sendTelegramAlert(eventType, failedAttempts, "LOCKED");
}

void finishLockdown() {
  stopTone();

  // Restore the armed light without commanding the servo again.
  setStatusLights(true, false, false);

  lockdownActive = false;
  lockdownAlertSent = false;

  failedAttempts = 0;
  wrongPinFailedAttempts = 0;
  unauthorizedRfidFailedAttempts = 0;
  personIsNearby = false;

  doorIsUnlocked = false;
  waitingForPin = false;
  enteredPin = "";

  alarmLowFrequency =
    safeBuzzerFrequency(loudestBuzzerFrequency - 150);
  alarmHighFrequency =
    safeBuzzerFrequency(loudestBuzzerFrequency + 150);
  alarmUsingHighFrequency = false;
  lastAlarmToneChange = 0;
  lastCountdownValue = -1;

  displayMessage("SYSTEM RESET", "VAULT SECURED");
  soundSystemReset();
  delay(500);

  displayMessage("DOOR LOCKED", "SYSTEM ARMED");
}

void handleLockdown() {
  // These operations are non-blocking and run every loop.
  updateLockdownAlarm();
  setStatusLights(false, false, true);

  unsigned long elapsedSeconds =
    (millis() - lockdownStartTime) / 1000;

  if (
    elapsedSeconds >=
    (unsigned long)lockdownSeconds
  ) {
    finishLockdown();
    return;
  }

  int secondsRemaining =
    lockdownSeconds - (int)elapsedSeconds;

  if (secondsRemaining != lastCountdownValue) {
    lastCountdownValue = secondsRemaining;
    displayLockdownCountdown(secondsRemaining);
  }
}

// ==================================================
// PIN, RFID AND PROXIMITY HANDLERS
// ==================================================

void handlePinEntry() {
  char pressedKey = readKeypad();
  if (pressedKey == NO_KEY) {
    return;
  }

  if (pressedKey >= '0' && pressedKey <= '9') {
    if (enteredPin.length() < 4) {
      enteredPin += pressedKey;
      showPinStars();
      soundKeyPress();
    } else {
      soundKeyPress();
    }
  } else if (pressedKey == '*') {
    soundKeyPress();
    enteredPin = "";
    showPinStars();
  } else if (pressedKey == '#') {
    Serial.println("PIN SUBMIT KEY DETECTED");
    soundKeyPress();

    if (enteredPin == correctPin) {
      Serial.println("PIN ACCEPTED");
      enteredPin = "";
      failedAttempts = 0;
      wrongPinFailedAttempts = 0;
      unauthorizedRfidFailedAttempts = 0;

      displayMessage("ACCESS VERIFIED", "UNLOCKING...");

      setStatusLights(false, true, false);
      Serial.println("GREEN LED ON - ACCESS GRANTED");

      // Start the servo before any long animation or sound.
      moveServoFast(unlockedAngle);
      Serial.println("SERVO UNLOCK COMMAND: 165");
      doorIsUnlocked = true;
      waitingForPin = false;

      // The servo is already moving during this animation.
      flashGreenForAccess();
      setStatusLights(false, true, false);

      displayMessage("ACCESS GRANTED", "DOOR UNLOCKED");
      soundAccessGranted();

      if (sendSuccessfulAccessAlerts == true) {
        sendTelegramAlert("ACCESS_GRANTED", 0, "UNLOCKED");
      }

      displayMessage("CLOSE THE DOOR", "PRESS D TO LOCK");
    } else {
      Serial.println("PIN REJECTED");
      // Erase the entered PIN before handling the failure.
      enteredPin = "";
      waitingForPin = false;
      registerFailedAttempt("WRONG_PIN");
    }
  } else if (pressedKey == 'D') {
    soundKeyPress();
    enteredPin = "";
    waitingForPin = false;
    lockDoor();
    displayMessage("SCAN CANCELLED", "SCAN RFID");
  } else {
    soundKeyPress();
  }
}

void handleRFIDScanning() {
  if (readRFID() == false) {
    return;
  }

  showRFIDScanningAnimation();

  if (rfidLearningMode == true) {
    printScannedRFID();
    displayMessage("RFID UID SHOWN", "CHECK SERIAL");
    soundKeyPress();
  } else if (checkRFID() == true) {
    enteredPin = "";
    waitingForPin = true;

    displayMessage("RFID VERIFIED", "ENTER PIN");
    soundRFIDAccepted();
    delay(200);

    displayMessage("ENTER PIN", "");
  } else {
    registerFailedAttempt("UNAUTHORIZED_RFID");
  }
}

void handleProximity() {
  if (millis() - lastDistanceTime < 250) {
    return;
  }

  lastDistanceTime = millis();
  float distance = readDistanceCm();

  if (distance > 0 && distance <= detectionDistanceCm) {
    if (personIsNearby == false) {
      personIsNearby = true;
      displayMessage("USER DETECTED", "SCAN RFID");
      soundUserDetected();
    }
  } else if (distance > detectionDistanceCm + 10) {
    if (personIsNearby == true && waitingForPin == false) {
      personIsNearby = false;
      displayMessage("TRACEVAULT", "SYSTEM ARMED");
    }
  }
}

void handleUnlockedDoor() {
  char pressedKey = readKeypad();

  if (pressedKey != NO_KEY) {
    soundKeyPress();
  }

  if (pressedKey == 'D') {
    displayMessage("LOCKING...", "PLEASE WAIT");

    // Turn green off and start locking immediately.
    setStatusLights(false, false, false);
    moveServoFast(lockedAngle);
    Serial.println("SERVO LOCK COMMAND: 15");

    doorIsUnlocked = false;
    waitingForPin = false;
    enteredPin = "";

    // The servo is already moving while this sound plays.
    soundDoorLocked();
    flashYellowAfterLock();
    setStatusLights(true, false, false);
    Serial.println("GREEN LED OFF - YELLOW ARMED LED ON");

    personIsNearby = false;
    displayMessage("DOOR LOCKED", "SYSTEM ARMED");
    sendTelegramAlert("DOOR_LOCKED", 0, "LOCKED");
  }
}

void runFullSystem() {
  maintainWiFiConnection();

  if (lockdownActive == true) {
    handleLockdown();
    return;
  }

  sendSystemStartedAlertIfNeeded();

  if (doorIsUnlocked == true) {
    handleUnlockedDoor();
  } else if (waitingForPin == true) {
    handlePinEntry();
  } else {
    handleProximity();
    if (personIsNearby == true) {
      handleRFIDScanning();
    }
  }
}

// ==================================================
// ARDUINO SETUP AND LOOP
// ==================================================

void setup() {
  Serial.begin(115200);

  Serial.println(buildIdentifier);
  Serial.print("ESP32 reset reason: ");
  Serial.println((int)esp_reset_reason());

  // 1. Initialize the status LED outputs.
  pinMode(yellowLedPin, OUTPUT);
  pinMode(greenLedPin, OUTPUT);
  pinMode(redLedPin, OUTPUT);
  setStatusLights(true, false, false);

  // 2. Initialize the keypad pins.
  // GPIO 32, 34, 35 and 36 remain inputs at all times.
  // External 10 kOhm pull-ups are installed, so use plain INPUT mode.
  for (int column = 0; column < 4; column++) {
    pinMode(keypadReadPins[column], INPUT);
  }

  // Only GPIO 13, 14, 16 and 17 are driven by the keypad scanner.
  for (int row = 0; row < 4; row++) {
    pinMode(keypadDrivePins[row], OUTPUT);
    digitalWrite(keypadDrivePins[row], HIGH);
  }

  // 3-7. Attach the servo, lock the latch, hold for 600 ms,
  // detach, and remember the locked position.
  doorServo.setPeriodHertz(50);
  doorServo.attach(servoPin, 500, 2400);
  servoIsAttached = true;
  doorServo.write(lockedAngle);
  delay(600);
  doorServo.detach();

  servoIsAttached = false;
  servoDetachPending = false;
  currentServoAngle = lockedAngle;
  doorIsUnlocked = false;
  waitingForPin = false;
  enteredPin = "";

  // 8. Reserve LEDC channel 6 exclusively for the buzzer.
  bool buzzerAttached = ledcAttachChannel(
    buzzerPin,
    3200,
    8,
    buzzerLedcChannel
  );

  if (buzzerAttached == true) {
    Serial.println(
      "Buzzer LEDC channel 6 attached successfully."
    );
  } else {
    Serial.println(
      "Buzzer LEDC channel 6 attachment failed."
    );
  }

  stopTone();

  pinMode(ultrasonicTrigPin, OUTPUT);
  pinMode(ultrasonicEchoPin, INPUT);
  digitalWrite(ultrasonicTrigPin, LOW);

  // 9. Initialize the LCD, RFID reader and Wi-Fi.
  Wire.begin(lcdSdaPin, lcdSclPin);
  lcd.init();
  lcd.backlight();

  displayMessage("TRACEVAULT V3", "STARTING...");
  delay(2000);

  startupLedSelfTest();
  displayMessage("TRACEVAULT", "SYSTEM ARMED");

  SPI.begin(rfidSckPin, rfidMisoPin, rfidMosiPin, rfidSsPin);
  rfidReader.PCD_Init();
  delay(50);

  byte rfidVersion = rfidReader.PCD_ReadRegister(
    MFRC522::VersionReg
  );

  Serial.print("RC522 VERSION: 0x");
  if (rfidVersion < 0x10) {
    Serial.print("0");
  }
  Serial.println(rfidVersion, HEX);

  if (rfidVersion == 0x00 || rfidVersion == 0xFF) {
    Serial.println(
      "RFID COMMUNICATION ERROR - CHECK RC522 WIRING"
    );
  } else {
    rfidReader.PCD_AntennaOn();
    Serial.println("RFID READER READY");
  }

  connectToWiFi();
  stopTone();
  displayMessage("TRACEVAULT", "SYSTEM ARMED");
}

void loop() {
  // Never attach or detach the servo while the lockdown alarm is active.
  if (lockdownActive == false) {
    updateServoDetach();
  }

  runFullSystem();

  delay(5);
}
