/*
 * ParallelPort.cpp - see ParallelPort.h
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#include "ParallelPort.h"

// DB25->GPIO map for the 8 data lines
static const int DATA_PINS[8] = {
  PIN_LPT_D0, PIN_LPT_D1, PIN_LPT_D2, PIN_LPT_D3,
  PIN_LPT_D4, PIN_LPT_D5, PIN_LPT_D6, PIN_LPT_D7
};

// status line detection masks
#define W_BUSY    0x01
#define W_ACK     0x02
#define W_PE      0x04
#define W_SELECT  0x08
#define W_ERROR   0x10

bool ParallelPort::selectEverWired() const {
  return (wiredMask & W_SELECT) != 0;
}

const char* lptStatusName(LptStatus s) {
  switch (s) {
    case LptStatus::ONLINE:      return "готов";
    case LptStatus::BUSY:        return "занят";
    case LptStatus::PAPER_OUT:   return "нет бумаги";
    case LptStatus::OFFLINE:     return "офлайн";
    case LptStatus::ERROR_STATE: return "ошибка принтера";
    case LptStatus::UNKNOWN:     return "принтер не подключен";
    default:                     return "принтер не подключен";
  }
}

void ParallelPort::begin() {
  for (int i = 0; i < 8; i++) {
    pinMode(DATA_PINS[i], OUTPUT);
    digitalWrite(DATA_PINS[i], LOW);
  }
  pinMode(PIN_LPT_STROBE, OUTPUT);
  digitalWrite(PIN_LPT_STROBE, HIGH);          // idle (inactive, STROBE is active-low)

  pinMode(PIN_LPT_BUSY,   INPUT);
  pinMode(PIN_LPT_ACK,    INPUT);
  pinMode(PIN_LPT_PE,     INPUT);
  pinMode(PIN_LPT_SELECT, INPUT);
  pinMode(PIN_LPT_ERROR,  INPUT);
  // Keep unwired inputs at a defined level: a floating status line slowly
  // drifts and can latch a phantom "ошибка" (ERROR pin reads LOW) hours
  // into idle. The internal pulls are weak (~45k) and lose against the
  // real printer's 5V-to-3.3V divider, so they only pin the floating
  // case. Idle-safe levels per the wiring table: BUSY low, ACK high,
  // PE low, SELECT low, ERROR high.
  pinMode(PIN_LPT_BUSY,   INPUT_PULLDOWN);
  pinMode(PIN_LPT_ACK,    INPUT_PULLUP);
  pinMode(PIN_LPT_PE,     INPUT_PULLDOWN);
  pinMode(PIN_LPT_SELECT, INPUT_PULLDOWN);
  pinMode(PIN_LPT_ERROR,  INPUT_PULLUP);

  pinMode(PIN_LPT_INIT, OUTPUT);
  digitalWrite(PIN_LPT_INIT, HIGH);            // inactive (INIT is active-low)
  pinMode(PIN_LPT_SELECTIN, OUTPUT);
  digitalWrite(PIN_LPT_SELECTIN, LOW);         // asserted: host requests printer online
  pinMode(PIN_LPT_AUTOFD, OUTPUT);
  digitalWrite(PIN_LPT_AUTOFD, HIGH);          // inactive (no forced LF)

  // Detect which status lines are physically wired: a line that never changes
  // from its power-up floating level during 30 ms of sampling is considered
  // floating. Floating inputs on ESP32-S3 usually read LOW (with GPIO47/48
  // having internal pull-ups), so we treat a stuck line as "not wired" only
  // if it also does not react to the printer being idle.
  uint8_t seen = 0;
  for (int i = 0; i < 30; i++) {
    if (digitalRead(PIN_LPT_BUSY)   == HIGH) seen |= W_BUSY;   // BUSY high while idle is odd but record it
    if (digitalRead(PIN_LPT_ACK)    == LOW)  seen |= W_ACK;
    if (digitalRead(PIN_LPT_PE)     == HIGH) seen |= W_PE;
    if (digitalRead(PIN_LPT_SELECT) == HIGH) seen |= W_SELECT;
    if (digitalRead(PIN_LPT_ERROR)  == LOW)  seen |= W_ERROR;
    delayMicroseconds(1000);
  }
  wiredMask = seen;

  // Pull-up/down guidance: with divider resistors installed a missing printer
  // must read: BUSY=LOW(=not busy), ACK=HIGH, PE=LOW, SELECT=LOW, ERROR=HIGH.
  // We bias the perception: if SELECT is not high at boot -> printer offline or absent.
  debStatus = rawStatus();
}

LptStatus ParallelPort::rawStatus() {
  // Only trust a status line that was seen wired at boot: a line the LPT
  // connector left floating (board on a desk with no printer/cable) must
  // not fake a printer into existence via a stray high level. A printer
  // that is really connected drives all five lines, so "SELECT wired AND
  // high" is the honest test of a present printer; everything unwired
  // reads as not asserted.
  bool err  = lineWired(W_ERROR)  && (digitalRead(PIN_LPT_ERROR)  == LOW);
  bool pe   = lineWired(W_PE)     && (digitalRead(PIN_LPT_PE)     == HIGH);
  bool sel  = lineWired(W_SELECT) && (digitalRead(PIN_LPT_SELECT) == HIGH);
  bool busy = lineWired(W_BUSY)   && (digitalRead(PIN_LPT_BUSY)   == HIGH);
  lastBusy = busy; lastPe = pe; lastSelect = sel; lastError = err;
  if (err)            return LptStatus::ERROR_STATE;
  if (pe)             return LptStatus::PAPER_OUT;
  if (!sel)           return LptStatus::OFFLINE;
  if (busy)           return LptStatus::BUSY;
  return LptStatus::ONLINE;
}

bool ParallelPort::isAccepting() {
  return lineWired(W_SELECT) && digitalRead(PIN_LPT_SELECT) == HIGH
      && digitalRead(PIN_LPT_BUSY)   == LOW
      && digitalRead(PIN_LPT_ERROR)  == HIGH
      && digitalRead(PIN_LPT_PE)     == LOW;
}

bool ParallelPort::ackSeen() {
  bool f = ackFlag;
  ackFlag = false;
  return f;
}

void ParallelPort::resetPrinter() {
  digitalWrite(PIN_LPT_INIT, LOW);
  delayMicroseconds(100);            // printer needs >= 50 us (IEEE 1284)
  digitalWrite(PIN_LPT_INIT, HIGH);
  delay(10);                          // printer re-initialises its I/F
}

bool ParallelPort::writeByte(uint8_t b, uint32_t timeoutMs) {
  uint32_t start = millis();

  // wait for ready (BUSY low, not error/paper-out/offline)
  // selLow timing: SELECT low for a sustained second means the printer is
  // deselected (powered off / offline button / nothing wired to the port).
  // It will not take this byte - waiting the full BUSY timeout (120 s,
  // meant for page-feed busy) froze the whole box, web UI included, for
  // two minutes per job. Fail fast instead; the caller drops the job with
  // an honest "нет ответа LPT". A wired printer that merely blips SELECT
  // (mode negotiation) never trips a full second.
  bool selLow = false;
  uint32_t selLowSince = 0;
  while (!isAccepting()) {
    if (millis() - start > timeoutMs) return false;
    if (digitalRead(PIN_LPT_ERROR) == LOW || digitalRead(PIN_LPT_PE) == HIGH) return false;
    if (digitalRead(PIN_LPT_SELECT) == HIGH) {
      selLow = false;
    } else if (!selLow) {
      selLow = true; selLowSince = millis();
    } else if (millis() - selLowSince >= 1000) {
      return false;                      // deselected: never going to accept
    }
    delayMicroseconds(20);
  }

  // put data on the bus, respect setup time
  for (int i = 0; i < 8; i++) digitalWrite(DATA_PINS[i], (b >> i) & 1);
  delayMicroseconds(T_DS_SETUP_US);

  // STROBE low pulse
  digitalWrite(PIN_LPT_STROBE, LOW);
  delayMicroseconds(T_STROBE_LOW_US);
  digitalWrite(PIN_LPT_STROBE, HIGH);

  // The printer raises BUSY within ~0.5 us of accepting; it clears BUSY and
  // pulses ACK when the byte is stored. Wait for BUSY to *fall back* (or
  // ACK pulse) so the next writeByte cannot overrun the printer's buffer.
  uint32_t tAccept = millis();
  bool sawBusy = false;
  while (true) {
    bool busy = digitalRead(PIN_LPT_BUSY) == HIGH;
    if (busy) sawBusy = true;
    if (digitalRead(PIN_LPT_ACK) == LOW) ackFlag = true;   // ACK is a low pulse; sample-level detect
    if (sawBusy && !busy) break;                            // BUSY rose and fell: accepted
    if (!sawBusy && digitalRead(PIN_LPT_ACK) == LOW) break; // very fast printer: ACK already
    if (millis() - tAccept > 10) break;                     // printers pulse BUSY per byte; 10 ms is plenty
    if (millis() - start > timeoutMs) return false;
  }
  return true;
}

void ParallelPort::poll() {
  // Self-healing wiring mask: a line asserting its active level IS driven
  // by a real printer, so mark it wired from now on. A floating line,
  // biased by the internal pulls, can never assert - so "no line has ever
  // asserted" honestly means "no printer on the port". This also recovers
  // when the printer is powered on AFTER the ESP (boot sampling misses it).
  if (digitalRead(PIN_LPT_SELECT) == HIGH) wiredMask |= W_SELECT;
  if (digitalRead(PIN_LPT_BUSY)   == HIGH) wiredMask |= W_BUSY;
  if (digitalRead(PIN_LPT_PE)     == HIGH) wiredMask |= W_PE;
  if (digitalRead(PIN_LPT_ERROR)  == LOW)  wiredMask |= W_ERROR;
  if (digitalRead(PIN_LPT_ACK)    == LOW)  wiredMask |= W_ACK;
  LptStatus s = rawStatus();
  if (s != debStatus) {
    debStatus = s;
    if (onStatusChange) onStatusChange(s);
  }
}

LptStatus ParallelPort::status() {
  return debStatus;
}
