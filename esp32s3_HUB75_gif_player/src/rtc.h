// PCF85063 real-time clock on the board's I2C bus.
//
// NTP is only reachable when the player has a network, so the clock is kept in
// this chip: it is written whenever NTP reports a fresh sync (once an hour by
// NTP_SYNC_INTERVAL_S) and read back whenever the time is needed, which is
// what keeps the panel right after a reboot without Wi-Fi.
#pragma once

#include <Arduino.h>
#include <time.h>

class Rtc {
 public:
  // Starts the I2C bus and checks that the chip answers.
  bool begin();
  bool ok() const { return ok_; }

  // True when the oscillator stopped since the last write, i.e. the chip has
  // no trustworthy time (fresh board, dead backup cell).
  bool lost_power() const { return lost_; }

  // Reads the current time. False when the chip is missing or its time is not
  // trustworthy.
  bool read(struct tm *out);

  // Stores the time and clears the oscillator-stop flag.
  bool write(const struct tm &t);

 private:
  bool ok_ = false;
  bool lost_ = true;
};
