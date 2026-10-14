// ESP32-S3 output driver for DP3364S HUB75 panels.
//
// The whole bitstream (register writes + one frame) is kept in memory and sent
// in an endless loop by GDMA -> LCD_CAM (i8080 mode, 16-bit bus). The LCD
// peripheral generates CLK (PCLK) in hardware, so the clock is perfectly
// steady regardless of Wi-Fi or CPU load; DP3364S derives its grayscale clock
// from CLK, so this is what keeps the panels flicker-free.
//
// Loop: [13 register writes] -> [frame] x frames_per_init -> repeat.
// show() re-encodes only the pixel-data bits of the frame in place.
#pragma once

#include <stdint.h>

#include "dp3364s.h"

struct Hub75Pins {
  int8_t r1, g1, b1, r2, g2, b2;
  int8_t a, b, c, d, e;
  int8_t lat, oe, clk;
};

class Dp3364sDisplay {
 public:
  // panels: number of 128x64 panels in the chain; pclk_hz: CLK frequency.
  bool begin(const Hub75Pins &pins, int panels, uint32_t pclk_hz, int frames_per_init);

  int width() const { return layout_.width(); }
  int height() const { return layout_.height(); }

  // Encode an RGB565 image (width() x height(), row-major) into the frame.
  void show(const uint16_t *rgb565);

  // 0..100, applied through the gamma table on the next show().
  void setBrightness(int percent);

  // Current gain register (REG08), e.g. 0x08BF. Rewritten with the registers.
  void setCurrentGain(uint8_t value);

  // REG0B bit 5: output current x1.5 (DP3364S datasheet).
  void setCurrentBoost(bool on);

  // Stop the output (before a firmware update: flash writes stall PSRAM DMA).
  void stop();
  // restarts the output if it stopped; false if a restart was needed
  bool ensureRunning();
  uint32_t restarts() const { return restarts_; }

  uint32_t frameRate() const;  // full frames per second at the configured CLK

  // Diagnostics: sample the 14 pins (R1..B2, A..E, LAT, OE, CLK) and count
  // level changes / high samples, to verify the signals leave the chip.
  static void pinActivity(const Hub75Pins &pins, uint32_t toggles[14], uint32_t highs[14], int samples);
  const char *error() const { return error_; }
  static const char *debugState();  // GDMA / LCD_CAM registers for the log

 private:
  void writeBackCache(void *p, size_t bytes);
  void buildRegisters();
  void startOutput();
  uint32_t restarts_ = 0;
  bool stopped_ = false;

  dp3364s::Layout layout_;
  uint16_t lut_[256];
  uint16_t registers_[dp3364s::DEFAULT_REGISTER_COUNT];
  uint16_t *regs_buf_ = nullptr;   // internal RAM
  uint16_t *frame_buf_ = nullptr;  // PSRAM (or internal if it fits)
  size_t regs_words_ = 0, frame_words_ = 0;
  bool frame_in_psram_ = false;
  void *desc_ = nullptr;
  uint32_t pclk_hz_ = 0;
  int frames_per_init_ = 16;
  const char *error_ = "";
};
