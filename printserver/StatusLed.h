/*
 * StatusLed.h - status LED: connectivity/job state and printer-attention
 * signals on the onboard WS2812 RGB LED (PIN_LED_RGB) and, if wired, on a
 * simple LED (PIN_LED_WIFI). Mirrors the OLED so the box stays readable
 * even when the display is dead or not fitted (user request).
 *
 *   blue    blink 2 Hz   joining WiFi ("Подкл. к ХХХ" on the OLED)
 *   green   steady       connected to WiFi
 *   yellow  blink 1 Hz   configuration AP is up (setup mode)
 *   cyan    blink 5 Hz   receiving a print job
 *   magenta blink 5 Hz   printing
 *   red     rare flashes printer needs the operator: no paper / error -
 *                         overrides everything, printing stops until fixed
 * The simple LED shows the same states as blink patterns (same on/off
 * timing as the RGB, see printserver/README.md for the table).
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#pragma once

#include <Arduino.h>
#include "config.h"
#include "StatusDisplay.h"     // UiState - the same states the OLED shows

class StatusLed {
public:
  void begin();                 // configure pins, LEDs off
  void setMode(UiState s);      // network/job state (call it everywhere the
                                // OLED state changes)
  void setAttention(bool on);   // printer needs the operator (paper out /
                                // printer error): red overrides all states
  void tick();                  // blink patterns; call from loop() and from
                                // the blocking-connect pumps
private:
  UiState mode = UiState::BOOT;
  bool attention = false;
  uint32_t lastBlink = 0;       // when the LED last toggled
  bool blinkOn = false;
  void raw(uint8_t r, uint8_t g, uint8_t b);  // scale + drive both LEDs
};
