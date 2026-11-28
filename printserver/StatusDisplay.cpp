/*
 * StatusDisplay.cpp - see StatusDisplay.h
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#include "StatusDisplay.h"
#include "ParallelPort.h"
#include "NetService.h"      // joiningSsid(), isForcedAp(): wifi state the OLED shows

// Triangle-wave scroll offset for a line wider than the 128 px viewport.
// One period = 2*span half-steps: sweep left to -span (= -(w-128) minus a
// 16 px dwell margin that shows the line's END clearly before reversing),
// then sweep back to 0. The old formula folded the second half of the
// period at shift>16, so long lines drifted 16 px left, JUMPED to the far
// end and crawled back - the left sweep never happened; the uint8_t frame
// counter additionally wrapped before wide lines completed one cycle.
int16_t StatusDisplay::scrollShift(uint16_t w, uint32_t frame) {
  if (w <= 128) return 0;
  const uint32_t span = (uint32_t)w - 128 + 16;
  uint32_t t = (frame / 2) % (2 * span);
  int16_t shift = (t < span) ? (int16_t)t : (int16_t)(2 * span - t);
  return -shift;
}

void StatusDisplay::begin() {
  u8g2 = new U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C(U8G2_R0, PIN_OLED_SCL, PIN_OLED_SDA, U8X8_PIN_NONE);
  u8g2->setI2CAddress(OLED_ADDR << 1);
  u8g2->begin();
  u8g2->setFontMode(1);
  u8g2->setFontDirection(0);
  u8g2->setContrast(128);
  render();
}

void StatusDisplay::setState(UiState s) {
  if (state != s) { state = s; dirty = true; }
}

void StatusDisplay::setError(const String& e) {
  if (errorText != e) { errorText = e; dirty = true; }
  errorUntil = millis() + OLED_ERROR_SHOW_MS;   // auto-expires, see tick()
}

void StatusDisplay::clearError() {
  if (errorText.length()) { errorText = ""; dirty = true; }
}

void StatusDisplay::setPrinterStatus(LptStatus s) {
  if (printerStatus != s) { printerStatus = s; dirty = true; }
}

void StatusDisplay::off() {
  if (u8g2) u8g2->setPowerSave(1);
}

void StatusDisplay::tick() {
  if (!u8g2) return;
  uint32_t now = millis();
  // transient error banner auto-expires so the network line (line 1) is
  // never held hostage by a one-shot failure message
  if (errorText.length() && (int32_t)(now - errorUntil) >= 0) {
    errorText = ""; dirty = true;
  }
  if (now - lastDraw < OLED_UPDATE_MS && !dirty) return;
  lastDraw = now;
  animFrame++;
  dirty = false;
  u8g2->setPowerSave(0);
  render();
}

void StatusDisplay::render() {
  if (!u8g2) return;
  u8g2->clearBuffer();

  // Display concept (user): line 1 = connectivity - firmware name at boot,
  // station IP once joined, AP IP while in setup mode; line 2 = what is
  // going on / printer state. Line 1 never shows job or printer states.
  String line1;
  bool busyAnim = false;
  LptStatus effective = printerStatus;

  // current connectivity address: AP IP in AP mode (even if the STA side
  // briefly associates during a retry - the user is on our AP), otherwise
  // the station IP; a job can also legitimately arrive from a client on
  // our own AP, so the working states reuse the same choice
  String addr;
  if (state == UiState::AP_MODE) {
    addr = "AP " + WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    addr = "IP " + WiFi.localIP().toString();
  } else {
    String ap = WiFi.softAPIP().toString();
    addr = (ap != "0.0.0.0") ? "AP " + ap : "IP " + WiFi.localIP().toString();
  }

  switch (state) {
    case UiState::BOOT:            line1 = FIRMWARE_NAME; break;
    case UiState::WIFI_CONNECTING: line1 = FIRMWARE_NAME; break;
    case UiState::AP_MODE:         line1 = addr; break;                               // b)
    case UiState::CONNECTED:       line1 = addr; break;                               // c)
    case UiState::RECEIVING:       line1 = addr; busyAnim = true; break;              // d)
    case UiState::PRINTING:        line1 = addr; busyAnim = true; break;              // e)
  }

  // --- line 1: 12px font, up to ~21 latin / ~10 cyrillic chars -------
  u8g2->setFont(u8g2_font_6x12_t_cyrillic);
  uint16_t w = u8g2->getUTF8Width(line1.c_str());
  int16_t x = scrollShift(w, animFrame);
  u8g2->drawUTF8(x, 12, line1.c_str());

  // --- line 2: same 12px font as line 1, one status line (user concept) --
  String line2;
  if (errorText.length()) {                        // g) transient errors first
    line2 = errorText;
  } else if (state == UiState::WIFI_CONNECTING || state == UiState::BOOT) {
    // "Подкл. к ХХХ" (user request): the SSID we are trying to join, straight
    // from the STA configuration (WiFi.SSID() stays empty until the link is
    // actually up, so it cannot be used here). Known SSID -> "Подкл. к
    // HomeWiFi..."; unknown/empty (blank first-boot config) -> the generic
    // "Подключение к WiFi...".
    String ssid = NetService::joiningSsid();
    line2 = ssid.length() ? "Подкл. к " + ssid + "..." : "Подключение к WiFi...";
                                        // a) join can take up to 20 s
  } else if (state == UiState::AP_MODE) {
    line2 = NetService::isForcedAp() ? "настройка" : "сеть не найдена";   // why we are in AP mode
  } else if (state == UiState::RECEIVING) {
    line2 = "Загрузка данных";                     // d)
  } else if (state == UiState::PRINTING) {
    line2 = (effective == LptStatus::PAPER_OUT || effective == LptStatus::ERROR_STATE ||
             effective == LptStatus::OFFLINE)
              ? String(lptStatusName(effective))   // operator must intervene
              : "печать...";                       // e) (OFFLINE = paused, waiting for the printer)
  } else if (effective == LptStatus::PAPER_OUT || effective == LptStatus::ERROR_STATE) {
    line2 = lptStatusName(effective);              // f) "нет бумаги" / "ошибка принтера"
  } else if (effective == LptStatus::OFFLINE || effective == LptStatus::UNKNOWN) {
    line2 = "принтер не подключен";                // unwired / SELECT low
  } else {
    line2 = "готов";                               // connected + printer online
  }
  if (line2.length()) {
    u8g2->setFont(u8g2_font_6x12_t_cyrillic);          // same as line 1
    uint16_t w2 = u8g2->getUTF8Width(line2.c_str());
    int16_t x2 = scrollShift(w2, animFrame);            // ping-pong, same as line 1
    u8g2->drawUTF8(x2, 27, line2.c_str());             // baseline 27 keeps 12px glyphs inside 32px
  }

  // --- activity icon (top-right): classic spinner /-\| ------------------
  // 2.4.2: the old bar-flicker drew random-length vertical bars that read
  // as noise ("странная анимация" - user report). A rotating stick is the
  // universal "busy" glyph: 4 frames, 45 degrees apart, drawn as a short
  // diagonal line in a 6x6 cell at x=122.
  if (busyAnim) {
    static const uint8_t f1[] = {2,1}, f2[] = {3,2}, f3[] = {2,3}, f4[] = {1,2};
    const uint8_t* segs[4] = {f1, f2, f3, f4};
    const uint8_t* s = segs[animFrame & 3];
    for (uint8_t i = 0; i < 2; i++)
      u8g2->drawPixel(122 + s[0] + i, 2 + s[1]);
    // center dot as the pivot: makes the rotation readable at 6 px
    u8g2->drawPixel(124, 4);
    u8g2->drawPixel(123, 3);
  }

  u8g2->sendBuffer();
}
