#include "screens.h"

#include <U8g2_for_Adafruit_GFX.h>
#include <stdio.h>
#include <time.h>

namespace {

U8G2_FOR_ADAFRUIT_GFX u8g2;

constexpr int PANEL_W = 128;
constexpr int PANEL_H = 64;
constexpr int MARGIN = 3;                       // empty columns on the left and right
constexpr int TOP_MARGIN = 5;                   // empty rows above the label
constexpr int BOTTOM_MARGIN = 5;                // empty rows below the digits
constexpr int INNER_W = PANEL_W - 2 * MARGIN;   // usable width (122)
constexpr int BOTTOM = PANEL_H - BOTTOM_MARGIN; // first empty row at the bottom (59)

// Colours use full channel values (0 or 255): the CIE gamma table makes any
// lower value much dimmer on the LEDs (170 -> 39 % PWM).
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

const uint16_t COLOR_LABEL = rgb(255, 255, 255);
const uint16_t COLOR_VISITORS = rgb(255, 255, 255);
const uint16_t COLOR_COUNTDOWN = rgb(255, 255, 255);
const uint16_t COLOR_LAST_HOUR = rgb(255, 255, 255);  // all text white
const uint16_t COLOR_DIM = rgb(70, 70, 70);
const uint16_t COLOR_DIVIDER = rgb(100, 180, 255);  // light blue
const uint16_t COLOR_CLOCK = rgb(255, 255, 255);

// Draw UTF-8 text centred horizontally in the panel that starts at ox.
void centered(int ox, int baseline, const uint8_t *font, uint16_t color, const char *text) {
  u8g2.setFont(font);
  u8g2.setForegroundColor(color);
  const int w = u8g2.getUTF8Width(text);
  u8g2.drawUTF8(ox + (PANEL_W - w) / 2, baseline, text);
}

// ---------------------------------------------------------------- labels
// Bold label on top of a panel: the largest Cyrillic font in which every word
// fits the panel, wrapped by words into at most two lines.
struct Label {
  const uint8_t *font = nullptr;
  bool fake_bold = true;  // draw twice with a 1-pixel shift
  int lines = 0;
  char line[2][64] = {};
  int bottom = 0;         // first free row under the label
};

int text_w(const char *t, bool bold) { return u8g2.getUTF8Width(t) + (bold ? 1 : 0); }

Label layout_label(const char *text) {
  static const uint8_t *const fonts[] = {u8g2_font_10x20_t_cyrillic, u8g2_font_9x15_t_cyrillic,
                                         u8g2_font_8x13_t_cyrillic, u8g2_font_6x13B_t_cyrillic};
  Label L;
  for (int f = 0; f < 4; ++f) {
    L = Label();
    L.font = fonts[f];
    L.fake_bold = f < 3;
    u8g2.setFont(L.font);
    // greedy word wrap
    char cur[64] = "";
    bool ok = true;
    const char *p = text;
    while (*p && ok) {
      while (*p == ' ') ++p;
      const char *e = p;
      while (*e && *e != ' ') ++e;
      if (e == p) break;
      char word[64];
      snprintf(word, sizeof(word), "%.*s", static_cast<int>(e - p), p);
      char trial[64];
      snprintf(trial, sizeof(trial), cur[0] ? "%s %s" : "%s%s", cur, word);
      if (text_w(trial, L.fake_bold) <= INNER_W) {
        snprintf(cur, sizeof(cur), "%s", trial);
      } else if (cur[0] && L.lines < 1 && text_w(word, L.fake_bold) <= INNER_W) {
        snprintf(L.line[L.lines++], sizeof(L.line[0]), "%s", cur);
        snprintf(cur, sizeof(cur), "%s", word);
      } else {
        ok = false;
      }
      p = e;
    }
    if (ok) {
      if (cur[0]) snprintf(L.line[L.lines++], sizeof(L.line[0]), "%s", cur);
      break;
    }
  }
  u8g2.setFont(L.font);
  const int ascent = u8g2.getFontAscent();
  L.bottom = L.lines ? TOP_MARGIN + ascent + (L.lines - 1) * (ascent + 2) + 2 : TOP_MARGIN;
  return L;
}

void draw_label(int ox, const Label &L) {
  u8g2.setFont(L.font);
  u8g2.setForegroundColor(COLOR_LABEL);
  const int ascent = u8g2.getFontAscent();
  for (int i = 0; i < L.lines; ++i) {
    const int w = text_w(L.line[i], L.fake_bold);
    const int x = ox + (PANEL_W - w) / 2;
    const int y = TOP_MARGIN + ascent + i * (ascent + 2);  // glyph rows: y-ascent .. y-1
    u8g2.drawUTF8(x, y, L.line[i]);
    if (L.fake_bold) u8g2.drawUTF8(x + 1, y, L.line[i]);
  }
}

// ---------------------------------------------------------------- digits
// Digits sit under the labels. Both panels use the same digit height: the
// largest that fits under the taller label. Every character is drawn in a
// fixed-width cell, so changing digits never makes the line jump.
const uint8_t *const kDigitFonts[] = {u8g2_font_logisoso42_tn, u8g2_font_logisoso38_tn,
                                      u8g2_font_logisoso32_tn, u8g2_font_logisoso28_tn,
                                      u8g2_font_logisoso24_tn, u8g2_font_logisoso20_tn,
                                      u8g2_font_logisoso16_tn};
constexpr int kDigitFontCount = sizeof(kDigitFonts) / sizeof(kDigitFonts[0]);

struct Digits {
  const uint8_t *font = nullptr;
  int digit_w = 0, colon_w = 0, ascent = 0;
};

Digits measure(const uint8_t *font) {
  Digits g;
  g.font = font;
  u8g2.setFont(font);
  for (char c = '0'; c <= '9'; ++c) {
    const char str[2] = {c, 0};
    const int w = u8g2.getUTF8Width(str);
    if (w > g.digit_w) g.digit_w = w;
  }
  g.colon_w = u8g2.getUTF8Width(":");
  g.ascent = u8g2.getFontAscent();
  return g;
}

// largest font whose digits fit max_h and whose "00:00:00" fits the panel
Digits choose_digits(int max_h) {
  for (int i = 0; i < kDigitFontCount; ++i) {
    const Digits g = measure(kDigitFonts[i]);
    if (g.ascent <= max_h && 6 * g.digit_w + 2 * g.colon_w <= INNER_W) return g;
  }
  return measure(kDigitFonts[kDigitFontCount - 1]);
}

// Plain number centred by its real width, on the same axis as the label
// (label width includes the 1-pixel fake-bold shift, hence +1).
void draw_number(int ox, const Digits &g, const char *text, uint16_t color) {
  u8g2.setFont(g.font);
  u8g2.setForegroundColor(color);
  const int w = u8g2.getUTF8Width(text);
  u8g2.drawUTF8(ox + (PANEL_W - w + 1) / 2, BOTTOM, text);
}

void draw_digits(int ox, int top, const Digits &g, const char *text, uint16_t color) {
  u8g2.setFont(g.font);
  u8g2.setForegroundColor(color);
  int total = 0;
  for (const char *p = text; *p; ++p) total += (*p == ':') ? g.colon_w : g.digit_w;
  int x = ox + (PANEL_W - total) / 2;
  const int baseline = BOTTOM;  // digits end BOTTOM_MARGIN rows above the panel edge
  for (const char *p = text; *p; ++p) {
    const char str[2] = {*p, 0};
    const int cell = (*p == ':') ? g.colon_w : g.digit_w;
    u8g2.drawUTF8(x + (cell - u8g2.getUTF8Width(str)) / 2, baseline, str);
    x += cell;
  }
}

void visitors_text(const ScreenData &d, char *buf, size_t n, uint16_t &color) {
  if (d.visitors >= 0) snprintf(buf, n, "%d", d.visitors);
  else snprintf(buf, n, "--");
  color = d.visitors >= 0 ? COLOR_VISITORS : COLOR_DIM;
}

void countdown_text(const ScreenData &d, char *buf, size_t n, uint16_t &color) {
  color = COLOR_COUNTDOWN;
  if (!d.time_valid || d.countdown.phase == CD_NOT_SET) {
    snprintf(buf, n, "--:--:--");
    color = COLOR_DIM;
    return;
  }
  // a start moment in the future counts down to it, then counts up from it
  const long r = static_cast<long>(d.countdown.phase == CD_WAITING ? d.countdown.remaining
                                                                   : d.countdown.elapsed);
  snprintf(buf, n, "%02ld:%02ld:%02ld", r / 3600, (r / 60) % 60, r % 60);
}

}  // namespace

void screens_begin(GFXcanvas16 &canvas) {
  u8g2.begin(canvas);
  u8g2.setFontMode(1);  // transparent background
  u8g2.setFontDirection(0);
}

void screens_draw(GFXcanvas16 &canvas, const ScreenData &d) {
  canvas.fillScreen(0);
  const int counter_x = d.swap_panels ? PANEL_W : 0;
  const int countdown_x = d.swap_panels ? 0 : PANEL_W;
  const bool two_panels = canvas.width() >= 2 * PANEL_W;

  const char *cd_label = d.label_countdown;
  const Label l1 = layout_label(d.label_counter);
  const Label l2 = layout_label(cd_label);
  draw_label(counter_x, l1);
  if (two_panels) draw_label(countdown_x, l2);

  // same digit size on both panels, as large as the taller label allows
  const int top = (l1.bottom > l2.bottom || !two_panels) ? l1.bottom : l2.bottom;
  const Digits g = choose_digits(BOTTOM - top - 2);  // keep a 2-row gap under the label
  char buf[24];
  uint16_t color;
  visitors_text(d, buf, sizeof(buf), color);
  draw_number(counter_x, g, buf, color);
  if (two_panels) {
    countdown_text(d, buf, sizeof(buf), color);
    draw_digits(countdown_x, top, g, buf, color);
  }

  // divider between the panels: one light-blue column on each side of the joint
  if (two_panels) {
    constexpr int DIVIDER_MARGIN = 10;  // empty rows above and below the line
    canvas.drawFastVLine(PANEL_W - 1, DIVIDER_MARGIN, PANEL_H - 2 * DIVIDER_MARGIN, COLOR_DIVIDER);
    canvas.drawFastVLine(PANEL_W, DIVIDER_MARGIN, PANEL_H - 2 * DIVIDER_MARGIN, COLOR_DIVIDER);
  }
  // red dot in the top-left corner while Wi-Fi is down
  if (!d.wifi_ok) canvas.fillRect(0, 0, 2, 2, rgb(255, 0, 0));
  // yellow dot in the counter panel's top-right corner: Raspberry Pi is not
  // answering, the number shown is the last one received
  if (d.visitors_stale && d.visitors >= 0) canvas.fillRect(counter_x + PANEL_W - 2, 0, 2, 2, rgb(255, 200, 0));
}
