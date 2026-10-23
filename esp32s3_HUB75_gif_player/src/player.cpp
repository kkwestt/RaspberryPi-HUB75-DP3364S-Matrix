#include "player.h"

#include <FS.h>
#include <SD_MMC.h>

#include "config.h"

namespace {
// The frame buffer is allocated for the largest clip we support, so a file of
// any width can be opened without reallocating.
constexpr size_t kMaxFrameBytes = static_cast<size_t>(128) * 8 * 64 * 2;

bool is_video(const String &name) { return name.endsWith(".bin") || name.endsWith(".BIN"); }
}  // namespace

void video_dimensions(const String &name, int *w, int *h) {
  *w = VIDEO_W;
  *h = VIDEO_H;
  // "<anything>_<W>x<H>_<anything>.bin": find the digits around the 'x'
  for (int i = 1; i < static_cast<int>(name.length()) - 1; ++i) {
    if (name[i] != 'x' && name[i] != 'X') continue;
    int a = i - 1, b = i + 1;
    while (a >= 0 && isdigit(name[a])) --a;
    while (b < static_cast<int>(name.length()) && isdigit(name[b])) ++b;
    if (a == i - 1 || b == i + 1) continue;  // digits on both sides required
    const int pw = name.substring(a + 1, i).toInt();
    const int ph = name.substring(i + 1, b).toInt();
    if (pw > 0 && ph > 0 && static_cast<size_t>(pw) * ph * 2 <= kMaxFrameBytes) {
      *w = pw;
      *h = ph;
      return;
    }
  }
}

bool Player::begin() {
  stop();
  SD_MMC.end();
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  // 1-bit bus: D1..D3 are not routed on this board. No auto-format: a card that
  // will not mount must not be wiped.
  if (!SD_MMC.begin("/sdcard", true, false)) {
    sd_ok_ = false;
    err_ = "card not mounted";
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    sd_ok_ = false;
    err_ = "no card";
    return false;
  }
  card_mb_ = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  if (!frame_buf_) frame_buf_ = static_cast<uint16_t *>(ps_malloc(kMaxFrameBytes));
  if (!frame_buf_) {
    sd_ok_ = false;
    err_ = "no PSRAM for the frame buffer";
    return false;
  }
  sd_ok_ = true;
  err_ = "";
  return true;
}

std::vector<VideoFile> Player::list() {
  std::vector<VideoFile> out;
  if (!sd_ok_) return out;
  File root = SD_MMC.open("/");
  if (!root) return out;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = f.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!f.isDirectory() && is_video(name)) {
      VideoFile v;
      v.name = name;
      v.size = f.size();
      video_dimensions(name, &v.w, &v.h);
      v.frames = f.size() / (static_cast<size_t>(v.w) * v.h * 2);
      out.push_back(v);
    }
    f.close();
  }
  root.close();
  return out;
}

bool Player::remove(const String &name) {
  if (!sd_ok_) return false;
  if (current_ == name) stop();
  return SD_MMC.remove("/" + name);
}

void Player::stop() {
  if (file_) {
    static_cast<File *>(file_)->close();
    delete static_cast<File *>(file_);
    file_ = nullptr;
  }
  current_ = "";
  frames_ = 0;
  frame_ = 0;
}

bool Player::play(const String &name, uint32_t frame_ms) {
  stop();
  if (!sd_ok_ || name.isEmpty()) return false;
  File f = SD_MMC.open("/" + name, FILE_READ);
  if (!f) {
    err_ = "cannot open the file";
    return false;
  }
  int vw, vh;
  video_dimensions(name, &vw, &vh);
  const uint32_t frames = f.size() / (static_cast<size_t>(vw) * vh * 2);
  if (frames == 0) {
    f.close();
    err_ = "file is shorter than one frame";
    return false;
  }
  file_ = new File(f);
  current_ = name;
  vw_ = vw;
  vh_ = vh;
  frames_ = frames;
  frame_ = 0;
  frame_ms_ = frame_ms ? frame_ms : 100;
  last_frame_at_ = 0;
  err_ = "";
  return true;
}

bool Player::next_frame(uint16_t *dst, int dst_w, int dst_h, bool rotate180) {
  if (!playing() || !frame_buf_ || !dst) return false;
  const uint32_t now = millis();
  if (last_frame_at_ && now - last_frame_at_ < frame_ms_) return false;
  last_frame_at_ = now;

  const size_t frame_bytes = static_cast<size_t>(vw_) * vh_ * 2;
  File *f = static_cast<File *>(file_);
  if (!f->seek(static_cast<size_t>(frame_) * frame_bytes)) return false;
  if (f->read(reinterpret_cast<uint8_t *>(frame_buf_), frame_bytes) != static_cast<int>(frame_bytes))
    return false;
  frame_ = (frame_ + 1) % frames_;

  // Centre the frame on the panel: a 384 px video on one panel shows its
  // middle third rather than a stretched or truncated picture.
  const int off_x = (vw_ - dst_w) / 2;
  const int off_y = (vh_ - dst_h) / 2;
  for (int y = 0; y < dst_h; ++y) {
    const int sy = y + off_y;
    for (int x = 0; x < dst_w; ++x) {
      const int sx = x + off_x;
      const uint16_t px = (sx < 0 || sx >= vw_ || sy < 0 || sy >= vh_)
                              ? 0
                              : frame_buf_[static_cast<size_t>(sy) * vw_ + sx];
      const int dx = rotate180 ? dst_w - 1 - x : x;
      const int dy = rotate180 ? dst_h - 1 - y : y;
      dst[static_cast<size_t>(dy) * dst_w + dx] = px;
    }
  }
  return true;
}
