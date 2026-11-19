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
  void begin();                 // init radio + kick the join with stored credentials (non-blocking)
  bool waitConnected(void (*pump)() = nullptr);   // blocking wait for that join (AP fallback);
                                                  // pump() runs every iteration so the OLED/LPT
                                                  // keep living while blocked
  void connectTo(const String& ssid, const String& pass, void (*pump)() = nullptr);
  bool isAp() const { return apMode; }
  String ip() const;
  String info() const;
  static String joiningSsid();  // SSID the STA is trying to join (esp_wifi STA config);
                                // "" when unknown -> display falls back to generic text
  // Wi-Fi scan (used by the /wifi web page). While the AP is up we must
  // stay in WIFI_AP_STA, otherwise the AP drops while scanning.
  void startScan();             // kick a fast async scan if none is running
  int  scanStatus();            // -1 running, -2 failed, 0 idle, 1 results ready
  String scanJson();            // results as [{ssid,rssi,sec},...] (clears them)
  void setBusy(bool b) { busy = b; }  // app is receiving/printing: defer the AP->STA
                                      // switchover (it would kill the job in flight)
  void tick();                  // reconnect supervision
  static String hostName();     // ESPPrint-XXXX
private:
  void startAp();               // AP+STA fallback with the background join hunt stopped
  bool apMode = false;
  bool haveStored = false;      // stored credentials exist (skip the 20 s wait otherwise)
  bool scanRunning = false;
  uint32_t scanStartMs = 0;     // watchdog: when the async scan was kicked
  bool apRetryActive = false;   // a stored-network retry is being supervised
  uint32_t apRetryStart = 0;    // when that retry was kicked
  bool promotePending = false;  // retry joined OK, waiting for an idle moment
                                // to tear the AP down (job in flight)
  bool busy = false;            // mirrored from the app via setBusy()
  uint32_t apLastRetry = 0;     // last retry time (cadence: AP_RETRY_MS)
  uint32_t lastCheck = 0;
};
