/*
 * config.h - central pin & feature configuration for the ESP32-S3 LPT print server.
 *
 * Hardware: ESP32-S3-DevKitC-1 N16R8 (16MB flash, 8MB PSRAM, octal PSRAM).
 * Printer:  ANY Centronics/IEEE-1284 compatibility-mode parallel printer
 *           (PCL-compatible laser printers work best). No model-specific
 *           logic anywhere in the firmware.
 * Display:  OLED 0.91" 128x32 I2C (SSD1306).
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------- Version
// CONVENTION (user request): every change to the firmware bumps this - the
// web panel (index page first status row, /wifi footer, /update page) shows
// it, so anyone can tell which build a flashed box is running. Minor for
// notable features, patch for small tweaks/fixes.
#define FIRMWARE_NAME    "ESP Print Server"
#define FIRMWARE_VERSION "2.4.0"     // 2.4.0: Android-print fix round -
                                    //      IPP body (header AND attribute
                                    //      walk AND document) now read
                                    //      through one BodyStream so chunked
                                    //      Transfer-Encoding is decoded
                                    //      everywhere (Android/curl jobs
                                    //      used to mis-parse and retry);
                                    //      Create-Job/Send-Document/Get-Jobs/
                                    //      Get-Job-Attributes/Cancel-Job
                                    //      supported (the two-step flavour
                                    //      Android actually uses), 0x0506 on
                                    //      a full spool; raw 9100 lazy spool -
                                    //      probe connections no longer open
                                    //      .tmp files / claim "Загрузка
                                    //      данных" on the OLED; per-service
                                    //      mDNS names "(IPP)"/"(socket)";
                                    //      every IPP answer logs its status
                                    //      code; empty request lines logged
                                    //      as port probes; bin filename
                                    //      carries the version;
                                    // 2.3.0: reboot policy - begin() purges the
                                    //      whole queue (no stale jobs after power-up),
                                    //      [ipp]/[raw]/[q] serial logs for connections
                                    //      and verdicts (a job that never arrived is
                                    //      now visible in the monitor);
                                    // 2.2.3: panel flash row renamed to
                                    //      "Занято во флэше (spool)" - it shows
                                    //      whole-partition usage, not queue payload;
                                    // 2.2.2: IPP review round (panel alive during
                                    //      IPP transfers, real job-id/job-uri);
                                    // 2.2.1: version shown on the web panel;
                                    // 2.2.0: web OTA update + endpoint column layout

// ---------------------------------------------------------------- Hostname / names (generic, model-agnostic)
#define HOST_NAME_PREFIX  "ESPPrint"   // hostname/mDNS prefix; full: ESPPrint-XXXX.local
// Two advertised endpoints, two DISTINGUISHABLE names (user request, 2.4.0):
// Android lists both _ipp._tcp and _pdl-datastream._tcp as candidate
// printers, and with identical instance names the list shows two entries
// nobody can tell apart. Name shape follows the user's wording
// ("ESP32 WiFi_to_LPT") with the transport the entry actually speaks.
#define PRINTER_NAME        "ESP32 WiFi_to_LPT (IPP)"       // _ipp._tcp instance / IPP printer-name
#define PRINTER_NAME_SOCKET "ESP32 WiFi_to_LPT (socket)"    // _pdl-datastream._tcp instance
#define PRINTER_INFO        "ESP32 WiFi_to_LPT print server"// IPP printer-info (shared description)
#define AP_SSID_PREFIX    "ESPPrintServer-"    // + chip id suffix

// ---------------------------------------------------------------- LPT pins (Centronics / DB25)
// Data lines D0..D7 (DB25 pins 2..9). ESP32-S3 GPIO are 3.3V only.
// Most parallel printers have 5V TTL inputs: drive D0..D7 / STROBE directly
// from ESP32-S3 (3.3V CMOS HIGH is read as TTL HIGH), and level-shift the
// printer's 5V outputs (BUSY/ACK/PE/SELECT/ERROR) DOWN to 3.3V with a
// resistor divider (see README wiring table).
//
// Pin map chosen to avoid: strapping pins 0/3/45/46, flash SPI 26-32,
// PSRAM octal 33-37, USB pins 19/20, I2C for OLED (8/9).
#define PIN_LPT_D0        4
#define PIN_LPT_D1        5
#define PIN_LPT_D2        6
#define PIN_LPT_D3        7
#define PIN_LPT_D4        10
#define PIN_LPT_D5        11
#define PIN_LPT_D6        12
#define PIN_LPT_D7        13
#define PIN_LPT_STROBE    14   // DB25 pin 1, inverted: LOW = data valid
#define PIN_LPT_ACK       15   // DB25 pin 10, input, LOW pulse = byte accepted
#define PIN_LPT_PE        16   // DB25 pin 12, input, HIGH = paper empty
#define PIN_LPT_SELECT    17   // DB25 pin 13, input, HIGH = printer online
#define PIN_LPT_ERROR     18   // DB25 pin 15, input, LOW = printer error
#define PIN_LPT_BUSY      21   // DB25 pin 11, input, HIGH = printer busy
// Control outputs (optional wiring):
#define PIN_LPT_INIT      38   // DB25 pin 16, output, LOW pulse >=50us resets printer
#define PIN_LPT_SELECTIN  39   // DB25 pin 17, output, must be LOW for printer to accept data
#define PIN_LPT_AUTOFD    40   // DB25 pin 14, output, LOW = auto linefeed (unused, tied high)

// Detect optionality: if a status pin reads floating we cannot trust it.
// We sample at boot and remember which lines are actually connected.
#define LPT_STATUS_LINES_OPTIONAL 1

// ---------------------------------------------------------------- OLED (I2C)
#define PIN_OLED_SDA      8
#define PIN_OLED_SCL      9
#define OLED_ADDR         0x3C
#define OLED_W            128
#define OLED_H            32

// ---------------------------------------------------------------- Button
// BOOT button of the ESP32-S3-DevKitC-1 (wired to GPIO0). Hold it while
// powering on - or any time later - to force the configuration AP.
#define PIN_BTN           0   // BOOT button on DevKitC-1 (GPIO0, has external pull-up)
#define BTN_HOLD_MS       3000UL  // hold this long to force AP mode

// ---------------------------------------------------------------- LEDs
// Status indication that does not depend on the OLED (user request: the
// box must stay readable even with a dead/unfitted display).
#define PIN_LED_RGB       48  // onboard WS2812 RGB LED of the DevKitC-1: GPIO48
                              // on N16R8 boards (octal PSRAM, the boards this
                              // project targets). On quad-PSRAM revisions the
                              // RGB sits on GPIO38 - unusable here, that pin
                              // drives LPT INIT. 255 = no WS2812 fitted.
#define PIN_LED_WIFI      2   // optional simple LED (anode to GPIO2 via ~470R,
                              // cathode to GND), mirrors the RGB patterns as
                              // on/off blinks; 255 = not wired. Kept for the
                              // original wiring docs of this project.

// ---------------------------------------------------------------- Timings (microseconds) - IEEE 1284 compatibility mode
#define T_DS_SETUP_US     1    // data setup before STROBE falling (>=0.5 us)
#define T_STROBE_LOW_US   2    // STROBE low pulse width (>=0.5 us, typ 1..5)
#define T_STROBE_HI_WAIT  1    // data hold after STROBE rising (>=0.5 us)
#define LPT_BUSY_TIMEOUT_MS 120000UL // hard cap while printer is busy (page feed)

// ---------------------------------------------------------------- Network
#define HTTP_PORT         80
#define IPP_PORT          631
#define RAW_PORT          9100   // JetDirect / AppSocket
#define RAW_IDLE_TIMEOUT_MS 300000UL // raw client silent this long = dead (half-open
                                     // TCP / crashed sender): drop its spool job and
                                     // free the slot. Healthy streams never pause 5 min.
#define LPD_PORT          515    // RFC 1179 Line Printer Daemon
#define AP_PASSWORD       ""     // open AP by default; set via web panel
#define WIFI_STA_TIMEOUT_MS 20000UL
#define SCAN_TIMEOUT_MS     15000UL  // /scan watchdog: a stuck async scan must not
                                     // leave the page spinning on "Поиск сетей..."
#define AP_RETRY_MS         120000UL // AP mode: retry the stored network this often
                                     // (self-heal when the router comes back)
#define AP_RETRY_TIMEOUT_MS 10000UL  // give each background retry this long before
                                     // returning the radio to clean AP duty
#define MDNS_TTL          120

// ---------------------------------------------------------------- OTA web update (/update page)
// POST /update streams the uploaded .bin straight into the idle OTA slot
// (app0/app1, 4 MB each - see partitions-16mb.csv) and reboots. The FIRST
// flash of a new board still has to go over USB: OTA only updates a box
// that already runs this firmware.
#define OTA_PASS          ""     // optional password for the /update POST. Empty =
                                 // open update (same trust model as the /wifi page:
                                 // trusted LAN). Set a non-empty string and the page
                                 // asks for it (sent as the X-OTA-Key header).
#define OTA_XFER_TIMEOUT_MS 20000UL // upload stall: no byte for this long = dead
                                    // client, abort the flash write cleanly. Bytes
                                    // flowing reset it, so a slow WiFi link is fine.

// ---------------------------------------------------------------- Print queue (LittleFS)
#define QUEUE_FILES_PREFIX "/q/"             // directory for spooled jobs
#define QUEUE_MAX_JOBS     32                // max jobs kept on flash
#define QUEUE_FREE_MARGIN  (256 * 1024)      // stop accepting below this free space

// ---------------------------------------------------------------- Raster pipeline
#define DEFAULT_DPI        300
// 8.5in (US Letter, the widest common paper - WIDER than A4's 8.27in):
// buffers sized to 2400 used to reject a full-width A4 page (2480 px @300dpi)
// and would have cut a Letter page to 8.0in. The decoder sizes its buffers
// from this: input lineBuf = 2550*3 B (srgb_8 worst case, 3 B/px - what IPP
// advertises), output pclRow = (2550+7)/8 = 319 B (1 bit/px, rounded UP -
// truncating to 318 let a full-width Letter row write 1 byte out of bounds).
// hdrW above MAX_RASTER_WIDTH (e.g. 600dpi pages) is refused with
// "line too wide": the spool only advertises 300 dpi anyway.
#define MAX_RASTER_WIDTH   (DEFAULT_DPI * 17 / 2)   // 8.5in at 300dpi = 2550
#define PSRAM_CHUNK        (1024 * 1024)

// ---------------------------------------------------------------- Misc
#define STATUS_POLL_MS     250    // LPT status lines polling period
#define OLED_UPDATE_MS     250    // OLED refresh period
#define OLED_ERROR_SHOW_MS 10000  // transient error banner (line 2) lifetime;
                                  // line 1 (network) must not stay hidden long
#define REBOOT_ON_ERROR_DELAY_MS 30000
