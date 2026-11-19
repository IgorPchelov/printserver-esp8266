/*
 * PrintQueue.h - spooling of print jobs to LittleFS with sequential job ids.
 *
 * Fixes vs original printserver-esp8266:
 *  - original used `byte head/tail` counters and file names without a
 *    directory; after 255 jobs the counter wrapped and old files were
 *    mistaken for new ones (jobs re-printed or lost). Here: monotonically
 *    increasing 32-bit id + /q/ dir + cleanup of stale files at boot.
 *  - original stored head/tail in files keyed by printer id and trusted
 *    SPIFFS free space counters; here we re-scan the directory at boot.
 *  - multi-file streams (one open File per client) kept, but with flush
 *    every 4 KB so a power cut loses at most the current job.
 *
 * This file is part of a port of gianluca-nitti/printserver-esp8266 (GPLv3).
 */
#pragma once

#include <Arduino.h>
#include <LittleFS.h>

#define MAX_CONCURRENT_JOBS 4

class PrintQueue {
public:
  void begin();
  bool startJob(int clientId);               // open a spool file for a new incoming job
  void markFormat(int clientId, bool pwgRaster); // remember doc format for this job
  void write(int clientId, const uint8_t* buf, size_t len);  // buffered write
  void writeByte(int clientId, uint8_t b);
  void endJob(int clientId, bool cancel);    // finalize or discard
  bool canStore();                           // flash space available
  bool hasData();                            // oldest complete job ready to print
  int  readByte();                           // -1 = EOF
  size_t currentJobSize() const { return curSize; }
  uint32_t currentJobId()  const { return curId; }
  bool currentJobIsPwg()   const { return curIsPwg; }   // true = needs PWG->PCL conversion
  uint32_t jobsDone()      const { return doneCount; }
  void dropCurrent();                         // delete job being read (error path)
  size_t usedBytes() const;
  int pendingCount() const { return pending; }
private:
  File writers[MAX_CONCURRENT_JOBS];
  uint32_t writerIds[MAX_CONCURRENT_JOBS] = {0};
  size_t   writerBytes[MAX_CONCURRENT_JOBS] = {0};
  bool     writerIsPwg[MAX_CONCURRENT_JOBS] = {false};
  File reader;
  uint32_t curId = 0;
  size_t curSize = 0;
  bool curIsPwg = false;
  bool bestIsPwg = false;
  uint32_t nextId = 1;
  uint32_t doneCount = 0;
  int pending = 0;
  void rescan();            // rebuild pending count from directory listing
  void cleanupStale();      // remove orphaned non-finalized files
  String jobPath(uint32_t id, bool finalized) const;
};
