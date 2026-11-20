/*
 * StatusLed.cpp - see StatusLed.h
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#include "StatusLed.h"

// Blink periods per state (ms, full cycle). The numbers give distinct
// patterns even with a single-color LED (steady = solid on):
//   WIFI_CONNECTING 2 Hz / AP_MODE 1 Hz / RECEIVING & PRINTING 5 Hz
static const uint16_t PERIOD_WIFI_MS        = 500;   // joining, 2 Hz
static const uint16_t PERIOD_AP_MS          = 1000;  // setup, 1 Hz
static const uint16_t PERIOD_JOB_MS         = 200;   // receive/print, 5 Hz
static const uint16_t PERIOD_ATTENTION_MS   = 250;   // operator needed, 4 Hz

void StatusLed::begin() {
  // The DevKitC-1 onboard WS2812 is a one-wire device, not a GPIO output:
  // driving it LOW with digitalWrite would latch a stale colour, so it is
  // fully controlled via neopixelWrite() calls only. The simple LED pin is
  // a plain GPIO. 255 = "not fitted" for either (see config.h).
  if (PIN_LED_WIFI != 255) pinMode(PIN_LED_WIFI, OUTPUT);
  raw(0, 0, 0);                         // both LEDs off until the first tick
}

void StatusLed::setMode(UiState s) {
  if (mode != s) { mode = s; lastBlink = 0; blinkOn = false; }
}

void StatusLed::setAttention(bool on) {
  if (attention != on) { attention = on; lastBlink = 0; blinkOn = false; }
}

void StatusLed::tick() {
  uint32_t now = millis();

  // Pattern per state (see StatusLed.h for the colour table). A pattern
  // with period 0 is a steady colour.
  uint8_t r = 0, g = 0, b = 0;
  uint16_t period = 0;
  if (attention) {                       // red overrides everything
    r = 255; g = 16; b = 0;              // ... a hint of orange so a quick
    period = PERIOD_ATTENTION_MS;        // glance tells "paper", not "death"
  } else switch (mode) {
    case UiState::BOOT:
    case UiState::WIFI_CONNECTING:       // blue blink 2 Hz
      r = 0; g = 16; b = 255;
      period = PERIOD_WIFI_MS;
      break;
    case UiState::AP_MODE:               // yellow blink 1 Hz
      r = 255; g = 140; b = 0;
      period = PERIOD_AP_MS;
      break;
    case UiState::CONNECTED:             // green steady
      r = 0; g = 255; b = 0;
      period = 0;
      break;
    case UiState::RECEIVING:             // cyan blink 5 Hz
      r = 0; g = 200; b = 255;
      period = PERIOD_JOB_MS;
      break;
    case UiState::PRINTING:              // magenta blink 5 Hz
      r = 255; g = 0; b = 200;
      period = PERIOD_JOB_MS;
      break;
  }

  // steady colour: write once when it changes (raw() no-ops on same colour)
  if (period == 0) { raw(r, g, b); return; }

  // blinking: toggle on half-period boundaries
  if (now - lastBlink >= period / 2) {
    lastBlink = now;
    blinkOn = !blinkOn;
    raw(blinkOn ? r : 0, blinkOn ? g : 0, blinkOn ? b : 0);
  }
}

void StatusLed::raw(uint8_t r, uint8_t g, uint8_t b) {
  // Scale down: full 255 is eye-burning on a bare WS2812; ~33% is plenty
  // visible and keeps the 5 V rail quiet during long prints.
  if (PIN_LED_RGB != 255) neopixelWrite(PIN_LED_RGB, r / 3, g / 3, b / 3);
  if (PIN_LED_WIFI != 255) digitalWrite(PIN_LED_WIFI, (r | g | b) ? HIGH : LOW);
}
