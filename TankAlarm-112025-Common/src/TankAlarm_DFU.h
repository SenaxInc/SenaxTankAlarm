/**
 * TankAlarm_DFU.h
 *
 * IAP (In-Application Programming) firmware update via Blues Notecard.
 *
 * The Blues Wireless for Opta carrier does NOT route the AUX pins needed for
 * Outboard DFU (ODFU). This module implements IAP DFU instead:
 *   1. Notecard downloads firmware from Notehub (automatic, background)
 *   2. Host polls dfu.status to detect when firmware is "ready"
 *   3. Host enters DFU mode (hub.set mode=dfu)
 *   4. Host reads firmware chunks via dfu.get
 *   5. Host writes chunks to internal flash via Mbed FlashIAP
 *   6. Host verifies, clears DFU state, restores hub mode, and reboots
 *
 * Reference: https://dev.blues.io/notehub/host-firmware-updates/iap-firmware-update/
 *            https://dev.blues.io/notehub/host-firmware-updates/notecard-api-requests-for-dfu/
 *
 * Copyright (c) 2025-2026 Senax Inc. All rights reserved.
 */

#ifndef TANKALARM_DFU_H
#define TANKALARM_DFU_H

#include <Arduino.h>
#include <Notecard.h>

// ============================================================================
// IAP DFU Configuration
// ============================================================================

// Chunk size for dfu.get reads (bytes of raw binary per request).
// Over I2C, must stay within Notecard I2C MTU. 128 bytes is safe and
// results in ~176 bytes of base64 in the JSON response.
#ifndef DFU_IAP_CHUNK_SIZE
#define DFU_IAP_CHUNK_SIZE 128
#endif

// Maximum retries for a single dfu.get chunk before aborting
#ifndef DFU_IAP_CHUNK_RETRIES
#define DFU_IAP_CHUNK_RETRIES 3
#endif

// Maximum time (ms) to wait for Notecard to enter DFU mode after hub.set
#ifndef DFU_IAP_MODE_TIMEOUT_MS
#define DFU_IAP_MODE_TIMEOUT_MS 60000UL
#endif

// ============================================================================
// IAP DFU Setup — called from each sketch's initializeNotecard()
// ============================================================================

/**
 * Enable IAP firmware download from Notehub to Notecard.
 * Replaces the ODFU card.dfu setup that doesn't work on Wireless for Opta.
 *
 * Sends: dfu.status {"on":true, "name":"user"}
 *
 * This allows the Notecard to accept firmware binaries pushed from Notehub.
 * The firmware sits in Notecard storage until the host reads it via dfu.get.
 */
static inline void tankalarm_enableIapDfu(Notecard &notecard) {
  J *req = notecard.newRequest("dfu.status");
  if (!req) {
    Serial.println(F("WARNING: Failed to create dfu.status enable request"));
    return;
  }
  JAddBoolToObject(req, "on", true);
  JAddStringToObject(req, "name", "user");

  J *rsp = notecard.requestAndResponse(req);
  if (rsp) {
    const char *err = JGetString(rsp, "err");
    if (err && err[0] != '\0') {
      Serial.print(F("WARNING: dfu.status enable failed: "));
      Serial.println(err);
    } else {
      Serial.println(F("IAP DFU enabled (firmware downloads from Notehub allowed)"));
    }
    notecard.deleteResponse(rsp);
  } else {
    Serial.println(F("WARNING: dfu.status enable returned no response"));
  }
}

// ============================================================================
// IAP DFU Status Check — called periodically from loop()
// ============================================================================

/**
 * IAP DFU status result, populated by tankalarm_checkDfuStatus().
 */
struct TankAlarmDfuStatus {
  bool updateAvailable;       // Firmware is fully downloaded and ready
  bool downloading;           // Notecard is still downloading from Notehub
  bool error;                 // DFU error occurred
  uint32_t firmwareLength;    // Size of firmware binary (when ready)
  uint32_t firmwareCrc32;      // CRC32 of firmware from Notecard body (when ready)
  char firmwareMd5[33];       // MD5 hash of firmware from Notecard body
  char firmwareName[96];      // Name of firmware from Notecard body
  char firmwareSource[96];    // Source filename from Notecard body
  char version[32];           // Available firmware version string
  char mode[16];              // Raw Notecard DFU mode string
  char errorMsg[64];          // Error message (if any)
};

// Extract a semantic version (e.g. "1.9.33") from a firmware filename such as
// "TankAlarm-Client-secure-v1.9.33.slot$20260617201834.bin". Returns true and fills
// `out` on the first "<major>.<minor>.<patch>" run that is not preceded by a digit or
// dot. Used to recover the OTA target version when the Notecard's dfu.status response
// has no explicit "version" field (the host .slot.bin embeds no Blues metadata).
static inline bool tankalarm_extractVersionFromFilename(const char *name, char *out, size_t outLen) {
  if (!name || !out || outLen == 0) return false;
  for (const char *p = name; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') continue;
    if (p != name) {
      const char prev = *(p - 1);
      if ((prev >= '0' && prev <= '9') || prev == '.') continue;  // middle of a number
    }
    int major = -1, minor = -1, patch = -1;
    if (sscanf(p, "%d.%d.%d", &major, &minor, &patch) == 3 &&
        major >= 0 && minor >= 0 && patch >= 0) {
      snprintf(out, outLen, "%d.%d.%d", major, minor, patch);
      return true;
    }
  }
  return false;
}

/**
 * Poll Notecard for IAP DFU status.
 *
 * Sends: dfu.status {"name":"user"}
 *
 * @param notecard  Reference to Notecard instance
 * @param status    Output: populated with current DFU state
 * @return true if Notecard responded (even if no update), false on I2C failure
 */
static inline bool tankalarm_checkDfuStatus(Notecard &notecard, TankAlarmDfuStatus &status) {
  memset(&status, 0, sizeof(status));
  strlcpy(status.mode, "idle", sizeof(status.mode));

  J *req = notecard.newRequest("dfu.status");
  if (!req) {
    return false;
  }
  JAddStringToObject(req, "name", "user");

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    return false;
  }

  const char *mode = JGetString(rsp, "mode");
  const char *version = JGetString(rsp, "version");
  const char *errMsg = JGetString(rsp, "err");

  if (mode && mode[0] != '\0') {
    strlcpy(status.mode, mode, sizeof(status.mode));
  }

  if (version && version[0] != '\0') {
    strlcpy(status.version, version, sizeof(status.version));
  }

  if (errMsg && errMsg[0] != '\0') {
    strlcpy(status.errorMsg, errMsg, sizeof(status.errorMsg));
    status.error = true;
  }

  // Parse firmware length, crc32, md5, name, source from body (available when mode is "ready")
  J *body = JGetObject(rsp, "body");
  if (body) {
    status.firmwareLength = (uint32_t)JGetNumber(body, "length");
    status.firmwareCrc32 = (uint32_t)JGetNumber(body, "crc32");
    const char *md5 = JGetString(body, "md5");
    if (md5) {
      strlcpy(status.firmwareMd5, md5, sizeof(status.firmwareMd5));
    }
    // Capture the update filename from whichever key the Notecard provides. Different
    // Notecard firmware revisions expose it as "name", "file", or "source"; we need at
    // least one to recover the version below when no explicit "version" is present.
    const char *name = JGetString(body, "name");
    if (!name || name[0] == '\0') name = JGetString(body, "file");
    if (!name || name[0] == '\0') name = JGetString(body, "source");
    if (name && name[0] != '\0') {
      strlcpy(status.firmwareName, name, sizeof(status.firmwareName));
    }
    const char *source = JGetString(body, "source");
    if (source && source[0] != '\0') {
      strlcpy(status.firmwareSource, source, sizeof(status.firmwareSource));
    }
  }

  // Some Notecard firmware revisions place the update filename at the response root
  // rather than inside "body"; capture it as a fallback for version recovery.
  if (status.firmwareName[0] == '\0') {
    const char *rootFile = JGetString(rsp, "file");
    if (rootFile && rootFile[0] != '\0') {
      strlcpy(status.firmwareName, rootFile, sizeof(status.firmwareName));
    }
  }

  // Determine state
  if (mode) {
    if (strcmp(mode, "ready") == 0) {
      status.updateAvailable = true;
    } else if (strcmp(mode, "downloading") == 0 || strcmp(mode, "download-pending") == 0) {
      status.downloading = true;
    } else if (strcmp(mode, "error") == 0) {
      status.error = true;
    }
  }

  // Recover the version when an update is READY but the Notecard reported no top-level
  // "version" (our host .slot.bin embeds no Blues firmware metadata, so dfu.status
  // returns an empty version). Derive it from the update filename so the host apply
  // path — which requires a non-empty version — does not silently ignore a ready OTA.
  if (status.updateAvailable && status.version[0] == '\0') {
    if (tankalarm_extractVersionFromFilename(status.firmwareName, status.version, sizeof(status.version)) ||
        tankalarm_extractVersionFromFilename(status.firmwareSource, status.version, sizeof(status.version))) {
      Serial.print(F("DFU: recovered update version from filename: "));
      Serial.println(status.version);
    }
  }

  notecard.deleteResponse(rsp);
  return true;
}

// ============================================================================
// IAP DFU Apply — reads firmware from Notecard and writes to flash
// ============================================================================

// Only available on Opta/Mbed platforms with FlashIAP
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
#include <FlashIAP.h>

/**
 * Simple base64 decode (RFC 4648).
 * Decodes in-place or to a separate output buffer.
 *
 * @param dst     Output buffer for decoded bytes
 * @param src     Null-terminated base64 input string
 * @param dstLen  Size of dst buffer
 * @return Number of bytes written to dst, or -1 on error
 */
static int tankalarm_b64decode(uint8_t *dst, const char *src, size_t dstLen) {
  static const int8_t LUT[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };

  size_t out = 0;
  uint32_t accum = 0;
  int bits = 0;

  for (const char *p = src; *p && *p != '='; p++) {
    int8_t val = LUT[(uint8_t)*p];
    if (val < 0) continue;  // Skip whitespace/invalid
    accum = (accum << 6) | (uint32_t)val;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (out >= dstLen) return -1;
      dst[out++] = (uint8_t)(accum >> bits) & 0xFF;
    }
  }
  return (int)out;
}

/**
 * CRC-32 (ISO 3309 / ITU-T V.42 / zlib polynomial 0xEDB88320).
 * Computes incrementally: pass previous crc to continue, or 0xFFFFFFFF to start.
 * Final result must be XORed with 0xFFFFFFFF (done internally when starting from ~0).
 */
static uint32_t tankalarm_crc32(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFF) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return crc;
}

/**
 * Perform IAP firmware update: read chunks from Notecard, write to flash, reboot.
 *
 * This is a BLOCKING operation that takes several minutes for large firmware.
 * The watchdog must be kicked via the callback. The Notecard is put into DFU
 * mode (no sync) for the duration.
 *
 * CRC-32 integrity verification: a running CRC is accumulated during download,
 * then compared against a read-back CRC of the written flash region. On mismatch,
 * the update is aborted and the device continues normal operation.
 *
 * @param notecard       Reference to Notecard instance
 * @param firmwareLength Total firmware size in bytes (from dfu.status body)
 * @param hubMode        Hub mode to restore if update fails ("continuous" or "periodic")
 * @param kickWatchdog   Optional watchdog kick callback (called between chunks)
 * @return true if update succeeded (device will reboot), false on failure
 */
static bool tankalarm_performIapUpdate(
    Notecard &notecard,
    uint32_t firmwareLength,
    const char *hubMode,
    void (*kickWatchdog)() = nullptr
) {
  if (firmwareLength == 0 || firmwareLength > 1536 * 1024) {
    Serial.print(F("IAP DFU: Invalid firmware length: "));
    Serial.println(firmwareLength);
    return false;
  }

  Serial.println(F("========================================"));
  Serial.println(F("IAP DFU: Starting firmware update"));
  Serial.print(F("Firmware size: "));
  Serial.print(firmwareLength);
  Serial.println(F(" bytes"));
  Serial.println(F("========================================"));

  // --- Step 1: Put Notecard into DFU mode ---
  Serial.println(F("IAP DFU: Entering Notecard DFU mode..."));
  {
    J *req = notecard.newRequest("hub.set");
    if (!req) {
      Serial.println(F("IAP DFU: FAILED to create hub.set request"));
      return false;
    }
    JAddStringToObject(req, "mode", "dfu");
    if (kickWatchdog) kickWatchdog();
    J *rsp = notecard.requestAndResponse(req);
    if (kickWatchdog) kickWatchdog();
    if (rsp) {
      const char *err = JGetString(rsp, "err");
      if (err && err[0] != '\0') {
        Serial.print(F("IAP DFU: hub.set dfu error: "));
        Serial.println(err);
        notecard.deleteResponse(rsp);
        return false;
      }
      notecard.deleteResponse(rsp);
    } else {
      Serial.println(F("IAP DFU: hub.set dfu no response"));
      return false;
    }
  }

  // --- Step 2: Wait for Notecard DFU mode to be active ---
  Serial.println(F("IAP DFU: Waiting for DFU mode to activate..."));
  {
    unsigned long start = millis();
    bool dfuReady = false;
    while ((millis() - start) < DFU_IAP_MODE_TIMEOUT_MS) {
      if (kickWatchdog) kickWatchdog();
      delay(2000);

      J *req = notecard.newRequest("dfu.get");
      if (!req) continue;
      JAddNumberToObject(req, "length", 0);
      if (kickWatchdog) kickWatchdog();
      J *rsp = notecard.requestAndResponse(req);
      if (kickWatchdog) kickWatchdog();
      if (rsp) {
        const char *err = JGetString(rsp, "err");
        if (!err || err[0] == '\0') {
          dfuReady = true;
          notecard.deleteResponse(rsp);
          break;
        }
        notecard.deleteResponse(rsp);
      }
    }
    if (!dfuReady) {
      Serial.println(F("IAP DFU: Timeout waiting for DFU mode"));
      goto iap_restore_hub;
    }
  }
  Serial.println(F("IAP DFU: DFU mode active"));

  // --- Step 3: Initialize flash and erase application area ---
  {
    mbed::FlashIAP flash;
    int flashResult = flash.init();
    if (flashResult != 0) {
      Serial.print(F("IAP DFU: FlashIAP init failed: "));
      Serial.println(flashResult);
      goto iap_restore_hub;
    }

    uint32_t flashStart = flash.get_flash_start();
    uint32_t flashSize = flash.get_flash_size();
    uint32_t pageSize = flash.get_page_size();
    uint32_t sectorSize = flash.get_sector_size(flashStart);

    Serial.print(F("Flash: start=0x"));
    Serial.print(flashStart, HEX);
    Serial.print(F(" size="));
    Serial.print(flashSize / 1024);
    Serial.print(F("KB page="));
    Serial.print(pageSize);
    Serial.print(F(" sector="));
    Serial.println(sectorSize);

    // Application start address — after the Arduino bootloader.
    // On STM32H747 (Opta), the bootloader occupies the first 256KB (0x40000).
    // The application binary from Arduino IDE targets 0x08040000.
    uint32_t appStart = flashStart + 0x40000;

    // Sanity check: firmware must fit in remaining flash
    if (firmwareLength > (flashStart + flashSize - appStart)) {
      Serial.println(F("IAP DFU: Firmware too large for flash"));
      flash.deinit();
      goto iap_restore_hub;
    }

    // Erase the sectors covering the firmware area
    // Round up to sector boundary
    uint32_t eraseAddr = appStart;
    uint32_t eraseSizeNeeded = firmwareLength;
    // Align erase size to sector boundaries
    uint32_t firstSectorSize = flash.get_sector_size(eraseAddr);
    uint32_t eraseSize = 0;
    uint32_t addr = eraseAddr;
    while (eraseSize < eraseSizeNeeded) {
      uint32_t ss = flash.get_sector_size(addr);
      eraseSize += ss;
      addr += ss;
    }

    Serial.print(F("IAP DFU: Erasing "));
    Serial.print(eraseSize / 1024);
    Serial.print(F("KB at 0x"));
    Serial.println(eraseAddr, HEX);

    // Erase sector-by-sector, kicking the watchdog between each sector.
    // A single flash.erase() of the whole application region is one long
    // blocking call; on a brown-out-prone supply it can approach or exceed
    // the ~30s watchdog window and reset the MCU with the app region already
    // wiped (an unrecoverable brick for IAP). Per-sector erase keeps each
    // blocking interval short and lets the watchdog be serviced in between.
    flashResult = 0;
    {
      uint32_t eraseCursor = eraseAddr;
      uint32_t eraseEnd = eraseAddr + eraseSize;
      while (eraseCursor < eraseEnd) {
        if (kickWatchdog) kickWatchdog();
        uint32_t ss = flash.get_sector_size(eraseCursor);
        flashResult = flash.erase(eraseCursor, ss);
        if (flashResult != 0) {
          Serial.print(F("IAP DFU: Flash erase failed at 0x"));
          Serial.print(eraseCursor, HEX);
          Serial.print(F(": "));
          Serial.println(flashResult);
          break;
        }
        eraseCursor += ss;
      }
    }
    if (flashResult != 0) {
      flash.deinit();
      goto iap_restore_hub;
    }
    if (kickWatchdog) kickWatchdog();

    Serial.println(F("IAP DFU: Flash erased, reading firmware chunks..."));

    // --- Step 4: Read firmware chunks via dfu.get and program flash ---
    // Buffer must be page-aligned in size for FlashIAP.program()
    const uint32_t chunkSize = DFU_IAP_CHUNK_SIZE;
    // Page-align the program buffer size
    uint32_t alignedBufSize = ((chunkSize + pageSize - 1) / pageSize) * pageSize;
    uint8_t *progBuf = (uint8_t *)malloc(alignedBufSize);

    if (!progBuf) {
      Serial.println(F("IAP DFU: Failed to allocate program buffer"));
      free(progBuf);
      flash.deinit();
      goto iap_restore_hub;
    }

    uint32_t offset = 0;
    uint32_t lastProgressPct = 0;
    uint32_t downloadCrc = 0xFFFFFFFF;  // Running CRC over downloaded bytes

    while (offset < firmwareLength) {
      if (kickWatchdog) kickWatchdog();

      uint32_t remaining = firmwareLength - offset;
      uint32_t thisChunk = (remaining < chunkSize) ? remaining : chunkSize;

      // Retry loop for this chunk
      bool chunkOk = false;
      for (uint8_t retry = 0; retry < DFU_IAP_CHUNK_RETRIES; retry++) {
        J *req = notecard.newRequest("dfu.get");
        if (!req) {
          delay(500);
          if (kickWatchdog) kickWatchdog();
          continue;
        }
        JAddNumberToObject(req, "length", (int)thisChunk);
        if (offset > 0) {
          JAddNumberToObject(req, "offset", (int)offset);
        }

        if (kickWatchdog) kickWatchdog();
        J *rsp = notecard.requestAndResponse(req);
        if (kickWatchdog) kickWatchdog();
        if (!rsp) {
          Serial.print(F("IAP DFU: dfu.get no response at offset "));
          Serial.println(offset);
          delay(500);
          if (kickWatchdog) kickWatchdog();
          continue;
        }

        const char *err = JGetString(rsp, "err");
        if (err && err[0] != '\0') {
          Serial.print(F("IAP DFU: dfu.get error: "));
          Serial.println(err);
          notecard.deleteResponse(rsp);
          delay(500);
          if (kickWatchdog) kickWatchdog();
          continue;
        }

        const char *payload = JGetString(rsp, "payload");
        if (!payload || payload[0] == '\0') {
          Serial.println(F("IAP DFU: Empty payload"));
          notecard.deleteResponse(rsp);
          delay(500);
          if (kickWatchdog) kickWatchdog();
          continue;
        }

        // Decode base64 payload
        memset(progBuf, flash.get_erase_value(), alignedBufSize);
        int decoded = tankalarm_b64decode(progBuf, payload, alignedBufSize);
        notecard.deleteResponse(rsp);

        if (decoded <= 0) {
          Serial.println(F("IAP DFU: Base64 decode failed"));
          delay(500);
          if (kickWatchdog) kickWatchdog();
          continue;
        }

        // Bounds check: decoded bytes must not exceed requested chunk or remaining firmware
        if ((uint32_t)decoded > thisChunk || (uint32_t)decoded > remaining) {
          Serial.print(F("IAP DFU: Decoded size "));
          Serial.print(decoded);
          Serial.print(F(" exceeds expected "));
          Serial.print(thisChunk);
          Serial.println(F(" — aborting"));
          free(progBuf);
          flash.deinit();
          goto iap_restore_hub;
        }

        // Accumulate CRC over raw decoded bytes (not page-alignment padding)
        downloadCrc = tankalarm_crc32(progBuf, (size_t)decoded, downloadCrc);

        // Program flash — size must be page-aligned
        uint32_t programSize = ((uint32_t)decoded + pageSize - 1) / pageSize * pageSize;
        flashResult = flash.program(progBuf, appStart + offset, programSize);
        if (flashResult != 0) {
          Serial.print(F("IAP DFU: Flash program failed at 0x"));
          Serial.print(appStart + offset, HEX);
          Serial.print(F(": "));
          Serial.println(flashResult);
          // Flash programming failure is fatal — do NOT continue
          free(progBuf);
          flash.deinit();
          goto iap_restore_hub;
        }

        offset += (uint32_t)decoded;
        chunkOk = true;
        break;
      }

      if (!chunkOk) {
        Serial.print(F("IAP DFU: Failed to read chunk at offset "));
        Serial.print(offset);
        Serial.println(F(" after retries"));
        free(progBuf);
        flash.deinit();
        goto iap_restore_hub;
      }

      // Progress reporting (every 10%)
      uint32_t pct = (offset * 100) / firmwareLength;
      if (pct >= lastProgressPct + 10) {
        lastProgressPct = pct;
        Serial.print(F("IAP DFU: "));
        Serial.print(pct);
        Serial.print(F("% ("));
        Serial.print(offset);
        Serial.print(F("/"));
        Serial.print(firmwareLength);
        Serial.println(F(")"));
      }
    }

    free(progBuf);

    // Finalize download CRC
    downloadCrc ^= 0xFFFFFFFF;

    Serial.println(F("IAP DFU: Firmware written to flash, verifying CRC..."));

    // --- Step 4b: Read-back CRC verification ---
    // Re-use a small read buffer to compute CRC over the written flash region.
    // Reading directly from flash memory-mapped address (appStart) is safe on STM32.
    {
      const uint8_t *flashPtr = (const uint8_t *)appStart;
      uint32_t readbackCrc = 0xFFFFFFFF;
      const uint32_t readChunk = 4096;
      uint32_t remaining = firmwareLength;
      uint32_t pos = 0;

      while (remaining > 0) {
        if (kickWatchdog) kickWatchdog();
        uint32_t toRead = (remaining < readChunk) ? remaining : readChunk;
        readbackCrc = tankalarm_crc32(flashPtr + pos, toRead, readbackCrc);
        pos += toRead;
        remaining -= toRead;
      }
      readbackCrc ^= 0xFFFFFFFF;

      Serial.print(F("IAP DFU: Download CRC=0x"));
      Serial.print(downloadCrc, HEX);
      Serial.print(F("  Flash CRC=0x"));
      Serial.println(readbackCrc, HEX);

      if (downloadCrc != readbackCrc) {
        Serial.println(F("IAP DFU: *** CRC MISMATCH — firmware corrupted during flash write ***"));
        Serial.println(F("IAP DFU: Aborting update, device will NOT reboot"));
        flash.deinit();
        goto iap_restore_hub;
      }

      Serial.println(F("IAP DFU: CRC verified OK"));
    }

    flash.deinit();
  }

  // --- Step 5: Clear DFU state and report success ---
  {
    J *req = notecard.newRequest("dfu.status");
    if (req) {
      JAddBoolToObject(req, "stop", true);
      JAddStringToObject(req, "status", "firmware update successful");
      JAddStringToObject(req, "name", "user");
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }

  // --- Step 6: Restore hub mode ---
  {
    J *req = notecard.newRequest("hub.set");
    if (req) {
      JAddStringToObject(req, "mode", hubMode);
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }

  Serial.println(F("========================================"));
  Serial.println(F("IAP DFU: UPDATE COMPLETE — REBOOTING"));
  Serial.println(F("========================================"));
  Serial.flush();
  delay(500);

  NVIC_SystemReset();
  // Never reaches here
  return true;

iap_restore_hub:
  // Failure path: restore hub mode so device continues normal operation
  Serial.println(F("IAP DFU: FAILED — restoring normal operation"));
  {
    // Report failure to Notehub
    J *req = notecard.newRequest("dfu.status");
    if (req) {
      JAddBoolToObject(req, "stop", true);
      JAddStringToObject(req, "status", "firmware update failed");
      JAddStringToObject(req, "name", "user");
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }
  {
    J *req = notecard.newRequest("hub.set");
    if (req) {
      JAddStringToObject(req, "mode", hubMode);
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }
  return false;
}

#if defined(TANKALARM_DFU_MCUBOOT)
#include <MCUboot.h>
#include "MBRBlockDevice.h"
#include "FATFileSystem.h"
#include "TankAlarm_MCUbootConfig.h"

// ----------------------------------------------------------------------------
// Shared OTA filesystem access (single owner of the "fs_ota" prefix)
// ----------------------------------------------------------------------------
// CRITICAL: every OTA helper below must mount partition 2 through THIS single
// pair of static objects. Previous revisions declared a separate static
// QSPIFBlockDevice + MBRBlockDevice + FATFileSystem("fs_ota") inside each
// function. That was unsafe on Mbed OS for two reasons:
//   1. A second QSPIFBlockDevice on the same physical pins fights the singleton
//      the application already created via BlockDevice::get_default_instance()
//      (which is mapped to partition 4 for the LittleFS config store). Two
//      drivers asserting the same CS line corrupts driver state / faults the bus.
//   2. Multiple function-local static FATFileSystem objects all try to register
//      the same "fs_ota" mount prefix in Mbed's global filesystem table, which
//      collides and makes later mount() calls fail (-EINVAL/-ENODEV).
// Routing every helper through tankalarm_otaFsMount()/Unmount() guarantees a
// single QSPI driver (the default instance) and a single "fs_ota" registration.
static mbed::MBRBlockDevice *tankalarm_otaPart = nullptr;
static mbed::FATFileSystem  *tankalarm_otaFs   = nullptr;

// Mount /fs_ota (partition 2) using the application's default QSPI block device.
// Returns true on success. Safe to call repeatedly; the objects are created once.
static inline bool tankalarm_otaFsMount() {
  mbed::BlockDevice *root = mbed::BlockDevice::get_default_instance();
  if (!root) {
    Serial.println(F("MCUboot: No default QSPI block device for OTA."));
    return false;
  }
  if (!tankalarm_otaPart) {
    tankalarm_otaPart = new mbed::MBRBlockDevice(root, 2);
  }
  if (!tankalarm_otaFs) {
    tankalarm_otaFs = new mbed::FATFileSystem("fs_ota");
  }
  if (!tankalarm_otaPart || !tankalarm_otaFs) {
    return false;
  }
  // Defensive: release any prior mount first so a re-entry (e.g. after a failed
  // staging attempt that left the FS mounted) cannot fail with -EINVAL.
  tankalarm_otaFs->unmount();
  return (tankalarm_otaFs->mount(tankalarm_otaPart) == 0);
}

static inline void tankalarm_otaFsUnmount() {
  if (tankalarm_otaFs) {
    tankalarm_otaFs->unmount();
  }
}

// Boot-time OTA readiness self-check. Prints, over Serial, whether the QSPI OTA
// partition (p2) mounts and whether update.bin / scratch.bin exist at their
// expected sizes. This is the single most useful breadcrumb when diagnosing
// "USB works but OTA fails": if this reports the partition or files missing/
// wrong-size, the unit was never fully provisioned (run KeyProvisioning) — no
// OTA can ever stage. Returns true only if the partition is fully provisioned.
// Safe no-op on a build without MCUboot. Call once from setup() after storage init.
static inline bool tankalarm_otaSelfCheck() {
  Serial.println(F("---- OTA readiness self-check ----"));
  if (!tankalarm_otaFsMount()) {
    Serial.println(F("  QSPI p2 (fs_ota): MOUNT FAILED -> NOT provisioned. Run KeyProvisioning."));
    return false;
  }
  long upSize = -1, scSize = -1;
  FILE *fu = fopen("/fs_ota/update.bin", "rb");
  if (fu) { fseek(fu, 0, SEEK_END); upSize = ftell(fu); fclose(fu); }
  FILE *fs = fopen("/fs_ota/scratch.bin", "rb");
  if (fs) { fseek(fs, 0, SEEK_END); scSize = ftell(fs); fclose(fs); }
  tankalarm_otaFsUnmount();

  const long wantUp = (long)TANKALARM_MCUBOOT_SLOT_SIZE;     // 0x1E0000
  const long wantSc = (long)TANKALARM_MCUBOOT_SCRATCH_SIZE;  // 0x20000
  bool ok = (upSize == wantUp) && (scSize == wantSc);
  Serial.println(F("  QSPI p2 (fs_ota): mounted OK"));
  Serial.print(F("  update.bin:  "));  Serial.print(upSize);
  Serial.print(F(" (want "));          Serial.print(wantUp);  Serial.println(upSize == wantUp ? F(") OK") : F(") MISMATCH"));
  Serial.print(F("  scratch.bin: "));  Serial.print(scSize);
  Serial.print(F(" (want "));          Serial.print(wantSc);  Serial.println(scSize == wantSc ? F(") OK") : F(") MISMATCH"));
  Serial.println(ok ? F("  OTA partition: READY") : F("  OTA partition: NOT READY -> run KeyProvisioning"));
  Serial.println(F("----------------------------------"));
  return ok;
}

static inline uint32_t tankalarm_versionToSeq(const char *verStr) {
  if (!verStr || verStr[0] == '\0') return 0;
  // Tolerate a leading 'v'/'V' (Git tag / Notehub version conventions like "v1.9.33")
  // so a prefixed string is not silently parsed as 0 and rejected by the downgrade guard.
  const char *p = verStr;
  if (*p == 'v' || *p == 'V') p++;
  int major = 0, minor = 0, patch = 0;
  const int n = sscanf(p, "%d.%d.%d", &major, &minor, &patch);
  // Require all three components and reject negatives; a malformed string returns 0
  // (treated as "not newer" = safe refuse) instead of an undefined partial parse.
  if (n != 3 || major < 0 || minor < 0 || patch < 0) return 0;
  return (uint32_t)(major * 100 + minor * 10 + patch);
}

static inline void tankalarm_resolvePendingOta(Notecard &notecard) {
  if (!tankalarm_otaFsMount()) {
    Serial.println(F("MCUboot: No QSPI fs_ota available for OTA checking."));
    return;
  }

  FILE *fp = fopen("/fs_ota/pending_ota.json", "r");
  if (!fp) {
    tankalarm_otaFsUnmount();
    return;
  }

  // Read JSON content
  char buf[256];
  memset(buf, 0, sizeof(buf));
  size_t read_bytes = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);

  if (read_bytes == 0) {
    remove("/fs_ota/pending_ota.json");
    tankalarm_otaFsUnmount();
    return;
  }

  // Parse pending ota json: {"target_seq":191,"target_v":"1.9.1","status":"trial"}
  unsigned int target_seq = 0;
  char target_v[32];
  memset(target_v, 0, sizeof(target_v));
  char status[32];
  memset(status, 0, sizeof(status));

  int parsed = sscanf(buf, "{\"target_seq\":%u,\"target_v\":\"%31[^\"]\",\"status\":\"%31[^\"]\"}", 
                      &target_seq, target_v, status);

  if (parsed < 2) {
    parsed = sscanf(buf, "{\x22target_seq\x22:%u,\x22target_v\x22:\x22%31[^\x22]\x22,\x22status\x22:\x22%31[^\x22]\x22}",
                    &target_seq, target_v, status);
  }

  if (parsed >= 2) {
    if (strcmp(status, "trial") == 0) {
      if (FIRMWARE_BUILD_SEQ == target_seq) {
        Serial.print(F("MCUboot: Trial boot of version "));
        Serial.print(target_v);
        Serial.println(F(" succeeded! Health gate verification pending."));
        
        FILE *f_write = fopen("/fs_ota/pending_ota.json", "w");
        if (f_write) {
          fprintf(f_write, "{\"target_seq\":%u,\"target_v\":\"%s\",\"status\":\"confirmed\"}\n", 
                  (unsigned int)target_seq, target_v);
          fclose(f_write);
        }
      } else {
        // Rollback Detected!
        Serial.print(F("MCUboot: ROLLBACK DETECTED! Expected target build seq: "));
        Serial.print(target_seq);
        Serial.print(F(" ("));
        Serial.print(target_v);
        Serial.print(F("), but currently running: "));
        Serial.println(FIRMWARE_BUILD_SEQ);

        // Send rollback & stop failure status to Notehub, evicting the bad image from Notecard cache
        J *req = notecard.newRequest("dfu.status");
        if (req) {
          JAddBoolToObject(req, "stop", true);
          JAddStringToObject(req, "name", "user");  // target the host DFU channel explicitly (matches other stop calls)
          char err_buf[128];
          snprintf(err_buf, sizeof(err_buf), "rollback detected - trial crashed - reverted to %s", FIRMWARE_VERSION);
          JAddStringToObject(req, "status", err_buf);
          J *rsp = notecard.requestAndResponse(req);
          if (rsp) notecard.deleteResponse(rsp);
        }

        // Store version blacklist locally
        FILE *f_write = fopen("/fs_ota/pending_ota.json", "w");
        if (f_write) {
          fprintf(f_write, "{\"target_seq\":%u,\"target_v\":\"%s\",\"status\":\"failed_rollback\"}\n", 
                  (unsigned int)target_seq, target_v);
          fclose(f_write);
        }
      }
    } else if (strcmp(status, "failed_rollback") == 0) {
      Serial.print(F("MCUboot: Version "));
      Serial.print(target_v);
      Serial.println(F(" is locally blacklisted due to a previous rollback."));
    }
  }

  tankalarm_otaFsUnmount();
}

static inline bool tankalarm_isVersionBlacklisted(const char *version) {
  if (!version || version[0] == '\0') return false;

  if (!tankalarm_otaFsMount()) return false;

  FILE *fp = fopen("/fs_ota/pending_ota.json", "r");
  if (!fp) {
    tankalarm_otaFsUnmount();
    return false;
  }

  char buf[256];
  memset(buf, 0, sizeof(buf));
  size_t read_bytes = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  tankalarm_otaFsUnmount();

  if (read_bytes == 0) return false;

  unsigned int target_seq = 0;
  char target_v[32];
  memset(target_v, 0, sizeof(target_v));
  char status[32];
  memset(status, 0, sizeof(status));

  int parsed = sscanf(buf, "{\"target_seq\":%u,\"target_v\":\"%31[^\"]\",\"status\":\"%31[^\"]\"}", 
                      &target_seq, target_v, status);
  if (parsed < 2) {
    parsed = sscanf(buf, "{\x22target_seq\x22:%u,\x22target_v\x22:\x22%31[^\x22]\x22,\x22status\x22:\x22%31[^\x22]\x22}",
                    &target_seq, target_v, status);
  }

  if (parsed >= 3 && strcmp(status, "failed_rollback") == 0 && strcmp(target_v, version) == 0) {
    return true;
  }
  return false;
}

// ----------------------------------------------------------------------------
// OTA outcome reporting bridge (server visibility)
// ----------------------------------------------------------------------------
// The host normally only tells Notehub (dfu.status) about a rollback. These helpers let the
// application read the OTA outcome from pending_ota.json and forward it to the SERVER (over the
// normal config-ack notefile) so the dashboard can show stalled/failed/applied updates. A
// separate ota_reported.json dedupes so each outcome is reported once. pending_ota.json is never
// modified here, preserving the failed_rollback blacklist used by tankalarm_isVersionBlacklisted.

struct TankAlarmOtaReport {
  char status[16];   // "reverted" | "applied"
  char targetV[32];  // firmware version the OTA targeted
};

// Peek at any OTA outcome that has not yet been reported. Returns true and fills `out` if there
// is something new to report; does not modify any files.
static inline bool tankalarm_peekOtaReport(TankAlarmOtaReport &out) {
  out.status[0] = '\0';
  out.targetV[0] = '\0';

  if (!tankalarm_otaFsMount()) return false;

  FILE *fp = fopen("/fs_ota/pending_ota.json", "r");
  if (!fp) { tankalarm_otaFsUnmount(); return false; }
  char buf[256]; memset(buf, 0, sizeof(buf));
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  if (n == 0) { tankalarm_otaFsUnmount(); return false; }

  unsigned int target_seq = 0;
  char target_v[32]; memset(target_v, 0, sizeof(target_v));
  char status[32]; memset(status, 0, sizeof(status));
  int parsed = sscanf(buf, "{\"target_seq\":%u,\"target_v\":\"%31[^\"]\",\"status\":\"%31[^\"]\"}",
                      &target_seq, target_v, status);
  if (parsed < 3) { tankalarm_otaFsUnmount(); return false; }

  const char *reportStatus = nullptr;
  if (strcmp(status, "failed_rollback") == 0) reportStatus = "reverted";
  else if (strcmp(status, "confirmed") == 0) reportStatus = "applied";
  if (!reportStatus) { tankalarm_otaFsUnmount(); return false; }

  // Dedupe against the last reported outcome.
  FILE *rf = fopen("/fs_ota/ota_reported.json", "r");
  if (rf) {
    char rbuf[64]; memset(rbuf, 0, sizeof(rbuf));
    size_t rn = fread(rbuf, 1, sizeof(rbuf) - 1, rf);
    fclose(rf);
    if (rn > 0) {
      char lastV[32]; memset(lastV, 0, sizeof(lastV));
      char lastS[16]; memset(lastS, 0, sizeof(lastS));
      sscanf(rbuf, "{\"v\":\"%31[^\"]\",\"s\":\"%15[^\"]\"}", lastV, lastS);
      if (strcmp(lastV, target_v) == 0 && strcmp(lastS, reportStatus) == 0) {
        tankalarm_otaFsUnmount();
        return false;
      }
    }
  }

  tankalarm_otaFsUnmount();
  snprintf(out.status, sizeof(out.status), "%s", reportStatus);
  snprintf(out.targetV, sizeof(out.targetV), "%s", target_v);
  return true;
}

// Record that the given outcome has been reported, so it is not sent again.
static inline void tankalarm_markOtaReported(const char *targetV, const char *status) {
  if (!targetV || !status) return;
  if (!tankalarm_otaFsMount()) return;
  FILE *wf = fopen("/fs_ota/ota_reported.json", "w");
  if (wf) {
    fprintf(wf, "{\"v\":\"%s\",\"s\":\"%s\"}\n", targetV, status);
    fclose(wf);
  }
  tankalarm_otaFsUnmount();
}

static bool tankalarm_performMcubootUpdate(
    Notecard &notecard,
    const TankAlarmDfuStatus &dfu,   // length + crc32 + name/source for H2
    const char *hubMode,
    uint8_t deviceRole,              // DEVICE_ROLE — for the role/source check
    void (*kickWatchdog)() = nullptr
) {
  FILE* fp = nullptr;
  uint32_t firmwareLength = dfu.firmwareLength;

  if (firmwareLength == 0 || firmwareLength > TANKALARM_MCUBOOT_SLOT_SIZE) {
    Serial.print(F("MCUboot DFU: Invalid firmware length: "));
    Serial.println(firmwareLength);
    // Route through cleanup so the Notecard DFU is stopped (clears the stale
    // "ready" state) instead of looping on the same malformed image every check.
    goto mcuboot_restore_hub;
  }

  // --- H2: Role/source check ---
  // Ensure that if a firmware source is available, it matches our role token.
  if (dfu.firmwareSource[0] != '\0' && !strstr(dfu.firmwareSource, tankalarm_roleToken(deviceRole))) {
    Serial.print(F("MCUboot DFU: ERROR - firmware source \""));
    Serial.print(dfu.firmwareSource);
    Serial.print(F("\" does not match device role: "));
    Serial.println(tankalarm_roleToken(deviceRole));
    // Wrong-role image: stop it on the Notecard so it is not re-detected forever.
    goto mcuboot_restore_hub;
  }

  Serial.println(F("========================================"));
  Serial.println(F("MCUboot DFU: Starting firmware update"));
  Serial.print(F("Device Role: "));
  Serial.println(tankalarm_roleToken(deviceRole));
  Serial.print(F("Firmware size: "));
  Serial.print(firmwareLength);
  Serial.println(F(" bytes"));
  Serial.println(F("========================================"));

  // --- Step 1: Put Notecard into DFU mode ---
  Serial.println(F("MCUboot DFU: Entering Notecard DFU mode..."));
  {
    J *req = notecard.newRequest("hub.set");
    if (!req) goto mcuboot_restore_hub;
    JAddStringToObject(req, "mode", "dfu");
    if (kickWatchdog) kickWatchdog();
    J *rsp = notecard.requestAndResponse(req);
    if (kickWatchdog) kickWatchdog();
    if (rsp) {
      notecard.deleteResponse(rsp);
    } else {
      // Lost response: the Notecard may have entered DFU mode (which suspends all
      // cellular sync). Restore hub mode via cleanup instead of returning so the
      // Notecard is not silently stranded offline until the 4h modem-stall timer.
      goto mcuboot_restore_hub;
    }
  }

  // --- Step 2: Wait for Notecard DFU mode ---
  Serial.println(F("MCUboot DFU: Waiting for DFU mode to activate..."));
  {
    unsigned long start = millis();
    bool dfuReady = false;
    while ((millis() - start) < DFU_IAP_MODE_TIMEOUT_MS) {
      if (kickWatchdog) kickWatchdog();
      delay(2000);

      J *req = notecard.newRequest("dfu.get");
      if (!req) continue;
      JAddNumberToObject(req, "length", 0);
      if (kickWatchdog) kickWatchdog();
      J *rsp = notecard.requestAndResponse(req);
      if (kickWatchdog) kickWatchdog();
      if (rsp) {
        const char *err = JGetString(rsp, "err");
        if (!err || err[0] == '\0') {
          dfuReady = true;
          notecard.deleteResponse(rsp);
          break;
        }
        notecard.deleteResponse(rsp);
      }
    }
    if (!dfuReady) {
      goto mcuboot_restore_hub;
    }
  }
  Serial.println(F("MCUboot DFU: DFU mode active"));

  // --- Step 3: Mount FAT filesystem and open update file ---
  {
    if (!tankalarm_otaFsMount()) {
      Serial.println(F("MCUboot DFU: Failed to mount MBR2 FAT filesystem. Run Provisioning Sketch."));
      goto mcuboot_restore_hub;
    }
    
    // --- 11.6: Fail-closed opening behaviour ---
    fp = fopen("/fs_ota/update.bin", "r+b");
    if (!fp) {
#if defined(TANKALARM_MCUBOOT_ALLOW_RECREATE_UPDATE_FILE)
      Serial.println(F("[WARNING] QSPI file allocation missing! Re-creating space, fragment risks alert. Run KeyProvisioning to restore contiguous sectors."));
      fp = fopen("/fs_ota/update.bin", "wb");
#else
      Serial.println(F("MCUboot DFU: ERROR - /fs_ota/update.bin missing or failed to open in r+b mode (QSPI partition not provisioned; run KeyProvisioning). Aborting."));
      goto mcuboot_restore_hub;
#endif
    }
  }

  if (!fp) {
    goto mcuboot_restore_hub;
  }

  Serial.println(F("MCUboot DFU: File opened, reading firmware chunks..."));

  // --- Step 4: Stream Notecard data to file (Unpadded) ---
  {
    const uint32_t chunkSize = DFU_IAP_CHUNK_SIZE;
    uint8_t *progBuf = (uint8_t *)malloc(chunkSize + 16);
    if (!progBuf) {
      fclose(fp);
      fp = nullptr;
      goto mcuboot_restore_hub;
    }

    uint32_t offset = 0;
    uint32_t downloadCrc = 0xFFFFFFFF;

    while (offset < firmwareLength) {
      if (kickWatchdog) kickWatchdog();

      uint32_t remaining = firmwareLength - offset;
      uint32_t thisChunk = (remaining < chunkSize) ? remaining : chunkSize;
      bool chunkOk = false;

      for (uint8_t retry = 0; retry < DFU_IAP_CHUNK_RETRIES; retry++) {
        J *req = notecard.newRequest("dfu.get");
        if (!req) { delay(500); if (kickWatchdog) kickWatchdog(); continue; }
        
        JAddNumberToObject(req, "length", (int)thisChunk);
        if (offset > 0) {
          JAddNumberToObject(req, "offset", (int)offset);
        }

        if (kickWatchdog) kickWatchdog();
        J *rsp = notecard.requestAndResponse(req);
        if (kickWatchdog) kickWatchdog();
        
        if (!rsp) { delay(500); if (kickWatchdog) kickWatchdog(); continue; }

        const char *err = JGetString(rsp, "err");
        if (err && err[0] != '\0') {
          notecard.deleteResponse(rsp);
          delay(500); if (kickWatchdog) kickWatchdog(); continue;
        }

        const char *payload = JGetString(rsp, "payload");
        if (!payload || payload[0] == '\0') {
          notecard.deleteResponse(rsp);
          delay(500); if (kickWatchdog) kickWatchdog(); continue;
        }

        memset(progBuf, 0, chunkSize + 16);
        int decoded = tankalarm_b64decode(progBuf, payload, chunkSize + 16);
        notecard.deleteResponse(rsp);

        if (decoded <= 0) {
          delay(500); if (kickWatchdog) kickWatchdog(); continue;
        }

        // Bounds check
        if ((uint32_t)decoded > thisChunk || (uint32_t)decoded > remaining) {
          free(progBuf);
          fclose(fp);
          fp = nullptr;
          goto mcuboot_restore_hub;
        }

        // --- H1 / First-chunk verification ---
        // Verify MCUboot magic 0x96f3b83d
        if (offset == 0 && decoded >= 4) {
          uint32_t magic = 0;
          memcpy(&magic, progBuf, 4);
          if (magic != 0x96f3b83d) {
            Serial.print(F("MCUboot DFU: ERROR - Invalid MCUboot magic: 0x"));
            Serial.println(magic, HEX);
            free(progBuf);
            fclose(fp);
            fp = nullptr;
            goto mcuboot_restore_hub;
          }
          Serial.println(F("MCUboot DFU: MCUboot magic verified."));
        }

        // Write to QSPI fat file
        fseek(fp, offset, SEEK_SET);
        if (fwrite(progBuf, 1, decoded, fp) != (size_t)decoded) {
          free(progBuf);
          fclose(fp);
          fp = nullptr;
          goto mcuboot_restore_hub;
        }

        downloadCrc = tankalarm_crc32(progBuf, (size_t)decoded, downloadCrc);
        offset += (uint32_t)decoded;
        chunkOk = true;
        break;
      }

      if (!chunkOk) {
        free(progBuf);
        fclose(fp);
        fp = nullptr;
        goto mcuboot_restore_hub;
      }
    }
    
    // --- Step 4b: Programmatically format the remainder of the slot ---
    // If the previous firmware was larger, remaining bytes might not be 0xFF.
    // Fill the remainder of the slot to ensure MCUboot sees a clean end.
    if (offset < TANKALARM_MCUBOOT_SLOT_SIZE) {
      Serial.println(F("MCUboot DFU: Padding remaining slot with 0xFF..."));
      memset(progBuf, 0xFF, chunkSize);
      while(offset < TANKALARM_MCUBOOT_SLOT_SIZE) {
        if (kickWatchdog) kickWatchdog();
        uint32_t padAmt = (TANKALARM_MCUBOOT_SLOT_SIZE - offset > chunkSize) ? chunkSize : (TANKALARM_MCUBOOT_SLOT_SIZE - offset);
        if(fwrite(progBuf, 1, padAmt, fp) != padAmt) {
          Serial.println(F("MCUboot DFU: ERROR - padding write failed. Fail-closed."));
          free(progBuf);
          fclose(fp);
          fp = nullptr;
          goto mcuboot_restore_hub;
        }
        offset += padAmt;
      }
    }

    free(progBuf);
    fflush(fp);
    fclose(fp);
    fp = nullptr;
    downloadCrc ^= 0xFFFFFFFF;
    
    // --- Step 4c: Verify integrity with CRC32 ---
    if (dfu.firmwareCrc32 != 0 && downloadCrc != dfu.firmwareCrc32) {
      Serial.print(F("MCUboot DFU: ERROR - CRC32 link integrity mismatch! Staged: 0x"));
      Serial.print(downloadCrc, HEX);
      Serial.print(F(", Expected/CRC in body: 0x"));
      Serial.println(dfu.firmwareCrc32, HEX);
      goto mcuboot_restore_hub;
    }
    
    Serial.println(F("MCUboot DFU: Firmware written, padded and verified."));
  }

  // --- Step 4d: Persistent Trial state file pending_ota.json ---
  {
    FILE *f_pending = fopen("/fs_ota/pending_ota.json", "w");
    if (f_pending) {
      uint32_t target_seq = tankalarm_versionToSeq(dfu.version);
      fprintf(f_pending, "{\"target_seq\":%u,\"target_v\":\"%s\",\"status\":\"trial\"}\n", 
              (unsigned int)target_seq, dfu.version);
      fclose(f_pending);
      Serial.println(F("MCUboot DFU: Written pending_ota.json for trial boot."));
    } else {
      Serial.println(F("MCUboot DFU: ERROR - failed to write pending_ota.json. Aborting update."));
      goto mcuboot_restore_hub;
    }
  }

  // --- Step 5: Inform Notehub ---
  {
    J *req = notecard.newRequest("dfu.status");
    if (req) {
      JAddBoolToObject(req, "stop", true);
      JAddStringToObject(req, "status", "staged for MCUboot");
      JAddStringToObject(req, "name", "user");
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }

  {
    J *req = notecard.newRequest("hub.set");
    if (req) {
      JAddStringToObject(req, "mode", hubMode);
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }

  Serial.println(F("========================================"));
  Serial.println(F("MCUboot DFU: STAGED * TRIGGERING SWAP"));
  Serial.println(F("========================================"));
  Serial.flush();
  delay(500);

  // Set the MCUboot trailer so the bootloader tests the firmware on next boot
  MCUboot::applyUpdate(false);
  delay(500);

  // Kick watchdog one last time before rebooting to maximize our ~30s window (13.6 option 1)
  if (kickWatchdog) kickWatchdog();

  NVIC_SystemReset();
  return true;

mcuboot_restore_hub:
  Serial.println(F("MCUboot DFU: FAILED * restoring normal operation"));
  if (fp) {
    fclose(fp);
    fp = nullptr;
  }
  tankalarm_otaFsUnmount();
  {
    J *req = notecard.newRequest("dfu.status");
    if (req) {
      JAddBoolToObject(req, "stop", true);
      JAddStringToObject(req, "status", "firmware update failed");
      JAddStringToObject(req, "name", "user");
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }
  {
    J *req = notecard.newRequest("hub.set");
    if (req) {
      JAddStringToObject(req, "mode", hubMode);
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }
  return false;
}

static inline void tankalarm_markFirmwareHealthy() {
  static bool confirmed = false;
  if (!confirmed) {
    MCUboot::confirmSketch();
    confirmed = true;
    Serial.println(F("MCUboot: firmware confirmed healthy."));
  }
}
#endif // TANKALARM_DFU_MCUBOOT

#endif // ARDUINO_OPTA || ARDUINO_ARCH_MBED

#endif // TANKALARM_DFU_H
