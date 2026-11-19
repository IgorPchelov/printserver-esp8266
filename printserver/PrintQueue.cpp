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
    } else if (n.endsWith(".ok")) {
      uint32_t id = (uint32_t) n.substring(n.lastIndexOf('/') + 1, n.length() - 3).toInt();
      if (id >= nextId) nextId = id + 1;
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
    if (n.endsWith(".ok")) pending++;
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
  writers[clientId].write(b);
  if (++writerBytes[clientId] % 4096 == 0) writers[clientId].flush();
}

void PrintQueue::write(int clientId, const uint8_t* buf, size_t len) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS || !writers[clientId] || !buf || !len) return;
  writers[clientId].write(buf, len);
  writerBytes[clientId] += len;
  if (writerBytes[clientId] % 4096 < len) writers[clientId].flush();  // ~every 4 KB
}

void PrintQueue::endJob(int clientId, bool cancel) {
  if (clientId < 0 || clientId >= MAX_CONCURRENT_JOBS || !writers[clientId]) return;
  uint32_t id = writerIds[clientId];
  bool empty = writerBytes[clientId] == 0;
  String tmp = jobPath(id, false);
  writers[clientId].close();
  writers[clientId] = File();
  if (cancel || empty) {
    LittleFS.remove(tmp);
  } else {
    // ".ok" = raw passthrough bytes; ".okw" = PWG raster (needs conversion)
    LittleFS.rename(tmp, jobPath(id, true) + (writerIsPwg[clientId] ? "w" : ""));
    pending++;
    doneCount++;
    writerIsPwg[clientId] = false;
  }
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
  reader = LittleFS.open(jobPath(best, true) + (curIsPwg ? "w" : ""), "r");
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
