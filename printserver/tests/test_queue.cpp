// Host unit test for PrintQueue.cpp running on a REAL littlefs in RAM
// (see host_littlefs/). First time the spool logic itself - not just the
// PWG decoder - is executed outside the firmware.
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "../PrintQueue.h"
#include "../config.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  else { printf("ok:   %s\n", msg); } } while (0)

// spool one job through the queue API and return its content
static void spool(PrintQueue& q, int clientId, const char* data, bool pwg) {
  CHECK(q.startJob(clientId), "startJob opens a writer");
  q.write(clientId, (const uint8_t*)data, strlen(data));
  q.markFormat(clientId, pwg);
  CHECK(q.endJob(clientId, false), "endJob finalizes a complete job");
}

// drain the reader fully; returns everything read
static std::string drain(PrintQueue& q) {
  std::string out;
  while (q.hasData()) {
    int b = q.readByte();
    if (b < 0) break;
    out.push_back((char)b);
    if ((int)out.size() > 100000) break;   // safety
  }
  return out;
}

// read ONLY the current job (stops at the job boundary - hasData() will
// have advanced the reader to the next job)
static std::string readJob(PrintQueue& q) {
  std::string out;
  uint32_t id = q.currentJobId();
  while (q.hasData() && q.currentJobId() == id) {
    int b = q.readByte();
    if (b < 0) break;
    out.push_back((char)b);
  }
  return out;
}

int main() {
  // ---------- group 1: basic spool/print round-trip ----------
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    CHECK(!q.hasData(), "empty queue: hasData false");
    CHECK(q.pendingCount() == 0, "empty queue: pending 0");
    spool(q, 0, "HelloPCL", false);
    CHECK(q.pendingCount() == 1, "one job spooled: pending 1");
    CHECK(q.hasData(), "job is ready to print");
    CHECK(q.currentJobId() == 1, "first job id is 1");
    CHECK(!q.currentJobIsPwg(), "raw job: currentJobIsPwg false");
    std::string out = drain(q);
    CHECK(out == "HelloPCL", "round-trip content matches");
    CHECK(!q.hasData(), "queue empty after full read");
    CHECK(q.pendingCount() == 0, "pending back to 0 after full read");
  }

  // ---------- group 2: the JOB BOUNDARY precondition (engine bug A) ------
  // hasData() must switch currentJobId()/currentJobIsPwg() to the NEXT job
  // as soon as the current one is exhausted - WITH bytes still unread. The
  // print engine relies on this to re-arm the PWG decoder at the boundary
  // (printserver.ino: engineSwitchJob). Pin the contract here.
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    spool(q, 0, "AAA", false);       // job 1: raw
    spool(q, 1, "PWG!", true);       // job 2: pwg raster
    CHECK(q.hasData() && q.currentJobId() == 1 && !q.currentJobIsPwg(),
          "reader starts on job 1 (raw)");
    std::string out;
    for (int i = 0; i < 3; i++) {
      // job 1 has exactly 3 bytes: after the last one is consumed the next
      // hasData() call must already present job 2
      int b = q.readByte();
      out.push_back((char)b);
    }
    CHECK(out == "AAA", "job 1 content intact");
    CHECK(q.hasData() && q.currentJobId() == 2 && q.currentJobIsPwg(),
          "job boundary: id + isPwg switch to job 2 while it has data");
    out.clear();
    while (q.hasData()) { int b = q.readByte(); if (b < 0) break; out.push_back((char)b); }
    CHECK(out == "PWG!", "job 2 content intact");
    CHECK(!q.hasData(), "both jobs drained");
  }

  // ---------- group 3: reboot policy - begin() purges the whole queue ------
  // User-requested contract: every boot starts with an EMPTY queue (a
  // year-old spooled file must not flush to a newly attached printer).
  // Purged ids still advance nextId so a fresh spool never reuses a name
  // a stale client reference (IPP job-uri quoted before the reboot) could
  // point at. Replaces the old "survive reboot with ids" group.
  lfsGlueFormat();
  {
    PrintQueue q1;
    q1.begin();
    spool(q1, 0, "one", true);    // .okw (Android PWG flavour)
    spool(q1, 1, "two", false);   // .ok  (raw flavour)
    {   // plus a stray .tmp as a power-loss leftover
      File f = LittleFS.open("/q/9.tmp", "w");
      CHECK((bool)f, "stray .tmp created");
      f.write((const uint8_t*)"garbage", 7);
      f.close();
    }
    CHECK(q1.pendingCount() == 2, "two jobs queued before reboot");
    // "reboot": a fresh queue instance over the same (mounted) FS
    PrintQueue q2;
    q2.begin();
    CHECK(!q2.hasData(), "after reboot: queue is empty (policy)");
    CHECK(q2.pendingCount() == 0, "after reboot: pending is 0");
    CHECK(q2.purgedAtBoot() == 3, "purgedAtBoot counts .ok + .okw + .tmp");
    CHECK(!LittleFS.open("/q/1.okw"), "purged .okw file gone");
    CHECK(!LittleFS.open("/q/2.ok"), "purged .ok file gone");
    CHECK(!LittleFS.open("/q/9.tmp"), "stray .tmp also purged");
    // nextId must still run past the purged max (stray .tmp id 9 counts
    // too): job ids never wrap onto old filenames (stale job-uri safety),
    // content is fresh
    spool(q2, 0, "four", true);
    CHECK(q2.hasData() && q2.currentJobId() == 10, "new job got id 10 (past purged 1,2,9)");
    CHECK(readJob(q2) == "four", "fresh job content intact");
    CHECK(!q2.hasData(), "queue drained");
    // a second "reboot" on an EMPTY queue is a no-op, not an error
    PrintQueue q3;
    q3.begin();
    CHECK(!q3.hasData() && q3.purgedAtBoot() == 0, "reboot on empty queue is a no-op");
  }

  // ---------- group 4: stale .tmp cleanup at begin() ----------------------
  lfsGlueFormat();
  {
    LittleFS.mkdir("/q");
    {   // simulate a power loss mid-transfer: a closed .tmp file
      File f = LittleFS.open("/q/9.tmp", "w");
      CHECK((bool)f, "stray .tmp created");
      f.write((const uint8_t*)"garbage", 7);
      f.close();
    }
    PrintQueue q;
    q.begin();
    CHECK(!q.hasData(), "stale .tmp is not printable");
    CHECK(!LittleFS.open("/q/9.tmp"), "stale .tmp removed by begin()");
  }

  // ---------- group 5: cancelled and empty jobs never finalize -----------
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    CHECK(q.startJob(0), "startJob for the cancel test");
    q.write(0, (const uint8_t*)"data", 4);
    CHECK(!q.endJob(0, true), "cancelled job is not finalized");
    CHECK(!q.hasData(), "cancelled job is not printable");
    CHECK(q.startJob(1), "startJob for the empty test");
    CHECK(!q.endJob(1, false), "empty job is not finalized");
    CHECK(!q.hasData(), "empty job is not printable");
  }

  // ---------- group 6: pendingCount bookkeeping ---------------------------
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    spool(q, 0, "job1", false);
    spool(q, 1, "job2", false);
    CHECK(q.pendingCount() == 2, "two jobs pending");
    CHECK(q.hasData() && q.readByte() == 'j', "read one byte of job 1");
    q.dropCurrent();
    CHECK(q.pendingCount() == 1, "dropCurrent decrements pending");
    CHECK(q.hasData() && q.currentJobId() == 2, "next job after drop");
    drain(q);
    CHECK(q.pendingCount() == 0, "pending 0 after draining");
  }

  // ---------- group 7: two writers in parallel ----------------------------
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    CHECK(q.startJob(0) && q.startJob(3), "concurrent writers on slots 0 and 3");
    q.write(0, (const uint8_t*)"A", 1);
    q.write(3, (const uint8_t*)"B", 1);
    q.markFormat(3, true);
    CHECK(q.endJob(0, false) && q.endJob(3, false), "both finalize");
    // lowest id first: slot 0's job
    CHECK(q.hasData() && q.currentJobId() == 1 && !q.currentJobIsPwg(),
          "slot 0's job is first (raw)");
    CHECK(readJob(q) == "A", "job 1 content intact");
    CHECK(q.hasData() && q.currentJobId() == 2 && q.currentJobIsPwg(),
          "slot 3's job is second (pwg)");
    CHECK(readJob(q) == "B", "job 2 content intact");
  }

  // ---------- group 8: flash fills mid-transfer -> job discarded ----------
  // The 64 KB RAM FS cannot hold ~200 KB. The sticky write-failure flag
  // (round-3 fix) must make endJob() discard the truncated job instead of
  // finalizing a file that would print as a half page.
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    CHECK(q.startJob(0), "startJob for the overflow test");
    uint8_t buf[1024];
    memset(buf, 'x', sizeof(buf));
    for (int i = 0; i < 200; i++) q.write(0, buf, sizeof(buf));
    CHECK(!q.endJob(0, false), "overflowed job is NOT finalized (truncated)");
    CHECK(!q.hasData(), "overflowed job is not printable");
    CHECK(q.pendingCount() == 0, "overflowed job not counted");
    // and the queue still works afterwards:
    spool(q, 1, "after-full", false);
    CHECK(q.hasData() && drain(q) == "after-full",
          "queue healthy after the overflow");
  }

  // ---------- group 9: jobId() accessor (IPP job-id/job-uri regression) ----
  // The IPP Print-Job response quotes the REAL spool id via jobId(): a
  // hardcoded 1 used to point at a phantom /job/1 that could collide with
  // a genuinely spooled job #1. Pin the accessor's contract: live while the
  // writer is open, zero after endJob, and unique across concurrent slots.
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    spool(q, 0, "one", false);               // job 1 (spool() opens+closes)
    CHECK(q.jobId(0) == 0, "jobId: closed slot reports 0");
    CHECK(q.startJob(1) && q.jobId(1) == 2, "jobId: live writer reports its id");
    CHECK(q.startJob(2) && q.jobId(2) == 3, "jobId: second concurrent slot distinct");
    CHECK(q.jobId(3) == 0, "jobId: never-opened slot reports 0");
    // give both writers data: an EMPTY job is discarded by endJob() by
    // design (group 5) - that path must not be confused with a broken one
    q.write(1, (const uint8_t*)"x", 1);
    q.write(2, (const uint8_t*)"y", 1);
    CHECK(q.endJob(1, false) && q.endJob(2, false), "both finalize");
    CHECK(q.jobId(1) == 0 && q.jobId(2) == 0, "jobId: back to 0 after endJob");
    CHECK(!q.jobId(4) && !q.jobId(-1), "jobId: out-of-range slot rejected");
  }

  // ---------- group 10: removeJobById (Cancel-Job, 2.4.0) ----------
  // Cancel-Job must remove a WAITING job by id and leave the others
  // (and their order) intact; the job the reader sits on mid-print must
  // refuse; unknown ids must fail cleanly.
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    spool(q, 0, "J1", false);      // job 1
    spool(q, 0, "J2", false);      // job 2
    spool(q, 0, "J3", false);      // job 3
    CHECK(q.pendingCount() == 3, "cancel: three jobs waiting");
    CHECK(q.removeJobById(2), "cancel: job 2 removed by id");
    CHECK(q.pendingCount() == 2, "cancel: pending drops to 2");
    CHECK(!q.removeJobById(2), "cancel: same id gone -> false");
    CHECK(!q.removeJobById(0), "cancel: id 0 rejected");
    CHECK(!q.removeJobById(999), "cancel: unknown id -> false");
    CHECK(q.hasData() && q.currentJobId() == 1, "cancel: job 1 still first");
    std::string out = drain(q);
    CHECK(out == "J1J3", "cancel: remaining jobs intact and in order");

    // mid-print job cannot be cancelled: open the reader on a job
    lfsGlueFormat();
    PrintQueue q2;
    q2.begin();
    spool(q2, 0, "MID", false);
    CHECK(q2.hasData(), "cancel-mid: reader opened on job");
    CHECK(!q2.removeJobById(q2.currentJobId()),
          "cancel-mid: job under the reader is refused");
    CHECK(q2.pendingCount() == 1, "cancel-mid: pending unchanged");
    // PWG flavour removable too
    spool(q2, 0, "P", true);
    CHECK(q2.removeJobById(2), "cancel: .okw job removable by id");
    CHECK(!q2.hasData() || q2.currentJobId() != 2, "cancel: .okw gone from queue");
  }

  // ---------- group 11: job-state registry (Get-Job-Attributes, 2.4.1) ----------
  // endJob -> 3 (pending); engineSwitchJob/markJobState -> 5 (processing);
  // print end -> 9 (completed). Rolling window of JOB_HISTORY; unknown = 0.
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    CHECK(q.jobStateOf(1) == 0, "registry: unseen id is unknown (0)");
    spool(q, 0, "A", false);                          // job 1 -> pending
    CHECK(q.jobStateOf(1) == 3, "registry: fresh spool = pending (3)");
    q.markJobState(1, 5);                              // engine starts it
    CHECK(q.jobStateOf(1) == 5, "registry: mark processing (5)");
    q.markJobState(1, 9);                              // engine finished it
    CHECK(q.jobStateOf(1) == 9, "registry: mark completed (9)");
    CHECK(q.jobStateOf(2) == 0, "registry: other id still unknown");
    // rolling window: JOB_HISTORY+2 distinct ids push id 1 out
    for (uint32_t i = 2; i <= PrintQueue::JOB_HISTORY + 3; i++) {
      q.startJob(0);
      q.write(0, (const uint8_t*)"x", 1);
      q.endJob(0, false);
    }
    CHECK(q.jobStateOf(1) == 0, "registry: window rolls, oldest forgotten");
    CHECK(q.jobStateOf(PrintQueue::JOB_HISTORY + 3) == 3,
          "registry: newest id still known");
  }

  // ---------- group 12: pushback (non-blocking engine, 2.4.2) ----------
  // readByte -> pushback -> readByte must replay the SAME byte once, then
  // continue in order. A byte lost or doubled here reprints garbage.
  lfsGlueFormat();
  {
    PrintQueue q;
    q.begin();
    spool(q, 0, "ABC", false);          // job 1 = "ABC"
    CHECK(q.hasData(), "pushback: job ready");
    int b1 = q.readByte();
    CHECK(b1 == 'A', "pushback: first read = A");
    q.pushback();
    int b2 = q.readByte();
    CHECK(b2 == 'A', "pushback: replayed byte = A (not lost)");
    int b3 = q.readByte();
    int b4 = q.readByte();
    CHECK(b3 == 'B' && b4 == 'C', "pushback: stream continues B, C in order");
    // after an EOF read the pushback slot is spent: arming without a fresh
    // byte must NOT replay the last byte again (a doubled byte = garbage)
    CHECK(q.readByte() == -1, "pushback: stream at EOF after C");
    q.pushback();
    CHECK(q.readByte() == -1, "pushback: after EOF no byte is resurrected");
    // the honest use right after a successful read: C is held back and
    // replayed (printer busy retry) - already covered above with A
    spool(q, 0, "XY", false);           // new job for a cross-boundary replay
    CHECK(q.hasData(), "pushback: next job ready");
    int x1 = q.readByte();
    CHECK(x1 == 'X', "pushback: new job reads X");
    q.pushback();
    CHECK(q.readByte() == 'X', "pushback: replay across the boundary stays exact");
  }

  printf(failures ? "\n%d FAILURES\n" : "\nALL QUEUE TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
