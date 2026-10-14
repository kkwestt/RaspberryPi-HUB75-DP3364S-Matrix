// Elapsed-time counter (or a countdown while the start moment is in the future): time passed since a start moment set on the web page
// (a reset button = start now, or an explicit date and time). Pure logic.
#pragma once

#include <stdint.h>

struct CountdownConfig {
  int64_t start = 0;  // unix time the counter runs from; 0 = not set
};

enum CountdownPhase { CD_NOT_SET, CD_RUNNING, CD_WAITING };  // WAITING: start is in the future, counting down to it

struct CountdownState {
  CountdownPhase phase = CD_NOT_SET;
  int64_t elapsed = 0;    // seconds since start
  int64_t remaining = 0;  // seconds until start (CD_WAITING)
};

inline CountdownState countdown_at(const CountdownConfig &c, int64_t now) {
  CountdownState s;
  if (c.start <= 0) return s;
  if (now < c.start) { s.phase = CD_WAITING; s.remaining = c.start - now; return s; }
  s.phase = CD_RUNNING;
  s.elapsed = now - c.start;
  return s;
}
