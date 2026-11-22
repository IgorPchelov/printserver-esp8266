/*
 * WebPages.h - pure HTML builders + request-gate logic for the web panel,
 * kept OUT of printserver.ino so the host tests can regress them directly
 * (tests/test_web.cpp): the OTA page requirements (file field, upload
 * progress, busy lock, optional password) and the index print-endpoint
 * table (one endpoint per row - the user found the old single line hard
 * to read) live here as free functions with no hardware dependencies.
 *
 * This file is part of printserver-esp8266 (GPLv3), see printserver.ino.
 */
#pragma once

#include "config.h"      // String (via Arduino.h), RAW_PORT / IPP_PORT / OTA_PASS

// ---- index page: print endpoints, one endpoint per table row -------------
// Was one long paragraph line: "печать: socket://ip:9100 · ipp://ip:631/lp".
// The endpoints are the thing users type into client dialogs, so each now
// gets its own bordered row (same table style as the status block above).
inline String printEndpointsTable(const String& ip) {
  return String("<h2 style='font-size:1.1em'>Печать</h2><table>")
       + "<tr><th>Raw 9100 (JetDirect)</th><td>socket://" + ip + ":" + String(RAW_PORT) + "</td></tr>"
       + "<tr><th>IPP (Android, CUPS)</th><td>ipp://" + ip + ":" + String(IPP_PORT) + "/lp</td></tr>"
       + "</table>";
}

// ---- /update page ---------------------------------------------------------
// busy:    engine printing or jobs spooled -> red warning (the POST gate
//          below refuses anyway; the warning just tells the user BEFORE the
//          multi-MB upload, not after)
// needKey: OTA_PASS set in config.h -> show the password field; the JS
//          sends it along as the X-OTA-Key header
// version: FIRMWARE_VERSION, shown so the user can compare old vs new bin
inline String otaPageHtml(bool busy, bool needKey, const char* version) {
  String s = String("<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Обновление</title>"
    "<style>body{font-family:sans-serif;max-width:640px;margin:1em auto;padding:0 1em}"
    "h1{font-size:1.3em}.hint{color:#666;font-size:.9em}"
    "input{display:block;margin:.3em 0 .8em}button{padding:.5em 1.2em}"
    "#bar{display:none;margin-top:1em;border:1px solid #ccc;border-radius:6px;"
    "overflow:hidden}#fill{width:0;height:1.2em;background:#2a2}"
    "#msg{margin-top:.6em}</style></head><body>"
    "<h1>Обновление прошивки</h1>"
    "<p class=hint>Текущая версия: ");
  s += version;
  s += ". Файл .bin берётся из своей сборки (pio run: .pio/build/esp32s3/firmware.bin;"
       " Arduino IDE: Скетч &rarr; Экспорт бинарного файла) или из firmware/ репозитория."
       " Прошивать только образы, собранные под ESP32-S3.</p>";
  if (busy)
    s += "<p style='color:#a00'>Идёт печать или очередь не пуста - обновление"
         " заблокировано до окончания печати.</p>";
  if (needKey)
    s += "Пароль обновления: <input type=password id=key>";
  s += "<input type=file id=f accept='.bin'>"
       "<button id=go>Загрузить и прошить</button>"
       "<div id=bar><div id=fill></div></div><div id=msg></div>"
       "<script>"
       "var m=document.getElementById('msg');"
       "function up(){"
       "var f=document.getElementById('f').files[0];"
       "if(!f){m.textContent='Сначала выберите файл .bin';return;}"
       "if(!/\\.bin$/i.test(f.name)){m.textContent='Нужен файл с расширением .bin';return;}"
       "var k=document.getElementById('key');"
       "var xhr=new XMLHttpRequest();"
       "xhr.open('POST','/update');"
       "if(k)xhr.setRequestHeader('X-OTA-Key',k.value);"
       "document.getElementById('bar').style.display='block';"
       "xhr.upload.onprogress=function(e){if(e.lengthComputable){"
       "var p=Math.round(100*e.loaded/e.total);"
       "document.getElementById('fill').style.width=p+'%';"
       "m.textContent='Загрузка '+p+'%';}};"
       "xhr.onload=function(){"
       "if(xhr.status==200){document.body.innerHTML='<h1>Обновление прошивки</h1>"
       "<p>Прошивка записана, устройство перезагружается (~15 с)."
       " Затем <a href=\"/\">откройте панель</a>.</p>';}"
       "else{m.textContent='Ошибка '+xhr.status+': '+xhr.responseText;}};"
       "xhr.onerror=function(){m.textContent='Сбой сети при передаче файла.';};"
       "m.textContent='Передача...';"
       "xhr.send(f);}"
       "document.getElementById('go').onclick=up;"
       "</script></body></html>";
  return s;
}

// ---- POST /update gate (pure decision, host-tested in test_web.cpp) ------
// Which HTTP status the POST must be answered with BEFORE any flash write:
//   503 - printing or spooled jobs: the flash write hogs the CPU for the
//         whole transfer and the post-success reboot would kill an
//         in-flight job mid-page (a half-printed page cannot be resumed)
//   401 - OTA_PASS set and the X-OTA-Key header doesn't match. Plain ==,
//         not constant-time - acceptable for this device's LAN threat
//         model (the /wifi page is equally open)
//   400 - no Content-Length: a raw-body upload needs the exact size up
//         front (Update.begin(len) and the read loop's end condition)
//   200 - proceed to Update.begin()
inline int otaPostGate(bool busy, bool keyRequired, bool keyOk, bool hasLength) {
  if (busy) return 503;
  if (keyRequired && !keyOk) return 401;
  if (!hasLength) return 400;
  return 200;
}

// one-line human reason shown by the page's error handler (plain text body)
inline const char* otaGateText(int code) {
  switch (code) {
    case 503: return "принтер занят (печать/очередь), дождитесь окончания и повторите";
    case 401: return "неверный пароль обновления";
    case 400: return "в запросе нет Content-Length";
    default:  return "";
  }
}

// ESP32 app images always start with 0xE9 (esp_image header magic). A wrong
// file (zip, random binary, an ESP8266 image) is refused on the FIRST byte
// with a clear message, instead of a cryptic Update error after the whole
// multi-MB transfer has already been written to flash.
inline bool otaMagicOk(uint8_t firstByte) { return firstByte == 0xE9; }
