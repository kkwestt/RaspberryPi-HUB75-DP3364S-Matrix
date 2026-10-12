// Real-time clock with milliseconds for HUB75 panels (any chain length).
// Shows HH:MM:SS.mmm, the date, a white border and three thin R/G/B gradient
// bars, so timing, geometry and grayscale can all be checked at a glance.
#include "graphics.h"
#include "led-matrix.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <string>

using namespace rgb_matrix;

static volatile bool stop = false;
static void handler(int) { stop = true; }

static int TextWidth(const Font &f, const char *s) {
  int w = 0;
  for (; *s; ++s) w += f.CharacterWidth(*s);
  return w;
}

// Monospace clock fonts, largest first; the first that fits the width is used.
static const char *kClockFonts[] = {"courB24", "10x20", "9x18B", "7x13B", "6x13B", "5x8"};
static const int kClockFontCount = sizeof(kClockFonts) / sizeof(kClockFonts[0]);

int main(int argc, char **argv) {
  // Fonts are loaded before the matrix is created: the library drops root
  // privileges during init and may then lose access to the font directory.
  const char *env_dir = getenv("FONT_DIR");
  const std::string font_dir = env_dir ? env_dir : "fonts";
  Font fonts[kClockFontCount];
  bool loaded[kClockFontCount];
  for (int i = 0; i < kClockFontCount; ++i)
    loaded[i] = fonts[i].LoadFont((font_dir + "/" + kClockFonts[i] + ".bdf").c_str());
  Font date_font;
  if (!date_font.LoadFont((font_dir + "/6x13B.bdf").c_str())) {
    fprintf(stderr, "cannot load fonts from %s (set FONT_DIR)\n", font_dir.c_str());
    return 1;
  }

  RGBMatrix::Options opt;
  RuntimeOptions rt;
  RGBMatrix *m = RGBMatrix::CreateFromFlags(&argc, &argv, &opt, &rt);
  if (!m) return 1;
  signal(SIGTERM, handler);
  signal(SIGINT, handler);

  FrameCanvas *c = m->CreateFrameCanvas();
  const int W = c->width(), H = c->height();

  const Font *pick = &date_font;
  for (int i = 0; i < kClockFontCount; ++i) {
    if (loaded[i] && TextWidth(fonts[i], "00:00:00.000") <= W - 6) {
      pick = &fonts[i];
      fprintf(stderr, "clock font: %s\n", kClockFonts[i]);
      break;
    }
  }
  const Font &clock_font = *pick;

  const Color hms(0, 220, 255), ms(255, 200, 0), date_col(160, 160, 160);
  const int clock_w = TextWidth(clock_font, "00:00:00.000");
  const int clock_x = (W - clock_w) / 2;
  const int bars_h = 3, bars_gap = 1;
  const int bars_top = H - 2 - 3 * (bars_h + bars_gap);
  const int clock_y = 3 + clock_font.baseline();
  const int date_y = clock_y + clock_font.height() - clock_font.baseline() + date_font.baseline() + 1;

  while (!stop) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm lt;
    localtime_r(&tv.tv_sec, &lt);
    char hms_s[16], ms_s[8], date_s[32];
    strftime(hms_s, sizeof(hms_s), "%H:%M:%S", &lt);
    snprintf(ms_s, sizeof(ms_s), ".%03ld", static_cast<long>(tv.tv_usec / 1000));
    strftime(date_s, sizeof(date_s), "%d.%m.%Y", &lt);

    c->Clear();
    // border
    for (int x = 0; x < W; ++x) { c->SetPixel(x, 0, 255, 255, 255); c->SetPixel(x, H - 1, 255, 255, 255); }
    for (int y = 0; y < H; ++y) { c->SetPixel(0, y, 255, 255, 255); c->SetPixel(W - 1, y, 255, 255, 255); }
    // clock: HH:MM:SS in cyan, .mmm in yellow
    // DrawText() returns the advance width, not the end position
    const int hms_w = DrawText(c, clock_font, clock_x, clock_y, hms, nullptr, hms_s);
    DrawText(c, clock_font, clock_x + hms_w, clock_y, ms, nullptr, ms_s);
    // date, centered
    DrawText(c, date_font, (W - TextWidth(date_font, date_s)) / 2, date_y, date_col, nullptr, date_s);
    // thin R, G, B gradient bars (dark -> bright)
    for (int px = 3; px < W - 3; ++px) {
      const int v = (px - 3) * 255 / (W - 7);
      for (int k = 0; k < bars_h; ++k) {
        c->SetPixel(px, bars_top + k, v, 0, 0);
        c->SetPixel(px, bars_top + (bars_h + bars_gap) + k, 0, v, 0);
        c->SetPixel(px, bars_top + 2 * (bars_h + bars_gap) + k, 0, 0, v);
      }
    }
    c = m->SwapOnVSync(c);
  }
  m->Clear();
  delete m;
  return 0;
}
