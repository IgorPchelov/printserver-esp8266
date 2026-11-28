/*
 * ParallelPort.h - direct-drive IEEE-1284 compatibility-mode (Centronics)
 * port for any Centronics (IEEE-1284 compatibility mode) parallel printer on ESP32-S3, with full status-line monitoring.
 *
 * Fixes vs. the original printserver-esp8266 code:
 *  1. Proper data setup time before STROBE falling edge and data hold after
 *     STROBE rising edge (original wrote data and pulsed STROBE with zero
 *     setup margin - lost/garbled bytes on faster hosts).
 *  2. After STROBE, waits for BUSY to rise (accept) and optionally for ACK
 *     pulse, with a timeout - instead of only checking BUSY before the byte.
 *  3. SELECTIN kept asserted (LOW) for the whole job; INIT line driven so a
 *     hung printer can be reset. Original never used these control lines.
 *  4. Status lines BUSY / ACK / PE (paper empty) / SELECT (online) / ERROR
 *     are all monitored and mapped to printer state for the UI and IPP.
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#pragma once

#include <Arduino.h>
#include "config.h"

enum class LptStatus : uint8_t {
  UNKNOWN = 0,   // status lines not readable (not wired / boot)
  ONLINE,        // idle, ready to accept data
  BUSY,          // processing previous byte/page
  PAPER_OUT,     // PE asserted
  OFFLINE,       // SELECT deasserted (printer taken offline)
  ERROR_STATE,   // ERROR asserted (jam, cover open, no toner...)
};

// Printable Russian names for the OLED / web UI
const char* lptStatusName(LptStatus s);

class ParallelPort {
public:
  void begin();

  // --- Centronics byte transfer ------------------------------------
  // Blocks until the printer accepts the byte or the budget (ms) expires.
  // Returns false on error. Two distinct refusals:
  //   * out==TIMEOUT - printer busy for the whole budget; the byte was NOT
  //     strobed, the caller should retry THIS byte after servicing the
  //     panel (web/mDNS stay alive during long page-feed busy).
  //   * out==DESELECTED - SELECT held low for a second (printer off/unwired);
  //     retrying is pointless until it comes back.
  enum class WriteResult : uint8_t { OK, TIMEOUT, DESELECTED };
  WriteResult writeByteBudget(uint8_t b, uint32_t budgetMs);
  // Legacy one-shot (blocking full timeout): true = byte went out.
  bool writeByte(uint8_t b, uint32_t timeoutMs);

  // --- Status ------------------------------------------------------
  LptStatus status();                    // debounced current status
  bool isAccepting();                    // BUSY low && SELECT high && !ERROR && !PE
  bool ackSeen();                        // consume "ACK pulse seen" flag
  void resetPrinter();                   // pulse INIT line >= 50 us

  // Which status lines were actually detected at boot (for diagnostics)
  bool lineWired(uint8_t mask) const { return (wiredMask & mask) != 0; }

  // True once the SELECT line has ever been driven high (a real printer
  // asserted it at least once since boot - the self-healing mask in poll()
  // keeps it sticky). Distinguishes "printer connected but deselected"
  // (power off / offline button: pause the job and wait) from "nothing on
  // the port" (fail fast, drop with an honest error - do not stall).
  bool selectEverWired() const;

  // Raw line snapshot (true = active assertion, already polarity-normalized)
  bool rawBusy()   const { return lastBusy; }
  bool rawPe()     const { return lastPe; }
  bool rawSelect() const { return lastSelect; }
  bool rawError()  const { return lastError; }

  void poll();                           // call from loop(): debounce + events

  // callbacks (set by UI layer)
  void (*onStatusChange)(LptStatus newStatus) = nullptr;

private:
  uint8_t wiredMask = 0;
  bool lastBusy = false, lastPe = false, lastSelect = true, lastError = false;
  bool ackFlag = false;
  LptStatus debStatus = LptStatus::UNKNOWN;
  LptStatus rawStatus();
};
