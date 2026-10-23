// LED video player: Waveshare ESP32-S3-RGB-Matrix driving three DP3364S
// 128x64 HUB75 panels (384x64). Videos from the TF card, switched over DMX
// (planned, see README). For now: Wi-Fi status on the panels and the web page
// for Wi-Fi and display settings: http://led-player.local
//
// Wi-Fi: joins the saved network. If there is none, or it cannot connect for
// WIFI_AP_FALLBACK_MS, it also starts the open setup access point ESP32S3+HUB75
// (captive portal at 192.168.4.1: every address redirects to /setup). The saved
// network is retried every WIFI_RETRY_AP_MS while the access point is up, and
// the access point switches off WIFI_AP_OFF_DELAY_MS after the station joins.
// None of this touches the panel: the player or the clock keeps running.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <vector>
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include <time.h>
#include "soc/gpio_sig_map.h"

#include "config.h"
#include "hub75_dp3364s.h"
#include "player.h"
#include "rtc.h"
#include "screens.h"
#include "secrets.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef OTA_PASSWORD
#warning "OTA_PASSWORD is not set in include/secrets.h, using the default"
#define OTA_PASSWORD "led-player"
#endif

#define WIFI_RETRY_MS 15000  // re-run WiFi.begin() this often while not connected

// ---------------------------------------------------------------- settings
struct Settings {
  String wifi_ssid = WIFI_SSID;
  String wifi_pass = WIFI_PASSWORD;
  int brightness = DEFAULT_BRIGHTNESS;
  int gain = DEFAULT_CURRENT_GAIN;
  bool boost = false;  // DP3364S REG0B bit 5: LED current x1.5
  String video;        // clip to play on boot, empty = show the clock
  int frame_ms = DEFAULT_FRAME_MS;
  bool autoplay = false;  // start `video` by itself after the splash screen
  int panels = PANELS;    // 128x64 modules in the chain; applied on reboot
  bool cycle = false;     // walk through every clip on the card
  int cycle_s = 5;        // seconds per clip while cycling
};

// REG08 holds the LED current in its low 6 bits (0..63 = 0..100 %); the top two
// bits are mode flags captured from the working Raspberry Pi setup, so they are
// carried over untouched. Raising the raw byte past 0xBF wraps the current
// field back to a low value, which is why 210 looks dimmer than 191.
constexpr uint8_t GAIN_MASK = 0x3F;

static int gain_to_pct(int reg) { return ((reg & GAIN_MASK) * 100 + GAIN_MASK / 2) / GAIN_MASK; }

static uint8_t pct_to_gain(int pct, int reg) {
  const int v = (pct * GAIN_MASK + 50) / 100;
  return static_cast<uint8_t>((reg & ~GAIN_MASK) | (v & GAIN_MASK));
}

static Settings S;
static Preferences prefs;

static void load_settings() {
  prefs.begin("player", true);
  S.wifi_ssid = prefs.getString("wifi_ssid", WIFI_SSID);
  S.wifi_pass = prefs.getString("wifi_pass", WIFI_PASSWORD);
  S.brightness = prefs.getInt("bright", DEFAULT_BRIGHTNESS);
  S.gain = prefs.getInt("gain", DEFAULT_CURRENT_GAIN);
  S.boost = prefs.getBool("boost", false);
  S.video = prefs.getString("video", "");
  S.frame_ms = prefs.getInt("frame_ms", DEFAULT_FRAME_MS);
  S.autoplay = prefs.getBool("autoplay", false);
  S.panels = constrain(prefs.getInt("panels", PANELS), 1, 8);
  S.cycle = prefs.getBool("cycle", false);
  S.cycle_s = constrain(prefs.getInt("cycle_s", 5), 1, 600);
  prefs.end();
}

static void save_settings() {
  prefs.begin("player", false);
  prefs.putString("wifi_ssid", S.wifi_ssid);
  prefs.putString("wifi_pass", S.wifi_pass);
  prefs.putInt("bright", S.brightness);
  prefs.putInt("gain", S.gain);
  prefs.putBool("boost", S.boost);
  prefs.putString("video", S.video);
  prefs.putInt("frame_ms", S.frame_ms);
  prefs.putBool("autoplay", S.autoplay);
  prefs.putInt("panels", S.panels);
  prefs.putBool("cycle", S.cycle);
  prefs.putInt("cycle_s", S.cycle_s);
  prefs.end();
}

// ---------------------------------------------------------------- state
static Dp3364sDisplay display;
static Player player;
static Rtc rtc;
static volatile bool g_ntp_fresh = false;  // set by the SNTP callback, handled in loop()
static String g_shown;  // what the panel is showing, so it is redrawn only on change
static GFXcanvas16 *canvas = nullptr;
static WebServer server(80);
static DNSServer dns;
static bool display_ok = false;
static const Hub75Pins kPins = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2, PIN_A,
                                PIN_B,  PIN_C,  PIN_D,  PIN_E,  PIN_LAT, PIN_OE, PIN_CLK};

// Wi-Fi
static String g_ap_ssid;
static bool g_ap_on = false;
static uint32_t g_offline_since = 0;    // millis() when the station lost / started looking for the network
static uint32_t g_connected_since = 0;  // millis() when the station connected (0 = not connected)
static uint32_t g_last_begin = 0;       // millis() of the last WiFi.begin()
static bool g_wifi_restart = false;     // settings changed: reconnect from the loop
static std::vector<String> g_scan;      // network names from the last scan
static bool g_scanned = false;

static bool wifi_connected() { return WiFi.status() == WL_CONNECTED; }

// Current time, read from the RTC when it has one. The system clock is the
// fallback: it is right while NTP has run this session, but it does not
// survive a reboot.
static bool time_valid(struct tm *out) {
  if (rtc.read(out)) return true;
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // the epoch only passes this after a sync
  localtime_r(&now, out);
  return true;
}

static void ap_start() {
  if (g_ap_on) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(g_ap_ssid.c_str());  // no password: the portal is the whole point
  dns.start(53, "*", WiFi.softAPIP());  // captive portal: every name -> the web page
  g_ap_on = true;
  Serial.printf("[wifi] setup AP %s at %s\n", g_ap_ssid.c_str(), WiFi.softAPIP().toString().c_str());
}

static void ap_stop() {
  if (!g_ap_on) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_ap_on = false;
  Serial.println("[wifi] setup AP off");
}

static void sta_begin() {
  g_last_begin = millis();
  if (S.wifi_ssid.isEmpty()) return;
  Serial.printf("[wifi] connecting to '%s'\n", S.wifi_ssid.c_str());
  WiFi.disconnect();
  WiFi.begin(S.wifi_ssid.c_str(), S.wifi_pass.c_str());
}

static void wifi_begin() {
  g_ap_ssid = AP_SSID;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  // Retries are done by wifi_loop(): the built-in auto-reconnect rescans all
  // the time when the network is missing, which disturbs the setup AP.
  WiFi.setAutoReconnect(false);
  g_offline_since = millis() | 1;
  if (S.wifi_ssid.isEmpty()) ap_start();
  else sta_begin();
}

static void wifi_loop() {
  const uint32_t now = millis();
  if (g_wifi_restart) {
    g_wifi_restart = false;
    g_connected_since = 0;
    g_offline_since = now | 1;
    if (S.wifi_ssid.isEmpty()) {
      WiFi.disconnect();
      ap_start();
    } else {
      sta_begin();
    }
  }

  if (wifi_connected()) {
    if (!g_connected_since) {
      g_connected_since = now | 1;
      g_offline_since = 0;
      Serial.printf("[wifi] connected to '%s', ip %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      MDNS.end();
      if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    }
    if (g_ap_on && now - g_connected_since > WIFI_AP_OFF_DELAY_MS) ap_stop();
  } else {
    if (g_connected_since) {
      Serial.println("[wifi] connection lost");
      g_connected_since = 0;
      g_offline_since = now | 1;
    }
    // While the setup AP is up, retry the saved network only every couple of
    // minutes: each attempt briefly disturbs the AP, and by then nobody has
    // used the portal anyway.
    const uint32_t retry_after = g_ap_on ? WIFI_RETRY_AP_MS : WIFI_RETRY_MS;
    if (!S.wifi_ssid.isEmpty() && now - g_last_begin > retry_after) sta_begin();
    if (!g_ap_on && (S.wifi_ssid.isEmpty() || now - g_offline_since > WIFI_AP_FALLBACK_MS)) ap_start();
  }
  if (g_ap_on) dns.processNextRequest();
}

static void wifi_scan() {
  const int n = WiFi.scanNetworks();
  g_scan.clear();
  for (int i = 0; i < n; ++i) {
    const String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (const auto &e : g_scan) dup = dup || e == s;
    if (!dup) g_scan.push_back(s);
  }
  WiFi.scanDelete();
  g_scanned = true;
}

static StatusData status_data() {
  static String ip, ap_ip;  // keep the strings alive for the returned pointers
  StatusData d;
  d.ssid = S.wifi_ssid.c_str();
  if (S.wifi_ssid.isEmpty()) d.wifi = WIFI_NOT_SET;
  else if (wifi_connected()) d.wifi = WIFI_CONNECTED;
  else d.wifi = WIFI_CONNECTING;
  ip = WiFi.localIP().toString();
  d.ip = ip.c_str();
  d.rssi = d.wifi == WIFI_CONNECTED ? WiFi.RSSI() : 0;
  d.connecting_s = g_offline_since ? (millis() - g_offline_since) / 1000 : 0;
  d.ap_on = g_ap_on;
  d.ap_ssid = g_ap_ssid.c_str();
  ap_ip = WiFi.softAPIP().toString();
  d.ap_ip = ap_ip.c_str();
  return d;
}

// ---------------------------------------------------------------- web page
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

static const char kStyle[] PROGMEM =
    "<!doctype html><html lang=ru><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>LED-плеер</title><style>"
    "body{font:15px system-ui,sans-serif;margin:0;padding:16px;background:#f4f5f7;color:#1d2330}"
    "@media(prefers-color-scheme:dark){body{background:#15171c;color:#e8eaf0}.c{background:#1e2128!important;border-color:#30343d!important}input,select{background:#15171c;color:#e8eaf0;border-color:#30343d}}"
    ".c{background:#fff;border:1px solid #e3e5ea;border-radius:10px;padding:14px;margin:0 auto 14px;max-width:560px}"
    "h1{font-size:18px;max-width:560px;margin:0 auto 14px;text-align:center}"
    "h2{font-size:13px;text-transform:uppercase;color:#888;margin:0 0 10px}"
    "td{padding:3px 12px 3px 0}label{display:block;margin:8px 0 3px}"
    "input,select{font:inherit;padding:6px 8px;border:1px solid #ccd;border-radius:6px;width:100%;box-sizing:border-box}"
    "button{font:inherit;padding:7px 14px;border-radius:6px;border:0;background:#2f6fde;color:#fff;margin:10px 8px 0 0;cursor:pointer}"
    "button.s{background:#888}"
    "a.b{display:inline-block;font:inherit;padding:7px 14px;border-radius:6px;background:#2f6fde;color:#fff;"
    "text-decoration:none;margin:4px 8px 0 0}a.b.s{background:#888}"
    ".f{border-top:1px solid #e3e5ea;padding:10px;margin:0 -10px;cursor:pointer;border-radius:8px}"
    ".f:first-of-type{border-top:0}.f.on{background:#e8eefc}"
    ".ft{display:block;width:100%;background:#000;border-radius:4px;image-rendering:pixelated;margin-bottom:6px}"
    ".fn{word-break:break-all}.fm{color:#888;font-size:13px;margin:2px 0 4px}"
    "@media(prefers-color-scheme:dark){.f{border-color:#30343d}.f.on{background:#2c3647}}</style>";

static void handle_root() {
  if (g_ap_on && !g_scanned) wifi_scan();  // first visit over the setup AP: list the networks
  const StatusData d = status_data();
  String h;
  h.reserve(6000);
  h += FPSTR(kStyle);
  h += "<h1>LED-плеер</h1><div class=c><h2>Сейчас</h2><table>";
  h += "<tr><td>Wi-Fi</td><td>";
  if (d.wifi == WIFI_CONNECTED)
    h += "подключено к «" + html_escape(S.wifi_ssid) + "», " + String(d.rssi) + " dBm, " + d.ip;
  else if (d.wifi == WIFI_CONNECTING)
    h += "подключение к «" + html_escape(S.wifi_ssid) + "», " + String(d.connecting_s) + " с";
  else
    h += "сеть не задана";
  h += "</td></tr>";
  h += "<tr><td>Точка доступа</td><td>" +
       (g_ap_on ? html_escape(g_ap_ssid) + ", " + d.ap_ip : String("выключена")) + "</td></tr>";
  h += "<tr><td>Экран</td><td>" + String(display.width()) + "×" + String(display.height()) + ", " +
       String(display.frameRate()) + " кадр/с" + (display_ok ? "" : ", ОШИБКА") + "</td></tr>";
  h += "<tr><td>PSRAM свободно</td><td>" + String(ESP.getFreePsram() / 1024) + " КБ</td></tr>";
  h += "<tr><td>Работает</td><td>" + String(millis() / 60000) + " мин</td></tr>";
  h += "</table></div>";

  h += "<div class=c><h2>Wi-Fi</h2><form method=post action=/wifi>";
  h += "<label>Сеть</label><input name=ssid list=nets maxlength=32 value=\"" + html_escape(S.wifi_ssid) + "\"><datalist id=nets>";
  for (const auto &s : g_scan) h += "<option value=\"" + html_escape(s) + "\">";
  h += "</datalist>";
  h += "<label>Пароль (пусто — оставить прежний для той же сети)</label><input name=pass type=password maxlength=64>";
  h += "<button>Сохранить и подключиться</button></form>";
  h += "<form method=post action=/scan><button class=s>Найти сети</button></form></div>";

  h += "<div class=c><h2>Экран</h2><form method=post action=/settings>";
  h += "<label>Яркость, % (гамма)</label><input name=bright type=number min=5 max=100 value=" + String(S.brightness) + ">";
  h += "<label>Ток светодиодов, % (REG08, младшие 6 бит; 100 % = 63, значение по умолчанию)</label>"
       "<input name=gain_pct type=number min=0 max=100 value=" + String(gain_to_pct(S.gain)) + ">";
  h += String("<label>Усиление тока ×1,5 (REG0B бит 5)</label><select name=boost><option value=0") + (S.boost ? "" : " selected") +
       ">выключено</option><option value=1" + (S.boost ? " selected" : "") + ">включено: ярче на 50%, больше нагрев и ток</option></select>";
  h += "<label>Модулей 128×64 в цепочке (применяется после перезагрузки)</label>"
       "<input name=panels type=number min=1 max=8 value=" + String(S.panels) + ">";
  h += "<button>Сохранить</button></form></div>";
  h += "<div class=c><h2>Сервис</h2>"
       "<a class=b href=/>Ролики и плеер</a>"
       "<a class=b href=/update>Обновить прошивку</a>"
       "<a class='b s' href=/pintest>Проверка линий HUB75</a>"
       "<a class='b s' href=/status.json>status.json</a></div>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void redirect_home() {
  server.sendHeader("Location", "/setup");
  server.send(303);
}

static void handle_wifi() {
  String ssid = server.arg("ssid");
  ssid.trim();
  const String pass = server.arg("pass");
  if (!pass.isEmpty() || ssid != S.wifi_ssid) S.wifi_pass = pass;  // new network: empty = open
  S.wifi_ssid = ssid;
  save_settings();
  String h = FPSTR(kStyle);
  h += "<h1>LED-плеер</h1><div class=c>";
  if (ssid.isEmpty()) {
    h += "Сеть удалена. Плеер работает только как точка доступа «" + html_escape(g_ap_ssid) + "».";
  } else {
    h += "Подключаюсь к «" + html_escape(ssid) + "». Если вы на этой же сети, откройте "
         "<a href='http://" HOSTNAME ".local/'>" HOSTNAME ".local</a> или IP-адрес с экрана плеера.";
  }
  h += "</div>";
  server.send(200, "text/html; charset=utf-8", h);
  g_wifi_restart = true;  // after the reply is sent
}

static void handle_scan() {
  wifi_scan();
  redirect_home();
}

static void handle_settings() {
  const int was_panels = S.panels;
  if (server.hasArg("panels")) S.panels = constrain(server.arg("panels").toInt(), 1, 8);
  if (server.hasArg("bright")) S.brightness = constrain(server.arg("bright").toInt(), 5, 100);
  if (server.hasArg("gain_pct")) S.gain = pct_to_gain(constrain(server.arg("gain_pct").toInt(), 0, 100), S.gain);
  if (server.hasArg("boost")) S.boost = server.arg("boost") == "1";
  save_settings();
  display.setBrightness(S.brightness);
  display.setCurrentGain(static_cast<uint8_t>(S.gain));
  display.setCurrentBoost(S.boost);
  if (S.panels != was_panels) {
    // the canvas and the DMA stream are sized when the display starts, so the
    // chain length can only change over a restart
    server.send(200, "text/html; charset=utf-8",
                "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content='6;url=/'>"
                "<body style='font:15px system-ui;padding:16px'>Перезагружаюсь с новым числом модулей…");
    delay(300);
    ESP.restart();
  }
  redirect_home();
}

static void handle_status() {
  JsonDocument d;
  const StatusData s = status_data();
  d["wifi_state"] = s.wifi == WIFI_CONNECTED ? "connected" : s.wifi == WIFI_CONNECTING ? "connecting" : "not_set";
  d["wifi_ssid"] = S.wifi_ssid;
  d["ip"] = s.ip;
  d["rssi"] = s.rssi;
  d["offline_s"] = s.connecting_s;
  d["ap_on"] = g_ap_on;
  d["ap_ssid"] = g_ap_ssid;
  d["frame_rate"] = display.frameRate();
  d["display_ok"] = display_ok;
  d["hw"] = Dp3364sDisplay::debugState();
  d["output_restarts"] = display.restarts();
  d["psram_free"] = ESP.getFreePsram();
  d["uptime_s"] = millis() / 1000;
  d["reset_reason"] = static_cast<int>(esp_reset_reason());
  d["rtc"] = !rtc.ok() ? "missing" : rtc.lost_power() ? "unset" : "ok";
  struct tm tnow;
  if (time_valid(&tnow)) {
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tnow);
    d["time"] = buf;
  }
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

// Unknown address: over the setup AP send the phone's captive-portal check to
// the settings page, otherwise 404.
static void handle_not_found() {
  if (g_ap_on) {
    // over the setup AP the point is to join a network, so land on the settings
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup");
    server.send(302);
    return;
  }
  server.send(404, "text/plain", "not found");
}

// ---------------------------------------------------------------- firmware update
// Browser upload at /update (user "admin", password OTA_PASSWORD); PlatformIO:
// pio run -e waveshare_matrix_ota -t upload
static bool require_auth() {
#if !WEB_PASSWORD
  return true;
#else
  if (server.authenticate("admin", OTA_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
#endif
}

static void handle_update_page() {
  if (!require_auth()) return;
  String h = FPSTR(kStyle);
  h += F("<h1>Обновление прошивки</h1><div class=c>"
         "<p>Файл: <code>.pio/build/&lt;env&gt;/firmware.bin</code>. Во время обновления панели погаснут.</p>"
         "<form method=post enctype=multipart/form-data><input type=file name=fw accept=.bin>"
         "<button>Загрузить</button></form></div>"
         "<div class=c><a class='b s' href=/>К плееру</a></div>");
  server.send(200, "text/html; charset=utf-8", h);
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
#if WEB_PASSWORD
    if (!server.authenticate("admin", OTA_PASSWORD)) return;
#endif
    Serial.printf("[ota] web update: %s\n", up.filename.c_str());
    display.stop();  // flash writes stall PSRAM DMA
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.isRunning() && Update.end(true)) Serial.printf("[ota] web update ok, %u bytes\n", up.totalSize);
    else Update.printError(Serial);
  }
}


// Browser-side GIF converter. The board never decodes GIF: the page turns the
// file into the raw RGB565 stream the player already reads and posts that to
// /upload. The decoder is written out in full below because the browser's own
// ImageDecoder needs a secure context and the device serves plain HTTP.
static const char kGifJs[] PROGMEM = R"JS(<script>
// GIF decoding happens here, in the page, not on the board. WebCodecs'
// ImageDecoder is not an option: it needs a secure context and the device
// serves plain HTTP, so this is a small GIF87a/89a decoder of our own.
const VW=window.VW, VH=window.VH;
const $=i=>document.getElementById(i);
const say=t=>{ $('gs').textContent=t; };

function gifEach(b, cb){
  let p=0;
  const u8=()=>b[p++];
  const u16=()=>{const v=b[p]|(b[p+1]<<8); p+=2; return v;};
  let sig=''; for(let i=0;i<6;i++) sig+=String.fromCharCode(b[p++]);
  if(sig!=='GIF87a'&&sig!=='GIF89a') throw new Error('это не GIF');
  const W=u16(), H=u16(), fl=u8(); u8(); u8();
  let gct=null;
  if(fl&0x80){ const n=2<<(fl&7); gct=b.subarray(p,p+n*3); p+=n*3; }

  const blocks=()=>{ const parts=[]; let t=0,n;
    while((n=u8())){ parts.push(b.subarray(p,p+n)); t+=n; p+=n; }
    const o=new Uint8Array(t); let k=0; for(const q of parts){ o.set(q,k); k+=q.length; } return o; };

  const lzw=(mcs,data,count)=>{
    const MAX=4096, clear=1<<mcs, eoi=clear+1;
    let cs=mcs+1, mask=(1<<cs)-1, next=eoi+1;
    const pre=new Int32Array(MAX), suf=new Int32Array(MAX), st=new Uint8Array(MAX+1);
    for(let i=0;i<clear;i++) suf[i]=i;
    const out=new Uint8Array(count);
    let bits=0,datum=0,bi=0,oi=0,top=0,first=0,old=-1;
    while(oi<count){
      if(top===0){
        if(bits<cs){ if(bi>=data.length) break; datum|=data[bi++]<<bits; bits+=8; continue; }
        let c=datum&mask; datum>>=cs; bits-=cs;
        if(c===clear){ cs=mcs+1; mask=(1<<cs)-1; next=eoi+1; old=-1; continue; }
        if(c===eoi) break;
        if(old===-1){ st[top++]=suf[c]; old=c; first=c; continue; }
        const inC=c;
        if(c>=next){ st[top++]=first; c=old; }
        while(c>=clear){ st[top++]=suf[c]; c=pre[c]; }
        first=suf[c]&0xff; st[top++]=first;
        if(next<MAX){ pre[next]=old; suf[next]=first; next++;
          if((next&mask)===0&&next<MAX){ cs++; mask=(1<<cs)-1; } }
        old=inC;
      }
      out[oi++]=st[--top];
    }
    return out;
  };

  const cv=new Uint8Array(W*H*4);
  let delay=10, tIdx=-1, disp=0, saved=null, n=0;
  for(;;){
    const blk=u8();
    if(blk===0x3B||blk===undefined) break;
    if(blk===0x21){ const lab=u8();
      if(lab===0xF9){ u8(); const f=u8(); delay=u16(); tIdx=u8(); u8(); disp=(f>>2)&7; if(!(f&1)) tIdx=-1; }
      else blocks();
      continue; }
    if(blk!==0x2C) break;
    const ix=u16(), iy=u16(), iw=u16(), ih=u16(), f=u8();
    let ct=gct;
    if(f&0x80){ const k=2<<(f&7); ct=b.subarray(p,p+k*3); p+=k*3; }
    const inter=!!(f&0x40), mcs=u8();
    const idx=lzw(mcs, blocks(), iw*ih);
    if(disp===3) saved=cv.slice();
    const rows=[];
    if(inter){ for(let y=0;y<ih;y+=8) rows.push(y); for(let y=4;y<ih;y+=8) rows.push(y);
               for(let y=2;y<ih;y+=4) rows.push(y); for(let y=1;y<ih;y+=2) rows.push(y); }
    else for(let y=0;y<ih;y++) rows.push(y);
    for(let r=0;r<rows.length;r++){ const y=rows[r];
      for(let x=0;x<iw;x++){ const v=idx[r*iw+x]; if(v===tIdx) continue;
        const cx=ix+x, cy=iy+y; if(cx>=W||cy>=H) continue;
        const o=(cy*W+cx)*4, c=v*3;
        cv[o]=ct[c]; cv[o+1]=ct[c+1]; cv[o+2]=ct[c+2]; cv[o+3]=255; } }
    cb(cv, W, H, (delay||10)*10); n++;
    if(disp===2){ for(let y=0;y<ih;y++) for(let x=0;x<iw;x++){
        const cx=ix+x, cy=iy+y; if(cx>=W||cy>=H) continue;
        const o=(cy*W+cx)*4; cv[o]=cv[o+1]=cv[o+2]=cv[o+3]=0; } }
    else if(disp===3&&saved) cv.set(saved);
  }
  return n;
}

// The decoded source frames are kept so the crop frame can be dragged without
// decoding again; the clip itself is rebuilt from the file on upload, so the
// whole animation is converted even when the preview holds only part of it.
const PREV_MAX=120, PREV_BUDGET=48<<20;
let gBytes=null, gSrc=[], gCount=0, gMs=100, gName='', gTimer=null;

function stopPreview(){ if(gTimer){ clearInterval(gTimer); gTimer=null; } }

// Crop position, 0..1 along each axis; 0.5 is centred. Dragging the frame on
// the source picture moves these.
let gOx=0.5, gOy=0.5;

// Where the scaled source lands on the panel, for the current settings.
function placement(W,H){
  const cover=$('gc').checked;
  const k=cover?Math.max(VW/W,VH/H):Math.min(VW/W,VH/H);
  const w=W*k, h=H*k;
  return {k, w, h, dx:(VW-w)*gOx, dy:(VH-h)*gOy};
}

function toPanel(dx2, rgba, W, H){
  const pl=placement(W,H);
  const src=toPanel.c||(toPanel.c=document.createElement('canvas'));
  const sx=src.getContext('2d');
  if(src.width!==W||src.height!==H){ src.width=W; src.height=H; }
  sx.putImageData(new ImageData(new Uint8ClampedArray(rgba), W, H), 0, 0);
  dx2.fillStyle='#000'; dx2.fillRect(0,0,VW,VH);
  dx2.drawImage(src, pl.dx, pl.dy, pl.w, pl.h);
}

function pack(img){
  const d=img.data, out=new Uint8Array(VW*VH*2);
  let o=0;
  for(let i=0;i<d.length;i+=4){
    const v=((d[i]&0xF8)<<8)|((d[i+1]&0xFC)<<3)|(d[i+2]>>3);
    out[o++]=v&255; out[o++]=v>>8;            // little-endian, as the player reads it
  }
  return out;
}

// source picture with the crop rectangle drawn over it
function drawOverlay(){
  if(!gSrc.length) return;
  const f=gSrc[0], cv=$('go');
  cv.width=f.W; cv.height=f.H;
  const cx=cv.getContext('2d');
  cx.putImageData(new ImageData(new Uint8ClampedArray(f.data), f.W, f.H), 0, 0);
  const pl=placement(f.W,f.H);
  // the panel rectangle, expressed back in source pixels
  const rx=-pl.dx/pl.k, ry=-pl.dy/pl.k, rw=VW/pl.k, rh=VH/pl.k;
  cx.save();
  cx.fillStyle='rgba(0,0,0,.55)';
  cx.fillRect(0,0,f.W,ry>0?ry:0);
  if(ry+rh<f.H) cx.fillRect(0,ry+rh,f.W,f.H-(ry+rh));
  if(rx>0) cx.fillRect(0,Math.max(0,ry),rx,Math.min(rh,f.H));
  if(rx+rw<f.W) cx.fillRect(rx+rw,Math.max(0,ry),f.W-(rx+rw),Math.min(rh,f.H));
  cx.strokeStyle='#2f6fde'; cx.lineWidth=Math.max(1,f.W/200);
  cx.strokeRect(rx,ry,rw,rh);
  cx.restore();
}

function render(){
  stopPreview();
  if(!gSrc.length) return;
  const cv=$('gp'); cv.width=VW; cv.height=VH;
  const cx=cv.getContext('2d',{willReadFrequently:true});
  let i=0;
  const tick=()=>{ const f=gSrc[i%gSrc.length]; toPanel(cx, f.data, f.W, f.H); i++; };
  tick();
  if(gSrc.length>1) gTimer=setInterval(tick, Math.max(40,gMs));
  drawOverlay();
  $('gi').textContent='Источник '+gSrc[0].W+'×'+gSrc[0].H+' · на панель '+VW+'×'+VH+
                      ' · кадров '+gCount+' · '+gMs+' мс на кадр · '+
                      (gCount*VW*VH*2/1048576).toFixed(1)+' МБ'+
                      (gSrc.length<gCount?' · в предпросмотре первые '+gSrc.length:'');
}

async function prepare(){
  const f=$('gf').files[0];
  stopPreview();
  gBytes=null; gSrc=[]; gCount=0; $('gw').style.display='none'; say('');
  if(!f) return;
  try{
    gOx=gOy=0.5;
    gBytes=new Uint8Array(await f.arrayBuffer());
    say('Разбираю файл…');
    await new Promise(r=>setTimeout(r,30));   // let the message paint first

    let delays=0, kept=0;
    const take=(rgba,W,H,ms)=>{
      gCount++; delays+=ms;
      if(gSrc.length<PREV_MAX && kept+rgba.length<=PREV_BUDGET){
        gSrc.push({data:rgba.slice(), W, H, ms}); kept+=rgba.length;
      }
    };

    if(f.type==='image/gif'||/\.gif$/i.test(f.name)){
      gifEach(gBytes, take);
    }else{
      const img=new Image(), url=URL.createObjectURL(f);
      await new Promise((ok,no)=>{ img.onload=ok; img.onerror=()=>no(new Error('не смог прочитать картинку')); img.src=url; });
      const c=document.createElement('canvas'); c.width=img.width; c.height=img.height;
      c.getContext('2d').drawImage(img,0,0);
      take(c.getContext('2d').getImageData(0,0,img.width,img.height).data, img.width, img.height, 100);
      URL.revokeObjectURL(url);
    }
    if(!gCount){ say('В файле нет кадров.'); return; }

    gMs=Math.max(10,Math.round(delays/gCount));
    gName=f.name.replace(/\.[^.]+$/,'').replace(/[^A-Za-z0-9_-]/g,'_')+'_'+VW+'x'+VH+'_rgb565.bin';
    $('gw').style.display='block';
    say('');
    render();
  }catch(e){
    say('Не получилось: '+e.message);
  }
}

async function upload(){
  if(!gBytes){ say('Сначала выберите файл.'); return; }
  $('gb').disabled=true;
  say('Собираю ролик целиком…');
  await new Promise(r=>setTimeout(r,30));
  try{
    const cv=document.createElement('canvas'); cv.width=VW; cv.height=VH;
    const cx=cv.getContext('2d',{willReadFrequently:true});
    const chunks=[]; let total=0;
    const conv=(rgba,W,H)=>{ toPanel(cx,rgba,W,H);
      const out=pack(cx.getImageData(0,0,VW,VH)); chunks.push(out); total+=out.length; };

    const isGif=$('gf').files[0] && ($('gf').files[0].type==='image/gif'||/\.gif$/i.test($('gf').files[0].name));
    if(isGif) gifEach(gBytes, conv);
    else { const f=gSrc[0]; conv(f.data,f.W,f.H); }

    const bin=new Uint8Array(total); let k=0;
    for(const c of chunks){ bin.set(c,k); k+=c.length; }
    say('Отправляю '+(total/1048576).toFixed(1)+' МБ на карту…');
    const fd=new FormData();
    fd.append('f',new Blob([bin],{type:'application/octet-stream'}),gName);
    const r=await fetch('/upload',{method:'POST',body:fd});
    if(!r.ok){ say('Карта вернула ошибку '+r.status); $('gb').disabled=false; return; }
    stopPreview();
    location.href='/';
  }catch(e){
    say('Не получилось: '+e.message);
    $('gb').disabled=false;
  }
}

// Dragging the crop frame: the pointer moves in CSS pixels on a canvas that is
// drawn at source resolution, so convert through the element's own box.
function dragSetup(){
  const go=$('go');
  let on=false, px=0, py=0;
  const clamp=v=>v<0?0:v>1?1:v;
  go.addEventListener('pointerdown',e=>{
    if(!gSrc.length) return;
    on=true; px=e.clientX; py=e.clientY;
    go.setPointerCapture(e.pointerId); go.style.cursor='grabbing';
  });
  go.addEventListener('pointermove',e=>{
    if(!on) return;
    const f=gSrc[0], r=go.getBoundingClientRect();
    const perCss=f.W/r.width;                       // source pixels per CSS pixel
    const pl=placement(f.W,f.H);
    const sx=(e.clientX-px)*perCss, sy=(e.clientY-py)*perCss;
    px=e.clientX; py=e.clientY;
    const spanX=VW-pl.w, spanY=VH-pl.h;             // 0 when the axis cannot move
    if(Math.abs(spanX)>0.5) gOx=clamp(gOx-sx*pl.k/spanX);
    if(Math.abs(spanY)>0.5) gOy=clamp(gOy-sy*pl.k/spanY);
    render();
  });
  const up=e=>{ on=false; go.style.cursor='grab'; };
  go.addEventListener('pointerup',up);
  go.addEventListener('pointercancel',up);
}

// Clip thumbnails: the board sends the first frame at half resolution as raw
// RGB565. Loaded only when the row scrolls into view, and one at a time, so a
// card full of clips does not flood a single-core web server.
const thumbQueue=[];
let thumbBusy=false;

async function thumbNext(){
  if(thumbBusy||!thumbQueue.length) return;
  thumbBusy=true;
  const cv=thumbQueue.shift();
  try{
    const r=await fetch('/thumb?f='+encodeURIComponent(cv.dataset.f));
    if(r.ok){
      const px=new Uint16Array(await r.arrayBuffer());
      const img=new ImageData(cv.width,cv.height);
      const d=img.data;
      for(let i=0,o=0;i<px.length;i++){
        const v=px[i];
        d[o++]=(v>>11)<<3; d[o++]=((v>>5)&63)<<2; d[o++]=(v&31)<<3; d[o++]=255;
      }
      cv.getContext('2d').putImageData(img,0,0);
    }
  }catch(e){ /* a missing thumbnail just stays black */ }
  thumbBusy=false;
  thumbNext();
}

function thumbsSetup(){
  const list=[...document.querySelectorAll('canvas.ft')];
  if(!list.length) return;
  const pending=new Set(list);
  const want=c=>{ if(!pending.has(c)) return; pending.delete(c); thumbQueue.push(c); thumbNext(); };
  if('IntersectionObserver' in window){
    const io=new IntersectionObserver(es=>{
      for(const e of es) if(e.isIntersecting){ io.unobserve(e.target); want(e.target); }
    },{rootMargin:'200px'});
    list.forEach(c=>io.observe(c));
  }
  // Safety net: visible rows go first, but everything else is fetched anyway a
  // few seconds later. Without this a list that starts off-screen - a hidden
  // tab, a short window - would stay blank until someone scrolled.
  setTimeout(()=>list.forEach(want), 3000);
}

window.addEventListener('DOMContentLoaded',()=>{
  $('gf').addEventListener('change',prepare);
  $('gc').addEventListener('input',render);
  dragSetup();
  thumbsSetup();
});
</script>)JS";

// ---------------------------------------------------------------- videos
// /files: what is on the TF card, upload a clip, start or delete one.
// Clips are raw RGB565, VIDEO_W x VIDEO_H per frame, frames back to back.
// Cycling: the file names are taken once when cycling starts, so the card is
// not listed again between clips.
static std::vector<String> g_cycle;
static size_t g_cycle_i = 0;
static uint32_t g_cycle_until = 0;

static void cycle_reload() {
  g_cycle.clear();
  for (const VideoFile &v : player.list()) g_cycle.push_back(v.name);
  g_cycle_i = 0;
  g_cycle_until = 0;
}

// Starts the next clip. S.video is updated but not saved: writing to flash
// every few seconds would wear it out.
static void cycle_step() {
  if (g_cycle.empty()) cycle_reload();
  if (g_cycle.empty()) { g_cycle_until = millis() + 5000; return; }
  const String name = g_cycle[g_cycle_i];
  g_cycle_i = (g_cycle_i + 1) % g_cycle.size();
  if (player.play(name, S.frame_ms)) S.video = name;
  g_cycle_until = millis() + static_cast<uint32_t>(S.cycle_s) * 1000;
}

static File g_upload;
static String g_upload_msg;
static String g_upload_name;   // set once a clip has landed, so it can start at once

static String human_size(uint32_t b) {
  char t[24];
  if (b >= 1024u * 1024u) snprintf(t, sizeof(t), "%.1f МБ", b / 1048576.0);
  else snprintf(t, sizeof(t), "%.1f КБ", b / 1024.0);
  return String(t);
}

// /thumb?f=NAME: the first frame of a clip at half resolution, raw RGB565.
// Streamed a row at a time, so a 384x64 clip needs a 2 KB buffer and not a
// whole frame, and the browser gets 12 KB instead of 48.
static void handle_thumb() {
  const String f = server.arg("f");
  if (!player.ok() || f.isEmpty() || f.indexOf('/') >= 0) {
    server.send(404, "text/plain", "no file");
    return;
  }
  int w, h;
  video_dimensions(f, &w, &h);
  if (w <= 1 || h <= 1 || w > 1024) {
    server.send(404, "text/plain", "bad size");
    return;
  }
  File file = SD_MMC.open("/" + f, FILE_READ);
  if (!file) {
    server.send(404, "text/plain", "no file");
    return;
  }
  const int tw = w / 2, th = h / 2;
  static uint16_t row[1024];
  static uint16_t out[512];
  server.setContentLength(static_cast<size_t>(tw) * th * 2);
  server.send(200, "application/octet-stream", "");
  for (int y = 0; y < th; ++y) {
    if (!file.seek(static_cast<size_t>(2 * y) * w * 2)) break;
    if (file.read(reinterpret_cast<uint8_t *>(row), w * 2) != w * 2) break;
    for (int x = 0; x < tw; ++x) out[x] = row[2 * x];
    server.sendContent(reinterpret_cast<const char *>(out), static_cast<size_t>(tw) * 2);
    esp_task_wdt_reset();
  }
  file.close();
}

static void handle_files() {
  String h;
  h.reserve(4000);
  h += FPSTR(kStyle);
  h += "<h1>Ролики на карте</h1>";

  if (!g_upload_msg.isEmpty()) {
    h += "<div class=c>" + g_upload_msg + "</div>";
    g_upload_msg = "";
  }

  if (!player.ok()) {
    h += "<div class=c><h2>Карта</h2><p>Недоступна: " + String(player.error()) +
         "</p><a class=b href='/?remount=1'>Подключить заново</a></div>";
  } else {
    h += "<div class=c><h2>Сейчас</h2><table>";
    h += "<tr><td>Карта</td><td>" + String((unsigned long)player.card_mb()) + " МБ</td></tr>";
    h += "<tr><td>Модулей</td><td>" + String(S.panels) + "</td></tr>";
    h += "<tr><td>Панель</td><td>" + String(display.width()) + "×" + String(display.height()) + "</td></tr>";
    h += "<tr><td>На экране</td><td>" +
         (player.playing() ? html_escape(player.current()) + ", кадр " + String(player.frame_index() + 1) +
                                 " из " + String(player.frames())
                           : String("часы")) +
         "</td></tr>";
    h += "<tr><td>Режим</td><td>" +
         String(S.cycle ? "по кругу, " + String(S.cycle_s) + " с на ролик" : "один ролик") +
         "</td></tr></table>";
    if (player.playing()) h += "<a class='b s' href=/stop>Показать часы</a>";
    h += "</div>";

    h += "<div class=c><h2>Ролики</h2>";
    const std::vector<VideoFile> files = player.list();
    if (files.empty()) {
      h += "<p>Роликов пока нет — загрузите файл ниже.</p>";
    } else {
      for (const VideoFile &v : files) {
        const bool now = player.playing() && player.current() == v.name;
        h += "<div class='f" + String(now ? " on" : "") + "' onclick=\"location.href='/play?f=" +
             v.name + "'\"><canvas class=ft data-f='" + v.name + "' width=" + String(v.w / 2) +
             " height=" + String(v.h / 2) + "></canvas>"
             "<div class=fn>" + String(now ? "▶ " : "") + html_escape(v.name) + "</div>"
             "<div class=fm>" + human_size(v.size) + " · " + String(v.w) + "×" + String(v.h) + " · " +
             String(v.frames) + " кадр.</div>"
             "<a class='b s' href='/delete?f=" + v.name +
             "' onclick=\"event.stopPropagation();return confirm('Удалить " + v.name +
             "?')\">Удалить</a></div>";
      }
    }
    h += "</div>";

    h += "<div class=c><h2>Воспроизведение</h2><form method=post action=/speed>";
    h += "<label>Скорость, мс на кадр</label><input name=ms type=number min=10 max=2000 value=" +
         String(S.frame_ms) + ">";
    h += String("<label><input type=checkbox name=autoplay value=1 style='width:auto'") +
         (S.autoplay ? " checked" : "") + "> Включать ролик автоматически после заставки</label>";
    h += String("<label><input type=checkbox name=cycle value=1 style='width:auto'") +
         (S.cycle ? " checked" : "") + "> Крутить все ролики по кругу</label>";
    h += "<label>Секунд на ролик</label><input name=cycle_s type=number min=1 max=600 value=" +
         String(S.cycle_s) + ">";
    h += "<button>Сохранить</button></form></div>";

    h += F("<div class=c><h2>Из GIF — с предпросмотром</h2>"
           "<p style='color:#888'>Любая гифка: браузер вырежет полосу под экран и переведёт в RGB565. "
           "Рамку можно двигать по исходнику. Плата ничего не декодирует.</p>"
           "<input type=file id=gf accept='image/gif,image/*'>"
           "<label><input type=checkbox id=gc checked style='width:auto'> Заполнить экран и обрезать лишнее "
           "(иначе вписать целиком с чёрными полями)</label>"
           "<div id=gw style='display:none'>"
           "<label>Рамка на исходнике — тяните мышью или пальцем</label>"
           "<canvas id=go style='width:100%;background:#000;border-radius:6px;cursor:grab;"
           "touch-action:none'></canvas>"
           "<label>Что попадёт на панель</label>"
           "<canvas id=gp style='width:100%;background:#000;border-radius:6px;image-rendering:pixelated'></canvas>"
           "<p id=gi style='color:#888'></p>"
           "<button id=gb onclick='upload()'>Загрузить на карту</button>"
           "</div><p id=gs style='color:#888'></p></div>");

    h += F("<div class=c><h2>Загрузить готовый .bin</h2>"
           "<form method=post action=/upload enctype='multipart/form-data'>"
           "<input type=file name=f accept='.bin'><button>Загрузить</button></form>"
           "<p style='color:#888'>Сырой RGB565, кадры подряд, без заголовка.</p></div>");

    h += "<script>window.VW=" + String(display.width()) + ";window.VH=" + String(display.height()) + ";</script>";
    h += FPSTR(kGifJs);
  }

  h += "<div class=c><h2>Сервис</h2>"
       "<a class=b href=/setup>Настройки</a>"
       "<a class='b s' href=/update>Обновить прошивку</a>"
       "<a class='b s' href=/status.json>status.json</a></div>";
  server.send(200, "text/html; charset=utf-8", h);
}

static void handle_files_page() {
  if (server.hasArg("remount")) player.begin();
  handle_files();
}

static void handle_play() {
  if (!require_auth()) return;
  const String f = server.arg("f");
  S.cycle = false;  // choosing a clip by hand stops the round robin
  if (player.play(f, S.frame_ms)) {
    S.video = f;
    save_settings();
    g_upload_msg = "Играет " + f;
  } else {
    g_upload_msg = "Не удалось включить " + f + ": " + player.error();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_stop() {
  player.stop();
  S.cycle = false;
  S.video = "";
  save_settings();
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_delete() {
  if (!require_auth()) return;
  const String f = server.arg("f");
  g_upload_msg = player.remove(f) ? ("Удалён " + f) : ("Не удалось удалить " + f);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handle_speed() {
  const bool was_cycle = S.cycle;
  S.frame_ms = constrain(server.arg("ms").toInt(), 10, 2000);
  S.autoplay = server.arg("autoplay") == "1";
  S.cycle = server.arg("cycle") == "1";
  S.cycle_s = constrain(server.arg("cycle_s").toInt(), 1, 600);
  save_settings();
  if (S.cycle && !was_cycle) {
    cycle_reload();
    cycle_step();  // start the round robin at once, do not wait for a tick
  } else if (player.playing()) {
    player.play(player.current(), S.frame_ms);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// streamed to the card so a clip larger than RAM can be uploaded
static void handle_upload_data() {
  if (!require_auth()) return;
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_upload_name = "";
    String name = up.filename;
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.isEmpty()) return;
    if (player.current() == name) player.stop();  // do not overwrite the open file
    SD_MMC.remove("/" + name);
    g_upload = SD_MMC.open("/" + name, FILE_WRITE);
    if (!g_upload) g_upload_msg = "Не удалось открыть файл на карте";
    else g_upload_name = name;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_upload) g_upload.write(up.buf, up.currentSize);
    esp_task_wdt_reset();  // a big clip takes longer than the watchdog timeout
  } else if (up.status == UPLOAD_FILE_END) {
    if (g_upload) {
      g_upload.close();
      // size() on a file still open for writing reads back as 0
      const uint32_t n = up.totalSize;
      g_upload_msg = "Загружено " + up.filename + ", " + human_size(n) + ", кадров " +
                     String(n / (static_cast<uint32_t>(VIDEO_W) * VIDEO_H * 2));
    }
  }
}

static void handle_upload_done() {
  // a freshly uploaded clip goes on the panel right away
  if (!g_upload_name.isEmpty()) {
    if (player.play(g_upload_name, S.frame_ms)) {
      S.video = g_upload_name;
      save_settings();
      g_upload_msg += " Играет.";
    } else {
      g_upload_msg += " Включить не удалось: " + String(player.error()) + ".";
    }
    g_upload_name = "";
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---------------------------------------------------------------- wiring test
// /pintest: stops the panel output and drives one HUB75 line high (others low)
// as a plain GPIO, so every line can be checked with a multimeter on the HUB75
// connector. /pintest?exit=1 reboots back to normal operation.
static const char *kSignalNames[14] = {"R1", "G1", "B1", "R2", "G2", "B2", "A", "B",
                                       "C",  "D",  "E",  "LAT", "OE", "CLK"};
static const int kHub75Pin[14] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 12, 8, 14, 15, 13};
static bool g_pintest = false;

static int8_t signal_gpio(int i) {
  const int8_t g[14] = {kPins.r1, kPins.g1, kPins.b1, kPins.r2, kPins.g2, kPins.b2, kPins.a,
                        kPins.b,  kPins.c,  kPins.d,  kPins.e,  kPins.lat, kPins.oe, kPins.clk};
  return g[i];
}

// No password here on purpose: the wiring test must be reachable while the
// panels are dark and nothing else works.
static void handle_pintest() {
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
  String h = FPSTR(kStyle);
  h += F("<h1>Проверка линий HUB75</h1><div class=c>"
         "<p>Вывод на панели остановлен. Выбранная линия = 1, остальные = 0. Мерьте мультиметром "
         "между выводом разъёма HUB75 и GND (выводы 4 или 16): 1 ≈ 5 В, 0 ≈ 0 В.</p>"
         "<table>");
  for (int i = 0; i < 14; ++i) {
    h += "<tr><td style='padding:4px 10px'><a href='/pintest?s=" + String(i) + "'><b>" + kSignalNames[i] +
         "</b></a></td><td style='padding:4px 10px'>GPIO " + String(signal_gpio(i)) +
         "</td><td style='padding:4px 10px'>HUB75 вывод " + String(kHub75Pin[i]) + "</td><td>" +
         (sel == i ? "◀ сейчас = 1" : "") + "</td></tr>";
  }
  h += "</table></div><div class=c><h2>Все линии сразу</h2>"
       "<a class=b href='/pintest?s=-1'>Все = 0</a><a class=b href='/pintest?s=14'>Все = 1</a>" +
       String(sel == 14 ? "<p>сейчас все = 1</p>" : sel == -1 ? "<p>сейчас все = 0</p>" : "") +
       "</div><div class=c><a class='b s' href='/pintest?exit=1'>Выйти и перезагрузить</a></div>";
  server.send(200, "text/html; charset=utf-8", h);
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

  // Wi-Fi first: it connects while the panels' power settles
  wifi_begin();

  display.setCurrentGain(static_cast<uint8_t>(S.gain));
  for (uint32_t t0 = millis(); millis() - t0 < STARTUP_DELAY_MS;) { esp_task_wdt_reset(); delay(50); }
  display_ok = display.begin(kPins, S.panels, PCLK_HZ, FRAMES_PER_INIT);
  if (!display_ok) {
    Serial.printf("display init failed (%s), rebooting in 5 s\n", display.error());
    delay(5000);
    ESP.restart();
  }
  display.setCurrentGain(static_cast<uint8_t>(S.gain));
  display.setCurrentBoost(S.boost);
  display.setBrightness(S.brightness);
  Serial.printf("display: %dx%d, %u frames/s\n", display.width(), display.height(), display.frameRate());

  canvas = new GFXcanvas16(display.width(), display.height());
#if DISPLAY_ROTATE_180
  canvas->setRotation(2);
#endif
  screens_begin(*canvas);

  if (player.begin()) {
    Serial.printf("[sd] card %llu MB\n", player.card_mb());
    if (S.autoplay && S.cycle) cycle_reload();
    else if (S.autoplay && !S.video.isEmpty() && !player.play(S.video, S.frame_ms))
      Serial.printf("[sd] cannot start '%s': %s\n", S.video.c_str(), player.error());
  } else {
    Serial.printf("[sd] %s\n", player.error());
  }

  if (rtc.begin()) {
    struct tm t;
    if (rtc.read(&t)) {
      // hand the RTC time to the system clock, so the panel is right at once
      time_t epoch = mktime(&t);
      struct timeval tv = {epoch, 0};
      settimeofday(&tv, nullptr);
      Serial.printf("[rtc] %04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      Serial.println("[rtc] present but has no valid time yet");
    }
  } else {
    Serial.println("[rtc] not found on the I2C bus");
  }

  sntp_set_time_sync_notification_cb([](struct timeval *) { g_ntp_fresh = true; });
  sntp_set_sync_interval(NTP_SYNC_INTERVAL_S * 1000UL);
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);


  // the player is the home page; the settings moved to /setup
  server.on("/", HTTP_GET, handle_files_page);
  server.on("/setup", HTTP_GET, handle_root);
  server.on("/wifi", HTTP_POST, handle_wifi);
  server.on("/scan", HTTP_POST, handle_scan);
  server.on("/settings", HTTP_POST, handle_settings);
  server.on("/status.json", HTTP_GET, handle_status);
  server.on("/update", HTTP_GET, handle_update_page);
  server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  server.on("/pintest", HTTP_GET, handle_pintest);
  server.on("/files", HTTP_GET, []() {  // old address, kept for bookmarks
    server.sendHeader("Location", "/");
    server.send(301);
  });
  server.on("/play", HTTP_GET, handle_play);
  server.on("/stop", HTTP_GET, handle_stop);
  server.on("/delete", HTTP_GET, handle_delete);
  server.on("/speed", HTTP_POST, handle_speed);
  server.on("/thumb", HTTP_GET, handle_thumb);
  server.on("/upload", HTTP_POST, handle_upload_done, handle_upload_data);
  server.onNotFound(handle_not_found);
  server.begin();
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  wifi_loop();

  // status line every 5 s on the serial log
  static uint32_t last_log = 0;
  if (millis() - last_log > 5000) {
    last_log = millis();
    Serial.printf("[status] display=%s err='%s' %dx%d %u fps | psram free=%u | wifi=%d '%s' ip=%s | ap=%d\n",
                  display_ok ? "ok" : "FAIL", display.error(), display.width(), display.height(),
                  display.frameRate(), ESP.getFreePsram(), WiFi.status(), S.wifi_ssid.c_str(),
                  WiFi.localIP().toString().c_str(), g_ap_on);
  }

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

  // NTP synced (at boot and then once an hour): store it in the RTC, so the
  // time survives a reboot without a network.
  if (g_ntp_fresh) {
    g_ntp_fresh = false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (rtc.write(t))
      Serial.printf("[rtc] synced from NTP: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    else if (rtc.ok())
      Serial.println("[rtc] write failed");
  }

  // The moment the station joins, put the address on the panel for a couple of
  // seconds: that is the only place to read it without the serial log.
  static bool was_connected = false;
  static uint32_t ip_until = 0;
  const bool connected_now = wifi_connected();
  if (connected_now && !was_connected && !g_pintest && canvas) {
    ip_until = millis() + IP_SCREEN_MS;
    screens_draw_ip(*canvas, WiFi.localIP().toString().c_str());
    display.show(canvas->getBuffer());
    g_shown = "";  // whatever was on the panel has to be drawn again afterwards
  }
  was_connected = connected_now;
  if (millis() < ip_until) { delay(5); return; }

  // For the first SPLASH_MS after boot the panel shows the connection screen,
  // so a player with no network still says what it is doing and where to reach
  // it. After that: the clip if one is playing, otherwise the clock.
  const bool splash = millis() < SPLASH_MS;

  // round robin: hand the panel to the next clip when this one's time is up
  if (S.cycle && !g_pintest && !splash && millis() >= g_cycle_until) cycle_step();

  // a clip takes over the panel; the clock is what runs when nothing plays
  if (!g_pintest && !splash && player.playing()) {
    if (player.next_frame(canvas->getBuffer(), display.width(), display.height(), DISPLAY_ROTATE_180))
      display.show(canvas->getBuffer());
    delay(1);
    return;
  }

  // redraw the screen when anything on it changes (checked twice a second)
  static uint32_t last_draw_check = 0;
  if (!g_pintest && millis() - last_draw_check > 500) {
    last_draw_check = millis();
    struct tm t;
    char key[200];
    if (!splash && time_valid(&t)) {
      // the colon blinks: half-second resolution is enough for the key
      const bool colon = (millis() / 500) % 2 == 0;
      snprintf(key, sizeof(key), "clock|%02d:%02d|%d|%d", t.tm_hour, t.tm_min, t.tm_mday, colon);
      if (g_shown != key) {
        g_shown = key;
        screens_draw_clock(*canvas, t, colon);
        display.show(canvas->getBuffer());
      }
    } else {
      const StatusData d = status_data();
      snprintf(key, sizeof(key), "%d|%s|%s|%d|%d|%d|%s|%s", d.wifi, d.ssid, d.ip, d.rssi, d.connecting_s,
               d.ap_on, d.ap_ssid, d.ap_ip);
      if (g_shown != key) {
        g_shown = key;
        screens_draw_status(*canvas, d);
        display.show(canvas->getBuffer());
      }
    }
  }
  delay(5);
}
