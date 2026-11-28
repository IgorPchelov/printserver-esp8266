/*
 * IppBody.h - minimal RFC 8011 request-body reader for the ESP32 print server.
 *
 * WHY THIS FILE EXISTS (2.4.0). The old handleIpp() read the 8-byte IPP
 * header and walked attribute groups straight off the socket. With a plain
 * Content-Length body that happens to work; with Transfer-Encoding: chunked
 * (what Android's IPP stack and curl --data-binary @... -H 'Transfer-
 * Encoding: chunked' send) it reads chunk-size lines and CRLFs as IPP
 * bytes: the operation id comes out garbage, the request is rejected with
 * client-error-operation-not-supported, and the client retries over and
 * over - exactly the "tens of POST /ipp/print with no verdict" the serial
 * monitor showed. This module gives both transport framings one
 * byte-stream view, and adds an early short-circuit for Print-Job so the
 * spool loop can stream the document instead of buffering it.
 *
 * Host-testable by design: no WiFi types, no Serial. The owner supplies
 * a Reader (pulls bytes from wherever they come from) and a Pump (called
 * while waiting - OLED/LED/LPT keepalive on the box, no-op in tests).
 *
 * This file is part of printserver-esp8266 (GPLv3 port), see config.h.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace ipp {

// ---- transport framing of the HTTP body ----
enum class Framing : uint8_t { UNKNOWN, LENGTH, CHUNKED };

// Bytes already handed out AND framing verdicts, for the serial log line.
struct Stats {
  size_t bodyBytes;      // payload bytes consumed from the transport
  size_t chunkCount;     // chunks started (diagnostic)
  bool   sawTerminal;    // chunked: the terminal 0-size chunk arrived
  bool   truncated;      // promised bytes never came / no terminal chunk
};

// Pull interface the caller implements over its socket (or test buffer):
//   read(buf, n) -> how many bytes were placed in buf (<= n)
//   available()  -> bytes readable RIGHT NOW without blocking
//   connected()  -> false once the peer is gone for good
// pump() is invoked while waiting for data (panel keep-alive on the box).
class BodySource {
public:
  virtual ~BodySource() {}
  virtual int   read(uint8_t* buf, size_t n) = 0;
  virtual int   available() = 0;
  virtual bool  connected() = 0;
  virtual void  pump() = 0;
};

// Little helper for "wait for >= n readable (or timeout/peer-death)":
// returns false if the wait gave up (caller treats as truncated).
bool waitAvail(BodySource& s, size_t n, uint32_t deadlineMs, uint32_t nowMs);

// The byte-stream view over both framings. Usage:
//   Body src(...);  Framing f = detect(headers);
//   BodyStream bs(src, f, contentLength, idleTimeoutMs);
//   bs.read(buf, n)  -> payload bytes (de-chunked when chunked)
//   bs.lastError()   -> "" while healthy
// All waits call src.pump() and respect src.connected().
class BodyStream {
public:
  BodyStream(BodySource& src, Framing f, long contentLength, uint32_t idleTimeoutMs);
  // De-chunking reads the size line itself; requires the reader to expose
  // line reads. For simplicity BodyStream implements its own tiny
  // line reader over read().
  int  read(uint8_t* buf, size_t n);         // payload bytes, 0 = wait, <0 = ended
  const Stats& stats() const { return st_; }
  Framing framing() const { return f_; }
  bool lastError() const { return err_; }    // true = transport is broken
private:
  BodySource& src_;
  Framing f_;
  long remaining_ = 0;                       // LENGTH: promised bytes left
  size_t chunkLeft_ = 0;                     // CHUNKED: bytes left in this chunk
  bool chunkHeaderPending_ = true;           // need to read a size line first
  uint32_t idleTimeoutMs_;
  bool done_ = false;      // terminal chunk seen; later reads return 0
  Stats st_ = {};
  bool err_ = false;
  // read a \r\n-terminated hex size line; returns -1 on transport trouble
  long readChunkHeader();
};

}  // namespace ipp
