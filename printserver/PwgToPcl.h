/*
 * PwgToPcl.h - streaming image/pwg-raster decoder + PCL 5 generator.
 *
 * PWG raster format (PWG 5102.4, verified against libcups raster-stream.c
 * and Android's lib_pwg.c producer):
 *   - 4-byte sync 0x52615332 ("RaS2", big-endian)
 *   - per page: 1796-byte cups_page_header2_t, multi-byte ints big-endian
 *   - then rows, compressed with modified PackBits:
 *       repeatCount byte N: this row is repeated (N+1) times
 *       per row: control bytes where
 *         0..127   -> repeat following bpp bytes (n+1) times
 *         128      -> fill rest of line with 0xFF (white)
 *         129..255 -> copy (257-n) literal bytes
 *     bpp = bytes per pixel (1 for 8-bit gray/SW, 3 for sRGB)
 *
 * PCL 5 output (generic PCL laser printer): reset, resolution, raster
 * graphics mode with 1 bit/pixel rows, end graphics, form feed per page.
 *
 * The class is a pull-parser: feed() consumes bytes as they arrive from
 * the spool file; output PCL bytes are pushed to the LPT port through a
 * callback. All state lives in the object (no globals), so it can also be
 * unit-tested on the host (see tests/test_pwg.cpp).
 *
 * Part of the GPLv3 port of gianluca-nitti/printserver-esp8266.
 */
#pragma once
#include <Arduino.h>
#include "config.h"

#define PWG_SYNC       0x52615332u
#define PWG_HDR_SIZE   1796
// verified with a host-compiled offsetof() run (tests/hdrsize.c):
#define PWG_OFF_HWRES_X    276
#define PWG_OFF_HWRES_Y    280
#define PWG_OFF_NUMCOPIES  348
#define PWG_OFF_PAGESIZE   352   // PageSize[2], points
#define PWG_OFF_WIDTH      372
#define PWG_OFF_HEIGHT     376
#define PWG_OFF_BPP        388   // cupsBitsPerPixel
#define PWG_OFF_BYTESLINE  392
#define PWG_OFF_COLORSPACE 400
#define PWG_OFF_INTEGER    452   // cupsInteger[16]

class PwgToPcl {
public:
  typedef bool (*OutFn)(void* ctx, uint8_t b);   // return false to abort

  void begin(OutFn out, void* ctx) {
    this->out = out; this->ctx = ctx;
    phase = Phase::SYNC;
    fill = 0; syncVal = 0;
    pages = rowsOut = inRow = 0;
    lastErr = nullptr;
    headerDone = false;
  }

  // Feed one input byte. Returns false on unrecoverable error (see lastError).
  bool feed(uint8_t b) {
    switch (phase) {
      case Phase::SYNC:
        syncVal = (syncVal << 8) | b;
        if (++fill == 4) {
          if (syncVal != PWG_SYNC) { lastErr = "bad sync"; return false; }
          phase = Phase::HDR; fill = 0;
        }
        break;
      case Phase::HDR:
        hdr[fill++] = b;
        if (fill == PWG_HDR_SIZE) {
          if (!parseHeader()) { lastErr = "bad header"; return false; }
          emitPagePrologue();
          phase = Phase::ROWCTL; fill = 0;
        }
        break;
      case Phase::ROWCTL:
        rowRepeat = (uint32_t)b + 1;
        phase = Phase::LINE;
        colByte = 0;
        break;
      case Phase::LINE: {
        // decode one compressed line into lineBuf
        if (b == 128) {                    // fill rest with white
          while (colByte < hdrBytesLine) lineBuf[colByte++] = 0xFF;
          finishLine();
        } else if (b & 0x80) {             // literal run: (257-b)*bpp bytes
          runMode = 1; runLeft = (uint32_t)(257 - b) * hdrBpp;
          phase = Phase::LIT;
        } else {                           // repeat run: ((b+1)*bpp) bytes of one pixel
          runMode = 2; runLeft = (uint32_t)b + 1;   // pixel repetitions
          pxIdx = 0;
          phase = Phase::REP;
        }
        break;
      }
      case Phase::LIT:
        if (colByte < hdrBytesLine) lineBuf[colByte++] = b;
        if (--runLeft == 0) {
          if (colByte >= hdrBytesLine) finishLine();
          else phase = Phase::LINE;
        }
        break;
      case Phase::REP:
        px[pxIdx++] = b;
        if (pxIdx == hdrBpp) {             // got the pixel to repeat
          pxIdx = 0;
          while (runLeft-- > 0 && colByte < hdrBytesLine) {
            // gray: use the (single) byte; rgb: average -> luma
            uint8_t g = (hdrBpp == 1) ? px[0]
                       : (uint8_t)((px[0] * 30 + px[1] * 59 + px[2] * 11) / 100);
            lineBuf[colByte++] = g;
          }
          if (colByte >= hdrBytesLine) finishLine();
          else phase = Phase::LINE;
        }
        break;
      case Phase::DONE:
        break;
    }
    return true;
  }

  // Optional: call when input ends; emits trailing form feed if a page is open.
  void end() {
    if (headerDone) emitPageEpilogue();
    phase = Phase::DONE;
  }

  uint32_t pagesPrinted() const { return pages; }
  uint32_t rowsPrinted()  const { return rowsOut; }
  const char* lastError() const { return lastErr ? lastErr : ""; }

private:
  enum class Phase : uint8_t { SYNC, HDR, ROWCTL, LINE, LIT, REP, DONE };
  OutFn out = nullptr; void* ctx = nullptr;
  Phase phase = Phase::SYNC;
  uint32_t syncVal = 0, fill = 0;
  uint8_t hdr[PWG_HDR_SIZE];
  bool headerDone = false;
  // parsed header
  uint32_t hdrW = 0, hdrH = 0, hdrBpp = 1, hdrBytesLine = 0, hdrCs = 0;
  uint32_t hdrResX = 300, hdrResY = 300;
  uint32_t rowRepeat = 0, runLeft = 0, runMode = 0, pxIdx = 0;
  uint8_t px[3];
  uint32_t colByte = 0;      // decoded bytes in current line
  uint32_t inRow = 0;        // source rows consumed for current page
  uint32_t outRow = 0;       // rows emitted (incl. repeats)
  uint32_t pages = 0, rowsOut = 0;
  const char* lastErr = nullptr;
  // 1-bit PCL output row: 8 pixels per byte, MSB first
  uint8_t pclRow[MAX_RASTER_WIDTH / 8 > 0 ? MAX_RASTER_WIDTH / 8 : 1];

  bool put(uint8_t b) { return out ? out(ctx, b) : true; }
  bool puts(const char* s) { while (*s) if (!put((uint8_t)*s++)) return false; return true; }

  bool parseHeader() {
    auto be32 = [&](uint16_t o) {
      return ((uint32_t)hdr[o] << 24) | ((uint32_t)hdr[o+1] << 16) |
             ((uint32_t)hdr[o+2] << 8) | hdr[o+3];
    };
    hdrW = be32(PWG_OFF_WIDTH); hdrH = be32(PWG_OFF_HEIGHT);
    hdrBpp = (be32(PWG_OFF_BPP) + 7) / 8;          // bytes per pixel
    hdrBytesLine = be32(PWG_OFF_BYTESLINE);
    hdrCs = be32(PWG_OFF_COLORSPACE);
    hdrResX = be32(PWG_OFF_HWRES_X); hdrResY = be32(PWG_OFF_HWRES_Y);
    if (!hdrW || !hdrH || !hdrBytesLine || !hdrBpp) return false;
    if (hdrBytesLine > sizeof(pclRow) * hdrBpp) {  // line too wide for buffer
      // we threshold at 2400 px (300dpi x 8in); wider images get rejected
      return false;
    }
    if (hdrResX == 0 || hdrResY == 0) { hdrResX = hdrResY = 300; }
    return true;
  }

  void emitPagePrologue() {
    headerDone = true; inRow = 0; outRow = 0;
    puts("\x1b" "E");                              // reset
    char t[48];
    snprintf(t, sizeof(t), "\x1b&u%uD\x1b*t%uR", (unsigned)hdrResX, (unsigned)hdrResX);
    puts(t);                                        // units + raster resolution
    // A4 (210x297mm) default; keep page size to the header's points
    uint32_t pw = be32hdr(PWG_OFF_PAGESIZE), ph = be32hdr(PWG_OFF_PAGESIZE + 4);
    snprintf(t, sizeof(t), "\x1b&l26A");            // A4; fine for LJ-class
    puts(t);
    snprintf(t, sizeof(t), "\x1b*r%uS\x1b*r%uT", (unsigned)hdrW, (unsigned)hdrH);
    puts(t);                                        // raster width/height
    puts("\x1b*r1A");                               // start raster at top of page
    puts("\x1b*b0M");                               // no compression on output
  }

  void emitPageEpilogue() {
    puts("\x1b*rB");                                // end raster
    puts("\x0c");                                   // form feed
    headerDone = false;
    pages++;
  }

  uint32_t be32hdr(uint16_t o) {
    return ((uint32_t)hdr[o] << 24) | ((uint32_t)hdr[o+1] << 16) |
           ((uint32_t)hdr[o+2] << 8) | hdr[o+3];
  }

  void finishLine() {
    // convert the decoded gray line (hdrBytesLine bytes of 8-bit gray) into a
    // 1-bit PCL row and emit it rowRepeat times
    uint32_t outBytes = (hdrW + 7) / 8;
    for (uint32_t i = 0; i < outBytes; i++) pclRow[i] = 0xFF;   // white default
    for (uint32_t x = 0; x < hdrW; x++) {
      uint8_t g = (x < hdrBytesLine) ? lineBuf[x] : 0xFF;
      if (g < 128) pclRow[x >> 3] &= ~(0x80 >> (x & 7));        // dark -> bit
    }
    char t[24];
    snprintf(t, sizeof(t), "\x1b*b%uW", (unsigned)outBytes);
    puts(t);
    for (uint32_t i = 0; i < outBytes; i++) put(pclRow[i]);
    rowsOut++;
    outRow++;
    inRow++;
    // repeats of this row
    for (uint32_t r = 1; r < rowRepeat; r++) {
      puts(t);
      for (uint32_t i = 0; i < outBytes; i++) put(pclRow[i]);
      rowsOut++; outRow++;
    }
    if (inRow >= hdrH) {
      emitPageEpilogue();
      phase = Phase::HDR; fill = 0;   // next page header follows
      headerDone = false;
    } else {
      phase = Phase::ROWCTL;
    }
  }

  // gray line buffer: big enough for 2400 px at 1 byte/pixel
  uint8_t lineBuf[MAX_RASTER_WIDTH];
};
