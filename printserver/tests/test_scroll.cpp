// test_scroll.cpp - host regression test for StatusDisplay::scrollShift().
//
// The old ping-pong formula folded the second half of the period at
// shift>16, so a long line drifted 16 px left, JUMPED ~150 px to the far
// end and crawled back - the left sweep never played. The uint8_t frame
// counter additionally wrapped (at 128 half-steps) before any realistically
// wide line finished a single sweep. The new formula must:
//   - never move more than 1 px per frame (no jumps, any width)
//   - sweep monotonically left, then monotonically right
//   - reach BOTH ends of the range [-(w-128)-16, 0] every cycle
//   - be periodic with period 4*span frames (2*span half-steps)
//   - complete full cycles even for wide lines (uint32 frame counter)
#include "StatusDisplay.h"
#include <cstdio>

static int fails = 0;
static void ok(const char* m) { printf("ok:   %s\n", m); }
static void bad(const char* m) { printf("FAIL: %s\n", m); fails++; }

// walk one full cycle of a given width, checking trajectory invariants
static void walkCycle(uint16_t w, const char* label) {
  const uint32_t span = (uint32_t)w - 128 + 16;
  const uint32_t period = 4 * span;              // frames per full cycle
  int prev = StatusDisplay::scrollShift(w, 0);
  int maxStep = 0, minS = prev, maxS = prev;
  bool monoL = true, monoR = true;
  for (uint32_t f = 1; f <= period; f++) {
    int s = StatusDisplay::scrollShift(w, f);
    int d = s > prev ? s - prev : prev - s;
    if (d > maxStep) maxStep = d;
    if (s < minS) minS = s;
    if (s > maxS) maxS = s;
    if (f <= 2 * span) { if (s > prev) monoL = false; }   // left sweep: down only
    else              { if (s < prev) monoR = false; }    // right sweep: up only
    prev = s;
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "%s: max step %d px (no jumps)", label, maxStep);
  maxStep <= 1 ? ok(msg) : bad(msg);
  snprintf(msg, sizeof(msg), "%s: left sweep monotone", label);
  monoL ? ok(msg) : bad(msg);
  snprintf(msg, sizeof(msg), "%s: right sweep monotone", label);
  monoR ? ok(msg) : bad(msg);
  snprintf(msg, sizeof(msg), "%s: range [%d, %d] = [-(w-128)-16, 0]",
           label, minS, maxS);
  (minS == -(int)span && maxS == 0) ? ok(msg) : bad(msg);
  snprintf(msg, sizeof(msg), "%s: periodic (period %u frames)", label, (unsigned)period);
  (StatusDisplay::scrollShift(w, period) == StatusDisplay::scrollShift(w, 0)) ? ok(msg) : bad(msg);
}

int main() {
  // 1) lines that fit never scroll
  bool pass = true;
  for (uint32_t f = 0; f < 2000; f++) {
    if (StatusDisplay::scrollShift(128, f) != 0) pass = false;
    if (StatusDisplay::scrollShift(100, f) != 0) pass = false;
  }
  pass ? ok("w <= 128: shift is always 0") : bad("w <= 128: nonzero shift");

  // 2) a typical wide line: "Подкл. к " + 25-char SSID at 6 px/char ~ 204 px
  //    (the old formula jumped ~155 px mid-cycle here)
  walkCycle(204, "204 px line");

  // 3) minimal scroll width (129 px) - edge case, span 17
  walkCycle(129, "129 px line");

  // 4) a very wide line (~28-char Cyrillic error message): the old uint8_t
  //    frame counter wrapped at 128 half-steps, so a 888-half-step sweep
  //    never completed. A uint32 counter must reach the far end.
  const uint16_t W = 1000;
  const int32_t span = W - 128 + 16;
  char msg[96];
  snprintf(msg, sizeof(msg), "1000 px line: peak -(w-128)-16 = %d reached", -(int)span);
  (StatusDisplay::scrollShift(W, 2 * span) == -(int)span) ? ok(msg) : bad(msg);

  return fails ? 1 : 0;
}
