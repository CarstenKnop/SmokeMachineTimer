
/*
  SmokeMachineTimer for Seeed XIAO ESP32-C3
  Features:
    - On/Off timer (0000.1...9999.9 sec) for relay output
    - Editable On/Off times (digit-by-digit, inverted digit in edit mode)
    - 4 buttons: Up, Down, # (edit/next/reset), * (manual output)
    - SSD1315 128x64 OLED (I2C)
    - EEPROM save/load for timer values
    - Pin mapping:
        GPIO2  (D0)  -> Relay Output
        GPIO3  (D1)  -> Up button
        GPIO4  (D2)  -> Down button
        GPIO9  (D9)  -> # button
        GPIO10 (D10) -> * button
        GPIO6  (D4)  -> SDA (OLED)
        GPIO7  (D5)  -> SCL (OLED)
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// Pin definitions
#define RELAY_PIN   2
#define BTN_UP      3
#define BTN_DOWN    4
#define BTN_HASH    9
#define BTN_STAR    10
#define OLED_SDA    6
#define OLED_SCL    7

// Timer settings
#define DIGITS 5
#define TIMER_MIN 1         // 0000.1s
#define TIMER_MAX 99999     // 9999.9s

// OLED display size
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Helper to print timer value in 0000.0s format at a given y position, with a small label
void printTimerValue(uint32_t value, int y, const char* label, int editDigit = 255, bool editMode = false, bool blinkState = false, int startX = 26) {
  char buf[8];
  unsigned long t = value / 1;
  snprintf(buf, sizeof(buf), "%04lu%01lu", t / 10, t % 10);
  display.setTextSize(2);
  int digitWidth = 11;
  int x = startX;
  for (uint8_t i = 0; i < DIGITS; i++) {
    bool inv = false;
    // Blinking inverted digit if in edit mode and this is the digit being edited
    if (editMode && editDigit == i && blinkState) inv = true;
    if (inv) {
      display.setTextColor(BLACK, WHITE);
      display.fillRect(x, y, digitWidth, 16, WHITE);
    } else {
      display.setTextColor(WHITE, BLACK);
      display.fillRect(x, y, digitWidth, 16, BLACK);
    }
    display.setCursor(x, y);
    display.print(buf[i]);
    if (i == DIGITS - 2) {
      display.print('.');
      x += digitWidth;
    }
    x += digitWidth;
  }

  // Calculate x position for label after the digits
  int labelX = startX + digitWidth * (DIGITS + 1) + 10;
  display.setTextSize(1);
  display.setTextColor(WHITE, BLACK);
  display.setCursor(labelX, y + 7);
  display.print(label);
  display.setTextSize(2);
}

// Timer settings
#define EEPROM_ADDR 0

uint32_t offTime = 1000; // ms, default 10.00s
uint32_t onTime = 1000;  // ms, default 10.00s
uint32_t timer = 0;
bool relayState = false;
bool editMode = false;
uint8_t editDigit = 0; // 0-5: Off, 6-11: On

// Button state
// Globals for edit mode and blinking for printTimerValue
bool editModeGlobal = false;
uint8_t editDigitGlobal = 255;
bool blinkStateGlobal = false;
bool lastUp = false, lastDown = false, lastHash = false, lastStar = false;

void saveTimers() {
  EEPROM.put(EEPROM_ADDR, offTime);
  EEPROM.put(EEPROM_ADDR + sizeof(offTime), onTime);
  EEPROM.commit();
}

void loadTimers() {
  EEPROM.get(EEPROM_ADDR, offTime);
  EEPROM.get(EEPROM_ADDR + sizeof(offTime), onTime);
  if (offTime < TIMER_MIN || offTime > TIMER_MAX * 10) offTime = 1000;
  if (onTime < TIMER_MIN || onTime > TIMER_MAX * 10) onTime = 1000;
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_HASH, INPUT_PULLUP);
  pinMode(BTN_STAR, INPUT_PULLUP);
  Wire.begin(OLED_SDA, OLED_SCL);
  EEPROM.begin(32);
  loadTimers();
  delay(100); // Allow power to stabilize
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // Display init failed, halt or blink LED
    while (1) {
      // Optionally blink relay LED to indicate error
      digitalWrite(RELAY_PIN, HIGH);
      delay(200);
      digitalWrite(RELAY_PIN, LOW);
      delay(200);
    }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print("Smooke Machine Timer v1.0");
  display.display();
  delay(1000);
  display.clearDisplay();
  display.display();
}

void loop() {
  // Read buttons (active low)
  bool up = !digitalRead(BTN_UP);
  bool down = !digitalRead(BTN_DOWN);
  bool hash = !digitalRead(BTN_HASH);
  bool star = !digitalRead(BTN_STAR);

  // Button edge detection
  bool upEdge = up && !lastUp;
  bool downEdge = down && !lastDown;
  bool hashEdge = hash && !lastHash;
  bool starEdge = star && !lastStar;
  lastUp = up; lastDown = down; lastHash = hash; lastStar = star;

  static unsigned long lastEditStep = 0;
  static bool blinkState = false;
  static unsigned long lastBlink = 0;
  unsigned long now = millis();

  // Blinking for edit digit
  if (editMode && now - lastBlink > 350) {
    blinkState = !blinkState;
    lastBlink = now;
  }

  // Hold-to-repeat for up/down in edit mode
  static unsigned long lastUpDown = 0;
  bool upHeld = up && lastUp;
  bool downHeld = down && lastDown;
  bool doUp = upEdge;
  bool doDown = downEdge;
  if (editMode && (upHeld || downHeld) && now - lastUpDown > 180) {
    if (upHeld) doUp = true;
    if (downHeld) doDown = true;
    lastUpDown = now;
  }

  if (editMode) {
    // Editing mode
    uint32_t *editVal = (editDigit < DIGITS) ? &offTime : &onTime;
    uint8_t digit = editDigit % DIGITS;
    uint32_t pow10 = pow(10, DIGITS - digit - 1);
    uint32_t val = *editVal / 10;
    if (doUp) {
      val += pow10;
      if (val > TIMER_MAX) val = TIMER_MIN;
      *editVal = val * 10;
    }
    if (doDown) {
      if (val > TIMER_MIN) val -= pow10;
      else val = TIMER_MAX;
      *editVal = val * 10;
    }
    if (hashEdge) {
      editDigit++;
      if (editDigit >= DIGITS * 2) {
        editMode = false;
        saveTimers();
      }
    }
  } else {
    // Run mode
    if (hashEdge) {
      // Reset timer
      relayState = false;
      timer = 0;
    }
    if (starEdge) {
      // Toggle output and shift timer to start of ON or OFF
      relayState = !relayState;
      timer = 0;
    }
    // Timer logic
    if (relayState) {
      if (timer < onTime) {
        timer++;
      } else {
        relayState = false;
        timer = 0;
      }
    } else {
      if (timer < offTime) {
        timer++;
      } else {
        relayState = true;
        timer = 0;
      }
    }
    if (upEdge || downEdge) {
      editMode = true;
      editDigit = 0;
    }
  }
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

  // Display
  display.clearDisplay();
  display.setTextSize(2);
  
  // T1 row (first line, label OFF)
  printTimerValue(offTime, 0, "OFF", (editMode && editDigit < DIGITS) ? editDigit : 255, editMode && editDigit < DIGITS, blinkState);

  // T2 row (second line, label ON)
  printTimerValue(onTime, 24, "ON", (editMode && editDigit >= DIGITS) ? (editDigit - DIGITS) : 255, editMode && editDigit >= DIGITS, blinkState);

  // Status row (third line, size 2)
  display.setCursor(0, 48);
  if (editMode) {
    display.print("EDIT MODE");
  } else {
    // ON/OFF indicator
    if (relayState) {
      display.print("*   ");
    } else {
      display.print("    ");
    }
    printTimerValue(timer, 48, "TIME");
  }
  display.display();
  delay(10);
}
