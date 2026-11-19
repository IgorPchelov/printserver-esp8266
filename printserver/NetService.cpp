/* NetService.cpp - see NetService.h */
#include "NetService.h"

String urlDecode(String str) {          // PR #29 (user's fix): %XX and '+' handling
  String decoded = "";
  decoded.reserve(str.length());
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '%' && i + 2 < str.length()) {
      char hex[3] = { str.charAt(i+1), str.charAt(i+2), 0 };
      decoded += (char)strtol(hex, NULL, 16);
      i += 2;
    } else if (c == '+') {
      decoded += ' ';
    } else {
      decoded += c;
    }
  }
  return decoded;
}

void NetService::begin() {
  WiFi.mode(WIFI_STA);                   // PR #31: reuse persisted credentials
  WiFi.setHostname(hostName().c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin();                          // connect with saved SSID/pass from NVS
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_STA_TIMEOUT_MS) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
  } else {
    // fall back to configuration AP; AP+STA so a Wi-Fi scan can run
    // without tearing the AP down (client stays connected to the page)
    String ssid = String(AP_SSID_PREFIX) + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid.c_str(), AP_PASSWORD);
    apMode = true;
  }
}

String NetService::hostName() {
  return String(HOST_NAME_PREFIX) + "-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
}

void NetService::connectTo(const String& ssid, const String& pass) {
  if (apMode) { WiFi.softAPdisconnect(true); apMode = false; }
  WiFi.persistent(true);                 // PR #31 (user's fix): save credentials
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName().c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_STA_TIMEOUT_MS) delay(200);
  if (WiFi.status() != WL_CONNECTED) {   // failed: reopen AP so user can retry
    // (AP+STA keeps the scan usable on the retry page)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP((String(AP_SSID_PREFIX) + String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX)).c_str(), AP_PASSWORD);
    apMode = true;
  }
}

String NetService::ip() const {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

// ---- Wi-Fi scan (for the /wifi web page) ------------------------------
void NetService::startScan() {
  if (scanRunning) return;
  // AP+STA handles scanning with the AP up; in pure STA we scan too
  // (brief traffic pause on the STA link, a few seconds).
  scanRunning = true;
  WiFi.scanNetworks(true);               // async
}

int NetService::scanStatus() {
  if (!scanRunning) return 0;            // idle
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n == WIFI_SCAN_FAILED) { scanRunning = false; return -2; }
  scanRunning = false;
  return n;                              // >= 0 results ready
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
      for (unsigned int k = 0; k < ssid.length(); k++) {
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
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }
}
