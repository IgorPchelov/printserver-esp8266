/*
 * StatusDisplay.cpp - see StatusDisplay.h
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#include "StatusDisplay.h"
#include "ParallelPort.h"

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
  if (now - lastDraw < OLED_UPDATE_MS) return;
  lastDraw = now;
  animFrame++;
  u8g2->setPowerSave(0);
  render();
}

void StatusDisplay::render() {
  if (!u8g2) return;
  u8g2->clearBuffer();

  // --- decide the main line ------------------------------------------
  String line1;
  bool busyAnim = false;
  LptStatus effective = printerStatus;

  if (errorText.length()) {
    line1 = errorText;                       // g) errors override everything
  } else if (effective == LptStatus::PAPER_OUT) {
    line1 = "нет бумаги";                    // f)
  } else if (effective == LptStatus::ERROR_STATE) {
    line1 = "ошибка";                        // g)
  } else {
    switch (state) {
      case UiState::BOOT:           line1 = FIRMWARE_NAME; break;
      case UiState::WIFI_CONNECTING:line1 = "подключение к wifi..."; busyAnim = true; break; // a)
      case UiState::AP_MODE:        line1 = "AP " + WiFi.softAPIP().toString(); break;      // b)
      case UiState::CONNECTED:      line1 = "IP " + WiFi.localIP().toString(); break;       // c)
      case UiState::RECEIVING:      line1 = "Загрузка данных"; busyAnim = true; break;      // d)
      case UiState::PRINTING:       line1 = "печать..."; busyAnim = true; break;            // e)
    }
  }

  // --- line 1: 12px font, up to ~21 latin / ~10 cyrillic chars -------
  u8g2->setFont(u8g2_font_6x12_t_cyrillic);
  uint16_t w = u8g2->getUTF8Width(line1.c_str());
  int16_t x = 0;
  if (w > 128) {
    // scroll long text slowly
    int16_t shift = (animFrame / 2) % (w - 128 + 16);
    if (shift > 16) shift = (w + 16 - 128) - shift; // ping-pong
    x = -shift;
  }
  u8g2->drawUTF8(x, 12, line1.c_str());

  // --- line 2: same 12px font as line 1 (user request), clipped to width
  String line2;
  if (state == UiState::CONNECTED) {
    line2 = String(HOST_NAME_PREFIX) + ".local";   // no "mDNS:" prefix: the
    // full host name barely fits 128 px as is; a prefix pushed it off-screen.
  } else if (state == UiState::AP_MODE) {
    line2 = "сеть не найдена";   // line 1 already shows the AP IP; repeating
    // the setup URL below it was pure duplication - say WHY we are in AP.
  } else if (state == UiState::RECEIVING || state == UiState::PRINTING) {
    line2 = String(HOST_NAME_PREFIX) + " - " + lptStatusName(effective);
  } else if (state == UiState::BOOT || state == UiState::WIFI_CONNECTING) {
    line2 = "Подключение к WiFi...";   // boot: line 1 = firmware name, and
    // the join can take up to 20 s - show what the box is doing meanwhile.
  } else if (errorText.length() == 0
          && (effective == LptStatus::OFFLINE || effective == LptStatus::UNKNOWN)) {
    // UNKNOWN -> "принтер не подключен" (self-describing, no prefix);
    // OFFLINE -> "принтер: офлайн"
    line2 = (effective == LptStatus::UNKNOWN) ? String(lptStatusName(effective))
                                              : "принтер: " + String(lptStatusName(effective));
  }
  if (line2.length()) {
    u8g2->setFont(u8g2_font_6x12_t_cyrillic);          // same as line 1
    uint16_t w2 = u8g2->getUTF8Width(line2.c_str());
    int16_t x2 = 0;
    if (w2 > 128) {
      // too wide even without a prefix (long host name): ping-pong scroll,
      // same as line 1, so the full value stays readable
      int16_t shift = (animFrame / 2) % (w2 - 128 + 16);
      if (shift > 16) shift = (w2 + 16 - 128) - shift;
      x2 = -shift;
    }
    u8g2->drawUTF8(x2, 27, line2.c_str());             // baseline 27 keeps 12px glyphs inside 32px
  }

  // --- activity icon (top-right) --------------------------------------
  if (busyAnim) {
    const uint8_t frames[4][6] = {
      {0x80,0x40,0x20,0x10,0x08,0x04},
      {0x04,0x08,0x10,0x20,0x40,0x80},
      {0x04,0x08,0x10,0x20,0x40,0x80},
      {0x80,0x40,0x20,0x10,0x08,0x04},
    };
    uint8_t f = (animFrame / 2) % 4;
    for (uint8_t i = 0; i < 6; i++)
      u8g2->drawVLine(120 + i, 2, 6 + frames[f][i] / 32); // simple bars
  }

  u8g2->sendBuffer();
}
