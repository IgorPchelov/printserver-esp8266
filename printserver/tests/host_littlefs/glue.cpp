// glue.cpp - littlefs RAM block device + globals for the host shim.
// See tests/host_littlefs/LittleFS.h.
#include "LittleFS.h"
#include <cstring>

lfs_t g_lfs;
FSClass LittleFS;

static uint8_t g_flash[FSClass::BLOCKS * FSClass::BLOCK_SIZE];

static int bd_read(const struct lfs_config* c, lfs_block_t block,
                   lfs_off_t off, void* buffer, lfs_size_t size) {
  (void)c;
  memcpy(buffer, g_flash + block * FSClass::BLOCK_SIZE + off, size);
  return 0;
}
static int bd_prog(const struct lfs_config* c, lfs_block_t block,
                   lfs_off_t off, const void* buffer, lfs_size_t size) {
  (void)c;
  memcpy(g_flash + block * FSClass::BLOCK_SIZE + off, buffer, size);
  return 0;
}
static int bd_erase(const struct lfs_config* c, lfs_block_t block) {
  (void)c;
  memset(g_flash + block * FSClass::BLOCK_SIZE, 0xff, FSClass::BLOCK_SIZE);
  return 0;
}
static int bd_sync(const struct lfs_config* c) { (void)c; return 0; }

static struct lfs_config g_cfg = {0};

static void cfgInit() {
  g_cfg.context = nullptr;
  g_cfg.read = bd_read;
  g_cfg.prog = bd_prog;
  g_cfg.erase = bd_erase;
  g_cfg.sync = bd_sync;
  g_cfg.read_size = 16;
  g_cfg.prog_size = 16;
  g_cfg.block_size = FSClass::BLOCK_SIZE;
  g_cfg.block_count = FSClass::BLOCKS;
  g_cfg.block_cycles = 500;
  g_cfg.cache_size = 256;
  g_cfg.lookahead_size = 64;
}

void lfsGlueFormat() {
  cfgInit();
  memset(g_flash, 0xff, sizeof(g_flash));
  int r = lfs_format(&g_lfs, &g_cfg);
  (void)r;
  r = lfs_mount(&g_lfs, &g_cfg);
  (void)r;
}

bool FSClass::begin(bool formatOnFail) {
  cfgInit();
  if (lfs_mount(&g_lfs, &g_cfg) == 0) return true;
  if (!formatOnFail) return false;
  lfsGlueFormat();
  return true;   // lfsGlueFormat mounts on success
}

static std::string norm(const std::string& p) {
  std::string r = p;
  while (r.size() && r[0] == '/') r.erase(0, 1);
  return r;
}

File lfsGlueOpenFile(const std::string& fullPath, const char* mode) {
  int flags = (mode && mode[0] == 'w')
              ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)
              : LFS_O_RDONLY;
  FileState* st = new FileState();
  st->path = fullPath;
  if (lfs_file_open(&g_lfs, &st->f, norm(fullPath).c_str(), flags) == 0) {
    st->valid = true;
  } else {
    delete st;
    return File();
  }
  return File(st);
}

File File::openNextFile() {
  if (!s || !s->valid || !s->isDir) return File();
  struct lfs_info info;
  while (lfs_dir_read(&g_lfs, &s->d, &info) > 0) {
    if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) continue;
    std::string child = s->path;
    if (child.empty() || child.back() != '/') child += '/';
    child += info.name;
    if (info.type == LFS_TYPE_DIR) {
      FileState* st = new FileState();
      st->valid = true;
      st->isDir = true;
      st->path = child;
      return File(st);
    }
    return lfsGlueOpenFile(child, "r");
  }
  return File();
}
