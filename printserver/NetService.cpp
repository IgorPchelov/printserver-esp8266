/* NetService.cpp - see NetService.h */
#include "NetService.h"
#include <esp_wifi.h>                   // esp_wifi_get_config: the STA config is
                                        // the only reliable source of "which SSID
                                        // are we joining" BEFORE the link is up

bool NetService::forcedAp = false;      // static: see NetService.h

String urlDecode(String str) {          // PR #29 (user's fix): %XX and '+' handling
  String decoded = "";
  decoded.reserve(str.length());
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '%' && i + 2 < str.length()) {
      char h1 = str.charAt(i+1), h2 = str.charAt(i+2);
      auto isHex = [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
      };
      if (isHex(h1) && isHex(h2)) {
        char hex[3] = { h1, h2, 0 };
        decoded += (char)strtol(hex, NULL, 16);
        i += 2;
      } else {
        decoded += c;                    // stray '%', keep it verbatim
      }
    } else if (c == '+') {
      decoded += ' ';
    } else {
      decoded += c;
    }
  }
  return decoded;
}

void NetService::begin() {
  WiFi.persistent(true);                 // PR #31 (user's fix): save credentials
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName().c_str());
  WiFi.setAutoReconnect(true);
  // mDNS reliability (2.4.1): default modem sleep lets the radio doze off
  // between beacons, which delays or loses mDNS responses - the Android
  // printer list then shows the service only for the moment a reply
  // happened to slip through ("shows for a second, then gone"). A print
  // server must be discoverable at all times; power draw is a non-issue
  // on mains-powered hardware.
  WiFi.setSleep(false);
  // how the join is started, and how the display learns the SSID:
  // - stored credentials: plain WiFi.begin() reads them from NVS
  // - a new SSID from the web page: WiFi.begin(ssid, pass) writes them
  //   (persistent(true) above) and starts the join immediately
  // In both cases the STA config (esp_wifi_get_config) holds the target SSID
  // from the very first moment - before the link is up - so joiningSsid()
  // can render "Подкл. к ХХХ" during the whole join.
  WiFi.begin();                          // connect with saved SSID/pass from NVS
  haveStored = joiningSsid().length() > 0;
}

String NetService::joiningSsid() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return "";
  return String((const char*)conf.sta.ssid);   // sta.ssid is uint8_t[33], NUL-padded
}

bool NetService::waitConnected(void (*pump)()) {
  if (!haveStored) {                     // no credentials ever saved: don't make
    startAp();                           // the user wait 20 s for nothing
    return false;
  }
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_STA_TIMEOUT_MS) {
    if (pump) pump(); else delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    return true;
  }
  startAp();
  return false;
}

void NetService::startAp() {
  // AP+STA so a Wi-Fi scan can run without tearing the AP down (the phone
  // stays on the page), but with the background STA join attempts STOPPED.
  // Left running, the auto-rejoin keeps hopping the radio off the AP channel
  // to hunt for the stored network - the phone misses beacons and the AP
  // feels unstable (what the user reported), and any scan the /wifi page
  // started is aborted into WIFI_SCAN_FAILED (the empty network list).
  WiFi.setAutoReconnect(false);          // stop the auto-rejoin loop
  WiFi.disconnect();                     // abort any join/scan in flight
  String ssid = String(AP_SSID_PREFIX) + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid.c_str(), AP_PASSWORD);
  apMode = true;
  promotePending = false;                // any AP (re)start cancels a pending promote
  apLastRetry = millis();
}

String NetService::hostName() {
  return String(HOST_NAME_PREFIX) + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
}

void NetService::connectTo(const String& ssid, const String& pass, void (*pump)()) {
  if (apMode) { WiFi.softAPdisconnect(true); apMode = false; }
  forcedAp = false;                       // new credentials saved: back to normal duty
  apRetryActive = false;
  promotePending = false;
  WiFi.persistent(true);                 // PR #31 (user's fix): save credentials
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName().c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_STA_TIMEOUT_MS) {
    if (pump) pump(); else delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(true);          // router reboot: fast auto-rejoin
    return;
  }
  // failed: reopen AP so the user can retry (AP+STA keeps the scan usable)
  startAp();
}

String NetService::ip() const {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void NetService::forceAp() {
  // Button-requested configuration AP. Different from the fallback AP:
  // the retry loop must stay off (the user deliberately left the home
  // network; hammering the stored one every 2 min would fight their new
  // choice) until new credentials are saved via connectTo().
  forcedAp = true;
  apRetryActive = false;
  promotePending = false;
  startAp();
}

// ---- Wi-Fi scan (for the /wifi web page) ------------------------------
void NetService::startScan() {
  if (scanRunning) return;
  // A scan requested from the /wifi page beats a background stored-network
  // retry: cancel the retry so it cannot abort the scan mid-flight.
  if (apRetryActive) {
    apRetryActive = false;
    WiFi.disconnect();                   // abort the join attempt (not the AP)
  }
  scanRunning = true;
  scanStartMs = millis();
  // Fast scan: ~1.6 s across 13 channels instead of the default ~4 s
  // (300 ms/channel). The shorter the radio is away from the AP channel,
  // the fewer beacons the phone misses while the page polls /scan every
  // second - and the less the STA side of AP+STA is disturbed.
  WiFi.scanNetworks(true, true, false, 120);   // async, show hidden, active, 120 ms/channel
}

int NetService::scanStatus() {
  if (!scanRunning) return 0;            // idle
  // Watchdog: an async scan that neither completes nor fails must not leave
  // the /wifi page spinning on "Поиск сетей..." forever.
  if (millis() - scanStartMs > SCAN_TIMEOUT_MS) {
    scanRunning = false;
    WiFi.scanDelete();
    return -2;                           // report as failed -> manual entry fallback
  }
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n == WIFI_SCAN_FAILED) { scanRunning = false; return -2; }
  scanRunning = false;
  return 1;                              // results ready (even if 0 found)
}

String NetService::scanJson() {
  String json = "[";
  int n = WiFi.scanComplete();
  if (n > 0) {
    for (int i = 0; i < n && i < 20; i++) {   // cap at 20 entries
      if (i) json += ",";
      json += "{\"ssid\":\"";
      String ssid = WiFi.SSID(i);
      // escape for JSON (quotes/backslash/control chars)
      for (unsigned k = 0; k < ssid.length(); k++) {
        char ch = ssid.charAt(k);
        if (ch == '"' || ch == '\\') { json += '\\'; json += ch; }
        else if (ch >= ' ') json += ch;
      }
      json += "\",\"rssi\":" + String(WiFi.RSSI(i));
      json += ",\"sec\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
      json += "}";
    }
  }
  json += "]";
  WiFi.scanDelete();                     // free the result buffer
  return json;
}


String NetService::info() const {
  if (apMode) return "AP mode, IP=" + WiFi.softAPIP().toString();
  if (WiFi.status() == WL_CONNECTED)
    return WiFi.SSID() + " IP=" + WiFi.localIP().toString() + " RSSI=" + String(WiFi.RSSI()) + "dBm";
  return "offline";
}

void NetService::tick() {
  uint32_t now = millis();

  // 1) Never touch the radio while the /wifi page has a scan in flight:
  //    WiFi.reconnect() aborts an async scan (WIFI_SCAN_FAILED -> the empty
  //    network list the user saw). The scan takes ~1.6 s; the STA reconnect
  //    can wait.
  if (scanRunning) return;

  if (apMode) {
    // forced AP (button): the user deliberately left the home network - no
    // retries, no switchover, until new credentials arrive via connectTo()
    if (forcedAp) return;

    // 2) Supervise an in-flight stored-network retry (non-blocking, see
    //    below): success promotes us to STA-only, failure goes back to AP
    //    duty without the radio hunting in the background.
    if (apRetryActive) {
      if (WiFi.status() == WL_CONNECTED) {
        apRetryActive = false;
        promotePending = true;              // joined OK - tear the AP down at the
                                            // next idle moment (a job arriving over
                                            // the AP must not die mid-page)
      } else if (now - apRetryStart > AP_RETRY_TIMEOUT_MS) {
        apRetryActive = false;
        WiFi.disconnect();                  // stop the hunt, back to clean AP duty
      }
      return;
    }
    if (promotePending) {
      // retry succeeded earlier: finish the switchover once the engine is idle
      if (!busy) {
        promotePending = false;
        apMode = false;
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
      }
      return;
    }
    // 3) Periodic single retry of the stored network so power-cycling the
    //    router is enough to recover (no reboot of the ESP needed): one
    //    deliberate, supervised attempt every AP_RETRY_MS.
    if (now - apLastRetry > AP_RETRY_MS) {
      apLastRetry = now;
      if (haveStored) {
        apRetryActive = true;
        apRetryStart = now;
        WiFi.begin();                       // stored credentials, non-blocking
      }
    }
    return;
  }

  if (now - lastCheck < 5000) return;
  lastCheck = now;
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }
}
