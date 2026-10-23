#include "screens.h"

#include <U8g2_for_Adafruit_GFX.h>
#include <stdio.h>

namespace {

U8G2_FOR_ADAFRUIT_GFX u8g2;

constexpr int MARGIN = 4;          // empty columns on the left
constexpr int TEXT_X = MARGIN + 12; // text starts after the status dot
constexpr int LINE_PITCH = 21;     // three lines of 9x15 in 64 rows
constexpr int FIRST_BASELINE = 16;

// Colours use full channel values (0 or 255): the CIE gamma table makes any
// lower value much dimmer on the LEDs (170 -> 39 % PWM).
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

// Everything on the panel is one warm white (~2700 K). The green/yellow/red
// state dot is warm white too now, so it marks the line rather than the state;
// change COLOR_WARM alone to go back to a cooler or brighter white.
const uint16_t COLOR_WARM = rgb(255, 180, 107);
const uint16_t COLOR_TEXT = COLOR_WARM;
const uint16_t COLOR_OK = COLOR_WARM;
const uint16_t COLOR_WAIT = COLOR_WARM;
const uint16_t COLOR_ERROR = COLOR_WARM;
const uint16_t COLOR_HINT = COLOR_WARM;

void line(int n, uint16_t color, const char *text) {
  u8g2.setFont(u8g2_font_9x15_t_cyrillic);
  u8g2.setForegroundColor(color);
  u8g2.drawUTF8(TEXT_X, FIRST_BASELINE + n * LINE_PITCH, text);
}

}  // namespace

void screens_begin(GFXcanvas16 &canvas) {
  u8g2.begin(canvas);
  u8g2.setFontMode(1);  // transparent background
  u8g2.setFontDirection(0);
}

void screens_draw_status(GFXcanvas16 &canvas, const StatusData &d) {
  canvas.fillScreen(0);
  char buf[96];

  // line 1: state, with a coloured dot in front
  uint16_t color;
  if (d.wifi == WIFI_CONNECTED) {
    color = COLOR_OK;
    snprintf(buf, sizeof(buf), "Wi-Fi: подключено, %d dBm", d.rssi);
  } else if (d.wifi == WIFI_CONNECTING) {
    color = COLOR_WAIT;
    snprintf(buf, sizeof(buf), "Wi-Fi: подключение... %d с", d.connecting_s);
  } else {
    color = COLOR_ERROR;
    snprintf(buf, sizeof(buf), "Wi-Fi: сеть не задана");
  }
  canvas.fillCircle(MARGIN + 4, FIRST_BASELINE - 5, 4, color);
  line(0, color, buf);

  // line 2: the network being joined
  if (d.wifi != WIFI_NOT_SET) {
    snprintf(buf, sizeof(buf), "Сеть: %s", d.ssid);
    line(1, COLOR_TEXT, buf);
  }

  // line 3: where to open the web page
  if (d.wifi == WIFI_CONNECTED) {
    snprintf(buf, sizeof(buf), "IP: %s", d.ip);
    line(2, COLOR_TEXT, buf);
  } else if (d.ap_on) {
    snprintf(buf, sizeof(buf), "Настройка: %s, %s", d.ap_ssid, d.ap_ip);
    line(d.wifi == WIFI_NOT_SET ? 1 : 2, COLOR_HINT, buf);
  }
}

void screens_draw_clock(GFXcanvas16 &canvas, const struct tm &t, bool colon_on) {
  canvas.fillScreen(0);
  const int w = canvas.width();
  char buf[32];

  // HH:MM as large as the panel allows; the colon blinks so a frozen clock is
  // obvious at a glance. Hours, colon and minutes are placed separately and the
  // colon's slot is always reserved: blanking it must not move the digits.
  u8g2.setFont(u8g2_font_logisoso32_tn);
  u8g2.setForegroundColor(COLOR_TEXT);
  char hh[4], mm[4];
  snprintf(hh, sizeof(hh), "%02d", t.tm_hour);
  snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  const int wh = u8g2.getUTF8Width(hh);
  const int wc = u8g2.getUTF8Width(":");
  const int x = (w - (wh + wc + u8g2.getUTF8Width(mm))) / 2;
  u8g2.drawUTF8(x, 40, hh);
  if (colon_on) u8g2.drawUTF8(x + wh, 40, ":");
  u8g2.drawUTF8(x + wh + wc, 40, mm);

  // date underneath, dimmer than the time
  u8g2.setFont(u8g2_font_9x15_t_cyrillic);
  u8g2.setForegroundColor(COLOR_HINT);
  snprintf(buf, sizeof(buf), "%02d.%02d.%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  const int dw = u8g2.getUTF8Width(buf);
  u8g2.drawUTF8((w - dw) / 2, 60, buf);
}

void screens_draw_ip(GFXcanvas16 &canvas, const char *ip) {
  canvas.fillScreen(0);
  const int w = canvas.width();

  u8g2.setFont(u8g2_font_9x15_t_cyrillic);
  u8g2.setForegroundColor(COLOR_TEXT);
  const char *label = "Адрес плеера";
  u8g2.drawUTF8((w - u8g2.getUTF8Width(label)) / 2, 24, label);

  // the address itself as large as it fits: 15 px on one panel, 24 px on more
  const bool wide = w >= 256;
  u8g2.setFont(wide ? u8g2_font_logisoso24_tn : u8g2_font_9x15_t_cyrillic);
  const int iw = u8g2.getUTF8Width(ip);
  u8g2.drawUTF8((w - iw) / 2, wide ? 56 : 48, ip);
}
