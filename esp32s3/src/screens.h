// Draws the two panels into one RGB565 canvas (PANELS*128 x 64).
#pragma once

#include <Adafruit_GFX.h>

#include "countdown.h"

struct ScreenData {
  int visitors = -1;            // -1 = no data from the Raspberry Pi
  bool visitors_stale = false;  // Pi not answering: last value shown + yellow dot
  bool time_valid = false;
  CountdownState countdown;
  const char *label_counter = "";
  const char *label_countdown = "";
  bool swap_panels = false;     // counter on the second panel instead of the first
  bool wifi_ok = false;
};

void screens_begin(GFXcanvas16 &canvas);
void screens_draw(GFXcanvas16 &canvas, const ScreenData &d);
