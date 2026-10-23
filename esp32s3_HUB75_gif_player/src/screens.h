// Screens drawn into one RGB565 canvas (PANELS*128 x 64).
#pragma once

#include <Adafruit_GFX.h>
#include <time.h>

enum WifiState {
  WIFI_NOT_SET,     // no network saved: only the setup access point
  WIFI_CONNECTING,  // trying to join `ssid`
  WIFI_CONNECTED,
};

struct StatusData {
  WifiState wifi = WIFI_NOT_SET;
  const char *ssid = "";      // network being joined / joined
  const char *ip = "";        // station IP when connected
  int rssi = 0;               // dBm when connected
  int connecting_s = 0;       // seconds since the connection attempt started
  bool ap_on = false;         // setup access point is running
  const char *ap_ssid = "";
  const char *ap_ip = "";
};

void screens_begin(GFXcanvas16 &canvas);

// Wi-Fi status: state, network name, address to open the web page at.
void screens_draw_status(GFXcanvas16 &canvas, const StatusData &d);

// Just joined the network: the address to open the web page at.
void screens_draw_ip(GFXcanvas16 &canvas, const char *ip);

// Clock: big HH:MM with a colon that blinks once a second, date underneath.
void screens_draw_clock(GFXcanvas16 &canvas, const struct tm &t, bool colon_on);
