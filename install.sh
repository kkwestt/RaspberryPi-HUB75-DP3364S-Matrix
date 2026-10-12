#!/bin/sh
# Build everything needed to drive DP3364S HUB75 panels on a Raspberry Pi:
#   1. install build dependencies
#   2. clone the S-PWM fork of rpi-rgb-led-matrix at the tested commit
#   3. apply the DP3364S patch
#   4. build the library and the tools (static-test, clock)
# Usage: ./install.sh        (run as a normal user; sudo is used for apt only)
set -e

FORK_URL=https://github.com/angyalr/rpi-rgb-led-matrix_pwm_DP3265S.git
FORK_COMMIT=8e4f9805568dabfc71d1b470846786e1a3e9bc89
HERE=$(cd "$(dirname "$0")" && pwd)
LIB="$HERE/rpi-rgb-led-matrix-spwm"

echo "== dependencies"
need=""
for p in git build-essential; do
  dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"
done
if [ -n "$need" ]; then
  sudo apt-get update
  sudo apt-get install -y $need
fi

echo "== library: $LIB"
if [ ! -d "$LIB/.git" ]; then
  git clone "$FORK_URL" "$LIB"
fi
cd "$LIB"
git fetch -q origin "$FORK_COMMIT" 2>/dev/null || true
if git diff --quiet; then
  git checkout -q "$FORK_COMMIT"
  git apply "$HERE/patches/0001-dp3364s-128x64-support.patch"
  echo "patch applied"
elif git apply --reverse --check "$HERE/patches/0001-dp3364s-128x64-support.patch" 2>/dev/null; then
  echo "patch already applied"
else
  echo "ERROR: $LIB has local changes that are not this patch; remove the folder and rerun." >&2
  exit 1
fi
make -j"$(nproc)" -C lib
make -j"$(nproc)" -C examples-api-use

echo "== tools"
cd "$HERE/tools"
for p in static-test clock; do
  g++ -O2 -I"$LIB/include" "$p.cc" -o "$p" -L"$LIB/lib" -lrgbmatrix -lrt -lm -lpthread
  echo "built tools/$p"
done

cat <<EOF

Done. Try (one panel, then two chained panels):
  sudo $HERE/tools/run-test.sh $HERE/tools/static-test 1
  sudo $HERE/tools/run-test.sh $HERE/tools/clock 2

Autostart the clock at boot: see "Run at boot" in README.md.
EOF
