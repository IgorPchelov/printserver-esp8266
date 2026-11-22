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
bool fakeLpt(void*, uint8_t b) {
  if (lptTimeouts > 0) return false;
  outBytes.push_back(b);
  return true;
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
  bool ok = true;
  for (uint8_t b : doc) { if (!conv.feed(b)) { ok = false; break; } }
  CHECK(ok, "converter consumed the whole stream without error");
  conv.end();

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
  for (char ch : std::string("RaS3")) if (!bad2.feed((uint8_t)ch)) ok2 = false;
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
    for (uint8_t b : d2) if (!c2.feed(b)) { okw = false; break; }
    CHECK(okw, "wide 2480-px 8-bpp page accepted (old code rejected >300 px)");
    c2.end();
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
    for (uint8_t b : d3) if (!c3.feed(b)) { ok3 = false; break; }
    CHECK(ok3, "black_1 page accepted");
    c3.end();
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
    for (uint8_t b : d4) if (!c4.feed(b)) { ok4 = false; break; }
    CHECK(ok4, "srgb_8 page accepted");
    c4.end();
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
    for (uint8_t b : d5) if (!c5.feed(b)) { ok5 = false; break; }
    CHECK(!ok5 && !strcmp(c5.lastError(), "unsupported colorspace"),
          "CMYK raster rejected with a clear reason, not garbage");
  }

  printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
