
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

void setupMCUBootOTAData() {
  // 1) Fast path: if partition 2 already holds update.bin + scratch.bin, skip
  //    re-formatting to reduce flash wear.
  {
    mbed::MBRBlockDevice ota_data(&root, 2);
    mbed::FATFileSystem ota_data_fs("fs_ota");
    if (ota_data_fs.mount(&ota_data) == 0) {
      FILE* f_up = fopen("/fs_ota/update.bin", "rb");
      FILE* f_sc = fopen("/fs_ota/scratch.bin", "rb");
      bool healthy = (f_up != NULL) && (f_sc != NULL);
      if (f_up) fclose(f_up);
      if (f_sc) fclose(f_sc);
      ota_data_fs.unmount();
      if (healthy) {
        Serial.println("QSPI partition already provisioned and healthy. Skipping format.");
        return;
      }
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
    return;
  }
  // Wipe the MBR sector so the partition table is written cleanly.
  root.erase(0x0, root.get_erase_size());
  int e1 = mbed::MBRBlockDevice::partition(&root, 1, 0x0B, 0,                1 * 1024 * 1024);
  int e2 = mbed::MBRBlockDevice::partition(&root, 2, 0x0B, 1 * 1024 * 1024,  6 * 1024 * 1024);
  int e3 = mbed::MBRBlockDevice::partition(&root, 3, 0x0B, 6 * 1024 * 1024,  7 * 1024 * 1024);
  int e4 = mbed::MBRBlockDevice::partition(&root, 4, 0x0B, 7 * 1024 * 1024, 14 * 1024 * 1024);
  if (e1 || e2 || e3 || e4) {
    Serial.println("Error creating MBR partitions! Aborting.");
    return;
  }
  Serial.println("MBR partition table created.");

  // 3) Format partition 2 (OTA) as FAT.
  mbed::MBRBlockDevice ota_data(&root, 2);
  mbed::FATFileSystem ota_data_fs("fs_ota");
  int err = ota_data_fs.reformat(&ota_data);
  if (err) {
    Serial.println("Error creating MCUboot FAT partition on p2! Aborting.");
    return;
  }
  Serial.println("FAT Partition MBR2 reformatted successfully.");

  // Pre-allocate scratch and update bin files with 0xFF bytes to save flash wear on OTA downloads
  FILE* fp = fopen("/fs_ota/scratch.bin", "wb");
  if (!fp) {
    Serial.println("Error: Failed to open /fs_ota/scratch.bin for writing!");
    return;
  }
  const int scratch_file_size = 128 * 1024;
  uint8_t buffer[128];
  memset(buffer, 0xFF, sizeof(buffer));
  int size = 0;

  Serial.println("\nAllocating scratch file");
  printProgress(size, scratch_file_size, 10, true);
  while (size < scratch_file_size) {
    int ret = fwrite(buffer, sizeof(buffer), 1, fp);
    if (ret != 1) {
      Serial.println("Error writing scratch file");
      break;
    }
    size += sizeof(buffer);
    printProgress(size, scratch_file_size, 10, false);
  }
  fclose(fp);

  fp = fopen("/fs_ota/update.bin", "wb");
  if (!fp) {
    Serial.println("Error: Failed to open /fs_ota/update.bin for writing!");
    return;
  }
  const int update_file_size = 15 * 128 * 1024; // 1.92 MB
  size = 0;

  Serial.println("\nAllocating update file (1.92 MB)");
  printProgress(size, update_file_size, 10, true);
  while (size < update_file_size) {
    int ret = fwrite(buffer, sizeof(buffer), 1, fp);
    if (ret != 1) {
      Serial.println("Error writing update file");
      break;
    }
    size += sizeof(buffer);
    printProgress(size, update_file_size, 5, false);
  }

  fclose(fp);
  Serial.println("\nMCUboot QSPI Data ready.");
}

void applyUpdate() {
  flash.init();
  setupMCUBootOTAData();
  flash.program(&enc_priv_key, ENCRYPT_KEY_ADDR, ENCRYPT_KEY_SIZE);
  flash.program(&ecdsa_pub_key, SIGNING_KEY_ADDR, SIGNING_KEY_SIZE);
  flash.deinit();
  Serial.println("\nDefault Security Keys provisioned successfully.");
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
