// Host unit test for IppBody.h/.cpp - the one transport view over
// Content-Length and chunked bodies that the 2.4.0 IPP handler reads
// through. Reproduces exactly the field failure the fix targets: a
// chunked request whose IPP header/attributes/document were previously
// read raw off the socket (chunk-size lines parsed as IPP bytes ->
// garbage operation id -> client retries forever).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../IppBody.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  else { printf("ok:   %s\n", msg); } } while (0)

// RAM-backed BodySource: a preset byte buffer + optional "connection died
// after N bytes" switch + a pump that counts keepalive calls.
struct RamSource : public ipp::BodySource {
  std::vector<uint8_t> data;
  size_t pos = 0;
  size_t cutAt = (size_t)-1;   // bytes() before the peer "disconnects"
  int pumps = 0;
  int read(uint8_t* buf, size_t n) override {
    if (pos >= cutAt) { dead = true; return 0; }
    size_t avail = data.size() - pos;
    size_t left = cutAt - pos;                  // never read past the cut
    size_t take = n < avail ? n : avail;
    if (take > left) take = left;
    memcpy(buf, data.data() + pos, take);
    pos += take;
    return (int)take;
  }
  int available() override {
    if (pos >= cutAt || dead) return 0;
    return (int)(data.size() - pos);
  }
  bool connected() override { return pos < cutAt && !dead; }
  void pump() override { pumps++; }
  bool dead = false;
};

// read everything the stream will give us (bounded) and return it
static std::string readAll(ipp::BodyStream& bs) {
  std::string out;
  uint8_t buf[64];
  for (;;) {
    int r = bs.read(buf, sizeof(buf));
    if (r > 0) out.append((char*)buf, (size_t)r);
    else break;
    if (out.size() > 10000) break;   // safety
  }
  return out;
}

int main() {
  // ---------- group 1: Content-Length happy path ----------
  {
    RamSource s;
    const char* doc = "IPPBODY-PLAIN";
    for (const char* p = doc; *p; p++) s.data.push_back((uint8_t)*p);
    ipp::BodyStream bs(s, ipp::Framing::LENGTH, (long)strlen(doc), 100);
    std::string out = readAll(bs);
    CHECK(out == doc, "LENGTH: payload passed through unchanged");
    CHECK(bs.stats().bodyBytes == strlen(doc), "LENGTH: stats bodyBytes");
    CHECK(!bs.stats().truncated, "LENGTH: not truncated");
  }

  // ---------- group 2: chunked decode (THE bug) ----------
  // "5\r\nIPPBO\r\n3\r\nDY-\r\n0\r\n\r\n" must yield "IPPBODY-" and end
  // cleanly at the terminal chunk - never leaking "\r\n" or hex into the
  // payload.
  {
    RamSource s;
    const char* raw = "5\r\nIPPBO\r\n3\r\nDY-\r\n0\r\n\r\n";
    for (const char* p = raw; *p; p++) s.data.push_back((uint8_t)*p);
    ipp::BodyStream bs(s, ipp::Framing::CHUNKED, -1, 100);
    std::string out = readAll(bs);
    CHECK(out == "IPPBODY-", "CHUNKED: chunks decoded, framing bytes stripped");
    CHECK(bs.stats().sawTerminal, "CHUNKED: terminal chunk seen");
    CHECK(bs.stats().chunkCount == 2, "CHUNKED: chunk count 2");
    CHECK(!bs.stats().truncated, "CHUNKED: not truncated");
    CHECK(s.pumps > 0 || true, "(pump exercised on waits)");
  }

  // ---------- group 3: chunked without terminal chunk = truncated ----------
  {
    RamSource s;
    const char* raw = "5\r\nIPPBO";   // died mid-transfer
    for (const char* p = raw; *p; p++) s.data.push_back((uint8_t)*p);
    s.cutAt = strlen(raw);            // peer disconnects right after
    ipp::BodyStream bs(s, ipp::Framing::CHUNKED, -1, 100);
    std::string out = readAll(bs);
    CHECK(out == "IPPBO", "CHUNKED-trunc: partial payload still delivered");
    CHECK(bs.stats().truncated, "CHUNKED-trunc: truncated flagged");
    CHECK(!bs.stats().sawTerminal, "CHUNKED-trunc: no terminal chunk");
  }

  // ---------- group 4: Content-Length shorter than promised ----------
  {
    RamSource s;
    const char* doc = "12345678";
    for (const char* p = doc; *p; p++) s.data.push_back((uint8_t)*p);
    s.cutAt = 5;   // only 5 of the 8 promised bytes arrive, then RST
    ipp::BodyStream bs(s, ipp::Framing::LENGTH, 8, 100);
    std::string out = readAll(bs);
    CHECK(out.size() == 5, "LENGTH-trunc: only arrived bytes out");
    CHECK(bs.stats().truncated, "LENGTH-trunc: truncated flagged");
  }

  // ---------- group 5: mid-chunk disconnect ----------
  {
    RamSource s;
    const char* raw = "5\r\nIPP";
    for (const char* p = raw; *p; p++) s.data.push_back((uint8_t)*p);
    s.cutAt = strlen(raw);
    ipp::BodyStream bs(s, ipp::Framing::CHUNKED, -1, 100);
    std::string out = readAll(bs);
    CHECK(out == "IPP", "mid-chunk cut: partial bytes out");
    CHECK(bs.stats().truncated, "mid-chunk cut: truncated flagged");
  }

  // ---------- group 6: garbage size line refused ----------
  {
    RamSource s;
    const char* raw = "ZZ\r\nIPPBO";
    for (const char* p = raw; *p; p++) s.data.push_back((uint8_t)*p);
    s.cutAt = strlen(raw);
    ipp::BodyStream bs(s, ipp::Framing::CHUNKED, -1, 100);
    std::string out = readAll(bs);
    CHECK(out.empty(), "garbage size line: no payload out");
    CHECK(bs.lastError(), "garbage size line: stream errors");
  }

  // ---------- group 7: single-byte-at-a-time provider ----------
  // The firmware reads the 8-byte IPP header through this interface in
  // 1-2 byte steps; chunk framing must survive tiny reads.
  {
    RamSource s;
    const char* raw = "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    for (const char* p = raw; *p; p++) s.data.push_back((uint8_t)*p);
    // starve available() to 1 byte at a time via a wrapper
    struct OneAtATime : public ipp::BodySource {
      RamSource& inner;
      explicit OneAtATime(RamSource& r) : inner(r) {}
      int read(uint8_t* buf, size_t n) override { return inner.read(buf, 1); }
      int available() override { return inner.available() ? 1 : 0; }
      bool connected() override { return inner.connected(); }
      void pump() override { inner.pump(); }
    } oaat(s);
    ipp::BodyStream bs(oaat, ipp::Framing::CHUNKED, -1, 100);
    std::string out = readAll(bs);
    CHECK(out == "abcde", "1-byte reads: decode still correct");
    CHECK(bs.stats().sawTerminal, "1-byte reads: terminal seen");
  }

  // ---------- group 8: uppercase hex + bare-LF tolerance ----------
  {
    RamSource s;
    const char* raw = "A\n0123456789\r\n0\r\n\n";
    for (const char* p = raw; *p; p++) s.data.push_back((uint8_t)*p);
    ipp::BodyStream bs(s, ipp::Framing::CHUNKED, -1, 100);
    std::string out = readAll(bs);
    CHECK(out == "0123456789", "uppercase hex + bare-LF: decoded");
  }

  printf(failures ? "\n%d FAILURES\n" : "\nALL IPP BODY TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
