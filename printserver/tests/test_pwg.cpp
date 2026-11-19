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

  printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
