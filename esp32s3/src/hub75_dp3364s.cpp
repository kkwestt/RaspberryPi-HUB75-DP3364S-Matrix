#include "hub75_dp3364s.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"
#include "hal/dma_types.h"
#include "hal/gpio_hal.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/lcd_cam_struct.h"
#include "soc/gdma_struct.h"
#include <stdio.h>
#include "esp_private/gdma.h"
#include "rom/ets_sys.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_private/periph_ctrl.h"
#include "esp_cache.h"
#else
#include "driver/periph_ctrl.h"
#include "esp32s3/rom/cache.h"
#endif

namespace {

constexpr size_t kPsramAlign = 64;         // GDMA external-memory block size
constexpr size_t kChunkPsram = 4032;       // <= 4095, multiple of 64
constexpr size_t kChunkSram = 4092;        // <= 4095, multiple of 4

gdma_channel_handle_t g_dma = nullptr;

size_t chunks_for(size_t bytes, size_t chunk) { return (bytes + chunk - 1) / chunk; }

void fill_desc(void *buf, size_t bytes, size_t chunk, dma_descriptor_t *&next_free) {
  uint8_t *p = static_cast<uint8_t *>(buf);
  while (bytes) {
    const size_t n = bytes > chunk ? chunk : bytes;
    dma_descriptor_t *cur = next_free++;
    cur->dw0.size = n;
    cur->dw0.length = n;
    cur->dw0.suc_eof = 0;  // never end: the LCD runs in 'always out' mode
    cur->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    cur->buffer = p;
    cur->next = next_free;  // fixed up by the caller for the last one
    p += n;
    bytes -= n;
  }
}

void route_pin(int pin, int signal) {
  if (pin < 0) return;
  gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
  gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
  gpio_set_drive_capability(static_cast<gpio_num_t>(pin), GPIO_DRIVE_CAP_3);
  esp_rom_gpio_connect_out_signal(pin, signal, false, false);
  // (input readback disabled)
}

}  // namespace

void Dp3364sDisplay::writeBackCache(void *p, size_t bytes) {
  if (!frame_in_psram_ || p != frame_buf_) return;  // internal RAM is not cached
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_cache_msync(p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#else
  Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(p), bytes);
#endif
}

// SDR command (DP3364S datasheet 10.1/10.3): LAT high for 15 clocks puts the
// driver into single-edge clock mode. After power-on the mode is undefined, so
// it is sent before every register refresh; in SDR mode it changes nothing.
constexpr size_t kSdrWords = 0;  // SDR command disabled: it blanked the panels

static size_t build_sdr(uint16_t *out) {
  size_t n = 0;
  for (int i = 0; i < 3; ++i) out[n++] = 0;
  for (int i = 0; i < 15; ++i) out[n++] = dp3364s::BIT_LAT;
  for (int i = 0; i < 3; ++i) out[n++] = 0;
  return n;
}

void Dp3364sDisplay::buildRegisters() {
  size_t n = 0;  // no SDR prefix
  for (int r = 0; r < dp3364s::DEFAULT_REGISTER_COUNT; ++r)
    n += dp3364s::build_register_block(regs_buf_ + n, layout_, registers_[r]);
}

bool Dp3364sDisplay::begin(const Hub75Pins &pins, int panels, uint32_t pclk_hz, int frames_per_init) {
  layout_.panels = panels;
  pclk_hz_ = pclk_hz;
  frames_per_init_ = frames_per_init < 1 ? 1 : frames_per_init;
  memcpy(registers_, dp3364s::DEFAULT_REGISTERS, sizeof(registers_));
  dp3364s::build_gamma_lut(lut_, 100);

  // Frame length padded to a multiple of 32 words (64 bytes) for PSRAM DMA.
  layout_.pad_end = 0;
  const size_t raw = layout_.frame_words();
  layout_.pad_end = static_cast<int>((32 - raw % 32) % 32);
  frame_words_ = layout_.frame_words();
  regs_words_ = kSdrWords + layout_.register_block_words() * dp3364s::DEFAULT_REGISTER_COUNT;
  if (regs_words_ % 2) ++regs_words_;  // keep byte size a multiple of 4

  const size_t frame_bytes = frame_words_ * 2, regs_bytes = regs_words_ * 2;
  regs_buf_ = static_cast<uint16_t *>(heap_caps_calloc(1, regs_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  frame_buf_ = static_cast<uint16_t *>(
      heap_caps_aligned_calloc(kPsramAlign, 1, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  frame_in_psram_ = frame_buf_ != nullptr;
  if (!frame_buf_)  // no PSRAM: try internal RAM (only fits small chains)
    frame_buf_ = static_cast<uint16_t *>(heap_caps_calloc(1, frame_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!regs_buf_ || !frame_buf_) { error_ = "out of memory for the bitstream"; return false; }

  buildRegisters();
  dp3364s::build_frame(frame_buf_, layout_);
  writeBackCache(frame_buf_, frame_bytes);

  // Descriptor ring: registers, then the same frame buffer frames_per_init times.
  const size_t frame_chunk = frame_in_psram_ ? kChunkPsram : kChunkSram;
  const size_t n_regs = chunks_for(regs_bytes, kChunkSram);
  const size_t n_frame = chunks_for(frame_bytes, frame_chunk);
  const size_t n_desc = n_regs + n_frame * frames_per_init_;
  dma_descriptor_t *desc = static_cast<dma_descriptor_t *>(
      heap_caps_calloc(n_desc, sizeof(dma_descriptor_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!desc) { error_ = "out of memory for DMA descriptors"; return false; }
  dma_descriptor_t *next = desc;
  fill_desc(regs_buf_, regs_bytes, kChunkSram, next);
  for (int f = 0; f < frames_per_init_; ++f) fill_desc(frame_buf_, frame_bytes, frame_chunk, next);
  desc[n_desc - 1].next = desc;  // endless loop
  desc[n_desc - 1].dw0.suc_eof = 1;
  desc_ = desc;

  // ---- LCD_CAM peripheral, i8080 mode, 16-bit bus, continuous output ----
  periph_module_enable(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_LCD_CAM_MODULE);
  LCD_CAM.lcd_user.lcd_reset = 1;
  ets_delay_us(100);

  // PCLK = 160 MHz / div_num (clk_equ_sysclk = 1). The fractional divider
  // needs div_a >= 1: with div_a = 0 the LCD clock does not run at all.
  uint32_t div_num = 160000000u / pclk_hz;
  if (div_num < 2) div_num = 2;
  if (div_num > 255) div_num = 255;
  pclk_hz_ = 160000000u / div_num;
  LCD_CAM.lcd_clock.clk_en = 1;
  LCD_CAM.lcd_clock.lcd_clk_sel = 3;          // PLL_F160M
  LCD_CAM.lcd_clock.lcd_clkm_div_num = div_num;
  LCD_CAM.lcd_clock.lcd_clkm_div_a = 1;
  LCD_CAM.lcd_clock.lcd_clkm_div_b = 0;
  LCD_CAM.lcd_clock.lcd_clkcnt_n = 1;         // must not be 0
  LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 1;
  LCD_CAM.lcd_clock.lcd_ck_idle_edge = 0;     // CLK idles low
  LCD_CAM.lcd_clock.lcd_ck_out_edge = 0;      // low in the 1st half: data is set,
                                              // then the rising edge latches it
  LCD_CAM.lcd_ctrl.lcd_rgb_mode_en = 0;       // i8080
  LCD_CAM.lcd_rgb_yuv.lcd_conv_bypass = 0;
  LCD_CAM.lcd_misc.lcd_next_frame_en = 0;
  LCD_CAM.lcd_misc.lcd_bk_en = 1;
  LCD_CAM.lcd_misc.lcd_vfk_cyclelen = 0;
  LCD_CAM.lcd_misc.lcd_vbk_cyclelen = 0;
  LCD_CAM.lcd_data_dout_mode.val = 0;
  LCD_CAM.lcd_user.lcd_always_out_en = 1;     // stream forever
  LCD_CAM.lcd_user.lcd_8bits_order = 0;
  LCD_CAM.lcd_user.lcd_bit_order = 0;
  LCD_CAM.lcd_user.lcd_byte_order = 0;
  LCD_CAM.lcd_user.lcd_2byte_en = 1;          // 16-bit words
  LCD_CAM.lcd_user.lcd_cmd = 0;
  LCD_CAM.lcd_user.lcd_dummy = 0;
  LCD_CAM.lcd_user.lcd_dummy_cyclelen = 0;
  LCD_CAM.lcd_user.lcd_dout = 1;
  LCD_CAM.lcd_user.lcd_update = 1;

  const int8_t bus[dp3364s::BUS_SIGNALS] = {pins.r1, pins.g1, pins.b1, pins.r2, pins.g2,
                                            pins.b2, pins.a,  pins.b,  pins.c,  pins.d,
                                            pins.e,  pins.lat, pins.oe};
  for (int i = 0; i < dp3364s::BUS_SIGNALS; ++i) route_pin(bus[i], LCD_DATA_OUT0_IDX + i);
  route_pin(pins.clk, LCD_PCLK_IDX);

  // ---- GDMA channel feeding the LCD ----
  gdma_channel_alloc_config_t cfg = {};
  cfg.direction = GDMA_CHANNEL_DIRECTION_TX;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
  if (gdma_new_ahb_channel(&cfg, &g_dma) != ESP_OK) { error_ = "no GDMA channel"; return false; }
#else
  if (gdma_new_channel(&cfg, &g_dma) != ESP_OK) { error_ = "no GDMA channel"; return false; }
#endif
  gdma_connect(g_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
  gdma_strategy_config_t strategy = {};
  strategy.owner_check = false;
  strategy.auto_update_desc = false;
  gdma_apply_strategy(g_dma, &strategy);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  gdma_transfer_config_t transfer = {};
  transfer.max_data_burst_size = 64;
  transfer.access_ext_mem = true;
  gdma_config_transfer(g_dma, &transfer);
#else
  gdma_transfer_ability_t ability = {};
  ability.sram_trans_align = 4;
  ability.psram_trans_align = kPsramAlign;
  gdma_set_transfer_ability(g_dma, &ability);
#endif

  desc_ = desc;
  startOutput();
  return true;
}

// clean start: reset DMA, apply LCD settings, reset the LCD FIFO, then go
void Dp3364sDisplay::startOutput() {
  LCD_CAM.lcd_user.lcd_start = 0;
  gdma_stop(g_dma);
  ets_delay_us(100);
  gdma_reset(g_dma);
  ets_delay_us(1000);
  LCD_CAM.lcd_user.lcd_dout = 1;
  LCD_CAM.lcd_user.lcd_update = 1;
  LCD_CAM.lcd_misc.lcd_afifo_reset = 1;
  gdma_start(g_dma, reinterpret_cast<intptr_t>(desc_));
  ets_delay_us(100);
  LCD_CAM.lcd_user.lcd_start = 1;
}

// after a cold power-up the LCD peripheral sometimes stops by itself
bool Dp3364sDisplay::ensureRunning() {
  if (!g_dma || !desc_ || stopped_ || LCD_CAM.lcd_user.lcd_start) return true;
  startOutput();
  restarts_++;
  return false;
}

void Dp3364sDisplay::show(const uint16_t *rgb565) {
  const int w = width();
  dp3364s::encode_frame(frame_buf_, layout_, lut_, [&](int x, int y, uint8_t rgb[3]) {
    const uint16_t c = rgb565[y * w + x];
    const uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
  });
  writeBackCache(frame_buf_, frame_words_ * 2);
}

void Dp3364sDisplay::setCurrentBoost(bool on) {
  for (auto &r : registers_)
    if ((r >> 8) == 0x0B) r = static_cast<uint16_t>(on ? (r | 0x20) : (r & ~0x20));
  if (regs_buf_) buildRegisters();
}

void Dp3364sDisplay::stop() {
  if (!g_dma) return;
  stopped_ = true;
  LCD_CAM.lcd_user.lcd_start = 0;
  gdma_stop(g_dma);
}

void Dp3364sDisplay::setBrightness(int percent) { dp3364s::build_gamma_lut(lut_, percent); }

void Dp3364sDisplay::setCurrentGain(uint8_t value) {
  for (auto &r : registers_)
    if ((r >> 8) == 0x08) r = 0x0800 | value;
  if (regs_buf_) buildRegisters();
}

void Dp3364sDisplay::pinActivity(const Hub75Pins &pins, uint32_t toggles[14], uint32_t highs[14], int samples) {
  const int8_t p[14] = {pins.r1, pins.g1, pins.b1, pins.r2, pins.g2, pins.b2, pins.a,
                        pins.b,  pins.c,  pins.d,  pins.e,  pins.lat, pins.oe, pins.clk};
  int last[14];
  for (int i = 0; i < 14; ++i) { toggles[i] = highs[i] = 0; last[i] = gpio_get_level(static_cast<gpio_num_t>(p[i])); }
  for (int n = 0; n < samples; ++n) {
    for (int i = 0; i < 14; ++i) {
      const int v = gpio_get_level(static_cast<gpio_num_t>(p[i]));
      if (v != last[i]) { ++toggles[i]; last[i] = v; }
      highs[i] += v;
    }
  }
}

const char *Dp3364sDisplay::debugState() {
  static char buf[200];
  int ch = -1;
  if (g_dma) gdma_get_channel_id(g_dma, &ch);
  uint32_t d1 = 0, d2 = 0, park = 0, sel = 0, st = 0;
  if (ch >= 0) {
    d1 = GDMA.channel[ch].out.state.dscr_addr;
    ets_delay_us(50);
    d2 = GDMA.channel[ch].out.state.dscr_addr;
    park = GDMA.channel[ch].out.link.park;
    sel = GDMA.channel[ch].out.peri_sel.sel;
    st = GDMA.channel[ch].out.state.val;
  }
  snprintf(buf, sizeof(buf),
           "dma ch=%d sel=%u park=%u dscr %05x->%05x state=%08x | lcd user=%08x clock=%08x misc=%08x",
           ch, (unsigned)sel, (unsigned)park, (unsigned)d1, (unsigned)d2, (unsigned)st,
           (unsigned)LCD_CAM.lcd_user.val, (unsigned)LCD_CAM.lcd_clock.val, (unsigned)LCD_CAM.lcd_misc.val);
  return buf;
}

uint32_t Dp3364sDisplay::frameRate() const {
  return pclk_hz_ / static_cast<uint32_t>(frame_words_ ? frame_words_ : 1);
}
