
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
  // value in tenths: integer part = value/10, fractional = value%10
  char buf[8];
  unsigned long integerPart = value / 10;
  unsigned long frac = value % 10;
  snprintf(buf, sizeof(buf), "%04lu%01lu", integerPart, frac);
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

// Stored in tenths of a second (value 100 = 10.0s)
uint32_t offTime = 100; // default 10.0s
uint32_t onTime  = 100; // default 10.0s
uint32_t timer = 0;
bool relayState = false;
// Application states
enum class AppState : uint8_t { RUN, EDIT, MENU_PROGRESS, MENU_SELECT, MENU_RESULT };
AppState appState = AppState::RUN;
uint8_t editDigit = 0; // 0-4: Off digits, 5-9: On digits (fractional at index 4 & 9)

// Forward state helpers
void enterEditMode();
void exitEditMode(bool forceSave);
void render(bool blinkState, uint8_t editDigit, bool inEdit);
void handleEditState(bool up, bool down, bool hash, bool star, bool upEdge, bool downEdge, bool hashEdge, unsigned long now);

// Menu / progress tracking
static unsigned long hashHoldStartGlobal = 0; // tracks # hold in RUN
static bool hashHoldActive = false;
static int menuIndex = 0; // 0-9
static int selectedMenu = -1;
static unsigned long menuResultStart = 0;
const int MENU_COUNT = 10;
const unsigned long MENU_PROGRESS_START_MS = 1000; // show bar after 1s
const unsigned long MENU_PROGRESS_FULL_MS  = 5000; // full at 5s

// Persistent digit buffers for edit mode
static uint8_t offDigits[DIGITS];
static uint8_t onDigits[DIGITS];
static bool editDigitsInitialized = false;

// Persistence throttle
static bool timersDirty = false;            // true when values changed in edit mode

// Forward declarations for persistence helpers
void saveTimers();
void loadTimers();

void markTimersDirty() { timersDirty = true; }

// State transition helpers
void enterEditMode() {
  appState = AppState::EDIT;
  editDigit = 0;
  editDigitsInitialized = false; // force buffer load
}

void exitEditMode(bool forceSave) {
  if (appState == AppState::EDIT) {
    appState = AppState::RUN;
    editDigitsInitialized = false;
    if (forceSave && timersDirty) {
      saveTimers();
      timersDirty = false;
    }
  }
}

// Button state
bool lastUp = false, lastDown = false, lastHash = false, lastStar = false;

void saveTimers() {
  EEPROM.put(EEPROM_ADDR, offTime);
  EEPROM.put(EEPROM_ADDR + sizeof(offTime), onTime);
  EEPROM.commit();
}

void loadTimers() {
  EEPROM.get(EEPROM_ADDR, offTime);
  EEPROM.get(EEPROM_ADDR + sizeof(offTime), onTime);
  // Validate (values already stored as tenths)
  if (offTime < TIMER_MIN || offTime > TIMER_MAX) offTime = 100; // 10.0s fallback
  if (onTime  < TIMER_MIN || onTime  > TIMER_MAX) onTime  = 100; // 10.0s fallback
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

// Edit state handler extracted from loop for clarity
void handleEditState(bool up, bool down, bool hash, bool star, bool upEdge, bool downEdge, bool hashEdge, unsigned long now) {
  static bool requireRelease = false;
  static unsigned long lastUpDown = 0;
  static unsigned long hashHoldStart = 0;
  static bool hashWasHeld = false;
  static bool firstCycle = true;
  static unsigned long holdStart = 0; // for refined auto-repeat
  const unsigned long INITIAL_DELAY = 400;
  const unsigned long FAST_INTERVAL  = 120;

  // Track # hold
  if (hash) {
    if (hashHoldStart == 0) hashHoldStart = now;
  } else {
    hashHoldStart = 0;
    hashWasHeld = false;
  }

  // Initialize digit buffers if needed
  if (!editDigitsInitialized) {
    uint32_t tempOffInt = offTime / 10;
    uint8_t  tempOffFrac = offTime % 10;
    uint32_t tempOnInt  = onTime / 10;
    uint8_t  tempOnFrac = onTime % 10;
    offDigits[DIGITS - 1] = tempOffFrac;
    onDigits [DIGITS - 1] = tempOnFrac;
    for (int i = DIGITS - 2; i >= 0; --i) { offDigits[i] = tempOffInt % 10; tempOffInt /= 10; }
    for (int i = DIGITS - 2; i >= 0; --i) { onDigits[i]  = tempOnInt  % 10; tempOnInt  /= 10; }
    editDigitsInitialized = true;
  }

  bool upHeld = up;
  bool downHeld = down;
  bool actUp = upEdge;
  bool actDown = downEdge;

  if (firstCycle) {
    requireRelease = true; // must release both buttons once after entering
    firstCycle = false;
  }
  if (requireRelease) {
    if (!upHeld && !downHeld) {
      requireRelease = false;
      holdStart = 0;
    }
    actUp = actDown = false;
  } else {
    // Refined auto-repeat: initial longer delay, then faster repeat
    if (upHeld || downHeld) {
      if (holdStart == 0) holdStart = now; // start hold timer
      unsigned long heldFor = now - holdStart;
      if (heldFor > INITIAL_DELAY) {
        if (now - lastUpDown > FAST_INTERVAL) {
          if (upHeld) actUp = true;
          if (downHeld) actDown = true;
          lastUpDown = now;
        } else {
          actUp = actDown = false; // wait until interval passes
        }
      } else {
        // During initial delay, only allow the edge event already captured
        if (!upEdge) actUp = false;
        if (!downEdge) actDown = false;
      }
    } else {
      holdStart = 0; // reset when released
    }
  }

  uint8_t *digits = (editDigit < DIGITS) ? offDigits : onDigits;
  uint8_t digit = editDigit % DIGITS;
  uint8_t originalDigitVal = digits[digit];
  bool changed = false;
  if (actUp)  { digits[digit] = (digits[digit] + 1) % 10; changed = true; }
  if (actDown){ digits[digit] = (digits[digit] + 9) % 10; changed = true; }
  if (changed) {
    uint32_t newVal = 0;
    for (int i = 0; i < DIGITS; ++i) newVal = newVal * 10 + digits[i];
    if (newVal < TIMER_MIN || newVal > TIMER_MAX) {
      digits[digit] = originalDigitVal; // revert this digit only
    } else {
      uint32_t *editVal = (editDigit < DIGITS) ? &offTime : &onTime;
      if (*editVal != newVal) {
        *editVal = newVal;
        markTimersDirty();
      }
    }
  }

  // Advance / exit logic
  if (hashEdge) {
    editDigit++;
    if (editDigit >= DIGITS * 2) {
      exitEditMode(true); // saves only if values changed
      firstCycle = true;
      return;
    }
    requireRelease = true;
  } else if (hash && !hashWasHeld && hashHoldStart && (now - hashHoldStart >= 2000)) {
    hashWasHeld = true;
    exitEditMode(true); // saves only if values changed
    firstCycle = true;
  }
}

void render(bool blinkState, uint8_t editDigit, bool inEdit) {
  display.clearDisplay();
  display.setTextSize(2);
  AppState state = appState;

  // Hide timers entirely during menu selection/result full-screen modes
  bool showTimers = !(state == AppState::MENU_SELECT || state == AppState::MENU_RESULT);

  if (showTimers) {
    if (inEdit && timersDirty) {
      display.setCursor(0, 0);
      display.setTextColor(WHITE, BLACK);
      display.print('!');
    } else {
      display.fillRect(0, 0, 12, 16, BLACK);
    }
    printTimerValue(offTime, 0, "OFF", (inEdit && editDigit < DIGITS) ? editDigit : 255, inEdit && editDigit < DIGITS, blinkState);
    printTimerValue(onTime, 24, "ON", (inEdit && editDigit >= DIGITS) ? (editDigit - DIGITS) : 255, inEdit && editDigit >= DIGITS, blinkState);
  }

  // Third line content depends on state
  display.setCursor(0, 48);
  display.setTextSize(2);
  switch (state) {
    case AppState::EDIT:
      display.print("EDIT MODE");
      break;
    case AppState::RUN:
      if (relayState) display.print("*   "); else display.print("    ");
      printTimerValue(timer, 48, "TIME");
      break;
    case AppState::MENU_PROGRESS: {
      // Draw progress bar (0..100%) full width minus margins
      unsigned long held = millis() - hashHoldStartGlobal;
      float prog = 0.0f;
      if (held > MENU_PROGRESS_START_MS) {
        unsigned long span = (held - MENU_PROGRESS_START_MS);
        unsigned long total = MENU_PROGRESS_FULL_MS - MENU_PROGRESS_START_MS;
        if (span > total) span = total;
        prog = (float)span / (float)total;
      }
      int barX = 0, barY = 48, barW = 128, barH = 16;
      display.drawRect(barX, barY, barW, barH, WHITE);
      int fillW = (int)((barW - 2) * prog);
      if (fillW > 0) display.fillRect(barX + 1, barY + 1, fillW, barH - 2, WHITE);
      break; }
    case AppState::MENU_SELECT: {
      // Full screen 3-line menu, size 2 font
      display.clearDisplay();
      display.setTextSize(2);
      // Determine indices for previous, current, next (with wrap)
      int prev = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
      int curr = menuIndex;
      int next = (menuIndex + 1) % MENU_COUNT;
      // Line heights at y = 0, 24, 48 (already using 24px spacing consistent with earlier layout)
      // Previous
      display.setCursor(0,0);
      display.setTextColor(WHITE, BLACK);
      display.print("  M"); display.print(prev + 1);
      // Current highlighted (invert)
      display.fillRect(0,24,128,20,WHITE);
      display.setCursor(0,24);
      display.setTextColor(BLACK, WHITE);
      display.print("> M"); display.print(curr + 1);
      // Next
      display.setTextColor(WHITE, BLACK);
      display.setCursor(0,48);
      display.print("  M"); display.print(next + 1);
      break; }
    case AppState::MENU_RESULT: {
      display.clearDisplay();
      display.setTextSize(2);
      display.setCursor(0,0);
      display.print("Selected");
      display.setCursor(0,24);
      display.print("Menu ");
      display.print(selectedMenu + 1);
      // Leave third line blank or could show countdown
      break; }
  }
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

  // Track # button hold for exiting edit mode
  static unsigned long hashHoldStart = 0;
  static bool hashWasHeld = false;
  if (hash) {
    if (hashHoldStart == 0) hashHoldStart = millis();
  } else {
    hashHoldStart = 0;
    hashWasHeld = false;
  }

  static bool blinkState = false;
  static unsigned long lastBlink = 0;
  unsigned long now = millis();

  // Blinking for edit digit
  if (appState == AppState::EDIT && now - lastBlink > 350) {
    blinkState = !blinkState;
    lastBlink = now;
  }

  if (appState == AppState::EDIT) {
    handleEditState(up, down, hash, star, upEdge, downEdge, hashEdge, now);
  } else if (appState == AppState::RUN) {
    // Run mode logic
    if (hashEdge) { relayState = false; timer = 0; }
    if (starEdge) { relayState = !relayState; timer = 0; }
    if (relayState) {
      if (timer < onTime) timer++; else { relayState = false; timer = 0; }
    } else {
      if (timer < offTime) timer++; else { relayState = true; timer = 0; }
    }
    if (upEdge || downEdge) { enterEditMode(); }
    // Track # hold for menu progress
    if (hash) {
      if (hashHoldStartGlobal == 0) hashHoldStartGlobal = now;
      unsigned long held = now - hashHoldStartGlobal;
      if (held >= MENU_PROGRESS_START_MS) {
        appState = AppState::MENU_PROGRESS;
      }
    } else {
      hashHoldStartGlobal = 0;
    }
  } else if (appState == AppState::MENU_PROGRESS) {
    // Continue measuring hold; if released before reaching threshold, revert to RUN
    if (hash) {
      unsigned long held = now - hashHoldStartGlobal;
      if (held >= MENU_PROGRESS_FULL_MS) {
        // Reached full progress but still holding; stay until release
      }
    } else {
      // Released -> enter menu select if at least started progress phase
      unsigned long held = now - hashHoldStartGlobal;
      if (held >= MENU_PROGRESS_START_MS) {
        appState = AppState::MENU_SELECT;
        menuIndex = 0;
      } else {
        appState = AppState::RUN; // cancelled
      }
      hashHoldStartGlobal = 0;
    }
  } else if (appState == AppState::MENU_SELECT) {
    // Navigate menu
    if (upEdge) { menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT; }
    if (downEdge) { menuIndex = (menuIndex + 1) % MENU_COUNT; }
    if (hashEdge) {
      selectedMenu = menuIndex;
      appState = AppState::MENU_RESULT;
      menuResultStart = now;
    }
  } else if (appState == AppState::MENU_RESULT) {
    if (now - menuResultStart >= 5000) {
      appState = AppState::RUN;
    }
  }

  // Periodic deferred save outside edit mode
  // Removed deferred background save: EEPROM only written on edit exit if changed
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

  // Display
  render(blinkState, editDigit, appState == AppState::EDIT);
  delay(10);
}
