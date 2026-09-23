// Fault-injecting I/O for the WarmTierStore.h host tests. Include this before
// WarmTierStore.h: it defines WARM_IO_OVERRIDE and routes every WARM_* call
// through wrappers that count operations and can fail the Nth one.
#ifndef WARM_TEST_IO_H
#define WARM_TEST_IO_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <map>
#include <string>
#include <vector>

#include <ArduinoJson.h>

namespace fi {

enum Op { OP_FOPEN, OP_FREAD, OP_FWRITE, OP_FFLUSH, OP_FCLOSE, OP_FSEEK, OP_FTELL,
          OP_RENAME, OP_REMOVE, OP_STAT, OP_MKDIR, OP_COUNT };

inline const char *opName(int op) {
  static const char *const names[OP_COUNT] = {"fopen", "fread", "fwrite", "fflush", "fclose", "fseek",
                                              "ftell", "rename", "remove", "stat", "mkdir"};
  return (op >= 0 && op < OP_COUNT) ? names[op] : "none";
}

struct State {
  long ops = 0;            // operations since reset()
  long failAt = -1;        // index of the operation to fail (-1 = none)
  int failedOp = -1;       // kind of the operation that was failed
  long perOp[OP_COUNT] = {};
  long readBudget = -1;    // bytes fread may still return, then a silent EOF (-1 = unlimited)
  long mallocs = 0;
  long mallocFailAt = -1;
  std::vector<FILE *> errFiles;          // files with an injected error flag
  std::vector<std::string> writeOpens;   // paths opened for writing
};

inline State &state() {
  static State s;
  return s;
}

inline void reset() { state() = State(); }

inline bool hit(Op op) {
  State &s = state();
  s.perOp[op]++;
  const long index = s.ops++;
  if (index == s.failAt) {
    s.failedOp = op;
    return true;
  }
  return false;
}

inline void markErr(FILE *f) { state().errFiles.push_back(f); }

inline bool hasErr(FILE *f) {
  for (FILE *e : state().errFiles) {
    if (e == f) return true;
  }
  return false;
}

inline void clearErr(FILE *f) {
  std::vector<FILE *> &v = state().errFiles;
  for (size_t i = v.size(); i > 0; --i) {
    if (v[i - 1] == f) v.erase(v.begin() + (long)(i - 1));
  }
}

}  // namespace fi

inline FILE *t_fopen(const char *path, const char *mode) {
  if (fi::hit(fi::OP_FOPEN)) { errno = EIO; return nullptr; }
  FILE *f = fopen(path, mode);
  if (f && mode[0] == 'w') fi::state().writeOpens.push_back(path);
  return f;
}

inline size_t t_fread(void *buf, size_t size, size_t n, FILE *f) {
  if (fi::hit(fi::OP_FREAD)) { fi::markErr(f); return 0; }
  long &budget = fi::state().readBudget;
  if (budget == 0) return 0;  // pretend the file ended early, without an error
  if (budget > 0 && (long)(size * n) > budget) n = (size_t)budget / size;
  const size_t got = fread(buf, size, n, f);
  if (budget > 0) budget -= (long)(got * size);
  return got;
}

inline size_t t_fwrite(const void *buf, size_t size, size_t n, FILE *f) {
  if (fi::hit(fi::OP_FWRITE)) { fi::markErr(f); return 0; }
  return fwrite(buf, size, n, f);
}

inline int t_fflush(FILE *f) {
  if (fi::hit(fi::OP_FFLUSH)) { fi::markErr(f); return EOF; }
  return fflush(f);
}

inline int t_fclose(FILE *f) {
  const bool fail = fi::hit(fi::OP_FCLOSE);
  fi::clearErr(f);
  const int rc = fclose(f);
  return fail ? EOF : rc;
}

inline int t_ferror(FILE *f) { return (ferror(f) || fi::hasErr(f)) ? 1 : 0; }

inline int t_fseek(FILE *f, long off, int whence) {
  if (fi::hit(fi::OP_FSEEK)) { errno = EIO; return -1; }
  return fseek(f, off, whence);
}

inline long t_ftell(FILE *f) {
  if (fi::hit(fi::OP_FTELL)) { errno = EIO; return -1; }
  return ftell(f);
}

inline int t_rename(const char *from, const char *to) {
  if (fi::hit(fi::OP_RENAME)) { errno = EIO; return -1; }
  return rename(from, to);
}

inline int t_remove(const char *path) {
  if (fi::hit(fi::OP_REMOVE)) { errno = EIO; return -1; }
  return remove(path);
}

inline int t_stat(const char *path, struct stat *st) {
  if (fi::hit(fi::OP_STAT)) { errno = EIO; return -1; }
  return stat(path, st);
}

inline int t_mkdir(const char *path) {
  if (fi::hit(fi::OP_MKDIR)) { errno = EIO; return -1; }
  return mkdir(path, 0777);
}

inline void *t_malloc(size_t n) {
  fi::State &s = fi::state();
  if (s.mallocs++ == s.mallocFailAt) return nullptr;
  return malloc(n);
}

inline void t_free(void *p) { free(p); }

#define WARM_IO_OVERRIDE 1
#define WARM_FOPEN(path, mode) t_fopen((path), (mode))
#define WARM_FREAD(buf, size, n, f) t_fread((buf), (size), (n), (f))
#define WARM_FWRITE(buf, size, n, f) t_fwrite((buf), (size), (n), (f))
#define WARM_FFLUSH(f) t_fflush(f)
#define WARM_FCLOSE(f) t_fclose(f)
#define WARM_FERROR(f) t_ferror(f)
#define WARM_FSEEK(f, off, whence) t_fseek((f), (off), (whence))
#define WARM_FTELL(f) t_ftell(f)
#define WARM_RENAME(from, to) t_rename((from), (to))
#define WARM_REMOVE(path) t_remove(path)
#define WARM_STAT(path, st) t_stat((path), (st))
#define WARM_MKDIR(path) t_mkdir(path)
#define WARM_MALLOC(n) t_malloc(n)
#define WARM_FREE(p) t_free(p)

// ArduinoJson allocator that fails its Nth fallible call and tracks live blocks.
// Like a real heap it can fail to allocate or grow a block but never to
// shrink one (ArduinoJson relies on that: StringBuilder.hpp, MemoryPoolList.hpp).
struct TestAllocator : ArduinoJson::Allocator {
  long calls = 0;     // fallible calls so far
  long failAt = -1;   // index of the fallible call to fail (-1 = none)
  std::map<void *, size_t> blocks;

  long live() const { return (long)blocks.size(); }

  void *allocate(size_t size) override {
    if (calls++ == failAt) return nullptr;
    void *p = malloc(size ? size : 1);
    if (p) blocks[p] = size;
    return p;
  }

  void deallocate(void *ptr) override {
    if (!ptr) return;
    blocks.erase(ptr);
    free(ptr);
  }

  void *reallocate(void *ptr, size_t size) override {
    size_t old = 0;
    if (ptr) {
      std::map<void *, size_t>::iterator it = blocks.find(ptr);
      if (it != blocks.end()) old = it->second;
    }
    const bool shrink = ptr != nullptr && size <= old;
    if (!shrink && calls++ == failAt) return nullptr;
    void *p = malloc(size ? size : 1);
    if (!p) return nullptr;
    if (ptr) {
      memcpy(p, ptr, old < size ? old : size);
      blocks.erase(ptr);
      free(ptr);
    }
    blocks[p] = size;
    return p;
  }
};

#endif  // WARM_TEST_IO_H
