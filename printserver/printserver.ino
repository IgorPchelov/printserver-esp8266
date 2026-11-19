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
/*
  * printserver.ino - ESP32-S3 generic LPT/Centronics WiFi print server.
  * Port of gianluca-nitti/printserver-esp8266 (GPLv3), heavily reworked.
  *
  * Features:
  *  [done] direct LPT drive w/ full status lines, OLED, WiFi/AP+web setup,
  *         spool queue on LittleFS, Raw 9100, mDNS advertizing, IPP 1.1
  *         Get-Printer-Attributes / Validate-Job / Print-Job;
  *         PWG-raster (Android native print) decoded and converted to PCL
  *         at print time (PwgToPcl.h); raw PCL/PJL jobs pass through.
  */
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include "config.h"
#include "ParallelPort.h"
#include "StatusDisplay.h"
#include "PrintQueue.h"
#include "NetService.h"
#include "PwgToPcl.h"

ParallelPort lpt;
StatusDisplay display;
PrintQueue queue;
NetService net;

// ---------------- raw 9100 (JetDirect) ----------------
#define MAX_RAW_CLIENTS 2
WiFiServer rawServer(RAW_PORT);
WiFiClient rawClients[MAX_RAW_CLIENTS];
bool rawActive[MAX_RAW_CLIENTS];

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
  if (!lpt.writeByte(b, LPT_BUSY_TIMEOUT_MS)) {
    display.setError("ошибка LPT");
    engState = EngineState::IDLE;
  }
}

// ---- PWG->PCL conversion state (active when queue.currentJobIsPwg()) ----
PwgToPcl pwg;
bool pwgActive = false;
bool pwgFailed = false;

// callback: PCL output byte -> LPT
bool pwgOut(void*, uint8_t b) {
  printBytes++;
  return lpt.writeByte(b, LPT_BUSY_TIMEOUT_MS);
}

void processQueue() {
  if (engState == EngineState::RECEIVING) return;    // spool first
  if (engState == EngineState::IDLE && queue.hasData()) {
    engState = EngineState::PRINTING;
    display.setState(UiState::PRINTING);
    if (queue.currentJobIsPwg()) {                   // set up the converter
      pwg.begin(pwgOut, nullptr);
      pwgActive = true; pwgFailed = false;
      Serial.println("[pwg] job is image/pwg-raster -> converting to PCL");
    }
  }
  if (engState == EngineState::PRINTING) {
    if (LptStatus st = lpt.status(); st == LptStatus::PAPER_OUT || st == LptStatus::ERROR_STATE) {
      display.setPrinterStatus(st);      // keep job, wait for operator
      return;
    }
    if (pwgFailed) {                     // broken raster: drop, keep serving
      queue.dropCurrent();
      pwgFailed = false; pwgActive = false;
      display.setError("ошибка PWG");
      engState = EngineState::IDLE;
      display.setState(uiFor(engState));
      return;
    }
    int n = 0;
    while (n < 512 && queue.hasData()) {
      int b = queue.readByte();
      if (b < 0) break;
      if (pwgActive) {
        if (!pwg.feed((uint8_t)b)) { pwgFailed = true; break; }
      } else {
        feedByteToLpt((uint8_t)b);
      }
      printBytes++;
      n++;
    }
    if (!queue.hasData() && !pwgFailed) {
      if (pwgActive) { pwg.end(); pwgActive = false; }
      engState = EngineState::IDLE;
      display.setState(uiFor(engState));
    }
  }
}

// ---------------- HTTP helpers ----------------
String urlDecode(String str);   // from NetService.cpp
void httpSend(WiFiClient& c, const String& type, const String& body) {
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %d\r\n\r\n",
           type.c_str(), body.length());
  c.print(body);
}

void handleHttp(WiFiClient c) {
  String req = c.readStringUntil('\r');
  c.readStringUntil('\n');
  int sp = req.indexOf(' '), sp2 = req.lastIndexOf(' ');
  if (sp <= 0 || sp2 <= sp) { c.stop(); return; }
  String method = req.substring(0, sp);
  String path   = req.substring(sp + 1, sp2);

  // drain headers
  while (c.connected()) {
    String h = c.readStringUntil('\r'); c.readStringUntil('\n');
    if (h.length() == 0) break;
  }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    String body = String("<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>") + PRINTER_NAME + "</title>"
      "<style>body{font-family:sans-serif;max-width:640px;margin:2em auto;padding:0 1em}"
      "h1{font-size:1.4em}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccc;padding:.4em}</style></head><body>"
      "<h1>" PRINTER_NAME "</h1>"
      "<table><tr><th>Состояние</th><td>" + lptStatusName(lpt.status()) + "</td></tr>"
      "<tr><th>Сеть</th><td>" + net.info() + "</td></tr>"
      "<tr><th>Заданий в очереди</th><td>" + String(queue.pendingCount()) + "</td></tr>"
      "<tr><th>Печатано байт</th><td>" + String(printBytes) + "</td></tr>"
      "<tr><th>Очередь на флэше</th><td>" + String(queue.usedBytes()) + " Б</td></tr></table>"
      "<p><a href='/wifi'>Настройка WiFi</a> &middot; печать: "
      "socket://" + net.ip() + ":" + String(RAW_PORT) +
      " &middot; ipp://" + net.ip() + ":" + String(IPP_PORT) + "/lp</p>"
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
    // scan (~2-3 s) and reports busy; the page polls every second until
    // results (or an error) come back.
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
      json = net.scanJson();              // st = number of results
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
    net.connectTo(ssid, pass);
    display.setState(net.isAp() ? UiState::AP_MODE : UiState::CONNECTED);
  } else {
    c.printf("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  }
  c.stop();
}

// ---------------- minimal IPP (RFC 8011 subset) ----------------
// Supports: Get-Printer-Attributes, Validate-Job, Print-Job.
// Print-Job data is spooled raw (PCL/PJL passthrough). If the client sends
// image/pwg-raster (Android's built-in print service does, since we
// advertise it), the spooled file is converted to PCL at print time - see
// processQueue() and PwgToPcl.h.
void handleIpp(WiFiClient c) {
  // ---- parse HTTP headers (content-length or chunked) ----
  int contentLength = -1;
  bool chunked = false;
  bool expect100 = false;
  String reqLine = c.readStringUntil('\r'); c.readStringUntil('\n');
  while (c.connected()) {
    String h = c.readStringUntil('\r'); c.readStringUntil('\n');
    if (h.length() == 0) break;
    h.toLowerCase();
    if (h.startsWith("content-length:")) contentLength = h.substring(15).toInt();
    else if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") >= 0) chunked = true;
    else if (h.startsWith("expect:") && h.indexOf("100-continue") >= 0) expect100 = true;
  }
  if (contentLength == -1 && !chunked) { c.stop(); return; }

  if (expect100) c.print("HTTP/1.1 100 Continue\r\n\r\n");

  // IPP body: version(2) op(2) reqid(4) attr-groups...
  uint8_t hdr[8];
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < 8 && millis() - t0 < 5000) {
    if (c.available()) hdr[got++] = c.read(); else delay(1);
  }
  if (got < 8) { c.stop(); return; }
  uint16_t opId = (hdr[2] << 8) | hdr[3];
  uint32_t reqId = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];

  bool spooling = (opId == 0x0002);   // Print-Job
  int slot = -1;
  if (spooling) {
    for (int i = 0; i < MAX_RAW_CLIENTS; i++) if (!rawActive[i]) { slot = i; break; }
    if (slot >= 0) { queue.startJob(slot); rawActive[slot] = true; engState = EngineState::RECEIVING;
      display.setState(UiState::RECEIVING); }
  }

  // ---- walk attribute groups to find end-of-attributes (0x03) ----
  // While walking, remember the requested document-format so we know whether
  // the incoming document is PWG raster (Android) or raw PCL (desktops).
  String docFormat = "application/octet-stream";
  bool inOpGroup = true;   // first group after header is operation-attributes
  {
    uint32_t tWalk = millis();
    while (millis() - tWalk < 10000) {
      int b = -1;
      // read with timeout helper
      while ((b = c.available()) == 0) { if (millis() - tWalk > 10000) break; delay(1); }
      if (b <= 0 && !c.available()) break;
      int tag = c.read();
      if (tag == 0x03) break;                       // end of attributes
      if (tag >= 0x01 && tag <= 0x05) { inOpGroup = (tag == 0x01); continue; }
      if (tag < 0x10) break;                        // out of spec; bail to data
      // value-tag: read name-len, name, value-len, value
      auto readU16 = [&]() -> uint16_t {
        while (c.available() < 2 && millis() - tWalk < 10000) delay(1);
        if (c.available() < 2) return 0xFFFF;
        uint16_t hi = c.read(); return (hi << 8) | c.read();
      };
      uint16_t nlen = readU16();
      String name;
      if (nlen != 0 && nlen != 0xFFFF) {
        while (c.available() < nlen && millis() - tWalk < 10000) delay(1);
        for (uint16_t i = 0; i < nlen && c.available(); i++) name += (char)c.read();
      }
      uint16_t vlen = readU16();
      if (vlen != 0xFFFF) {
        while (c.available() < vlen && millis() - tWalk < 10000) delay(1);
        String val;
        for (uint16_t i = 0; i < vlen && c.available(); i++) val += (char)c.read();
        if (inOpGroup && name == "document-format") docFormat = val;
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
  ippResp += (char)0x01; ippResp += (char)0x01;   // version 1.1
  uint16_t status = 0;
  ippResp += (char)(status >> 8); ippResp += (char)(status & 255);
  ippResp += (char)((reqId >> 24) & 255); ippResp += (char)((reqId >> 16) & 255);
  ippResp += (char)((reqId >> 8) & 255); ippResp += (char)(reqId & 255);
  ippResp += (char)0x01;                          // operation attributes tag
  attrStr(0x47, "attributes-charset", "utf-8");
  attrStr(0x48, "attributes-natural-language", "en-us");

  if (opId == 0x000B || opId == 0x0004 || opId == 0x0002) {  // Get attrs / validate / print
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
      attrStr(0x41, "printer-info", "ESP32-S3 WiFi-to-LPT print server");
      attrStr(0x41, "printer-location", "LPT");
      // PWG raster first: Android wprint picks the format by exact string
      // match against document-format-supported; without image/pwg-raster
      // here, Android shows "принтер не поддерживается".
      attrStrMulti(0x49, "document-format-supported",
                   {"image/pwg-raster", "application/vnd.hp-pcl", "application/octet-stream"});
      attrStrMulti(0x44, "ipp-versions-supported", {"1.1", "2.0"});
      attrStr(0x44, "printer-state", "idle");
      attrStr(0x44, "printer-state-reasons", "none");
      // boolean printer-is-accepting-jobs
      {
        const char* n = "printer-is-accepting-jobs";
        uint16_t nl = strlen(n);
        ippResp += (char)0x22; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
        ippResp += n; ippResp += (char)0; ippResp += (char)1; ippResp += (char)1;
      }
      // enum printer-state (3 = idle)
      {
        const char* n = "printer-state";
        uint16_t nl = strlen(n);
        ippResp += (char)0x23; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
        ippResp += n; ippResp += (char)0; ippResp += (char)4;
        ippResp += (char)0; ippResp += (char)0; ippResp += (char)0; ippResp += (char)3;
        // (string duplicate above removed in this edit)
      }
      attrStr(0x44, "queued-job-count", "0");
      attrStrMulti(0x44, "media-supported", {"iso_a4_210x297mm", "na_letter_8.5x11in"});
      attrStr(0x44, "media-default", "iso_a4_210x297mm");
      attrStr(0x44, "print-color-mode-default", "monochrome");
      attrStrMulti(0x44, "print-color-mode-supported", {"monochrome"});
      attrStr(0x44, "pdl-override-supported", "not-attempted");
      attrStrMulti(0x44, "compression-supported", {"none"});
      attrStrMulti(0x44, "sides-supported", {"one-sided"});
      attrStr(0x44, "copies-default", "1");
      attrStr(0x44, "printer-up-time", String(millis() / 1000).c_str());
      // pwg-raster-document-parameters Android looks for:
      attrStr(0x44, "pwg-raster-document-resolution-supported", "300dpi");
      attrStr(0x44, "pwg-raster-document-type-supported", "black_1");
      attrStr(0x44, "pwg-raster-document-sheet-back", "normal");
      attrStr(0x44, "printer-device-id", "MFG:ESP;MDL:PrintServer;CMD:PCL;");
      attrStrMulti(0x44, "printer-resolution-supported", {"300dpi"});
    } else if (opId == 0x0002) {
      ippResp += (char)0x02;                       // job attributes tag
      // job-id integer 1
      uint16_t nl = strlen("job-id");
      ippResp += (char)0x21; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
      ippResp += "job-id";
      ippResp += (char)0; ippResp += (char)4;
      ippResp += (char)0; ippResp += (char)0; ippResp += (char)0; ippResp += (char)1;
      // enum job-state (3 = pending)
      nl = strlen("job-state");
      ippResp += (char)0x23; ippResp += (char)(nl >> 8); ippResp += (char)(nl & 255);
      ippResp += "job-state";
      ippResp += (char)0; ippResp += (char)4;
      ippResp += (char)0; ippResp += (char)0; ippResp += (char)0; ippResp += (char)3;
      attrStr(0x44, "job-state-reasons", "none");
      attrStr(0x42, "job-uri", ("ipp://" + net.ip() + ":" + String(IPP_PORT) + "/lp/job/1").c_str());
    }
    ippResp += (char)0x03;                         // end-of-attributes
    // stream the document data (after the attr groups) into the spool.
    // Handles both Content-Length and chunked transfer, and knows the doc
    // format (isPwg) - PWG raster files get converted to PCL at print time.
    if (spooling && slot >= 0) {
      queue.markFormat(slot, isPwg);
      uint8_t buf[1024];
      uint32_t idle = 0;
      long remaining = contentLength;   // -1 if chunked
      size_t chunkLeft = 0;
      bool chunkHeaderPending = true;   // need to read a chunk-size line first
      while (c.connected() && idle < 10000) {
        int av = c.available();
        if (av <= 0) { idle += 10; delay(10); continue; }
        idle = 0;
        if (chunked) {
          if (chunkHeaderPending) {
            String line = c.readStringUntil('\r'); c.read(); // consume \n
            chunkLeft = strtol(line.c_str(), NULL, 16);
            chunkHeaderPending = false;
            if (chunkLeft == 0) break;             // terminal chunk
            continue;
          }
          int n = c.read(buf, min((size_t)av, min(chunkLeft, sizeof(buf))));
          chunkLeft -= n; remaining -= n;
          queue.write(slot, buf, n);
          if (chunkLeft == 0) {                    // consume trailing CRLF
            while (c.available() < 2 && idle < 1000) { delay(1); }
            c.read(); c.read();
            chunkHeaderPending = true;
          }
        } else {
          if (remaining == 0) break;
          int n = c.read(buf, min((size_t)av, min((size_t)remaining, sizeof(buf))));
          remaining -= n;
          queue.write(slot, buf, n);
        }
      }
      queue.endJob(slot, false);
      rawActive[slot] = false;
      engState = EngineState::IDLE;
      display.setState(uiFor(engState));
    }
    c.print(ippResp);
  } else {
    // unsupported op
    ippResp += (char)0x05;   // unsupported
    ippResp += (char)0x03;
    c.print(ippResp);
  }
  c.stop();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  // WIFI_CONNECTING before display.begin(): begin() calls render() itself,
  // so "ESP Print Server / Подключение к WiFi..." is on screen the moment
  // the OLED is initialised - not only after the (blocking) net.begin().
  display.setState(UiState::WIFI_CONNECTING);
  display.begin();

  LittleFS.begin(true);
  lpt.begin();
  queue.begin();
  net.begin();
  display.setState(net.isAp() ? UiState::AP_MODE : UiState::CONNECTED);

  rawServer.begin();
  httpServer.begin();
  ippServer.begin();

  // mDNS: advertize _ipp._tcp (Android native discovery), _pdl-datastream (raw)
  if (MDNS.begin(NetService::hostName().c_str())) {
    MDNS.setInstanceName(PRINTER_NAME);
    MDNS.addService("ipp", "tcp", IPP_PORT);
    MDNS.addServiceTxt("ipp", "tcp", "rp", "lp");
    MDNS.addServiceTxt("ipp", "tcp", "note", PRINTER_NAME);
    MDNS.addServiceTxt("ipp", "tcp", "UUID",
      String("964") + String((uint32_t)ESP.getEfuseMac(), HEX) + "-esp32s3");
    MDNS.addService("pdl-datastream", "tcp", RAW_PORT);
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
  net.tick();
  lpt.poll();
  display.tick();

  // raw 9100 accept + pump
  WiFiClient nc = rawServer.accept();
  if (nc) {
    for (int i = 0; i < MAX_RAW_CLIENTS; i++) {
      if (!rawActive[i]) {
        rawClients[i] = nc; rawActive[i] = true;
        queue.startJob(i);
        engState = EngineState::RECEIVING;
        display.setState(UiState::RECEIVING);
        break;
      }
      if (i == MAX_RAW_CLIENTS - 1) nc.stop();
    }
  }
  for (int i = 0; i < MAX_RAW_CLIENTS; i++) {
    if (!rawActive[i] || !rawClients[i]) continue;
    int av = rawClients[i].available();
    if (av > 0) {
      uint8_t buf[512];
      int n = rawClients[i].read(buf, min(av, (int)sizeof(buf)));
      for (int k = 0; k < n; k++) queue.writeByte(i, buf[k]);
    } else if (!rawClients[i].connected()) {
      queue.endJob(i, false);
      rawClients[i].stop();
      rawActive[i] = false;
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
