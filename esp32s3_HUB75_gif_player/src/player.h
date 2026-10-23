// Video playback from the TF card: raw RGB565 files, VIDEO_W x VIDEO_H pixels
// per frame, frames stored back to back with no header (that is what
// tools/ produces and what gif/*.bin already are).
//
// The card is on SDMMC in 1-bit mode; D1..D3 are not wired on this board.
#pragma once

#include <Arduino.h>

#include <vector>

struct VideoFile {
  String name;
  uint32_t size = 0;
  uint32_t frames = 0;
  int w = 0;  // frame size, taken from the "..._384x64_rgb565.bin" name
  int h = 0;
};

// Frame size of a clip, read from its file name; falls back to VIDEO_W/VIDEO_H
// when the name carries no "<W>x<H>" part.
void video_dimensions(const String &name, int *w, int *h);

class Player {
 public:
  // Mounts the card. Safe to call again to re-mount after a card swap.
  bool begin();
  bool ok() const { return sd_ok_; }
  const char *error() const { return err_; }
  uint64_t card_mb() const { return card_mb_; }

  std::vector<VideoFile> list();
  bool remove(const String &name);

  // Starts a file from the beginning; empty name stops playback.
  bool play(const String &name, uint32_t frame_ms);
  void stop();
  bool playing() const { return file_ && frames_ > 0; }
  const String &current() const { return current_; }
  uint32_t frame_ms() const { return frame_ms_; }
  uint32_t frame_index() const { return frame_; }
  uint32_t frames() const { return frames_; }
  int video_w() const { return vw_; }
  int video_h() const { return vh_; }

  // Reads the next frame when its time has come and writes it into `dst`
  // (dst_w x dst_h RGB565, the display buffer), cropping or centring a frame
  // that does not match the panel. Returns true when dst was written.
  bool next_frame(uint16_t *dst, int dst_w, int dst_h, bool rotate180);

 private:
  bool sd_ok_ = false;
  const char *err_ = "";
  uint64_t card_mb_ = 0;
  void *file_ = nullptr;  // File*, kept opaque so the header stays light
  String current_;
  uint16_t *frame_buf_ = nullptr;  // one frame, in PSRAM
  uint32_t frames_ = 0;
  int vw_ = 0;
  int vh_ = 0;
  uint32_t frame_ = 0;
  uint32_t frame_ms_ = 100;
  uint32_t last_frame_at_ = 0;
};
