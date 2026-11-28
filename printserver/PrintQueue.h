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
  // Finalize or discard. Returns true only if the job was actually stored:
  // cancelled, empty, or write-failed (flash filled mid-transfer) jobs are
  // removed - a truncated file must never print as a half page.
  bool endJob(int clientId, bool cancel);
  bool canStore();                           // flash space available
  // Spool id of the job being written on this slot (0 = no open writer).
  // The IPP Print-Job response quotes it as the REAL job-id/job-uri - a
  // hardcoded 1 referenced a phantom /job/1 that could collide with a
  // genuinely spooled job #1 later.
  uint32_t jobId(int clientId) const;
  bool hasData();                            // oldest complete job ready to print
  int  readByte();                           // -1 = EOF
  // Un-read the LAST byte readByte() returned (2.4.2): when the LPT write
  // is only deferred (printer busy, not dead), the print engine parks the
  // byte back instead of losing it, and continues in a later loop()
  // pass after the panel was serviced. One slot deep - exactly what a
  // deferred writeByteBudget() needs; a "dequeued and thrown away" byte
  // would reprint garbage.
  void pushback();                           // repeat the last read byte
  size_t currentJobSize() const { return curSize; }
  uint32_t currentJobId()  const { return curId; }
  bool currentJobIsPwg()   const { return curIsPwg; }   // true = needs PWG->PCL conversion
  uint32_t jobsDone()      const { return doneCount; }
  void dropCurrent();                         // delete job being read (error path)
  // Cancel-Job support: remove a WAITING job by spool id. Mid-print jobs
  // are refused (LPT has no pause/resume); unknown ids return false.
  bool removeJobById(uint32_t id);
  // Job-state registry for Get-Job-Attributes (2.4.1): Android polls job
  // state right after submitting; "unknown-job" made the notification flip
  // to failed/unresponsive while the job was actually queued. States:
  // 3 = pending (waiting in the spool), 5 = processing (engine feeds it),
  // 9 = completed (printed; kept as history, no file behind it anymore).
  static const uint32_t JOB_HISTORY = 8;         // remember the last N jobs
  uint8_t jobStateOf(uint32_t id) const;        // 0 = unknown
  void markJobState(uint32_t id, uint8_t state); // registry upkeep
  size_t usedBytes() const;
  int pendingCount() const { return pending; }
  // How many queued jobs (both flavours) the last begin() removed from /q
  // (user-requested reboot policy). Diagnostic: 0 is not worth a log line.
  int purgedAtBoot() const { return purgedBoot; }
private:
  File writers[MAX_CONCURRENT_JOBS];
  uint32_t writerIds[MAX_CONCURRENT_JOBS] = {0};
  size_t   writerBytes[MAX_CONCURRENT_JOBS] = {0};
  bool     writerIsPwg[MAX_CONCURRENT_JOBS] = {false};
  bool     writerFailed[MAX_CONCURRENT_JOBS] = {false}; // a write didn't fit
  File reader;
  uint8_t pbByte = 0;        // pushback() slot: byte to re-read when armed
  bool pbValid = false;      // pushback armed (set by pushback, eaten by read)
  bool pbFresh = false;      // last readByte() returned a real byte
  uint32_t curId = 0;
  size_t curSize = 0;
  bool curIsPwg = false;
  bool bestIsPwg = false;
  uint32_t nextId = 1;
  uint32_t doneCount = 0;
  int pending = 0;
  int purgedBoot = 0;        // jobs begin() removed this boot (see purgedAtBoot())
  // rolling job-state registry (jobStateOf/markJobState), oldest replaced
  uint32_t histIds[JOB_HISTORY] = {0};
  uint8_t  histStates[JOB_HISTORY] = {0};
  uint32_t histNext = 0;     // ring index
  void rescan();            // rebuild pending count from directory listing
  void cleanupStale();      // remove orphaned non-finalized files
  void purgeAll();          // reboot policy: drop every job from /q
  String jobPath(uint32_t id, bool finalized) const;
};
