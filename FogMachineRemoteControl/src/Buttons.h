#pragma once
#include <Arduino.h>
#include "Pins.h"

struct ButtonState {
  bool up=false, down=false, left=false, right=false;
  bool upEdge=false, downEdge=false, leftEdge=false, rightEdge=false;
  // Legacy compatibility (hash/star mapped to left/right) to avoid interim build errors
  bool hash=false; // maps to left
  bool star=false; // maps to right
  bool hashEdge=false; // maps to leftEdge
  bool starEdge=false; // maps to rightEdge
};

class Buttons {
public:
  void begin();
  ButtonState poll();
private:
  bool lastUp=false, lastDown=false, lastLeft=false, lastRight=false;
};
