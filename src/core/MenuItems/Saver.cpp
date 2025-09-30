#include "Saver.h"
#include "../Buttons.h"
#include "../Config.h"
#include "../Screensaver.h"

namespace SaverMenu {
	void EditController::begin(uint16_t current) {
		editingValue = current - (current % 10);
		resetRepeat();
		ignoreFirstHashEdge = true;
	}

	void EditController::repeatHandler(const ButtonState& bs, unsigned long now) {
		if (!repeatInited) { repeatInited=true; holdStart=0; }
		actUp = bs.upEdge; actDown = bs.downEdge;
		bool heldAny = bs.up || bs.down;
		if (heldAny) {
				if (holdStart==0) { holdStart=now; lastStep=now; }
				unsigned long held = now - holdStart;
				if (held > Defaults::EDIT_INITIAL_DELAY_MS) {
						if (now - lastStep >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
								if (bs.up) actUp = true; if (bs.down) actDown = true; lastStep = now;
						} else { actUp = actDown = false; }
				} else {
						if (!bs.upEdge) actUp=false; if (!bs.downEdge) actDown=false;
				}
		} else {
				holdStart=0;
		}
	}

	void EditController::resetRepeat() { holdStart=0; lastStep=0; repeatInited=false; actUp=actDown=false; }

	bool EditController::handle(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver) {
		ButtonState local = bs;
		if (ignoreFirstHashEdge && bs.hashEdge) { local.hashEdge=false; ignoreFirstHashEdge=false; }
		repeatHandler(local, now);
		bool changed=false;
		if (actUp) {
				if (editingValue == 0) editingValue = 10;
				else if (editingValue == 990) editingValue = 0;
				else editingValue += 10;
				changed = true;
		}
		if (actDown) {
				if (editingValue == 0) editingValue = 990;
				else if (editingValue == 10) editingValue = 0;
				else editingValue -= 10;
				changed = true;
		}
		if (changed) { saver.noteActivity(now); if (dirtyCb) dirtyCb(); }
		if (local.starEdge) { return true; }
		if (local.hashEdge) {
				config.saveScreensaverIfChanged(editingValue);
				saver.configure(config.get().screensaverDelaySec = editingValue);
				saver.noteActivity(now);
				return true;
		}
		return false;
	}
}
