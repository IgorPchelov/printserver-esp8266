// LittleFS.h - host shim: Arduino File/FSClass API backed by a REAL
// littlefs (vendored under littlefs/) on a RAM "flash". Lets PrintQueue.cpp
// run unmodified in host unit tests (tests/test_queue.cpp).
//
// Mirrors arduino-esp32 2.0.x VFSImpl semantics that PrintQueue relies on:
//   - open("/q/") (trailing slash) stats as a dir and opendirs  - verified
//     against esp_littlefs + littlefs path handling
//   - File::name() of a directory entry returns the BASE name (no path)
//   - File is a value type with shared state (assignment/copy safe)
//   - write() returns bytes written, 0 when the FS is full (NOSPC)
#pragma once
#include "Arduino.h"

extern "C" {
#include "littlefs/lfs.h"
}

extern lfs_t g_lfs;          // glue.cpp owns the instance + block device
class FSClass;
extern FSClass LittleFS;     // glue.cpp

struct FileState {
  int rc = 1;
  bool valid = false;
  bool isDir = false;
  std::string path;          // full path, leading '/', e.g. "/q/5.ok"
  lfs_file_t f;
  lfs_dir_t d;
};

class File {
public:
  File() : s(nullptr) {}
  explicit File(FileState* st) : s(st) {}
  ~File() { release(); }
  File(const File& o) : s(o.s) { if (s) s->rc++; }
  File& operator=(const File& o) {
    if (this != &o) { release(); s = o.s; if (s) s->rc++; }
    return *this;
  }
  operator bool() const { return s && s->valid; }
  bool isDirectory() const { return s && s->valid && s->isDir; }

  String name() const {
    if (!s) return String();
    size_t p = s->path.rfind('/');
    return String(p == std::string::npos ? s->path : s->path.substr(p + 1));
  }

  size_t size() const {
    if (!s || !s->valid || s->isDir) return 0;
    lfs_ssize_t sz = lfs_file_size(&g_lfs, &s->f);
    return sz > 0 ? (size_t)sz : 0;
  }
  size_t available() const {
    if (!s || !s->valid || s->isDir) return 0;
    lfs_soff_t pos = lfs_file_tell(&g_lfs, &s->f);
    lfs_ssize_t sz = lfs_file_size(&g_lfs, &s->f);
    return (sz > 0 && pos >= 0) ? (size_t)(sz - pos) : 0;
  }
  int read() {
    if (!s || !s->valid || s->isDir) return -1;
    uint8_t b;
    return lfs_file_read(&g_lfs, &s->f, &b, 1) == 1 ? b : -1;
  }
  size_t read(uint8_t* buf, size_t len) {
    if (!s || !s->valid || s->isDir || !buf || !len) return 0;
    lfs_ssize_t r = lfs_file_read(&g_lfs, &s->f, buf, len);
    return r > 0 ? (size_t)r : 0;
  }
  size_t write(uint8_t b) {
    if (!s || !s->valid || s->isDir) return 0;
    return lfs_file_write(&g_lfs, &s->f, &b, 1) == 1 ? 1 : 0;
  }
  size_t write(const uint8_t* buf, size_t len) {
    if (!s || !s->valid || s->isDir || !buf || !len) return 0;
    lfs_ssize_t r = lfs_file_write(&g_lfs, &s->f, buf, len);
    return r > 0 ? (size_t)r : 0;
  }
  void flush() { if (s && s->valid && !s->isDir) lfs_file_sync(&g_lfs, &s->f); }
  void close() {
    if (s && s->valid) {
      if (s->isDir) lfs_dir_close(&g_lfs, &s->d);
      else lfs_file_close(&g_lfs, &s->f);
      s->valid = false;
    }
  }
  // POSIX-style readdir skipping "." / ".." (like esp_littlefs's readdir):
  // REG entries come back as readable Files, DIR entries as dir Files.
  File openNextFile();

private:
  void release() {
    if (s && --s->rc == 0) {
      close();
      delete s;
    }
  }
  FileState* s;
};

// shared helper (glue.cpp): opens "/q/5.ok"-style paths with 'r'/'w' modes
File lfsGlueOpenFile(const std::string& fullPath, const char* mode);

class FSClass {
public:
  bool begin(bool formatOnFail = false);   // mount; format+retry on failure
  File open(const String& path, const char* mode = "r") {
    std::string n = normPath(path);
    struct lfs_info info;
    bool exists = lfs_stat(&g_lfs, n.c_str(), &info) == 0;
    std::string full = fullPathOf(path);
    if (exists && info.type == LFS_TYPE_DIR) {
      FileState* st = new FileState();
      st->path = full;
      if (lfs_dir_open(&g_lfs, &st->d, n.c_str()) == 0) {
        st->valid = true; st->isDir = true;
      } else { delete st; return File(); }
      return File(st);
    }
    if (!exists && (!mode || mode[0] != 'w')) return File();  // 'r' on missing
    return lfsGlueOpenFile(full, mode);
  }
  bool remove(const String& path) {
    return lfs_remove(&g_lfs, normPath(path).c_str()) == 0;
  }
  bool rename(const String& from, const String& to) {
    return lfs_rename(&g_lfs, normPath(from).c_str(), normPath(to).c_str()) == 0;
  }
  bool mkdir(const String& path) {
    return lfs_mkdir(&g_lfs, normPath(path).c_str()) == 0;
  }
  size_t totalBytes() const { return BLOCKS * BLOCK_SIZE; }
  size_t usedBytes() const {
    lfs_ssize_t b = lfs_fs_size(&g_lfs);
    return b > 0 ? (size_t)b * BLOCK_SIZE : 0;
  }

  // RAM geometry (small on purpose: the disk-full test needs to fill it)
  static const size_t BLOCK_SIZE = 4096;
  static const size_t BLOCKS = 16;

private:
  static std::string normPath(const String& p) {
    std::string r(p.c_str());
    while (r.size() && r[0] == '/') r.erase(0, 1);   // littlefs: no leading '/'
    return r;
  }
  static std::string fullPathOf(const String& p) {
    std::string r(p.c_str());
    if (r.empty() || r[0] != '/') r.insert(0, "/");
    return r;
  }
};

// test-side: format + mount the RAM FS (fresh state between test groups)
void lfsGlueFormat();
