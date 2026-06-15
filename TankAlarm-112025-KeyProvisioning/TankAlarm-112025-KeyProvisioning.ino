
/**
 * TankAlarm-112025-KeyProvisioning.ino
 * 
 * MCUboot keys in this repository are the public Arduino default keys. 
 * They exist solely so the Arduino MCUboot bootloader will accept our images 
 * and so we get mechanical benefits: image integrity (hash/magic) checks 
 * and A/B rollback on a bad boot. They provide no firmware authenticity: 
 * anyone can sign an image the bootloader accepts. This is an accepted 
 * trade-off — device firmware confidentiality/authenticity is explicitly out 
 * of scope for this product. Do not describe these signatures as a security control.
 */

#include "FlashIAP.h"
#include "QSPIFBlockDevice.h"
#include "MBRBlockDevice.h"
#include "FATFileSystem.h"

// MCUboot OTA slot geometry (must match TankAlarm_MCUbootConfig.h used by the
// Client/Server/Viewer updater). Kept as local literals so this provisioning
// sketch stays standalone (no Common library on its include path).
#define TANKALARM_PROV_UPDATE_FILE_SIZE   (15 * 128 * 1024)  // 0x1E0000 = 1,966,080 bytes
#define TANKALARM_PROV_SCRATCH_FILE_SIZE  (128 * 1024)       // 0x20000  =   131,072 bytes

// Include standard Arduino default keys to enable basic A/B rollback
#include "ecdsa-p256-encrypt-key.h"
#include "ecdsa-p256-signing-key.h"

#ifndef CORE_CM7
  #error Update the bootloader by uploading the sketch to the M7 core instead of the M4 core.
#endif

#define BOOTLOADER_ADDR   (0x8000000)
#define SIGNING_KEY_ADDR  (0x8000300)
#define ENCRYPT_KEY_ADDR  (0x8000400)
#define ENCRYPT_KEY_SIZE  (0x0000100)
#define SIGNING_KEY_SIZE  (0x0000100)

mbed::FlashIAP flash;
QSPIFBlockDevice root(QSPI_SO0, QSPI_SO1, QSPI_SO2, QSPI_SO3,  QSPI_SCK, QSPI_CS, QSPIF_POLARITY_MODE_1, 40000000);

bool writeKeys = false;
uint32_t bootloader_data_offset = 0x1F000;
uint8_t* bootloader_data = (uint8_t*)(BOOTLOADER_ADDR + bootloader_data_offset);

uint32_t bootloader_identification_offset = 0x2F0;
uint8_t* bootloader_identification = (uint8_t*)(BOOTLOADER_ADDR + bootloader_identification_offset);

void printProgress(uint32_t offset, uint32_t size, uint32_t threshold, bool reset) {
  static int percent_done = 0;
  if (reset == true) {
    percent_done = 0;
    Serial.println("Flashed " + String(percent_done) + "%");
  } else {
    uint32_t percent_done_new = offset * 100 / size;
    if (percent_done_new >= percent_done + threshold) {
      percent_done = percent_done_new;
      Serial.println("Flashed " + String(percent_done) + "%");
    }
  }
}

// Return the size of a file in bytes, or -1 if it cannot be opened.
long tankalarm_provFileSize(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fclose(f);
  return size;
}

// Provision (or verify) the MCUboot OTA staging partition (MBR partition 2).
// Returns true ONLY if partition 2 is FAT-formatted and both update.bin and
// scratch.bin exist at their exact expected sizes. Returns false on ANY failure
// so the caller never reports a half-provisioned board as healthy.
bool setupMCUBootOTAData() {
  // 1) Fast path: partition 2 is only "healthy" if update.bin + scratch.bin
  //    exist AT THEIR EXACT EXPECTED SIZES. A truncated/0-byte file from an
  //    aborted prior run opens fine but is NOT usable for staging, so a plain
  //    fopen()!=NULL check would falsely skip the re-format (the H2/§13.2 bug).
  {
    mbed::MBRBlockDevice ota_data(&root, 2);
    mbed::FATFileSystem ota_data_fs("fs_ota");
    if (ota_data_fs.mount(&ota_data) == 0) {
      long upSize = tankalarm_provFileSize("/fs_ota/update.bin");
      long scSize = tankalarm_provFileSize("/fs_ota/scratch.bin");
      bool healthy = (upSize == (long)TANKALARM_PROV_UPDATE_FILE_SIZE) &&
                     (scSize == (long)TANKALARM_PROV_SCRATCH_FILE_SIZE);
      ota_data_fs.unmount();
      if (healthy) {
        Serial.println("QSPI partition already provisioned and healthy. Skipping format.");
        Serial.print("  update.bin size: ");  Serial.println(upSize);
        Serial.print("  scratch.bin size: "); Serial.println(scSize);
        return true;
      }
      Serial.println("QSPI partition present but files missing/wrong size. Re-provisioning...");
      Serial.print("  update.bin size: ");  Serial.print(upSize);
      Serial.print(" (expected "); Serial.print((long)TANKALARM_PROV_UPDATE_FILE_SIZE); Serial.println(")");
      Serial.print("  scratch.bin size: "); Serial.print(scSize);
      Serial.print(" (expected "); Serial.print((long)TANKALARM_PROV_SCRATCH_FILE_SIZE); Serial.println(")");
    }
  }


  // 2) Create the standard Arduino Opta MBR partition table.
  //    Mirrors STM32H747_System/examples/QSPIFormat so the OTA region lands on
  //    partition 2 (where MCUboot and the updater expect it):
  //      p1 0..1 MB   WiFi firmware/certs (unused by TankAlarm)
  //      p2 1..6 MB   MCUboot OTA staging (update.bin + scratch.bin)
  //      p3 6..7 MB   Provisioning KVStore (unused by TankAlarm)
  //      p4 7..14 MB  User data (application LittleFS config store)
  Serial.println("\nCreating MBR partition table (p1 WiFi, p2 OTA, p3 KVStore, p4 user)...");
  if (root.init() != 0) {
    Serial.println("Error: QSPI init failed. Cannot create partitions.");
    return false;
  }
  // Wipe the MBR sector so the partition table is written cleanly.
  root.erase(0x0, root.get_erase_size());
  int e1 = mbed::MBRBlockDevice::partition(&root, 1, 0x0B, 0,                1 * 1024 * 1024);
  int e2 = mbed::MBRBlockDevice::partition(&root, 2, 0x0B, 1 * 1024 * 1024,  6 * 1024 * 1024);
  int e3 = mbed::MBRBlockDevice::partition(&root, 3, 0x0B, 6 * 1024 * 1024,  7 * 1024 * 1024);
  int e4 = mbed::MBRBlockDevice::partition(&root, 4, 0x0B, 7 * 1024 * 1024, 14 * 1024 * 1024);
  if (e1 || e2 || e3 || e4) {
    Serial.println("Error creating MBR partitions! Aborting.");
    return false;
  }
  Serial.println("MBR partition table created.");

  // 3) Format partition 2 (OTA) as FAT.
  mbed::MBRBlockDevice ota_data(&root, 2);
  mbed::FATFileSystem ota_data_fs("fs_ota");
  int err = ota_data_fs.reformat(&ota_data);
  if (err) {
    Serial.println("Error creating MCUboot FAT partition on p2! Aborting.");
    return false;
  }
  Serial.println("FAT Partition MBR2 reformatted successfully.");

  // Pre-allocate scratch and update bin files with 0xFF bytes to save flash wear on OTA downloads
  FILE* fp = fopen("/fs_ota/scratch.bin", "wb");
  if (!fp) {
    Serial.println("Error: Failed to open /fs_ota/scratch.bin for writing!");
    ota_data_fs.unmount();
    return false;
  }
  const int scratch_file_size = TANKALARM_PROV_SCRATCH_FILE_SIZE;
  uint8_t buffer[128];
  memset(buffer, 0xFF, sizeof(buffer));
  int size = 0;
  bool writeOk = true;

  Serial.println("\nAllocating scratch file");
  printProgress(size, scratch_file_size, 10, true);
  while (size < scratch_file_size) {
    int ret = fwrite(buffer, sizeof(buffer), 1, fp);
    if (ret != 1) {
      Serial.println("Error writing scratch file");
      writeOk = false;
      break;
    }
    size += sizeof(buffer);
    printProgress(size, scratch_file_size, 10, false);
  }
  fclose(fp);
  if (!writeOk) {
    Serial.println("FATAL: scratch.bin allocation failed. Aborting.");
    ota_data_fs.unmount();
    return false;
  }

  fp = fopen("/fs_ota/update.bin", "wb");
  if (!fp) {
    Serial.println("Error: Failed to open /fs_ota/update.bin for writing!");
    ota_data_fs.unmount();
    return false;
  }
  const int update_file_size = TANKALARM_PROV_UPDATE_FILE_SIZE; // 1.92 MB
  size = 0;

  Serial.println("\nAllocating update file (1.92 MB)");
  printProgress(size, update_file_size, 10, true);
  while (size < update_file_size) {
    int ret = fwrite(buffer, sizeof(buffer), 1, fp);
    if (ret != 1) {
      Serial.println("Error writing update file");
      writeOk = false;
      break;
    }
    size += sizeof(buffer);
    printProgress(size, update_file_size, 5, false);
  }
  fclose(fp);
  if (!writeOk) {
    Serial.println("FATAL: update.bin allocation failed. Aborting.");
    ota_data_fs.unmount();
    return false;
  }

  // Verify both files landed at EXACTLY the expected size before declaring success.
  long upCheck = tankalarm_provFileSize("/fs_ota/update.bin");
  long scCheck = tankalarm_provFileSize("/fs_ota/scratch.bin");
  ota_data_fs.unmount();
  if (upCheck != (long)update_file_size || scCheck != (long)scratch_file_size) {
    Serial.println("FATAL: post-write size verification failed.");
    Serial.print("  update.bin: ");  Serial.print(upCheck);  Serial.print(" expected "); Serial.println((long)update_file_size);
    Serial.print("  scratch.bin: "); Serial.print(scCheck);  Serial.print(" expected "); Serial.println((long)scratch_file_size);
    return false;
  }

  Serial.println("\nMCUboot QSPI Data ready.");
  return true;
}

void applyUpdate() {
  flash.init();
  bool otaOk = setupMCUBootOTAData();
  flash.program(&enc_priv_key, ENCRYPT_KEY_ADDR, ENCRYPT_KEY_SIZE);
  flash.program(&ecdsa_pub_key, SIGNING_KEY_ADDR, SIGNING_KEY_SIZE);
  flash.deinit();

  // Print an explicit, per-component provisioning summary so an operator can
  // never mistake a half-provisioned board for a healthy one (the §11.3 bug).
  Serial.println("\n================ PROVISIONING SUMMARY ================");
  Serial.println("  MCUboot keys programmed:        yes");
  Serial.print("  QSPI OTA partition (p2) ready:  "); Serial.println(otaOk ? "yes" : "NO");
  Serial.print("  update.bin (0x1E0000) allocated:"); Serial.println(otaOk ? " yes" : " NO");
  Serial.print("  scratch.bin (0x20000) allocated:"); Serial.println(otaOk ? " yes" : " NO");
  if (otaOk) {
    Serial.println("  RESULT: System provisioned for MCUboot OTA.");
  } else {
    Serial.println("  RESULT: *** OTA PROVISIONING INCOMPLETE - DO NOT DEPLOY ***");
    Serial.println("  Keys are written but the OTA partition is not usable.");
    Serial.println("  Re-run this sketch; if it keeps failing, the QSPI may need a full erase.");
  }
  Serial.println("======================================================");
}

bool waitResponse() {
  bool confirmation = false;
  while (confirmation == false) {
    if (Serial.available()) {
      char choice = Serial.read();
      switch (choice) {
        case 'y':
        case 'Y':
          return true;
        case 'n':
        case 'N':
          return false;
      }
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  uint8_t currentBootloaderVersion = bootloader_data[1];
  String currentBootloaderIdentifier = String(bootloader_identification, 15);

  if((currentBootloaderVersion > 24) && (currentBootloaderIdentifier.equals("MCUboot Arduino"))) {
    Serial.println("=========================================");
    Serial.println("PUBLIC PROVISIONING TOOL: MCUBoot USB INIT");
    Serial.println("=========================================");
    Serial.println("This sketch will burn the baseline Arduino Public Signing and Private Encrypt keys for simple Auth-Rollback support without secrets.");
    Serial.println("WARNING: Future sketches MUST be uploaded with Security Settings -> Signing + Encryption using Zephyr imgtool.");
    Serial.println("WARNING: This will DESTROY all current configuration on the QSPI disk.");
    Serial.println("Do you want to proceed and load the default keys? Y/[n]");
    
    if (waitResponse()) {
      Serial.println("\nKeys will be loaded, and QSPI will be formatted to FAT/MBR2 for OTA updates...");
      writeKeys = true;
    } else {
      Serial.println("\nAborted.");
    }
  } else {
    Serial.println("FATAL ERROR: Security features are not available for this bootloader version.");
    Serial.println("Please update it using STM32H747_manageBootloader sketch first.");
    while(true){ delay(1000); }
  }
  
  if (writeKeys) {
    applyUpdate();
    Serial.println("System provisioned. It's now safe to reboot or disconnect your board.");
  }
}

void loop() {
  delay(1000);
}
