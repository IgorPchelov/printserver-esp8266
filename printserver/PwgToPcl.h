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
    outFailed = false;
  }

  // Feed one input byte. Returns false on unrecoverable error (see lastError).
  // Row-unit note (verified against CUPS cups/raster-stream.c): the PackBits
  // "pixel" unit is (cupsBitsPerPixel+7)/8 BYTES, so 1-bit rows repeat single
  // bytes just like 8-bit rows - the run decoding below is bit-depth agnostic.
  // Only the final unpacking of lineBuf into bits differs per depth.
  bool feed(uint8_t b) {
    if (outFailed) {                   // the output sink failed earlier (dead
      if (!lastErr) lastErr = "output stalled";   // LPT): abort instead of
      return false;                    // hammering it for the rest of the job
    }
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
          if (!parseHeader()) { lastErr = lastErr ? lastErr : "bad header"; return false; }
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
        if (b == 128) {                    // fill rest of line to the space's
          uint8_t fillByte = fillByteForSpace();  // "no ink" level (see fn)
          while (colByte < hdrBytesLine) lineBuf[colByte++] = fillByte;
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
          // Expand the RAW pixel bytes (hdrBpp per repetition) - finishLine()
          // owns all depth conversion. Writing a pre-converted gray byte here
          // used to store only 1 byte per pixel for srgb_8 while finishLine()
          // reads 3 bytes per pixel: a repeat-coded colour row came out short
          // (the line never completed) and garbled. black_1 is unaffected:
          // its "pixel" is one packed byte (1=white, MSB first) copied
          // verbatim - finishLine() handles the bit semantics.
          while (runLeft-- > 0 && colByte + hdrBpp <= hdrBytesLine) {
            for (uint32_t k = 0; k < hdrBpp; k++) lineBuf[colByte++] = px[k];
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
  uint32_t hdrW = 0, hdrH = 0, hdrBpp = 1, hdrBpc = 8, hdrBytesLine = 0, hdrCs = 0;
  uint32_t hdrResX = 300, hdrResY = 300;
  uint32_t rowRepeat = 0, runLeft = 0, runMode = 0, pxIdx = 0;
  uint8_t px[3];
  uint32_t colByte = 0;      // decoded bytes in current line
  uint32_t inRow = 0;        // PAGE rows emitted for current page (incl. repeats)
  uint32_t outRow = 0;       // rows emitted (incl. repeats)
  uint32_t pages = 0, rowsOut = 0;
  const char* lastErr = nullptr;
  bool outFailed = false;    // the output callback returned false once
  // 1-bit PCL output row: 8 pixels per byte, MSB first.
  // (MAX_RASTER_WIDTH + 7) / 8 = 319 B for 2550 px. The old
  // MAX_RASTER_WIDTH / 8 truncated to 318 B, so the last byte of a
  // full-width Letter (300 dpi) row wrote one byte past the end,
  // silently corrupting the adjacent lineBuf.
  uint8_t pclRow[(MAX_RASTER_WIDTH + 7) / 8 > 0 ? (MAX_RASTER_WIDTH + 7) / 8 : 1];

  // OutFn's "return false to abort" contract was never enforced: put()
  // discarded the result, so a failing sink (dead LPT) was retried for every
  // remaining byte of the job. Now the first failure latches outFailed and
  // feed() aborts (lastError "output stalled").
  bool put(uint8_t b) {
    if (!out) return true;                   // no sink configured: accept
    if (outFailed) return false;
    if (!out(ctx, b)) { outFailed = true; return false; }
    return true;
  }
  bool puts(const char* s) { while (*s) if (!put((uint8_t)*s++)) return false; return true; }

  bool parseHeader() {
    auto be32 = [&](uint16_t o) {
      return ((uint32_t)hdr[o] << 24) | ((uint32_t)hdr[o+1] << 16) |
             ((uint32_t)hdr[o+2] << 8) | hdr[o+3];
    };
    hdrW = be32(PWG_OFF_WIDTH); hdrH = be32(PWG_OFF_HEIGHT);
    hdrBpp = (be32(PWG_OFF_BPP) + 7) / 8;          // bytes per pixel
    hdrBpc = be32(PWG_OFF_BPP);                    // raw bits per pixel
    hdrBytesLine = be32(PWG_OFF_BYTESLINE);
    hdrCs = be32(PWG_OFF_COLORSPACE);
    hdrResX = be32(PWG_OFF_HWRES_X); hdrResY = be32(PWG_OFF_HWRES_Y);
    if (!hdrW || !hdrH || !hdrBytesLine || !hdrBpp) return false;
    // Pixel-width cap: the 1-bit OUTPUT row (pclRow) holds
    // (MAX_RASTER_WIDTH+7)/8 bytes, so hdrW beyond MAX_RASTER_WIDTH would
    // overflow it even when the packed INPUT line (hdrBytesLine, small for
    // 1-bit pages: a 600 dpi A4 black_1 needs only 620 B) passes the
    // lineBuf check below. We advertise 300 dpi in IPP; wider pages get an
    // honest "line too wide" refusal instead of memory corruption.
    if (hdrW > MAX_RASTER_WIDTH) {
      lastErr = "line too wide";
      return false;
    }
    // The INPUT line buffer is the size that matters here (it was wrongly
    // checked against pclRow, the 1-bit OUTPUT row, so any page wider than
    // ~300 px was rejected - a full A4 300dpi page never stood a chance).
    if (hdrBytesLine > sizeof(lineBuf)) {          // input line too wide
      lastErr = "line too wide";
      return false;
    }
    // Accept the color spaces Android/CUPS actually send a mono printserver:
    //   3  (K)   black, cupsBitsPerPixel=1 (black_1) or 8 (black_8)
    //   0  (W), 18 (SW)  gray / sGray, 8 bpp (sgray_8)
    //   1 (RGB), 19 (SRGB) 8bpc color (srgb_8) - converted to luma below
    // Anything else (CMYK etc.) cannot be honestly printed: reject instead
    // of emitting garbage.
    if (hdrCs != 0 && hdrCs != 1 && hdrCs != 3 && hdrCs != 18 && hdrCs != 19) {
      lastErr = "unsupported colorspace";
      return false;
    }
    // Bit-depth guard: finishLine() implements 1/8/24 bpp only. 16-bit gray
    // (bpc=16 -> 2 B/px) used to fall through to the "3 bytes/pixel" branch
    // and print garbage rows; anything else is refused honestly.
    if (hdrBpc != 1 && hdrBpc != 8 && hdrBpc != 24) {
      lastErr = "unsupported bit depth";
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
    // PCL page size from the header's PageSize (points). The old code always
    // sent 26A (A4): a Letter-width raster (612x792 pt - one of the two media
    // we advertise in IPP) landed on a narrower declared page and was clipped
    // by the printer. 612 pt -> Letter (2A), 595 pt -> A4 (26A), else A4.
    uint32_t pw = be32hdr(PWG_OFF_PAGESIZE);
    const char* pageCode = "26";                    // A4
    if (pw >= 600 && pw <= 625) pageCode = "2";     // US Letter 612 pt
    snprintf(t, sizeof(t), "\x1b&l%sA", pageCode);
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
    // convert the decoded line into a 1-bit PCL row and emit it rowRepeat times.
    // Input depth handling (verified against CUPS raster semantics):
    //  - hdrBpc == 1 (black_1): packed bits, MSB first, 1 = white, 0 = black.
    //    This matches the PDF embedding OpenPrinting's pwgtopdf uses for
    //    CUPS_CSPACE_K (DeviceGray, no /Decode array -> 0=black, 1=white).
    //  - hdrBpc == 8 (sgray_8): one gray byte per pixel, threshold at 128.
    //  - hdrBpc == 24 (srgb_8): 3 bytes/pixel, luma (BT.601), threshold 128.
    uint32_t outBytes = (hdrW + 7) / 8;
    for (uint32_t i = 0; i < outBytes; i++) pclRow[i] = 0xFF;   // white default
    if (hdrBpc == 1) {
      // input line is already the packed 1-bit row: (hdrW+7)/8 bytes in
      // lineBuf, same MSB-first bit order PCL wants - but with 1=white
      // polarity, and PCL raster rows also use 1=white, so a direct copy
      // works. Only the length may differ (trailing pad bits).
      uint32_t inBytes = hdrBytesLine < outBytes ? hdrBytesLine : outBytes;
      for (uint32_t i = 0; i < inBytes; i++) pclRow[i] = lineBuf[i];
    } else {
      for (uint32_t x = 0; x < hdrW; x++) {
        uint8_t g;
        if (hdrBpp == 1) g = (x < hdrBytesLine) ? lineBuf[x] : 0xFF;
        else {                                   // 3 bytes/pixel srgb_8
          uint32_t o = x * 3;
          g = (o + 2 < hdrBytesLine)
                ? (uint8_t)((lineBuf[o] * 30 + lineBuf[o+1] * 59 + lineBuf[o+2] * 11) / 100)
                : 0xFF;
        }
        if (g < 128) pclRow[x >> 3] &= ~(0x80 >> (x & 7));      // dark -> bit
      }
    }
    char t[24];
    snprintf(t, sizeof(t), "\x1b*b%uW", (unsigned)outBytes);
    puts(t);
    for (uint32_t i = 0; i < outBytes; i++) put(pclRow[i]);
    rowsOut++;
    outRow++;
    // A repeat-coded row stands for rowRepeat PAGE rows (the repeats below
    // each emit the same transfer again). Counting coding units here (the
    // old inRow++) left inRow below cupsHeight on pages with repeat-coded
    // rows, so the page never completed and the NEXT page's header was
    // decoded as garbage rows of this page (multi-page prints died with
    // "bad header").
    inRow += rowRepeat;
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

  // Byte the ctl=128 "fill rest of line" code expands to. CUPS
  // (cups/raster-stream.c, reader switch on cupsColorSpace) fills W/RGB/SW/
  // SRGB/RGBW/AdobeRGB with 0xFF (white) and EVERYTHING ELSE - including
  // K/black, i.e. black_1 jobs - with 0x00. Filling K with white here would
  // print solid black pages whenever Android ends a short line with 128.
  uint8_t fillByteForSpace() const {
    switch (hdrCs) {
      case 0: case 1: case 18: case 19: return 0xFF;   // W RGB SW SRGB
      default: return 0x00;                            // K(3) and everything else
    }
  }

  // Input line buffer, worst case = full Letter width at 3 bytes/pixel
  // (srgb_8, which IPP advertises in pwg-raster-document-type-supported):
  // 2550 * 3 = 7650 B. 1-bit and 8-bit pages use a fraction of it. The old
  // 1 byte/px sizing silently rejected every full-width srgb_8 page with
  // "line too wide" AFTER the client had already started transferring.
  uint8_t lineBuf[MAX_RASTER_WIDTH * 3];
};
