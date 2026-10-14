// Build-time configuration. Wi-Fi credentials go to include/secrets.h
// (copy include/secrets.h.example). Everything marked "web" can also be
// changed at runtime on the device's web page and is stored in flash.
#pragma once

// ---- panels ----
#define PANELS 2                 // 128x64 DP3364S panels in one chain
#define PCLK_HZ 8000000          // HUB75 CLK; frames/s = PCLK / ~140k (2 panels)
#define STARTUP_DELAY_MS 3000    // wait for panel power before starting the output
#define FRAMES_PER_INIT 16       // rewrite the driver registers every N frames

#if defined(BOARD_DEVKIT_N16R8)
// ---- HUB75 pins: ESP32-S3-DevKitC-1 N16R8 (all on the left header) ----
// Avoids strapping pins (0, 3, 45, 46), USB (19, 20), flash/octal PSRAM
// (26-37) and the UART console (43, 44). Pass through a 74HCT245 to 5 V.
#define PIN_R1 4
#define PIN_G1 5
#define PIN_B1 6
#define PIN_R2 7
#define PIN_G2 15
#define PIN_B2 16
#define PIN_A 17
#define PIN_B 18
#define PIN_C 8
#define PIN_D 9
#define PIN_E 10
#define PIN_LAT 11
#define PIN_OE 12
#define PIN_CLK 13
#else
// ---- HUB75 pins: Adafruit MatrixPortal S3 (on-board HUB75 connector) ----
#define PIN_R1 42
#define PIN_G1 41
#define PIN_B1 40
#define PIN_R2 38
#define PIN_G2 39
#define PIN_B2 37
#define PIN_A 45
#define PIN_B 36
#define PIN_C 48
#define PIN_D 35
#define PIN_E 21
#define PIN_LAT 47
#define PIN_OE 14
#define PIN_CLK 2
#endif

// ---- time ----
#define TZ_INFO "MSK-3"          // POSIX TZ string (Moscow, no DST)
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define NTP_SYNC_INTERVAL_S 3600 // re-sync with NTP every hour

// ---- data source: the stand counter on the Raspberry Pi ----
#define COUNTER_POLL_MS 2000

// WS2812 LED that lights up when the visitor count goes up
#ifndef PIN_WS2812
#define PIN_WS2812 21
#endif
#define DEFAULT_LED_SEC 15
#define DEFAULT_LED_COLOR 0x00FF00
#define VISITORS_STALE_MS 30000  // no reply this long -> keep last value, show a yellow dot
#define COUNTER_JSON_KEY "stopped_today"  // people who stood >= 10 s today

// ---- defaults for the web settings ----
#define DEFAULT_LABEL_COUNTER "ОСТАНОВИЛИСЬ"
#define DEFAULT_LABEL_COUNTDOWN "РАБОТАЕТ"
#define DEFAULT_BRIGHTNESS 100   // %, gamma-table scale
#define DEFAULT_CURRENT_GAIN 0xBF  // DP3364S REG08 (output current gain)

#define WDT_TIMEOUT_S 30           // reboot if the main loop hangs this long
#define WIFI_REBOOT_MS 900000      // reboot after 15 min without Wi-Fi

#define HOSTNAME "stand-display"   // http://stand-display.local
