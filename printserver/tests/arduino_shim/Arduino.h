// Minimal Arduino.h shim for host-side unit tests of firmware modules.
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
typedef uint8_t byte;
// millis() for PwgToPcl::pump() budgeting: a fake clock the test can advance
// (default 0 = "budget never expires", exactly what the host tests want:
// pump() drains as fast as the sink accepts).
inline uint32_t g_shimMillis = 0;
inline uint32_t millis() { return g_shimMillis; }
inline void delayMicroseconds(uint32_t) {}
