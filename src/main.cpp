
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
// Layout:
// [0..3]  offTime (uint32_t)
// [4..7]  onTime  (uint32_t)
// [8..9]  screensaverDelaySec (uint16_t)
// (expand later if needed)
#define EEPROM_ADDR_SAVER (EEPROM_ADDR + sizeof(offTime) + sizeof(onTime))

// Stored in tenths of a second (value 100 = 10.0s)
uint32_t offTime = 100; // default 10.0s
uint32_t onTime  = 100; // default 10.0s
uint32_t timer = 0;
bool relayState = false;
// Application states
enum class AppState : uint8_t { RUN, EDIT, MENU_PROGRESS, MENU_SELECT, MENU_RESULT, MENU_SAVER_EDIT };
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
const unsigned long MENU_PROGRESS_START_MS = 500; // show bar after 0.5s
const unsigned long MENU_PROGRESS_FULL_MS  = 3000; // full at 3s
// Smooth scrolling variables for menu
static float menuScrollPos = 0.0f; // animated index position
static unsigned long lastScrollUpdate = 0;
const float MENU_SCROLL_SPEED = 5.0f; // units per second toward target

// Screensaver settings
static uint16_t screensaverDelaySec = 0; // 0 = OFF
static unsigned long lastUserActivity = 0; // millis of last button edge (wakes if blanked)
static bool displayBlanked = false;
static bool wakeConsume = false; // first press after wake is consumed
// Absolute target time (millis) when we should blank; 0 means disabled
static unsigned long nextBlankAt = 0;

// Screensaver edit digits (3 digits 000-999)
static uint8_t saverDigits[3];
static uint8_t saverEditIndex = 0;
static bool saverDigitsInit = false;
static uint16_t editingSaverValue = 0; // working value in seconds (multiple of 10)
static bool saverEditSessionInit = false;

// Menu item names (index 0 customized)
static const char* menuNames[MENU_COUNT] = {
  "Saver", "Menu2", "Menu3", "Menu4", "Menu5",
  "Menu6", "Menu7", "Menu8", "Menu9", "Menu10"
};

// Persistent digit buffers for edit mode
static uint8_t offDigits[DIGITS];
static uint8_t onDigits[DIGITS];
static bool editDigitsInitialized = false;

// Persistence throttle
static bool timersDirty = false;            // true when values changed in edit mode
static uint16_t lastSavedSaverDelay = 0xFFFF; // sentinel

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

// Enter screensaver config
void enterScreensaverEdit() {
  appState = AppState::MENU_SAVER_EDIT;
  saverEditIndex = 0; // unused now
  saverDigitsInit = false; // legacy
  saverEditSessionInit = false;
}

void finalizeScreensaverEdit() {
  // Assign from editing value (already multiple of 10 or 0)
  screensaverDelaySec = editingSaverValue;
  // Return to menu select instead of run so user can configure more items
  appState = AppState::MENU_SELECT;
  menuIndex = 0; // keep focus on first menu (screensaver)
  menuScrollPos = (float)menuIndex;
  lastScrollUpdate = millis();
  // Reset inactivity timer
  lastUserActivity = millis();
  // Recompute next blank target after changing delay
  if (screensaverDelaySec > 0) {
    nextBlankAt = lastUserActivity + (unsigned long)screensaverDelaySec * 1000UL;
  } else {
    nextBlankAt = 0;
  }
  // Persist only if changed
  if (screensaverDelaySec != lastSavedSaverDelay) {
    EEPROM.put(EEPROM_ADDR_SAVER, screensaverDelaySec);
    EEPROM.commit();
    lastSavedSaverDelay = screensaverDelaySec;
  }
}

// Button state
bool lastUp = false, lastDown = false, lastHash = false, lastStar = false;

void saveTimers() {
  EEPROM.put(EEPROM_ADDR, offTime);
  EEPROM.put(EEPROM_ADDR + sizeof(offTime), onTime);
  // Do not commit screensaver here (separate conditional save)
  EEPROM.commit();
}

void loadTimers() {
  EEPROM.get(EEPROM_ADDR, offTime);
  EEPROM.get(EEPROM_ADDR + sizeof(offTime), onTime);
  EEPROM.get(EEPROM_ADDR_SAVER, screensaverDelaySec);
  // Validate (values already stored as tenths)
  if (offTime < TIMER_MIN || offTime > TIMER_MAX) offTime = 100; // 10.0s fallback
  if (onTime  < TIMER_MIN || onTime  > TIMER_MAX) onTime  = 100; // 10.0s fallback
  if (screensaverDelaySec > 999) screensaverDelaySec = 0; // clamp
  lastSavedSaverDelay = screensaverDelaySec;
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
  lastUserActivity = millis(); // initialize inactivity baseline
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
  if (star && !hash) { // * exits edit immediately, save changes
    exitEditMode(true);
    return;
  }
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

// Handle screensaver delay edit (similar to timer digit editing but 3 digits only)
void handleSaverEdit(bool up, bool down, bool hash, bool star, bool upEdge, bool downEdge, bool hashEdge, unsigned long now) {
  if (!saverEditSessionInit) {
    editingSaverValue = screensaverDelaySec - (screensaverDelaySec % 10); // normalize
    saverEditSessionInit = true;
  }
  static unsigned long holdStart = 0;
  static unsigned long lastStep = 0;
  const unsigned long INITIAL_DELAY = 400;  // ms before repeat
  const unsigned long REPEAT_INTERVAL = 120; // ms per repeat
  bool upHeld = up;
  bool downHeld = down;
  bool actUp = upEdge;
  bool actDown = downEdge;

  if (upHeld || downHeld) {
    if (holdStart == 0) { holdStart = now; lastStep = now; }
    unsigned long heldFor = now - holdStart;
    if (heldFor > INITIAL_DELAY) {
      if (now - lastStep >= REPEAT_INTERVAL) {
        if (upHeld) actUp = true;
        if (downHeld) actDown = true;
        lastStep = now;
      } else {
        actUp = actDown = false;
      }
    } else {
      // Pre-delay: only initial edges allowed
      if (!upEdge) actUp = false;
      if (!downEdge) actDown = false;
    }
  } else {
    holdStart = 0;
  }

  bool changed = false;
  if (actUp) {
    if (editingSaverValue == 0) {
      editingSaverValue = 10; // first step from OFF goes to 10s
    } else if (editingSaverValue == 990) {
      editingSaverValue = 0; // rollover top -> OFF
    } else {
      editingSaverValue += 10; // normal increment
    }
    changed = true;
  }
  if (actDown) {
    if (editingSaverValue == 0) {
      editingSaverValue = 990; // rollover OFF -> top
    } else if (editingSaverValue == 10) {
      editingSaverValue = 0; // single step down to OFF
    } else {
      editingSaverValue -= 10; // normal decrement
    }
    changed = true;
  }
  if (changed) {
    lastUserActivity = now;
    if (screensaverDelaySec > 0) {
      // Keep existing schedule for current active delay; editing itself shouldn't alter blank deadline
      // (We only reschedule on actual user input or final save)
      nextBlankAt = lastUserActivity + (unsigned long)screensaverDelaySec * 1000UL;
    }
  }
  if (hashEdge) {
    finalizeScreensaverEdit();
  }
}

void render(bool blinkState, uint8_t editDigit, bool inEdit) {
  display.clearDisplay();
  display.setTextSize(2);
  AppState state = appState;

  // Hide timers entirely during menu selection/result full-screen modes
  bool showTimers = !(state == AppState::MENU_SELECT || state == AppState::MENU_RESULT || state == AppState::MENU_SAVER_EDIT);

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
      // Early menu indicator: show 'M' at (0,0) while # held but before progress bar start
      if (hashHoldStartGlobal != 0) {
        unsigned long held = millis() - hashHoldStartGlobal;
        if (held < MENU_PROGRESS_START_MS) {
          display.setTextSize(2);
          display.setTextColor(WHITE, BLACK);
          display.setCursor(0,0);
          display.print('M');
        }
      }
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
      // When full progress reached, show inverted centered blinking MENU label
      if (held >= MENU_PROGRESS_FULL_MS) {
        static bool menuFullBlink = false;
        static unsigned long lastBlinkFull = 0;
        if (millis() - lastBlinkFull > 400) { menuFullBlink = !menuFullBlink; lastBlinkFull = millis(); }
        if (menuFullBlink) {
          const char *txt = "MENU";
          int txtWidth = 4 * 12; // approximate 4 chars * 12px each at size 2
          int xTxt = barX + (barW - txtWidth) / 2;
          int yTxt = barY + 2;
          display.setTextColor(BLACK, WHITE);
          display.setCursor(xTxt, yTxt);
          display.print(txt);
        }
      }
      break; }
    case AppState::MENU_SELECT: {
      // Smooth scrolling list: show three adjacent items centered with interpolation
      display.clearDisplay();
      display.setTextSize(2);
      // Each item row height (approx) we treat as 24px spacing
      float centerY = 24.0f; // center row y position
      // Fractional offset from integer position
      float offset = menuScrollPos - floor(menuScrollPos);
      int baseIndex = (int)floor(menuScrollPos) % MENU_COUNT;
      if (baseIndex < 0) baseIndex += MENU_COUNT;

      // We draw items for rows -1, 0, +1 relative to baseIndex
      for (int rel = -1; rel <= 1; ++rel) {
        int idx = (baseIndex + rel + MENU_COUNT) % MENU_COUNT;
        float logicalRow = (float)rel - offset; // 0 is current target row
        float y = centerY + logicalRow * 24.0f; // vertical interpolation
        int yi = (int) y;
        // Determine if this is the selected (nearest) item
        bool isSelected = fabs(logicalRow) < 0.5f; // close to center
        const char* name = menuNames[idx];
        if (isSelected) {
          display.fillRect(0, yi, 128, 20, WHITE);
          display.setTextColor(BLACK, WHITE);
          display.setCursor(0, yi);
          display.print("> "); display.print(name);
        } else {
          display.setTextColor(WHITE, BLACK);
          display.setCursor(0, yi);
          display.print("  "); display.print(name);
        }
      }
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
    case AppState::MENU_SAVER_EDIT: {
      // Reuse timer digit style (similar spacing) for 3-digit seconds value
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0,0);
      display.print("Saver Delay s");
      display.setTextSize(2);
      int startX = 10;
      uint16_t val = editingSaverValue;
      if (val == 0) {
        // OFF display
        int boxW = 60; int boxH = 18;
        if (blinkState) {
          display.fillRect(startX,24,boxW,boxH,WHITE);
          display.setTextColor(BLACK,WHITE);
        } else {
          display.fillRect(startX,24,boxW,boxH,BLACK);
          display.setTextColor(WHITE,BLACK);
        }
        display.setCursor(startX+2,24);
        display.print("OFF");
      } else {
        char buf[6];
        snprintf(buf,sizeof(buf),"%u", val);
        int len = strlen(buf);
        int digitWidth = 11;
        int boxW = len * digitWidth + 6;
        if (blinkState) {
          display.fillRect(startX,24,boxW,18,WHITE);
          display.setTextColor(BLACK,WHITE);
        } else {
          display.fillRect(startX,24,boxW,18,BLACK);
          display.setTextColor(WHITE,BLACK);
        }
        display.setCursor(startX+2,24);
        display.print(buf);
        display.setTextColor(WHITE,BLACK);
        display.setCursor(startX + boxW + 2,24);
        display.print('s');
      }
      // OFF indicator
      display.setTextSize(1);
      display.setTextColor(WHITE,BLACK);
      display.setCursor(50,46);
      if (val == 0) display.print("OFF"); else { display.print("    "); }
      display.setCursor(0,56);
      display.print("#=Save *=Cancel");
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
  if ((appState == AppState::EDIT || appState == AppState::MENU_SAVER_EDIT) && now - lastBlink > 350) {
    blinkState = !blinkState;
    lastBlink = now;
  }

  // Track user activity on any button activity (press or hold) while display on
  if (!displayBlanked && (up || down || hash || star)) {
    lastUserActivity = now;
    if (screensaverDelaySec > 0) {
      nextBlankAt = lastUserActivity + (unsigned long)screensaverDelaySec * 1000UL;
    }
  }

  // Screensaver blanking logic
  if (!displayBlanked && nextBlankAt != 0 && (long)(now - nextBlankAt) >= 0) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayBlanked = true;
  }
  if (displayBlanked) {
    if (up || down || hash || star) {
      // Wake on any press
      display.ssd1306_command(SSD1306_DISPLAYON);
      displayBlanked = false;
      wakeConsume = true;
      lastUserActivity = now;
      if (screensaverDelaySec > 0) {
        nextBlankAt = lastUserActivity + (unsigned long)screensaverDelaySec * 1000UL;
      }
      // Consume this frame's edges
      upEdge = downEdge = hashEdge = starEdge = false;
    } else {
      // While blanked and no wake yet, do nothing further
      delay(10);
      return;
    }
  } else if (wakeConsume) {
    // Wait for all keys released before resuming normal processing
    if (!up && !down && !hash && !star) {
      wakeConsume = false;
    } else {
      // Consume inputs until release
      upEdge = downEdge = hashEdge = starEdge = false;
      delay(10);
      return;
    }
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
    // Progress bar phase: must hold until full (>= MENU_PROGRESS_FULL_MS) to unlock menu
    if (hash) {
      // Still holding; nothing else required here
    } else {
      unsigned long held = now - hashHoldStartGlobal;
      if (held >= MENU_PROGRESS_FULL_MS) {
        // Full hold achieved -> enter menu select on release
        appState = AppState::MENU_SELECT;
        menuIndex = 0;
        menuScrollPos = (float)menuIndex;
        lastScrollUpdate = now;
      } else {
        // Not fully held -> cancel
        appState = AppState::RUN;
      }
      hashHoldStartGlobal = 0;
    }
  } else if (appState == AppState::MENU_SELECT) {
    // Navigate menu
    if (upEdge) { menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT; }
    if (downEdge) { menuIndex = (menuIndex + 1) % MENU_COUNT; }
    if (starEdge) { // * exits menu back to RUN
      appState = AppState::RUN;
      lastUserActivity = now;
    } else if (hashEdge) {
      selectedMenu = menuIndex;
      if (selectedMenu == 0) { // Menu 1: screensaver config
        enterScreensaverEdit();
      } else {
        appState = AppState::MENU_RESULT;
        menuResultStart = now;
      }
    }
    // Animate scroll position toward menuIndex
    unsigned long dtMs = now - lastScrollUpdate;
    if (dtMs > 0) {
      float dt = dtMs / 1000.0f;
      float target = (float)menuIndex;
      float diff = target - menuScrollPos;
      // Wrap shortest path for cyclic menu (choose direction across boundary if shorter)
      if (diff > (MENU_COUNT / 2)) diff -= MENU_COUNT;
      else if (diff < -(MENU_COUNT / 2)) diff += MENU_COUNT;
      float step = MENU_SCROLL_SPEED * dt;
      if (fabs(diff) <= step) {
        menuScrollPos = target;
      } else {
        menuScrollPos += (diff > 0 ? step : -step);
        // Normalize into range 0..MENU_COUNT
        if (menuScrollPos < 0) menuScrollPos += MENU_COUNT;
        if (menuScrollPos >= MENU_COUNT) menuScrollPos -= MENU_COUNT;
      }
      lastScrollUpdate = now;
    }
  } else if (appState == AppState::MENU_RESULT) {
    if (now - menuResultStart >= 5000) {
      appState = AppState::RUN;
    }
  } else if (appState == AppState::MENU_SAVER_EDIT) {
    if (starEdge) { // cancel edit -> back to menu select without saving changes
      // Re-load current persisted value digits if user returns later
      saverDigitsInit = false;
      appState = AppState::MENU_SELECT;
      lastUserActivity = now;
      if (screensaverDelaySec > 0) {
        nextBlankAt = lastUserActivity + (unsigned long)screensaverDelaySec * 1000UL;
      }
    } else {
      handleSaverEdit(up, down, hash, star, upEdge, downEdge, hashEdge, now);
    }
  }

  // Periodic deferred save outside edit mode
  // Removed deferred background save: EEPROM only written on edit exit if changed
  digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

  // Display
  render(blinkState, editDigit, appState == AppState::EDIT);
  delay(10);
}
