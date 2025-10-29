// DisplayManager.cpp
// Handles OLED rendering, UI, and battery indicator.
#include <Wire.h>
#include "Pins.h"
#include "DisplayManager.h"
#include "ui/ButtonInput.h"
#include "debug/DebugMetrics.h"
#include "Defaults.h"
#include "comm/CommManager.h"

DisplayManager::DisplayManager() : display(128, 64, &Wire, -1) {}

void DisplayManager::splash() {
	display.clearDisplay();
	display.setTextSize(1);
	display.setTextColor(SSD1306_WHITE);
	display.setCursor(0, 0);
	display.println("FogMachine Remote");
	display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
	display.setCursor(0, 20);
	display.println(Defaults::VERSION());
	display.display();
}

void DisplayManager::begin() {
	// Try primary I2C pins first, then fallback
	selectedSda = OLED_SDA_PIN;
	selectedScl = OLED_SCL_PIN;
	Wire.begin(selectedSda, selectedScl);
	bool ok = display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
	if (!ok) {
		// Try alternate pins
		selectedSda = OLED_SDA_PIN_ALT;
		selectedScl = OLED_SCL_PIN_ALT;
		Wire.begin(selectedSda, selectedScl);
		ok = display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
	}
	if (!ok) {
		initFailed = true;
		inited = false;
		return;
	}
	inited = true;
	initFailed = false;
	display.clearDisplay();
	display.setRotation(Defaults::OLED_ROTATION);
	if (!skipSplash) splash();
	// Start unblanked
	isBlanked = false;
	lastWakeMs = millis();
}

void DisplayManager::setPreventBlanking(bool value) {
	if (preventBlanking == value) {
		return;
	}
	preventBlanking = value;
	if (preventBlanking) {
		lastWakeMs = millis();
		if (isBlanked) {
			isBlanked = false;
			display.ssd1306_command(SSD1306_DISPLAYON);
		}
	}
}

void DisplayManager::blankNow() {
	if (!inited) {
		return;
	}
	preventBlanking = false;
	if (!isBlanked) {
		isBlanked = true;
		display.ssd1306_command(SSD1306_DISPLAYOFF);
	}
}

void DisplayManager::render(const DeviceManager& deviceMgr, const BatteryMonitor& battery, const MenuSystem& menu, const ButtonInput& buttons) {
	if (!inited) {
		// If init failed, draw a minimal error panel repeatedly
		if (initFailed) {
			drawErrorScreen();
		}
		return;
	}

	// Apply contrast/brightness from menu (clamped in menu)
	display.ssd1306_command(SSD1306_SETCONTRAST);
	display.ssd1306_command(menu.getAppliedOledBrightness());

	// Wake on any interaction
	bool anyActive = buttons.upHeld() || buttons.downHeld() || buttons.hashHeld() || buttons.starHeld() ||
					 buttons.upPressed() || buttons.downPressed() || buttons.hashPressed() || buttons.starPressed();
	bool keepAwake = anyActive || preventBlanking;
	if (keepAwake) {
		lastWakeMs = millis();
		if (isBlanked) { isBlanked = false; display.ssd1306_command(SSD1306_DISPLAYON); }
	}
	// Handle Auto Off based on applied seconds
	int blankSecs = menu.getAppliedBlankingSeconds();
	if (preventBlanking) {
		blankSecs = 0;
	}
	if (blankSecs > 0) {
		unsigned long now = millis();
		if (!isBlanked && (now - lastWakeMs) >= (unsigned long)blankSecs * 1000UL) {
			isBlanked = true;
			display.ssd1306_command(SSD1306_DISPLAYOFF);
		}
	} else {
		// Blanking disabled
		if (isBlanked) { isBlanked = false; display.ssd1306_command(SSD1306_DISPLAYON); }
	}
	if (isBlanked) {
		// Nothing to draw while blanked
		return;
	}

	// Draw frame
	unsigned long tStart = millis();
	display.clearDisplay();

	// Route to menu or main
	if (menu.isInMenu() || menu.getMode() != MenuSystem::Mode::ROOT) {
		drawMenu(menu, deviceMgr, battery);
	} else {
		// Battery indicator (top-left) only on main screen
	drawBatteryIndicator(battery.getPercent());
	drawMainScreen(deviceMgr, battery, menu);
		// Show hold progress bar for '#' (menu entry visual)
		unsigned long hold = buttons.hashHoldDuration();
		if (hold >= Defaults::MENU_PROGRESS_START_MS) {
			unsigned long prog = hold - Defaults::MENU_PROGRESS_START_MS;
			unsigned long full = (Defaults::BUTTON_LONG_PRESS_MS > Defaults::MENU_PROGRESS_START_MS)
								? (Defaults::BUTTON_LONG_PRESS_MS - Defaults::MENU_PROGRESS_START_MS)
								: Defaults::BUTTON_LONG_PRESS_MS;
			drawProgressBar(prog, full ? full : Defaults::BUTTON_LONG_PRESS_MS);
		}
	}
	// Flush to OLED
	unsigned long tFlushStartUs = micros();
	display.display();
	unsigned long flushMs = (micros() - tFlushStartUs) / 1000UL;
	unsigned long prepMs = (millis() - tStart);
	unsigned long totalMs = prepMs + flushMs;
	DebugMetrics::instance().recordDisplayFrame((uint32_t)prepMs, (uint32_t)flushMs, (uint32_t)totalMs);
}


