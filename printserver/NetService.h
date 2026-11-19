/* NetService.h - WiFi STA/AP manager with persistent credentials.
 * Incorporates urlDecode fix from PR #29 and WiFi.persistent from PR #31
 * (user's fixes to the original printserver-esp8266), plus ESP32 port.
 * Part of the GPLv3 port. */
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

class NetService {
public:
  void begin();                 // connect or start AP
  void connectTo(const String& ssid, const String& pass);
  bool isAp() const { return apMode; }
  String ip() const;
  String info() const;
  // Wi-Fi scan (used by the /wifi web page). While the AP is up we must
  // stay in WIFI_AP_STA, otherwise the AP drops while scanning.
  void startScan();             // kick an async scan if none is running
  int  scanStatus();            // -1 running, 0 idle, >0 = number of results
  String scanJson();            // results as [{ssid,rssi,sec},...] (clears them)
  void tick();                  // reconnect supervision
  static String hostName();     // ESPPrint-XXXX
private:
  bool apMode = false;
  bool scanRunning = false;
  uint32_t lastCheck = 0;
};
