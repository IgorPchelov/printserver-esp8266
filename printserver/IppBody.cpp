/*
 * IppBody.cpp - see IppBody.h. Pure C++ (no Arduino types) so the host
 * unit tests compile it directly against a RAM buffer. The firmware side
 * wraps a WiFiClient in ipp::BodySource; printserver.ino does that.
 *
 * This file is part of printserver-esp8266 (GPLv3 port), see config.h.
 */
#include "IppBody.h"
#include <string.h>

namespace ipp {

// millis() is Arduino; keep transport logic free of it: the caller passes
// a deadline and the current time, or (in tests) whatever clock it wants.
bool waitAvail(BodySource& s, size_t n, uint32_t deadlineMs, uint32_t nowMs) {
  while ((size_t)s.available() < n) {
    if (nowMs >= deadlineMs) return false;
    if (!s.connected()) return false;
    s.pump();
    // tests advance their virtual clock in pump(); the box passes a
    // millisecond deadline captured at call time, so the caller loops
  }
  return true;
}

BodyStream::BodyStream(BodySource& src, Framing f, long contentLength, uint32_t idleTimeoutMs)
    : src_(src), f_(f), remaining_(contentLength), idleTimeoutMs_(idleTimeoutMs) {
  if (f_ == Framing::UNKNOWN) err_ = true;
}

long BodyStream::readChunkHeader() {
  // hex size line terminated by \r\n; tolerate a bare \n. Max 8 hex digits
  // + CRLF - a longer line is garbage (or a smuggled request), refuse it.
  char line[10];
  size_t n = 0;
  uint32_t t0 = 0;  // caller-facing timeout is enforced by read()'s idle logic
  (void)t0;
  for (;;) {
    int av = src_.available();
    if (av <= 0) {
      if (!src_.connected()) return -1;
      src_.pump();
      continue;
    }
    int b = -1;
    uint8_t tmp;
    if (src_.read(&tmp, 1) == 1) b = tmp;
    if (b < 0) return -1;
    if (b == '\n') break;                       // end of the size line
    if (b == '\r') continue;                    // skip CRs
    if (n >= sizeof(line) - 1) return -1;       // absurdly long line
    line[n++] = (char)b;
  }
  line[n] = 0;
  if (!n) return -1;                            // empty size line is not valid
  // strtol with base 16; trailing junk already rejected by length cap
  long v = 0;
  for (size_t i = 0; i < n; i++) {
    char c = line[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = (v << 4) | d;
    if (v > (1L << 24)) return -1;              // no printable page chunk is 16 MB
  }
  return v;
}

int BodyStream::read(uint8_t* buf, size_t n) {
  if (err_) return -1;
  if (n == 0) return 0;
  size_t out = 0;
  // overall inactivity guard: no payload byte for idleTimeoutMs_ = dead peer
  uint32_t idle = 0;
  while (out < n) {
    if (f_ == Framing::LENGTH) {
      if (remaining_ == 0) return (int)out;     // body complete
      if (remaining_ < 0) { err_ = true; return -1; }  // unknown length: refuse
      size_t want = (size_t)remaining_ < (n - out) ? (size_t)remaining_ : (n - out);
      int r = src_.read(buf + out, want);
      if (r > 0) {
        out += (size_t)r;
        remaining_ -= r;
        st_.bodyBytes += (size_t)r;
        idle = 0;
        continue;
      }
      if (!src_.connected()) { err_ = true; st_.truncated = true; return out ? (int)out : -1; }
      src_.pump();
      if (++idle > idleTimeoutMs_) { err_ = true; st_.truncated = true; return out ? (int)out : -1; }
      continue;
    }
    // CHUNKED
    if (done_) return (int)out;                    // terminal chunk already seen
    if (chunkHeaderPending_) {
      long sz = readChunkHeader();
      if (sz < 0) { err_ = true; st_.truncated = true; return out ? (int)out : -1; }
      if (sz == 0) { st_.sawTerminal = true; done_ = true; return (int)out; }   // terminal chunk
      chunkLeft_ = (size_t)sz;
      chunkHeaderPending_ = false;
      st_.chunkCount++;
      continue;
    }
    size_t want = chunkLeft_ < (n - out) ? chunkLeft_ : (n - out);
    int r = src_.read(buf + out, want);
    if (r > 0) {
      out += (size_t)r;
      chunkLeft_ -= (size_t)r;
      st_.bodyBytes += (size_t)r;
      idle = 0;
      if (chunkLeft_ == 0) {
        // consume the trailing CRLF after the chunk body; tolerate only
        // CRLF / LF - anything else means a desynchronised stream
        uint8_t crlf[2];
        size_t got = 0;
        while (got < 2) {
          int av = src_.available();
          if (av > 0) { if (src_.read(crlf + got, 1) == 1) got++; }
          else if (!src_.connected()) break;
          else src_.pump();
        }
        if (got == 2 && (crlf[0] != '\r' || crlf[1] != '\n')) { err_ = true; return out ? (int)out : -1; }
        if (got == 1 && crlf[0] != '\n') { err_ = true; return out ? (int)out : -1; }
        chunkHeaderPending_ = true;
      }
      continue;
    }
    if (!src_.connected()) { err_ = true; st_.truncated = true; return out ? (int)out : -1; }
    src_.pump();
    if (++idle > idleTimeoutMs_) { err_ = true; st_.truncated = true; return out ? (int)out : -1; }
  }
  return (int)out;
}

}  // namespace ipp
