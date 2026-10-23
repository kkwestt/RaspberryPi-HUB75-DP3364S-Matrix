// Build-time configuration. Default Wi-Fi credentials and passwords go to
// include/secrets.h (copy include/secrets.h.example). Wi-Fi, brightness and
// LED current can also be changed at runtime on the device's web page and are
// stored in flash.
#pragma once

// ---- panels ----
#ifndef PANELS
#define PANELS 3                 // 128x64 DP3364S panels in one chain (384x64)
#endif

// The panel's own orientation: with the HUB75 connector at the far end the
// image comes out upside down. 1 = turn the picture 180 degrees in software.
#ifndef DISPLAY_ROTATE_180
#define DISPLAY_ROTATE_180 1
#endif
#ifndef PCLK_HZ
#define PCLK_HZ 8000000          // HUB75 CLK; frames/s = PCLK / ~205k words (3 panels)
#endif
#define STARTUP_DELAY_MS 3000    // wait for panel power before starting the output
#define FRAMES_PER_INIT 16       // rewrite the driver registers every N frames

// ---- Waveshare ESP32-S3-RGB-Matrix (ESP32-S3-WROOM-2-N32R16V) ----
// Pin map from the board schematic; the full table is in README.md.
// HUB75 goes through two on-board SN74HC245 buffers to the 2x8 connector.
#define PIN_R1 4
#define PIN_G1 5
#define PIN_B1 6
#define PIN_R2 7
#define PIN_G2 15
#define PIN_B2 16
#define PIN_A 18
#define PIN_B 8
#define PIN_C 3
#define PIN_D 42
#define PIN_E 9
#define PIN_LAT 40
#define PIN_OE 2
#define PIN_CLK 41

// TF card: SDMMC, 1-bit bus only (D1..D3 are not wired)
#define PIN_SD_CLK 1
#define PIN_SD_CMD 44
#define PIN_SD_D0 17

// 4-pin GPIO header U8: 1 = IO45, 2 = IO46, 3 = 3V3, 4 = GND.
// DMX512 input from an RS-485 receiver (MAX485 / isolated module), UART RX only.
#define PIN_DMX_RX 45

// I2C bus shared by the RTC (PCF85063 at 0x51), IMU, codec and SHTC3.
#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define RTC_ADDR 0x51

#define PIN_BOOT_BUTTON 0

// ---- video ----
// Raw RGB565 clips on the TF card: this many pixels per frame, frames back to
// back, no header. The files in gif/ are made for three panels in a row.
#define VIDEO_W 384
#define VIDEO_H 64
#define DEFAULT_FRAME_MS 100     // playback speed when the file has no setting
#define SPLASH_MS 10000          // show the connection screen this long after boot
#define IP_SCREEN_MS 2000        // show the address this long right after joining Wi-Fi

// ---- time ----
#define TZ_INFO "MSK-3"          // POSIX TZ string (Moscow, no DST)
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define NTP_SYNC_INTERVAL_S 3600 // re-sync with NTP every hour

// ---- Wi-Fi ----
// The web page asks for no password at all. Anyone who can reach the device
// can then flash firmware, delete clips and stop the output, so turn this back
// on (1) if the player ever leaves a trusted network.
#define WEB_PASSWORD 0

#define HOSTNAME "led-player"            // http://led-player.local
#define AP_SSID "ESP32S3+HUB75"          // open setup network, no password
#define WIFI_AP_FALLBACK_MS 20000        // no connection this long -> also start the setup AP
#define WIFI_RETRY_AP_MS 120000          // with the setup AP up, retry the saved network this often
#define WIFI_AP_OFF_DELAY_MS 60000       // connected this long -> switch the setup AP off

// ---- defaults for the web settings ----
#define DEFAULT_BRIGHTNESS 100   // %, gamma-table scale
#define DEFAULT_CURRENT_GAIN 0xBF  // DP3364S REG08 (output current gain)

#define WDT_TIMEOUT_S 30           // reboot if the main loop hangs this long
