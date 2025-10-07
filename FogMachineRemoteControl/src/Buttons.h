#pragma once
#include <Arduino.h>
#include "Defaults.h"

struct ButtonState {
  bool up=false, down=false, hash=false, star=false;
  bool upEdge=false, downEdge=false, hashEdge=false, starEdge=false;
};

class Buttons {
public:
  void begin();
  ButtonState poll();
private:
  bool lastUp=false, lastDown=false, lastHash=false, lastStar=false;
};
