// Static geometry/color test pattern for a 128x64 HUB75 panel.
#include "led-matrix.h"
#include <signal.h>
#include <unistd.h>
using namespace rgb_matrix;
static volatile bool stop = false;
static void handler(int) { stop = true; }
int main(int argc, char **argv) {
  RGBMatrix::Options opt; RuntimeOptions rt;
  RGBMatrix *m = RGBMatrix::CreateFromFlags(&argc, &argv, &opt, &rt);
  if (!m) return 1;
  signal(SIGTERM, handler); signal(SIGINT, handler);
  FrameCanvas *c = m->CreateFrameCanvas();
  const int W = c->width(), H = c->height();
  c->Clear();
  // white 1px border
  for (int x = 0; x < W; ++x) { c->SetPixel(x, 0, 255,255,255); c->SetPixel(x, H-1, 255,255,255); }
  for (int y = 0; y < H; ++y) { c->SetPixel(0, y, 255,255,255); c->SetPixel(W-1, y, 255,255,255); }
  // corner markers: top-left RED, top-right GREEN, bottom-left BLUE, bottom-right WHITE (8x8)
  for (int y = 2; y < 10; ++y) for (int x = 2; x < 10; ++x) {
    c->SetPixel(x, y, 255,0,0);
    c->SetPixel(W-1-x, y, 0,255,0);
    c->SetPixel(x, H-1-y, 0,0,255);
    c->SetPixel(W-1-x, H-1-y, 255,255,255);
  }
  // middle: three horizontal gradient bars R, G, B (brightness left->right)
  for (int x = 14; x < W-14; ++x) {
    int v = (x - 14) * 255 / (W - 29);
    for (int y = 14; y < 22; ++y) c->SetPixel(x, y, v,0,0);
    for (int y = 28; y < 36; ++y) c->SetPixel(x, y, 0,v,0);
    for (int y = 42; y < 50; ++y) c->SetPixel(x, y, 0,0,v);
  }
  c = m->SwapOnVSync(c);
  while (!stop) sleep(1);
  m->Clear(); delete m; return 0;
}
