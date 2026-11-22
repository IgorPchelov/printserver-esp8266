// Minimal Arduino/ESP32 core stubs so every firmware module can be
// SYNTAX-checked on the host (g++ -fsyntax-only). NOT a behaviour
// emulation: method bodies are empty / return defaults.
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>

typedef uint8_t byte;
typedef std::string _StdStr;

#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define HEX 16
#define LOW 0
#define HIGH 1

// ---- Arduino String (just enough surface; heap semantics ignored) ----
class String : public _StdStr {
public:
  String() {}
  String(const char* s) : _StdStr(s ? s : "") {}
  String(const _StdStr& s) : _StdStr(s) {}
  String(char c) : _StdStr(1, c) {}
  String(int v) : _StdStr(std::to_string(v)) {}
  String(unsigned v) : _StdStr(std::to_string(v)) {}
  String(long v) : _StdStr(std::to_string(v)) {}
  String(unsigned long v) : _StdStr(std::to_string(v)) {}
  String(uint32_t v, int base) : _StdStr("hex") {}
  const char* c_str() const { return _StdStr::c_str(); }
  unsigned length() const { return (unsigned)_StdStr::size(); }
  char charAt(unsigned i) const { return (*this)[i]; }
  int indexOf(char c) const { auto p = _StdStr::find(c); return p == npos ? -1 : (int)p; }
  int indexOf(const char* s) const { auto p = _StdStr::find(s); return p == npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = _StdStr::rfind(c); return p == npos ? -1 : (int)p; }
  int lastIndexOf(const char* s) const { auto p = _StdStr::rfind(s); return p == npos ? -1 : (int)p; }
  String substring(unsigned a) const { return String(_StdStr::substr(a)); }
  String substring(unsigned a, unsigned b) const { return String(_StdStr::substr(a, b - a)); }
  bool startsWith(const char* s) const { return _StdStr::compare(0, strlen(s), s) == 0; }
  bool endsWith(const char* s) const {
    size_t n = strlen(s); return size() >= n && _StdStr::compare(size() - n, n, s) == 0;
  }
  void toLowerCase() { for (auto& c : *this) c = tolower((unsigned char)c); }
  int toInt() const { return atoi(c_str()); }
  void reserve(size_t) {}
  String operator+(const String& o) const { return String(_StdStr(*this) + _StdStr(o)); }
  String& operator+=(const String& o) { _StdStr::append(o); return *this; }
  String& operator+=(const char* o) { _StdStr::append(o); return *this; }
  String& operator+=(char c) { _StdStr::push_back(c); return *this; }
  // numeric overloads - without them `str += someUint32` binds to
  // operator+=(char) and silently builds garbage (see host_littlefs shim)
  String& operator+=(int v) { _StdStr::append(std::to_string(v)); return *this; }
  String& operator+=(unsigned v) { _StdStr::append(std::to_string(v)); return *this; }
  String& operator+=(long v) { _StdStr::append(std::to_string(v)); return *this; }
  String& operator+=(unsigned long v) { _StdStr::append(std::to_string(v)); return *this; }
};
inline String operator+(const char* a, const String& b) { return String(a) + b; }
inline bool operator==(const String& a, const char* b) { return a.compare(b) == 0; }

inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline int min(int a, int b) { return a < b ? a : b; }
inline size_t min(size_t a, size_t b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }

class HardwareSerial {
public:
  void begin(unsigned long) {}
  void println(const char* = "") {}
  void println(const String&) {}
  void printf(const char*, ...) {}
};
extern HardwareSerial Serial;

class EspClass {
public:
  uint64_t getEfuseMac() { return 0; }
  uint32_t getFreeHeap() { return 0; }
};
extern EspClass ESP;

inline void neopixelWrite(int, uint8_t, uint8_t, uint8_t) {}

// FS/File (LittleFS surface)
class File {
public:
  operator bool() const { return false; }
  bool isDirectory() const { return false; }
  String name() const { return String(); }
  size_t size() const { return 0; }
  size_t available() const { return 0; }
  File openNextFile() { return File(); }
  int read() { return -1; }
  size_t read(uint8_t*, size_t) { return 0; }
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t*, size_t) { return 0; }
  void flush() {}
  void close() {}
};
class FSClass {
public:
  bool begin(bool = false) { return true; }
  bool mkdir(const char*) { return true; }
  bool remove(const String&) { return true; }
  bool rename(const String&, const String&) { return true; }
  File open(const String&, const char* = "r") { return File(); }
  size_t totalBytes() const { return 16 * 1024 * 1024; }
  size_t usedBytes() const { return 0; }
};
extern FSClass LittleFS;
