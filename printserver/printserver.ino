/*
    This file is part of printserver-esp8266.

    printserver-esp8266 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    printserver-esp8266 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with printserver-esp8266.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  * printserver.ino - ESP32-S3 generic LPT/Centronics WiFi print server.
  * Port of gianluca-nitti/printserver-esp8266 (GPLv3), heavily reworked.
  *
 * Features:
 *  [done] direct LPT drive w/ full status lines, OLED, WiFi/AP+web setup,
 *         spool queue on LittleFS, Raw 9100, mDNS advertizing, IPP 1.1
 *         Get-Printer-Attributes / Validate-Job / Print-Job;
 *         PWG-raster (Android native print) decoded and converted to PCL
 *         at print time (PwgToPcl.h); raw PCL/PJL jobs pass through;
 *         OTA firmware update from the web panel (/update, dual app slots).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>                     // OTA: write the uploaded bin into the
                                        // idle app slot, then reboot into it
#include "config.h"
#include "ParallelPort.h"
#include "StatusDisplay.h"
#include "StatusLed.h"
#include "PrintQueue.h"
#include "NetService.h"
#include "PwgToPcl.h"
#include "WebPages.h"
#include "IppBody.h"
// per-service mDNS instance names (Android shows two entries otherwise both
// named the same); the Arduino ESPmDNS wrapper only sets ONE global instance,
// so call the IDF API it is built on directly for the second service
#include "mdns.h"

ParallelPort lpt;
StatusDisplay display;
StatusLed led;
PrintQueue queue;
NetService net;

// ---------------- raw 9100 (JetDirect) ----------------
// Spool slots shared by ALL inbound protocols: the raw 9100 accept loop AND
// the IPP Print-Job handler pick their slot from the same rawActive[] array,
// while the queue itself (PrintQueue) carries MAX_CONCURRENT_JOBS writers.
// Hardcoding 2 here left slots 2..3 of the queue unreachable and made IPP
// refuse a job whenever both raw slots were busy, despite spare capacity.
#define MAX_RAW_CLIENTS MAX_CONCURRENT_JOBS
WiFiServer rawServer(RAW_PORT);
WiFiClient rawClients[MAX_RAW_CLIENTS];
bool rawActive[MAX_RAW_CLIENTS];
// 2.4.0 lazy spool: true once the slot's first byte opened the spool file.
// A raw connection that never sends (Android printer probing) stays at
// false and leaves no .tmp behind.
bool rawSpooled[MAX_RAW_CLIENTS] = {false};
// last time each raw slot saw a byte. A slot whose client goes silent for
// RAW_IDLE_TIMEOUT_MS (crashed sender / half-open TCP that never FINs) is
// reaped below - without this, four such ghosts wedge every slot and the
// box stops accepting jobs until rebooted.
uint32_t rawLastByte[MAX_RAW_CLIENTS] = {0};

// ---------------- HTTP (status + wifi setup) ----------------
WiFiServer httpServer(HTTP_PORT);

// ---------------- IPP ----------------
WiFiServer ippServer(IPP_PORT);

// ---------------- print engine ----------------
enum class EngineState { IDLE, RECEIVING, PRINTING };
EngineState engState = EngineState::IDLE;
uint32_t printBytes = 0;
UiState uiFor(EngineState e) {
  return e == EngineState::RECEIVING ? UiState::RECEIVING :
         e == EngineState::PRINTING  ? UiState::PRINTING  :
         (net.isAp() ? UiState::AP_MODE : UiState::CONNECTED);
}

void feedByteToLpt(uint8_t b) {
  if (lpt.writeByte(b, LPT_BUSY_TIMEOUT_MS)) return;
  // BUSY never fell for the full timeout. Before dropping the half-printed
  // job, try the one recovery the Centronics interface actually has: the
  // INIT pulse (>= 50 us, IEEE 1284) resets the printer's interface logic
  // and often recovers a hung BUSY. If the byte then goes through, the job
  // continues seamlessly from this very byte.
  lpt.resetPrinter();
  if (lpt.writeByte(b, 2000)) return;   // INIT recovered it
  // Still dead: the job is half-printed; keeping it in the queue would
  // reprint the WHOLE job later (garbage). Dropping loses the rest - the
  // operator can reprint from source.
  queue.dropCurrent();
  engState = EngineState::IDLE;
  display.setError("нет ответа LPT");
  display.setState(uiFor(engState));
}

// ---- BOOT button: hold BTN_HOLD_MS to force the configuration AP --------
// Polled in loop() and in the blocking-connect pumps (net.waitConnected /
// net.connectTo), so the button also works while the box is stuck joining
// a bad network - exactly when the user needs it most.
uint32_t btnDownSince = 0;          // 0 = not pressed
void pollButton() {
  if (digitalRead(PIN_BTN) == LOW) {          // pressed (button pulls GPIO0 low)
    if (!btnDownSince) btnDownSince = millis();
    if (millis() - btnDownSince >= BTN_HOLD_MS && !net.isForcedAp()) {
      // fires once per hold: forceAp() sets the static forcedAp flag, and
      // the !isForcedAp() guard above keeps it from re-firing while held.
      // It re-arms for the next hold after new credentials are saved
      // (connectTo() clears the flag) or after a reboot.
      Serial.println("[btn] long press -> forcing configuration AP");
      net.forceAp();                          // STA down, AP up, retries off
      display.setState(UiState::AP_MODE);     // line 2 says "настройка" (not
      led.setMode(UiState::AP_MODE);          // "сеть не найдена") via isForcedAp()
    }
  } else {
    btnDownSince = 0;
  }
}

// Keep LED and OLED telling the same story: call wherever the UiState or
// the printer-attention condition changes.
void syncLed() {
  led.setMode(display.uiState());
  led.setAttention(lpt.status() == LptStatus::PAPER_OUT ||
                   lpt.status() == LptStatus::ERROR_STATE);
}

// Panel keep-alive for code paths that BLOCK loop() for seconds (the wifi
// join waits, the IPP transfer loops): OLED, LED, LPT polling and the BOOT
// button all stay serviced while a handler sits in a read loop. NOT called
// from the OTA flash-write loop - see the comment there (RMT/I2C activity
// during Update.write() erase windows); LittleFS page writes from the
// spoolers are short, and loop() itself already interleaves them with the
// panel ticks.
void panelPump() {
  lpt.poll();
  display.tick();
  syncLed();
  led.tick();
  pollButton();
  delay(1);
}

// ---- PWG->PCL conversion state (active when queue.currentJobIsPwg()) ----
PwgToPcl pwg;
bool pwgActive = false;
bool pwgFailed = false;
// Job id the print engine currently works on. hasData() advances the reader
// to the NEXT queued job (silently - it closes/removes the old file and
// opens the new one inside the while-condition), so this id is compared on
// every byte to detect job boundaries inside one 512-byte chunk.
uint32_t engineJobId = 0;

// callback: PCL output byte -> LPT. Same recovery as feedByteToLpt(): a hung
// BUSY gets one INIT pulse + short retry; if that also fails, return false
// so the decoder aborts (the job is then dropped via the pwgFailed branch).
// Returning false used to be pointless - the decoder ignored it and kept
// stalling 120 s PER OUTPUT BYTE on a dead printer.
bool pwgOut(void*, uint8_t b) {
  printBytes++;
  if (lpt.writeByte(b, LPT_BUSY_TIMEOUT_MS)) return true;
  lpt.resetPrinter();
  return lpt.writeByte(b, 2000);
}

// Set up (or tear down) the converters for the queue's CURRENT job. Called at
// the IDLE->PRINTING start AND at every job boundary inside the byte loop:
// the engine must never stream a new job through the previous job's setup -
// a .okw arriving after a .ok used to go to the port raw (garbage pages),
// and anything after a .okw hit the live decoder ("bad sync"/"bad header")
// and was dropped. Only a single-job queue ever printed correctly.
void engineSwitchJob() {
  if (pwgActive) {                 // finish the converted job: raster end +
    pwg.end();                     // form feed flush the last partial page
    pwgActive = false;
  }
  engineJobId = queue.currentJobId();
  if (queue.currentJobIsPwg()) {
    pwg.begin(pwgOut, nullptr);    // fresh decoder state for the new job
    pwgActive = true;
    pwgFailed = false;
    Serial.println("[pwg] job is image/pwg-raster -> converting to PCL");
  }
}

void processQueue() {
  net.setBusy(engState != EngineState::IDLE);   // defer AP->STA switchover while a
                                                // job is in flight (it would die)
  if (engState == EngineState::RECEIVING) return;    // spool first
  if (engState == EngineState::IDLE && queue.hasData()) {
    engState = EngineState::PRINTING;
    display.setState(UiState::PRINTING);
    engineSwitchJob();                            // converters for job #1
  }
  if (engState == EngineState::PRINTING) {
    LptStatus st = lpt.status();
    if (st == LptStatus::PAPER_OUT || st == LptStatus::ERROR_STATE) {
      display.setPrinterStatus(st);      // keep job, wait for operator
      return;
    }
    // Printer KNOWN to be connected (SELECT driven high at least once) but
    // deselected right now (powered off / offline button): hold the job and
    // wait - the moment poll() sees SELECT high again, printing resumes from
    // exactly this byte. A port where SELECT never asserted (nothing wired)
    // does NOT pause here: writeByte fails fast (~1 s) and the job drops
    // with "нет ответа LPT" instead of silently piling up forever.
    if (lpt.status() == LptStatus::OFFLINE && lpt.selectEverWired()) {
      display.setPrinterStatus(LptStatus::OFFLINE);
      return;
    }
    if (pwgFailed) {                     // broken raster: drop, keep serving
      queue.dropCurrent();
      pwgFailed = false; pwgActive = false;
      // say WHY the page died: pwg.lastError() is "bad sync"/"bad header"/
      // "line too wide"/"unsupported colorspace"/"output stalled" - the serial
      // log gets the exact reason, the OLED gets the short human text
      // ("output stalled" = LPT dead, not a broken file).
      String pwgErr = pwg.lastError();
      Serial.println(String("[pwg] dropped: ") + pwgErr);
      display.setError(pwgErr == "output stalled" ? "нет ответа LPT" : "битый файл");
      engState = EngineState::IDLE;
      display.setState(uiFor(engState));
      return;
    }
    int n = 0;
    while (n < 512 && queue.hasData() && engState == EngineState::PRINTING) {
      if (queue.currentJobId() != engineJobId) {
        // hasData() just advanced the reader to the NEXT queued job (the old
        // one is exhausted). Switch converters AT the boundary so the new
        // job gets its own decoder/raw path - see engineSwitchJob().
        engineSwitchJob();
      }
      int b = queue.readByte();
      if (b < 0) break;
      if (pwgActive) {
        if (!pwg.feed((uint8_t)b)) { pwgFailed = true; break; }
        // pwgOut() already counts the CONVERTED bytes actually pushed to the
        // port; counting the source bytes here too would double-increment.
      } else {
        feedByteToLpt((uint8_t)b);
        printBytes++;
      }
      n++;
    }
    if (engState != EngineState::PRINTING) return;   // dropped mid-job above
    if (!queue.hasData() && !pwgFailed) {
      if (pwgActive) { pwg.end(); pwgActive = false; }
      engState = EngineState::IDLE;
      display.setState(uiFor(engState));
    }
  }
}

// ---------------- HTTP helpers ----------------
String urlDecode(String str);   // from NetService.cpp
void httpSendCode(WiFiClient& c, int code, const char* text,
                  const String& type, const String& body) {
  c.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %d\r\n\r\n",
           code, text, type.c_str(), body.length());
  c.print(body);
}
void httpSend(WiFiClient& c, const String& type, const String& body) {
  httpSendCode(c, 200, "OK", type, body);
}

void handleHttp(WiFiClient c) {
  String req = c.readStringUntil('\r');
  c.readStringUntil('\n');
  int sp = req.indexOf(' '), sp2 = req.lastIndexOf(' ');
  if (sp <= 0 || sp2 <= sp) { c.stop(); return; }
  String method = req.substring(0, sp);
  String path   = req.substring(sp + 1, sp2);

  // drain headers. Line-capped: a client that drips a header line every
  // ~0.9 s (broken TCP stack, slowloris-ish tool) would otherwise keep this
  // loop alive indefinitely while c.connected() stays true - the box stops
  // serving. Real requests carry well under 40 header lines.
  // POST /update additionally needs two header values (the raw-body upload
  // has no other way to learn its size / password), so those are captured
  // while draining. Header names are case-insensitive (RFC 7230); values
  // may carry one leading space.
  long contentLen = -1;                 // POST /update: exact image size
  String otaKey;                        // POST /update: X-OTA-Key value, if sent
  bool otaExpect100 = false;            // POST /update: Expect: 100-Continue (curl)
  for (int lines = 0; c.connected() && lines < 40; lines++) {
    String h = c.readStringUntil('\r'); c.readStringUntil('\n');
    if (h.length() == 0) break;
    if (method == "POST" && path == "/update") {
      String hl = h; hl.toLowerCase();
      if (hl.startsWith("content-length:")) contentLen = hl.substring(15).toInt();
      else if (hl.startsWith("x-ota-key:")) {
        int v = 10;                      // past "x-ota-key:", skip spaces
        while (v < (int)h.length() && h.charAt(v) == ' ') v++;
        otaKey = h.substring(v);
      }
      else if (hl.startsWith("expect:") && hl.indexOf("100-continue") >= 0)
        otaExpect100 = true;             // same handling as handleIpp below
    }
  }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    String body = String("<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>") + PRINTER_NAME + " " + FIRMWARE_VERSION + "</title>"
      "<style>body{font-family:sans-serif;max-width:640px;margin:2em auto;padding:0 1em}"
      "h1{font-size:1.4em}h2{font-size:1.1em}table{border-collapse:collapse;width:100%;margin:.4em 0 1em}"
      "td,th{border:1px solid #ccc;padding:.4em}</style></head><body>"
      "<h1>" PRINTER_NAME "</h1>";
    // status table with the firmware version as its first row (the user
    // needs to see which build the box runs - especially with OTA updates)
    // - see WebPages.h
    body += indexStatusTable(lptStatusName(lpt.status()), net.info(),
                             queue.pendingCount(), printBytes,
                             queue.usedBytes(), FIRMWARE_VERSION);
    // print endpoints: one endpoint per row (was one long line the user
    // found hard to read) - see WebPages.h
    body += printEndpointsTable(net.ip());
    body += "<p><a href='/wifi'>Настройка WiFi</a> &middot; "
            "<a href='/update'>Обновление прошивки</a></p>"
            "</body></html>";
    httpSend(c, "text/html; charset=utf-8", body);
  } else if (method == "GET" && path == "/wifi") {
    // Wi-Fi setup page: scanned network list (click to pick) AND manual
    // SSID/password entry - both ways, per user request.
    String body = "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'><title>WiFi</title>"
      "<style>body{font-family:sans-serif;max-width:640px;margin:1em auto;padding:0 1em}"
      "h1{font-size:1.3em}.net{display:block;padding:.55em .7em;margin:.25em 0;"
      "border:1px solid #ccc;border-radius:6px;text-decoration:none;color:#000;"
      "display:flex;justify-content:space-between;gap:.5em}.net:hover{background:#eef}"
      ".sig{white-space:nowrap;color:#666}input{margin:.2em 0 .6em;padding:.4em;"
      "width:100%;box-sizing:border-box}button{padding:.5em 1.2em}"
      ".hint{color:#666;font-size:.9em}#spin{display:none}</style></head><body>"
      "<h1>Настройка WiFi</h1>"
      "<p class=hint>Нажмите на сеть из списка или введите SSID вручную.</p>"
      "<div id=list><span id=spin>Поиск сетей...</span></div>"
      "<h2 style='font-size:1.1em'>Ручной ввод</h2>"
      "<form method=POST action=/wifi-connect>"
      "SSID: <input name=SSID id=ssid maxlength=32 required><br>"
      "Пароль: <input type=password name=password maxlength=64><br>"
      "<button>Подключить</button></form>"
      "<p>Текущее: " + net.info() + "</p>"
      "<p class=hint>Версия прошивки: " FIRMWARE_VERSION "</p>"
      "<script>"
      // poll every second until the scan is done, then render the list
      "function load(){fetch('/scan').then(r=>r.json()).then(a=>{"
      "if(a.busy){setTimeout(load,1000);return;}"
      "var L=document.getElementById('list');L.innerHTML='';"
      "if(!a.length){L.innerHTML='<p class=hint>Сети не найдены - введите SSID вручную.</p>';}"
      "a.sort((x,y)=>x.ssid.localeCompare(y.ssid)).forEach(n=>{"
      "var a2=document.createElement('a');a2.className='net';"
      "a2.href='javascript:void(0)';"
      "a2.onclick=()=>{document.getElementById('ssid').value=n.ssid;window.scrollTo(0,document.body.scrollHeight)};"
      "var s=document.createElement('span');s.textContent=n.ssid;"
      "var r=document.createElement('span');r.className='sig';"
      "r.textContent=(n.sec?'[закрытая] ':'[открытая] ')+n.rssi+' dBm';"
      "a2.appendChild(s);a2.appendChild(r);L.appendChild(a2);});"
      "}).catch(()=>{document.getElementById('spin').textContent='Скан недоступен, введите SSID вручную';});}"
      "load();"
      "</script></body></html>";
    httpSend(c, "text/html; charset=utf-8", body);
  } else if (method == "GET" && path == "/scan") {
    // Wi-Fi scan endpoint for the /wifi page. First call kicks an async
    // scan (~1.6 s with the fast 120 ms/channel timing) and reports busy;
    // the page polls every second until results (or an error) come back.
    String json;
    int st = net.scanStatus();
    if (st == -1) {                       // still scanning
      json = "{\"busy\":true}";
    } else if (st == 0) {                 // idle: kick one off, caller retries
      net.startScan();
      json = "{\"busy\":true}";
    } else if (st == -2) {                // scan failed
      json = "[]";
    } else {
      json = net.scanJson();              // st == 1: results ready (possibly 0)
    }
    httpSend(c, "application/json", json);
  } else if (method == "POST" && path == "/wifi-connect") {
    // tiny urlencoded parser
    String body = c.readString();
    String ssid, pass;
    int a = body.indexOf("SSID="), b = body.indexOf("&password=");
    if (a >= 0) ssid = urlDecode(body.substring(a + 5, b));
    if (b >= 0) pass = urlDecode(body.substring(b + 10));
    httpSend(c, "text/html; charset=utf-8", "<html><body>Подключение к " + ssid + "...</body></html>");
    delay(500);
    display.setState(UiState::WIFI_CONNECTING);   // repaint before the blocking
    display.tick();                               // connectTo() so the user sees it
    net.connectTo(ssid, pass, panelPump);
    display.setState(net.isAp() ? UiState::AP_MODE : UiState::CONNECTED);
  } else if (method == "GET" && path == "/update") {
    // OTA page: file picker + progress bar; the actual upload logic lives
    // in WebPages.h (page) and the POST branch below (flash write).
    bool busy = engState != EngineState::IDLE || queue.pendingCount() > 0;
    httpSend(c, "text/html; charset=utf-8",
             otaPageHtml(busy, strlen(OTA_PASS) > 0, FIRMWARE_VERSION));
  } else if (method == "POST" && path == "/update") {
    // OTA flash write. The browser's XHR sends the .bin as the raw POST
    // body with Content-Length - no multipart to parse. Design notes:
    //  - refused while a job prints/spools (see otaPostGate in WebPages.h):
    //    the flash write hogs the CPU for the whole transfer and the
    //    post-success reboot would kill an in-flight job mid-page
    //  - first byte must be 0xE9 (ESP image magic) - wrong file refused
    //    early with a clear message, not after a full flash write
    //  - the read loop yields with delay(1) (task watchdog) and polls only
    //    the LPT GPIO lines - no OLED/LED updates during flash writes, see
    //    the comment inside the loop
    //  - Update.end(true) validates the image (checksum) before the
    //    response is sent; only then ESP.restart() into the new slot
    bool busy = engState != EngineState::IDLE || queue.pendingCount() > 0;
    bool keyRequired = strlen(OTA_PASS) > 0;
    bool keyOk = !keyRequired || otaKey == OTA_PASS;
    int gate = otaPostGate(busy, keyRequired, keyOk, contentLen > 0);
    if (gate != 200) {
      const char* st = gate == 503 ? "Service Unavailable" :
                       gate == 401 ? "Unauthorized" : "Bad Request";
      httpSendCode(c, gate, st, "text/plain; charset=utf-8", String(otaGateText(gate)));
    } else {
      // gate passed: unblock a curl that waits for 100-Continue before
      // sending its body (browsers never ask, this is curl/headless only).
      // Sent only AFTER the gate - a refused request gets the final status
      // immediately and the body is never transferred.
      if (otaExpect100) c.print("HTTP/1.1 100 Continue\r\n\r\n");
      if (!Update.begin((size_t)contentLen)) {
        // too big for the idle OTA slot (or flash layer trouble)
        httpSendCode(c, 500, "Internal Server Error", "text/plain; charset=utf-8",
                     String("Update.begin: ") + Update.errorString());
      } else {
        String fail;                       // "" until something goes wrong
        uint8_t ubuf[2048];
        long got = 0;
        uint32_t idle = 0;                 // ms without a byte
        bool magicChecked = false;
        while (got < contentLen) {
          int av = c.available();
          if (av > 0) {
            int n = c.read(ubuf, min((size_t)av, min((size_t)(contentLen - got), sizeof(ubuf))));
            if (n <= 0) { fail = "обрыв приёма данных"; break; }
            if (!magicChecked) {
              magicChecked = true;
              if (!otaMagicOk(ubuf[0])) { fail = "файл не похож на прошивку ESP32 (нет магического байта 0xE9)"; break; }
            }
            if (Update.write(ubuf, (size_t)n) != (size_t)n) {
              fail = String("ошибка записи flash: ") + Update.errorString();
              break;
            }
            got += n;
            idle = 0;
          } else {
            // dead peer fast-fail: connected() stays true while data is still
            // buffered, so reaching here with it false means the socket is
            // gone AND everything sent was consumed - no point waiting out
            // the full OTA_XFER_TIMEOUT_MS for a client that already left.
            if (!c.connected()) { fail = "обрыв соединения"; break; }
            delay(1);                       // yield: watchdog + stack
            if (++idle > OTA_XFER_TIMEOUT_MS) { fail = "тайм-аут приёма (клиент пропал)"; break; }
            // Deliberately NO display.tick()/led.tick() in this loop, unlike
            // the /wifi-connect pump: Update.write() opens flash erase/
            // program windows with the cache off and interrupts restricted,
            // and the WS2812 (RMT) / OLED (I2C) drivers do not guarantee
            // IRAM-safe ISRs - one firing mid-erase can panic the box.
            // The panel simply holds its last state for the few seconds of
            // the upload. LPT polling is pure GPIO reads (safe) and keeps
            // the "printer unplugged" detection live. pollButton is skipped
            // for the same radio reason as before.
            lpt.poll();
          }
        }
        if (fail.length() == 0 && got == contentLen && Update.end(true)) {
          Serial.printf("[ota] %ld bytes written into the idle slot, rebooting\n", (long)contentLen);
          httpSend(c, "text/plain; charset=utf-8", "OK. Прошивка записана, перезагрузка...");
          delay(1000);                      // let the response reach the browser
          ESP.restart();                    // boot into the new image
        } else {
          if (fail.length() == 0)
            fail = String("Update.end: ") + Update.errorString();
          Update.abort();                   // leave the running image untouched
          Serial.println(String("[ota] failed: ") + fail);
          httpSendCode(c, 500, "Internal Server Error", "text/plain; charset=utf-8", fail);
        }
      }
    }
  } else {
    c.printf("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  }
  c.stop();
}

// ---------------- minimal IPP (RFC 8011 subset) ----------------
// Supports: Get-Printer-Attributes, Validate-Job, Print-Job, Create-Job +
// Send-Document (the two-step flavour Android's IPP stack actually uses),
// Get-Job-Attributes, Get-Jobs, Cancel-Job (client-side cancel).
// Print data is spooled raw (PCL/PJL passthrough). If the client sends
// image/pwg-raster (Android's built-in print service does, since we
// advertise it), the spooled file is converted to PCL at print time - see
// processQueue() and PwgToPcl.h.
//
// All body reads go through ipp::BodyStream (IppBody.h) so a chunked
// Transfer-Encoding is decoded for the IPP header AND attribute groups
// too - the old code read chunk-size lines as IPP bytes, got garbage
// operation ids and rejected valid jobs (2.4.0).

// live spool handles for Send-Document: job created by Create-Job, waiting
// for its document. slot == -1 = none. Only ONE at a time: Android sends
// Create-Job -> Send-Document back to back on the SAME connection pool.
static int ippOpenSlot = -1;
static uint32_t ippOpenJobId = 0;
static uint32_t ippOpenSince = 0;
// Create-Job->Send-Document pairing needs the job-id from the LAST
// Create-Job response; when a Create-Job call comes with an open spool
// already hanging (client died between the two calls), the stale spool
// is dropped first (removeJobById, below).
static uint32_t ippLastCreatedId = 0;

// status code -> short serial text so every IPP answer line says its verdict
const char* ippStatusText(uint16_t st) {
  switch (st) {
    case 0x0000: return "successful-ok";
    case 0x0400: return "client-error-bad-request";
    case 0x0404: return "client-error-not-found";
    case 0x040C: return "client-error-operation-not-supported";
    case 0x0408: return "client-error-request-too-long";
    case 0x0503: return "server-error-device-error";
    case 0x0506: return "server-error-not-accepting-jobs";
    default:     return "status-(unknown)";
  }
}

// Wrap the accepted WiFiClient in the host-testable BodySource interface.
struct IppClientSource : public ipp::BodySource {
  WiFiClient c;
  explicit IppClientSource(WiFiClient client) : c(client) {}
  int  read(uint8_t* buf, size_t n) override { return c.read(buf, n); }
  int  available() override { return c.available(); }
  bool connected() override { return c.connected(); }
  void pump() override { panelPump(); }
};

void handleIpp(WiFiClient c) {
  IppClientSource src(c);

  // ---- parse HTTP headers (content-length or chunked) ----
  int contentLength = -1;
  bool chunked = false;
  bool expect100 = false;
  String reqLine = c.readStringUntil('\r'); c.readStringUntil('\n');
  // Minimal request line the client actually sent - Android adds a printer
  // by bare IP and probes ports; when a job never arrives ("q=0" in the
  // [dbg] line, no [ipp] log at all) the probe/POST line tells whether the
  // phone reached the box at all and which path it hit. Empty request lines
  // (the [ipp] blank in the field log) are TCP keepalives from port probes.
  Serial.println(String("[ipp] ") + (reqLine.length() ? reqLine.substring(0, 60) : "(empty line - port probe)"));
  // Line-capped like handleHttp: see the comment there.
  for (int lines = 0; c.connected() && lines < 40; lines++) {
    String h = c.readStringUntil('\r'); c.readStringUntil('\n');
    if (h.length() == 0) break;
    h.toLowerCase();
    if (h.startsWith("content-length:")) contentLength = h.substring(15).toInt();
    else if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") >= 0) chunked = true;
    else if (h.startsWith("expect:") && h.indexOf("100-continue") >= 0) expect100 = true;
  }
  if (contentLength == -1 && !chunked) {
    Serial.println("[ipp] refused: neither Content-Length nor chunked framing - 400");
    // answer, do not just drop the socket: a silently dropped connection is
    // indistinguishable from a dead box and the client retries forever
    c.printf("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    c.stop();
    return;
  }

  if (expect100) c.print("HTTP/1.1 100 Continue\r\n\r\n");

  // ---- IPP body over ONE transport view (Content-Length or chunked) ----
  // BodyStream de-chunks EVERYTHING that follows - header, attribute groups
  // and (in the spool path) the document - so the parsing code below never
  // sees a chunk boundary as an IPP byte (2.4.0; the old code parsed the
  // raw socket and mis-parsed chunked requests).
  ipp::Framing framing = chunked ? ipp::Framing::CHUNKED : ipp::Framing::LENGTH;
  ipp::BodyStream bs(src, framing, contentLength, 10000);
  size_t hdrGot = 0;
  uint8_t hdr[8];
  while (hdrGot < 8) {
    int r = bs.read(hdr + hdrGot, 8 - hdrGot);
    if (r > 0) { hdrGot += (size_t)r; continue; }
    if (r == 0 && c.connected()) { continue; }     // wait state (pump ran)
    Serial.println("[ipp] refused: IPP header never arrived - 400");
    c.printf("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    c.stop();
    return;
  }
  uint16_t opId = (hdr[2] << 8) | hdr[3];
  uint32_t reqId = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];

  bool createJob = (opId == 0x0005);  // Create-Job
  bool sendDoc   = (opId == 0x0006);  // Send-Document
  int slot = -1;
  if (createJob) {
    // Same capacity gate as raw-accept and Print-Job.
    for (int i = 0; i < MAX_RAW_CLIENTS; i++) if (!rawActive[i]) { slot = i; break; }
    if (slot >= 0 && (!queue.canStore() || !queue.startJob(slot))) slot = -1;
    if (slot >= 0) {
      rawActive[slot] = true;
      ippOpenSlot = slot; ippOpenJobId = queue.jobId(slot); ippOpenSince = millis();
      ippLastCreatedId = ippOpenJobId;
      if (engState == EngineState::IDLE) {
        engState = EngineState::RECEIVING;
        display.setState(UiState::RECEIVING);
      }
    }
  } else if (sendDoc) {
    // Pair with the job Create-Job opened. Android does Create-Job ->
    // Send-Document on the same connection; the job-id attribute in the
    // request may be absent (implicit "last created").
    if (ippOpenSlot >= 0 && !rawActive[ippOpenSlot]) ippOpenSlot = -1;  // stale
    slot = ippOpenSlot;
    if (slot >= 0) {
      if (engState == EngineState::IDLE) {
        engState = EngineState::RECEIVING;
        display.setState(UiState::RECEIVING);
      }
    }
  }

  // ---- walk attribute groups to find end-of-attributes (0x03) ----
  // While walking, remember the requested document-format so we know whether
  // the incoming document is PWG raster (Android) or raw PCL (desktops).
  String docFormat = "application/octet-stream";
  uint32_t reqJobId = 0;      // Send-Document/Cancel/Get-Job-Attributes target
  bool inOpGroup = true;   // first group after header is operation-attributes
  {
    // bounded by the transport: BodyStream ends at the body end; a peer
    // that stalls mid-attributes is cut off by its idle timeout (10 s)
    while (true) {
      uint8_t tb;
      int r = bs.read(&tb, 1);
      if (r < 0) break;
      int tag = tb;
      if (tag == 0x03) break;                       // end of attributes
      if (tag >= 0x01 && tag <= 0x05) { inOpGroup = (tag == 0x01); continue; }
      if (tag < 0x10) break;                        // out of spec; bail to data
      // value-tag: read name-len, name, value-len, value
      auto readU16 = [&]() -> uint16_t {
        uint8_t b2[2];
        if (bs.read(b2, 2) != 2) return 0xFFFF;
        return (uint16_t)((b2[0] << 8) | b2[1]);
      };
      uint16_t nlen = readU16();
      String name;
      if (nlen != 0 && nlen != 0xFFFF) {
        for (uint16_t i = 0; i < nlen; i++) {
          uint8_t ch;
          if (bs.read(&ch, 1) != 1) { nlen = 0xFFFF; break; }
          name += (char)ch;
        }
      }
      uint16_t vlen = readU16();
      String val;
      if (vlen != 0xFFFF) {
        for (uint16_t i = 0; i < vlen; i++) {
          uint8_t ch;
          if (bs.read(&ch, 1) != 1) { vlen = 0xFFFF; break; }
          val += (char)ch;
        }
        if (inOpGroup && name == "document-format") docFormat = val;
        if (inOpGroup && name == "job-id") reqJobId = (uint32_t)val.toInt();
      }
      if (nlen == 0xFFFF || vlen == 0xFFFF) break;
    }
  }

  bool isPwg = docFormat == "image/pwg-raster";

  // response builder
  String ippResp = "HTTP/1.1 200 OK\r\nContent-Type: application/ipp\r\n"
                   "Connection: close\r\n\r\n";
  auto attrStr = [&](uint8_t tag, const char* name, const char* val) {
    uint16_t nl = strlen(name), vl = strlen(val);
    ippResp += (char)tag; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
    ippResp += name;
    ippResp += (char)(vl >> 8); ippResp += (char)(vl & 255);
    ippResp += val;
  };
  // multi-value: first value carries the name, the rest have empty name
  auto attrStrMulti = [&](uint8_t tag, const char* name, std::initializer_list<const char*> vals) {
    bool first = true;
    for (const char* v : vals) {
      uint16_t nl = first ? strlen(name) : 0, vl = strlen(v);
      ippResp += (char)tag; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
      if (first) ippResp += name;
      ippResp += (char)(vl >> 8); ippResp += (char)(vl & 255);
      ippResp += v;
      first = false;
    }
  };
  // integer (0x21) / enum (0x23) values are BINARY 4-byte. The old code sent
  // queued-job-count / copies-default / printer-up-time as keyword strings
  // ("0"/"1") - a type mismatch a strict RFC 8011 client rejects the whole
  // response for - and job-id/printer-state were hand-rolled per-attribute.
  auto attrInt = [&](uint8_t tag, const char* name, uint32_t v) {
    uint16_t nl = strlen(name);
    ippResp += (char)tag; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
    ippResp += name;
    ippResp += (char)0; ippResp += (char)4;
    ippResp += (char)(v >> 24); ippResp += (char)(v >> 16);
    ippResp += (char)(v >> 8);  ippResp += (char)(v);
  };
  ippResp += (char)0x01; ippResp += (char)0x01;   // version 1.1
  uint16_t status = 0;
  ippResp += (char)(status >> 8); ippResp += (char)(status & 255);
  ippResp += (char)((reqId >> 24) & 255); ippResp += (char)((reqId >> 16) & 255);
  ippResp += (char)((reqId >> 8) & 255); ippResp += (char)(reqId & 255);
  // remember where the status-code bytes sit inside ippResp so a refusal
  // (disk full) can patch them in place later - they follow the HTTP header
  // ("\r\n\r\n") and the 2 version bytes.
  const size_t ippBodyStart = ippResp.indexOf("\r\n\r\n") + 4;
  const size_t statusOff = ippBodyStart + 2;
  ippResp += (char)0x01;                          // operation attributes tag
  attrStr(0x47, "attributes-charset", "utf-8");
  attrStr(0x48, "attributes-natural-language", "en-us");

  bool supported = (opId == 0x000B || opId == 0x0004 || opId == 0x0002 ||
                    opId == 0x0005 || opId == 0x0006 || opId == 0x0009 ||
                    opId == 0x000A || opId == 0x0008);
  // Get-Printer-Attributes / Validate-Job / Print-Job / Create-Job /
  // Send-Document / Get-Job-Attributes / Get-Jobs / Cancel-Job
  if (supported) {
    if (opId == 0x000B) {
      ippResp += (char)0x04;                       // printer-attributes-tag
      // NOTE ordering below follows the RFC 8011 "required order" so strict
      // clients (Android wprint, iOS) parse it: charset, natural-language,
      // printer-uri-* ..., then everything else.
      attrStr(0x45, "printer-uri-supported", ("ipp://" + net.ip() + ":" + String(IPP_PORT) + "/lp").c_str());
      attrStr(0x44, "uri-security-supported", "none");
      attrStr(0x44, "uri-authentication-supported", "none");
      attrStr(0x42, "printer-name", PRINTER_NAME);
      attrStr(0x41, "printer-make-and-model", "ESP Print Server (parallel port)");
      attrStr(0x41, "printer-info", PRINTER_INFO);
      attrStr(0x41, "printer-location", "LPT");
      // PWG raster first: Android wprint picks the format by exact string
      // match against document-format-supported; without image/pwg-raster
      // here, Android shows "принтер не поддерживается".
      attrStrMulti(0x49, "document-format-supported",
                   {"image/pwg-raster", "application/vnd.hp-pcl", "application/octet-stream"});
      attrStrMulti(0x44, "ipp-versions-supported", {"1.1", "2.0"});
      // operations-supported: the FULL set this build actually answers -
      // clients probe it (Get-Printer-Attributes + CUPS ops) to decide
      // whether two-step Create-Job/Send-Document may be used. The old list
      // lacked Create-Job/Send-Document/Get-Job-Attributes/Get-Jobs/
      // Cancel-Job, so Android's IPP stack either fell back to Print-Job or
      // failed on its preferred path.
      attrStrMulti(0x44, "operations-supported",
                   {"0x0002", "0x0004", "0x0005", "0x0006", "0x0008", "0x0009", "0x000A", "0x000B"});
      // printer-state: RFC 8011 enum, emitted ONCE (a keyword "idle" copy
      // used to precede it - two attributes with the same name and
      // conflicting types in one group). Live: 5 (processing) while the
      // engine feeds a job, 3 (idle) otherwise.
      attrInt(0x23, "printer-state", engState == EngineState::PRINTING ? 5 : 3);
      // honest reasons: paper-out and LPT error are things the client/user
      // can actually act on; a dead disk is "other"
      attrStr(0x44, "printer-state-reasons",
              lpt.status() == LptStatus::PAPER_OUT ? "media-empty" :
              lpt.status() == LptStatus::ERROR_STATE ? "other" : "none");
      // boolean printer-is-accepting-jobs: tied to actual spool capacity
      // (flash below the free margin = not accepting), not a hard "yes"
      {
        const char* n = "printer-is-accepting-jobs";
        uint16_t nl = strlen(n);
        ippResp += (char)0x22; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
        ippResp += n; ippResp += (char)0; ippResp += (char)1;
        ippResp += (char)(queue.canStore() ? 1 : 0);
      }
      attrInt(0x21, "queued-job-count", (uint32_t)queue.pendingCount());   // live, not "0"
      attrStrMulti(0x44, "media-supported", {"iso_a4_210x297mm", "na_letter_8.5x11in"});
      attrStr(0x44, "media-default", "iso_a4_210x297mm");
      attrStr(0x44, "print-color-mode-default", "monochrome");
      attrStrMulti(0x44, "print-color-mode-supported", {"monochrome"});
      attrStr(0x44, "pdl-override-supported", "not-attempted");
      attrStrMulti(0x44, "compression-supported", {"none"});
      attrStrMulti(0x44, "sides-supported", {"one-sided"});
      attrInt(0x21, "copies-default", 1);
      attrInt(0x21, "printer-up-time", millis() / 1000);
      // pwg-raster-document-parameters Android looks for. A B&W printserver
      // must advertise BOTH types (Michael Sweet's guidance on the PWG list,
      // https://www.pwg.org/archives/ipp/2014/018345.html): black_1 (1-bit)
      // and sgray_8 (8-bit gray) - whichever the client picks, the firmware
      // decoder handles both (PwgToPcl). srgb_8 is also accepted (luma).
      attrStr(0x44, "pwg-raster-document-resolution-supported", "300dpi");
      attrStrMulti(0x44, "pwg-raster-document-type-supported",
                   {"black_1", "sgray_8", "srgb_8"});
      attrStr(0x44, "pwg-raster-document-sheet-back", "normal");
      attrStr(0x44, "printer-device-id", "MFG:ESP;MDL:PrintServer;CMD:PCL;");
      attrStrMulti(0x44, "printer-resolution-supported", {"300dpi"});
    } else if (opId == 0x0002 || opId == 0x0005 || opId == 0x0006) {
      // job attributes for Print-Job / Create-Job / Send-Document.
      // REAL spool id (see PrintQueue::jobId): a hardcoded 1 pointed at a
      // phantom /job/1 that could collide with a truly spooled job #1.
      uint32_t jid;
      if (opId == 0x0002) {
        jid = 0;   // Print-Job: id assigned when the spool opens (below)
      } else if (opId == 0x0005) {
        jid = slot >= 0 ? queue.jobId(slot) : 0;
      } else {              // Send-Document: the paired job id
        jid = (slot >= 0) ? ippOpenJobId : 0;
      }
      ippResp += (char)0x02;                       // job attributes tag
      attrInt(0x21, "job-id", jid);                // integer
      if (opId == 0x0005) {
        attrInt(0x23, "job-state", 3);             // pending: document not yet sent
      } else {
        attrInt(0x23, "job-state", 3);             // enum: 3 = pending
      }
      attrStr(0x44, "job-state-reasons", "none");
      attrStr(0x42, "job-uri", ("ipp://" + net.ip() + ":" + String(IPP_PORT) + "/lp/job/" + String(jid)).c_str());
    } else if (opId == 0x0009 || opId == 0x000A) {   // Get-Job-Attributes / Get-Jobs
      // honest minimal answer: no job history kept after print - completed
      // jobs vanish from the spool, so unknown-job / empty list.
      if (opId == 0x0009) {
        ippResp += (char)0x02;                     // job attributes tag
        // (job unknown: only unsupplied attributes are "unknown")
      }
      // Get-Jobs: no jobs-list group at all
    } else if (opId == 0x0008) {                   // Cancel-Job
      // cancel the pending job if it is still waiting in the spool; a job
      // mid-print cannot be stopped mid-page (no pause/resume on LPT)
      bool cancelled = queue.removeJobById(reqJobId ? reqJobId : ippLastCreatedId);
      if (cancelled) Serial.printf("[ipp] Cancel-Job: removed job %u from the spool\n",
                                    (unsigned)(reqJobId ? reqJobId : ippLastCreatedId));
      // status stays 0 unless the job is unknown (handled by removeJobById
      // returning false -> 0x0404 below)
      if (!cancelled) {
        ippResp = ippResp.substring(0, statusOff) + (char)0x04 + (char)0x04 +
                  ippResp.substring(statusOff + 2);
      }
    }
    ippResp += (char)0x03;                         // end-of-attributes
    bool stored = true;                            // endJob verdict (below)
    bool truncated = false;                        // transfer ended before all
                                                   // promised bytes (denied below)
    // stream the document data (after the attr groups) into the spool.
    // Handles both Content-Length and chunked transfer via BodyStream, and
    // knows the doc format (isPwg) - PWG raster files get converted to PCL
    // at print time. Send-Document closes the spool; a Create-Job with no
    // document keeps its slot open for the following Send-Document.
    if ((opId == 0x0002 || opId == 0x0006) && slot >= 0) {
      if (opId == 0x0002) {                     // one-shot job: open its own spool now
        for (int i = 0; i < MAX_RAW_CLIENTS; i++) if (!rawActive[i]) { slot = i; break; }
        if (slot >= 0 && (!queue.canStore() || !queue.startJob(slot))) slot = -1;
        if (slot >= 0) {
          rawActive[slot] = true;
          if (engState == EngineState::IDLE) {
            engState = EngineState::RECEIVING;
            display.setState(UiState::RECEIVING);
          }
        }
      }
      if (slot >= 0) {
        queue.markFormat(slot, isPwg);
        uint8_t buf[1024];
        size_t spoolBytes = 0;        // bytes actually stored (for the log line)
        int r;
        while ((r = bs.read(buf, sizeof(buf))) != 0) {
          if (r < 0) { truncated = true; break; }
          queue.write(slot, buf, (size_t)r);
          spoolBytes += (size_t)r;
        }
        uint32_t jid = queue.jobId(slot);    // 0 after endJob - capture first
        stored = queue.endJob(slot, truncated);
        rawActive[slot] = false;
        if (opId == 0x0006) { ippOpenSlot = -1; ippOpenJobId = 0; }
        Serial.printf("[ipp] %s: %u B (job %u)%s\n",
                      opId == 0x0006 ? "Send-Document" : "Print-Job",
                      (unsigned)spoolBytes, (unsigned)jid,
                      truncated ? " - DENIED (truncated transfer)" :
                      stored ? " stored" : " DENIED");
        // IDLE only from RECEIVING - see the slot-taking comment above: never
        // clobber PRINTING, or the running job's decoder restarts mid-file.
        if (engState == EngineState::RECEIVING) {
          engState = EngineState::IDLE;
          display.setState(uiFor(engState));
        }
      }
    }
    if ((opId == 0x0002 || opId == 0x0005) && slot < 0) {
      // Create-Job/Print-Job refused BEFORE spooling (disk full / too many
      // concurrent jobs): 0x0506 not-accepting rather than a bare device
      // error - the client can hold its job and retry later.
      ippResp = ippResp.substring(0, statusOff) + (char)0x05 + (char)0x06 +
                ippResp.substring(statusOff + 2);
    } else if (slot >= 0 && !stored) {
      // refused BEFORE spooling (disk full / too many concurrent jobs), the
      // flash filled mid-transfer, or the transfer was truncated: tell the
      // client honestly instead of "OK" + a vanishing/half job. RFC 8011:
      // 0x0503 = server-error-device-error (our side is full/broken),
      // 0x0400 = client-error-bad-request (truncated request body).
      uint16_t deny = truncated ? 0x0400 : 0x0503;
      ippResp = ippResp.substring(0, statusOff) + (char)(deny >> 8) + (char)(deny & 255) +
                ippResp.substring(statusOff + 2);
    }
    // final status text into the serial verdict line
    c.print(ippResp);
  } else {
    // unsupported operation (e.g. CUPS Get-Notifications): respond with
    // client-error-operation-not-supported (0x040C) and a VALID attribute
    // list. The old code appended two stray bytes where attributes were
    // expected and left the success status (0x0000) in place - strict
    // clients failed parsing the whole response.
    ippResp = ippResp.substring(0, statusOff) + (char)0x04 + (char)0x0C +
              ippResp.substring(statusOff + 2);
    ippResp += (char)0x03;                         // end-of-attributes
    c.print(ippResp);
  }
  Serial.printf("[ipp] op=0x%04X -> %s\n", (unsigned)opId,
                ippStatusText((uint16_t)((unsigned char)ippResp[statusOff] << 8 |
                                        (unsigned char)ippResp[statusOff + 1])));
  c.stop();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  // WIFI_CONNECTING before display.begin(): begin() calls render() itself,
  // so "ESP Print Server / Подкл. к ХХХ" is on screen the moment the OLED is
  // initialised - not only after the (blocking) net.begin().
  display.setState(UiState::WIFI_CONNECTING);
  display.begin();
  led.begin();                            // status LED mirrors the OLED from now on
  pinMode(PIN_BTN, INPUT_PULLUP);         // BOOT button: hold = configuration AP

  LittleFS.begin(true);
  lpt.begin();
  queue.begin();   // reboot policy: wipes any queued jobs (see PrintQueue)
  if (queue.purgedAtBoot() > 0)
    Serial.printf("[q] reboot policy: purged %d queued job(s) from flash\n",
                  queue.purgedAtBoot());
  net.begin();                           // non-blocking join with stored credentials
  net.waitConnected(panelPump);
  display.setState(net.isAp() ? UiState::AP_MODE : UiState::CONNECTED);

  rawServer.begin();
  httpServer.begin();
  ippServer.begin();

  // mDNS: advertize _ipp._tcp (Android native discovery), _pdl-datastream (raw)
  // Per-service instance names (2.4.0): Android lists every advertised
  // service as a candidate printer; with one shared instance name the list
  // showed two identical "ESP32-S3 WiFi-to_LPT print server" entries and
  // there was no telling which is IPP and which the raw socket. The suffix
  // tells the user which endpoint a picked entry talks to. Arduino's
  // MDNS.setInstanceName sets ONE global name for all services, so the
  // second (socket) name goes through the IDF API the wrapper is built on.
  if (MDNS.begin(NetService::hostName().c_str())) {
    MDNS.setInstanceName(PRINTER_NAME);            // base name for _ipp
    MDNS.addService("ipp", "tcp", IPP_PORT);
    MDNS.addServiceTxt("ipp", "tcp", "rp", "lp");
    MDNS.addServiceTxt("ipp", "tcp", "note", PRINTER_NAME);
    MDNS.addServiceTxt("ipp", "tcp", "UUID",
      String("964") + String((uint32_t)ESP.getEfuseMac(), HEX) + "-esp32s3");
    MDNS.addService("pdl-datastream", "tcp", RAW_PORT);
    mdns_service_instance_name_set("_pdl-datastream", "_tcp", PRINTER_NAME_SOCKET);
    MDNS.addService("http", "tcp", HTTP_PORT);
  }

  lpt.onStatusChange = [](LptStatus s) {
    display.setPrinterStatus(s);
    // "ошибка принтера" (not a bare "ошибка"): in AP mode the user thought
    // the box itself / the WiFi was failing, while this is the LPT ERROR line.
    if (s == LptStatus::ERROR_STATE) display.setError("ошибка принтера");
    else display.clearError();
  };
  display.setPrinterStatus(lpt.status());   // pump the boot-time status in:
  // with no printer on the port the display must say "принтер не подключен"
  // right away - the callback only fires on a status CHANGE
  Serial.println("boot ok: " + net.info());
}

void loop() {
  bool wasAp = net.isAp();
  net.tick();
  lpt.poll();
  display.tick();
  pollButton();                           // BOOT hold: force AP (works while joining too)
  syncLed();                              // LED mirrors the OLED state + printer attention
  led.tick();

  // self-heal made visible: when the supervised AP retry joined the router,
  // refresh the display state (IP line + "готов") instead of showing stale
  // AP info until the next print job
  if (wasAp != net.isAp())
    display.setState(net.isAp() ? UiState::AP_MODE : UiState::CONNECTED);

  // raw 9100 accept + pump
  // LAZY spool (2.4.0): Android/HyperOS "add printer" probes port 9100 by
  // opening a connection and closing it without sending a byte. The old
  // accept path opened a spool file and turned the OLED to "Загрузка
  // данных" for every such probe - four probes, four .tmp files, "purged 4
  // queued job(s)" after the reboot. Now the slot is only ATTACHED to the
  // queue when the first byte arrives; probe connections that close with
  // zero bytes leave no trace.
  WiFiClient nc = rawServer.accept();
  if (nc) {
    int slot = -1;
    for (int i = 0; i < MAX_RAW_CLIENTS; i++) if (!rawActive[i]) { slot = i; break; }
    if (slot < 0) {
      Serial.println("[raw] connect refused: no slot free");
      nc.stop();      // all slots busy: refuse the connection
    } else {
      Serial.println("[raw] connect accepted (JetDirect 9100)");
      rawClients[slot] = nc; rawActive[slot] = true;
      rawSpooled[slot] = false;               // no spool file yet - see below
      rawLastByte[slot] = millis();
    }
  }
  for (int i = 0; i < MAX_RAW_CLIENTS; i++) {
    if (!rawActive[i] || !rawClients[i]) continue;
    int av = rawClients[i].available();
    if (av > 0) {
      // No canStore() poll here (it walks the FS): a mid-receive fill is
      // caught exactly by PrintQueue's sticky write-failure flag - writeByte
      // returns 0 once the FS is full, endJob() then discards the truncated
      // job instead of finalizing a half page. The accept-time check below
      // covers the "already full" case.
      rawLastByte[i] = millis();
      if (!rawSpooled[i]) {
        // first payload byte: NOW commit the slot to the queue (startJob
        // opens the .tmp file and the OLED goes to "Загрузка данных").
        // canStore() / startJob() failing here means flash full: drop the
        // connection instead of silently swallowing the job.
        if (!queue.canStore() || !queue.startJob(i)) {
          Serial.println("[raw] first byte refused: flash full - dropping connection");
          rawClients[i].stop();
          rawActive[i] = false;
          continue;
        }
        rawSpooled[i] = true;
        // RECEIVING only from IDLE. Overwriting PRINTING here would pause the
        // in-flight job, and the later IDLE->PRINTING transition restarts the
        // PWG decoder on a mid-file reader -> "bad sync" -> job dropped.
        // Receiving while printing is fine: spool and print run in parallel.
        if (engState == EngineState::IDLE) {
          engState = EngineState::RECEIVING;
          display.setState(UiState::RECEIVING);
        }
      }
      uint8_t buf[512];
      int n = rawClients[i].read(buf, min(av, (int)sizeof(buf)));
      for (int k = 0; k < n; k++) queue.writeByte(i, buf[k]);
    } else if (!rawClients[i].connected()) {
      // a probe (never spooled) closes here with zero trace; a spooled
      // client finishing its transfer finalizes the job
      if (rawSpooled[i]) queue.endJob(i, false);
      rawClients[i].stop();
      rawActive[i] = false;
      // leave RECEIVING only (never clobber PRINTING - decoder-restart bug,
      // see the first-byte comment above)
      if (engState == EngineState::RECEIVING) {
        engState = EngineState::IDLE;
        display.setState(uiFor(engState));
      }
    } else if (millis() - rawLastByte[i] > RAW_IDLE_TIMEOUT_MS) {
      // connected() still true but not a byte in RAW_IDLE_TIMEOUT_MS: the
      // peer is a ghost (crashed mid-send / half-open connection). The
      // spooled data is incomplete - cancel it (endJob discards), free the
      // slot. Real print streams never pause this long mid-job.
      Serial.printf("[raw] slot %d idle %lus - dropping ghost job\n", i, (unsigned long)RAW_IDLE_TIMEOUT_MS / 1000);
      if (rawSpooled[i]) queue.endJob(i, true);
      rawClients[i].stop();
      rawActive[i] = false;
      if (engState == EngineState::RECEIVING) {
        engState = EngineState::IDLE;
        display.setState(uiFor(engState));
      }
    }
  }

  // a Create-Job whose Send-Document never came (client died between the
  // two calls) holds a spool slot + a .tmp file forever. Bound it: half a
  // minute without the pairing request finalizes what arrived (usually
  // nothing) and frees the slot.
  if (ippOpenSlot >= 0 && millis() - ippOpenSince > 30000) {
    Serial.println("[ipp] Create-Job pairing timed out - dropping half-open spool");
    queue.endJob(ippOpenSlot, true);
    rawActive[ippOpenSlot] = false;
    ippOpenSlot = -1; ippOpenJobId = 0;
    if (engState == EngineState::RECEIVING) {
      engState = EngineState::IDLE;
      display.setState(uiFor(engState));
    }
  }

  // HTTP + IPP accept
  if (WiFiClient hc = httpServer.accept()) handleHttp(hc);
  if (WiFiClient ic = ippServer.accept()) handleIpp(ic);

  processQueue();

  // status LED heartbeat + debug
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 10000) {
    lastDbg = millis();
    Serial.printf("[dbg] %s | lpt=%s | q=%d | heap=%u\n",
      net.info().c_str(), lptStatusName(lpt.status()), queue.pendingCount(),
      (unsigned)ESP.getFreeHeap());
  }
  delay(1);
}
