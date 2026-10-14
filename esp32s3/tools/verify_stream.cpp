// Host-side check: the DP3364S generator must reproduce, clock for clock, the
// stream captured from the working Raspberry Pi setup.
//
//   c++ -O2 -std=c++17 -I../src verify_stream.cpp ../src/dp3364s.cpp -o verify_stream
//   ./verify_stream ref.bin ref_init.bin
//
// ref.bin / ref_init.bin: uint32 GPIO state at each CLK rising edge, recorded
// with the capture-patched rpi-rgb-led-matrix ('regular' pin mapping), test
// image as drawn by test_pixel() below, 2 chained panels.
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "dp3364s.h"

using namespace dp3364s;

// 'regular' hzeller mapping: bus signal -> Raspberry Pi GPIO number
static const int kPin[BUS_SIGNALS] = {11, 27, 7, 8, 9, 10, 22, 23, 24, 25, 15, 4, 18};
static const char *kName[BUS_SIGNALS] = {"R1", "G1", "B1", "R2", "G2", "B2", "A", "B",
                                         "C",  "D",  "E",  "LAT", "ROW"};

static uint16_t to_bus(uint32_t gpio) {
  uint16_t v = 0;
  for (int i = 0; i < BUS_SIGNALS; ++i)
    if (gpio & (1u << kPin[i])) v |= 1u << i;
  return v;
}

static std::vector<uint16_t> load(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  std::vector<uint32_t> raw;
  uint32_t w;
  while (fread(&w, 4, 1, f) == 1) raw.push_back(w);
  fclose(f);
  std::vector<uint16_t> out(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) out[i] = to_bus(raw[i]);
  return out;
}

// Same image as the capture program.
static void test_pixel(int x, int y, uint8_t rgb[3]) {
  const int W = 256, H = 64;
  rgb[0] = rgb[1] = rgb[2] = 0;
  auto set = [&](int r, int g, int b) { rgb[0] = r; rgb[1] = g; rgb[2] = b; };
  if (y == 0) set(255, 255, 255);
  if (y == H - 1) set(255, 0, 0);
  if (x == 0) set(0, 255, 0);
  if (x == W - 1) set(0, 0, 255);
  if (x == 5 && y == 3) set(255, 0, 0);
  if (x == 130 && y == 40) set(0, 255, 0);
  if (x == 200 && y == 33) set(0, 0, 255);
  if (x >= 10 && x < 26 && y >= 10 && y < 14) { const int v = (x - 10) * 17; set(v, v, v); }
}

static bool is_vsync(const std::vector<uint16_t> &s, size_t i) {
  return (s[i] & BIT_LAT) && (s[i + 1] & BIT_LAT) && (s[i + 2] & BIT_LAT) &&
         !(s[i + 3] & BIT_LAT) && (i == 0 || !(s[i - 1] & BIT_LAT));
}

static int compare(const char *what, const uint16_t *got, const uint16_t *ref, size_t n) {
  size_t bad = 0;
  for (size_t i = 0; i < n; ++i) {
    if (got[i] != ref[i]) {
      if (bad < 10) {
        printf("  %s mismatch at %zu: gen=0x%04x ref=0x%04x (", what, i, got[i], ref[i]);
        for (int b = 0; b < BUS_SIGNALS; ++b)
          if ((got[i] ^ ref[i]) & (1u << b)) printf(" %s", kName[b]);
        printf(" )\n");
      }
      ++bad;
    }
  }
  printf("%-28s %zu words, %zu mismatches -> %s\n", what, n, bad, bad ? "FAIL" : "OK");
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s ref.bin ref_init.bin\n", argv[0]); return 2; }
  Layout l;  // defaults: 2 panels, 1/32 scan
  uint16_t lut[256];
  build_gamma_lut(lut, 100);

  std::vector<uint16_t> frame(l.frame_words());
  build_frame(frame.data(), l);
  encode_frame(frame.data(), l, lut, test_pixel);
  printf("frame: %zu words (%.1f KB)\n", frame.size(), frame.size() * 2 / 1024.0);

  int fails = 0;
  // 1) steady-state frame: VSYNC to VSYNC in ref.bin
  std::vector<uint16_t> ref = load(argv[1]);
  std::vector<size_t> vs;
  for (size_t i = 0; i + 4 < ref.size(); ++i)
    if (is_vsync(ref, i)) vs.push_back(i);
  if (vs.size() < 3) { printf("not enough frames in %s\n", argv[1]); return 1; }
  printf("ref frames: %zu, length %zu\n", vs.size() - 1, vs[2] - vs[1]);
  if (vs[2] - vs[1] != frame.size()) { printf("frame length differs: gen %zu\n", frame.size()); ++fails; }
  fails += compare("steady frame #1", frame.data(), &ref[vs[1]], frame.size());
  fails += compare("steady frame #5", frame.data(), &ref[vs[5]], frame.size());

  // 2) register blocks: in ref_init every frame is preceded by 13 writes
  std::vector<uint16_t> ini = load(argv[2]);
  size_t start = 0;
  for (size_t i = 1; i + 40 < ini.size(); ++i) {  // first VSYNC followed by PRE_ACT
    if (is_vsync(ini, i) && (ini[i + 20] & BIT_LAT) && (ini[i + 33] & BIT_LAT)) { start = i; break; }
  }
  std::vector<uint16_t> regs;
  for (int r = 0; r < DEFAULT_REGISTER_COUNT; ++r) {
    std::vector<uint16_t> b(l.register_block_words());
    build_register_block(b.data(), l, DEFAULT_REGISTERS[r]);
    regs.insert(regs.end(), b.begin(), b.end());
  }
  // the reference keeps the previous row address on A-E during register
  // writes; only compare data, LAT and ROW there
  std::vector<uint16_t> ref_regs(ini.begin() + start, ini.begin() + start + regs.size());
  const uint16_t addr_mask = BIT_A | BIT_B | BIT_C | BIT_D | BIT_E;
  for (auto &v : ref_regs) v &= ~addr_mask;
  fails += compare("13 register writes", regs.data(), ref_regs.data(), regs.size());
  // and the frame that follows them. After register writes the reference has
  // address 0 (not 31) on A-E during VSYNC + idle; ROW is off there, so the
  // address is irrelevant in those 20 clocks.
  std::vector<uint16_t> after(ini.begin() + start + regs.size(),
                              ini.begin() + start + regs.size() + frame.size());
  std::vector<uint16_t> gen_after(frame);
  for (int i = 0; i < 3 + l.vsync_idle; ++i) { after[i] &= ~addr_mask; gen_after[i] &= ~addr_mask; }
  fails += compare("frame after init", gen_after.data(), after.data(), frame.size());

  printf(fails ? "\nRESULT: FAIL\n" : "\nRESULT: generator matches the reference stream\n");
  return fails ? 1 : 0;
}
