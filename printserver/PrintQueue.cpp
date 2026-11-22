/* PrintQueue.cpp - see PrintQueue.h. Part of the GPLv3 port of printserver-esp8266. */
#include "PrintQueue.h"
#include "config.h"

String PrintQueue::jobPath(uint32_t id, bool finalized) const {
  String p = QUEUE_FILES_PREFIX;
  p += id;
  if (finalized) p += ".ok";
  else p += ".tmp";
  return p;
}

void PrintQueue::begin() {
  LittleFS.mkdir("/q");
  cleanupStale();
  rescan();
}

void PrintQueue::cleanupStale() {
  File dir = LittleFS.open(QUEUE_FILES_PREFIX);
  if (!dir || !dir.isDirectory()) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (!n.startsWith("/") ) n = String(QUEUE_FILES_PREFIX) + n;
    if (n.endsWith(".tmp")) {
      LittleFS.remove(n);   // job never finalized: power loss mid-transfer
    } else {
      // Track ids of BOTH finalized flavours (.ok raw, .okw PWG). The old
      // code parsed only ".ok": a queue holding only Android PWG jobs reset
      // nextId to 1 on every reboot, and the next spooled job silently
      // OVERWROTE the oldest queued .okw file (job loss after each reboot).
      bool pwg = n.endsWith(".okw");
      if (pwg || n.endsWith(".ok")) {
        uint32_t id = (uint32_t) n.substring(n.lastIndexOf('/') + 1, n.length() - (pwg ? 4 : 3)).toInt();
        if (id >= nextId) nextId = id + 1;
      }
    }
  }
  dir.close();
}

void PrintQueue::rescan() {
  pending = 0;
  File dir = LittleFS.open(QUEUE_FILES_PREFIX);
  if (!dir || !dir.isDirectory()) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (!n.startsWith("/")) n = String(QUEUE_FILES_PREFIX) + n;
    if (n.endsWith(".ok") || n.endsWith(".okw")) pending++;   // both flavours
  }
  dir.close();
}

bool PrintQueue::canStore() {
  return (size_t)LittleFS.totalBytes() - (size_t)LittleFS.usedBytes() > QUEUE_FREE_MARGIN;
}

bool PrintQueue::startJob(int clientId) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS) return false;
  if (writers[clientId]) writers[clientId].close();
  String p = jobPath(nextId, false);
  writers[clientId] = LittleFS.open(p, "w");
  if (!writers[clientId]) return false;
  writerIds[clientId] = nextId;
  writerBytes[clientId] = 0;
  nextId++;
  return true;
}

void PrintQueue::markFormat(int clientId, bool pwgRaster) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS) return;
  writerIsPwg[clientId] = pwgRaster;   // honored at endJob() via file extension
}

void PrintQueue::writeByte(int clientId, uint8_t b) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS || !writers[clientId]) return;
  if (writers[clientId].write(b) != 1) writerFailed[clientId] = true;  // flash full mid-job
  if (++writerBytes[clientId] % 4096 == 0) writers[clientId].flush();
}

void PrintQueue::write(int clientId, const uint8_t* buf, size_t len) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS || !writers[clientId] || !buf || !len) return;
  size_t w = writers[clientId].write(buf, len);
  if (w < len) writerFailed[clientId] = true;   // truncated write: flash full
  writerBytes[clientId] += len;
  if (writerBytes[clientId] % 4096 < len) writers[clientId].flush();  // ~every 4 KB
}

// Returns true only if the job was finalized into the queue (renamed to
// .ok/.okw and counted in pending()). A job whose writes failed anywhere
// (flash filled mid-transfer) is DISCARDED: finalizing it would print half
// a page while telling the client "OK" (IPP handlers use the return value
// to answer honestly with 0x0503 instead).
bool PrintQueue::endJob(int clientId, bool cancel) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS || !writers[clientId]) return false;
  uint32_t id = writerIds[clientId];
  bool empty = writerBytes[clientId] == 0;
  bool failed = writerFailed[clientId];
  String tmp = jobPath(id, false);
  writers[clientId].close();
  writers[clientId] = File();
  bool finalized = false;
  if (cancel || empty || failed) {
    LittleFS.remove(tmp);
  } else {
    // ".ok" = raw passthrough bytes; ".okw" = PWG raster (needs conversion)
    LittleFS.rename(tmp, jobPath(id, true) + (writerIsPwg[clientId] ? "w" : ""));
    pending++;
    doneCount++;
    finalized = true;
  }
  writerIsPwg[clientId] = false;
  writerFailed[clientId] = false;
  return finalized;
}

bool PrintQueue::hasData() {
  if (reader && reader.available() > 0) return true;
  if (reader) {
    reader.close();
    LittleFS.remove(jobPath(curId, true) + (curIsPwg ? "w" : ""));
    pending--;
  }
  // find lowest finalized id >= 1
  uint32_t best = 0;
  File dir = LittleFS.open(QUEUE_FILES_PREFIX);
  if (dir && dir.isDirectory()) {
    File f;
    while ((f = dir.openNextFile())) {
      String n = f.name();
      f.close();
      if (!n.startsWith("/")) n = String(QUEUE_FILES_PREFIX) + n;
      bool pwg = n.endsWith(".okw");
      if (!pwg && !n.endsWith(".ok")) continue;
      uint32_t id = (uint32_t) n.substring(n.lastIndexOf('/') + 1, n.length() - (pwg ? 4 : 3)).toInt();
      if (id && (best == 0 || id < best)) { best = id; bestIsPwg = pwg; }
    }
    dir.close();
  }
  if (!best) return false;
  curId = best;
  curIsPwg = bestIsPwg;
  String p = jobPath(best, true) + (curIsPwg ? "w" : "");
  reader = LittleFS.open(p, "r");
  if (!reader) {
    // Listed but unopenable (littlefs corruption): remove it instead of
    // picking the same lowest id on every hasData() call forever - a hot
    // failed-open loop that starves loop().
    LittleFS.remove(p);
    if (pending > 0) pending--;
    return false;
  }
  curSize = reader.size();
  return reader && reader.available() > 0;
}

int PrintQueue::readByte() {
  if (!reader || !reader.available()) return -1;
  return reader.read();
}

void PrintQueue::dropCurrent() {
  if (reader) {
    reader.close();
    LittleFS.remove(jobPath(curId, true) + (curIsPwg ? "w" : ""));
    if (pending > 0) pending--;
  }
}

size_t PrintQueue::usedBytes() const {
  return (size_t)LittleFS.usedBytes();
}
