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
#define FIRMWARE_NAME    "ESP Print Server"
#define FIRMWARE_VERSION "2.1.0"

// ---------------------------------------------------------------- Hostname / names (generic, model-agnostic)
#define HOST_NAME_PREFIX  "ESPPrint"   // hostname/mDNS prefix; full: ESPPrint-XXXX.local
#define PRINTER_NAME      "ESP LPT PrintServer" // advertised mDNS instance / web panel title
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

// ---------------------------------------------------------------- Print queue (LittleFS)
#define QUEUE_FILES_PREFIX "/q/"             // directory for spooled jobs
#define QUEUE_MAX_JOBS     32                // max jobs kept on flash
#define QUEUE_FREE_MARGIN  (256 * 1024)      // stop accepting below this free space

// ---------------------------------------------------------------- Raster pipeline
#define DEFAULT_DPI        300
// 8.5in (US Letter, the widest common paper - WIDER than A4's 8.27in):
// buffers sized to 2400 used to reject a full-width A4 page (2480 px @300dpi)
// and would have cut a Letter page to 8.0in. lineBuf/pclRow live in PSRAM-class
// statics: 8.5in needs 2550 px = 2550 B input + 320 B output, still tiny.
#define MAX_RASTER_WIDTH   (DEFAULT_DPI * 17 / 2)   // 8.5in at 300dpi = 2550
#define PSRAM_CHUNK        (1024 * 1024)

// ---------------------------------------------------------------- Misc
#define STATUS_POLL_MS     250    // LPT status lines polling period
#define OLED_UPDATE_MS     250    // OLED refresh period
#define OLED_ERROR_SHOW_MS 10000  // transient error banner (line 2) lifetime;
                                  // line 1 (network) must not stay hidden long
#define REBOOT_ON_ERROR_DELAY_MS 30000
