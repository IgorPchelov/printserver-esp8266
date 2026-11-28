// Host unit test for PwgToPcl.h - builds a synthetic PWG-raster stream
// (sync RaS2 + 1796-byte BE header + PackBits rows, mirroring Android's
// lib_pwg.c producer), feeds it to the converter, and validates the PCL.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include "../PwgToPcl.h"

// ---- capture output ----
static std::vector<uint8_t> outBytes;
static int lptTimeouts = 0;
// Sink verdict emulation: lptTimeouts>0 -> FAIL (dead port); lptHolds>0 ->
// HOLD for the first lptHolds bytes (busy port), then OK.
static int lptHolds = 0;
static int holderN = 0;
PwgToPcl::Sink heldSink(void*, uint8_t b) {
  if (++holderN % 3 == 0) return PwgToPcl::Sink::HOLD;   // busy this byte
  outBytes.push_back(b);
  return PwgToPcl::Sink::OK;
}

PwgToPcl::Sink fakeLpt(void*, uint8_t b) {
  if (lptTimeouts > 0) return PwgToPcl::Sink::FAIL;
  if (lptHolds > 0) { lptHolds--; return PwgToPcl::Sink::HOLD; }
  outBytes.push_back(b);
  return PwgToPcl::Sink::OK;
}

// Feed a whole document with the engine's discipline: never feed while
// output is pending; pump between feeds; drain fully at the end. Returns
// false on FAIL (the lastError explains), and counts HELD pauses.
static int heldPauses = 0;
bool feedDoc(PwgToPcl& c, const std::vector<uint8_t>& doc) {
  for (uint8_t b : doc) {
    if (c.needsPump()) {
      PwgToPcl::Feed pf = c.pump(1000);
      if (pf == PwgToPcl::Feed::FAIL) return false;
      if (pf == PwgToPcl::Feed::HELD) { heldPauses++; }
    }
    PwgToPcl::Feed f = c.feed(b);
    if (f == PwgToPcl::Feed::FAIL) return false;
    // HELD: the byte was consumed; keep feeding (test sinks never hold
    // longer than one byte, so input continues right after the pause)
    if (f == PwgToPcl::Feed::HELD) { heldPauses++; }
  }
  return true;
}

// Drain to completion after end(): pump until OK, HELD never settles
// (test sinks never hold forever), FAIL returns false.
bool drainDoc(PwgToPcl& c) {
  for (;;) {
    PwgToPcl::Feed pf = c.pump(1000);
    if (pf == PwgToPcl::Feed::OK) return !c.needsPump();
    if (pf == PwgToPcl::Feed::FAIL) return false;
    if (pf == PwgToPcl::Feed::HELD) heldPauses++;
  }
}

static void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

static int failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  else { printf("ok:   %s\n", msg); } } while (0)

int main() {
  // ---------- build a 64x32 page, 8-bit gray, 300 dpi ----------
  const uint32_t W = 64, H = 32;
  std::vector<uint8_t> doc;

  // sync "RaS2"
  doc.push_back('R'); doc.push_back('a'); doc.push_back('S'); doc.push_back('2');

  // header: 1796 bytes, zero-filled, key fields BE
  std::vector<uint8_t> hdr(PWG_HDR_SIZE, 0);
  auto putBE = [&](uint16_t off, uint32_t val) {
    hdr[off] = val >> 24; hdr[off+1] = val >> 16; hdr[off+2] = val >> 8; hdr[off+3] = val;
  };
  putBE(PWG_OFF_HWRES_X, 300);
  putBE(PWG_OFF_HWRES_Y, 300);
  putBE(PWG_OFF_WIDTH, W);
  putBE(PWG_OFF_HEIGHT, H);
  putBE(PWG_OFF_BPP, 8);              // 8 bpp gray (cupsColorSpace SW=18)
  putBE(PWG_OFF_BYTESLINE, W);
  putBE(PWG_OFF_COLORSPACE, 18);      // CUPS_CSPACE_SW
  memcpy(&hdr[0], "PwgRaster", 9);    // MediaClass
  doc.insert(doc.end(), hdr.begin(), hdr.end());

  // rows: row 0..15 black (0x00) with repeat, rows 16..31 mixed literals
  // PackBits per CUPS: rowRepeat-1 byte, then runs.
  // Row content 0: repeat-run of 0x00 pixel: ctl = (W/1)-1 = 63 -> repeat 64 px of byte 0x00
  doc.push_back(15);                  // row repeat count-1: row repeated 16 times
  doc.push_back(63); doc.push_back(0x00);   // repeat 64 pixels of 0x00

  // rows 16..31: one literal line, then a repeat of 15? Keep simple:
  // each of the remaining 16 rows: repeat count 0 (single), half black half white
  for (uint32_t r = 0; r < 16; r++) {
    doc.push_back(0);                 // this row appears once
    // literal run: 257-ctl = 32 bytes => ctl = 225
    doc.push_back(225);
    for (uint32_t i = 0; i < 32; i++) doc.push_back(0x00);   // left half dark
    // then white fill
    doc.push_back(128);               // fill rest with 0xFF
  }

  // ---------- run the converter ----------
  PwgToPcl conv;
  conv.begin(fakeLpt, nullptr);
  bool ok = feedDoc(conv, doc);
  CHECK(ok, "converter consumed the whole stream without error");
  conv.end();
  CHECK(drainDoc(conv), "output fully drained after end()");

  printf("info: pages=%u rows=%u outBytes=%zu\n",
         conv.pagesPrinted(), conv.rowsPrinted(), outBytes.size());

  CHECK(conv.pagesPrinted() == 1, "exactly one page emitted");
  CHECK(conv.rowsPrinted() == 32, "32 raster rows emitted (with repeats)");
  CHECK(!strcmp(conv.lastError(), ""), "no converter error");

  // ---------- validate PCL output ----------
  std::string s(outBytes.begin(), outBytes.end());
  CHECK(s.find("\x1b""E") == 0, "output starts with PCL reset (ESC E)");
  CHECK(s.find("\x1b*t300R") != std::string::npos, "resolution 300 set");
  CHECK(s.find("\x1b*r64S") != std::string::npos, "raster width 64 set");
  CHECK(s.find("\x1b*r32T") != std::string::npos, "raster height 32 set");
  CHECK(s.find("\x1b*r1A") != std::string::npos, "raster start");
  CHECK(s.find("\x1b*rB") != std::string::npos, "raster end");
  CHECK(s.find("\x0c") != std::string::npos, "form feed present");

  // transfer: 64px = 8 bytes/row, header "\x1b*b8W"
  CHECK(s.find("\x1b*b8W") != std::string::npos, "8-byte transfer rows");

  // first data row after the first "\x1b*b8W": all 8 bytes must be 0x00 (black)
  size_t p = s.find("\x1b*b8W");
  CHECK(p != std::string::npos, "row marker found");
  if (p != std::string::npos) {
    p += 5;
    bool allBlack = true;
    for (int i = 0; i < 8; i++) if (outBytes[p + i] != 0x00) allBlack = false;
    CHECK(allBlack, "first (repeated) row is solid black");
    // row 17th (after 16 identical black rows): left half black, right white
    // locate the 17th marker
    size_t q = p;
    for (int r = 0; r < 16; r++) q = s.find("\x1b*b8W", q + 8);
    if (q != std::string::npos) {
      q += 5;
      bool leftBlack = true, rightWhite = true;
      for (int i = 0; i < 4; i++) if (outBytes[q + i] != 0x00) leftBlack = false;
      for (int i = 4; i < 8; i++) if (outBytes[q + i] != 0xFF) rightWhite = false;
      CHECK(leftBlack && rightWhite, "literal+fill row: left half black, right half white");
    }
  }

  // ---------- error path: bad sync ----------
  PwgToPcl bad;
  bad.begin(fakeLpt, nullptr);
  bool ok2 = true;
  bad.feed('X'); bad.feed('a'); bad.feed('S'); bad.feed('3');   // "RaS3", not "RaS2"
  CHECK(!ok2 || true, "fed wrong sync");       // sync mismatch detected on 4th byte
  ok2 = true;
  for (char ch : {'X','a','S','2'}) { }        // (silence unused warnings)
  PwgToPcl bad2;
  bad2.begin(fakeLpt, nullptr);
  for (char ch : std::string("RaS3")) { if (bad2.feed((uint8_t)ch) == PwgToPcl::Feed::FAIL) ok2 = false; }
  CHECK(!ok2 && !strcmp(bad2.lastError(), "bad sync"), "bad sync detected");

  // ---------- wide line: full A4 width 300dpi (regression) ----------

  // The old code checked hdrBytesLine (the INPUT line, 310 bytes for a
  // 2480-px page) against pclRow (the 310-byte... no, 2480/8=310-byte OUTPUT
  // row is FINE for 1bpp, but for 8bpp gray the input line is 2480 bytes and
  // was compared against the 310-byte output buffer -> any real Android
  // page wider than ~300 px was rejected with "bad header". This test
  // builds exactly such a page and expects it to print.
  {
    outBytes.clear();
    const uint32_t W = 2480, H = 4;            // A4 width at 300 dpi, tiny height
    std::vector<uint8_t> d2;
    d2.push_back('R'); d2.push_back('a'); d2.push_back('S'); d2.push_back('2');
    std::vector<uint8_t> h2(PWG_HDR_SIZE, 0);
    auto pb = [&](uint16_t off, uint32_t val) {
      h2[off] = val >> 24; h2[off+1] = val >> 16; h2[off+2] = val >> 8; h2[off+3] = val;
    };
    pb(PWG_OFF_HWRES_X, 300); pb(PWG_OFF_HWRES_Y, 300);
    pb(PWG_OFF_WIDTH, W); pb(PWG_OFF_HEIGHT, H);
    pb(PWG_OFF_BPP, 8); pb(PWG_OFF_BYTESLINE, W);
    pb(PWG_OFF_COLORSPACE, 18);                // sgray_8
    memcpy(&h2[0], "PwgRaster", 9);
    d2.insert(d2.end(), h2.begin(), h2.end());
    for (uint32_t r = 0; r < H; r++) {
      d2.push_back(0);                         // this row once
      d2.push_back(127);                       // ctl 0..127 = REPEAT run: 128 px
      d2.push_back(0x00);                      // ...of black 0x00
      d2.push_back(128);                       // fill rest of line (white for SW)
    }
    PwgToPcl c2;
    c2.begin(fakeLpt, nullptr);
    bool okw = true;
    okw = feedDoc(c2, d2);
    CHECK(okw, "wide 2480-px 8-bpp page accepted (old code rejected >300 px)");
    c2.end();
    CHECK(drainDoc(c2), "c2: output drained after end()");
    CHECK(c2.rowsPrinted() == H, "wide page: all rows emitted");
    CHECK(!strcmp(c2.lastError(), ""), "wide page: no error");
  }

  // ---------- black_1: 1-bit page (Android's other raster type) ----------
  {
    outBytes.clear();
    const uint32_t W = 64, H = 2;              // 64 px -> 8 packed bytes/row
    std::vector<uint8_t> d3;
    d3.push_back('R'); d3.push_back('a'); d3.push_back('S'); d3.push_back('2');
    std::vector<uint8_t> h3(PWG_HDR_SIZE, 0);
    auto pb3 = [&](uint16_t off, uint32_t val) {
      h3[off] = val >> 24; h3[off+1] = val >> 16; h3[off+2] = val >> 8; h3[off+3] = val;
    };
    pb3(PWG_OFF_HWRES_X, 300); pb3(PWG_OFF_HWRES_Y, 300);
    pb3(PWG_OFF_WIDTH, W); pb3(PWG_OFF_HEIGHT, H);
    pb3(PWG_OFF_BPP, 1);                       // black_1!
    pb3(PWG_OFF_BYTESLINE, 8);                 // 64 bits = 8 bytes
    pb3(PWG_OFF_COLORSPACE, 3);                // CUPS_CSPACE_K
    memcpy(&h3[0], "PwgRaster", 9);
    d3.insert(d3.end(), h3.begin(), h3.end());

    // Row 1: stripes 0xAA (10101010) - 1=white, 0=black, MSB first
    d3.push_back(0);                           // appears once
    d3.push_back(0); d3.push_back(0xAA);       // ctl=0: repeat 1 "pixel" (=byte here), byte 0xAA
    d3.push_back(128);                         // fill REST OF LINE... in K space
    // the fill must NOT be needed (8 bytes already done? no: ctl=0 repeated
    // ONE byte), so the 128-fill covers bytes 1..7 of the packed row and -
    // per CUPS raster-stream.c - must expand to 0x00 (BLACK) in K space.

    PwgToPcl c3;
    c3.begin(fakeLpt, nullptr);
    bool ok3 = true;
    ok3 = feedDoc(c3, d3);
    CHECK(ok3, "black_1 page accepted");
    c3.end();
    CHECK(drainDoc(c3), "c3: output drained after end()");
    CHECK(c3.rowsPrinted() == 1, "black_1: one row emitted");
    std::string s3(outBytes.begin(), outBytes.end());
    size_t p3 = s3.find("\x1b*b8W");
    CHECK(p3 != std::string::npos, "black_1: 8-byte transfer row present");
    if (p3 != std::string::npos) {
      p3 += 5;
      // first packed byte must be 0xAA verbatim (bit=1 is white in BOTH the
      // PWG source and the PCL output - no inversion, verified against
      // OpenPrinting rastertohp.c which fwrites CUPS rows unmodified)
      CHECK(outBytes[p3] == 0xAA, "black_1: packed byte passed through verbatim");
      // bytes 1..7 come from the ctl=128 fill: BLACK (0x00) in K space -
      // the old code filled white and would print near-blank pages
      bool fillBlack = true;
      for (int i = 1; i < 8; i++) if (outBytes[p3 + i] != 0x00) fillBlack = false;
      CHECK(fillBlack, "black_1: ctl=128 fill is BLACK in K space (CUPS semantics)");
    }
  }

  // ---------- srgb_8: 24-bit color page -> luma ----------
  {
    outBytes.clear();
    const uint32_t W = 16, H = 1;
    std::vector<uint8_t> d4;
    d4.push_back('R'); d4.push_back('a'); d4.push_back('S'); d4.push_back('2');
    std::vector<uint8_t> h4(PWG_HDR_SIZE, 0);
    auto pb4 = [&](uint16_t off, uint32_t val) {
      h4[off] = val >> 24; h4[off+1] = val >> 16; h4[off+2] = val >> 8; h4[off+3] = val;
    };
    pb4(PWG_OFF_HWRES_X, 300); pb4(PWG_OFF_HWRES_Y, 300);
    pb4(PWG_OFF_WIDTH, W); pb4(PWG_OFF_HEIGHT, H);
    pb4(PWG_OFF_BPP, 24);                      // srgb_8
    pb4(PWG_OFF_BYTESLINE, W * 3);
    pb4(PWG_OFF_COLORSPACE, 19);               // CUPS_CSPACE_SRGB
    memcpy(&h4[0], "PwgRaster", 9);
    d4.insert(d4.end(), h4.begin(), h4.end());

    d4.push_back(0);                           // row appears once
    // 16 pixels as literal run: ctl = 257-16 = 241, then 16*3 bytes
    d4.push_back(241);
    // 8 black pixels (0,0,0), 8 white pixels (255,255,255)
    for (int i = 0; i < 8; i++) { d4.push_back(0); d4.push_back(0); d4.push_back(0); }
    for (int i = 0; i < 8; i++) { d4.push_back(255); d4.push_back(255); d4.push_back(255); }

    PwgToPcl c4;
    c4.begin(fakeLpt, nullptr);
    bool ok4 = true;
    ok4 = feedDoc(c4, d4);
    CHECK(ok4, "srgb_8 page accepted");
    c4.end();
    CHECK(drainDoc(c4), "c4: output drained after end()");
    CHECK(c4.rowsPrinted() == 1, "srgb_8: one row emitted");
    std::string s4(outBytes.begin(), outBytes.end());
    size_t p4 = s4.find("\x1b*b2W");           // 16 px -> 2 bytes/row
    CHECK(p4 != std::string::npos, "srgb_8: 2-byte transfer row present");
    if (p4 != std::string::npos) {
      p4 += 5;
      CHECK(outBytes[p4] == 0x00, "srgb_8: black pixels -> black bits");
      CHECK(outBytes[p4 + 1] == 0xFF, "srgb_8: white pixels -> white bits");
    }
  }

  // ---------- unsupported color space rejected honestly ----------
  {
    outBytes.clear();
    PwgToPcl c5;
    c5.begin(fakeLpt, nullptr);
    std::vector<uint8_t> d5;
    d5.push_back('R'); d5.push_back('a'); d5.push_back('S'); d5.push_back('2');
    std::vector<uint8_t> h5(PWG_HDR_SIZE, 0);
    auto pb5 = [&](uint16_t off, uint32_t val) {
      h5[off] = val >> 24; h5[off+1] = val >> 16; h5[off+2] = val >> 8; h5[off+3] = val;
    };
    pb5(PWG_OFF_WIDTH, 32); pb5(PWG_OFF_HEIGHT, 2);
    pb5(PWG_OFF_BPP, 32); pb5(PWG_OFF_BYTESLINE, 128);
    pb5(PWG_OFF_COLORSPACE, 6);                // CMYK - not printable on LPT mono
    memcpy(&h5[0], "PwgRaster", 9);
    d5.insert(d5.end(), h5.begin(), h5.end());
    bool ok5 = true;
    ok5 = feedDoc(c5, d5);
    CHECK(!ok5 && !strcmp(c5.lastError(), "unsupported colorspace"),
          "CMYK raster rejected with a clear reason, not garbage");
  }

  // ---------- regression: full-width Letter black_1 (pclRow sizing) ----------
  // 2550 px @300dpi -> outBytes=(2550+7)/8=319. pclRow used to be
  // MAX_RASTER_WIDTH/8 = 318 B: the last byte of each row wrote out of
  // bounds into lineBuf and corrupted the row (visible as white specks).
  {
    outBytes.clear();
    const uint32_t W = 2550, H = 2;            // US Letter width, 2 rows
    std::vector<uint8_t> d6;
    d6.push_back('R'); d6.push_back('a'); d6.push_back('S'); d6.push_back('2');
    std::vector<uint8_t> h6(PWG_HDR_SIZE, 0);
    auto pb6 = [&](uint16_t off, uint32_t val) {
      h6[off] = val >> 24; h6[off+1] = val >> 16; h6[off+2] = val >> 8; h6[off+3] = val;
    };
    pb6(PWG_OFF_HWRES_X, 300); pb6(PWG_OFF_HWRES_Y, 300);
    pb6(PWG_OFF_WIDTH, W); pb6(PWG_OFF_HEIGHT, H);
    pb6(PWG_OFF_BPP, 1);                       // black_1
    pb6(PWG_OFF_BYTESLINE, (W + 7) / 8);       // 319 packed bytes/row
    pb6(PWG_OFF_COLORSPACE, 3);                // CUPS_CSPACE_K
    memcpy(&h6[0], "PwgRaster", 9);
    d6.insert(d6.end(), h6.begin(), h6.end());
    for (uint32_t r = 0; r < H; r++) {
      d6.push_back(0);                         // row appears once
      d6.push_back(128);                       // fill whole line (K -> black)
    }
    PwgToPcl c6;
    c6.begin(fakeLpt, nullptr);
    bool ok6 = true;
    ok6 = feedDoc(c6, d6);
    CHECK(ok6, "Letter-width (2550 px) black_1 page accepted");
    c6.end();
    CHECK(drainDoc(c6), "c6: output drained after end()");
    CHECK(c6.rowsPrinted() == H, "Letter black_1: both rows emitted");
    std::string s6(outBytes.begin(), outBytes.end());
    const std::string marker6 = "\x1b*b319W";
    int rows6 = 0, bad6 = 0;
    size_t p6 = 0;
    while ((p6 = s6.find(marker6, p6)) != std::string::npos) {
      p6 += marker6.size();
      for (int i = 0; i < 319; i++) if (outBytes[p6 + i] != 0x00) bad6++;
      rows6++;
    }
    CHECK(rows6 == 2, "Letter black_1: 319-byte transfer rows (full width)");
    CHECK(bad6 == 0, "Letter black_1: all-black rows intact (no pclRow overflow)");
  }

  // ---------- regression: srgb_8 repeat-run (REP run unit) ----------
  // A colour row coded as ONE repeat run (the common case for solid areas:
  // Android's encoder loves these). REP used to store 1 gray byte per
  // pixel while finishLine() reads 3 bytes/pixel: the line never completed
  // and the row was silently lost (rows==0).
  {
    outBytes.clear();
    const uint32_t W = 8, H = 1;
    std::vector<uint8_t> d7;
    d7.push_back('R'); d7.push_back('a'); d7.push_back('S'); d7.push_back('2');
    std::vector<uint8_t> h7(PWG_HDR_SIZE, 0);
    auto pb7 = [&](uint16_t off, uint32_t val) {
      h7[off] = val >> 24; h7[off+1] = val >> 16; h7[off+2] = val >> 8; h7[off+3] = val;
    };
    pb7(PWG_OFF_HWRES_X, 300); pb7(PWG_OFF_HWRES_Y, 300);
    pb7(PWG_OFF_WIDTH, W); pb7(PWG_OFF_HEIGHT, H);
    pb7(PWG_OFF_BPP, 24);                      // srgb_8
    pb7(PWG_OFF_BYTESLINE, W * 3);             // 24 bytes/row
    pb7(PWG_OFF_COLORSPACE, 19);               // CUPS_CSPACE_SRGB
    memcpy(&h7[0], "PwgRaster", 9);
    d7.insert(d7.end(), h7.begin(), h7.end());
    d7.push_back(0);                           // row appears once
    d7.push_back(7);                           // ctl 0..127: repeat 8 pixels...
    d7.push_back(0x00); d7.push_back(0x00); d7.push_back(0x00);  // ...of black
    PwgToPcl c7;
    c7.begin(fakeLpt, nullptr);
    bool ok7 = true;
    ok7 = feedDoc(c7, d7);
    CHECK(ok7, "srgb_8 repeat-run row accepted");
    c7.end();
    CHECK(drainDoc(c7), "c7: output drained after end()");
    CHECK(c7.rowsPrinted() == 1, "srgb_8 repeat-run: the row is not lost");
    std::string s7(outBytes.begin(), outBytes.end());
    size_t p7 = s7.find("\x1b*b1W");
    CHECK(p7 != std::string::npos, "srgb_8 repeat-run: transfer row present");
    if (p7 != std::string::npos)
      CHECK(outBytes[p7 + 5] == 0x00, "srgb_8 repeat-run: black row decoded");
  }

  // ---------- regression: 600dpi A4 black_1 refused (output-row guard) ------
  // hdrBytesLine=620 fits lineBuf (7650 B), but outBytes=620 would blow the
  // 319-B pclRow. Must be refused by the hdrW check with "line too wide",
  // not corrupt memory. (The server advertises 300 dpi only.)
  {
    outBytes.clear();
    const uint32_t W = 4960, H = 1;            // A4 at 600 dpi
    std::vector<uint8_t> d8;
    d8.push_back('R'); d8.push_back('a'); d8.push_back('S'); d8.push_back('2');
    std::vector<uint8_t> h8(PWG_HDR_SIZE, 0);
    auto pb8 = [&](uint16_t off, uint32_t val) {
      h8[off] = val >> 24; h8[off+1] = val >> 16; h8[off+2] = val >> 8; h8[off+3] = val;
    };
    pb8(PWG_OFF_HWRES_X, 600); pb8(PWG_OFF_HWRES_Y, 600);
    pb8(PWG_OFF_WIDTH, W); pb8(PWG_OFF_HEIGHT, H);
    pb8(PWG_OFF_BPP, 1);                       // black_1
    pb8(PWG_OFF_BYTESLINE, (W + 7) / 8);       // 620 B - fits lineBuf
    pb8(PWG_OFF_COLORSPACE, 3);
    memcpy(&h8[0], "PwgRaster", 9);
    d8.insert(d8.end(), h8.begin(), h8.end());
    d8.push_back(0);
    d8.push_back(128);
    PwgToPcl c8;
    c8.begin(fakeLpt, nullptr);
    bool ok8 = true;
    ok8 = feedDoc(c8, d8);
    CHECK(!ok8 && !strcmp(c8.lastError(), "line too wide"),
          "600dpi A4 black_1 refused honestly (would overflow pclRow)");
  }

  // ---------- regression: full-width srgb_8 accepted (lineBuf sizing) -------
  // A4 at 300 dpi, 24 bpp: hdrBytesLine = 2480*3 = 7440 B. The old 2550-B
  // lineBuf rejected this AFTER IPP advertised srgb_8 as supported - the
  // client had already started transferring. lineBuf is 2550*3 now.
  {
    outBytes.clear();
    const uint32_t W = 2480, H = 1;            // A4 at 300 dpi
    std::vector<uint8_t> d9;
    d9.push_back('R'); d9.push_back('a'); d9.push_back('S'); d9.push_back('2');
    std::vector<uint8_t> h9(PWG_HDR_SIZE, 0);
    auto pb9 = [&](uint16_t off, uint32_t val) {
      h9[off] = val >> 24; h9[off+1] = val >> 16; h9[off+2] = val >> 8; h9[off+3] = val;
    };
    pb9(PWG_OFF_HWRES_X, 300); pb9(PWG_OFF_HWRES_Y, 300);
    pb9(PWG_OFF_WIDTH, W); pb9(PWG_OFF_HEIGHT, H);
    pb9(PWG_OFF_BPP, 24);                      // srgb_8
    pb9(PWG_OFF_BYTESLINE, W * 3);             // 7440 B
    pb9(PWG_OFF_COLORSPACE, 19);
    memcpy(&h9[0], "PwgRaster", 9);
    d9.insert(d9.end(), h9.begin(), h9.end());
    d9.push_back(0);                           // row once
    // whole 2480-px line as black repeat runs: 19 x 128 px + 48 px
    uint32_t pxLeft = W;
    while (pxLeft >= 128) {
      d9.push_back(127);                       // repeat 128 px of one pixel
      d9.push_back(0x00); d9.push_back(0x00); d9.push_back(0x00);
      pxLeft -= 128;
    }
    if (pxLeft) {
      d9.push_back(pxLeft - 1);                // repeat the remaining px
      d9.push_back(0x00); d9.push_back(0x00); d9.push_back(0x00);
    }
    PwgToPcl c9;
    c9.begin(fakeLpt, nullptr);
    bool ok9 = true;
    ok9 = feedDoc(c9, d9);
    CHECK(ok9, "full-width srgb_8 A4 page accepted (lineBuf sized for 3 B/px)");
    c9.end();
    CHECK(drainDoc(c9), "c9: output drained after end()");
    CHECK(c9.rowsPrinted() == 1, "full-width srgb_8: row emitted");
    CHECK(!strcmp(c9.lastError(), ""), "full-width srgb_8: no error");
    // the 2480-px row packs into 310 bytes, all black
    std::string s9(outBytes.begin(), outBytes.end());
    const std::string marker9 = "\x1b*b310W";
    size_t p9 = s9.find(marker9);
    CHECK(p9 != std::string::npos, "full-width srgb_8: 310-byte transfer row");
    if (p9 != std::string::npos) {
      p9 += marker9.size();
      bool allBlack = true;
      for (int i = 0; i < 310; i++) if (outBytes[p9 + i] != 0x00) allBlack = false;
      CHECK(allBlack, "full-width srgb_8: whole black row decoded via repeats");
    }
  }

  // ---------- regression: multi-page stream (inRow must count repeats) -------
  // A repeat-coded row represents rowRepeat PAGE rows, but inRow was bumped
  // once per CODING unit: after a page whose rows are repeat-coded (Android
  // does this for every blank band) inRow stayed below cupsHeight, the page
  // never "completed", and the NEXT page's 1796-byte header was decoded as
  // garbage rows of the current page -> "bad header" / lost pages. A 2-page
  // document must emit exactly 2 form feeds and 2 pages.
  {
    outBytes.clear();
    const uint32_t W = 8, H = 2;
    auto buildPage = [&](std::vector<uint8_t>& d) {
      std::vector<uint8_t> h(PWG_HDR_SIZE, 0);
      auto p = [&](uint16_t off, uint32_t val) {
        h[off] = val >> 24; h[off+1] = val >> 16; h[off+2] = val >> 8; h[off+3] = val;
      };
      p(PWG_OFF_HWRES_X, 300); p(PWG_OFF_HWRES_Y, 300);
      p(PWG_OFF_WIDTH, W); p(PWG_OFF_HEIGHT, H);
      p(PWG_OFF_BPP, 1); p(PWG_OFF_BYTESLINE, 1);
      p(PWG_OFF_COLORSPACE, 3);                 // K
      memcpy(&h[0], "PwgRaster", 9);
      d.insert(d.end(), h.begin(), h.end());
      // ONE coding unit: repeat count 1 (row appears 2x = the whole page),
      // then ctl=0 (repeat 1 "pixel" = 1 packed byte) of 0x00 (black)
      d.push_back(1);
      d.push_back(0); d.push_back(0x00);
    };
    std::vector<uint8_t> d10;
    d10.push_back('R'); d10.push_back('a'); d10.push_back('S'); d10.push_back('2');
    buildPage(d10);                            // page 1
    buildPage(d10);                            // page 2 (header right after rows)
    PwgToPcl c10;
    c10.begin(fakeLpt, nullptr);
    bool ok10 = true;
    ok10 = feedDoc(c10, d10);
    CHECK(ok10, "multi-page: stream consumed without error");
    c10.end();
    CHECK(drainDoc(c10), "c10: output drained after end()");
    CHECK(c10.pagesPrinted() == 2, "multi-page: exactly 2 pages emitted");
    CHECK(!strcmp(c10.lastError(), ""), "multi-page: no error");
    std::string s10(outBytes.begin(), outBytes.end());
    int ff = 0;
    for (size_t i = 0; i < s10.size(); i++) if (s10[i] == '\x0c') ff++;
    CHECK(ff == 2, "multi-page: one form feed per page");
    int rasterStarts = 0;
    for (size_t p10 = 0; (p10 = s10.find("\x1b*r1A", p10)) != std::string::npos; rasterStarts++, p10++);
    CHECK(rasterStarts == 2, "multi-page: raster started once per page");
  }

  // ---------- regression: output abort propagates (dead printer) -------------
  // pwgOut() used to ignore writeByte() failures, so a dead/stalled LPT made
  // every output byte wait the full 120 s timeout and the job never finished
  // or dropped. The decoder must abort on the first failed output byte.
  {
    outBytes.clear();
    lptTimeouts = 1;                           // fakeLpt now fails every byte
    const uint32_t W = 8, H = 1;
    std::vector<uint8_t> d11;
    d11.push_back('R'); d11.push_back('a'); d11.push_back('S'); d11.push_back('2');
    std::vector<uint8_t> h(PWG_HDR_SIZE, 0);
    auto p11 = [&](uint16_t off, uint32_t val) {
      h[off] = val >> 24; h[off+1] = val >> 16; h[off+2] = val >> 8; h[off+3] = val;
    };
    p11(PWG_OFF_HWRES_X, 300); p11(PWG_OFF_HWRES_Y, 300);
    p11(PWG_OFF_WIDTH, W); p11(PWG_OFF_HEIGHT, H);
    p11(PWG_OFF_BPP, 1); p11(PWG_OFF_BYTESLINE, 1);
    p11(PWG_OFF_COLORSPACE, 3);
    memcpy(&h[0], "PwgRaster", 9);
    d11.insert(d11.end(), h.begin(), h.end());
    d11.push_back(0); d11.push_back(0); d11.push_back(0x00);
    PwgToPcl c11;
    c11.begin(fakeLpt, nullptr);
    bool ok11 = true;
    ok11 = feedDoc(c11, d11);
    CHECK(!ok11, "output failure aborts the conversion");
    CHECK(!strcmp(c11.lastError(), "output stalled"), "abort reason is 'output stalled'");
    lptTimeouts = 0;                           // restore for later tests
  }

  // ---------- regression: unsupported bit depth refused honestly -------------
  // 16-bit gray (bpc=16, bpp=2) used to slip through the colorspace check and
  // land in the "3 bytes/pixel" branch of finishLine() - i.e. garbage rows.
  {
    outBytes.clear();
    PwgToPcl c12;
    c12.begin(fakeLpt, nullptr);
    std::vector<uint8_t> d12;
    d12.push_back('R'); d12.push_back('a'); d12.push_back('S'); d12.push_back('2');
    std::vector<uint8_t> h(PWG_HDR_SIZE, 0);
    auto p12 = [&](uint16_t off, uint32_t val) {
      h[off] = val >> 24; h[off+1] = val >> 16; h[off+2] = val >> 8; h[off+3] = val;
    };
    p12(PWG_OFF_HWRES_X, 300); p12(PWG_OFF_HWRES_Y, 300);
    p12(PWG_OFF_WIDTH, 64); p12(PWG_OFF_HEIGHT, 2);
    p12(PWG_OFF_BPP, 16); p12(PWG_OFF_BYTESLINE, 128);
    p12(PWG_OFF_COLORSPACE, 18);               // SW gray, but 16-bit
    memcpy(&h[0], "PwgRaster", 9);
    d12.insert(d12.end(), h.begin(), h.end());
    bool ok12 = true;
    ok12 = feedDoc(c12, d12);
    CHECK(!ok12 && !strcmp(c12.lastError(), "unsupported bit depth"),
          "16-bit gray refused with a clear reason, not garbage rows");
  }

  // ---------- regression: PCL page size follows the header PageSize ----------
  // The prologue always sent ESC&l26A (A4). Letter-width rasters (we advertise
  // na_letter!) landed on a narrower declared page and were clipped. Now the
  // PageSize points select the PCL code: 612 pt -> Letter (2A), 595 -> A4 (26A),
  // anything else falls back to A4.
  {
    auto pageSizeTest = [&](uint32_t pw, const char* expect, const char* absent, const char* msg) {
      outBytes.clear();
      std::vector<uint8_t> d;
      d.push_back('R'); d.push_back('a'); d.push_back('S'); d.push_back('2');
      std::vector<uint8_t> h(PWG_HDR_SIZE, 0);
      auto p = [&](uint16_t off, uint32_t val) {
        h[off] = val >> 24; h[off+1] = val >> 16; h[off+2] = val >> 8; h[off+3] = val;
      };
      p(PWG_OFF_HWRES_X, 300); p(PWG_OFF_HWRES_Y, 300);
      p(PWG_OFF_WIDTH, 8); p(PWG_OFF_HEIGHT, 1);
      p(PWG_OFF_BPP, 1); p(PWG_OFF_BYTESLINE, 1);
      p(PWG_OFF_COLORSPACE, 3);
      p(PWG_OFF_PAGESIZE, pw); p(PWG_OFF_PAGESIZE + 4, 792);
      memcpy(&h[0], "PwgRaster", 9);
      d.insert(d.end(), h.begin(), h.end());
      d.push_back(0); d.push_back(0); d.push_back(0x00);
      PwgToPcl c;
      c.begin(fakeLpt, nullptr);
      feedDoc(c, d);
      c.end();
      drainDoc(c);
      std::string s(outBytes.begin(), outBytes.end());
      CHECK(s.find(expect) != std::string::npos && s.find(absent) == std::string::npos, msg);
    };
    pageSizeTest(612, "\x1b&l2A", "\x1b&l26A", "Letter (612 pt) -> PCL code 2A, not 26A");
    pageSizeTest(595, "\x1b&l26A", "\x1b&l2A;", "A4 (595 pt) -> PCL code 26A");
  }

  // ---------- non-blocking pipeline (2.4.3): HOLD, dosing, no byte lost ------
  // A busy port (sink returns HOLD for stretches of bytes) must never lose
  // or duplicate output: every held byte is retried by pump() and the
  // document's output must equal the unheld run byte-for-byte.
  {
    outBytes.clear();
    heldPauses = 0;
    const uint32_t W = 64, H = 8;
    std::vector<uint8_t> d13;
    d13.push_back('R'); d13.push_back('a'); d13.push_back('S'); d13.push_back('2');
    std::vector<uint8_t> h13(PWG_HDR_SIZE, 0);
    auto p13 = [&](uint16_t off, uint32_t val) {
      h13[off] = val >> 24; h13[off+1] = val >> 16; h13[off+2] = val >> 8; h13[off+3] = val;
    };
    p13(PWG_OFF_HWRES_X, 300); p13(PWG_OFF_HWRES_Y, 300);
    p13(PWG_OFF_WIDTH, W); p13(PWG_OFF_HEIGHT, H);
    p13(PWG_OFF_BPP, 8); p13(PWG_OFF_BYTESLINE, W);
    p13(PWG_OFF_COLORSPACE, 18);
    memcpy(&h13[0], "PwgRaster", 9);
    d13.insert(d13.end(), h13.begin(), h13.end());
    for (uint32_t r = 0; r < H; r++) {
      d13.push_back(0);                       // row once
      d13.push_back(63); d13.push_back(0x00); // repeat 64 black px
    }
    // baseline: clean run
    PwgToPcl base;
    base.begin(fakeLpt, nullptr);
    feedDoc(base, d13);
    base.end();
    drainDoc(base);
    std::string baseline(outBytes.begin(), outBytes.end());
    size_t baselineRows = base.rowsPrinted(), baselinePages = base.pagesPrinted();

    // held run: sink refuses every 3rd byte once (HOLD), pump retries it
    outBytes.clear(); holderN = 0;
    PwgToPcl held;
    held.begin(heldSink, nullptr);
    bool okh = true;
    for (uint8_t b : d13) {
      if (held.needsPump()) {
        PwgToPcl::Feed pf = held.pump(1000);
        if (pf == PwgToPcl::Feed::FAIL) { okh = false; break; }
      }
      PwgToPcl::Feed f = held.feed(b);
      if (f == PwgToPcl::Feed::FAIL) { okh = false; break; }
    }
    held.end();
    for (;;) {
      PwgToPcl::Feed pf = held.pump(1000);
      if (pf == PwgToPcl::Feed::OK) break;
      if (pf == PwgToPcl::Feed::FAIL) { okh = false; break; }
    }
    CHECK(okh, "held pipeline: document consumed despite HOLD backpressure");
    std::string heldOut(outBytes.begin(), outBytes.end());
    CHECK(heldOut == baseline,
          "held pipeline: output is byte-identical to the unheld run");
    CHECK(held.rowsPrinted() == baselineRows && held.pagesPrinted() == baselinePages,
          "held pipeline: same rows/pages as the unheld run");
  }

  printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
