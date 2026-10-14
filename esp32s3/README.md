# ESP32-S3 driver for DP3364S HUB75 panels

Flicker-free output for 128x64 P2.5 panels with **DP3364S** S-PWM column
drivers, as an alternative to the Raspberry Pi driver in this repository.

Why: DP3364S derives its grayscale clock from the HUB75 CLK line through an
on-chip PLL. On a Raspberry Pi, CLK is bit-banged, so any memory-bus load (video
decoding, neural networks) makes the CLK rate, and with it the brightness, jump.
The ESP32-S3 sends CLK, data, LAT, OE/ROW and A-E from memory through
**GDMA → LCD_CAM** (i8080, 16-bit bus). The clock is generated in hardware and
never varies.

The example application is a stand display for two chained panels (256x64):

- **left panel:** a number fetched from a Raspberry Pi over HTTP (people who
  stayed at the stand for 10 s);
- **right panel:** time elapsed since a start moment set on the web page;
- NTP time, re-synced every hour;
- web settings, OTA updates, watchdog.

Replace `src/screens.cpp` and the polling in `src/main.cpp` with your own content.

## How the bitstream was obtained

The protocol was **captured from the working Raspberry Pi setup**, not written
from the datasheet:

- the capture-patched rpi-rgb-led-matrix in `tools/capture` recorded the state
  of every pin at every CLK edge;
- `src/dp3364s.*` regenerates that stream **bit-for-bit**.

```sh
cd tools
c++ -O2 -std=c++17 -I../src verify_stream.cpp ../src/dp3364s.cpp -o verify_stream
./verify_stream ref.bin ref_init.bin      # captures from tools/capture (not committed, 12 MB)
#   steady frame        139796 words, 0 mismatches -> OK
#   13 register writes    3796 words, 0 mismatches -> OK
#   frame after init    139796 words, 0 mismatches -> OK
```

### Stream format (2 panels, 1/32 scan)

| Part | Clocks |
|---|---|
| VSYNC (LAT high) | 3 |
| idle | 17 |
| scan: ROW pulse 4 clocks wide every 128 clocks, A-E = row 0..31 | 139 264 |
| ...first 131 072 scan clocks carry data: 512 blocks of 16 chips x 16 bits, MSB first, each followed by one DATA_LATCH clock (LAT high, scan counter frozen) | +512 |
| register write: VSYNC, 17 idle, PRE_ACT (LAT 14), 1 idle, 16 bits x chips (LAT on the last 5 = WR_CFG), 1 idle | 292 each |

Pixel mapping: latch = line x 16 + (x mod 16), slot = x div 16. The upper half
(y < 32) goes on R1/G1/B1, the lower half on R2/G2/B2. Grayscale: CIE 1931
lightness, 11-bit, MSB-aligned to 12 bits (REG03 = REG04 = 0x3F shows 64 x 64
levels).

Registers (REG01-0D): `0x1100 0x021F 0x033F 0x043F 0x0504 0x0642 0x0700 0x08BF
0x0960 0x0ABE 0x0B8B 0x0C08 0x0D12`.

The output loop is [SDR command + 13 register writes] → frame x 16 → repeat.
Rewriting the registers about 4 times a second lets the panels recover by
themselves after a power or cable glitch. The SDR command (LAT 15 clocks)
selects single-edge clocking, which is undefined after power-on.

## Hardware

Tested with an **ESP32-S3-DevKitC-1 N16R8** (8 MB octal PSRAM) wired to the
40-pin header of the iarduino RGB Matrix HAT, at the positions a Raspberry Pi
would use. The HAT's 74HC245 buffer and HUB75 connector are reused.

| Signal | ESP32-S3 GPIO | HAT header pin (Pi GPIO) | HUB75 pin |
|---|---|---|---|
| R1 | 4 | 23 (GPIO11) | 1 |
| G1 | 5 | 13 (GPIO27) | 2 |
| B1 | 6 | 26 (GPIO7) | 3 |
| R2 | 7 | 24 (GPIO8) | 5 |
| G2 | 15 | 21 (GPIO9) | 6 |
| B2 | 16 | 19 (GPIO10) | 7 |
| A | 17 | 15 (GPIO22) | 9 |
| B | 18 | 16 (GPIO23) | 10 |
| C | 8 | 18 (GPIO24) | 11 |
| D | 9 | 22 (GPIO25) | 12 |
| E | 10 | 10 (GPIO15) | 8 |
| LAT | 11 | 7 (GPIO4) | 14 |
| OE | 12 | 12 (GPIO18) | 15 |
| CLK | 13 | 11 (GPIO17) | 13 |
| 5V / 3V3 / GND | 5V / 3V3 / GND | 2 / 1 / 6 | — |

- **PSRAM is required.** The stream for two panels is 273 KB, and GDMA reads it
  from PSRAM.
- **CLK is 8 MHz** (57 full frames/s). At 16 MHz the panels stayed dark over
  hand-soldered wires.
- An Adafruit MatrixPortal S3 environment is included (`matrixportal_s3`) but
  untested.

## Build and flash

```sh
cp include/secrets.h.example include/secrets.h   # Wi-Fi, OTA password, data URL
pio run -e esp32s3_devkitc_n16r8 -t upload       # first time over USB
OTA_PASSWORD=... pio run -e esp32s3_devkitc_n16r8_ota -t upload --upload-port <ip>
```

Web page: `http://<ip>/`. It shows status, the start moment of the elapsed
counter, labels, brightness, LED current (REG08 and the REG0B x1.5 boost) and
panel order. `/update` takes a firmware upload from the browser. `/pintest`
drives each HUB75 line high one at a time, so the wiring can be checked with a
multimeter. `/status.json` reports the state.

Set your own `OTA_PASSWORD` in `secrets.h`. The default is `stand-display`.

## Robustness

- task watchdog (30 s);
- reboot if the display does not start, or after 15 min without Wi-Fi;
- a 3 s delay before the output starts, so the panel power settles;
- the last value received from the Pi is kept in flash and shown while the Pi
  is unreachable (with a small yellow dot in the corner).
