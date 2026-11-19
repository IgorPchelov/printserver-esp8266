/*
 * StatusDisplay.h - 0.91" 128x32 SSD1306 OLED status line.
 *
 * Shows (Russian, per user requirements):
 *   a) "подключение к wifi..." while joining a network
 *   b) "AP 192.168.4.1"       in access-point mode (actual AP IP)
 *   c) "IP 192.168.1.200"     when connected (actual station IP)
 *   d) "Загрузка данных"       while receiving a print job
 *   e) "печать..."             while feeding bytes to the printer
 *   f) "нет бумаги"            when PE is asserted
 *   g) "ошибка"                on printer/hardware errors
 * plus a small activity icon and mDNS name hint on the second line.
 * In AP mode the second line is prefixed with a 12x12 gear icon (XBM)
 * instead of the "setup: " word - keeps the URL from scrolling.
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#pragma once

#include <U8g2lib.h>
#include <WiFi.h>
#include "config.h"
#include "ParallelPort.h"

enum class UiState : uint8_t {
  BOOT,
  WIFI_CONNECTING,   // a
  AP_MODE,           // b
  CONNECTED,         // c
  RECEIVING,         // d
  PRINTING,          // e
};

class StatusDisplay {
public:
  void begin();
  void setState(UiState s);          // network/job driven state
  void setError(const String& e);    // transient error text (g)
  void clearError();
  void setPrinterStatus(LptStatus s);// reflects LPT lines (f/g handled here too)
  void tick();                       // periodic refresh, animations
  void off();                        // display off (power save)
private:
  U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C* u8g2 = nullptr;
  UiState state = UiState::BOOT;
  LptStatus printerStatus = LptStatus::UNKNOWN;
  String errorText = "";
  uint32_t lastDraw = 0;
  uint8_t animFrame = 0;
  bool dirty = true;
  void render();
};
