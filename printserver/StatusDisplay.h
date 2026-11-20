/*
 * StatusDisplay.h - 0.91" 128x32 SSD1306 OLED status line.
 *
 * Layout concept (user): line 1 = connectivity only - firmware name at
 * boot / while joining WiFi, station IP once connected, AP IP while in
 * setup mode. Line 2 = what is going on / printer state:
 *   a) "Подкл. к ХХХ..."      while joining network XXX (generic
 *                              "Подключение к WiFi..." when the SSID is
 *                              unknown, e.g. blank first-boot config)
 *   b) "сеть не найдена"       why we are in access-point mode
 *   c) "готов"                 connected + printer online
 *   d) "Загрузка данных"       while receiving a print job
 *   e) "печать..."             while feeding bytes to the printer
 *   f) "нет бумаги"            when PE is asserted
 *   g) "ошибка принтера"       LPT ERROR asserted (transient errors like
 *                              "ошибка LPT"/"ошибка PWG" also land here and
 *                              auto-expire after OLED_ERROR_SHOW_MS so line 1
 *                              is never held hostage by a one-shot message)
 *   h) "принтер не подключен"  LPT status lines unreadable / SELECT low
 * A small activity icon in the top-right corner animates while busy.
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
  UiState uiState() const { return state; }   // for StatusLed: LED mirrors the OLED
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
  uint32_t errorUntil = 0;           // millis() when errorText auto-expires
  uint32_t lastDraw = 0;
  uint8_t animFrame = 0;
  bool dirty = true;
  void render();
};
