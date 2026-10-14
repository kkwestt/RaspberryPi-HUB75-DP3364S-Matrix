# Patch rpi-rgb-led-matrix GPIO class to record the pin state at every CLK
# rising edge (what the panel samples). Output: raw little-endian uint32 per edge.
import sys

h = sys.argv[1] + "/lib/gpio.h"
c = sys.argv[1] + "/lib/gpio.cc"

s = open(h).read()
decl = """
// ---- capture hook (reference stream recording) ----
extern uint32_t capture_state;
extern uint32_t capture_clock_mask;
extern uint32_t *capture_buf;
extern size_t capture_len, capture_cap;
static inline void capture_set(uint32_t v) {
  const uint32_t before = capture_state;
  capture_state |= v;
  if (capture_buf && (v & capture_clock_mask) && !(before & capture_clock_mask) &&
      capture_len < capture_cap) {
    capture_buf[capture_len++] = capture_state;
  }
}
static inline void capture_clr(uint32_t v) { capture_state &= ~v; }
"""
anchor = "namespace rgb_matrix {"
assert s.count(anchor) >= 1
s = s.replace(anchor, decl + anchor, 1)

old_set = "    *gpio_set_bits_low_ = static_cast<uint32_t>(value & 0xFFFFFFFF);"
old_clr = "    *gpio_clr_bits_low_ = static_cast<uint32_t>(value & 0xFFFFFFFF);"
assert old_set in s and old_clr in s
s = s.replace(old_set, "    capture_set(static_cast<uint32_t>(value));\n" + old_set)
s = s.replace(old_clr, "    capture_clr(static_cast<uint32_t>(value));\n" + old_clr)
if "#include <stddef.h>" not in s:
    s = "#include <stddef.h>\n#include <stdint.h>\n" + s
open(h, "w").write(s)

t = open(c).read()
t += """
// ---- capture hook storage ----
uint32_t capture_state = 0;
uint32_t capture_clock_mask = 1u << 17;  // 'regular' mapping CLK = GPIO17
uint32_t *capture_buf = nullptr;
size_t capture_len = 0, capture_cap = 0;
"""
open(c, "w").write(t)
print("capture patch applied")
