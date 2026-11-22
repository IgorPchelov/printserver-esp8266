// test_web.cpp - host regression test for the web panel page builders
// (WebPages.h).
//
// Covers the two user-facing requirements and the OTA refusal rules:
//   - index page: raw 9100 / IPP endpoints shown ONE PER TABLE ROW (was a
//     single line "socket://ip:9100 · ipp://ip:631/lp" the user found
//     hard to read)
//   - /update page: file picker for the .bin, XHR upload with progress,
//     busy warning exactly when a job is in flight, password field
//     exactly when OTA_PASS is configured
//   - POST /update gate: busy -> 503 (wins over everything), bad key ->
//     401, no Content-Length -> 400, all clear -> 200
//   - first-byte check: only 0xE9 (ESP32 image magic) accepted
#include "../WebPages.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
static void ok(const char* m) { printf("ok:   %s\n", m); }
static void bad(const char* m) { printf("FAIL: %s\n", m); fails++; }
#define CHECK(cond, msg) (cond) ? ok(msg) : bad(msg)

// is [needle..needle+len) entirely inside the i-th <tr>...</tr> row?
static bool insideRow(const String& html, const char* needle, int row) {
  int r = -1, rEnd = -1;
  for (int i = 0; i <= row; i++) {
    r = html.indexOf("<tr>", rEnd + 1);
    if (r < 0) return false;
    rEnd = html.indexOf("</tr>", r);
    if (rEnd < 0) return false;
  }
  int p = html.indexOf(needle, (unsigned)(r + 1));
  return p > r && p < rEnd;
}

int main() {
  // ---------- print endpoints: one endpoint per row ----------
  String t = printEndpointsTable(String("10.0.0.42"));
  CHECK(t.indexOf("socket://10.0.0.42:9100") > 0, "endpoints: raw socket URI with ip+port");
  CHECK(t.indexOf("ipp://10.0.0.42:631/lp") > 0, "endpoints: ipp URI with ip+port+path");
  CHECK(t.indexOf("<table>") > 0, "endpoints: rendered as a table");
  CHECK(insideRow(t, "socket://", 0), "endpoints: raw sits in its own row 1");
  CHECK(insideRow(t, "ipp://", 1), "endpoints: ipp sits in its own row 2");
  CHECK(!insideRow(t, "ipp://", 0), "endpoints: ipp NOT in row 1 (column layout)");
  CHECK(t.indexOf("9100") < t.indexOf("631"), "endpoints: row order raw, then ipp");
  // ports must come from config.h, not be retyped here
  String cfg = String("port check: ") + String(RAW_PORT) + "/" + String(IPP_PORT);
  CHECK(cfg == "port check: 9100/631", "endpoints: RAW_PORT/IPP_PORT config values");

  // ---------- /update page ----------
  String p = otaPageHtml(false, false, "9.9.9");
  CHECK(p.indexOf("<input type=file") > 0, "ota page: file picker present");
  CHECK(p.indexOf("accept='.bin'") > 0, "ota page: .bin filter on the picker");
  CHECK(p.indexOf("id=go") > 0, "ota page: upload button present");
  CHECK(p.indexOf("XMLHttpRequest") > 0, "ota page: XHR upload (progress events)");
  CHECK(p.indexOf("onprogress") > 0, "ota page: upload progress handler");
  CHECK(p.indexOf("id=bar") > 0 && p.indexOf("id=fill") > 0, "ota page: progress bar elements");
  CHECK(p.indexOf("9.9.9") > 0, "ota page: firmware version shown");
  CHECK(p.indexOf("id=key") < 0, "ota page: no password field when OTA_PASS empty");
  CHECK(p.indexOf("заблокировано") < 0, "ota page: no busy warning when idle");
  CHECK(p.indexOf("/update") > 0, "ota page: posts to /update");

  String busy = otaPageHtml(true, false, "9.9.9");
  CHECK(busy.indexOf("заблокировано") > 0, "ota page: busy warning when printing");
  CHECK(busy.indexOf("id=key") < 0, "ota page: busy alone adds no password field");

  String keyed = otaPageHtml(false, true, "9.9.9");
  CHECK(keyed.indexOf("id=key") > 0, "ota page: password field when OTA_PASS set");
  CHECK(keyed.indexOf("заблокировано") < 0, "ota page: needKey alone adds no busy warning");
  CHECK(keyed.indexOf("X-OTA-Key") > 0, "ota page: JS sends the X-OTA-Key header");

  String both = otaPageHtml(true, true, "9.9.9");
  CHECK(both.indexOf("id=key") > 0 && both.indexOf("заблокировано") > 0,
        "ota page: busy + password independent of each other");

  // ---------- POST /update gate ----------
  CHECK(otaPostGate(true, false, true, true) == 503, "gate: busy -> 503");
  CHECK(otaPostGate(true, true, false, false) == 503, "gate: busy wins over bad key + no length");
  CHECK(otaPostGate(false, true, false, true) == 401, "gate: key required, wrong -> 401");
  CHECK(otaPostGate(false, true, true, true) == 200, "gate: key required, correct -> 200");
  CHECK(otaPostGate(false, false, false, true) == 200, "gate: no key configured -> 200");
  CHECK(otaPostGate(false, false, true, false) == 400, "gate: no Content-Length -> 400");
  CHECK(otaPostGate(false, true, false, false) == 401, "gate: key checked before length");
  CHECK(strlen(otaGateText(503)) > 0 && strlen(otaGateText(401)) > 0 &&
        strlen(otaGateText(400)) > 0, "gate: 503/401/400 carry a human reason");

  // ---------- ESP32 image magic ----------
  CHECK(otaMagicOk(0xE9), "magic: 0xE9 accepted");
  CHECK(!otaMagicOk(0x00), "magic: 0x00 rejected");
  CHECK(!otaMagicOk(0xEA), "magic: 0xEA rejected");
  CHECK(!otaMagicOk(0xE8), "magic: 0xE8 rejected");

  return fails ? 1 : 0;
}
