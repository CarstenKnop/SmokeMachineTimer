#pragma once
#include <Arduino.h>
#include <cmath>
#include "Defaults.h"
#include "Buttons.h"
#include "Config.h"
#include "Screensaver.h"
#include "MenuItems/Help.h"
#include "MenuItems/Saver.h"

class MenuSystem {
public:
  enum class State : uint8_t { INACTIVE, PROGRESS, SELECT, RESULT, SAVER_EDIT, HELP };

  void begin();

  State getState() const { return state; }
  bool showMenuHint() const { return menuHint; }
  int getMenuIndex() const { return menuIndex; }
  float getScrollPos() const { return menuScrollPos; }
  int getSelectedMenu() const { return selectedMenu; }
  unsigned long getMenuResultStart() const { return menuResultStart; }
  uint16_t getEditingSaverValue() const { return saverEdit.value(); }

  bool inSaverEdit() const { return state == State::SAVER_EDIT; }
  bool inSelect() const { return state == State::SELECT; }
  bool inProgress() const { return state == State::PROGRESS; }
  bool inResult() const { return state == State::RESULT; }

  void startProgress(unsigned long now);

  // Deferred start: invoked only after initial threshold so progress bar begins empty
  void startProgressDeferred(unsigned long now);

  void updateProgress(bool hashHeld, bool hashReleased, unsigned long now);

  void cancel();
  void setMenuHint(bool v) { menuHint = v; if (!v && state==State::INACTIVE) {/*no-op*/} }

  void navigate(const ButtonState& bs, unsigned long now);

  // Returns true if consumed selection
  bool handleSelect(const ButtonState& bs, unsigned long now, Config& config);

  void updateResult(unsigned long now);

  // Saver edit handling (rollover 0<->990; increments of 10)
  // Deprecated internal saver edit logic replaced by SaverMenu::EditController.

  void animateScroll(unsigned long now);

  float progressFraction(unsigned long now) const;

  bool progressFull(unsigned long now) const;

  // Dynamic menu: currently two items (Saver, Help); can expand later
  int getMenuCount() const { return 2; }
  const char* getMenuName(int idx) const { return (idx==0)? SaverMenu::NAME : (idx==1? HelpContent::NAME : ""); }

  // Help support
  bool inHelp() const { return state == State::HELP; }
  int getHelpScroll() const { return helpCtrl.currentStart(); }
  float getHelpScrollPos() const { return helpCtrl.scrollPos(); }
  int getHelpLines() const; // now in Help module
  const char* getHelpLine(int i) const; // forwarded to Help module
  void enterHelp(); // delegates to help controller
  void processInput(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver); // unified button handling
  void updateHelpAnimation(unsigned long now); // delegates to help controller

private:
  void enterSelect(unsigned long now);
  void beginSaverEdit(uint16_t current); // uses saver controller
  void handleHelp(const ButtonState& bs); // uses help controller

  State state = State::INACTIVE;
  unsigned long hashHoldStart = 0;
  int menuIndex = 0;
  int selectedMenu = -1;
  unsigned long menuResultStart = 0;
  float menuScrollPos = 0.0f;
  unsigned long lastScrollUpdate = 0;
  // Saver controller
  SaverMenu::EditController saverEdit;
  bool menuHint=false; // show 'M' immediately after hash press
  // Help controller
  HelpContent::Controller helpCtrl;
  // Help data moved to HelpContent namespace (MenuItems/Help.*)
};
