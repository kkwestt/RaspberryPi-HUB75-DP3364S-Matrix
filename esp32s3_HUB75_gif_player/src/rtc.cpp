#include "rtc.h"

#include <Wire.h>

#include "config.h"

namespace {
// PCF85063 registers
constexpr uint8_t REG_CTRL1 = 0x00;
constexpr uint8_t REG_SECONDS = 0x04;  // bit 7 = OS, the oscillator-stop flag

uint8_t from_bcd(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t to_bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

bool read_regs(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(RTC_ADDR), static_cast<int>(n)) != static_cast<int>(n)) return false;
  for (size_t i = 0; i < n; ++i) buf[i] = Wire.read();
  return true;
}
}  // namespace

bool Rtc::begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  uint8_t ctrl1 = 0;
  if (!read_regs(REG_CTRL1, &ctrl1, 1)) {
    ok_ = false;
    return false;
  }
  // 24-hour mode, normal run: clear STOP (bit 5) and 12_24 (bit 1)
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(REG_CTRL1);
  Wire.write(static_cast<uint8_t>(ctrl1 & ~0x22));
  ok_ = Wire.endTransmission() == 0;

  uint8_t sec = 0;
  if (ok_ && read_regs(REG_SECONDS, &sec, 1)) lost_ = (sec & 0x80) != 0;
  return ok_;
}

bool Rtc::read(struct tm *out) {
  if (!ok_ || !out) return false;
  uint8_t b[7];
  if (!read_regs(REG_SECONDS, b, sizeof(b))) return false;
  lost_ = (b[0] & 0x80) != 0;
  if (lost_) return false;

  memset(out, 0, sizeof(*out));
  out->tm_sec = from_bcd(b[0] & 0x7F);
  out->tm_min = from_bcd(b[1] & 0x7F);
  out->tm_hour = from_bcd(b[2] & 0x3F);
  out->tm_mday = from_bcd(b[3] & 0x3F);
  out->tm_wday = b[4] & 0x07;
  out->tm_mon = from_bcd(b[5] & 0x1F) - 1;
  out->tm_year = from_bcd(b[6]) + 100;  // the chip stores 00..99 from 2000
  out->tm_isdst = -1;
  // a chip that answers with nonsense is worse than one that says nothing
  if (out->tm_mon < 0 || out->tm_mon > 11 || out->tm_mday < 1 || out->tm_mday > 31 ||
      out->tm_hour > 23 || out->tm_min > 59 || out->tm_sec > 59)
    return false;
  return true;
}

bool Rtc::write(const struct tm &t) {
  if (!ok_) return false;
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(REG_SECONDS);
  Wire.write(to_bcd(t.tm_sec) & 0x7F);  // clears OS: the time is trustworthy again
  Wire.write(to_bcd(t.tm_min));
  Wire.write(to_bcd(t.tm_hour));
  Wire.write(to_bcd(t.tm_mday));
  Wire.write(static_cast<uint8_t>(t.tm_wday & 0x07));
  Wire.write(to_bcd(t.tm_mon + 1));
  Wire.write(to_bcd((t.tm_year + 1900) % 100));
  if (Wire.endTransmission() != 0) return false;
  lost_ = false;
  return true;
}
