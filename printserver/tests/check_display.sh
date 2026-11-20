#!/bin/sh
# Host syntax/semantic check of the firmware sources, focused on the files
# touched by the OLED text changes. Arduino ESP32 core / U8g2 / WiFi / mDNS /
# LittleFS are stubbed with minimal fakes so g++ -fsyntax-only compiles the
# REAL firmware sources (not mocks of them). No hardware needed.
set -e
cd "$(dirname "$0")/../.."   # repo root (script lives in printserver/tests/)
OUT=$(mktemp -d)
mkdir -p "$OUT/fake"

cat > "$OUT/fake/Arduino.h" <<'EOF'
// self-contained Arduino ESP32 core stub
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <string>
#include <algorithm>
using std::min;
using std::max;
typedef uint8_t byte;
class String {
  std::string s;
public:
  String() {}
  String(const char* p) : s(p ? p : "") {}
  String(const std::string& v) : s(v) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v) : s(std::to_string(v)) {}
  String(uint32_t v, int base) {
    char b[33]; const char* digits = "0123456789abcdef"; unsigned i = 0;
    if (!v) b[i++] = '0';
    while (v) { b[i++] = digits[v % base]; v /= base; }
    b[i] = 0; char r[33];
    for (unsigned k = 0; k < i; k++) r[k] = b[i - 1 - k];
    r[i] = 0; s = r; }
  String(uint64_t v, int base) { char b[64]; char* p = b + 64; *(--p) = 0;
    if (!v) *(--p) = '0';
    while (v) { int d = v % base; *(--p) = d < 10 ? '0' + d : 'a' + d - 10; v /= base; }
    s = p; }
  const char* c_str() const { return s.c_str(); }
  unsigned int length() const { return (unsigned int)s.size(); }
  char charAt(unsigned int i) const { return i < s.size() ? s[i] : 0; }
  void reserve(unsigned int) {}
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  String substring(unsigned a) const { return String(s.substr(a)); }
  int indexOf(const char* p, unsigned from = 0) const {
    auto r = s.find(p, from); return r == std::string::npos ? -1 : (int)r; }
  int indexOf(char c, unsigned from = 0) const {
    auto r = s.find(c, from); return r == std::string::npos ? -1 : (int)r; }
  int lastIndexOf(const char* p) const {
    auto r = s.rfind(p); return r == std::string::npos ? -1 : (int)r; }
  int lastIndexOf(char c) const {
    auto r = s.rfind(c); return r == std::string::npos ? -1 : (int)r; }
  bool startsWith(const String& o) const { return s.rfind(o.s, 0) == 0; }
  bool endsWith(const String& o) const {
    return s.size() >= o.s.size() && s.compare(s.size() - o.s.size(), o.s.size(), o.s) == 0; }
  void replace(const String& a, const String& b) {
    std::string out; size_t pos = 0;
    while (true) { size_t hit = s.find(a.s, pos);
      if (hit == std::string::npos) { out += s.substr(pos); break; }
      out += s.substr(pos, hit - pos); out += b.s; pos = hit + a.s.size(); }
    s = out; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* p) { s += p; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
  int toInt() const { return atoi(s.c_str()); }
  void toLowerCase() { for (auto& c : s) c = tolower((unsigned char)c); }
  void toUpperCase() { for (auto& c : s) c = toupper((unsigned char)c); }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1); }
  friend String operator+(String a, const String& b) { a.s += b.s; return a; }
  friend String operator+(String a, const char* p) { a.s += p; return a; }
  friend String operator+(const char* p, const String& b) { String r(p); r.s += b.s; return r; }
};
inline uint32_t millis() { static uint32_t t; return t += 10; }
struct SerialClass {
  void begin(unsigned long) {}
  void print(const String&) {}
  void println(const String&) {}
  void printf(const char*, ...) {}
};
extern SerialClass Serial;
inline void delay(unsigned) {}
inline void delayMicroseconds(unsigned) {}
inline void yield() {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 1; }
inline void neopixelWrite(int, uint8_t, uint8_t, uint8_t) {}   // WS2812 (StatusLed)
#define INPUT 0x1
#define OUTPUT 0x3
#define INPUT_PULLUP 0x5
#define INPUT_PULLDOWN 0x9
#define HIGH 1
#define LOW 0
#define HEX 16
class EspClass {
public:
  uint64_t getEfuseMac() { return 0x1122334455667788ULL; }
  uint32_t getFreeHeap() { return 300000; }
};
extern EspClass ESP;
EOF

cat > "$OUT/fake/U8g2lib.h" <<'EOF'
// minimal U8g2 stub: only the calls StatusDisplay.cpp makes
#pragma once
#include <Arduino.h>
#define U8G2_R0 0
#define U8X8_PIN_NONE 255
extern const uint8_t u8g2_font_6x12_t_cyrillic[];
class U8G2 {
public:
  void setI2CAddress(int) {}
  void begin() {}
  void setFontMode(int) {}
  void setFontDirection(int) {}
  void setContrast(int) {}
  void setPowerSave(int) {}
  void clearBuffer() {}
  void sendBuffer() {}
  void setFont(const uint8_t[]) {}
  uint16_t getUTF8Width(const char* str) { return (uint16_t)(strlen(str) * 6); }
  void drawUTF8(int16_t, int16_t, const char*) {}
  void drawVLine(uint16_t, uint16_t, uint16_t) {}
  void drawXBMP(int16_t, int16_t, uint16_t, uint16_t, const uint8_t[]) {}
};
class U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C : public U8G2 {
public:
  U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C(int, int, int, int) {}
};
EOF

cat > "$OUT/fake/esp_wifi.h" <<'EOF'
// minimal esp_wifi stub: esp_wifi_get_config (STA config) + wifi_config_t
#pragma once
#include <cstdint>
#include <cstring>
typedef enum { ESP_OK = 0 } esp_err_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP = 1 } wifi_interface_t;
typedef struct {
  uint8_t ssid[33];
  uint8_t password[65];
} wifi_sta_config_t;
typedef union {
  wifi_sta_config_t sta;
} wifi_config_t;
inline esp_err_t esp_wifi_get_config(wifi_interface_t, wifi_config_t* out) {
  memset(out, 0, sizeof(*out));
  return ESP_OK;
}
EOF

cat > "$OUT/fake/WiFi.h" <<'EOF'
// minimal WiFi stub
#pragma once
#include <Arduino.h>
struct FakeIP { String toString() const { return "192.168.4.1"; } };
enum { WL_CONNECTED = 3, WL_IDLE_STATUS = 0 };
enum wifi_mode_t { WIFI_MODE_NULL, WIFI_STA, WIFI_AP, WIFI_AP_STA };
enum { WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2, WIFI_AUTH_OPEN = 0 };
class WiFiClient;   // fwd
class WiFiServer {
public:
  explicit WiFiServer(int) {}
  void begin() {}
  WiFiClient accept();
};
class WiFiClient {
public:
  operator bool() const { return false; }
  bool connected() { return false; }
  void stop() {}
  int available() { return 0; }
  int read(uint8_t*, int) { return 0; }
  int read() { return -1; }
  size_t write(const uint8_t*, size_t n) { return n; }
  size_t write(uint8_t) { return 1; }
  void print(const String&) {}
  void println(const String&) {}
  void printf(const char*, ...) {}
  String readStringUntil(char) { return ""; }
  String readString() { return ""; }
  void setNoDelay(bool) {}
  friend class WiFiServer;
};
class WiFiClass {
public:
  void mode(wifi_mode_t) {}
  void setHostname(const char*) {}
  void setAutoReconnect(bool) {}
  void begin() {}
  bool begin(const char*, const char*) { return true; }
  int status() { return WL_CONNECTED; }
  void reconnect() {}
  bool softAP(const char*, const char*) { return true; }
  bool softAPdisconnect(bool) { return true; }
  void persistent(bool) {}
  int16_t scanNetworks(bool) { return 0; }
  // full ESP32 signature: async, show_hidden, passive, max_ms_per_chan,
  // ... (defaults for the rest)
  int16_t scanNetworks(bool, bool, bool, uint16_t) { return 0; }
  bool disconnect() { return true; }
  int16_t scanComplete() { return 0; }
  void scanDelete() {}
  String SSID() { return "net"; }
  String SSID(int) { return "net"; }
  int32_t RSSI(int) { return -50; }
  int32_t RSSI() { return -50; }
  int encryptionType(int) { return WIFI_AUTH_OPEN; }
  FakeIP softAPIP() const { return {}; }
  FakeIP localIP() const { return {}; }
};
extern WiFiClass WiFi;
inline WiFiClient WiFiServer::accept() { return {}; }
EOF

cat > "$OUT/fake/ESPmDNS.h" <<'EOF'
#pragma once
#include <Arduino.h>
class MDNSResponder {
public:
  bool begin(const char*) { return true; }
  void setInstanceName(const char*) {}
  void addService(const char*, const char*, int) {}
  void addServiceTxt(const char*, const char*, const char*, const String&) {}
};
extern MDNSResponder MDNS;
EOF

cat > "$OUT/fake/LittleFS.h" <<'EOF'
#pragma once
#include <Arduino.h>
class File {
public:
  operator bool() const { return false; }
  bool isDirectory() const { return false; }
  String name() const { return ""; }
  size_t size() const { return 0; }
  size_t write(const uint8_t*, size_t n) { return n; }
  size_t write(uint8_t) { return 1; }
  int read(uint8_t*, int) { return 0; }
  int read() { return -1; }
  void close() {}
  bool seek(uint32_t) { return true; }
  uint32_t position() const { return 0; }
  File openNextFile() { return {}; }
};
class FSClass {
public:
  bool begin(bool = false) { return true; }
  bool exists(const char*) { return false; }
  bool mkdir(const char*) { return true; }
  bool rmdir(const char*) { return true; }
  bool remove(const char*) { return true; }
  File open(const char*, const char* = "r") { return {}; }
  size_t totalBytes() { return 0; }
  size_t usedBytes() { return 0; }
};
extern FSClass LittleFS;
EOF

cat > "$OUT/fake/defs.cpp" <<'EOF'
#include "Arduino.h"
#include "WiFi.h"
#include "ESPmDNS.h"
#include "LittleFS.h"
const uint8_t u8g2_font_6x12_t_cyrillic[1] = {0};
SerialClass Serial;
EspClass ESP;
WiFiClass WiFi;
MDNSResponder MDNS;
FSClass LittleFS;
EOF

for f in StatusDisplay StatusLed ParallelPort NetService; do
  echo "=== $f.cpp"
  g++ -std=c++17 -fsyntax-only -I printserver -I "$OUT/fake" printserver/$f.cpp
done
echo "=== printserver.ino (as C++)"
cp printserver/printserver.ino "$OUT/fake/printserver_ino.cpp"
g++ -std=c++17 -fsyntax-only -I printserver -I "$OUT/fake" "$OUT/fake/printserver_ino.cpp"
echo "SYNTAX OK (all 5 files)"
rm -rf "$OUT"
