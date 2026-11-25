// Host tests for tankalarm_posix_write_file_atomic() in TankAlarm-112025-Common/src/TankAlarm_Platform.h.
//
// The writer writes a temp file and renames it over the file. The temp file may take the place of
// the file only when all of it is there, so a write that fails at any step must leave the file as
// it was. fclose() is the step that is easy to overlook: on Mbed OS it flushes the last bytes to
// flash, and a flush that fails shows only in what fclose() returns.
//
// The writer's fopen(), fwrite(), fclose() and rename() go through the wrappers below, which can
// make one of them fail. A failed fclose() also loses the bytes it had yet to flush.
//
//   atomic_write_test <folder>     writes its files into <folder>, which it makes

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "Arduino.h"

namespace fault {

enum Op { NONE, FOPEN, FWRITE, FWRITE_ERROR, FCLOSE, RENAME };

Op failing = NONE;
std::string openedForWriting;  // the last file the writer opened to write

}  // namespace fault

static FILE *test_fopen(const char *path, const char *mode) {
  if (fault::failing == fault::FOPEN) {
    errno = ENOSPC;
    return nullptr;
  }
  if (strchr(mode, 'w') != nullptr) {
    fault::openedForWriting = path;
  }
  return fopen(path, mode);
}

static size_t test_fwrite(const void *data, size_t size, size_t count, FILE *fp) {
  if (fault::failing == fault::FWRITE) {
    return fwrite(data, size, count / 2, fp);  // the flash fills up half way
  }
  const size_t written = fwrite(data, size, count, fp);
  if (fault::failing == fault::FWRITE_ERROR) {
    (void)fgetc(fp);  // every byte was taken, and the stream's error flag is set (a read of a file opened to write)
  }
  return written;
}

static int test_fclose(FILE *fp) {
  const int closed = fclose(fp);
  if (fault::failing != fault::FCLOSE) {
    return closed;
  }
  // The last flush fails: what it had yet to write is not in the file.
  struct stat st;
  if (!fault::openedForWriting.empty() && stat(fault::openedForWriting.c_str(), &st) == 0) {
    if (truncate(fault::openedForWriting.c_str(), st.st_size / 2) != 0) {
      perror("truncate");
    }
  }
  errno = ENOSPC;
  return EOF;
}

static int test_rename(const char *from, const char *to) {
  if (fault::failing == fault::RENAME) {
    errno = EIO;
    return -1;
  }
  return rename(from, to);
}

#define fopen test_fopen
#define fwrite test_fwrite
#define fclose test_fclose
#define rename test_rename
#define TANKALARM_POSIX_FILE_IO_AVAILABLE
#include "TankAlarm_Platform.h"
#undef fopen
#undef fwrite
#undef fclose
#undef rename

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++checks;                                                           \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                   \
  } while (0)

static bool exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string contents(const std::string &path) {
  std::string text;
  FILE *fp = fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    return "(none)";
  }
  char buf[256];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
    text.append(buf, got);
  }
  fclose(fp);
  return text;
}

static bool write(const std::string &path, const std::string &text, fault::Op failing = fault::NONE) {
  fault::failing = failing;
  fault::openedForWriting.clear();
  Serial.printed.clear();
  const bool ok = tankalarm_posix_write_file_atomic(path.c_str(), text.data(), text.size());
  fault::failing = fault::NONE;
  return ok;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <folder>\n", argv[0]);
    return 2;
  }
  const std::string folder = argv[1];
  if (mkdir(folder.c_str(), 0755) != 0 && errno != EEXIST) {
    perror(folder.c_str());
    return 2;
  }
  const std::string path = folder + "/journal.json";
  const std::string tmp = path + ".tmp";
  remove(path.c_str());
  remove(tmp.c_str());

  const std::string before = "{\"req\":1795600000,\"ph\":2,\"ctl\":1}";
  const std::string after = "{\"req\":1795600000,\"ph\":0,\"ctl\":1,\"ans\":1795600000}";

  // A file that is not there yet, and one that is replaced: no temp file is left.
  CHECK(write(path, before));
  CHECK(contents(path) == before);
  CHECK(!exists(tmp));
  CHECK(write(path, after));
  CHECK(contents(path) == after);
  CHECK(!exists(tmp));
  CHECK(Serial.printed.empty());

  // The temp file cannot be opened, or the flash fills up as it is written: the file is as it was.
  CHECK(write(path, before));
  CHECK(!write(path, after, fault::FOPEN));
  CHECK(contents(path) == before);
  CHECK(!exists(tmp));
  CHECK(!write(path, after, fault::FWRITE));
  CHECK(contents(path) == before);
  CHECK(!exists(tmp));
  CHECK(Serial.printed.find("fwrite") != std::string::npos);
  CHECK(!write(path, after, fault::FWRITE_ERROR));  // the stream reports an error, though fwrite() took it all
  CHECK(contents(path) == before);
  CHECK(!exists(tmp));

  // Every byte was handed to fwrite(), and the last of them were lost as fclose() flushed them.
  // The half-written temp file must not take the place of the file.
  CHECK(!write(path, after, fault::FCLOSE));
  CHECK(contents(path) == before);
  CHECK(!exists(tmp));
  CHECK(Serial.printed.find("fclose") != std::string::npos);

  // The rename fails: the file is as it was, and the whole temp file is left for the cleanup at
  // boot (tankalarm_posix_cleanup_tmp_files()).
  CHECK(!write(path, after, fault::RENAME));
  CHECK(contents(path) == before);
  CHECK(contents(tmp) == after);
  remove(tmp.c_str());

  // And after each failure the next write goes through.
  CHECK(write(path, after));
  CHECK(contents(path) == after);
  CHECK(!exists(tmp));

  // An empty file is a file too.
  CHECK(write(path, ""));
  CHECK(exists(path) && contents(path).empty());

  remove(path.c_str());
  if (failures != 0) {
    fprintf(stderr, "atomic_write: %d of %d checks failed\n", failures, checks);
    return 1;
  }
  printf("atomic_write: all %d checks passed\n", checks);
  return 0;
}
