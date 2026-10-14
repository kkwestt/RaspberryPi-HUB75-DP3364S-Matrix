#include "dp3364s.h"

#include <math.h>

namespace dp3364s {

static inline uint16_t addr_bits(int row) {
  return static_cast<uint16_t>((row & 0x1F) << ADDR_SHIFT);
}

void build_frame(uint16_t *out, const Layout &l) {
  size_t n = 0;
  int row = l.scan_rows - 1;  // address of the last row stays on the bus
  // VSYNC
  for (int i = 0; i < 3; ++i) out[n++] = BIT_LAT | addr_bits(row);
  for (int i = 0; i < l.vsync_idle; ++i) out[n++] = addr_bits(row);
  const int scan = l.scan_clocks();
  const int data = l.data_clocks();
  const int block = l.block_clocks();
  for (int p = 0; p < scan; ++p) {
    if (p > 0 && p <= data && p % block == 0) {
      out[n++] = BIT_LAT | addr_bits(row);  // DATA_LATCH, scan counter frozen
    }
    if (p % l.row_period == 0) row = (row + 1) % l.scan_rows;
    uint16_t v = addr_bits(row);
    if (p % l.row_period < l.row_pulse) v |= BIT_ROW;
    out[n++] = v;
  }
  for (int i = 0; i < l.pad_end; ++i) out[n++] = addr_bits(row);
}

size_t build_register_block(uint16_t *out, const Layout &l, uint16_t reg) {
  size_t n = 0;
  for (int i = 0; i < 3; ++i) out[n++] = BIT_LAT;   // VSYNC
  for (int i = 0; i < 17; ++i) out[n++] = 0;
  for (int i = 0; i < 14; ++i) out[n++] = BIT_LAT;  // PRE_ACT
  out[n++] = 0;
  const int clocks = l.block_clocks();
  for (int i = 0; i < clocks; ++i) {
    const int b = 15 - (i % 16);  // same 16-bit word for every chip, MSB first
    uint16_t v = ((reg >> b) & 1) ? DATA_MASK : 0;
    if (i >= clocks - 5) v |= BIT_LAT;  // WR_CFG = LAT high for 5 clocks
    out[n++] = v;
  }
  out[n++] = (reg & 1) ? DATA_MASK : 0;  // data lines keep the last bit
  return n;
}

void build_gamma_lut(uint16_t lut[256], int brightness_percent) {
  if (brightness_percent < 0) brightness_percent = 0;
  if (brightness_percent > 100) brightness_percent = 100;
  const float out_max = 2047.0f;  // 11-bit PWM
  for (int c = 0; c < 256; ++c) {
    const float v = c * brightness_percent / 255.0f;  // CIE L* 0..100
    const float y = (v <= 8.0f) ? v / 902.3f : powf((v + 16.0f) / 116.0f, 3.0f);
    lut[c] = static_cast<uint16_t>(lroundf(out_max * y)) << 1;  // 12-bit aligned
  }
}

}  // namespace dp3364s
