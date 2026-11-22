// Minimal Arduino.h for host-side unit tests that need String only.
// File / FSClass / LittleFS live in LittleFS.h (a real littlefs shim) -
// see tests/host_littlefs/README note in run_tests.sh.
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>

typedef uint8_t byte;

#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define LOW 0
#define HIGH 1
#define HEX 16

// ---- Arduino String (surface used by PrintQueue.cpp; heap-free) ----
class String : public std::string {
public:
  String() {}
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(char c) : std::string(1, c) {}
  String(int v) : std::string(std::to_string(v)) {}
  String(unsigned v) : std::string(std::to_string(v)) {}
  String(long v) : std::string(std::to_string(v)) {}
  String(unsigned long v) : std::string(std::to_string(v)) {}
  String(uint32_t v, int base) : std::string("hex") {}
  const char* c_str() const { return std::string::c_str(); }
  unsigned length() const { return (unsigned)std::string::size(); }
  char charAt(unsigned i) const { return (*this)[i]; }
  int indexOf(char c) const { auto p = std::string::find(c); return p == npos ? -1 : (int)p; }
  int indexOf(const char* s) const { auto p = std::string::find(s); return p == npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = std::string::rfind(c); return p == npos ? -1 : (int)p; }
  int lastIndexOf(const char* s) const { auto p = std::string::rfind(s); return p == npos ? -1 : (int)p; }
  String substring(unsigned a) const { return String(std::string::substr(a)); }
  String substring(unsigned a, unsigned b) const { return String(std::string::substr(a, b - a)); }
  bool startsWith(const char* s) const { return std::string::compare(0, strlen(s), s) == 0; }
  bool endsWith(const char* s) const {
    size_t n = strlen(s); return size() >= n && std::string::compare(size() - n, n, s) == 0;
  }
  void toLowerCase() { for (auto& c : *this) c = tolower((unsigned char)c); }
  int toInt() const { return atoi(c_str()); }
  void reserve(size_t) {}
  String operator+(const String& o) const { return String(std::string(*this) + std::string(o)); }
  String& operator+=(const String& o) { std::string::append(o); return *this; }
  String& operator+=(const char* o) { std::string::append(o); return *this; }
  String& operator+=(char c) { std::string::push_back(c); return *this; }
  // numeric overloads: without them `str += someUint32` binds to
  // operator+=(char) (a standard integral conversion outranks the
  // user-defined one to String) and quietly builds garbage filenames -
  // exactly what the first run of test_queue caught.
  String& operator+=(int v) { std::string::append(std::to_string(v)); return *this; }
  String& operator+=(unsigned v) { std::string::append(std::to_string(v)); return *this; }
  String& operator+=(long v) { std::string::append(std::to_string(v)); return *this; }
  String& operator+=(unsigned long v) { std::string::append(std::to_string(v)); return *this; }
};
inline String operator+(const char* a, const String& b) { return String(a) + b; }
inline bool operator==(const String& a, const char* b) { return a.compare(b) == 0; }

inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
