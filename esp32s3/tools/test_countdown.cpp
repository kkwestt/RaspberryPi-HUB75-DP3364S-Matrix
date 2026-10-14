// c++ -std=c++17 -I../src test_countdown.cpp -o test_countdown && ./test_countdown
#include <stdio.h>

#include "countdown.h"

static int fails = 0;
static void check(const char *what, bool ok) {
  printf("%-40s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) ++fails;
}

int main() {
  CountdownConfig c;
  const int64_t t0 = 1760000000;
  check("not set", countdown_at(c, t0).phase == CD_NOT_SET);
  c.start = t0;
  check("before start: waiting", countdown_at(c, t0 - 5).phase == CD_WAITING);
  check("before start: 5 s left", countdown_at(c, t0 - 5).remaining == 5);
  auto s = countdown_at(c, t0);
  check("at start: 0 s", s.phase == CD_RUNNING && s.elapsed == 0);
  s = countdown_at(c, t0 + 3 * 86400 + 61);
  check("3 days 1 min 1 s later", s.elapsed == 3 * 86400 + 61);
  printf(fails ? "\nFAIL\n" : "\nall checks passed\n");
  return fails ? 1 : 0;
}
