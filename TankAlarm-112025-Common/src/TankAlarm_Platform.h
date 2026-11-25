/**
 * TankAlarm_Platform.h
 * 
 * Platform detection and abstractions for TankAlarm 112025.
 * Handles differences between AVR, STM32, and Mbed OS platforms.
 * 
 * Copyright (c) 2025 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_PLATFORM_H
#define TANKALARM_PLATFORM_H

#include <Arduino.h>

// ============================================================================
// Platform Detection and PROGMEM Support
// ============================================================================

#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
#else
  #ifndef PROGMEM
    #define PROGMEM
  #endif
  #ifndef pgm_read_byte_near
    #define pgm_read_byte_near(addr) (*(const uint8_t *)(addr))
  #endif
#endif

// ============================================================================
// Filesystem Support Detection
// ============================================================================
#if defined(ARDUINO_ARCH_STM32) && !defined(ARDUINO_ARCH_MBED)
  #include <LittleFS.h>
  #include <IWatchdog.h>
  #define TANKALARM_FILESYSTEM_AVAILABLE
  #define TANKALARM_WATCHDOG_AVAILABLE
#elif defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  #include <LittleFileSystem.h>
  #include <BlockDevice.h>
  #include <mbed.h>
  // Bring specific mbed types into global namespace
  using mbed::LittleFileSystem;
  using mbed::BlockDevice;
  using mbed::Watchdog;
  // Bring arduino types into global namespace for library compatibility
  using arduino::Stream;
  using arduino::Print;
  #define TANKALARM_FILESYSTEM_AVAILABLE
  #define TANKALARM_WATCHDOG_AVAILABLE
  #define TANKALARM_POSIX_FILE_IO_AVAILABLE
  #define TANKALARM_POSIX_FS_PREFIX "/fs"
#endif

// ============================================================================
// Watchdog Helper Class (Mbed OS)
// ============================================================================
#if defined(TANKALARM_WATCHDOG_AVAILABLE) && (defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED))
class MbedWatchdogHelper {
public:
  bool start(uint32_t timeoutMs) {
    mbed::Watchdog &watchdog = mbed::Watchdog::get_instance();
    return watchdog.start(timeoutMs);
  }
  
  void kick() {
    mbed::Watchdog &watchdog = mbed::Watchdog::get_instance();
    watchdog.kick();
  }
  
  uint32_t get_timeout() const {
    mbed::Watchdog &watchdog = mbed::Watchdog::get_instance();
    return watchdog.get_timeout();
  }
};
#endif

// ============================================================================
// Watchdog Macros (platform-agnostic)
// ============================================================================
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    #define TANKALARM_WATCHDOG_KICK(watchdog) (watchdog).kick()
    #define TANKALARM_WATCHDOG_START(watchdog, ms) (watchdog).start(ms)
  #else
    #define TANKALARM_WATCHDOG_KICK(watchdog) IWatchdog.reload()
    #define TANKALARM_WATCHDOG_START(watchdog, ms) IWatchdog.begin((ms) * 1000UL)
  #endif
#else
  #define TANKALARM_WATCHDOG_KICK(watchdog) ((void)0)
  #define TANKALARM_WATCHDOG_START(watchdog, ms) ((void)0)
#endif

// ============================================================================
// POSIX File I/O Helpers
// ============================================================================
#ifdef TANKALARM_POSIX_FILE_IO_AVAILABLE

#include <cstdio>
#include <cerrno>

/**
 * Get file size using POSIX file operations
 * @param fp Open file pointer
 * @return File size in bytes, or -1 on error
 */
static inline long tankalarm_posix_file_size(FILE *fp) {
  if (!fp) return -1;
  long currentPos = ftell(fp);
  if (currentPos < 0) return -1;
  if (fseek(fp, 0, SEEK_END) != 0) return -1;
  long size = ftell(fp);
  fseek(fp, currentPos, SEEK_SET);
  return size;
}

/**
 * Check if file exists using POSIX file operations
 * @param path File path
 * @return true if file exists
 */
static inline bool tankalarm_posix_file_exists(const char *path) {
  FILE *fp = fopen(path, "r");
  if (fp) {
    fclose(fp);
    return true;
  }
  return false;
}

/**
 * Log POSIX error for debugging
 * @param operation Operation name
 * @param path File path
 */
static inline void tankalarm_posix_log_error(const char *operation, const char *path) {
#ifdef DEBUG_MODE
  Serial.print(F("POSIX error in "));
  Serial.print(operation);
  Serial.print(F(" for "));
  Serial.print(path);
  Serial.print(F(": errno="));
  Serial.println(errno);
#else
  (void)operation;
  (void)path;
#endif
}

/**
 * Atomic file write using write-to-temp-then-rename pattern.
 * Prevents data loss if power fails during a save operation.
 *
 * On LittleFS (Mbed OS), rename() atomically replaces the target file.
 * If power fails during fwrite(), the original file is untouched.
 * If power fails during rename(), LittleFS recovers to old or new state.
 *
 * Binary-safe: opens temp file in "wb" mode.  Text callers can pass
 * string bytes directly — the only difference on this platform is that
 * no newline translation occurs (LittleFS has none anyway).
 *
 * @param path   Target file path (e.g., "/fs/server_config.json")
 * @param data   Data buffer to write (text or binary)
 * @param len    Number of bytes to write
 * @return true on success, false on any error (original file preserved)
 */
static inline bool tankalarm_posix_write_file_atomic(const char *path,
                                                      const char *data,
                                                      size_t len)
{
    // Build temp path: append ".tmp" suffix
    size_t pathLen = strlen(path);
    if (pathLen == 0 || pathLen > 250) {  // 250 + 4 (.tmp) + 1 (null) = 255
        Serial.println(F("atomic write: path too long or empty"));
        return false;
    }
    char tmpPath[256];
    memcpy(tmpPath, path, pathLen);
    memcpy(tmpPath + pathLen, ".tmp", 5);  // includes null terminator

    // Step 1: Write data to temporary file
    FILE *fp = fopen(tmpPath, "wb");
    if (!fp) {
        tankalarm_posix_log_error("atomic:fopen", tmpPath);
        Serial.print(F("atomic write failed (fopen): "));
        Serial.println(path);
        return false;
    }

    size_t written = fwrite(data, 1, len, fp);
    int writeErr = ferror(fp);

    // fclose() on Mbed OS flushes all buffers and syncs to storage.
    // No separate fsync() needed on this platform. A small write sits in
    // stdio's buffer until then, so a flash that does not take it (full, or
    // a bad block) shows only in what fclose() returns.
    int closeErr = fclose(fp);

    if (writeErr || written != len) {
        tankalarm_posix_log_error("atomic:fwrite", tmpPath);
        Serial.print(F("atomic write failed (fwrite): "));
        Serial.println(path);
        remove(tmpPath);  // Clean up partial temp file to free flash space
        return false;
    }

    // A temp file whose close failed may be short or empty: it must not take
    // the target's place, nor be left for the boot-time cleanup to rename.
    if (closeErr != 0) {
        tankalarm_posix_log_error("atomic:fclose", tmpPath);
        Serial.print(F("atomic write failed (fclose): "));
        Serial.println(path);
        remove(tmpPath);
        return false;
    }

    // Step 2: Atomic rename — replaces target in one operation.
    // LittleFS rename() atomically handles overwriting an existing target.
    // Do NOT call remove(path) first — that creates a data-loss window.
    if (rename(tmpPath, path) != 0) {
        tankalarm_posix_log_error("atomic:rename", path);
        Serial.print(F("atomic write failed (rename): "));
        Serial.println(path);
        // Leave tmpPath on disk — recovery code can complete the rename on boot.
        return false;
    }

    return true;
}

/**
 * Boot-time cleanup for orphaned .tmp files left by interrupted atomic writes.
 *
 * For each path in the list:
 *  - If both path.tmp and path exist → remove .tmp (original is intact)
 *  - If only path.tmp exists → complete the rename (only copy of data)
 *
 * Call once during setup(), after the filesystem is mounted.
 *
 * @param paths      Array of canonical file paths (e.g., "/fs/server_config.json")
 * @param pathCount  Number of entries in the array
 */
static inline void tankalarm_posix_cleanup_tmp_files(const char * const paths[],
                                                      size_t pathCount)
{
    for (size_t i = 0; i < pathCount; i++) {
        size_t pathLen = strlen(paths[i]);
        if (pathLen == 0 || pathLen > 250) continue;

        char tmpPath[256];
        memcpy(tmpPath, paths[i], pathLen);
        memcpy(tmpPath + pathLen, ".tmp", 5);

        if (!tankalarm_posix_file_exists(tmpPath)) continue;

        if (tankalarm_posix_file_exists(paths[i])) {
            // Original intact — discard orphaned temp
            remove(tmpPath);
            Serial.print(F("tmp cleanup: removed orphan "));
            Serial.println(tmpPath);
        } else {
            // Only temp exists — complete the interrupted rename
            if (rename(tmpPath, paths[i]) == 0) {
                Serial.print(F("tmp cleanup: recovered "));
                Serial.println(paths[i]);
            } else {
                Serial.print(F("tmp cleanup: rename failed "));
                Serial.println(paths[i]);
            }
        }
    }
}

#endif // TANKALARM_POSIX_FILE_IO_AVAILABLE

// ============================================================================
// LittleFS Atomic File Write (non-POSIX STM32duino builds)
// ============================================================================
#if defined(TANKALARM_FILESYSTEM_AVAILABLE) && !defined(TANKALARM_POSIX_FILE_IO_AVAILABLE)

/**
 * Atomic file write for Arduino LittleFS (STM32duino).
 * Same write-to-temp-then-rename pattern as the POSIX version.
 *
 * @param path   Target file path (e.g., "/client_config.json")
 * @param data   Data buffer to write
 * @param len    Number of bytes to write
 * @return true on success, false on any error (original file preserved)
 */
static bool tankalarm_littlefs_write_file_atomic(const char *path,
                                                  const uint8_t *data,
                                                  size_t len)
{
    String tmpPath = String(path) + ".tmp";

    File tmp = LittleFS.open(tmpPath.c_str(), "w");
    if (!tmp) {
        Serial.print(F("atomic write failed (open): "));
        Serial.println(path);
        return false;
    }

    size_t written = tmp.write(data, len);
    tmp.close();

    if (written != len) {
        Serial.print(F("atomic write failed (write): "));
        Serial.println(path);
        LittleFS.remove(tmpPath.c_str());
        return false;
    }

    // LittleFS.rename() atomically overwrites existing target on STM32.
    // Do NOT call LittleFS.remove(path) first.
    if (!LittleFS.rename(tmpPath.c_str(), path)) {
        Serial.print(F("atomic write failed (rename): "));
        Serial.println(path);
        return false;
    }

    return true;
}

/**
 * Boot-time cleanup for orphaned .tmp files (LittleFS variant).
 * Same logic as the POSIX version — see tankalarm_posix_cleanup_tmp_files().
 */
static void tankalarm_littlefs_cleanup_tmp_files(const char * const paths[],
                                                  size_t pathCount)
{
    for (size_t i = 0; i < pathCount; i++) {
        String tmpPath = String(paths[i]) + ".tmp";
        if (!LittleFS.exists(tmpPath.c_str())) continue;

        if (LittleFS.exists(paths[i])) {
            LittleFS.remove(tmpPath.c_str());
            Serial.print(F("tmp cleanup: removed orphan "));
            Serial.println(tmpPath);
        } else {
            if (LittleFS.rename(tmpPath.c_str(), paths[i])) {
                Serial.print(F("tmp cleanup: recovered "));
                Serial.println(paths[i]);
            } else {
                Serial.print(F("tmp cleanup: rename failed "));
                Serial.println(paths[i]);
            }
        }
    }
}

#endif // TANKALARM_FILESYSTEM_AVAILABLE && !TANKALARM_POSIX_FILE_IO_AVAILABLE

#endif // TANKALARM_PLATFORM_H
