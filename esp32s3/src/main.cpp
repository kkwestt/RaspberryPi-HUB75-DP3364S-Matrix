// Stand display: ESP32-S3 driving two DP3364S 128x64 HUB75 panels.
//   panel 1: visitors who stood at the stand >= 10 s today (from the Pi)
//   panel 2: time elapsed since a start moment set on the web page (reset / date)
// Time comes from NTP (re-synced every hour). Settings: http://stand-display.local
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include <time.h>

#include "config.h"
#include "countdown.h"
#include "esp_sntp.h"
#include "hub75_dp3364s.h"
#include "screens.h"
#include "secrets.h"

#ifndef OTA_PASSWORD
#warning "OTA_PASSWORD is not set in include/secrets.h, using the default"
#define OTA_PASSWORD "stand-display"
#endif

// ---------------------------------------------------------------- settings
struct Settings {
  CountdownConfig cd;
  String label_counter = DEFAULT_LABEL_COUNTER;
  String label_countdown = DEFAULT_LABEL_COUNTDOWN;
  String counter_url = DEFAULT_COUNTER_URL;
  int brightness = DEFAULT_BRIGHTNESS;
  int gain = DEFAULT_CURRENT_GAIN;
  bool swap_panels = false;
  bool boost = false;     // DP3364S REG0B bit 5: LED current x1.5
  int led_sec = DEFAULT_LED_SEC;          // WS2812 on time after a new visitor, 0 = off
  uint32_t led_color = DEFAULT_LED_COLOR; // 0xRRGGBB
};

static Settings S;
static Preferences prefs;

static void load_settings() {
  prefs.begin("display", true);
  S.cd.start = prefs.getLong64("up_start", 0);
  S.label_counter = prefs.getString("label1", DEFAULT_LABEL_COUNTER);
  S.label_countdown = prefs.getString("label2", DEFAULT_LABEL_COUNTDOWN);
  S.counter_url = prefs.getString("url", DEFAULT_COUNTER_URL);
  S.brightness = prefs.getInt("bright", DEFAULT_BRIGHTNESS);
  S.gain = prefs.getInt("gain", DEFAULT_CURRENT_GAIN);
  S.swap_panels = prefs.getBool("swap", false);
  S.boost = prefs.getBool("boost", false);
  S.led_sec = prefs.getInt("led_sec", DEFAULT_LED_SEC);
  S.led_color = prefs.getUInt("led_col", DEFAULT_LED_COLOR);
  prefs.end();
}

static void save_settings() {
  prefs.begin("display", false);
  prefs.putLong64("up_start", S.cd.start);
  prefs.putString("label1", S.label_counter);
  prefs.putString("label2", S.label_countdown);
  prefs.putString("url", S.counter_url);
  prefs.putInt("bright", S.brightness);
  prefs.putInt("gain", S.gain);
  prefs.putBool("swap", S.swap_panels);
  prefs.putBool("boost", S.boost);
  prefs.putInt("led_sec", S.led_sec);
  prefs.putUInt("led_col", S.led_color);
  prefs.end();
}

// ---------------------------------------------------------------- state
static Dp3364sDisplay display;
static GFXcanvas16 *canvas = nullptr;
static WebServer server(80);
// Last visitor count received from the Raspberry Pi. It is kept (and saved to
// flash) so the display keeps showing it while the Pi is unreachable and
// after an ESP32 reboot.
static volatile int g_visitors = -1;           // -1 = never received
static volatile time_t g_visitors_time = 0;    // unix time it was received (0 = unknown)
static volatile uint32_t g_visitors_at = 0;    // millis() of the last good reply this boot
static volatile time_t g_last_ntp = 0;
static volatile bool g_redraw = true;
static volatile uint32_t g_led_until = 0;      // millis() when the WS2812 goes off (0 = off)
static bool display_ok = false;
static const Hub75Pins kPins = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2, PIN_A,
                                PIN_B,  PIN_C,  PIN_D,  PIN_E,  PIN_LAT, PIN_OE, PIN_CLK};

static bool time_valid() { return time(nullptr) > 1700000000; }

static void on_ntp_sync(struct timeval *) { g_last_ntp = time(nullptr); }

// Data is "stale" when the Pi has not answered for VISITORS_STALE_MS.
static bool visitors_stale() {
  return g_visitors_at == 0 || millis() - g_visitors_at > VISITORS_STALE_MS;
}

static bool same_local_day(time_t a, time_t b) {
  struct tm ta, tb;
  localtime_r(&a, &ta);
  localtime_r(&b, &tb);
  return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

// The Pi reports "today"; a value received on an earlier day is shown as 0.
static int visitors_for_display(time_t now) {
  const int v = g_visitors;
  if (v < 0) return -1;
  if (time_valid() && g_visitors_time > 0 && !same_local_day(g_visitors_time, now)) return 0;
  return v;
}

static void load_last_visitors() {
  prefs.begin("display", true);
  g_visitors = prefs.getInt("vis", -1);
  g_visitors_time = prefs.getLong64("vis_t", 0);
  prefs.end();
}

// ---------------------------------------------------------------- counter poll
static void poll_task(void *) {
  JsonDocument filter;
  filter[COUNTER_JSON_KEY] = true;
  Preferences store;          // own handle: NVS is thread-safe, Preferences is not
  int saved = g_visitors;     // value currently in flash
  uint32_t saved_at = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(1500);
      http.setConnectTimeout(1500);
      if (http.begin(S.counter_url)) {
        if (http.GET() == HTTP_CODE_OK) {
          JsonDocument doc;
          if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) &&
              doc[COUNTER_JSON_KEY].is<int>()) {
            const int v = doc[COUNTER_JSON_KEY].as<int>();
            // a new visitor (not the first reply after boot, not a reset to 0)
            if (g_visitors_at != 0 && v > g_visitors && S.led_sec > 0)
              g_led_until = (millis() + S.led_sec * 1000UL) | 1;
            if (v != g_visitors || visitors_stale()) g_redraw = true;
            g_visitors = v;
            g_visitors_time = time_valid() ? time(nullptr) : 0;
            g_visitors_at = millis();
          }
        }
        http.end();
      }
    }
    // save the last value to flash when it changed, at most once a minute
    if (g_visitors >= 0 && g_visitors != saved && (saved_at == 0 || millis() - saved_at > 60000)) {
      store.begin("display", false);
      store.putInt("vis", g_visitors);
      store.putLong64("vis_t", g_visitors_time);
      store.end();
      saved = g_visitors;
      saved_at = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(COUNTER_POLL_MS));
  }
}

// ---------------------------------------------------------------- web page
static String fmt_time(time_t t) {
  if (t <= 0) return "—";
  struct tm tm;
  localtime_r(&t, &tm);
  char b[32];
  strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", &tm);
  return b;
}

static String fmt_remaining(const CountdownState &s) {
  if (s.phase == CD_NOT_SET) return "не задан";
  char b[48];
  const long r = static_cast<long>(s.phase == CD_WAITING ? s.remaining : s.elapsed);
  if (s.phase == CD_WAITING) {
    snprintf(b, sizeof(b), "до старта %02ld:%02ld:%02ld", r / 3600, (r / 60) % 60, r % 60);
    return b;
  }
  snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", r / 3600, (r / 60) % 60, r % 60);
  return b;
}

// unix time -> value for <input type=datetime-local> (local time)
static String fmt_input(time_t t) {
  if (t <= 0) return "";
  struct tm tm;
  localtime_r(&t, &tm);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M", &tm);
  return b;
}

static String html_escape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

static void handle_root() {
  const time_t now = time(nullptr);
  const CountdownState cs = countdown_at(S.cd, now);
  String h;
  h.reserve(6000);
  h += F("<!doctype html><html lang=ru><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Табло стенда</title><style>"
         "body{font:15px system-ui,sans-serif;margin:0;padding:16px;background:#f4f5f7;color:#1d2330}"
         "@media(prefers-color-scheme:dark){body{background:#15171c;color:#e8eaf0}.c{background:#1e2128!important;border-color:#30343d!important}input,select{background:#15171c;color:#e8eaf0;border-color:#30343d}}"
         ".c{background:#fff;border:1px solid #e3e5ea;border-radius:10px;padding:14px;margin:0 0 14px;max-width:560px}"
         "h1{font-size:18px}h2{font-size:13px;text-transform:uppercase;color:#888;margin:0 0 10px}"
         "td{padding:3px 12px 3px 0}label{display:block;margin:8px 0 3px}"
         "input,select{font:inherit;padding:6px 8px;border:1px solid #ccd;border-radius:6px;width:100%;box-sizing:border-box}"
         "button{font:inherit;padding:7px 14px;border-radius:6px;border:0;background:#2f6fde;color:#fff;margin:10px 8px 0 0;cursor:pointer}"
         "button.s{background:#888}</style><h1>Табло стенда</h1>");
  h += "<div class=c><h2>Сейчас</h2><table>";
  h += "<tr><td>Время</td><td>" + (time_valid() ? fmt_time(now) : String("не синхронизировано")) + "</td></tr>";
  h += "<tr><td>Последняя синхронизация NTP</td><td>" + fmt_time(g_last_ntp) + "</td></tr>";
  h += "<tr><td>Посетители (с Raspberry Pi)</td><td>" +
       (g_visitors >= 0 ? String(visitors_for_display(now)) + " (получено " + fmt_time(g_visitors_time) + ")" : String("нет данных")) +
       (visitors_stale() ? " — Raspberry Pi не отвечает, показано последнее значение" : "") + "</td></tr>";
  h += "<tr><td>Счётчик</td><td>" + fmt_remaining(cs) + "</td></tr>";
  h += "<tr><td>Счёт с</td><td>" + fmt_time(S.cd.start) + "</td></tr>";
  h += "<tr><td>Wi-Fi</td><td>" + WiFi.SSID() + ", " + String(WiFi.RSSI()) + " dBm, " + WiFi.localIP().toString() + "</td></tr>";
  h += "<tr><td>Обновление панелей</td><td>" + String(display.frameRate()) + " кадр/с</td></tr>";
  h += "</table></div>";

  h += "<div class=c><h2>Счётчик времени</h2><form method=post action=/countdown>";
  h += "<label>Считать с (дата и время, местное; дата в будущем — обратный отсчёт до неё)</label><input name=at type=datetime-local value=\"" + fmt_input(S.cd.start) + "\">";
  h += "<button name=action value=save>Сохранить дату</button>"
       "<button name=action value=reset>Сбросить (с текущего момента)</button>"
       "<button class=s name=action value=clear>Очистить</button></form></div>";

  h += "<div class=c><h2>Настройки</h2><form method=post action=/settings>";
  h += "<label>Надпись над счётчиком</label><input name=label1 maxlength=24 value=\"" + html_escape(S.label_counter) + "\">";
  h += "<label>Надпись над отсчётом</label><input name=label2 maxlength=24 value=\"" + html_escape(S.label_countdown) + "\">";
  h += "<label>Адрес счётчика на Raspberry Pi</label><input name=url value=\"" + html_escape(S.counter_url) + "\">";
  h += "<label>Яркость, % (гамма)</label><input name=bright type=number min=5 max=100 value=" + String(S.brightness) + ">";
  h += "<label>Ток светодиодов, REG08 (0–255; 191 по умолчанию, 255 = +33%)</label><input name=gain type=number min=0 max=255 value=" + String(S.gain) + ">";
  h += String("<label>Усиление тока ×1,5 (REG0B бит 5)</label><select name=boost><option value=0") + (S.boost ? "" : " selected") +
       ">выключено</option><option value=1" + (S.boost ? " selected" : "") + ">включено: ярче на 50%, больше нагрев и ток</option></select>";
  h += String("<label>Порядок панелей</label><select name=swap><option value=0") + (S.swap_panels ? "" : " selected") +
       ">счётчик на первой панели</option><option value=1" + (S.swap_panels ? " selected" : "") + ">счётчик на второй панели</option></select>";
  char hex[8];
  snprintf(hex, sizeof(hex), "%06X", static_cast<unsigned>(S.led_color & 0xFFFFFF));
  h += "<label>Светодиод WS2812 при новом посетителе: время, с (0–60, 0 = выключен)</label><input name=led_sec type=number min=0 max=60 value=" + String(S.led_sec) + ">";
  h += "<label>Цвет светодиода (HEX, 6 цифр)</label><input name=led_color maxlength=7 pattern=\"#?[0-9A-Fa-f]{6}\" value=\"" + String(hex) + "\">";
  h += "<button>Сохранить</button></form>";
  h += "<form method=post action=/ledtest><button class=s>Проверить светодиод</button></form></div>";
  h += "<div class=c><h2>Сервис</h2><a href=/update>Обновить прошивку через браузер</a> · "
       "<a href=/pintest>Проверка линий HUB75 (мультиметром)</a></div>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void redirect_home() {
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_countdown() {
  const String action = server.arg("action");
  if (action == "save") {
    int Y, M, D, h, m;
    if (sscanf(server.arg("at").c_str(), "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m) == 5) {
      struct tm tm = {};
      tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m;
      tm.tm_isdst = -1;
      S.cd.start = mktime(&tm);  // TZ is set, so this is local time
    }
  } else if (action == "reset" && time_valid()) {
    S.cd.start = time(nullptr);
  } else if (action == "clear") {
    S.cd.start = 0;
  }
  save_settings();
  g_redraw = true;
  redirect_home();
}

static void handle_settings() {
  if (server.hasArg("label1")) S.label_counter = server.arg("label1");
  if (server.hasArg("label2")) S.label_countdown = server.arg("label2");
  if (server.hasArg("url")) S.counter_url = server.arg("url");
  if (server.hasArg("bright")) S.brightness = constrain(server.arg("bright").toInt(), 5, 100);
  if (server.hasArg("gain")) S.gain = constrain(server.arg("gain").toInt(), 0, 255);
  if (server.hasArg("swap")) S.swap_panels = server.arg("swap") == "1";
  if (server.hasArg("boost")) S.boost = server.arg("boost") == "1";
  if (server.hasArg("led_sec")) S.led_sec = constrain(server.arg("led_sec").toInt(), 0, 60);
  if (server.hasArg("led_color")) {
    String c = server.arg("led_color");
    c.trim();
    if (c.startsWith("#")) c = c.substring(1);
    bool ok = c.length() == 6;
    for (char ch : c) ok = ok && isxdigit(static_cast<unsigned char>(ch));
    if (ok) S.led_color = strtoul(c.c_str(), nullptr, 16);
  }
  save_settings();
  display.setBrightness(S.brightness);
  display.setCurrentGain(static_cast<uint8_t>(S.gain));
  display.setCurrentBoost(S.boost);
  g_redraw = true;
  redirect_home();
}

static void handle_ledtest() {
  g_led_until = (millis() + (S.led_sec > 0 ? S.led_sec : 3) * 1000UL) | 1;
  redirect_home();
}

// WS2812: on while g_led_until is in the future
static void led_update() {
  static int shown = -1;
  const uint32_t until = g_led_until;
  const bool on = until != 0 && static_cast<int32_t>(until - millis()) > 0;
  if (!on && until != 0) g_led_until = 0;
  const int want = on ? static_cast<int>(S.led_color & 0xFFFFFF) : 0;
  // resend twice a second: a glitch on the data line is corrected quickly
  static uint32_t sent_at = 0;
  if (want == shown && millis() - sent_at < 500) return;
  shown = want;
  sent_at = millis();
  neopixelWrite(PIN_WS2812, (want >> 16) & 0xFF, (want >> 8) & 0xFF, want & 0xFF);
}

static void handle_status() {
  JsonDocument d;
  const time_t now = time(nullptr);
  const CountdownState cs = countdown_at(S.cd, now);
  d["time"] = static_cast<long long>(now);
  d["time_valid"] = time_valid();
  d["last_ntp"] = static_cast<long long>(g_last_ntp);
  d["visitors"] = visitors_for_display(now);
  d["visitors_received_at"] = static_cast<long long>(g_visitors_time);
  d["visitors_stale"] = visitors_stale();
  d["elapsed"] = static_cast<long long>(cs.elapsed);
  d["remaining"] = static_cast<long long>(cs.remaining);
  d["countdown_phase"] = static_cast<int>(cs.phase);
  d["frame_rate"] = display.frameRate();
  // diagnostics: DMA/LCD registers
  d["hw"] = Dp3364sDisplay::debugState();
  d["display_ok"] = display_ok;
  d["output_restarts"] = display.restarts();
  d["uptime_s"] = millis() / 1000;
  d["reset_reason"] = static_cast<int>(esp_reset_reason());
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------- firmware update
// Browser upload at /update (user "admin", password OTA_PASSWORD) in addition
// to ArduinoOTA (PlatformIO: pio run -e <env>_ota -t upload).
static bool require_auth() {
  if (server.authenticate("admin", OTA_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

static void handle_update_page() {
  if (!require_auth()) return;
  server.send(200, "text/html; charset=utf-8",
              F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                "<title>Обновление прошивки</title><body style='font:15px system-ui;padding:16px'>"
                "<h1 style='font-size:18px'>Обновление прошивки</h1>"
                "<p>Файл: <code>.pio/build/&lt;env&gt;/firmware.bin</code>. Во время обновления панели погаснут.</p>"
                "<form method=post enctype=multipart/form-data><input type=file name=fw accept=.bin> "
                "<button>Загрузить</button></form><p><a href=/>← назад</a></p>"));
}

static void handle_update_done() {
  if (!require_auth()) return;
  const bool ok = !Update.hasError();
  server.send(200, "text/html; charset=utf-8",
              ok ? "Готово, перезагрузка… <a href=/>на главную</a>" : "Ошибка обновления, см. лог. <a href=/update>назад</a>");
  delay(500);
  if (ok) ESP.restart();
}

static void handle_update_upload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!server.authenticate("admin", OTA_PASSWORD)) return;
    Serial.printf("[ota] web update: %s\n", up.filename.c_str());
    display.stop();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.isRunning() && Update.end(true)) Serial.printf("[ota] web update ok, %u bytes\n", up.totalSize);
    else Update.printError(Serial);
  }
}

// ---------------------------------------------------------------- wiring test
// /pintest: stops the panel output and drives one HUB75 line high (others low)
// as a plain GPIO, so every solder joint can be checked with a multimeter on
// the HUB75 connector. /pintest?exit=1 reboots back to normal operation.
static const char *kSignalNames[14] = {"R1", "G1", "B1", "R2", "G2", "B2", "A", "B",
                                       "C",  "D",  "E",  "LAT", "OE", "CLK"};
static const int kHub75Pin[14] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 12, 8, 14, 15, 13};
static bool g_pintest = false;

static int8_t signal_gpio(int i) {
  const int8_t g[14] = {kPins.r1, kPins.g1, kPins.b1, kPins.r2, kPins.g2, kPins.b2, kPins.a,
                        kPins.b,  kPins.c,  kPins.d,  kPins.e,  kPins.lat, kPins.oe, kPins.clk};
  return g[i];
}

static void handle_pintest() {
  if (!require_auth()) return;
  if (server.hasArg("exit")) {
    server.send(200, "text/html; charset=utf-8", "Перезагрузка… <a href=/>на главную</a>");
    delay(300);
    ESP.restart();
  }
  int sel = -2;  // -2: nothing chosen yet, -1: all low, 14: all high
  if (server.hasArg("s")) sel = server.arg("s").toInt();
  if (sel != -2) {
    if (!g_pintest) { display.stop(); g_pintest = true; }
    for (int i = 0; i < 14; ++i) {
      const gpio_num_t g = static_cast<gpio_num_t>(signal_gpio(i));
      esp_rom_gpio_connect_out_signal(g, SIG_GPIO_OUT_IDX, false, false);
      gpio_set_direction(g, GPIO_MODE_OUTPUT);
      gpio_set_level(g, (sel == 14 || sel == i) ? 1 : 0);
    }
  }
  String h = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
               "<title>Проверка линий</title><body style='font:15px system-ui;padding:16px;max-width:640px'>"
               "<h1 style='font-size:18px'>Проверка линий HUB75</h1>"
               "<p>Вывод на панели остановлен. Выбранная линия = 1, остальные = 0. Мерьте мультиметром "
               "между выводом разъёма HUB75 на шилде и GND (выводы 4 или 16): 1 ≈ 3,3–5 В, 0 ≈ 0 В.</p>"
               "<table style='border-collapse:collapse'>");
  for (int i = 0; i < 14; ++i) {
    h += "<tr><td style='padding:4px 10px'><a href='/pintest?s=" + String(i) + "'><b>" + kSignalNames[i] +
         "</b></a></td><td style='padding:4px 10px'>GPIO " + String(signal_gpio(i)) +
         "</td><td style='padding:4px 10px'>HUB75 вывод " + String(kHub75Pin[i]) + "</td><td>" +
         (sel == i ? "◀ сейчас = 1" : "") + "</td></tr>";
  }
  h += "</table><p><a href='/pintest?s=-1'>Все = 0</a> · <a href='/pintest?s=14'>Все = 1</a>" +
       String(sel == 14 ? " (сейчас все = 1)" : sel == -1 ? " (сейчас все = 0)" : "") +
       "</p><p><a href='/pintest?exit=1'>Выйти и перезагрузить</a></p>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void ota_begin() {
  ArduinoOTA.setHostname(HOSTNAME);  // also announces HOSTNAME.local via mDNS
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[ota] start");
    display.stop();
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[ota] error %u, rebooting\n", e);
    delay(200);
    ESP.restart();  // the display was stopped; start over cleanly
  });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);
  delay(200);
  // watchdog: reboot if the main loop stalls for WDT_TIMEOUT_S
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);
  Serial.printf("[boot] reset reason: %d\n", static_cast<int>(esp_reset_reason()));
  load_settings();
  load_last_visitors();

  const Hub75Pins &pins = kPins;
  display.setCurrentGain(static_cast<uint8_t>(S.gain));
  // let the panels' power settle when everything is switched on together
  for (uint32_t t0 = millis(); millis() - t0 < STARTUP_DELAY_MS;) { esp_task_wdt_reset(); delay(50); }
  display_ok = display.begin(pins, PANELS, PCLK_HZ, FRAMES_PER_INIT);
  if (!display_ok) {
    Serial.printf("display init failed (%s), rebooting in 5 s\n", display.error());
    delay(5000);
    ESP.restart();
    Serial.printf("display init failed: %s\n", display.error());
  }
  display.setCurrentGain(static_cast<uint8_t>(S.gain));
  display.setCurrentBoost(S.boost);
  display.setBrightness(S.brightness);
  Serial.printf("display: %dx%d, %u frames/s\n", display.width(), display.height(), display.frameRate());

  canvas = new GFXcanvas16(display.width(), display.height());
  screens_begin(*canvas);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  sntp_set_sync_interval(NTP_SYNC_INTERVAL_S * 1000UL);
  sntp_set_time_sync_notification_cb(on_ntp_sync);
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);

  server.on("/", HTTP_GET, handle_root);
  server.on("/countdown", HTTP_POST, handle_countdown);
  server.on("/settings", HTTP_POST, handle_settings);
  server.on("/status.json", HTTP_GET, handle_status);
  server.on("/update", HTTP_GET, handle_update_page);
  server.on("/pintest", HTTP_GET, handle_pintest);
  server.on("/ledtest", HTTP_POST, handle_ledtest);
  server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  server.begin();

  xTaskCreatePinnedToCore(poll_task, "poll", 6144, nullptr, 1, nullptr, 0);
}

void loop() {
  static bool mdns_started = false;
  static long last_remaining = -2;
  static bool last_wifi = false, last_time_valid = false, last_stale = true;
  static int last_shown = -2;

  esp_task_wdt_reset();
  server.handleClient();
  ArduinoOTA.handle();
  // no Wi-Fi for WIFI_REBOOT_MS -> reboot and try again
  static uint32_t wifi_lost_at = 0;
  if (WiFi.status() == WL_CONNECTED) wifi_lost_at = 0;
  else if (!wifi_lost_at) wifi_lost_at = millis() | 1;
  else if (millis() - wifi_lost_at > WIFI_REBOOT_MS) { Serial.println("no Wi-Fi, rebooting"); ESP.restart(); }

  // status line every 5 s on the serial log
  static uint32_t last_log = 0;
  if (millis() - last_log > 5000) {
    last_log = millis();
    Serial.printf("[status] display=%s err='%s' %dx%d %u fps | psram total=%u free=%u | wifi=%d ip=%s | time_valid=%d | visitors=%d stale=%d\n",
                  display_ok ? "ok" : "FAIL", display.error(), display.width(), display.height(),
                  display.frameRate(), ESP.getPsramSize(), ESP.getFreePsram(), WiFi.status(),
                  WiFi.localIP().toString().c_str(), time_valid(), g_visitors, visitors_stale());
    Serial.printf("[hw] %s\n", Dp3364sDisplay::debugState());
  }

  const bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi && !mdns_started) {  // ArduinoOTA starts mDNS (stand-display.local)
    ota_begin();
    mdns_started = true;
  }

  const time_t now = time(nullptr);
  const bool tv = time_valid();
  const CountdownState cs = countdown_at(S.cd, now);
  // redraw every second: countdown digits, or the clock once it has finished
  const long rem = cs.phase == CD_RUNNING ? static_cast<long>(cs.elapsed)
                 : cs.phase == CD_WAITING ? -2 - static_cast<long>(cs.remaining) : -1;
  const int shown = visitors_for_display(now);
  const bool stale = visitors_stale();

  led_update();
  static uint32_t last_check = 0;
  if (display_ok && !g_pintest && millis() - last_check > 2000) {
    last_check = millis();
    if (!display.ensureRunning()) {
      // after a cold power-up the output stops by itself and a restart of the
      // output alone leaves the panels dark; a software reboot fixes it
      Serial.println("display output had stopped, rebooting");
      delay(100);
      ESP.restart();
    }
  }

  if (!g_pintest && (g_redraw || rem != last_remaining || wifi != last_wifi || tv != last_time_valid ||
      shown != last_shown || stale != last_stale)) {
    g_redraw = false;
    last_remaining = rem;
    last_wifi = wifi;
    last_time_valid = tv;
    last_shown = shown;
    last_stale = stale;
    ScreenData d;
    d.visitors = shown;
    d.visitors_stale = stale;
    d.time_valid = tv;
    d.countdown = cs;
    d.label_counter = S.label_counter.c_str();
    d.label_countdown = S.label_countdown.c_str();
    d.swap_panels = S.swap_panels;
    d.wifi_ok = wifi;
    screens_draw(*canvas, d);
    display.show(canvas->getBuffer());
  }
  delay(20);
}
