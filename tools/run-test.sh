#!/bin/sh
# Run a program on 128x64 P2.5 HUB75 panels with DP3364S column drivers.
# Usage: sudo ./run-test.sh [binary] [chain]
#   binary: ./static-test (default) or ./clock
#   chain:  number of panels daisy-chained on one connector (default 1)
BIN=${1:-./static-test}
CHAIN=${2:-1}
HERE=$(cd "$(dirname "$0")" && pwd)

export SPWM_DP3265S_REG02=0x021F   # DP3364S LINE_SET: 32 scan lines (1/32 scan)
export SPWM_DP3265S_SCAN_ROWS=32   # drive A-E through 32 rows (fork default is 8)
export SPWM_GRAY_BITS=12           # driver shows 64 groups x 64 clocks = 12-bit grayscale
export SPWM_REINIT_MS=5000         # rewrite registers every 5 s (panel power/cable hot-plug)
export FONT_DIR=${FONT_DIR:-$HERE/../rpi-rgb-led-matrix-spwm/fonts}

# chain length = 8 DP3364S chips per data line per panel
INI=/etc/spwm-panel.ini
printf '[dp3265s]\nupload_chip_count = %d\n' $((8 * CHAIN)) > "$INI"

exec "$BIN" \
  --led-rows=64 --led-cols=128 --led-chain="$CHAIN" \
  --led-gpio-mapping=regular \
  --led-panel-type=dp3265s \
  --led-spwm-row-addr-type=0 \
  --led-slowdown-gpio=5 \
  --led-no-hardware-pulse
