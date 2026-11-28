// WiFi / esp_wifi / mDNS / U8g2 stubs for host syntax checking. See Arduino.h.
#pragma once
#include "Arduino.h"

#define WL_CONNECTED 3
#define WIFI_STA 1
#define WIFI_AP_STA 3
#define WIFI_AUTH_OPEN 0
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

class WiFiClient {
public:
  operator bool() const { return false; }
  bool connected() { return false; }
  int available() { return 0; }
  int read() { return -1; }
  size_t read(uint8_t*, size_t) { return 0; }
  String readStringUntil(char) { return String(); }
  String readString() { return String(); }
  size_t print(const String&) { return 0; }
  size_t print(const char*) { return 0; }
  size_t printf(const char*, ...) { return 0; }
  void stop() {}
};

class IPAddress {
public:
  String toString() const { return String("0.0.0.0"); }
};

class WiFiServer {
public:
  WiFiServer(uint16_t) {}
  void begin() {}
  WiFiClient accept() { return WiFiClient(); }
};

typedef struct { struct { uint8_t ssid[33]; } sta; } wifi_config_t;
#define WIFI_IF_STA 0
#define ESP_OK 0
inline int esp_wifi_get_config(int, wifi_config_t*) { return ESP_OK; }

class WiFiClass {
public:
  void persistent(bool) {}
  void mode(int) {}
  bool setHostname(const String&) { return true; }
  void setAutoReconnect(bool) {}
  void setSleep(bool) {}
  void begin(const char* = 0, const char* = 0) {}
  bool disconnect(bool = false) { return true; }
  void reconnect() {}
  int status() { return 0; }
  int16_t scanNetworks(bool, bool, bool, uint16_t, uint8_t = 255, bool = false) { return 0; }
  int16_t scanComplete() { return 0; }
  void scanDelete() {}
  bool softAP(const char*, const char*) { return true; }
  bool softAPdisconnect(bool) { return true; }
  IPAddress softAPIP() const { return IPAddress(); }
  IPAddress localIP() const { return IPAddress(); }
  String SSID() const { return String(); }
  String SSID(int) const { return String(); }
  int RSSI() { return 0; }
  int RSSI(int) { return 0; }
  int encryptionType(int) { return 0; }
};
extern WiFiClass WiFi;

class MDNSClass {
public:
  bool begin(const char*) { return true; }
  void setInstanceName(const char*) {}
  void addService(const char*, const char*, uint16_t) {}
  void addServiceTxt(const char*, const char*, const char*, const String&) {}
};
extern MDNSClass MDNS;

// ---- U8g2 ----
#define U8G2_R0 0
#define U8X8_PIN_NONE (-1)
class U8G2 {
public:
  U8G2(int, int, int, int) {}
  U8G2() {}
  void setI2CAddress(int) {}
  bool begin() { return true; }
  void setFontMode(int) {}
  void setFontDirection(int) {}
  void setContrast(int) {}
  void setPowerSave(int) {}
  void clearBuffer() {}
  void sendBuffer() {}
  void setFont(const void*) {}
  uint16_t getUTF8Width(const char*) { return 0; }
  void drawUTF8(int, int, const char*) {}
  void drawVLine(int, int, int) {}
  void drawHLine(int, int, int) {}
  void drawFrame(int, int, int, int) {}
  void drawBox(int, int, int, int) {}
  void drawPixel(int, int) {}
};
typedef U8G2 U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C;
extern const void* u8g2_font_6x12_t_cyrillic;
