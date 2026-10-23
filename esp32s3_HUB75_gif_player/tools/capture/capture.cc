// Draw a known test image on 2 chained 128x64 DP3364S panels and record the
// first N CLK edges emitted by the (capture-patched) library to a file.
#include "led-matrix.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
using namespace rgb_matrix;

extern uint32_t *capture_buf;
extern size_t capture_len, capture_cap;

int main(int argc, char **argv) {
  const char *out = getenv("CAPTURE_FILE");
  const size_t edges = getenv("CAPTURE_EDGES") ? atol(getenv("CAPTURE_EDGES")) : 3000000;
  RGBMatrix::Options opt; RuntimeOptions rt;
  RGBMatrix *m = RGBMatrix::CreateFromFlags(&argc, &argv, &opt, &rt);
  if (!m) return 1;
  FrameCanvas *c = m->CreateFrameCanvas();
  const int W = c->width(), H = c->height();
  c->Clear();
  // Same pattern as the ESP32 host test (tools/ref_image): border, corners,
  // a few single pixels and gray steps.
  for (int x = 0; x < W; ++x) { c->SetPixel(x, 0, 255, 255, 255); c->SetPixel(x, H - 1, 255, 0, 0); }
  for (int y = 0; y < H; ++y) { c->SetPixel(0, y, 0, 255, 0); c->SetPixel(W - 1, y, 0, 0, 255); }
  c->SetPixel(5, 3, 255, 0, 0);
  c->SetPixel(130, 40, 0, 255, 0);
  c->SetPixel(200, 33, 0, 0, 255);
  for (int i = 0; i < 16; ++i)
    for (int y = 10; y < 14; ++y) c->SetPixel(10 + i, y, i * 17, i * 17, i * 17);
  c = m->SwapOnVSync(c);
  usleep(300000);  // let a few frames go out with the final image
  capture_buf = (uint32_t *)malloc(edges * 4);
  capture_len = 0;
  capture_cap = edges;
  while (capture_len < capture_cap) usleep(10000);
  uint32_t *buf = capture_buf;
  capture_buf = nullptr;
  FILE *f = fopen(out, "wb");
  fwrite(buf, 4, edges, f);
  fclose(f);
  fprintf(stderr, "captured %zu edges -> %s\n", edges, out);
  m->Clear();
  delete m;
  return 0;
}
