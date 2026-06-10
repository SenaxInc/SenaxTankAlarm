// Tank Alarm Server 112025 - Arduino Opta + Blues Notecard
// Version: see FIRMWARE_VERSION in TankAlarm_Common.h
// NOTE: Save this file as UTF-8 without BOM to avoid stray character compile errors.
//
// Hardware:
// - Arduino Opta Lite (built-in Ethernet)
// - Blues Wireless Notecard for Opta adapter
//
// Features:
// - Aggregates telemetry from client nodes via Blues Notecard
// - Dispatches SMS alerts for alarm events
// - Sends daily email summary of sensor levels
// - Hosts lightweight intranet dashboard and REST API
// - Persists configuration to internal flash (LittleFS)
// - Allows remote client configuration updates via web UI
//
// Created: November 2025
// Using GitHub Copilot for code generation

#define DEVICE_ROLE TANKALARM_ROLE_SERVER

// Shared library - common constants and utilities
#include <TankAlarm_Common.h>

// Ensure FIRMWARE_BUILD_TIME is defined (in case library cache is stale)
#ifndef FIRMWARE_BUILD_TIME
#define FIRMWARE_BUILD_TIME __TIME__
#endif

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#if defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_H7_M7) || defined(ARDUINO_PORTENTA_H7_M4)
  #include <PortentaEthernet.h>
  #include <Ethernet.h>
#else
  #include <Ethernet.h>
#endif
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <FtpsClient.h>
#include <FtpsTypes.h>
#include <FtpsErrors.h>

// POSIX-compliant standard library headers
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <vector>

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  #include "netsocket/NetworkInterface.h"
  #include "netsocket/SocketAddress.h"
  #include "netsocket/TCPSocket.h"
  #include "netsocket/TLSSocketWrapper.h"
  #include "mbedtls/ssl.h"
  #include "mbedtls/sha256.h"
#endif

// POSIX file I/O types (for platforms that support it)
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  #include <fcntl.h>
  #include <sys/stat.h>
#endif

// Debug mode - controls Serial output and Notecard debug logging
// For PRODUCTION: Leave commented out (default) to save power consumption
// For DEVELOPMENT: Uncomment the line below for troubleshooting and monitoring
//#define DEBUG_MODE

// Filesystem support - Mbed OS filesystem instance
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  // Mbed OS filesystem instance - mounted at "/fs" for POSIX path compatibility
  static LittleFileSystem *mbedFS = nullptr;
  static BlockDevice *mbedBD = nullptr;
  static MbedWatchdogHelper mbedWatchdog;
  
  // POSIX-compatible file path prefix for Mbed OS VFS
  #define POSIX_FS_PREFIX "/fs"
  #define FILESYSTEM_AVAILABLE
  #define POSIX_FILE_IO_AVAILABLE
#elif defined(ARDUINO_ARCH_STM32)
  #include <LittleFS.h>
  #include <IWatchdog.h>
  #define FILESYSTEM_AVAILABLE
#endif

// Optional: Create a "ServerConfig.h" file in this sketch folder to set
// compile-time defaults (e.g. #define DEFAULT_SERVER_PRODUCT_UID "com.company.product:project").
// If the file does not exist, the product UID must be set via the Server Settings web page.
#if __has_include("ServerConfig.h")
  #include "ServerConfig.h"
#endif

#ifndef DEFAULT_SERVER_PRODUCT_UID
#define DEFAULT_SERVER_PRODUCT_UID ""  // Set via Server Settings web page
#endif

#ifndef SERVER_CONFIG_PATH
#define SERVER_CONFIG_PATH "/server_config.json"
#define CONTACTS_CONFIG_PATH "/contacts_config.json"
#endif

#ifndef SERVER_HEARTBEAT_PATH
#define SERVER_HEARTBEAT_PATH "/server_heartbeat.json"
#endif

#ifndef SERVER_HEARTBEAT_INTERVAL_SECONDS
#define SERVER_HEARTBEAT_INTERVAL_SECONDS 3600UL  // Persist heartbeat once per hour
#endif

#ifndef SERVER_DOWN_THRESHOLD_SECONDS
#define SERVER_DOWN_THRESHOLD_SECONDS 86400UL  // 24 hours
#endif

// Email buffer must accommodate all sensors. Per-sensor JSON: ~230 bytes worst-case
// (48 clientUid + 32 site + 24 label + 24 alarmType + keys/floats ~100)
// 32 sensors * 230 = 7360 + overhead. Using 16KB for ~70 sensors capacity with margin.
#ifndef MAX_EMAIL_BUFFER
#define MAX_EMAIL_BUFFER 16384
#endif

#ifndef DAILY_EMAIL_HOUR_DEFAULT
#define DAILY_EMAIL_HOUR_DEFAULT 6
#endif

#ifndef DAILY_EMAIL_MINUTE_DEFAULT
#define DAILY_EMAIL_MINUTE_DEFAULT 0
#endif

#ifndef MAX_SMS_ALERTS_PER_HOUR
#define MAX_SMS_ALERTS_PER_HOUR 2  // Maximum SMS alerts per sensor per hour
#endif

#ifndef MIN_SMS_ALERT_INTERVAL_SECONDS
#define MIN_SMS_ALERT_INTERVAL_SECONDS 300  // Minimum 5 minutes between SMS for same sensor
#endif

#ifndef CLIENT_CONFIG_CACHE_PATH
#define CLIENT_CONFIG_CACHE_PATH "/client_config_cache.txt"
#endif

#ifndef SENSOR_REGISTRY_PATH
#define SENSOR_REGISTRY_PATH "/sensor_registry.json"
#endif

#ifndef CLIENT_METADATA_CACHE_PATH
#define CLIENT_METADATA_CACHE_PATH "/client_metadata.json"
#endif

// Stale client alerting threshold (49 hours in seconds)
// The daily report is the de-facto heartbeat; 49h allows one missed
// daily before flagging stale, avoiding false positives.
#ifndef STALE_CLIENT_THRESHOLD_SECONDS
#define STALE_CLIENT_THRESHOLD_SECONDS 176400UL  // 49 hours
#endif

#ifndef STALE_CHECK_INTERVAL_MS
#define STALE_CHECK_INTERVAL_MS 3600000UL  // Check every hour
#endif

// Orphaned sensor auto-prune threshold (72 hours / 3 days)
// When a client has fresh AND stale sensors, sensors stale beyond this age
// are considered orphans from a reconfiguration and are auto-pruned.
#ifndef ORPHAN_SENSOR_PRUNE_SECONDS
#define ORPHAN_SENSOR_PRUNE_SECONDS 259200UL  // 72 hours (3 days)
#endif

// Auto-remove fully stale clients threshold (7 days)
// When ALL sensors for a client are stale beyond this threshold, the client's
// records are automatically removed. Prevents replaced/decommissioned devices
// from lingering indefinitely. Set to 0 to disable auto-removal.
#ifndef STALE_CLIENT_PRUNE_SECONDS
#define STALE_CLIENT_PRUNE_SECONDS 604800UL  // 7 days
#endif

// Minimum client age before archiving to FTP on removal (30 days)
// Clients active for less than this duration are not worth archiving.
#ifndef MIN_ARCHIVE_AGE_SECONDS
#define MIN_ARCHIVE_AGE_SECONDS 2592000UL  // 30 days
#endif

// Persistence save interval (avoid writing flash on every telemetry update)
#ifndef REGISTRY_SAVE_INTERVAL_MS
#define REGISTRY_SAVE_INTERVAL_MS 300000UL  // Save every 5 minutes if dirty
#endif

// CONFIG_ACK_INBOX_FILE is defined in TankAlarm_Common.h

#ifndef FTP_PORT_DEFAULT
#define FTP_PORT_DEFAULT 21
#endif

#ifndef FTP_PATH_DEFAULT
#define FTP_PATH_DEFAULT "/tankalarm/server"
#endif

#ifndef FTP_TIMEOUT_MS
#define FTP_TIMEOUT_MS 8000UL
#endif

#ifndef FTP_MAX_FILE_BYTES
#define FTP_MAX_FILE_BYTES 24576UL
#endif

// Diagnostic switch: when enabled, skip watchdog kicks only inside the
// ftp-backup critical path to help distinguish watchdog resets from faults.
#ifndef FTP_BACKUP_DISABLE_WATCHDOG_KICK_DIAG
#define FTP_BACKUP_DISABLE_WATCHDOG_KICK_DIAG 0
#endif

#ifndef FTP_BACKUP_MARKER_PATH
#define FTP_BACKUP_MARKER_PATH "/ftp_backup_marker.txt"
#endif

// Diagnostic switch: when enabled for FTPS backups, reconnect for each file
// upload so we can test whether control-session reuse triggers the reset.
#ifndef FTP_BACKUP_FTPS_RECONNECT_PER_FILE_DIAG
#define FTP_BACKUP_FTPS_RECONNECT_PER_FILE_DIAG 0
#endif

#ifndef FTP_BACKUP_FORCE_PLAIN_FTP_DIAG
#define FTP_BACKUP_FORCE_PLAIN_FTP_DIAG 0
#endif

// --- Per-file retry policy (FTPS multi-file backup) -------------------------
// See CODE REVIEW/PER_FILE_RETRY_PLAN_04172026.md for the full rationale.
//
// On Arduino Opta the LWIP socket pool is hard-baked at 4 PCBs and SO_LINGER
// is unsupported, so a freshly-closed data socket sits in TIME_WAIT for ~60s.
// If a STOR fails because the previous data PCB has not yet drained, the
// failure presents as -3005 (NSAPI_ERROR_NO_SOCKET) wrapped in
// FtpsError::DataConnectionFailed. The control channel is still alive in that
// case, so a bounded retry of just that file is safe and usually recovers.
//
// Retries are gated TWICE: by FtpsError class AND by the underlying NSAPI
// code. -3005 is retried; -3008 (timeout) and -3001 (DNS) are not.
//
// If FTP_BACKUP_PER_FILE_MAX_ATTEMPTS files in a row exhaust their retries we
// abort the rest of the batch — the situation is systemic and burning more
// retry waits is wasteful.
#ifndef FTP_BACKUP_PER_FILE_MAX_ATTEMPTS
#define FTP_BACKUP_PER_FILE_MAX_ATTEMPTS 3
#endif
#ifndef FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES
#define FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES 2
#endif
// Stepped backoff between retries (ms). Index 0 used between attempt 1 and
// attempt 2 (catches a partially-drained TIME_WAIT). Index 1 used between
// attempt 2 and attempt 3 (covers a full TIME_WAIT cycle).
#ifndef FTP_BACKUP_RETRY_BACKOFF_1_MS
#define FTP_BACKUP_RETRY_BACKOFF_1_MS 20000UL
#endif
#ifndef FTP_BACKUP_RETRY_BACKOFF_2_MS
#define FTP_BACKUP_RETRY_BACKOFF_2_MS 40000UL
#endif
// Inter-file pacing: see SPEED-OPT comment near the loop body. Promoted from
// magic literal to a #define so it can be tuned without code-search regressions
// once retry telemetry shows we can shorten it (target 45000 once stable).
#ifndef FTP_BACKUP_INTER_FILE_DELAY_MS
#define FTP_BACKUP_INTER_FILE_DELAY_MS 65000UL
#endif
// Deterministic retry-path verification hook.
// When enabled, the first successful transfer of the target file is
// intentionally treated as a retriable -3005 failure so the next attempt
// exercises the bounded retry/recovery telemetry path.
#ifndef FTP_BACKUP_TEST_FORCE_ONE_RETRY
#define FTP_BACKUP_TEST_FORCE_ONE_RETRY 0
#endif
#ifndef FTP_BACKUP_TEST_FORCE_ONE_RETRY_TARGET
#define FTP_BACKUP_TEST_FORCE_ONE_RETRY_TARGET "server_config.json"
#endif

#ifndef MAX_CLIENT_CONFIG_SNAPSHOTS
#define MAX_CLIENT_CONFIG_SNAPSHOTS 20
#endif

#ifndef VIEWER_SUMMARY_FILE
#define VIEWER_SUMMARY_FILE VIEWER_SUMMARY_OUTBOX_FILE  // "viewer_summary.qo" — server sends outbound
#endif

// Viewer summary cadence — defined in TankAlarm_Common.h; duplicated here as a fallback
#ifndef VIEWER_SUMMARY_INTERVAL_SECONDS
#define VIEWER_SUMMARY_INTERVAL_SECONDS 21600UL  // 6 hours
#endif
#ifndef VIEWER_SUMMARY_BASE_HOUR
#define VIEWER_SUMMARY_BASE_HOUR 6  // Start at 6 AM UTC
#endif

// MAX_RELAYS and CLIENT_SERIAL_BUFFER_SIZE are defined in TankAlarm_Common.h

#ifndef SERVER_SERIAL_BUFFER_SIZE
#define SERVER_SERIAL_BUFFER_SIZE 60   // Keep last 60 server serial messages (reduced for RAM)
#endif

#ifndef MAX_CLIENT_SERIAL_LOGS
#define MAX_CLIENT_SERIAL_LOGS 5    // Track serial logs for up to 5 clients (reduced for RAM)
#endif

// ============================================================================
// Server Notefile Names — Override common header for server perspective
// Server reads inbound .qi files (from clients via Route) and sends .qo outbound.
// Blues Notecard rule: note.add ONLY accepts .qo/.qos/.db/.dbs/.dbx
//                     note.get reads from .qi/.qis/.db/.dbx
// Cross-device delivery is handled by Notehub Routes — no device: prefix needed.
// ============================================================================

// Inbound data notefiles (client → route → server .qi)
// TELEMETRY_INBOX_FILE, ALARM_INBOX_FILE, DAILY_INBOX_FILE, UNLOAD_INBOX_FILE
// are defined in TankAlarm_Common.h and correct for server (reads .qi)

#ifndef SERIAL_LOG_FILE
#define SERIAL_LOG_FILE SERIAL_LOG_INBOX_FILE   // "serial_log.qi" — server receives logs
#endif

// SERIAL_REQUEST_FILE and LOCATION_REQUEST_FILE removed — server sends via command.qo

#ifndef SERIAL_ACK_FILE
#define SERIAL_ACK_FILE SERIAL_ACK_INBOX_FILE   // "serial_ack.qi" — server receives acks
#endif

#ifndef RELAY_FORWARD_FILE
#define RELAY_FORWARD_FILE RELAY_FORWARD_INBOX_FILE  // "relay_forward.qi" — server receives relay forward requests
#endif

// LOCATION_REQUEST_FILE removed — server sends via command.qo

#ifndef LOCATION_RESPONSE_FILE
#define LOCATION_RESPONSE_FILE LOCATION_RESPONSE_INBOX_FILE  // "location_response.qi" — server receives
#endif

#ifndef SERIAL_DEFAULT_MAX_ENTRIES
#define SERIAL_DEFAULT_MAX_ENTRIES 50
#endif

#ifndef SERIAL_STALE_SECONDS
#define SERIAL_STALE_SECONDS 1800  // 30 minutes
#endif

#ifndef MAX_HTTP_BODY_BYTES
#define MAX_HTTP_BODY_BYTES 16384  // Global cap on incoming HTTP body size (16KB)
#endif

#ifndef MAX_NOTES_PER_FILE_PER_POLL
#define MAX_NOTES_PER_FILE_PER_POLL 10  // Prevent long blocking notefile drains
#endif

// Calibration learning system constants
#ifndef CALIBRATION_LOG_PATH
#define CALIBRATION_LOG_PATH "/calibration_log.txt"
#endif

#ifndef MAX_CALIBRATION_ENTRIES
#define MAX_CALIBRATION_ENTRIES 100  // Max calibration entries per sensor
#endif

#ifndef MAX_CALIBRATION_SENSORS
#define MAX_CALIBRATION_SENSORS 20  // Max sensors to track calibration for
#endif

// National Weather Service API constants (for weather data during calibration)
#ifndef NWS_API_HOST
#define NWS_API_HOST "api.weather.gov"
#endif

#ifndef NWS_API_PORT
#define NWS_API_PORT 80  // Use HTTP for Arduino compatibility (HTTPS not easily supported)
#endif

#ifndef NWS_API_TIMEOUT_MS
#define NWS_API_TIMEOUT_MS 10000UL  // 10 second timeout for NWS API calls
#endif

#ifndef NWS_AVG_HOURS
#define NWS_AVG_HOURS 6  // Average temperature over this many hours for calibration
#endif

// Temperature cache TTL for real-time compensation (30 minutes)
#ifndef TEMP_CACHE_TTL_SECONDS
#define TEMP_CACHE_TTL_SECONDS 1800
#endif

// Temperature sentinel value (indicates no weather data available)
#define TEMPERATURE_UNAVAILABLE -999.0f

static byte gMacAddress[6] = { 0 }; // Initialize to 0, will be read from hardware or set if needed
static IPAddress gStaticIp(192, 168, 1, 200);
static IPAddress gStaticGateway(192, 168, 1, 1);
static IPAddress gStaticSubnet(255, 255, 255, 0);
static IPAddress gStaticDns(8, 8, 8, 8);
static const char DEFAULT_ADMIN_PIN[] = "";  // Empty: force user to set PIN on first use

struct ServerConfig {
  char serverName[32];
  char serverFleet[32];  // Fleet this server belongs to (clients target this fleet, e.g., "tankalarm-server")
  char clientFleet[32];  // Target fleet for client devices (e.g., "tankalarm-clients")
  char productUid[64];   // Notehub product UID (configurable for different fleets)
  char smsPrimary[20];
  char smsSecondary[20];
  char dailyEmail[64];
  char configPin[8];
  uint8_t dailyHour;
  uint8_t dailyMinute;
  uint16_t webRefreshSeconds;
  bool useStaticIp;      // false = DHCP (default), true = use static settings
  bool smsOnHigh;
  bool smsOnLow;
  bool smsOnClear;
  bool serverDownSmsEnabled;
  // Optional viewer device (summary publishing)
  bool viewerEnabled;
  uint8_t updatePolicy;             // 0=Disabled,1=AlertDFU,2=AlertGitHub,3=AutoGitHub,4=AutoDFU
  bool checkClientVersionAlerts;    // Show dashboard banner when a connected client reports outdated firmware
  bool checkViewerVersionAlerts;    // Show dashboard banner when the viewer reports outdated firmware
  // Optional LAN FTP backup/restore
  bool ftpEnabled;
  bool ftpPassive;
  bool ftpBackupOnChange;
  bool ftpRestoreOnBoot;
  uint16_t ftpPort;
  char ftpHost[64];
  char ftpUser[32];
  char ftpPass[32];
  char ftpPath[64];
  // FTPS (Explicit TLS) settings — requires FTPSclientOPTA library
  bool ftpsEnabled;                // true = use FTPS (TLS), false = plain FTP
  uint8_t ftpsTrustMode;           // 0 = Fingerprint, 1 = Imported PEM cert
  char ftpsFingerprint[65];        // SHA-256 hex fingerprint (64 chars + null)
  char ftpsTlsServerName[128];     // TLS SNI hostname for cert validation
};

struct SensorRecord {
  char clientUid[48];
  char site[32];
  char label[24];
  char contents[24];          // Tank contents (e.g., "Diesel", "Water") - not used for RPM monitors
  uint8_t sensorIndex;        // Internal key (auto-assigned index, part of composite key with clientUid)
  uint8_t userNumber;         // Optional user-assigned display number (0 = unset). Allows associating sensors on same physical tank.
  float levelInches;
  float sensorMa;             // Raw 4-20mA sensor reading (0 if not available)
  float sensorVoltage;        // Raw voltage sensor reading (0 if not available)
  char objectType[16];        // Object type: "tank", "engine", "pump", "gas", "flow"
  char sensorType[16];        // Sensor interface: "analog", "digital", "currentLoop", "pulse"
  char measurementUnit[8];    // Unit for display: "inches", "rpm", "psi", "gpm", etc.
  bool alarmActive;
  char alarmType[24];
  double lastUpdateEpoch;
  double firstSeenEpoch;        // When this record was first created (for archive naming)
  // 24-hour change tracking (computed server-side)
  float previousLevelInches;  // Level reading from ~24h ago
  double previousLevelEpoch;  // When the previous level was recorded
  // Rate limiting for SMS alerts
  double lastSmsAlertEpoch;
  uint8_t smsAlertsInLastHour;
  double smsAlertTimestamps[10];  // Track last 10 SMS alerts per sensor
};

// Per-client metadata (VIN voltage, etc.)
#ifndef MAX_CLIENT_METADATA
#define MAX_CLIENT_METADATA 20
#endif

struct ClientMetadata {
  char clientUid[48];
  float vinVoltage;          // Blues Notecard VIN voltage from daily report
  double vinVoltageEpoch;    // When the VIN voltage was last updated
  // GPS location from Blues Notecard (for NWS weather lookup)
  float latitude;            // Latitude in decimal degrees (e.g., 38.8894)
  float longitude;           // Longitude in decimal degrees (e.g., -77.0352)
  double locationEpoch;      // When the location was last updated
  // NWS API grid point cache (to avoid repeated lookups)
  char nwsGridOffice[4];     // NWS office ID (e.g., "LWX")
  uint16_t nwsGridX;         // Grid X coordinate
  uint16_t nwsGridY;         // Grid Y coordinate
  bool nwsGridValid;         // True if grid data is cached
  // Current temperature cache for real-time compensation
  float cachedTemperatureF;  // Cached current temperature in °F
  double tempCacheEpoch;     // When temperature was last fetched
  // Client firmware version tracking
  char firmwareVersion[16];  // Firmware version string from client telemetry/daily
  // Stale alerting state
  bool staleAlertSent;       // Whether a stale alert has been sent for this client
  // Last system-level alarm (solar/battery/power) — stored here, not on SensorRecord
  char lastSystemAlarmType[16];  // "solar", "battery", "power", or ""
  double lastSystemAlarmEpoch;   // When the last system alarm was received
  // Cellular signal strength (from client daily reports)
  int8_t signalBars;         // 0-4 bars, -1 = unknown
  int16_t signalRssi;        // RSSI in dBm
  int16_t signalRsrp;        // RSRP in dBm (LTE reference signal power)
  int16_t signalRsrq;        // RSRQ in dB  (LTE reference signal quality)
  char signalRat[8];         // Radio access technology (e.g., "lte", "catm")
  double signalEpoch;        // When signal data was last updated
  // Daily report part tracking (runtime only — not persisted)
  double dailyReportEpoch;    // Epoch of most recent daily report batch
  uint8_t dailyPartsReceived; // Bitmask of parts received (bits 0-7)
  bool dailyComplete;         // True when final part (m=false) received
  uint8_t dailyExpectedParts; // Part number of final part + 1
};

struct SerialLogEntry {
  double timestamp;          // Epoch timestamp
  char message[160];         // Log message
  char level[8];             // Log level (info, warn, error)
  char source[16];           // Source module identifier
};

struct ServerSerialBuffer {
  SerialLogEntry entries[SERVER_SERIAL_BUFFER_SIZE];
  uint8_t writeIndex;
  uint8_t count;
};

struct ClientSerialBuffer {
  char clientUid[48];
  SerialLogEntry entries[CLIENT_SERIAL_BUFFER_SIZE];
  uint8_t writeIndex;
  uint8_t count;
  double lastRequestEpoch;
  double lastAckEpoch;
  double lastLogEpoch;
  bool awaitingLogs;
  char lastAckStatus[24];
};

static ClientMetadata gClientMetadata[MAX_CLIENT_METADATA];
static uint8_t gClientMetadataCount = 0;

static ServerSerialBuffer gServerSerial;
static ClientSerialBuffer gClientSerialBuffers[MAX_CLIENT_SERIAL_LOGS];
static uint8_t gClientSerialBufferCount = 0;

// Calibration learning system data structures
// Stores manual level readings paired with sensor readings for learning
struct CalibrationEntry {
  double timestamp;           // Epoch time of the reading
  float sensorReading;        // Raw sensor value (mA for 4-20mA sensors)
  float verifiedLevelInches;  // Manually verified sensor level in inches
  float temperatureF;         // NWS temperature (6-hour avg) at calibration time, -999 if unavailable
  char notes[64];             // Optional notes about the reading
};

// Per-sensor calibration data with learned parameters
struct SensorCalibration {
  char clientUid[48];
  uint8_t sensorIndex;
  // Learned multiple linear regression parameters: 
  // level = slope * sensorReading + tempCoef * (temperature - refTemp) + offset
  // Temperature is normalized around a reference temperature (70°F) for numerical stability
  float learnedSlope;         // Learned inches per mA (replaces maxValue calculation)
  float learnedOffset;        // Learned offset in inches
  float learnedTempCoef;      // Temperature coefficient (inches per °F deviation from refTemp)
  bool hasLearnedCalibration; // True if we have enough data points for calibration
  bool hasTempCompensation;   // True if enough temp data points for reliable temp coefficient
  uint8_t entryCount;         // Number of calibration entries (total)
  uint8_t tempEntryCount;     // Number of entries with valid temperature data
  float rSquared;             // Goodness of fit (0-1, higher is better)
  double lastCalibrationEpoch; // When calibration was last updated
  // Original sensor configuration for reference
  float originalMaxValue;     // Original maxValue from config
  char sensorType[16];        // "pressure" or "ultrasonic"
  float sensorMountHeight;    // Sensor mount height in inches
  // Quality metrics for calibration warnings
  float minSensorMa;          // Minimum sensor reading in calibration data
  float maxSensorMa;          // Maximum sensor reading in calibration data
  float minLevelInches;       // Minimum level in calibration data
  float maxLevelInches;       // Maximum level in calibration data
  // Implied fluid specific gravity, derived from learnedSlope vs the theoretical
  // slope-for-water computed from the sensor's configured pressure range. Useful as
  // a sanity check ("operator says diesel, math says 0.86 \u2014 looks right") and as a
  // contamination/product-change indicator over time. 0.0f = not yet computed.
  float impliedSpecificGravity;
  // |impliedSG - presetSG|; flagged in the dashboard when > 0.05.
  float specificGravityDriftFromPreset;
};

// Reference temperature for temperature compensation (°F)
// Using 70°F as a reasonable "room temperature" baseline
static const float TEMP_REFERENCE_F = 70.0f;
// Minimum entries with temperature data to enable temperature compensation
static const uint8_t MIN_TEMP_ENTRIES_FOR_COMPENSATION = 5;

static SensorCalibration gSensorCalibrations[MAX_CALIBRATION_SENSORS];
static uint8_t gSensorCalibrationCount = 0;

// ============================================================================
// Historical Data & Tiered Storage System
// ============================================================================
// Structure for alarm history logging
#ifndef MAX_ALARM_LOG_ENTRIES
#define MAX_ALARM_LOG_ENTRIES 50   // Ring buffer of recent alarms (reduced for RAM)
#endif

struct AlarmLogEntry {
  double timestamp;           // Epoch timestamp
  char siteName[32];          // Site name
  char clientUid[48];         // Client UID
  uint8_t sensorIndex;         // Internal sensor index
  float level;                // Level at time of alarm
  bool isHigh;                // True = high alarm, false = low alarm
  bool cleared;               // Whether alarm has been cleared
  double clearedTimestamp;    // When alarm was cleared (0 if not cleared)
};

static AlarmLogEntry alarmLog[MAX_ALARM_LOG_ENTRIES];
static uint8_t alarmLogCount = 0;
static uint8_t alarmLogWriteIndex = 0;  // Ring buffer write pointer

// ============================================================================
// Transmission Log System (outbound messages to clients/Notehub)
// ============================================================================
#ifndef MAX_TRANSMISSION_LOG_ENTRIES
#define MAX_TRANSMISSION_LOG_ENTRIES 50   // Ring buffer of recent outbound transmissions (reduced for RAM)
#endif

struct TransmissionLogEntry {
  double timestamp;             // When message was queued
  char siteName[32];            // Target site name (if applicable)
  char clientUid[48];           // Target client UID (if applicable)
  char messageType[24];         // Type: sms, email, config, relay, relay_clear, viewer_summary, serial_request, location_request
  char status[12];              // outbox, sent, failed
  char detail[64];              // Brief description/detail
};

static TransmissionLogEntry gTransmissionLog[MAX_TRANSMISSION_LOG_ENTRIES];
static uint8_t gTransmissionLogCount = 0;
static uint8_t gTransmissionLogWriteIndex = 0;  // Ring buffer write pointer

// Forward declaration for transmission log
static void logTransmission(const char *clientUid, const char *site, const char *messageType, const char *status, const char *detail);
static void handleTransmissionLogGet(EthernetClient &client);

// ============================================================================
// Sensor Unload Log System
// ============================================================================
// Logs when fill-and-empty sensors are unloaded (significant level drop from peak)
#ifndef MAX_UNLOAD_LOG_ENTRIES
#define MAX_UNLOAD_LOG_ENTRIES 50  // Ring buffer of recent unload events
#endif

struct UnloadLogEntry {
  double eventTimestamp;      // When unload was detected
  double peakTimestamp;       // When peak level was recorded
  char siteName[32];          // Site name
  char clientUid[48];         // Client UID
  char tankLabel[24];         // Tank label/name
  uint8_t sensorIndex;         // Internal sensor index
  float peakInches;           // Peak level before unload
  float emptyInches;          // Level after unload (empty reading)
  float peakSensorMa;         // Sensor reading at peak (for diagnostics)
  float emptySensorMa;        // Sensor reading at empty (for diagnostics)
  char measurementUnit[8];    // Unit of measurement (inches, psi, etc.)
  bool smsNotified;           // Whether SMS was sent for this event
  bool emailNotified;         // Whether included in email summary
};

static UnloadLogEntry gUnloadLog[MAX_UNLOAD_LOG_ENTRIES];
static uint8_t gUnloadLogCount = 0;
static uint8_t gUnloadLogWriteIndex = 0;  // Ring buffer write pointer

// Forward declarations for unload functions
static void handleUnload(JsonDocument &doc, double epoch);
static void logUnloadEvent(const UnloadLogEntry &entry);
static void sendUnloadSms(const UnloadLogEntry &entry);

// Structure for hourly telemetry snapshots (hot tier)
#ifndef MAX_HOURLY_HISTORY_PER_SENSOR
#define MAX_HOURLY_HISTORY_PER_SENSOR 90   // ~90 snapshots per sensor (ring buffer). Warm tier on LittleFS extends to 3 months; cold tier on FTP extends further.
#endif
// NOTE: Ring buffer holds 90 snapshots per sensor in RAM. Older data lives in
// the warm tier (LittleFS daily summaries, 3 months) and cold tier (FTP
// monthly archives, unlimited). Combined tiers provide long-term retention.

#ifndef MAX_HISTORY_SENSORS
#define MAX_HISTORY_SENSORS 20
#endif

struct TelemetrySnapshot {
  double timestamp;           // Epoch timestamp
  float level;                // Level in inches
  float voltage;              // VIN voltage (0 if not available)
};

struct SensorHourlyHistory {
  char clientUid[48];
  char siteName[32];
  uint8_t sensorIndex;
  float heightInches;         // Tank height for percentage calculation
  TelemetrySnapshot snapshots[MAX_HOURLY_HISTORY_PER_SENSOR];
  uint16_t snapshotCount;
  uint16_t writeIndex;        // Ring buffer write pointer
};

static SensorHourlyHistory gSensorHistory[MAX_HISTORY_SENSORS];
static uint8_t gSensorHistoryCount = 0;

// Structure for daily summaries (warm tier, persisted to LittleFS)
struct DailySummary {
  uint32_t date;              // Date as YYYYMMDD
  float minLevel;
  float maxLevel;
  float avgLevel;
  float openingLevel;         // Level at start of day
  float closingLevel;         // Level at end of day
  uint8_t alarmCount;
  float avgVoltage;
};

// Monthly archive header for FTP warm tier
struct MonthlyArchiveHeader {
  uint16_t year;
  uint8_t month;
  uint8_t sensorCount;
  uint32_t recordCount;
  uint32_t fileSize;
};

// History storage settings
struct HistorySettings {
  uint16_t hotTierRetentionDays;    // Days to keep in RAM ring buffer (max = MAX_HOURLY_HISTORY_PER_SENSOR snapshots)
  uint8_t warmTierRetentionMonths;  // Months to keep daily summaries (default 24)
  bool ftpArchiveEnabled;           // Whether to archive to FTP
  uint8_t ftpSyncHour;              // Hour to sync to FTP (default 3 AM)
  double lastFtpSyncEpoch;          // Last successful FTP sync
  double lastPruneEpoch;            // Last data pruning
  uint32_t totalRecordsPruned;      // Lifetime pruned records count
};

#define MAX_HISTORY_SETTINGS_FILE_SIZE 1024  // Max size for history settings JSON file

static HistorySettings gHistorySettings = {
  90,    // hotTierRetentionDays (matches ring buffer: 90 snapshots)
  24,    // warmTierRetentionMonths
  false, // ftpArchiveEnabled (uses existing FTP config)
  3,     // ftpSyncHour
  0.0,   // lastFtpSyncEpoch
  0.0,   // lastPruneEpoch
  0      // totalRecordsPruned
};
static bool gWarmTierDataExists = false;  // Set true when warm tier data is actually present on disk

// ============================================================================
// LittleFS Daily Summary Storage (Warm Tier)
// ============================================================================
// Compact daily summaries persisted to flash. Survives reboots. ~32 bytes per
// day per sensor, stored as JSON in /fs/history/daily_YYYYMM.json files.
// When FTP is available, monthly archives extend range beyond 3 months.
// When FTP is NOT available, data is limited to hot tier + 3 months of dailies.
#ifndef MAX_DAILY_SUMMARY_MONTHS
#define MAX_DAILY_SUMMARY_MONTHS 3  // Keep 3 months of daily files on LittleFS
#endif

// Last daily rollup tracking (persistent across reboots via history settings)
static uint32_t gLastDailyRollupDate = 0;  // YYYYMMDD of last completed rollup

// FTP archive month cache — avoids re-downloading during a single web session
struct FtpArchiveCache {
  uint16_t cachedYear;
  uint8_t cachedMonth;
  bool valid;
  // Cached summary data per sensor
  struct CachedSensorSummary {
    char clientUid[48];
    uint8_t sensorIndex;
    float minLevel, maxLevel, avgLevel;
    float avgVoltage;
    uint16_t readings;
  };
  CachedSensorSummary sensors[MAX_HISTORY_SENSORS];
  uint8_t sensorCount;
};
static FtpArchiveCache gFtpArchiveCache = {0, 0, false, {}, 0};

// Forward declarations for history functions
static void logAlarmEvent(const char *clientUid, const char *siteName, uint8_t sensorIndex, float level, bool isHigh);
static void clearAlarmEvent(const char *clientUid, uint8_t sensorIndex);
static void recordTelemetrySnapshot(const char *clientUid, const char *siteName, uint8_t sensorIndex, float heightInches, float level, float voltage);
// Daily summary warm tier functions
static void rollupDailySummaries();
static bool saveDailySummaryFile(uint16_t year, uint8_t month);
static bool loadDailySummaryMonth(uint16_t year, uint8_t month, JsonDocument &doc);
static void pruneDailySummaryFiles();
// Hot tier persistence (survive reboots)
static bool saveHotTierSnapshot();
static void loadHotTierSnapshot();
// FTP archive retrieval
static bool loadFtpArchiveCached(uint16_t year, uint8_t month);
static void populateStatsFromFtpCache(const char *clientUid, uint8_t sensorIndex, JsonObject &statsObj);
static SensorHourlyHistory *findOrCreateSensorHistory(const char *clientUid, uint8_t sensorIndex);
static void pruneHotTierIfNeeded();
static bool archiveMonthToFtp(uint16_t year, uint8_t month);
static bool loadArchivedMonth(uint16_t year, uint8_t month, JsonDocument &doc);
static void populateHistorySettingsJson(JsonDocument &doc);
static void applyHistorySettingsFromJson(const JsonDocument &doc);
static bool saveHistorySettings();
static void loadHistorySettings();

// Forward declarations for FTP structs
struct FtpSession;
struct FtpResult;

// Forward declarations for FTP functions
static bool ftpSendCommand(FtpSession &session, const char *command, int &code, char *message, size_t maxLen);
static bool ftpConnectAndLogin(FtpSession &session, char *error, size_t errorSize);
static bool ftpEnterPassive(FtpSession &session, IPAddress &dataHost, uint16_t &dataPort, char *error, size_t errorSize);
static bool ftpStoreBuffer(FtpSession &session, const char *remoteFile, const uint8_t *data, size_t len, char *error, size_t errorSize);
static bool ftpRetrieveBuffer(FtpSession &session, const char *remoteFile, char *out, size_t outMax, size_t &outLen, char *error, size_t errorSize);
static void ftpQuit(FtpSession &session);
static bool ftpsConnectAndLogin(char *error, size_t errorSize);
static bool ftpsStoreBuffer(const char *remoteFile, const uint8_t *data, size_t len, char *error, size_t errorSize);
static bool ftpsRetrieveBuffer(const char *remoteFile, char *out, size_t outMax, size_t &outLen, char *error, size_t errorSize);
static void ftpsQuit();
static void ftpsProgressKick();
static bool ftpBackupClientConfigs(FtpSession &session, bool useFtps, char *error, size_t errorSize, uint8_t &uploadedFiles);
static bool ftpRestoreClientConfigs(FtpSession &session, bool useFtps, char *error, size_t errorSize, uint8_t &restoredFiles);
static FtpResult performFtpBackupDetailed();
static FtpResult performFtpRestoreDetailed();

static ServerConfig gConfig;
static bool gConfigDirty = false;
static bool gPendingFtpBackup = false;
static bool gPaused = false;  // When true, pause Notecard processing for maintenance

// Rate limiting for authentication attempts
static uint8_t gAuthFailureCount = 0;
static unsigned long gLastAuthFailureTime = 0;
static unsigned long gAuthLockoutStart = 0;  // When the current backoff/lockout started
static unsigned long gAuthLockoutDuration = 0;  // Duration of current lockout in ms
static const unsigned long AUTH_LOCKOUT_DURATION = 30000;  // 30 seconds after max failures
static const uint8_t AUTH_MAX_FAILURES = 5;  // Max failures before lockout

// Single-session enforcement: only one browser session active at a time.
// A new login invalidates any previous session token.
static char gSessionToken[17] = "";  // 16 hex chars + null terminator

// VIN_ANALOG_PIN must be defined before generateSessionToken() so the function
// can use the configured pin rather than a hardcoded fallback.
#ifndef VIN_ANALOG_PIN
#define VIN_ANALOG_PIN A0           // Opta analog input pin (A0-A7, 0-10V range)
#endif
// Supplementary ADC pins sampled for thermal/shot noise during token generation.
// Override if these pins are in use for other purposes on your hardware.
#ifndef ENTROPY_ADC_PIN_1
#define ENTROPY_ADC_PIN_1 A1
#endif
#ifndef ENTROPY_ADC_PIN_2
#define ENTROPY_ADC_PIN_2 A2
#endif
#ifndef ENTROPY_ADC_PIN_3
#define ENTROPY_ADC_PIN_3 A3
#endif

static void generateSessionToken() {
  // Gather entropy from timing jitter and ADC noise across multiple pins.
  // Multiple analogRead() calls add thermal/shot noise; timing samples taken
  // before and after the ADC reads capture execution jitter.
  uint32_t t0 = micros();
  uint32_t a0 = (uint32_t)analogRead(VIN_ANALOG_PIN);
  uint32_t a1 = (uint32_t)analogRead(ENTROPY_ADC_PIN_1);
  uint32_t a2 = (uint32_t)analogRead(ENTROPY_ADC_PIN_2);
  uint32_t a3 = (uint32_t)analogRead(ENTROPY_ADC_PIN_3);
  uint32_t t1 = micros();         // Second timing sample after ADC reads captures jitter
  uint32_t currentMillis = millis();
  // Build a 64-bit seed by placing independent entropy sources in the high and
  // low halves; prime-valued shifts reduce correlation between mixed terms.
  // 64-bit state gives 2^64 possible seeds, making brute-force infeasible.
  // Cast to uint64_t before shifting to avoid truncating bits in 32-bit arithmetic.
  uint64_t seedHigh = (uint64_t)t0 ^ ((uint64_t)currentMillis << 13) ^ ((uint64_t)a0 << 5) ^ ((uint64_t)a1 << 11);
  uint64_t seedLow  = (uint64_t)t1 ^ ((uint64_t)currentMillis >> 19) ^ ((uint64_t)a0 >> 27) ^ ((uint64_t)a2 << 21) ^ ((uint64_t)a3 << 7);
  uint64_t seed = (seedHigh << 32) | seedLow;
  // 64-bit LCG constants from Knuth (MMIX) — full-period over 2^64.
  for (int i = 0; i < 16; i++) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    gSessionToken[i] = "0123456789abcdef"[(seed >> 60) & 0xF];  // Top 4 bits of 64-bit state
  }
  gSessionToken[16] = '\0';
}

// Constant-time comparison for session tokens (16 chars)
static bool sessionTokenMatches(const char *candidate) {
  if (!candidate || strlen(candidate) != 16 || gSessionToken[0] == '\0') {
    return false;
  }
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < 16; ++i) {
    diff |= (uint8_t)(candidate[i] ^ gSessionToken[i]);
  }
  return diff == 0;
}

static String gContactsCache;
static bool gContactsCacheValid = false;

static SensorRecord gSensorRecords[MAX_SENSOR_RECORDS];
static uint8_t gSensorRecordCount = 0;

// Hash table for O(1) sensor lookups by clientUid + sensorIndex
// Uses djb2 hash algorithm with linear probing for collision resolution
#define SENSOR_HASH_TABLE_SIZE 128  // Must be power of 2, >= 2 * MAX_SENSOR_RECORDS
#define SENSOR_HASH_EMPTY 0xFF      // Sentinel value for empty slot

// Compile-time validation: hash table must be at least 2x the max records for good performance
static_assert(SENSOR_HASH_TABLE_SIZE >= 2 * MAX_SENSOR_RECORDS, "Hash table too small - must be >= 2 * MAX_SENSOR_RECORDS");
static_assert((SENSOR_HASH_TABLE_SIZE & (SENSOR_HASH_TABLE_SIZE - 1)) == 0, "Hash table size must be power of 2");

static uint8_t gSensorHashTable[SENSOR_HASH_TABLE_SIZE];  // Index into gSensorRecords, or SENSOR_HASH_EMPTY

// djb2 hash function - fast and good distribution for strings
static uint32_t hashSensorKey(const char *clientUid, uint8_t sensorIndex) {
  uint32_t hash = 5381;
  const char *p = clientUid;
  while (*p) {
    hash = ((hash << 5) + hash) ^ (uint8_t)*p++;  // hash * 33 ^ c
  }
  hash = ((hash << 5) + hash) ^ sensorIndex;
  return hash;
}

// Initialize hash table (call after clearing gSensorRecords)
static void initSensorHashTable() {
  memset(gSensorHashTable, SENSOR_HASH_EMPTY, sizeof(gSensorHashTable));
}

// Insert sensor record into hash table
static void insertSensorIntoHash(uint8_t recordIndex) {
  if (recordIndex >= gSensorRecordCount) return;
  SensorRecord &rec = gSensorRecords[recordIndex];
  uint32_t hash = hashSensorKey(rec.clientUid, rec.sensorIndex);
  uint32_t idx = hash & (SENSOR_HASH_TABLE_SIZE - 1);  // Fast mod for power of 2
  
  // Linear probing to find empty slot
  for (uint32_t i = 0; i < SENSOR_HASH_TABLE_SIZE; ++i) {
    uint32_t probeIdx = (idx + i) & (SENSOR_HASH_TABLE_SIZE - 1);
    if (gSensorHashTable[probeIdx] == SENSOR_HASH_EMPTY) {
      gSensorHashTable[probeIdx] = recordIndex;
      return;
    }
  }
  // Table full - should not happen if SENSOR_HASH_TABLE_SIZE >= 2 * MAX_SENSOR_RECORDS
  Serial.println(F("WARNING: Sensor hash table full, cannot insert record"));
}

// Rebuild hash table from scratch (call after bulk operations)
static void rebuildSensorHashTable() {
  initSensorHashTable();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    insertSensorIntoHash(i);
  }
}

// Find sensor record using hash table - O(1) average case
static SensorRecord *findSensorByHash(const char *clientUid, uint8_t sensorIndex) {
  uint32_t hash = hashSensorKey(clientUid, sensorIndex);
  uint32_t idx = hash & (SENSOR_HASH_TABLE_SIZE - 1);
  
  // Linear probing to find matching entry
  for (uint32_t i = 0; i < SENSOR_HASH_TABLE_SIZE; ++i) {
    uint32_t probeIdx = (idx + i) & (SENSOR_HASH_TABLE_SIZE - 1);
    uint8_t recordIdx = gSensorHashTable[probeIdx];
    
    if (recordIdx == SENSOR_HASH_EMPTY) {
      return nullptr;  // Not found
    }
    
    // Validate recordIdx is within bounds before accessing array
    if (recordIdx >= MAX_SENSOR_RECORDS) {
      Serial.print(F("ERROR: Invalid hash table entry: "));
      Serial.println(recordIdx);
      return nullptr;  // Corrupted hash table entry
    }
    
    SensorRecord &rec = gSensorRecords[recordIdx];
    if (strcmp(rec.clientUid, clientUid) == 0 && rec.sensorIndex == sensorIndex) {
      return &rec;
    }
  }
  return nullptr;  // Table full (should not happen)
}

struct ClientConfigSnapshot {
  char uid[48];
  char site[32];
  char payload[1536];
  bool pendingDispatch;      // true when Notecard send failed and needs retry (persisted)
  char configVersion[16];    // Config version hash for ACK tracking
  double lastAckEpoch;       // When last config ACK was received from client
  char lastAckStatus[16];    // Last ACK status: "applied", "rejected", "parse_error"
  uint8_t dispatchAttempts;  // Number of times this config has been dispatched/retried
  double lastDispatchEpoch;  // When the most recent dispatch attempt occurred
};

// Maximum number of config dispatch retries before auto-cancel
#ifndef MAX_CONFIG_DISPATCH_RETRIES
#define MAX_CONFIG_DISPATCH_RETRIES 5
#endif

// Enum for client config dispatch status
enum class ConfigDispatchStatus {
  Ok,
  OkWithPurge,        // Success — but older pending configs were purged from outbox
  PayloadTooLarge,
  NotecardFailure,
  CachedOnly          // Config saved locally but Notecard unavailable
};

// Tracks how many stale config notes were purged from the outbox during last dispatch
static uint8_t gLastConfigPurgeCount = 0;

// FTP session and result structures for backup/restore operations
struct FtpSession {
  EthernetClient ctrl;
};

struct FtpResult {
  bool success;               // Overall operation success
  uint8_t filesProcessed;     // Number of files successfully processed
  uint8_t filesFailed;        // Number of files that failed
  // Per-file retry telemetry (FTPS backup only). All counts are batch-scoped.
  uint16_t retryAttemptsTotal;     // Total retry attempts across all files (excludes first attempt)
  uint8_t  filesRecoveredByRetry;  // Files that succeeded only after >= 1 retry
  uint8_t  filesRetryExhausted;    // Files that consumed all attempts and still failed
  char failedFiles[256];      // Comma-separated list of failed file names
  char errorMessage[128];     // Human-readable error message
  
  FtpResult() : success(false), filesProcessed(0), filesFailed(0),
                retryAttemptsTotal(0), filesRecoveredByRetry(0),
                filesRetryExhausted(0) {
    failedFiles[0] = '\0';
    errorMessage[0] = '\0';
  }
  
  void addFailedFile(const char *fileName) {
    if (strlen(failedFiles) > 0 && strlen(failedFiles) + strlen(fileName) + 2 < sizeof(failedFiles)) {
      strncat(failedFiles, ", ", sizeof(failedFiles) - strlen(failedFiles) - 1);
    }
    if (strlen(failedFiles) + strlen(fileName) < sizeof(failedFiles)) {
      strncat(failedFiles, fileName, sizeof(failedFiles) - strlen(failedFiles) - 1);
    }
  }
};

static ClientConfigSnapshot gClientConfigs[MAX_CLIENT_CONFIG_SNAPSHOTS];
static uint8_t gClientConfigCount = 0;

static Notecard notecard;
static EthernetServer gWebServer(ETHERNET_PORT);
static FtpsClient gFtpsClient;
static char gServerUid[48] = {0};

// Set true while performFtpBackup is running so handleWebRequests refuses
// new connections. Each accepted browser request consumes a precious LWIP
// socket slot and the FTPS data channels need every slot they can get.
// Connection that initiated the backup is unaffected (it's already past
// the accept stage and just blocked in respondJson).
static volatile bool gBackupInProgress = false;

static double gLastSyncedEpoch = 0.0;
static unsigned long gLastSyncMillis = 0;
static double gNextDailyEmailEpoch = 0.0;
static double gNextViewerSummaryEpoch = 0.0;
static double gLastViewerSummaryEpoch = 0.0;

static unsigned long gLastPollMillis = 0;
static unsigned long gLastLinkCheckMillis = 0;
static bool gLastLinkState = false;

static double gLastHeartbeatPersistEpoch = 0.0;
static double gLastHeartbeatFileEpoch = 0.0;
static bool gServerDownChecked = false;

// DFU (Device Firmware Update) state tracking
static unsigned long gLastDfuCheckMillis = 0;
static bool gDfuUpdateAvailable = false;
static char gDfuVersion[32] = {0};
static uint32_t gDfuFirmwareLength = 0;
static char gDfuMode[16] = "idle";  // Notecard DFU mode: idle, downloading, ready, error, etc.
static bool gDfuInProgress = false;
static char gDfuError[128] = {0};   // Last error message from Notecard dfu.status
static TankAlarmDfuStatus gDfuStatus;

// GitHub release check state
static bool gGitHubUpdateAvailable = false;
static char gGitHubLatestVersion[32] = {0};
static char gGitHubReleaseUrl[128] = {0};
static bool gGitHubAssetAvailable = false;
static char gGitHubAssetUrl[256] = {0};
static uint32_t gGitHubAssetSize = 0;
static char gGitHubAssetSha256[65] = {0};
static unsigned long gLastGitHubCheckMs = 0;
static bool gGitHubBootCheckDone = false;
struct GitHubFirmwareTargetState {
  uint8_t target;
  bool checked;
  bool updateAvailable;
  bool assetAvailable;
  char latestVersion[32];
  char releaseUrl[128];
  char assetUrl[256];
  uint32_t assetSize;
  char assetSha256[65];
  char error[96];
};

#define GITHUB_CHECK_INTERVAL_MS 86400000UL  // Re-check every 24 hours
#define GITHUB_REPO_OWNER "SenaxInc"
#define GITHUB_REPO_NAME  "SenaxTankAlarm"
#define GITHUB_DIRECT_DOWNLOAD_CHUNK_SIZE 1024U
#define GITHUB_DIRECT_MAX_REDIRECTS 4
#define GITHUB_DIRECT_HTTP_TIMEOUT_MS 15000
#define GITHUB_DIRECT_HEADER_LINE_MAX 2048
#define FIRMWARE_TARGET_SERVER 0
#define FIRMWARE_TARGET_FTPS_TEST 1
#define DFU_SOURCE_NOTEHUB 0
#define DFU_SOURCE_GITHUB_DIRECT 1
#define UPDATE_POLICY_DISABLED     0   // No update checking
#define UPDATE_POLICY_ALERT_DFU    1   // Check Notehub DFU, alert on dashboard if available
#define UPDATE_POLICY_ALERT_GITHUB 2   // Check GitHub releases, alert on dashboard if available
#define UPDATE_POLICY_AUTO_GITHUB  3   // Check GitHub releases, automatically install if found
#define UPDATE_POLICY_AUTO_DFU     4   // Check Notehub DFU, automatically install if found

static GitHubFirmwareTargetState gSelectedFirmwareTargetState = {
  FIRMWARE_TARGET_SERVER,
  false,
  false,
  false,
  {0},
  {0},
  {0},
  0,
  {0},
  {0}
};

static void dfuKickWatchdog() {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
}

// I2C bus health tracking (required by TankAlarm_I2C.h)
uint32_t gCurrentLoopI2cErrors = 0;    // Not used by Server but required by extern
uint32_t gI2cBusRecoveryCount = 0;
static bool gNotecardAvailable = true;
static uint16_t gNotecardFailureCount = 0;
static unsigned long gLastSuccessfulNotecardComm = 0;

// Server voltage monitoring
// NOTE: card.voltage reads the Notecard's V+ power rail (~5V from Opta regulator),
// NOT the external 12V battery. To read actual battery voltage, either:
//   1. Use an Opta analog input (A0-A7) with a voltage divider (see below)
//   2. Use Modbus from a charge controller (SunSaver MPPT on client devices)
// The Notecard voltage is still useful for detecting Notecard power supply issues.
static float gNotecardVoltage = 0.0f;       // Notecard V+ rail (~3.3-5V, regulated)
static float gInputVoltage = 0.0f;          // Actual input voltage (from analog pin or 0 if not wired)
static double gVoltageEpoch = 0.0;
static unsigned long gLastVoltageCheckMillis = 0;
#define VOLTAGE_CHECK_INTERVAL_MS 900000UL  // Check voltage every 15 minutes

// Analog input voltage divider configuration for reading Vin (12V battery)
// To read 12V+ on an Opta analog input (0-10V range, 12-bit ADC):
//   Wire a voltage divider: Vin --> R1 --> A0 --> R2 --> GND
//   Example: R1=22K, R2=47K gives divider ratio = 47/(22+47) = 0.6812
//            12V * 0.6812 = 8.17V at A0 (within 0-10V range)
//   Set VIN_DIVIDER_RATIO = R2 / (R1 + R2)
//   Set VIN_ANALOG_PIN to the Opta analog pin connected to the divider
//   Set VIN_ANALOG_ENABLED to true once hardware is wired
#ifndef VIN_ANALOG_ENABLED
#define VIN_ANALOG_ENABLED false  // Set true when voltage divider hardware is connected
#endif
// VIN_ANALOG_PIN is defined earlier (before generateSessionToken) so it is available for preprocessing/macro expansion.
#ifndef VIN_DIVIDER_RATIO
#define VIN_DIVIDER_RATIO 0.6812f   // R2/(R1+R2) — default for R1=22K, R2=47K
#endif
#ifndef VIN_ADC_MAX
#define VIN_ADC_MAX 65535.0f         // Opta 16-bit ADC (analogReadResolution(16))
#endif
#ifndef VIN_ADC_REF_VOLTAGE
#define VIN_ADC_REF_VOLTAGE 10.0f   // Opta analog inputs: 0-10V range
#endif

// Email rate limiting
static double gLastDailyEmailSentEpoch = 0.0;
#define MIN_DAILY_EMAIL_INTERVAL_SECONDS 3600  // Minimum 1 hour between daily emails

// Sensor registry & client metadata persistence state
static bool gSensorRegistryDirty = false;         // True when sensor records need saving
static bool gClientMetadataDirty = false;        // True when client metadata needs saving
static unsigned long gLastRegistrySaveMillis = 0;
static unsigned long gLastStaleCheckMillis = 0;

// ----------------------------------------------------------------------------
// Flash write observability
// ----------------------------------------------------------------------------
// Wraps tankalarm_posix_write_file_atomic() with counters so the dashboard
// can show per-boot write activity. Hash-gate skips also tracked so we can
// see how often the throttle is actually preventing redundant writes.
//
// IMPORTANT: All persistent writers in this sketch should call
// serverWriteFileAtomic() instead of tankalarm_posix_write_file_atomic()
// directly. The local posix_write_file() helper above also routes through
// here.
static volatile uint32_t gFlashWritesSinceBoot = 0;
static volatile uint32_t gFlashWriteFailuresSinceBoot = 0;
static volatile uint32_t gFlashWritesSkippedHash = 0;
static volatile uint32_t gFlashBytesWrittenSinceBoot = 0;
static const char *gLastFlashWritePath = "";
static unsigned long gLastFlashWriteMillis = 0;
static const char *gLastFlashWriteFailurePath = "";
static unsigned long gLastFlashWriteFailureMillis = 0;

static inline bool serverWriteFileAtomic(const char *path, const char *data, size_t len) {
  bool ok = tankalarm_posix_write_file_atomic(path, data, len);
  if (ok) {
    gFlashWritesSinceBoot++;
    gFlashBytesWrittenSinceBoot += (uint32_t)len;
    gLastFlashWritePath = path ? path : "";
    gLastFlashWriteMillis = millis();
  } else {
    gFlashWriteFailuresSinceBoot++;
    gLastFlashWriteFailurePath = path ? path : "";
    gLastFlashWriteFailureMillis = millis();
  }
  return ok;
}

// FNV-1a 32-bit hash. Used to gate periodic full-file rewrites: if the
// serialized payload hashes to the same value as the last successful save,
// skip the write entirely. Cheap on Cortex-M7 and avoids pulling in CRC32
// table storage.
static inline uint32_t fnv1a32(const char *data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= (uint8_t)data[i];
    h *= 16777619u;
  }
  return h;
}

// Last-saved hashes for hash-gated writers. 0 means "no prior save".
static uint32_t gHotTierLastHash = 0;

// POSIX file helpers - use tankalarm_ prefixed versions from shared library
// Keep posix_write_file and posix_read_file locally as they have custom implementations
#if defined(POSIX_FILE_IO_AVAILABLE)
static inline long posix_file_size(FILE *fp) { return tankalarm_posix_file_size(fp); }
static inline bool posix_file_exists(const char *path) { return tankalarm_posix_file_exists(path); }
static inline void posix_log_error(const char *op, const char *path) { tankalarm_posix_log_error(op, path); }

// POSIX-compliant safe file write — delegates to atomic write-to-temp-then-rename
// to prevent data loss on power failure during save operations. Routed through
// serverWriteFileAtomic() so writes are counted in /api/system-status.
static bool posix_write_file(const char *path, const char *data, size_t len) {
  return serverWriteFileAtomic(path, data, len);
}

// POSIX-compliant safe file read with error handling
static ssize_t posix_read_file(const char *path, char *buffer, size_t bufSize) {
  FILE *fp = fopen(path, "r");
  if (!fp) {
    posix_log_error("fopen", path);
    return -1;
  }
  
  long fileSize = posix_file_size(fp);
  if (fileSize < 0) {
    fclose(fp);
    return -1;
  }
  fseek(fp, 0, SEEK_SET);
  
  size_t toRead = (size_t)fileSize;
  if (toRead > bufSize - 1) {
    toRead = bufSize - 1;
  }
  
  size_t bytesRead = fread(buffer, 1, toRead, fp);
  int readErr = ferror(fp);
  fclose(fp);
  
  if (readErr) {
    posix_log_error("fread", path);
    return -1;
  }
  
  buffer[bytesRead] = '\0';
  return (ssize_t)bytesRead;
}
#endif

// Wrapper for shared library roundTo function
static inline float roundTo(float val, int decimals) { return tankalarm_roundTo(val, decimals); }

static bool isValidPin(const char *pin) {
  if (!pin) {
    return false;
  }
  size_t len = strlen(pin);
  if (len != 4) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    if (!isdigit(pin[i])) {
      return false;
    }
  }
  return true;
}

static bool pinMatches(const char *pin) {
  if (!pin || gConfig.configPin[0] == '\0') {
    return false;
  }
  
  // Ensure both supplied and configured PINs are valid 4-digit PINs
  if (!isValidPin(pin) || !isValidPin(gConfig.configPin)) {
    return false;
  }
  
  // Constant-time 4-byte comparison to prevent timing attacks
  // The admin PIN is always exactly 4 digits when valid.
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < 4; ++i) {
    diff |= (uint8_t)(pin[i] ^ gConfig.configPin[i]);
  }
  
  return (diff == 0);
}

// Forward declaration for addServerSerialLog (full declaration with defaults is in the forward
// declarations block below; this early declaration is needed because isValidClientUid calls
// addServerSerialLog before the main forward declaration block)
static void addServerSerialLog(const char *message, const char *level, const char *source);

// Validate that a client UID will fit in our buffers without truncation
// and has the expected Notecard device UID format (dev: prefix).
// Returns false if UID is empty, too long, or missing the dev: prefix.
static bool isValidClientUid(const char *clientUid) {
  if (!clientUid || strlen(clientUid) == 0) {
    return false;
  }
  
  // Reject UIDs that don't start with "dev:" — these are phantom entries
  // caused by client boot-time race conditions before the Notecard syncs.
  // Valid Blues Notecard device UIDs always begin with "dev:".
  if (strncmp(clientUid, "dev:", 4) != 0) {
    Serial.print(F("WARNING: Rejected non-device UID (missing dev: prefix): "));
    Serial.println(clientUid);
    addServerSerialLog("Rejected invalid client UID (no dev: prefix)", "warn", "telemetry");
    return false;
  }
  
  // Most clientUid buffers in the codebase are 48 bytes (47 chars + null)
  const size_t MAX_CLIENT_UID_LEN = 47;
  
  size_t len = strlen(clientUid);
  if (len > MAX_CLIENT_UID_LEN) {
    Serial.print(F("WARNING: Client UID too long ("));
    Serial.print(len);
    Serial.print(F(" chars): "));
    Serial.println(clientUid);
    return false;
  }
  
  return true;
}

// Validate measurement unit string against known safe values.
// Rejects unknown/garbage strings to prevent display corruption.
static bool isValidMeasurementUnit(const char *unit) {
  if (!unit || unit[0] == '\0') return false;
  static const char *const VALID_UNITS[] = {
    "inches", "psi", "bar", "kpa", "mbar", "m", "ft", "in", "cm",
    "rpm", "gpm", "gal", "l", "ml", "PSI"  // uppercase PSI for legacy compat
  };
  for (size_t i = 0; i < sizeof(VALID_UNITS) / sizeof(VALID_UNITS[0]); ++i) {
    if (strcasecmp(unit, VALID_UNITS[i]) == 0) return true;
  }
  return false;
}


// Forward declaration for respondStatus (defined later in the web server section)
static void respondStatus(EthernetClient &client, int status, const char *message);
static void respondStatus(EthernetClient &client, int status, const String &message);

// Forward declarations for FTP backup/restore functions (defined in FTP section)
static bool performFtpBackup(char *errorOut = nullptr, size_t errorSize = 0);
static bool performFtpRestore(char *errorOut = nullptr, size_t errorSize = 0);

// Forward declaration for purgePendingConfigNotes (defined in Notecard Config Dispatch section)
static uint8_t purgePendingConfigNotes(const char *clientUid);

// Check if authentication is currently rate-limited (non-blocking)
// Returns true if rate-limited, and responds with 429 status
static bool isAuthRateLimited(EthernetClient &client) {
  unsigned long now = millis();
  
  // Check if we're in an active backoff/lockout window (overflow-safe subtraction)
  if (gAuthLockoutDuration > 0 && (now - gAuthLockoutStart) < gAuthLockoutDuration) {
    unsigned long remaining = (gAuthLockoutDuration - (now - gAuthLockoutStart)) / 1000;
    String msg = "Too many failed attempts. Try again in ";
    msg += String(remaining);
    msg += " seconds.";
    respondStatus(client, 429, msg);
    return true;
  }
  
  // Check if we're in lockout period after max failures
  if (gAuthFailureCount >= AUTH_MAX_FAILURES) {
    unsigned long timeSinceFail = now - gLastAuthFailureTime;
    if (timeSinceFail < AUTH_LOCKOUT_DURATION) {
      unsigned long remaining = (AUTH_LOCKOUT_DURATION - timeSinceFail) / 1000;
      String msg = "Too many failed attempts. Try again in ";
      msg += String(remaining);
      msg += " seconds.";
      respondStatus(client, 429, msg);
      return true;
    } else {
      // Lockout period expired, reset counter
      gAuthFailureCount = 0;
      gAuthLockoutDuration = 0;
    }
  }
  
  return false;
}

// Record a failed authentication attempt and calculate lockout duration (overflow-safe)
static void recordAuthFailure() {
  unsigned long now = millis();
  gAuthFailureCount++;
  gLastAuthFailureTime = now;
  gAuthLockoutStart = now;
  
  // Calculate exponential backoff: 1s, 2s, 4s, 8s, 16s
  if (gAuthFailureCount > 0 && gAuthFailureCount <= 5) {
    gAuthLockoutDuration = 1000UL << (gAuthFailureCount - 1);  // 2^(n-1) seconds
  } else if (gAuthFailureCount > AUTH_MAX_FAILURES) {
    // Enter full lockout period
    gAuthLockoutDuration = AUTH_LOCKOUT_DURATION;
  }
}

// Reset auth failure tracking on successful authentication
static void resetAuthFailures() {
  gAuthFailureCount = 0;
  gAuthLockoutDuration = 0;
}

// Require that a valid admin PIN is configured and an active session exists.
// The session was created by a successful PIN login — re-prompting is unnecessary.
// The X-Session header is already validated by the middleware before handlers run.
static bool requireValidPin(EthernetClient &client, const char * /* pinValue */) {
  // Check rate limiting first (non-blocking)
  if (isAuthRateLimited(client)) {
    return false;
  }
  
  // Reject if no active session (server-side logout clears the token)
  if (gSessionToken[0] == '\0') {
    respondStatus(client, 401, "Session expired");
    return false;
  }
  
  if (!isValidPin(gConfig.configPin)) {
    respondStatus(client, 403, "Configure admin PIN before making changes");
    return false;
  }
  
  // Session already validated by middleware — no additional PIN required
  resetAuthFailures();
  return true;
}

// Require an explicit PIN match (used only for changing the admin PIN).
static bool requireExplicitPinMatch(EthernetClient &client, const char *pinValue) {
  if (isAuthRateLimited(client)) {
    return false;
  }
  if (gSessionToken[0] == '\0') {
    respondStatus(client, 401, "Session expired");
    return false;
  }
  if (!isValidPin(gConfig.configPin)) {
    respondStatus(client, 403, "Configure admin PIN before making changes");
    return false;
  }
  if (!pinMatches(pinValue)) {
    recordAuthFailure();
    respondStatus(client, 403, "Invalid PIN");
    return false;
  }
  resetAuthFailures();
  return true;
}

static const char STYLE_CSS[] PROGMEM = R"HTML(:root{--primary:#0066cc;--primary-hover:#004c99;--bg:#f2f2f2;--card:#ffffff;--text:#333333;--muted:#666666;--border:#cccccc;--danger:#cc0000;--success:#28a745;--warning:#ffc107;--chip:#e0e0e0;--radius:0;--space-1:0.4rem;--space-2:0.75rem;--space-3:1rem;--space-4:1.5rem;--space-5:1.75rem;--space-6:2.25rem;--card-border:#d7d7d7;--text-secondary:#6b7280;--focus:#1d4ed8}
body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:var(--bg);color:var(--text);line-height:1.5}
*{box-sizing:border-box}
header{background:var(--card);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:100}
.bar{max-width:1200px;margin:0 auto;padding:var(--space-2) var(--space-3);display:flex;align-items:center;gap:var(--space-5)}
.brand{font-weight:700;font-size:1.1rem;white-space:nowrap;color:#201442}
.login-title{margin:0 0 0.75rem;font-size:1.6rem}
.login-title .brand{font-size:inherit}
.login-form{display:flex;flex-direction:column;align-items:center;gap:0.6rem;margin-top:0.5rem}
.login-form .pin-input{max-width:180px;width:100%}
.login-form .login-button{min-width:120px}
.error{color:var(--danger);font-size:0.9rem;display:none}
.header-actions{display:flex;flex-wrap:wrap;gap:var(--space-2);row-gap:var(--space-1);margin-left:auto}
.pill{text-decoration:none;color:var(--text);padding:0.5rem 1rem;border-radius:var(--radius);font-size:0.875rem;font-weight:600;background:var(--card);border:1px solid var(--border);white-space:nowrap;transition:all 0.2s;margin:0.25rem}
.pill:hover{background:#f0f0f0}
.pill.secondary{background:var(--card);color:var(--muted)}
.pill.secondary:hover{background:#f7f7f7;color:var(--text)}
.pill[href^="http"]{color:var(--primary)}
main{max-width:1200px;margin:var(--space-6) auto;padding:0 var(--space-3);display:grid;gap:var(--space-5)}
.card{background:var(--card);border:1px solid var(--border);border-radius:var(--radius);padding:var(--space-4);box-shadow:none}
h2{margin:0 0 1.5rem;font-size:1.25rem;font-weight:600}
h3{margin:var(--space-4) 0 var(--space-3);font-size:1rem;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:0.05em}
.form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:var(--space-4);margin-bottom:var(--space-5)}
.field{display:flex;flex-direction:column;gap:var(--space-1)}
.field span{font-size:0.875rem;font-weight:500;color:var(--text)}
input,select,textarea{padding:0.6rem var(--space-2);border:1px solid var(--border);border-radius:var(--radius);font-size:0.95rem;background:var(--card);transition:border-color 0.15s;width:100%}
input:focus,select:focus,textarea:focus{outline:none;border-color:var(--primary);box-shadow:none}
button:focus-visible,input:focus-visible,select:focus-visible,textarea:focus-visible,a:focus-visible{outline:2px solid var(--focus);outline-offset:2px}
button{background:var(--primary);color:white;border:1px solid var(--primary);padding:0.6rem 1.2rem;border-radius:var(--radius);font-weight:600;cursor:pointer;font-size:0.95rem;transition:all 0.2s;margin:0.25rem}
button:hover{background:var(--primary-hover);transform:translateY(-1px);box-shadow:0 2px 4px rgba(0,0,0,0.1)}
button:active{transform:translateY(0);box-shadow:none}
#loading-overlay{position:fixed;inset:0;background:rgba(255,255,255,0.8);backdrop-filter:blur(4px);z-index:9999;display:flex;align-items:center;justify-content:center;transition:opacity 0.3s;animation:overlayFade 10s forwards}
#loading-overlay.hidden{opacity:0;pointer-events:none}
.spinner{width:32px;height:32px;border:3px solid var(--border);border-top-color:var(--primary);border-radius:50%;animation:spin 1s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
@keyframes overlayFade{0%,80%{opacity:1}100%{opacity:0;visibility:hidden;pointer-events:none}}
button.secondary{background:var(--card);color:var(--text);border:1px solid var(--border)}
button.secondary:hover{background:#f0f0f0}
button.tertiary{background:#e6f2ff;color:#0066cc;border:1px solid #b3d9ff}
button.tertiary:hover{background:#cce5ff;border-color:#80c3ff}
button.btn-small{padding:0.25rem 0.6rem;font-size:0.75rem;margin:0.15rem}
button.remove-btn{background:transparent;color:var(--danger);padding:0.25rem 0.5rem;font-size:0.85rem;border:1px solid transparent}
button.remove-btn:hover{background:#fee2e2;border-color:#f5b5b5}
.pause-btn{font-size:0.85rem;padding:0.3rem 0.85rem;background:transparent;border:1px solid var(--danger);color:var(--danger);margin-right:var(--space-1)}
.pause-btn:hover{background:var(--danger);color:white}
.pause-btn.paused{background:var(--danger);color:white;border-color:var(--danger)}
.toggle-group{display:flex;flex-wrap:wrap;gap:var(--space-5);margin-bottom:var(--space-5)}
.toggle{display:flex;align-items:center;gap:0.5rem;cursor:pointer}
.actions{display:flex;gap:var(--space-4);align-items:center;flex-wrap:wrap}
.actions + .actions{margin-top:var(--space-3)}
#toast{position:fixed;bottom:var(--space-6);right:var(--space-6);background:var(--primary);color:white;padding:0.75rem 1.5rem;border-radius:var(--radius);border:1px solid var(--border);box-shadow:none;transform:translateY(150%);transition:transform 0.3s;z-index:200}
#toast.show{transform:translateY(0)}
.config-pending{background:#fef3c7;color:#92400e;padding:0.75rem;border-radius:var(--radius);border:1px solid #fbbf24;margin-bottom:1rem}
.config-success{background:#d1fae5;color:#065f46;padding:0.75rem;border-radius:var(--radius);border:1px solid #10b981;margin-bottom:1rem}
.config-error{background:#fee2e2;color:#991b1b;padding:0.75rem;border-radius:var(--radius);border:1px solid #ef4444;margin-bottom:1rem}
.modal{position:fixed;inset:0;background:rgba(0,0,0,0.5);display:flex;align-items:center;justify-content:center;z-index:150}
.modal.hidden{display:none}
.hidden{display:none}
.collapsible-section{display:none}
.collapsible-section.visible{display:block}
.modal-card{background:var(--card);padding:2rem;border-radius:var(--radius);width:100%;max-width:400px;box-shadow:none;border:1px solid var(--border)}
.modal-content{background:var(--card);padding:2rem;border-radius:var(--radius);width:100%;max-width:480px;box-shadow:none;border:1px solid var(--border)}
.modal-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:var(--space-3)}
.modal-close{background:transparent;border:none;font-size:1.25rem;line-height:1;color:var(--muted);cursor:pointer;padding:0}
.hidden{display:none!important}
.pin-chip{display:inline-flex;align-items:center;padding:2px 8px;border-radius:var(--radius);font-size:0.75rem;font-weight:600;background:#fee2e2;color:#ef4444}
.pin-chip.ok{background:#d1fae5;color:#059669}
.tooltip-icon{display:inline-flex;align-items:center;justify-content:center;width:16px;height:16px;border-radius:var(--radius);background:var(--muted);color:white;font-size:10px;margin-left:6px;cursor:help;position:relative}
.tooltip-icon:hover::after,.tooltip-icon:focus::after{content:attr(data-tooltip);position:absolute;left:50%;top:calc(100% + 8px);transform:translateX(-50%);background:#1f2937;color:white;padding:8px 12px;border-radius:6px;font-size:0.75rem;line-height:1.4;white-space:nowrap;max-width:300px;width:max-content;z-index:1000;box-shadow:0 4px 6px rgba(0,0,0,0.1);pointer-events:none}
.tooltip-icon:hover::before,.tooltip-icon:focus::before{content:'';position:absolute;left:50%;top:calc(100% + 2px);transform:translateX(-50%);border:6px solid transparent;border-bottom-color:#1f2937;z-index:1001;pointer-events:none}
.tooltip-icon:focus{outline:2px solid var(--primary);outline-offset:2px}
table{width:100%;border-collapse:collapse;border:1px solid var(--border);font-size:0.95rem}
thead th{background:#f7f7f7;color:var(--text);text-align:left;padding:0.6rem;border-bottom:1px solid var(--border)}
tbody td{padding:0.55rem;border-bottom:1px solid #e5e5e5;vertical-align:top}
tbody tr:nth-child(even){background:#fafafa}
.controls{display:flex;flex-wrap:wrap;gap:var(--space-2);align-items:center;margin-bottom:var(--space-3)}
.controls-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:var(--space-3);align-items:end;margin-bottom:var(--space-3)}
.control-group{display:flex;flex-direction:column;gap:var(--space-1)}
.control-group.compact{flex-direction:row;align-items:center;gap:var(--space-2)}
.log-container{border:1px solid var(--border);background:#fcfcfc;padding:var(--space-2);height:260px;overflow:auto;font-family:ui-monospace,Consolas,monospace;font-size:0.85rem}
.log-container.slim{height:180px}
.log-entry{display:flex;gap:var(--space-2);padding:0.25rem 0;border-bottom:1px dashed #e0e0e0}
.log-entry:last-child{border-bottom:none}
.log-time{color:var(--muted);min-width:90px}
.log-meta{color:var(--text-secondary);min-width:120px}
.empty-state{color:var(--muted);padding:var(--space-3);text-align:center}
.recipient-list{border:1px solid var(--border);background:#fafafa;padding:var(--space-2);margin-bottom:var(--space-2)}
.recipient-item{display:flex;justify-content:space-between;align-items:center;padding:var(--space-2);border-bottom:1px solid var(--border)}
.recipient-item:last-child{border-bottom:none}
.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:var(--space-3)}
.stat-box{border:1px solid var(--border);padding:var(--space-3);background:#fafafa}
.stat-value{font-size:1.4rem;font-weight:700}
.stat-label{color:var(--muted);font-size:0.85rem}
.stats-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:var(--space-3);margin-bottom:var(--space-3)}
.stat-item{border:1px solid var(--border);padding:var(--space-2);background:#fafafa;text-align:center}
.stat-item strong{display:block;font-size:1.2rem}
.panel-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:var(--space-3)}
.client-panel{border:1px solid var(--border);padding:var(--space-3);background:#fafafa}
.panel-head{display:flex;justify-content:space-between;gap:var(--space-2);margin-bottom:var(--space-2)}
.panel-title{font-weight:700}
.panel-subtitle,.panel-meta{color:var(--muted);font-size:0.85rem}
.panel-actions{display:flex;gap:var(--space-2);flex-wrap:wrap;align-items:center}
.chip-row{display:flex;flex-wrap:wrap;gap:var(--space-2);margin:var(--space-2) 0}
.chip{background:var(--chip);border:1px solid var(--border);padding:2px 8px;font-size:0.75rem}
.ghost{background:transparent;border:1px solid var(--border);color:var(--text)}
.level-input-group{display:flex;gap:var(--space-1);align-items:center}
.info-box{border:1px solid var(--border);background:#fafafa;padding:var(--space-3);font-size:0.9rem;color:var(--text)}
.calibration-status{display:inline-block;padding:2px 6px;border:1px solid var(--border);font-size:0.75rem}
.calibration-status.calibrated{background:#d1fae5;color:#065f46;border-color:#a7f3d0}
.calibration-status.learning{background:#fef3c7;color:#92400e;border-color:#fde68a}
.calibration-status.uncalibrated{background:#fee2e2;color:#991b1b;border-color:#fecaca}
.drift-indicator{display:inline-block;padding:2px 6px;border:1px solid var(--border);font-size:0.75rem}
.drift-indicator.low{background:#d1fae5;color:#065f46;border-color:#a7f3d0}
.drift-indicator.medium{background:#fef3c7;color:#92400e;border-color:#fde68a}
.drift-indicator.high{background:#fee2e2;color:#991b1b;border-color:#fecaca}
.btn-reset{padding:4px 8px;font-size:0.75rem;background:#fee2e2;color:#991b1b;border:1px solid #fecaca;cursor:pointer;border-radius:4px}
.btn-reset:hover{background:#fecaca}
.quality-warning{cursor:help;margin-left:4px}
.sensor-link{color:var(--accent);text-decoration:none;cursor:pointer}
.sensor-link:hover{text-decoration:underline}
.chart-container{border:1px solid var(--border);padding:var(--space-2);height:320px;background:#ffffff}
.sensor-card{border:1px solid var(--border);padding:var(--space-3);margin-bottom:var(--space-3);background:#fafafa;border-radius:var(--radius)}
.sensor-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:var(--space-3);padding-bottom:var(--space-2);border-bottom:1px solid var(--border)}
.sensor-title{font-weight:600;font-size:1rem}
.add-section-btn{background:var(--card);color:var(--muted);border:1px dashed var(--border);padding:0.4rem 0.8rem;font-size:0.85rem;cursor:pointer;margin-top:var(--space-2);border-radius:var(--radius)}
.add-section-btn:hover{background:#f0f0f0;color:var(--text);border-color:var(--text)}
.client-list{max-height:400px;overflow-y:auto}
.client-item{padding:var(--space-3);border-bottom:1px solid var(--border);cursor:pointer;transition:background 0.15s}
.client-item:hover{background:#f0f0f0}
.client-item:last-child{border-bottom:none}
@media (max-width: 900px){
  .bar{flex-direction:column;align-items:flex-start;gap:var(--space-2)}
  .header-actions{width:100%;flex-wrap:wrap}
  .controls-grid{grid-template-columns:1fr}
  .panel-head{flex-direction:column;align-items:flex-start}
}
@media (max-width: 600px){
  main{margin:var(--space-5) auto;padding:0 var(--space-2)}
  .actions{gap:var(--space-2)}
  .log-container{height:200px}
}
)HTML";

static const char SERVER_SETTINGS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Server Settings - Tank Alarm</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Server Configuration</h2><form id="settingsForm"><h3>Blues Notehub</h3><div class="form-grid"><label class="field"><span>Product UID <span style="color:var(--danger);">*</span></span><input id="productUidInput" type="text" placeholder="com.company.product:project" required></label></div><p style="color:var(--muted);font-size:0.85rem;margin:-8px 0 16px;">Required. The Product UID from your Blues Notehub project (e.g. com.company.product:project). Changing this requires a device restart to fully apply.</p><h3>Server SMS Alert Recipients</h3><p style="color:var(--muted);font-size:0.85rem;margin-bottom:12px;">Contacts who receive SMS alerts for server events (e.g., power restoration after outage).</p><div id="smsRecipientsList" class="recipient-list"><div class="empty-state">No SMS recipients configured.</div></div><div class="actions" style="margin-bottom:16px;"><button type="button" class="secondary" id="addSmsRecipientBtn">+ Add SMS Recipient</button></div><div class="toggle-group" style="margin-top:-12px;"><label class="toggle"><span>Server down (power loss &gt; 24h)<span class="tooltip-icon" tabindex="0" data-tooltip="Sends an SMS if the server was offline for at least 24 hours before reboot.">?</span></span><input type="checkbox" id="serverDownSmsToggle" checked></label></div><h3>Daily Email Report Recipients</h3><p style="color:var(--muted);font-size:0.85rem;margin-bottom:12px;">Contacts who receive the daily tank level summary email.</p><div class="form-grid"><label class="field"><span>Daily Email Time (HH:MM, UTC)</span><input id="dailyEmailTimeInput" type="time" value="05:00"></label></div><div id="dailyRecipientsList" class="recipient-list"><div class="empty-state">No daily report recipients configured.</div></div><div class="actions" style="margin-bottom:16px;"><div id="dailyRecipientControls" style="display:flex;gap:8px;align-items:center;"><select id="dailyRecipientDropdown" style="min-width:250px;"><option value="">Choose a contact...</option></select><button type="button" id="addSelectedDailyRecipient" class="secondary">Add</button></div><a class="pill secondary" href="/contacts" id="addNewContactLink" style="display:none;">+ Add New Contact</a><a class="pill secondary" href="/email-format" style="margin-left:8px;">Email Formatting</a></div><h3>Security</h3><div class="actions" style="margin-bottom: 24px;"><button type="button" class="secondary" id="changePinBtn">Change Admin PIN</button><span id="pinBadge" class="pin-chip hidden">PIN SET</span><span id="pinStatus" style="margin-left:12px;font-size:0.9rem;color:var(--muted)"></span></div><h3>FTP Backup & Restore</h3><div class="form-grid"><label class="field"><span>FTP Host</span><input id="ftpHost" type="text" placeholder="192.168.1.50"></label><label class="field"><span>FTP Port</span><input id="ftpPort" type="number" min="1" max="65535" value="21"></label><label class="field"><span>FTP User</span><input id="ftpUser" type="text" placeholder="user"></label><label class="field"><span>FTP Password <small style="color:var(--muted);font-weight:400;">(leave blank to keep)</small></span><input id="ftpPass" type="password" autocomplete="off"></label><label class="field"><span>FTP Path</span><input id="ftpPath" type="text" placeholder="/tankalarm/server"></label><label class="field"><span>TLS Server Name (SNI)</span><input id="ftpsTlsServerName" type="text" placeholder="ftp.example.com"></label><label class="field"><span>SHA-256 Fingerprint</span><input id="ftpsFingerprint" type="text" placeholder="64 hex chars" maxlength="64"></label><label class="field"><span>FTPS Trust Mode</span><select id="ftpsTrustMode"><option value="0">Fingerprint</option><option value="1">Imported Certificate</option></select></label></div><div class="toggle-group"><label class="toggle"><span>Enable FTP</span><input type="checkbox" id="ftpEnabled"></label><label class="toggle"><span>Passive Mode</span><input type="checkbox" id="ftpPassive" checked></label><label class="toggle"><span>Enable FTPS (Explicit TLS)</span><input type="checkbox" id="ftpsEnabled"></label><label class="toggle"><span>Auto-backup on save</span><input type="checkbox" id="ftpBackupOnChange"></label><label class="toggle"><span>Restore on boot</span><input type="checkbox" id="ftpRestoreOnBoot"></label></div><p style="color:var(--muted);font-size:0.85rem;margin-top:8px;">Fingerprint trust mode is currently supported. Imported certificate mode is reserved for a follow-up update.</p><div class="actions"><button type="button" id="ftpBackupNow">Backup Now</button><button type="button" class="secondary" id="ftpRestoreNow">Restore Now</button></div><h3>Viewer Device</h3><p style="color:var(--muted);font-size:0.85rem;margin-bottom:12px;">Enable periodic viewer summary publishing via Notecard. Only enable this if you have a Viewer Opta device set up and connected.</p><div class="toggle-group"><label class="toggle"><span>Enable Viewer Summary<span class="tooltip-icon" tabindex="0" data-tooltip="When enabled, the server publishes sensor summary data to a Viewer Opta device every 6 hours via Notecard.">?</span></span><input type="checkbox" id="viewerEnabled"></label></div><h3>Update Policy</h3><p style="color:var(--muted);font-size:0.85rem;margin-bottom:12px;">Choose how the server handles firmware updates. Alert modes notify you on the dashboard when an update is available. Automatic modes apply updates without user intervention.</p><div class="form-grid"><label class="field" style="min-width:260px;"><span>Update Policy</span><select id="updatePolicy"><option value="0">Disabled</option><option value="1">Alert for DFU update available</option><option value="2">Alert for GitHub update available</option><option value="3">Update from GitHub automatically</option><option value="4">Update from DFU automatically</option></select></label></div><div class="toggle-group" style="margin-top:12px;"><label class="toggle"><span>Alert when connected clients report outdated firmware<span class="tooltip-icon" tabindex="0" data-tooltip="When enabled, a banner appears on the dashboard if any connected client is running a firmware version older than the latest GitHub release.">?</span></span><input type="checkbox" id="checkClientVersionAlerts" checked></label><label class="toggle"><span>Alert when viewer reports outdated firmware<span class="tooltip-icon" tabindex="0" data-tooltip="When enabled, a banner appears on the dashboard if the connected Viewer device reports a firmware version older than the latest GitHub release.">?</span></span><input type="checkbox" id="checkViewerVersionAlerts" checked></label></div><div class="actions"><button type="submit">Save Settings</button></div></form></div><div class="card"><h2>Tools & System Info</h2><h3>System Status</h3><div class="form-grid"><div class="field"><span>Firmware Version</span><div id="fwVersionDisplay" style="padding:10px 12px;background:var(--chip);border:1px solid var(--card-border)">Loading...</div></div><div class="field"><span>Build Date</span><div id="fwBuildDisplay" style="padding:10px 12px;background:var(--chip);border:1px solid var(--card-border)">Loading...</div></div><div class="field"><span>Firmware Last Updated</span><div id="fwLastUpdatedDisplay" style="padding:10px 12px;background:var(--chip);border:1px solid var(--card-border)">Loading...</div></div><div class="field"><span>Notecard Supply (V+ rail)</span><div id="serverVoltageDisplay" style="padding:10px 12px;background:var(--chip);border:1px solid var(--card-border)">Loading...</div></div><div class="field"><span>Server Time</span><div id="serverTimeDisplay" style="padding:10px 12px;background:var(--chip);border:1px solid var(--card-border)">Loading...</div></div></div><h3>Notecard Status</h3><div id="notecardStatusPanel" style="padding:16px;background:var(--chip);border:1px solid var(--card-border);border-radius:var(--radius);margin-bottom:16px;"><div style="display:flex;align-items:center;gap:10px;margin-bottom:8px;"><span id="notecardStatusDot" style="display:inline-block;width:12px;height:12px;border-radius:50%;background:#888;"></span><strong id="notecardStatusLabel">Checking...</strong></div><div class="form-grid" style="margin:0;"><div class="field"><span>Connection</span><div id="ncConnStatus" style="padding:6px 10px;background:var(--bg);border:1px solid var(--card-border);font-size:0.9rem;">--</div></div><div class="field"><span>Product UID</span><div id="ncProductUid" style="padding:6px 10px;background:var(--bg);border:1px solid var(--card-border);font-size:0.9rem;">--</div></div><div class="field"><span>Server UID</span><div id="ncServerUid" style="padding:6px 10px;background:var(--bg);border:1px solid var(--card-border);font-size:0.9rem;">--</div></div><div class="field"><span>Sync Mode</span><div id="ncSyncMode" style="padding:6px 10px;background:var(--bg);border:1px solid var(--card-border);font-size:0.9rem;">--</div></div></div><div class="actions" style="margin-top:12px;"><button type="button" class="secondary" id="ncRefreshBtn">Refresh Notecard Status</button></div></div><h3>Firmware Update (DFU)</h3><div id="dfuStatus" style="padding:12px;background:var(--chip);border:1px solid var(--card-border);margin-bottom:12px"><span id="dfuStatusText">Checking for updates...</span></div><div class="actions"><button type="button" id="dfuCheckBtn" class="secondary">Check for Update</button><button type="button" id="dfuEnableBtn" disabled>Install Update</button></div><p style="color:var(--muted);font-size:0.85rem;margin-top:8px">Manual install uses the policy source. GitHub Alert/Auto policies try GitHub Direct first and fall back to Notehub DFU. All other policies use Notehub DFU.</p><h3>Quick Access</h3><div class="actions"><button type="button" class="secondary" id="pauseBodyBtn">Pause Server</button><a class="pill" href="/serial-monitor">Open Serial Monitor</a><a class="pill" href="/transmission-log">Transmission Log</a><a class="pill secondary" href="https://github.com/SenaxInc/SenaxTankAlarm/blob/master/Tutorials/Tutorials-112025/SERVER_INSTALLATION_GUIDE.md" target="_blank" title="View Server Installation Guide">Help</a></div></div></main><div id="toast"></div><div id="contactSelectModal" class="modal hidden"><div class="modal-content"><div class="modal-header"><h2 id="contactSelectTitle">Add Recipient</h2><button class="modal-close" onclick="closeContactSelectModal()">&times;</button></div><form id="contactSelectForm"><div class="form-grid"><div class="form-field"><label>Select Contact</label><select id="contactSelectDropdown" required><option value="">Choose a contact...</option></select></div></div><div style="display: flex; gap: 12px; justify-content: flex-end; margin-top: 24px;"><button type="button" class="btn btn-secondary" onclick="closeContactSelectModal()">Cancel</button><button type="submit" class="btn btn-primary">Add</button></div></form></div></div><div id="pinModal" class="modal hidden"><div class="modal-card"><div class="modal-badge" id="pinSessionBadge">Session</div><h2 id="pinModalTitle">Set Admin PIN</h2><p id="pinModalDescription">Enter a 4-digit PIN to unlock configuration changes.</p><form id="pinForm"><label class="field hidden" id="pinCurrentGroup"><span>Current PIN</span><input type="password" id="pinCurrentInput" inputmode="numeric" pattern="\d*" maxlength="4" autocomplete="off"></label><label class="field" id="pinPrimaryGroup"><span id="pinPrimaryLabel">PIN</span><input type="password" id="pinInput" inputmode="numeric" pattern="\d*" maxlength="4" autocomplete="off" required placeholder="4 digits" aria-describedby="pinHint" title="Enter exactly four digits (0-9)"><small class="pin-hint" id="pinHint">Use exactly 4 digits (0-9). The PIN is kept locally in this browser for 90 days.</small></label><label class="field hidden" id="pinConfirmGroup"><span>Confirm PIN</span><input type="password" id="pinConfirmInput" inputmode="numeric" pattern="\d*" maxlength="4" autocomplete="off"></label><div class="actions"><button type="submit" id="pinSubmit">Save PIN</button><button type="button" class="secondary" id="pinCancel">Cancel</button></div></form></div></div><script>document.addEventListener('DOMContentLoaded', async () => {try{const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);const state={pin:null,pinConfigured:false,pendingAction:null,contacts:[],smsAlertRecipients:[],dailyReportRecipients:[],contactSelectMode:null};
const getEl=(id)=>document.getElementById(id);const els={pauseBtn:getEl('pauseBtn'),pauseBodyBtn:getEl('pauseBodyBtn'),toast:getEl('toast'),form:getEl('settingsForm'),productUid:getEl('productUidInput'),serverDownSmsToggle:getEl('serverDownSmsToggle'),dailyEmailTime:getEl('dailyEmailTimeInput'),ftpEnabled:getEl('ftpEnabled'),ftpPassive:getEl('ftpPassive'),ftpsEnabled:getEl('ftpsEnabled'),ftpsTrustMode:getEl('ftpsTrustMode'),ftpsFingerprint:getEl('ftpsFingerprint'),ftpsTlsServerName:getEl('ftpsTlsServerName'),ftpBackupOnChange:getEl('ftpBackupOnChange'),ftpRestoreOnBoot:getEl('ftpRestoreOnBoot'),ftpHost:getEl('ftpHost'),ftpPort:getEl('ftpPort'),ftpUser:getEl('ftpUser'),ftpPass:getEl('ftpPass'),ftpPath:getEl('ftpPath'),ftpBackupNow:getEl('ftpBackupNow'),ftpRestoreNow:getEl('ftpRestoreNow'),changePinBtn:getEl('changePinBtn'),pinStatus:getEl('pinStatus'),pinBadge:getEl('pinBadge'),smsRecipientsList:getEl('smsRecipientsList'),dailyRecipientsList:getEl('dailyRecipientsList'),addSmsRecipientBtn:getEl('addSmsRecipientBtn'),dailyRecipientDropdown:getEl('dailyRecipientDropdown'),addSelectedDailyRecipient:getEl('addSelectedDailyRecipient'),contactSelectModal:getEl('contactSelectModal'),contactSelectTitle:getEl('contactSelectTitle'),contactSelectDropdown:getEl('contactSelectDropdown'),contactSelectForm:getEl('contactSelectForm'),viewerEnabled:getEl('viewerEnabled'),updatePolicy:getEl('updatePolicy'),checkClientVersionAlerts:getEl('checkClientVersionAlerts'),checkViewerVersionAlerts:getEl('checkViewerVersionAlerts')};if(els.pauseBtn)els.pauseBtn.addEventListener('click',togglePause);if(els.pauseBodyBtn)els.pauseBodyBtn.addEventListener('click',togglePause);const pinEls={modal:getEl('pinModal'),title:getEl('pinModalTitle'),desc:getEl('pinModalDescription'),form:getEl('pinForm'),currentGroup:getEl('pinCurrentGroup'),currentInput:getEl('pinCurrentInput'),primaryGroup:getEl('pinPrimaryGroup'),primaryLabel:getEl('pinPrimaryLabel'),input:getEl('pinInput'),confirmGroup:getEl('pinConfirmGroup'),confirmInput:getEl('pinConfirmInput'),submit:getEl('pinSubmit'),cancel:getEl('pinCancel'),badge:getEl('pinSessionBadge')};)HTML"
R"HTML(let pinMode='unlock';state.paused=false;funct)HTML" R"HTML(ion showToast(message, isError){if(els.toast)els.toast.textContent=message;if(els.toast)els.toast.style.background=isError?'#dc2626':'#0284c7';if(els.toast)els.toast.classList.add('show');setTimeout(()=>{if(els.toast)els.toast.classList.remove('show')},2500);})HTML"
R"HTML(funct)HTML" R"HTML(ion escapeHtml(text){const div=document.createElement('div');div.textContent=text;return div.innerHTML;})HTML"
R"HTML(funct)HTML" R"HTML(ion renderPauseButtons(){if(els.pauseBtn){els.pauseBtn.style.display=state.paused?'':'none';els.pauseBtn.textContent='Unpause';els.pauseBtn.title='Resume data flow';}if(els.pauseBodyBtn){els.pauseBodyBtn.style.display=state.paused?'none':'';els.pauseBodyBtn.textContent='Pause Server';els.pauseBodyBtn.title='Pause data flow';}})HTML"
R"HTML(async funct)HTML" R"HTML(ion togglePause(){const targetPaused=!state.paused;try{const res=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:targetPaused})});if(!res.ok){const text=await res.text();throw new Error(text||'Pause toggle failed');}const data=await res.json();state.paused=!!data.paused;renderPauseButtons();showToast(state.paused?'Paused for maintenance':'Resumed');}catch(err){showToast(err.message||'Pause toggle failed',true);}})HTML"
R"HTML(funct)HTML" R"HTML(ion showPinModal(mode, callback){if(!pinEls.modal)return;pinMode=mode;state.pendingAction=callback;pinEls.form.reset();pinEls.currentGroup.classList.add('hidden');pinEls.confirmGroup.classList.add('hidden');pinEls.badge.classList.add('hidden');pinEls.currentInput.required=false;pinEls.confirmInput.required=false;if(mode==='setup'){pinEls.title.textContent='Set Admin PIN';pinEls.desc.textContent='Create a 4-digit PIN to secure server settings.';pinEls.primaryLabel.textContent='New PIN';pinEls.confirmGroup.classList.remove('hidden');pinEls.confirmInput.required=true;pinEls.submit.textContent='Set PIN';}else if(mode==='change'){pinEls.title.textContent='Change Admin PIN';pinEls.desc.textContent='Enter your current PIN and a new PIN.';pinEls.currentGroup.classList.remove('hidden');pinEls.currentInput.required=true;pinEls.primaryLabel.textContent='New PIN';pinEls.confirmGroup.classList.remove('hidden');pinEls.confirmInput.required=true;pinEls.submit.textContent='Change PIN';}else{pinEls.title.textContent='Admin Access Required';pinEls.desc.textContent='Enter your 4-digit PIN to continue.';pinEls.primaryLabel.textContent='PIN';pinEls.badge.classList.remove('hidden');pinEls.submit.textContent='Unlock';}pinEls.modal.classList.remove('hidden');if(mode==='change'){pinEls.currentInput.focus();}else{pinEls.input.focus();}})HTML"
R"HTML(funct)HTML" R"HTML(ion hidePinModal(){if(pinEls.modal)pinEls.modal.classList.add('hidden');state.pendingAction=null;})HTML"
R"HTML(async funct)HTML" R"HTML(ion handlePinSubmit(e){e.preventDefault();const pin=pinEls.input.value.trim();if(pinMode==='unlock'){if(pin.length!==4){showToast('PIN must be 4 digits',true);return;}state.pin=pin;hidePinModal();if(state.pendingAction)state.pendingAction();return;}const confirm=pinEls.confirmInput.value.trim();if(pin!==confirm){showToast('PINs do not match',true);return;}const payload={newPin:pin};if(pinMode==='change'){payload.pin=pinEls.currentInput.value.trim();}try{const res=await fetch('/api/pin',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});if(!res.ok){const text=await res.text();throw new Error(text||'Failed to update PIN');}const wasConfigured=state.pinConfigured;state.pin=pin;state.pinConfigured=true;showToast(wasConfigured?'PIN changed successfully':'PIN set successfully');hidePinModal();updatePinButton();}catch(err){showToast(err.message,true);if(els.pinStatus){els.pinStatus.textContent='Error: '+err.message;els.pinStatus.style.color='#ef4444';}}}if(pinEls.form)pinEls.form.addEventListener('submit', handlePinSubmit);if(pinEls.cancel)pinEls.cancel.addEventListener('click', hidePinModal);)HTML"
R"HTML(funct)HTML" R"HTML(ion updatePinButton(){if(state.pinConfigured){if(els.changePinBtn)els.changePinBtn.textContent='Change Admin PIN';if(els.pinStatus){els.pinStatus.textContent='PIN configured';els.pinStatus.style.color='#10b981';}if(els.pinBadge){els.pinBadge.textContent='PIN SET';els.pinBadge.classList.add('ok');els.pinBadge.classList.remove('hidden');}}else{if(els.changePinBtn)els.changePinBtn.textContent='Set Admin PIN';if(els.pinStatus){els.pinStatus.textContent='No PIN set';els.pinStatus.style.color='#f59e0b';}if(els.pinBadge){els.pinBadge.classList.remove('ok');els.pinBadge.classList.add('hidden');}}}if(els.changePinBtn)els.changePinBtn.addEventListener('click', ()=>{if(state.pinConfigured){showPinModal('change');}else{showPinModal('setup');}});)HTML"
R"HTML(funct)HTML" R"HTML(ion requestPin(callback){callback();})HTML"
R"HTML(funct)HTML" R"HTML(ion renderSmsRecipients(){const container=els.smsRecipientsList;if(!container)return;if(state.smsAlertRecipients.length===0){container.innerHTML='<div class="empty-state">No SMS recipients configured. Add contacts with phone numbers from the Contacts page first.</div>';return;}container.innerHTML=state.smsAlertRecipients.map(recipientId=>{const contact=state.contacts.find(c=>c.id===recipientId);if(!contact)return'';return`<div class="recipient-item"><div><strong>${escapeHtml(contact.name)}</strong>${contact.phone?` - ${escapeHtml(contact.phone)}`:''}</div><button type="button" class="btn btn-small btn-danger" data-recipient-id="${escapeHtml(recipientId)}" data-type="sms">Remove</button></div>`;}).filter(Boolean).join('');container.querySelectorAll('[data-type="sms"]').forEach(btn=>{btn.addEventListener('click',()=>removeSmsRecipient(btn.dataset.recipientId));});}
funct)HTML" R"HTML(ion renderDailyRecipients(){const container=els.dailyRecipientsList;if(!container)return;if(state.dailyReportRecipients.length===0){container.innerHTML='<div class="empty-state">No daily report recipients configured. Add contacts with email addresses from the Contacts page first.</div>';return;}container.innerHTML=state.dailyReportRecipients.map(recipientId=>{const contact=state.contacts.find(c=>c.id===recipientId);if(!contact)return'';return`<div class="recipient-item"><div><strong>${escapeHtml(contact.name)}</strong>${contact.email?` - ${escapeHtml(contact.email)}`:''}</div><button type="button" class="btn btn-small btn-danger" data-recipient-id="${escapeHtml(recipientId)}" data-type="daily">Remove</button></div>`;}).filter(Boolean).join('');container.querySelectorAll('[data-type="daily"]').forEach(btn=>{btn.addEventListener('click',()=>removeDailyRecipient(btn.dataset.recipientId));});}
funct)HTML" R"HTML(ion removeSmsRecipient(recipientId){state.smsAlertRecipients=state.smsAlertRecipients.filter(r=>r!==recipientId);renderSmsRecipients();saveContactsData();}
funct)HTML" R"HTML(ion removeDailyRecipient(recipientId){state.dailyReportRecipients=state.dailyReportRecipients.filter(r=>r!==recipientId);renderDailyRecipients();saveContactsData();}
funct)HTML" R"HTML(ion openContactSelectModal(mode){state.contactSelectMode=mode;const dropdown=mode==='sms'?els.contactSelectDropdown:els.dailyRecipientDropdown;dropdown.innerHTML='<option value="">Choose a contact...</option>';const existingRecipients=mode==='sms'?state.smsAlertRecipients:state.dailyReportRecipients;const filterField=mode==='sms'?'phone':'email';state.contacts.forEach(contact=>{if(contact[filterField]&&!existingRecipients.includes(contact.id)){const option=document.createElement('option');option.value=contact.id;option.textContent=`${contact.name} (${contact[filterField]})`;dropdown.appendChild(option);}});if(mode==='sms'){if(dropdown.options.length===1){showToast('No contacts with phone numbers available',true);return;}els.contactSelectTitle.textContent='Add SMS Recipient';els.contactSelectModal.classList.remove('hidden');}else{const dailyControls=getEl('dailyRecipientControls');const addNewLink=getEl('addNewContactLink');if(dropdown.options.length===1){if(dailyControls)dailyControls.style.display='none';if(addNewLink)addNewLink.style.display='inline-block';}else{if(dailyControls)dailyControls.style.display='flex';if(addNewLink)addNewLink.style.display='none';}}}
window.closeContactSelectModal=funct)HTML" R"HTML(ion(){els.contactSelectModal.classList.add('hidden');state.contactSelectMode=null;};els.contactSelectForm.addEventListener('submit',(e)=>{e.preventDefault();const contactId=els.contactSelectDropdown.value;if(!contactId){showToast('Please select a contact',true);return;}if(state.contactSelectMode==='sms'){if(!state.smsAlertRecipients.includes(contactId)){state.smsAlertRecipients.push(contactId);renderSmsRecipients();saveContactsData();}}closeContactSelectModal();});els.addSmsRecipientBtn.addEventListener('click',()=>openContactSelectModal('sms'));if(els.addSelectedDailyRecipient){els.addSelectedDailyRecipient.addEventListener('click',()=>{const contactId=els.dailyRecipientDropdown.value;if(!contactId){showToast('Please select a contact',true);return;}if(!state.dailyReportRecipients.includes(contactId)){state.dailyReportRecipients.push(contactId);renderDailyRecipients();openContactSelectModal('daily');saveContactsData();}});}loadSettings().then(()=>openContactSelectModal('daily'));)HTML"
R"HTML(async funct)HTML" R"HTML(ion loadContactsData(){try{const res=await fetch('/api/contacts');if(!res.ok)throw new Error('Failed to load contacts');const data=await res.json();state.contacts=data.contacts||[];state.smsAlertRecipients=data.smsAlertRecipients||[];state.dailyReportRecipients=data.dailyReportRecipients||[];renderSmsRecipients();renderDailyRecipients();}catch(err){console.error('Failed to load contacts:',err);}}
async funct)HTML" R"HTML(ion saveContactsData(){try{const res=await fetch('/api/contacts',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({contacts:state.contacts,smsAlertRecipients:state.smsAlertRecipients,dailyReportRecipients:state.dailyReportRecipients})});if(!res.ok)throw new Error('Failed to save contacts');showToast('Recipients updated');}catch(err){showToast('Failed to save: '+err.message,true);}}
async funct)HTML" R"HTML(ion loadSettings(){if(els.pinStatus){els.pinStatus.textContent='Loading...';els.pinStatus.style.color='var(--muted)';}try{const res=await fetch('/api/clients?summary=1');if(!res.ok)throw new Error('Server returned '+res.status);const data=await res.json();const s=(data&&data.srv)||{};const hour=typeof s.dh==='number'?s.dh:5;const minute=typeof s.dm==='number'?s.dm:0;const timeStr=String(hour).padStart(2,'0')+':'+String(minute).padStart(2,'0');state.pinConfigured=!!s.pc;state.paused=!!s.ps;updatePinButton();renderPauseButtons();if(els.productUid)els.productUid.value=s.pu||'';if(els.serverDownSmsToggle)els.serverDownSmsToggle.checked=s.sds!==false;if(els.dailyEmailTime)els.dailyEmailTime.value=timeStr;const ftp=s.ftp||{};if(els.ftpEnabled)els.ftpEnabled.checked=!!ftp.enabled;if(els.ftpPassive)els.ftpPassive.checked=ftp.passive!==false;if(els.ftpsEnabled)els.ftpsEnabled.checked=!!ftp.ftpsEnabled;if(els.ftpsTrustMode)els.ftpsTrustMode.value=String(typeof ftp.ftpsTrustMode==='number'?ftp.ftpsTrustMode:0);if(els.ftpsFingerprint)els.ftpsFingerprint.value=ftp.ftpsFingerprint||'';if(els.ftpsTlsServerName)els.ftpsTlsServerName.value=ftp.ftpsTlsServerName||'';if(els.ftpBackupOnChange)els.ftpBackupOnChange.checked=!!ftp.backupOnChange;if(els.ftpRestoreOnBoot)els.ftpRestoreOnBoot.checked=!!ftp.restoreOnBoot;if(els.ftpHost)els.ftpHost.value=ftp.host||'';if(els.ftpPort)els.ftpPort.value=ftp.port||21;if(els.ftpUser)els.ftpUser.value=ftp.user||'';if(els.ftpPath)els.ftpPath.value=ftp.path||'/tankalarm/server';if(els.ftpPass)els.ftpPass.value='';if(els.viewerEnabled)els.viewerEnabled.checked=!!s.ve;if(els.updatePolicy)els.updatePolicy.value=String(typeof s.up==='number'?s.up:0);if(els.checkClientVersionAlerts)els.checkClientVersionAlerts.checked=s.ccva!==false;if(els.checkViewerVersionAlerts)els.checkViewerVersionAlerts.checked=s.cvva!==false;await loadContactsData();}catch(err){showToast(err.message||'Failed to load settings',true);if(els.pinStatus){els.pinStatus.textContent='Failed to load';els.pinStatus.style.color='#ef4444';}}})HTML"
R"HTML(async funct)HTML" R"HTML(ion saveSettingsImpl(){if(!els.productUid.value.trim()){showToast('Product UID is required',true);els.productUid.focus();return;}const ftpSettings={enabled:!!els.ftpEnabled.checked,passive:!!els.ftpPassive.checked,ftpsEnabled:!!(els.ftpsEnabled&&els.ftpsEnabled.checked),ftpsTrustMode:parseInt((els.ftpsTrustMode&&els.ftpsTrustMode.value)||'0',10)||0,ftpsFingerprint:(els.ftpsFingerprint&&els.ftpsFingerprint.value?els.ftpsFingerprint.value.trim().toUpperCase():''),ftpsTlsServerName:els.ftpsTlsServerName&&els.ftpsTlsServerName.value?els.ftpsTlsServerName.value.trim():'',backupOnChange:!!els.ftpBackupOnChange.checked,restoreOnBoot:!!els.ftpRestoreOnBoot.checked,host:els.ftpHost.value.trim(),port:parseInt(els.ftpPort.value,10)||21,user:els.ftpUser.value.trim(),path:els.ftpPath.value.trim()||'/tankalarm/server'};const ftpPass=els.ftpPass.value.trim();if(ftpPass){ftpSettings.pass=ftpPass;}const timeValue=(els.dailyEmailTime&&els.dailyEmailTime.value)||'05:00';const timeParts=timeValue.split(':');const parsedHour=(timeParts.length===2&&!isNaN(parseInt(timeParts[0],10)))?Math.min(23,Math.max(0,parseInt(timeParts[0],10))):5;const parsedMinute=(timeParts.length===2&&!isNaN(parseInt(timeParts[1],10)))?Math.min(59,Math.max(0,parseInt(timeParts[1],10))):0;const payload={server:{productUid:els.productUid.value.trim(),serverDownSmsEnabled:!!(els.serverDownSmsToggle&&els.serverDownSmsToggle.checked),viewerEnabled:!!(els.viewerEnabled&&els.viewerEnabled.checked),updatePolicy:parseInt((els.updatePolicy&&els.updatePolicy.value)||'0',10),checkClientVersionAlerts:!!(els.checkClientVersionAlerts&&els.checkClientVersionAlerts.checked),checkViewerVersionAlerts:!!(els.checkViewerVersionAlerts&&els.checkViewerVersionAlerts.checked),dailyHour:parsedHour,dailyMinute:parsedMinute,ftp:ftpSettings}};try{const res=await fetch('/api/server-settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});if(!res.ok){const text=await res.text();throw new Error(text||'Server rejected settings');}showToast('Settings saved successfully');await loadSettings();}catch(err){showToast(err.message||'Failed to save settings',true);}}if(els.form)els.form.addEventListener('submit',(e)=>{e.preventDefault();saveSettingsImpl();});async funct)HTML" R"HTML(ion performFtpAction(kind){const payload={};)HTML"
R"HTML(const endpoint=kind==='restore'?'/api/ftp-restore':'/api/ftp-backup';try{const res=await fetch(endpoint,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});if(!res.ok){const text=await res.text();throw new Error(text||`${kind} failed`);}const data=await res.json();if(data&&data.message){showToast(data.message,false);}else{showToast(kind==='restore'?'FTP restore completed':'FTP backup completed');}}catch(err){showToast(err.message||`FTP ${kind} failed`,true);}}if(els.ftpBackupNow)els.ftpBackupNow.addEventListener('click',()=>performFtpAction('backup'));if(els.ftpRestoreNow)els.ftpRestoreNow.addEventListener('click',()=>performFtpAction('restore'));const ncEls={dot:getEl('notecardStatusDot'),label:getEl('notecardStatusLabel'),conn:getEl('ncConnStatus'),product:getEl('ncProductUid'),uid:getEl('ncServerUid'),mode:getEl('ncSyncMode'),refreshBtn:getEl('ncRefreshBtn')};async function loadNotecardStatus(){if(ncEls.label)ncEls.label.textContent='Checking...';if(ncEls.dot)ncEls.dot.style.background='#888';try{const res=await fetch('/api/notecard/status');if(!res.ok)throw new Error('Failed');const d=await res.json();const ok=d.connected;if(ncEls.dot)ncEls.dot.style.background=ok?'#10b981':'#ef4444';if(ncEls.label)ncEls.label.textContent=ok?'Connected':'Disconnected';if(ncEls.conn)ncEls.conn.textContent=ok?'I2C OK — Notecard responding':'I2C FAILED — no response from Notecard';if(ncEls.conn)ncEls.conn.style.color=ok?'#10b981':'#ef4444';if(ncEls.product)ncEls.product.textContent=d.productUid||'Not configured';if(ncEls.uid)ncEls.uid.textContent=d.serverUid||'Unknown';if(ncEls.mode)ncEls.mode.textContent=d.syncMode||'Unknown';if(!ok&&ncEls.product){ncEls.product.style.color='var(--muted)';}}catch(err){if(ncEls.dot)ncEls.dot.style.background='#f59e0b';if(ncEls.label)ncEls.label.textContent='Error: '+err.message;if(ncEls.conn)ncEls.conn.textContent='Could not reach server';}}if(ncEls.refreshBtn)ncEls.refreshBtn.addEventListener('click',loadNotecardStatus);const dfuEls={checkBtn:getEl('dfuCheckBtn'),enableBtn:getEl('dfuEnableBtn'),statusText:getEl('dfuStatusText'),fwVersion:getEl('fwVersionDisplay'),fwBuild:getEl('fwBuildDisplay'),voltage:getEl('serverVoltageDisplay'),serverTime:getEl('serverTimeDisplay'),fwLastUpdated:getEl('fwLastUpdatedDisplay')};function updateDfuUI(data){if(dfuEls.fwVersion)dfuEls.fwVersion.textContent='v'+data.currentVersion;if(dfuEls.fwBuild)dfuEls.fwBuild.textContent=data.buildDate||'Unknown';if(dfuEls.fwLastUpdated){if(data.buildDate){var updatedStr=data.buildDate;if(data.buildTime)updatedStr+=' '+data.buildTime;dfuEls.fwLastUpdated.textContent=updatedStr;}else{dfuEls.fwLastUpdated.textContent='Unknown';}}if(dfuEls.voltage){var vParts=[];if(data.inputVoltage>0.5){vParts.push('Vin: '+data.inputVoltage.toFixed(2)+'V');}if(data.notecardVoltage>0){vParts.push('NC: '+data.notecardVoltage.toFixed(2)+'V');}dfuEls.voltage.textContent=vParts.length>0?vParts.join(' | '):'Not available';}if(dfuEls.serverTime){if(data.serverTime>0){const d=new Date(data.serverTime*1000);dfuEls.serverTime.textContent=d.toLocaleString();}else{dfuEls.serverTime.textContent='Not synced';}}var mode=data.dfuMode||'idle';if(data.dfuInProgress){if(dfuEls.statusText)dfuEls.statusText.textContent='Firmware update in progress... ('+mode+')';if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=true;if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=true;}else if(mode==='downloading'){if(dfuEls.statusText)dfuEls.statusText.textContent='Downloading firmware from Notehub...';if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=true;if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=false;}else if(mode==='ready'||data.updateAvailable){var verText=data.availableVersion||'';if(dfuEls.statusText)dfuEls.statusText.textContent=mode==='ready'?'Update ready to install: v'+verText:'Update available: v'+verText;if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=false;if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=false;}else if(mode==='error'){var errDetail=data.dfuError?' ('+data.dfuError+')':'';if(dfuEls.statusText)dfuEls.statusText.textContent='DFU error'+errDetail+' - try checking again';if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=true;if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=false;}else{if(dfuEls.statusText)dfuEls.statusText.textContent='Firmware is up to date (v'+data.currentVersion+')';if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=true;if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=false;}}async function loadDfuStatus(){try{const res=await fetch('/api/dfu/status');if(!res.ok)throw new Error('Failed to fetch DFU status');const data=await res.json();updateDfuUI(data);}catch(err){if(dfuEls.statusText)dfuEls.statusText.textContent='Error: '+err.message;if(dfuEls.fwVersion)dfuEls.fwVersion.textContent='Unknown';if(dfuEls.fwBuild)dfuEls.fwBuild.textContent='Unknown';if(dfuEls.fwLastUpdated)dfuEls.fwLastUpdated.textContent='Unknown';if(dfuEls.voltage)dfuEls.voltage.textContent='Not available';if(dfuEls.serverTime)dfuEls.serverTime.textContent='Not synced';if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=true;if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=false;}}if(dfuEls.checkBtn)dfuEls.checkBtn.addEventListener('click',async()=>{dfuEls.checkBtn.disabled=true;if(dfuEls.statusText)dfuEls.statusText.textContent='Querying Notecard for updates...';try{const res=await fetch('/api/dfu/check',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({})});if(!res.ok)throw new Error('Check failed');const data=await res.json();updateDfuUI(data);showToast(data.updateAvailable?'Update found: v'+data.availableVersion:'No updates available');}catch(err){if(dfuEls.statusText)dfuEls.statusText.textContent='Error checking: '+err.message;showToast('Failed to check for updates',true);}finally{if(dfuEls.checkBtn)dfuEls.checkBtn.disabled=false;}});if(dfuEls.enableBtn)dfuEls.enableBtn.addEventListener('click',async()=>{if(!confirm('Install firmware update using selected source? (May fall back to Notehub if configured)'))return;try{dfuEls.enableBtn.disabled=true;if(dfuEls.statusText)dfuEls.statusText.textContent='Starting firmware install...';const res=await fetch('/api/dfu/enable',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({})});if(!res.ok){const text=await res.text();throw new Error(text||'Failed to enable DFU');}const data=await res.json();showToast(data.message||'Firmware install started');if(dfuEls.statusText)dfuEls.statusText.textContent=data.message||'Firmware install started';setTimeout(loadDfuStatus,5000);}catch(err){showToast(err.message,true);if(dfuEls.enableBtn)dfuEls.enableBtn.disabled=false;if(dfuEls.statusText)dfuEls.statusText.textContent='Error: '+err.message;}});await loadDfuStatus();await loadNotecardStatus();await loadSettings();}catch(e){console.error(e);const t=document.getElementById('toast');if(t){t.innerText='Script Error! '+e.message;t.style.background='red';t.classList.add('show');}}});</script></body></html>)HTML";

static const char EMAIL_FORMAT_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Email Formatting - Tank Alarm</title><link rel="stylesheet" href="/style.css"><style>.format-section{border:1px solid var(--border);padding:var(--space-3);margin-bottom:var(--space-3);background:#fafafa}.format-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:var(--space-2)}.format-title{font-weight:600;font-size:1rem}.preview-box{border:1px solid var(--border);background:#fff;padding:var(--space-3);font-family:ui-monospace,Consolas,monospace;font-size:0.85rem;white-space:pre-wrap;max-height:400px;overflow:auto}.field-list{display:flex;flex-direction:column;gap:var(--space-2)}.field-item{display:flex;align-items:center;gap:var(--space-2);padding:var(--space-2);border:1px solid var(--border);background:#fff}.field-item label{flex:1;display:flex;align-items:center;gap:8px}.field-item input[type="checkbox"]{width:auto}.drag-handle{cursor:grab;color:var(--muted);padding:0 8px}.drag-handle:active{cursor:grabbing}</style></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Daily Email Format</h2><p style="color:var(--muted);margin-bottom:16px;">Customize the content and layout of daily sensor summary emails. Changes are saved automatically.</p><form id="formatForm"><div class="format-section"><div class="format-header"><span class="format-title">Email Header</span></div><div class="form-grid"><label class="field"><span>Email Subject</span><input type="text" id="emailSubject" value="Daily Tank Summary - {date}" placeholder="Daily Tank Summary - {date}"></label><label class="field"><span>Company/Organization Name</span><input type="text" id="companyName" value="" placeholder="Your Company Name (optional)"></label></div><div class="toggle-group" style="margin-top:12px"><label class="toggle"><span>Include Date in Header</span><input type="checkbox" id="includeDate" checked></label><label class="toggle"><span>Include Time Generated</span><input type="checkbox" id="includeTime"></label><label class="toggle"><span>Include Server Name</span><input type="checkbox" id="includeServerName" checked></label></div></div><div class="format-section"><div class="format-header"><span class="format-title">Site Information</span></div><div class="toggle-group"><label class="toggle"><span>Group Sensors by Site</span><input type="checkbox" id="groupBySite" checked></label><label class="toggle"><span>Show Site Summary (total change)</span><input type="checkbox" id="showSiteSummary" checked></label><label class="toggle"><span>Show Site Total Capacity</span><input type="checkbox" id="showSiteCapacity"></label></div></div><div class="format-section"><div class="format-header"><span class="format-title">Tank Details</span></div><p style="color:var(--muted);font-size:0.85rem;margin-bottom:12px">Select which fields to include for each sensor:</p><div class="toggle-group"><label class="toggle"><span>Tank Number</span><input type="checkbox" id="fieldSensorIndex" checked></label><label class="toggle"><span>Tank Name</span><input type="checkbox" id="fieldSensorName" checked></label><label class="toggle"><span>Contents (Diesel, Water, etc.)</span><input type="checkbox" id="fieldContents" checked></label><label class="toggle"><span>Current Level (inches)</span><input type="checkbox" id="fieldLevelInches" checked></label><label class="toggle"><span>Current Level (percentage)</span><input type="checkbox" id="fieldLevelPercent"></label><label class="toggle"><span>Level Change (24h)</span><input type="checkbox" id="fieldLevelChange" checked></label><label class="toggle"><span>Delivery Indicator</span><input type="checkbox" id="fieldDeliveryIndicator" checked></label><label class="toggle"><span>Last Reading Time</span><input type="checkbox" id="fieldLastReading"></label><label class="toggle"><span>Sensor Raw Value (mA)</span><input type="checkbox" id="fieldSensorMa"></label><label class="toggle"><span>High/Low Alarm Status</span><input type="checkbox" id="fieldAlarmStatus" checked></label><label class="toggle"><span>Tank Capacity (max height)</span><input type="checkbox" id="fieldCapacity"></label></div></div><div class="format-section"><div class="format-header"><span class="format-title">Summary Section</span></div><div class="toggle-group"><label class="toggle"><span>Include Fleet Summary</span><input type="checkbox" id="includeFleetSummary" checked></label><label class="toggle"><span>Total Sensors Monitored</span><input type="checkbox" id="summaryTotalSensors" checked></label><label class="toggle"><span>Sensors with Active Alarms</span><input type="checkbox" id="summaryActiveAlarms" checked></label><label class="toggle"><span>Total Deliveries (24h)</span><input type="checkbox" id="summaryDeliveries" checked></label><label class="toggle"><span>Stale Readings Warning</span><input type="checkbox" id="summaryStaleWarning" checked></label></div></div><div class="format-section"><div class="format-header"><span class="format-title">Formatting Options</span></div><div class="form-grid"><label class="field"><span>Number Format</span><select id="numberFormat"><option value="decimal">Decimal (12.5)</option><option value="fraction">Fraction (12 1/2)</option></select></label><label class="field"><span>Date Format</span><select id="dateFormat"><option value="us">US (MM/DD/YYYY)</option><option value="iso">ISO (YYYY-MM-DD)</option><option value="eu">EU (DD/MM/YYYY)</option></select></label><label class="field"><span>Change Indicator Style</span><select id="changeStyle"><option value="arrow">Arrows (&#x2191; &#x2193;)</option><option value="plusminus">Plus/Minus (+5.2 / -3.1)</option><option value="text">Text (increased/decreased)</option></select></label></div><div class="toggle-group" style="margin-top:12px"><label class="toggle"><span>Use Color Coding (HTML emails)</span><input type="checkbox" id="useColors" checked></label><label class="toggle"><span>Include Horizontal Dividers</span><input type="checkbox" id="useDividers" checked></label></div></div><div class="actions"><button type="submit">Save Format</button><button type="button" class="secondary" id="previewBtn">Preview Email</button><button type="button" class="secondary" id="resetBtn">Reset to Defaults</button><a class="pill secondary" href="/server-settings">Back to Settings</a></div></form></div><div class="card" id="previewCard" style="display:none"><h2>Email Preview</h2><div id="previewContent" class="preview-box"></div></div></main><div id="toast"></div><script>document.addEventListener('DOMContentLoaded',()=>{const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);const toast=document.getElementById('toast');const previewCard=document.getElementById('previewCard');const previewContent=document.getElementById('previewContent');function showToast(msg,isError){toast.textContent=msg;toast.style.background=isError?'#dc2626':'#0284c7';toast.classList.add('show');setTimeout(()=>toast.classList.remove('show'),2500);}const fields=['emailSubject','companyName','includeDate','includeTime','includeServerName','groupBySite','showSiteSummary','showSiteCapacity','fieldSensorIndex','fieldSensorName','fieldContents','fieldLevelInches','fieldLevelPercent','fieldLevelChange','fieldDeliveryIndicator','fieldLastReading','fieldSensorMa','fieldAlarmStatus','fieldCapacity','includeFleetSummary','summaryTotalSensors','summaryActiveAlarms','summaryDeliveries','summaryStaleWarning','numberFormat','dateFormat','changeStyle','useColors','useDividers'];function getFormData(){const data={};fields.forEach(f=>{const el=document.getElementById(f);if(el.type==='checkbox')data[f]=el.checked;else data[f]=el.value;});return data;}function setFormData(data){fields.forEach(f=>{const el=document.getElementById(f);if(!el)return;if(el.type==='checkbox')el.checked=!!data[f];else if(data[f]!==undefined)el.value=data[f];});}async function loadFormat(){try{const res=await fetch('/api/email-format');if(!res.ok)throw new Error('Failed to load');const data=await res.json();setFormData(data);}catch(err){console.log('Using defaults');}}async function saveFormat(e){if(e)e.preventDefault();const data=getFormData();try{const res=await fetch('/api/email-format',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});if(!res.ok)throw new Error('Failed to save');showToast('Format saved successfully');}catch(err){showToast(err.message,true);}}function generatePreview(){const d=getFormData();const dateStr=d.dateFormat==='iso'?new Date().toISOString().split('T')[0]:d.dateFormat==='eu'?new Date().toLocaleDateString('en-GB'):new Date().toLocaleDateString('en-US');let subject=d.emailSubject.replace('{date}',dateStr);let preview='Subject: '+subject+'\n\n';if(d.companyName)preview+=d.companyName+'\n';if(d.includeServerName)preview+='Server: TankAlarm-Server-01\n';if(d.includeDate)preview+='Date: '+dateStr+'\n';if(d.includeTime)preview+='Generated: '+new Date().toLocaleTimeString()+'\n';preview+='\n'+'-'.repeat(50)+'\n\n';const sites=[{name:'Site Alpha',sensors:[{num:1,name:'Main Tank',contents:'Diesel',level:85.5,change:12.3,alarm:false,capacity:120},{num:2,name:'Reserve',contents:'Diesel',level:45.2,change:-8.1,alarm:true,alarmType:'low',capacity:100}]},{name:'Site Beta',sensors:[{num:1,name:'Water Tank',contents:'Water',level:92.0,change:0,alarm:false,capacity:150}]}];if(d.groupBySite){sites.forEach(site=>{preview+='=== '+site.name+' ===\n';if(d.showSiteSummary){const totalChange=site.sensors.reduce((s,t)=>s+t.change,0);const arrow=d.changeStyle==='arrow'?(totalChange>0?'\u2191':'\u2193'):d.changeStyle==='plusminus'?(totalChange>0?'+':''):' ';preview+='Site Total Change: '+arrow+(d.numberFormat==='fraction'?Math.round(Math.abs(totalChange)):Math.abs(totalChange).toFixed(1))+' in\n';}if(d.showSiteCapacity){const totalCap=site.sensors.reduce((s,t)=>s+t.capacity,0);preview+='Total Capacity: '+totalCap+' in\n';}preview+='\n';site.sensors.forEach(t=>{let line='';if(d.fieldSensorIndex)line+='Tank #'+t.num+' ';if(d.fieldSensorName)line+=t.name+' ';if(d.fieldContents)line+='['+t.contents+'] ';preview+=line.trim()+'\n';let details='  ';if(d.fieldLevelInches)details+='Level: '+(d.numberFormat==='fraction'?Math.round(t.level):t.level.toFixed(1))+' in  ';if(d.fieldLevelPercent)details+=Math.round(t.level/t.capacity*100)+'%  ';if(d.fieldLevelChange&&t.change!==0){const arrow=d.changeStyle==='arrow'?(t.change>0?'\u2191':'\u2193'):d.changeStyle==='plusminus'?(t.change>0?'+':''): '';details+=arrow+Math.abs(t.change).toFixed(1)+' in  ';}if(d.fieldDeliveryIndicator&&t.change>5)details+='&#x1F4E6; DELIVERY  ';if(d.fieldAlarmStatus&&t.alarm)details+='&#x26A0;&#xFE0F; '+t.alarmType.toUpperCase()+' ALARM  ';if(d.fieldCapacity)details+='(Cap: '+t.capacity+' in)  ';preview+=details.trim()+'\n';if(d.fieldLastReading)preview+='  Last: '+new Date().toLocaleString()+'\n';if(d.fieldSensorMa)preview+='  Sensor: 12.45 mA\n';preview+='\n';});if(d.useDividers)preview+='-'.repeat(50)+'\n\n';});}if(d.includeFleetSummary){preview+='=== FLEET SUMMARY ===\n';if(d.summaryTotalSensors)preview+='Total Sensors: 3\n';if(d.summaryActiveAlarms)preview+='Active Alarms: 1\n';if(d.summaryDeliveries)preview+='Deliveries (24h): 1\n';if(d.summaryStaleWarning)preview+='Stale Readings: 0\n';}previewContent.textContent=preview;previewCard.style.display='block';previewCard.scrollIntoView({behavior:'smooth'});}document.getElementById('formatForm').addEventListener('submit',saveFormat);document.getElementById('previewBtn').addEventListener('click',generatePreview);document.getElementById('resetBtn').addEventListener('click',()=>{const defaults={emailSubject:'Daily Tank Summary - {date}',companyName:'',includeDate:true,includeTime:false,includeServerName:true,groupBySite:true,showSiteSummary:true,showSiteCapacity:false,fieldSensorIndex:true,fieldSensorName:true,fieldContents:true,fieldLevelInches:true,fieldLevelPercent:false,fieldLevelChange:true,fieldDeliveryIndicator:true,fieldLastReading:false,fieldSensorMa:false,fieldAlarmStatus:true,fieldCapacity:false,includeFleetSummary:true,summaryTotalSensors:true,summaryActiveAlarms:true,summaryDeliveries:true,summaryStaleWarning:true,numberFormat:'decimal',dateFormat:'us',changeStyle:'arrow',useColors:true,useDividers:true};setFormData(defaults);showToast('Reset to defaults');});loadFormat();});</script></body></html>)HTML";

static const char SERVER_SETTINGS_DFU_TARGET_INJECTION[] PROGMEM = R"HTML(
<script>
document.addEventListener('DOMContentLoaded', () => {
  const statusCard = document.getElementById('dfuStatus');
  const statusText = document.getElementById('dfuStatusText');
  const originalCheckButton = document.getElementById('dfuCheckBtn');
  const originalInstallButton = document.getElementById('dfuEnableBtn');
  if (!statusCard || !statusText || !originalCheckButton || !originalInstallButton) {
    return;
  }

  let targetSelect = document.getElementById('dfuTargetSelect');
  if (!targetSelect) {
    const wrapper = document.createElement('div');
    wrapper.className = 'form-grid';
    wrapper.style.marginBottom = '12px';
    wrapper.innerHTML = '<label class="field" style="max-width:320px;"><span>Firmware Target</span><select id="dfuTargetSelect"><option value="server">TankAlarm Server</option><option value="ftps-test">FTPS Test</option></select></label>';
    statusCard.insertAdjacentElement('afterend', wrapper);
    targetSelect = wrapper.querySelector('#dfuTargetSelect');
  }

  const originalActions = originalInstallButton.parentElement;
  if (originalInstallButton) {
    originalInstallButton.textContent = 'Install Selected Firmware';
  }
  if (originalActions && originalActions.nextElementSibling && originalActions.nextElementSibling.tagName === 'P') {
    originalActions.nextElementSibling.textContent = 'Manual install now targets the selected firmware image. Server installs still fall back to Notehub DFU when a ready GitHub asset is not available.';
  }

  function showToast(message, isError) {
    const toast = document.getElementById('toast');
    if (!toast) {
      return;
    }
    toast.textContent = message;
    toast.style.background = isError ? '#dc2626' : '#0284c7';
    toast.classList.add('show');
    setTimeout(() => toast.classList.remove('show'), 2500);
  }

  function getTargetId() {
    return targetSelect ? targetSelect.value : 'server';
  }

  function getTargetLabel() {
    if (!targetSelect) {
      return 'Selected firmware';
    }
    return targetSelect.options[targetSelect.selectedIndex].textContent || 'Selected firmware';
  }

  function replaceButton(button) {
    const clone = button.cloneNode(true);
    button.parentNode.replaceChild(clone, button);
    return clone;
  }

  let checkButton = replaceButton(originalCheckButton);
  let installButton = replaceButton(originalInstallButton);

  function updateTargetUi(data) {
    if (targetSelect && data.selectedTarget) {
      targetSelect.value = data.selectedTarget;
    }
    statusText.textContent = data.selectedTargetStatusText || ('Select ' + getTargetLabel() + ', then click Check for Update');
    installButton.disabled = !data.selectedTargetInstallEnabled;
    checkButton.disabled = !!data.dfuInProgress;
  }

  async function refreshTargetStatus() {
    const response = await fetch('/api/dfu/status');
    if (!response.ok) {
      throw new Error('Failed to fetch DFU status');
    }
    const data = await response.json();
    updateTargetUi(data);
    return data;
  }

  if (targetSelect) {
    targetSelect.addEventListener('change', () => {
      statusText.textContent = 'Select ' + getTargetLabel() + ', then click Check for Update';
      installButton.disabled = true;
    });
  }

  checkButton.addEventListener('click', async () => {
    checkButton.disabled = true;
    statusText.textContent = 'Checking ' + getTargetLabel() + '...';
    try {
      const response = await fetch('/api/dfu/check', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({target: getTargetId()})
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || 'Check failed');
      }
      const data = await response.json();
      updateTargetUi(data);
      showToast(data.selectedTargetInstallEnabled ? getTargetLabel() + ' is ready to install' : (data.selectedTargetStatusText || ('No installable update for ' + getTargetLabel())));
    } catch (error) {
      statusText.textContent = 'Error checking: ' + error.message;
      showToast('Failed to check for updates', true);
    } finally {
      checkButton.disabled = false;
    }
  });

  installButton.addEventListener('click', async () => {
    if (!confirm('Install ' + getTargetLabel() + ' now? The device will reboot when flashing starts.')) {
      return;
    }

    try {
      installButton.disabled = true;
      statusText.textContent = 'Starting ' + getTargetLabel() + ' install...';
      const response = await fetch('/api/dfu/enable', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({target: getTargetId()})
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || 'Failed to start firmware install');
      }
      const data = await response.json();
      statusText.textContent = data.message || 'Firmware install started';
      showToast(data.message || 'Firmware install started');
      setTimeout(() => {
        refreshTargetStatus().catch(() => {});
      }, 5000);
    } catch (error) {
      installButton.disabled = false;
      statusText.textContent = 'Error: ' + error.message;
      showToast(error.message, true);
    }
  });

  refreshTargetStatus().catch((error) => {
    statusText.textContent = 'Error: ' + error.message;
    showToast('Failed to load updater status', true);
  });
});
</script>
)HTML";

static const char CONFIG_GENERATOR_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Client Configuration</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill" href="/config-generator">Client Config</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Client Configuration</h2>
<div class="actions" style="margin-bottom:20px;justify-content:space-between;flex-wrap:wrap;gap:10px;">
  <div style="display:flex;gap:8px;"><button type="button" id="loadFromCloudBtn" class="secondary">Load from Cloud</button>
  <button type="button" id="importBtn" class="secondary">Import JSON</button></div>
  <div style="display:flex;gap:8px;"><button type="submit" form="generatorForm">Save to Device</button>
  <button type="button" id="downloadBtn" class="secondary">Download JSON</button></div>
</div>
<div id="configStatus" style="display:none;padding:12px;margin-bottom:16px;border-radius:var(--radius);background:#e6f2ff;border:1px solid #b3d9ff;color:#0066cc;"></div><form id="generatorForm"><div class="form-grid"><label class="field"><span>Product UID <span class="tooltip-icon" tabindex="0" data-tooltip="Blues Notehub Product UID. Must match the server's Product UID for client-server communication.">?</span></span><input id="productUid" type="text" placeholder="com.company.product:project" required></label><label class="field"><span>Device UID</span><input id="clientUid" type="text" placeholder="dev:xxxxxxxx"></label><label class="field"><span>Site Name</span><input id="siteName" type="text" placeholder="Site Name" required></label><label class="field"><span>Device Label</span><input id="deviceLabel" type="text" placeholder="Device Label" required></label><label class="field"><span>Client Fleet</span><input id="clientFleet" type="text" placeholder="Client Fleet"></label><label class="field"><span>Server Fleet</span><input id="serverFleet" type="text" value="tankalarm-server"></label><label class="field"><span>Sample Minutes</span><input id="sampleMinutes" type="number" value="30" min="1" max="1440"></label><label class="field"><span>Report Time</span><input id="reportTime" type="time" value="05:00"></label><label class="field"><span>Daily Report Email Recipient</span><input id="dailyEmail" type="email"></label></div><h3>Power Configuration</h3><div class="form-grid"><label class="field"><span>Power Source<span class="tooltip-icon" tabindex="0" data-tooltip="Primary power supply feeding the device. Battery selection is configured separately below.">?</span></span><select id="powerSource" onchange="updatePowerConfigInfo()"><option value="grid">Grid AC</option><option value="solar">Solar (Passive Charger)</option><option value="solar_mppt">Solar + Non-Monitored MPPT</option><option value="solar_modbus_mppt">Solar + Modbus MPPT (RS-485)</option></select></label><label class="field"><span>Battery<span class="tooltip-icon" tabindex="0" data-tooltip="Battery chemistry. Lead-acid variants share thresholds; Flooded uses a higher equalization ceiling. Select None for grid-only or solar-direct installs.">?</span></span><select id="batteryType" onchange="updatePowerConfigInfo()"><option value="none">None (no battery)</option><option value="agm" selected>AGM (Sealed)</option><option value="sla">SLA (Sealed Lead-Acid)</option><option value="flooded">Flooded (Wet Cell)</option><option value="gel">Gel</option><option value="lifepo4">LiFePO4</option><option value="li_ion">Li-ion (NMC)</option><option value="lipo">LiPo (single cell)</option><option value="custom">Custom (manual thresholds)</option></select></label><label class="field" id="batteryVoltageField"><span>Battery Pack Voltage<span class="tooltip-icon" tabindex="0" data-tooltip="Nominal pack voltage. 24V scales all thresholds 2x from the 12V baseline. 24V support is experimental — verify Vin divider headroom.">?</span></span><select id="batteryVoltage"><option value="12" selected>12V</option><option value="24">24V (experimental)</option></select></label><label class="field"><span>Vin Monitor<span class="tooltip-icon" tabindex="0" data-tooltip="Enable an external voltage divider on an Opta analog input to read actual battery voltage. Required for accurate solar-only sunset detection.">?</span></span><select id="vinMonitorEnabled" onchange="updatePowerConfigInfo()"><option value="0">Disabled</option><option value="1">Enabled</option></select></label></div><div id="powerCombinationWarning" style="display:none;background:#fff3cd;border:1px solid #ffc107;color:#856404;padding:10px 12px;margin-bottom:12px;border-radius:var(--radius);font-size:0.88rem;"></div><div id="powerConfigInfo" style="display:none;background:var(--chip);border:1px solid var(--card-border);padding:12px;margin-bottom:16px;font-size:0.9rem;color:var(--muted);"><strong>Hardware Requirement:</strong> Modbus MPPT requires Opta WiFi (AFX00002) or Opta RS485 (AFX00001). Opta Lite (AFX00003) needs an external RS485 transceiver.</div><div id="vinConfigSection" style="display:none;background:var(--chip);border:1px solid var(--card-border);padding:12px;margin-bottom:16px;border-radius:var(--radius);"><strong style="display:block;margin-bottom:8px;">Vin Monitor — Voltage Divider Configuration</strong><p style="font-size:0.85rem;color:var(--muted);margin:0 0 12px;">Wire a voltage divider from the battery to an Opta analog input: Battery+ → R1 → Pin → R2 → GND</p><div class="form-grid"><label class="field"><span>Analog Pin</span><select id="vinPin"><option value="0">A0</option><option value="1">A1</option><option value="2">A2</option><option value="3">A3</option><option value="4">A4</option><option value="5">A5</option><option value="6">A6</option><option value="7">A7</option></select></label><label class="field"><span>R1 High-side (kΩ)</span><input id="vinR1" type="number" value="22" min="1" max="1000" step="0.1" onchange="updateVinCalc()"></label><label class="field"><span>R2 Low-side (kΩ)</span><input id="vinR2" type="number" value="47" min="1" max="1000" step="0.1" onchange="updateVinCalc()"></label></div><div id="vinCalcInfo" style="margin-top:8px;font-size:0.85rem;color:var(--muted);"></div></div><div id="solarOnlyConfigSection" style="display:none;background:var(--chip);border:1px solid var(--card-border);padding:12px;margin-bottom:16px;border-radius:var(--radius);"><strong style="display:block;margin-bottom:8px;">Solar-Only (No Battery) Settings</strong><p style="font-size:0.85rem;color:var(--muted);margin:0 0 12px;">This device is powered directly by a solar panel without battery backup. It will only operate during daylight hours.</p><div class="form-grid"><label class="field" id="solarOnlyDebounceVField"><span>Startup Debounce Voltage (V)<span class="tooltip-icon" tabindex="0" data-tooltip="Minimum Vin voltage before the system considers power stable enough to start. Only applies when Vin divider is connected.">?</span></span><input id="solarOnlyDebounceV" type="number" value="10" min="5" max="20" step="0.5"></label><label class="field" id="solarOnlyDebounceSField"><span>Debounce Duration (sec)<span class="tooltip-icon" tabindex="0" data-tooltip="Number of seconds the voltage must remain above the debounce threshold. Only applies when Vin divider is connected.">?</span></span><input id="solarOnlyDebounceSec" type="number" value="30" min="5" max="300"></label><label class="field" id="solarOnlyWarmupField"><span>Startup Warmup (sec)<span class="tooltip-icon" tabindex="0" data-tooltip="Fixed warmup timer used when no Vin divider is installed. The device waits this long after boot before reading sensors.">?</span></span><input id="solarOnlyWarmupSec" type="number" value="60" min="10" max="600"></label><label class="field" id="solarOnlySensorGateField"><span>Sensor Gate Voltage (V)<span class="tooltip-icon" tabindex="0" data-tooltip="Minimum Vin voltage required before sensor readings are taken. 4-20mA sensors typically need at least 11V excitation. Only applies when Vin divider is connected.">?</span></span><input id="solarOnlySensorGateV" type="number" value="11" min="5" max="20" step="0.5"></label><label class="field" id="solarOnlySunsetVField"><span>Sunset Voltage (V)<span class="tooltip-icon" tabindex="0" data-tooltip="When Vin drops below this and continues declining, the device saves state and prepares for power loss. Only applies when Vin divider is connected.">?</span></span><input id="solarOnlySunsetV" type="number" value="10" min="5" max="20" step="0.5"></label><label class="field"><span>Sunset Confirm Duration (sec)<span class="tooltip-icon" tabindex="0" data-tooltip="How long voltage must be declining below sunset threshold before triggering shutdown save.">?</span></span><input id="solarOnlySunsetSec" type="number" value="120" min="30" max="600"></label><label class="field"><span>Opportunistic Report (hours)<span class="tooltip-icon" tabindex="0" data-tooltip="If more than this many hours have passed since the last daily report, send one immediately on startup. For solar-only, reports cannot be scheduled at a fixed time.">?</span></span><input id="solarOnlyReportHours" type="number" value="20" min="6" max="48"></label></div><div id="solarOnlyBatFailSection" style="display:none;margin-top:12px;border-top:1px solid var(--card-border);padding-top:12px;"><label style="display:flex;align-items:center;gap:8px;font-size:0.9rem;margin-bottom:8px;"><input type="checkbox" id="solarOnlyBatFail"> Enable battery failure fallback<span class="tooltip-icon" tabindex="0" data-tooltip="For solar+battery setups: if the battery voltage stays critically low for extended time, automatically switch to solar-only behaviors (opportunistic reporting, sunset protocol).">?</span></label><label class="field"><span>Failure Threshold (readings)<span class="tooltip-icon" tabindex="0" data-tooltip="Number of consecutive critical-voltage readings before declaring battery failure.">?</span></span><input id="solarOnlyBatFailCount" type="number" value="10" min="3" max="50"></label></div></div><h3>Sensors</h3><div id="sensorsContainer"></div><div class="actions" style="margin-bottom: 24px;"><button type="button" id="addSensorBtn" class="secondary">+ Add Sensor</button></div><h3>Inputs (Buttons &amp; Switches)</h3><p style="color: var(--muted); font-size: 0.9rem; margin-bottom: 12px;">Configure physical inputs for actions like clearing relay alarms.</p><div id="inputsContainer"></div><div class="actions" style="margin-bottom: 24px;"><button type="button" id="addInputBtn" class="secondary">+ Add Input</button></div><div class="actions"><button type="submit" id="sendConfigBtn">Save Configuration to Device</button><button type="button" id="retryConfigBtn" class="secondary" style="display:none">Retry Send to Notecard</button><button type="button" id="syncRequestBtn" class="secondary" style="display:none" title="Force the client Notecard to sync inbound notes (useful for weak signal)">Request Client Sync</button></div></form></div></main><div id="toast"></div><div id="selectClientModal" class="modal hidden"><div class="modal-content"><div class="modal-header"><h2>Load Configuration</h2><button class="modal-close" onclick="closeSelectClientModal()">&times;</button></div><div id="clientList" class="client-list"><div class="empty-state">Loading clients...</div></div></div></div><datalist id="relayTargetSuggestions"></datalist><input type="file" id="importFileInput" accept=".json" style="display:none" />
<script>
(async () => {
const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);
const state={paused:false,clients:[]};
const els={toast:document.getElementById('toast'),form:document.getElementById('generatorForm'),productUid:document.getElementById('productUid'),clientUid:document.getElementById('clientUid'),clientList:document.getElementById('clientList'),selectClientModal:document.getElementById('selectClientModal'),loadFromCloudBtn:document.getElementById('loadFromCloudBtn'),siteName:document.getElementById('siteName'),deviceLabel:document.getElementById('deviceLabel'),clientFleet:document.getElementById('clientFleet'),serverFleet:document.getElementById('serverFleet'),sampleMinutes:document.getElementById('sampleMinutes'),dailyEmail:document.getElementById('dailyEmail'),reportTime:document.getElementById('reportTime'),powerSource:document.getElementById('powerSource')};
function showToast(m,e,dur){if(els.toast){els.toast.innerText=m;els.toast.style.background=e==='warn'?'#d97706':e?'#dc2626':'#0284c7';els.toast.classList.add('show');setTimeout(()=>els.toast.classList.remove('show'),dur||3000);}}

const pauseBtn=document.getElementById('pauseBtn');function renderPauseBtn(){if(pauseBtn){pauseBtn.style.display=state.paused?'':'none';}}
async function loadPauseState(){try{const r=await fetch('/api/clients?summary=1');const d=await r.json();if(d&&d.srv){state.paused=!!d.srv.ps;renderPauseBtn();}}catch(e){}}
if(pauseBtn)pauseBtn.addEventListener('click',async()=>{try{const r=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:false})});if(r.ok){state.paused=false;renderPauseBtn();showToast('Resumed');}else throw new Error('Failed');}catch(e){showToast('Error resuming',true);}});
await loadPauseState();
window.validatePowerCombination=function(){
  const ps=document.getElementById('powerSource').value||'grid';
  const bt=document.getElementById('batteryType').value||'agm';
  const bv=parseInt(document.getElementById('batteryVoltage').value,10)||12;
  const vin=document.getElementById('vinMonitorEnabled').value==='1';
  const w=document.getElementById('powerCombinationWarning');
  if(!w)return true;
  let msg='';let block=false;
  if(ps==='solar_modbus_mppt'&&bt==='none'){
    msg='SunSaver / Modbus MPPT chargers require a battery to operate. Select a battery chemistry or change power source.';
    block=true;
  }else if(ps==='solar_mppt'&&bt==='none'){
    msg='MPPT controllers require a battery load. Switch to plain Solar (Passive Charger) for solar-direct installs.';
    block=true;
  }else if(ps.startsWith('solar')&&bt==='none'&&!vin){
    msg='Solar-direct without a Vin Monitor cannot detect sunset. Strongly recommended to enable Vin Monitor.';
  }else if(ps==='grid'&&bt==='none'&&vin){
    msg='Vin Monitor on a grid-only install with no battery has nothing useful to read.';
  }else if(ps==='solar_modbus_mppt'&&(bt==='lifepo4'||bt==='li_ion')){
    msg='SunSaver MPPT has no lithium DIP preset. You MUST load a custom lithium charge profile via Morningstar MSView (USB MeterBus adapter) before connecting the battery — DIP-only lead-acid presets will undercharge LiFePO4 and may damage Li-ion. The firmware will read the setpoints back over Modbus and warn if they still look like a lead-acid preset.';
  }
  if(bv===24&&vin){
    const r1=parseFloat(document.getElementById('vinR1').value)||22;
    const r2=parseFloat(document.getElementById('vinR2').value)||47;
    const maxV=10.0/(r2/(r1+r2));
    if(maxV<27.0){
      const sep=msg?' ':'';
      msg+=sep+'24V pack with R1='+r1+'k, R2='+r2+'k caps at '+maxV.toFixed(1)+'V. Recommend R1=47k, R2=33k (max ~33V).';
    }
  }
  if(msg){w.style.display='block';w.innerHTML=(block?'<strong>Blocked:</strong> ':'<strong>Warning:</strong> ')+msg;w.style.background=block?'#f8d7da':'#fff3cd';w.style.borderColor=block?'#dc3545':'#ffc107';w.style.color=block?'#721c24':'#856404';}else{w.style.display='none';}
  return !block;
};
window.updatePowerConfigInfo=function(){
  const ps=document.getElementById('powerSource').value||'grid';
  const bt=document.getElementById('batteryType').value||'agm';
  const vin=document.getElementById('vinMonitorEnabled').value==='1';
  /* Hide battery voltage selector when no battery */
  const bvField=document.getElementById('batteryVoltageField');
  if(bvField)bvField.style.display=(bt==='none'||bt==='lipo')?'none':'flex';
  /* Hardware-requirement banner */
  const i=document.getElementById('powerConfigInfo');
  if(i){
    if(ps==='solar_modbus_mppt'){i.style.display='block';i.innerHTML='<strong>Hardware Requirement:</strong> Modbus MPPT requires Opta WiFi (AFX00002) or Opta RS485 (AFX00001). Opta Lite (AFX00003) needs an external RS485 transceiver.';}
    else if(ps==='solar_mppt'){i.style.display='block';i.innerHTML='<strong>Hardware Requirement:</strong> Use this for higher-quality non-monitored solar chargers. For basic PWM chargers (for example SunKeeper-6), use Solar (Passive Charger). This mode does not require RS485 and uses battery/vin telemetry only.';}
    else{i.style.display='none';}
  }
  /* Vin divider config section */
  const v=document.getElementById('vinConfigSection');if(v)v.style.display=vin?'block':'none';if(vin)updateVinCalc();
  /* Solar-only (no battery) section auto-enables when battery=None and power source is solar */
  const isSolarOnly=(bt==='none'&&ps.startsWith('solar'));
  const so=document.getElementById('solarOnlyConfigSection');if(so)so.style.display=isSolarOnly?'block':'none';
  const vinFields=['solarOnlyDebounceVField','solarOnlyDebounceSField','solarOnlySensorGateField','solarOnlySunsetVField'];
  vinFields.forEach(fid=>{const el=document.getElementById(fid);if(el)el.style.display=(isSolarOnly&&vin)?'flex':'none';});
  const warmupField=document.getElementById('solarOnlyWarmupField');
  if(warmupField)warmupField.style.display=(isSolarOnly&&!vin)?'flex':'none';
  /* Battery failure fallback only meaningful when there IS a battery on solar */
  const batFailSec=document.getElementById('solarOnlyBatFailSection');
  if(batFailSec)batFailSec.style.display=(ps.startsWith('solar')&&bt!=='none')?'block':'none';
  validatePowerCombination();
};window.updateVinCalc=function(){const r1=parseFloat(document.getElementById('vinR1').value)||22;const r2=parseFloat(document.getElementById('vinR2').value)||47;const ratio=r2/(r1+r2);const maxV=(10.0/ratio).toFixed(1);const bleed=(12/(r1+r2)*1000).toFixed(1);document.getElementById('vinCalcInfo').innerHTML='Divider ratio: '+ratio.toFixed(4)+' — Max readable: '+maxV+'V — Quiescent draw at 12V: ~'+bleed+'µA';};
/* CONSTANTS */
const sensorTypes=[{value:0,label:'Digital Input(Float Switch)'},{value:1,label:'Analog Input(0-10V)'},{value:2,label:'Current Loop(4-20mA)'},{value:3,label:'Hall Effect RPM'}];
const currentLoopTypes=[{value:'pressure',label:'Pressure Sensor(Bottom-Mounted)'},{value:'ultrasonic',label:'Ultrasonic Sensor(Top-Mounted)'}];
const monitorTypes=[{value:'tank',label:'Tank Level'},{value:'gas',label:'Gas Pressure'},{value:'rpm',label:'RPM Sensor'}];
const optaPins=[{value:0,label:'Opta I1'},{value:1,label:'Opta I2'},{value:2,label:'Opta I3'},{value:3,label:'Opta I4'},{value:4,label:'Opta I5'},{value:5,label:'Opta I6'},{value:6,label:'Opta I7'},{value:7,label:'Opta I8'}];
const expansionChannels=[{value:0,label:'A0602 Ch1'},{value:1,label:'A0602 Ch2'},{value:2,label:'A0602 Ch3'},{value:3,label:'A0602 Ch4'},{value:4,label:'A0602 Ch5'},{value:5,label:'A0602 Ch6'}];
const inputActions=[{value:'clear_relays',label:'Clear All Relay Alarms'},{value:'none',label:'Disabled(No Action)'}];
const inputModes=[{value:'active_low',label:'Active LOW(Button to GND,internal pullup)'},{value:'active_high',label:'Active HIGH(Button to VCC,external pull-down)'}];
let sensorCount=0;let inputIdCounter=0;
/* SENSOR CARD HTML */
function createSensorHtml(id){return `<div class="sensor-card" id="sensor-${id}"><div class="sensor-header"><span class="sensor-title">Sensor #${id+1}</span><button type="button" class="remove-btn" onclick="removeSensor(${id})">Remove</button></div><div class="form-grid"><label class="field"><span>Monitor Type</span><select class="monitor-type" onchange="updateMonitorFields(${id})">${monitorTypes.map(t=>`<option value="${t.value}">${t.label}</option>`).join('')}</select></label><label class="field tank-num-field"><span>Display Number (optional)</span><input type="number" class="tank-num" placeholder="e.g. 1, 2, 3"></label><label class="field"><span><span class="name-label">Name</span></span><input type="text" class="tank-name" placeholder="Name"></label><label class="field contents-field"><span>Contents</span><input type="text" class="tank-contents" placeholder="e.g. Diesel, Water"></label><label class="field"><span>Sensor Type</span><select class="sensor-type" onchange="updatePinOptions(${id})">${sensorTypes.map(t=>`<option value="${t.value}">${t.label}</option>`).join('')}</select></label><label class="field"><span>Pin / Channel</span><select class="sensor-pin">${optaPins.map(p=>`<option value="${p.value}">${p.label}</option>`).join('')}</select></label><label class="field switch-mode-field" style="display:none;"><span>Switch Mode<span class="tooltip-icon" tabindex="0" data-tooltip="NO(Normally-Open): Switch is open by default, closes when fluid is present. NC(Normally-Closed): Switch is closed by default, opens when fluid is present.">?</span></span><select class="switch-mode"><option value="NO">Normally-Open(NO)</option><option value="NC">Normally-Closed(NC)</option></select></label><label class="field pulses-per-rev-field" style="display:none;"><span>Pulses/Rev</span><input type="number" class="pulses-per-rev" value="1" min="1" max="255"></label><label class="field current-loop-type-field" style="display:none;"><span>4-20mA Sensor Type<span class="tooltip-icon" tabindex="0" data-tooltip="Select the type of 4-20mA sensor: Pressure sensors are mounted near the sensor bottom and measure liquid pressure. Ultrasonic sensors are mounted on top of the sensor and measure distance to the liquid surface.">?</span></span><select class="current-loop-type" onchange="updateCurrentLoopFields(${id})"><option value="pressure">Pressure Sensor(Bottom-Mounted)</option><option value="ultrasonic">Ultrasonic Sensor(Top-Mounted)</option></select></label><label class="field sensor-range-field" style="display:none;"><span><span class="sensor-range-label">Sensor Range</span><span class="tooltip-icon sensor-range-tooltip" tabindex="0" data-tooltip="Native measurement range of the sensor (e.g., 0-5 PSI for pressure, 0-10m for ultrasonic). This is the range that corresponds to 4-20mA output.">?</span></span><div style="display:flex;gap:8px;align-items:center;"><input type="number" class="sensor-range-min" value="0" step="0.1" style="width:70px;" placeholder="Min"><span>to</span><input type="number" class="sensor-range-max" value="5" step="0.1" style="width:70px;" placeholder="Max"><select class="sensor-range-unit" style="width:70px;"><option value="PSI">PSI</option><option value="bar">bar</option><option value="m">m</option><option value="ft">ft</option><option value="in">in</option><option value="cm">cm</option></select></div></label><label class="field sensor-mount-height-field" style="display:none;"><span><span class="mount-height-label">Sensor Mount Height(in)</span><span class="tooltip-icon mount-height-tooltip" tabindex="0" data-tooltip="For pressure sensors: height of sensor above tank bottom (usually 0-2 inches). For ultrasonic sensors: distance from sensor to tank bottom when empty.">?</span></span><input type="number" class="sensor-mount-height" value="0" step="0.1" min="0"></label><label class="field fluid-type-field" style="display:none;"><span><span>Fluid Type</span><span class="tooltip-icon" tabindex="0" data-tooltip="Specific gravity of the stored fluid. Pick the matching preset, or choose Custom and enter the SG manually. The server's calibration learning will refine this automatically once stick-gauge readings are submitted.">?</span></span><div style="display:flex;gap:8px;align-items:center;"><select class="fluid-type" onchange="updateFluidTypeFields(${id})" style="flex:1;"><option value="water">Water (1.00)</option><option value="diesel">Diesel (0.85)</option><option value="gasoline">Gasoline (0.74)</option><option value="heatingOil">Heating Oil (0.85)</option><option value="propane">Propane / LPG (0.50)</option><option value="brine">Brine (1.20)</option><option value="crudeOil">Crude Oil (0.83)</option><option value="usedOil">Used Oil (0.92)</option><option value="glycol50">Glycol 50% (1.07)</option><option value="def">DEF / AdBlue (1.09)</option><option value="ethanol">Ethanol (0.79)</option><option value="custom">Custom...</option></select><input type="number" class="fluid-sg-custom" step="0.001" min="0.3" max="2.0" placeholder="SG" style="width:80px;display:none;"></div></label><label class="field analog-voltage-field" style="display:none;"><span><span class="analog-voltage-label">Sensor Voltage Range</span><span class="tooltip-icon analog-voltage-tooltip" tabindex="0" data-tooltip="The voltage output range of the analog sensor. Common ranges: 0-10V, 0-5V, 1-5V.">?</span></span><div style="display:flex;gap:8px;align-items:center;"><input type="number" class="analog-voltage-min" value="0" step="0.1" style="width:70px;" placeholder="Min"><span>to</span><input type="number" class="analog-voltage-max" value="10" step="0.1" style="width:70px;" placeholder="Max"><span>V</span></div></label></div><label style="display:flex;align-items:center;gap:6px;margin-top:8px;"><input type="checkbox" class="stuck-detection" checked> Stuck Sensor Detection<span class="tooltip-icon" tabindex="0" data-tooltip="When enabled, the system flags a sensor as failed if it reports the same reading 10 consecutive times. Disable for sensors where readings naturally stay constant (e.g., gas pressure monitors).">?</span></label><label style="display:flex;align-items:center;gap:6px;margin-top:8px;"><input type="checkbox" class="calibration-enabled" checked> Calibration Learning<span class="tooltip-icon" tabindex="0" data-tooltip="When enabled, you can submit manual verification readings on the Calibration page to teach the system the actual relationship between sensor output and measured values. Recommended for tank level sensors. For gas pressure or RPM sensors, disable unless you have reference gauges for comparison.">?</span></label><div class="digital-sensor-info" style="display:none;background:var(--chip);border:1px solid var(--card-border);padding:12px;margin-top:8px;font-size:0.9rem;color:var(--muted);"><strong>Float Switch Mode:</strong> This sensor only detects whether fluid has reached the switch position.<br><br><strong>Wiring Note:</strong> Connect the switch between the input pin and GND. The software uses an internal pull-up resistor.</div><div class="current-loop-sensor-info" style="display:none;background:var(--chip);border:1px solid var(--card-border);padding:12px;margin-top:8px;font-size:0.9rem;color:var(--muted);"><div class="pressure-sensor-info"><strong>Pressure Sensor(Bottom-Mounted):</strong> Measures the pressure of the liquid column above it.<br>- 4mA = Empty tank (0 pressure)<br>- 20mA = Full tank (max pressure)<br>- Sensor Range: The native pressure range (e.g., 0-5 PSI)<br>- Mount Height: Distance from sensor to tank bottom</div><div class="ultrasonic-sensor-info" style="display:none;"><strong>Ultrasonic Sensor(Top-Mounted):</strong> Measures the distance from the sensor to the liquid surface.<br>- 4mA = Full tank (liquid close to sensor)<br>- 20mA = Empty tank (liquid far from sensor)<br>- Sensor Range: The native distance range (e.g., 0-10m)<br>- Sensor Mount Height: Distance from sensor to tank bottom when empty</div></div><button type="button" class="add-section-btn add-alarm-btn" onclick="toggleAlarmSection(${id})">+ Add Alarm</button><div class="collapsible-section alarm-section"><h4 style="margin:16px 0 8px;font-size:0.95rem;border-top:1px solid var(--card-border);padding-top:12px;"><span class="alarm-section-title">Alarm Thresholds</span><button type="button" class="remove-btn" onclick="removeAlarmSection(${id})" style="float:right;">Remove Alarm</button></h4><div class="form-grid alarm-thresholds-grid"><div class="field"><span><label style="display:flex;align-items:center;gap:6px;"><input type="checkbox" class="high-alarm-enabled" checked> High Alarm</label></span><input type="number" class="high-alarm" value="100"></div><div class="field"><span><label style="display:flex;align-items:center;gap:6px;"><input type="checkbox" class="low-alarm-enabled" checked> Low Alarm</label></span><input type="number" class="low-alarm" value="20"></div></div><div class="form-grid digital-alarm-grid" style="display:none;"><div class="field" style="grid-column:1 / -1;"><span>Trigger Condition<span class="tooltip-icon" tabindex="0" data-tooltip="Select when the alarm should trigger based on the float switch state.">?</span></span><select class="digital-trigger-state"><option value="activated">When Switch is Activated (fluid detected)</option><option value="not_activated">When Switch is NOT Activated (no fluid)</option></select></div></div></div><button type="button" class="add-section-btn add-relay-btn hidden" onclick="toggleRelaySection(${id})">+ Add Relay Control</button><div class="collapsible-section relay-section"><h4 style="margin:16px 0 8px;font-size:0.95rem;border-top:1px solid var(--card-border);padding-top:12px;">Relay Switch Control<button type="button" class="remove-btn" onclick="removeRelaySection(${id})" style="float:right;">Remove Relay</button></h4><div class="form-grid"><label class="field"><span>Target Client UID</span><input type="text" class="relay-target" list="relayTargetSuggestions" placeholder="dev:IMEI (optional)"></label><label class="field"><span>Trigger On</span><select class="relay-trigger"><option value="any">Any Alarm (High or Low)</option><option value="high">High Alarm Only</option><option value="low">Low Alarm Only</option></select></label><label class="field"><span>Relay Mode</span><select class="relay-mode" onchange="toggleRelayDurations(${id})"><option value="momentary">Momentary (configurable duration)</option><option value="until_clear">Stay On Until Alarm Clears</option><option value="manual_reset">Stay On Until Manual Server Reset</option></select></label><div class="field"><span>Relay Outputs</span><div style="display:flex;gap:12px;padding:8px 0;"><label style="display:flex;align-items:center;gap:4px;"><input type="checkbox" class="relay-1" value="1"> R1</label><label style="display:flex;align-items:center;gap:4px;"><input type="checkbox" class="relay-2" value="2"> R2</label><label style="display:flex;align-items:center;gap:4px;"><input type="checkbox" class="relay-3" value="4"> R3</label><label style="display:flex;align-items:center;gap:4px;"><input type="checkbox" class="relay-4" value="8"> R4</label></div></div><div class="relay-durations-section" style="grid-column:1 / -1;display:block;"><span style="font-size:0.85rem;color:var(--text-secondary);margin-bottom:8px;display:block;">Momentary Duration per Relay (seconds, 0 = default 30 min):</span><div style="display:flex;gap:12px;flex-wrap:wrap;"><label style="display:flex;align-items:center;gap:4px;font-size:0.85rem;">R1:<input type="number" class="relay-duration-1" value="0" min="0" max="86400" style="width:70px;"></label><label style="display:flex;align-items:center;gap:4px;font-size:0.85rem;">R2:<input type="number" class="relay-duration-2" value="0" min="0" max="86400" style="width:70px;"></label><label style="display:flex;align-items:center;gap:4px;font-size:0.85rem;">R3:<input type="number" class="relay-duration-3" value="0" min="0" max="86400" style="width:70px;"></label><label style="display:flex;align-items:center;gap:4px;font-size:0.85rem;">R4:<input type="number" class="relay-duration-4" value="0" min="0" max="86400" style="width:70px;"></label></div></div><label class="field relay-max-on-section" style="grid-column:1 / -1;display:none;"><span>Safety Max ON Duration (seconds, 0 = no limit)<span class="tooltip-icon" tabindex="0" data-tooltip="Maximum time a relay can stay ON in manual_reset mode. After this duration the relay is forced OFF and a relay_timeout alarm is sent. Set to 0 to disable. Max 604800 (7 days).">?</span></span><input type="number" class="relay-max-on" value="0" min="0" max="604800" style="width:120px;"></label></div></div><button type="button" class="add-section-btn add-sms-btn hidden" onclick="toggleSmsSection(${id})">+ Add SMS Alert</button><div class="collapsible-section sms-section"><h4 style="margin:16px 0 8px;font-size:0.95rem;border-top:1px solid var(--card-border);padding-top:12px;">SMS Alert Notifications<button type="button" class="remove-btn" onclick="removeSmsSection(${id})" style="float:right;">Remove SMS Alert</button></h4><div class="form-grid"><label class="field" style="grid-column:1 / -1;"><span>Phone Numbers<span class="tooltip-icon" tabindex="0" data-tooltip="Enter phone numbers with country code (e.g., +15551234567). Separate multiple numbers with commas.">?</span></span><input type="text" class="sms-phones" placeholder="+15551234567,+15559876543"></label><label class="field"><span>Trigger On</span><select class="sms-trigger"><option value="any">Any Alarm (High or Low)</option><option value="high">High Alarm Only</option><option value="low">Low Alarm Only</option></select></label><label class="field" style="grid-column:span 2;"><span>Custom Message (optional)</span><input type="text" class="sms-message" placeholder="Tank alarm triggered"></label></div></div></div>`;}
/* SENSOR FUNCTIONS */
function normalizeBulletText(root){if(!root)return;root.querySelectorAll('.current-loop-sensor-info').forEach(el=>{el.innerHTML=el.innerHTML.replace(/[^\x20-\x7E]+/g,'-');});}
function ensureSensorGuidance(card){if(!card)return;const alarmGrid=card.querySelector('.alarm-thresholds-grid');if(alarmGrid&&!card.querySelector('.alarm-guidance-note')){const note=document.createElement('div');note.className='alarm-guidance-note';note.style.cssText='grid-column:1 / -1;background:var(--chip);border:1px solid var(--card-border);padding:10px 12px;font-size:0.85rem;color:var(--muted);display:none;';alarmGrid.appendChild(note);}const relaySection=card.querySelector('.relay-durations-section');if(relaySection&&!card.querySelector('.relay-timing-note')){const note=document.createElement('div');note.className='relay-timing-note';note.style.cssText='margin-top:8px;font-size:0.82rem;color:var(--muted);';note.textContent='Momentary relay timers turn off on the next main-loop pass after expiry. Add a small safety margin if you need a tighter pulse width.';relaySection.appendChild(note);}}
function updateSensorGuidance(id){const card=document.getElementById(`sensor-${id}`);if(!card)return;ensureSensorGuidance(card);const note=card.querySelector('.alarm-guidance-note');if(!note)return;const type=parseInt(card.querySelector('.sensor-type').value,10);const isAnalog=(type!==0&&type!==2&&type!==3);if(isAnalog){note.textContent='Analog voltage thresholds are evaluated in converted inches. Keep hysteresis at 1.0 inch or higher unless you have field-tested noise margins; generated configs default to 2.0 inches.';note.style.display='block';}else{note.style.display='none';}}
function addSensor(){const container=document.getElementById('sensorsContainer');const div=document.createElement('div');div.innerHTML=createSensorHtml(sensorCount);container.appendChild(div.firstElementChild);normalizeBulletText(div.firstElementChild);ensureSensorGuidance(div.firstElementChild);updateSensorTypeFields(sensorCount);sensorCount++;}
window.removeSensor=function(id){const el=document.getElementById(`sensor-${id}`);if(el)el.remove();};
window.toggleAlarmSection=function(id){const card=document.getElementById(`sensor-${id}`);const alarmSection=card.querySelector('.alarm-section');const addAlarmBtn=card.querySelector('.add-alarm-btn');const addRelayBtn=card.querySelector('.add-relay-btn');const addSmsBtn=card.querySelector('.add-sms-btn');alarmSection.classList.add('visible');addAlarmBtn.classList.add('hidden');addRelayBtn.classList.remove('hidden');addSmsBtn.classList.remove('hidden');};
window.removeAlarmSection=function(id){const card=document.getElementById(`sensor-${id}`);const alarmSection=card.querySelector('.alarm-section');const addAlarmBtn=card.querySelector('.add-alarm-btn');const addRelayBtn=card.querySelector('.add-relay-btn');const addSmsBtn=card.querySelector('.add-sms-btn');const relaySection=card.querySelector('.relay-section');const smsSection=card.querySelector('.sms-section');alarmSection.classList.remove('visible');addAlarmBtn.classList.remove('hidden');addRelayBtn.classList.add('hidden');addSmsBtn.classList.add('hidden');relaySection.classList.remove('visible');smsSection.classList.remove('visible');card.querySelector('.high-alarm').value='100';card.querySelector('.low-alarm').value='20';card.querySelector('.high-alarm-enabled').checked=true;card.querySelector('.low-alarm-enabled').checked=true;card.querySelector('.relay-target').value='';card.querySelector('.relay-trigger').value='any';card.querySelector('.relay-mode').value='momentary';['relay-1','relay-2','relay-3','relay-4'].forEach(cls=>{card.querySelector('.'+cls).checked=false;});card.querySelector('.sms-phones').value='';card.querySelector('.sms-trigger').value='any';card.querySelector('.sms-message').value='';};
window.toggleRelaySection=function(id){const card=document.getElementById(`sensor-${id}`);const relaySection=card.querySelector('.relay-section');const addBtn=card.querySelector('.add-relay-btn');relaySection.classList.add('visible');addBtn.classList.add('hidden');};
window.removeRelaySection=function(id){const card=document.getElementById(`sensor-${id}`);const relaySection=card.querySelector('.relay-section');const addBtn=card.querySelector('.add-relay-btn');relaySection.classList.remove('visible');addBtn.classList.remove('hidden');card.querySelector('.relay-target').value='';card.querySelector('.relay-trigger').value='any';card.querySelector('.relay-mode').value='momentary';['relay-1','relay-2','relay-3','relay-4'].forEach(cls=>{card.querySelector('.'+cls).checked=false;});['relay-duration-1','relay-duration-2','relay-duration-3','relay-duration-4'].forEach(cls=>{card.querySelector('.'+cls).value='0';});card.querySelector('.relay-max-on').value='0';card.querySelector('.relay-durations-section').style.display='block';card.querySelector('.relay-max-on-section').style.display='none';};
window.toggleSmsSection=function(id){const card=document.getElementById(`sensor-${id}`);const smsSection=card.querySelector('.sms-section');const addBtn=card.querySelector('.add-sms-btn');smsSection.classList.add('visible');addBtn.classList.add('hidden');};
window.removeSmsSection=function(id){const card=document.getElementById(`sensor-${id}`);const smsSection=card.querySelector('.sms-section');const addBtn=card.querySelector('.add-sms-btn');smsSection.classList.remove('visible');addBtn.classList.remove('hidden');card.querySelector('.sms-phones').value='';card.querySelector('.sms-trigger').value='any';card.querySelector('.sms-message').value='';};
window.toggleRelayDurations=function(id){const card=document.getElementById(`sensor-${id}`);const relayMode=card.querySelector('.relay-mode').value;const durationsSection=card.querySelector('.relay-durations-section');const maxOnSection=card.querySelector('.relay-max-on-section');if(relayMode==='momentary'){durationsSection.style.display='block';maxOnSection.style.display='none';}else if(relayMode==='manual_reset'){durationsSection.style.display='none';maxOnSection.style.display='flex';}else{durationsSection.style.display='none';maxOnSection.style.display='none';}};
window.updateMonitorFields=function(id){const card=document.getElementById(`sensor-${id}`);const type=card.querySelector('.monitor-type').value;const numField=card.querySelector('.tank-num-field');const numFieldLabel=numField.querySelector('span');const nameLabel=card.querySelector('.name-label');const sensorTypeSelect=card.querySelector('.sensor-type');const pulsesPerRevField=card.querySelector('.pulses-per-rev-field');const contentsField=card.querySelector('.contents-field');const calCheckbox=card.querySelector('.calibration-enabled');if(type==='gas'){numField.style.display='flex';numFieldLabel.textContent='Display Number (optional)';nameLabel.textContent='System Name';pulsesPerRevField.style.display='none';contentsField.style.display='flex';sensorTypeSelect.value='2';calCheckbox.checked=false;updatePinOptions(id);}else if(type==='rpm'){numField.style.display='flex';numFieldLabel.textContent='Engine Number (optional)';nameLabel.textContent='Engine Name';pulsesPerRevField.style.display='flex';contentsField.style.display='none';sensorTypeSelect.value='3';calCheckbox.checked=false;updatePinOptions(id);}else{numField.style.display='flex';numFieldLabel.textContent='Tank Number (optional)';nameLabel.textContent='Name';pulsesPerRevField.style.display='none';contentsField.style.display='flex';calCheckbox.checked=true;}updateSensorTypeFields(id);};
window.updatePinOptions=function(id){const card=document.getElementById(`sensor-${id}`);const typeSelect=card.querySelector('.sensor-type');const pinSelect=card.querySelector('.sensor-pin');const type=parseInt(typeSelect.value);pinSelect.innerHTML='';let options=[];if(type===2){options=expansionChannels;}else{options=optaPins;}options.forEach(opt=>{const option=document.createElement('option');option.value=opt.value;option.textContent=opt.label;pinSelect.appendChild(option);});updateSensorTypeFields(id);};
window.updateSensorTypeFields=function(id){const card=document.getElementById(`sensor-${id}`);const type=parseInt(card.querySelector('.sensor-type').value);const digitalInfoBox=card.querySelector('.digital-sensor-info');const currentLoopInfoBox=card.querySelector('.current-loop-sensor-info');const alarmThresholdsGrid=card.querySelector('.alarm-thresholds-grid');const digitalAlarmGrid=card.querySelector('.digital-alarm-grid');const alarmSectionTitle=card.querySelector('.alarm-section-title');const pulsesPerRevField=card.querySelector('.pulses-per-rev-field');const switchModeField=card.querySelector('.switch-mode-field');const currentLoopTypeField=card.querySelector('.current-loop-type-field');const sensorMountHeightField=card.querySelector('.sensor-mount-height-field');const sensorRangeField=card.querySelector('.sensor-range-field');const analogVoltageField=card.querySelector('.analog-voltage-field');if(type===0){digitalInfoBox.style.display='block';currentLoopInfoBox.style.display='none';switchModeField.style.display='flex';currentLoopTypeField.style.display='none';sensorMountHeightField.style.display='none';sensorRangeField.style.display='none';analogVoltageField.style.display='none';alarmThresholdsGrid.style.display='none';digitalAlarmGrid.style.display='grid';alarmSectionTitle.textContent='Float Switch Alarm';pulsesPerRevField.style.display='none';}else if(type===2){const monitorType=card.querySelector('.monitor-type').value;digitalInfoBox.style.display='none';currentLoopInfoBox.style.display='block';switchModeField.style.display='none';analogVoltageField.style.display='none';const loopTypeSelect=card.querySelector('.current-loop-type');if(monitorType==='tank'){currentLoopTypeField.style.display='flex';loopTypeSelect.innerHTML='<option value="pressure">Pressure Sensor(Bottom-Mounted)</option><option value="ultrasonic">Ultrasonic Sensor(Top-Mounted)</option>';sensorMountHeightField.style.display='flex';sensorRangeField.style.display='flex';}else if(monitorType==='gas'){currentLoopTypeField.style.display='none';loopTypeSelect.innerHTML='<option value="pressure">Pressure Sensor</option>';loopTypeSelect.value='pressure';sensorMountHeightField.style.display='none';sensorRangeField.style.display='flex';}else{currentLoopTypeField.style.display='flex';loopTypeSelect.innerHTML='<option value="pressure">Pressure Sensor</option>';loopTypeSelect.value='pressure';sensorMountHeightField.style.display='none';sensorRangeField.style.display='flex';}alarmThresholdsGrid.style.display='grid';digitalAlarmGrid.style.display='none';alarmSectionTitle.textContent='Alarm Thresholds';pulsesPerRevField.style.display='none';updateCurrentLoopFields(id);}else if(type===3){digitalInfoBox.style.display='none';currentLoopInfoBox.style.display='none';switchModeField.style.display='none';currentLoopTypeField.style.display='none';sensorMountHeightField.style.display='none';sensorRangeField.style.display='none';analogVoltageField.style.display='none';alarmThresholdsGrid.style.display='grid';digitalAlarmGrid.style.display='none';alarmSectionTitle.textContent='Alarm Thresholds';pulsesPerRevField.style.display='flex';}else{const monitorType=card.querySelector('.monitor-type').value;digitalInfoBox.style.display='none';currentLoopInfoBox.style.display='none';switchModeField.style.display='none';currentLoopTypeField.style.display='none';analogVoltageField.style.display='flex';sensorRangeField.style.display='flex';if(monitorType==='tank'){sensorMountHeightField.style.display='flex';}else{sensorMountHeightField.style.display='none';}alarmThresholdsGrid.style.display='grid';digitalAlarmGrid.style.display='none';alarmSectionTitle.textContent='Alarm Thresholds';pulsesPerRevField.style.display='none';}updateSensorGuidance(id);updateFluidVisibility(id);};
window.updateCurrentLoopFields=function(id){const card=document.getElementById(`sensor-${id}`);const currentLoopType=card.querySelector('.current-loop-type').value;const currentLoopInfoBox=card.querySelector('.current-loop-sensor-info');const pressureInfo=currentLoopInfoBox.querySelector('.pressure-sensor-info');const ultrasonicInfo=currentLoopInfoBox.querySelector('.ultrasonic-sensor-info');const mountHeightLabel=card.querySelector('.mount-height-label');const mountHeightTooltip=card.querySelector('.mount-height-tooltip');const sensorRangeLabel=card.querySelector('.sensor-range-label');const sensorRangeTooltip=card.querySelector('.sensor-range-tooltip');const sensorRangeUnit=card.querySelector('.sensor-range-unit');const sensorRangeMax=card.querySelector('.sensor-range-max');if(currentLoopType==='ultrasonic'){pressureInfo.style.display='none';ultrasonicInfo.style.display='block';mountHeightLabel.textContent='Sensor Mount Height(in)';mountHeightTooltip.setAttribute('data-tooltip','Distance from the ultrasonic sensor to the sensor bottom when empty.');sensorRangeLabel.textContent='Sensor Range';sensorRangeTooltip.setAttribute('data-tooltip','Native distance range of the ultrasonic sensor (e.g., 0-10m).');sensorRangeUnit.value='m';sensorRangeMax.value='10';}else{const monitorType=card.querySelector('.monitor-type').value;pressureInfo.style.display='block';ultrasonicInfo.style.display='none';if(monitorType==='gas'){sensorRangeTooltip.setAttribute('data-tooltip','Native pressure range of the sensor that maps to 4-20mA output.');}else{mountHeightLabel.textContent='Sensor Mount Height(in)';mountHeightTooltip.setAttribute('data-tooltip','Height of the pressure sensor above the sensor bottom (usually 0-2 inches).');sensorRangeTooltip.setAttribute('data-tooltip','Native pressure range of the sensor (e.g., 0-5 PSI).');}sensorRangeLabel.textContent='Sensor Range';sensorRangeUnit.value='PSI';sensorRangeMax.value='5';}updateFluidVisibility(id);};
window.updateFluidTypeFields=function(id){const card=document.getElementById(`sensor-${id}`);const sel=card.querySelector('.fluid-type');const customInput=card.querySelector('.fluid-sg-custom');if(!sel||!customInput)return;customInput.style.display=(sel.value==='custom')?'inline-block':'none';};
window.updateFluidVisibility=function(id){const card=document.getElementById(`sensor-${id}`);const fluidField=card.querySelector('.fluid-type-field');if(!fluidField)return;const monitorType=card.querySelector('.monitor-type').value;const type=parseInt(card.querySelector('.sensor-type').value);let show=false;if(monitorType==='tank'){if(type===2){const lt=card.querySelector('.current-loop-type').value;if(lt==='pressure')show=true;}else if(type!==0&&type!==3){show=true;}}fluidField.style.display=show?'flex':'none';if(show)updateFluidTypeFields(id);};
document.getElementById('addSensorBtn').addEventListener('click',addSensor);
/* INPUTS */
function addInput(){const id=inputIdCounter++;const container=document.getElementById('inputsContainer');const card=document.createElement('div');card.className='sensor-card';card.id=`input-${id}`;card.innerHTML=`<div class="card-header" style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;"><h4 style="margin:0;font-size:1rem;">Input ${id+1}</h4><button type="button" class="remove-btn" onclick="removeInput(${id})">Remove</button></div><div class="form-grid"><label class="field"><span>Input Name</span><input type="text" class="input-name" placeholder="Clear Button" value="Clear Button"></label><label class="field"><span>Pin Number<span class="tooltip-icon" tabindex="0" data-tooltip="Arduino Opta pin number for the input.">?</span></span><input type="number" class="input-pin" value="0" min="0" max="99"></label><label class="field"><span>Input Mode<span class="tooltip-icon" tabindex="0" data-tooltip="Active LOW: Button connects pin to GND, uses internal pull-up. Active HIGH: Button connects pin to VCC, requires external pull-down.">?</span></span><select class="input-mode">${inputModes.map(m=>`<option value="${m.value}">${m.label}</option>`).join('')}</select></label><label class="field"><span>Action<span class="tooltip-icon" tabindex="0" data-tooltip="What happens when this input is activated.">?</span></span><select class="input-action">${inputActions.map(a=>`<option value="${a.value}">${a.label}</option>`).join('')}</select></label></div>`;container.appendChild(card);}
window.removeInput=function(id){const card=document.getElementById(`input-${id}`);if(card)card.remove();};
document.getElementById('addInputBtn').addEventListener('click',addInput);
/* CONFIG COLLECTION */
function sensorKeyFromValue(value){switch(value){case 0:return 'digital';case 2:return 'current';case 3:return 'rpm';default:return 'analog';}}
function collectConfig(){if(!validatePowerCombination()){showToast('Invalid power/battery combination — see warning above',true);return null;}const sMinutes=Math.max(1,Math.min(1440,parseInt(document.getElementById('sampleMinutes').value,10)||30));const time=(document.getElementById('reportTime').value||'05:00').split(':');const reportHour=parseInt(time[0])||5;const reportMinute=parseInt(time[1])||0;const psRaw=document.getElementById('powerSource').value||'grid';const btStr=document.getElementById('batteryType').value||'agm';const bvNominal=parseInt(document.getElementById('batteryVoltage').value,10)||12;const hasVin=document.getElementById('vinMonitorEnabled').value==='1';const isSolarOnly=(btStr==='none'&&psRaw.startsWith('solar'));/* battery type enum value matching TankAlarm_Battery.h (chemistry only) */const btEnumMap={'none':0,'agm':1,'flooded':2,'gel':3,'sla':4,'lifepo4':5,'li_ion':6,'lipo':7,'custom':8};const btEnum=btEnumMap[btStr]!==undefined?btEnumMap[btStr]:1;const cfg={productUid:document.getElementById('productUid').value.trim(),deviceUid:(document.getElementById('clientUid').value||'').trim(),site:document.getElementById('siteName').value.trim(),deviceLabel:document.getElementById('deviceLabel').value.trim()||'Unconfigured Client',clientFleet:(document.getElementById('clientFleet').value||'').trim(),serverFleet:document.getElementById('serverFleet').value.trim()||'tankalarm-server',sampleSeconds:sMinutes*60,reportHour:reportHour,reportMinute:reportMinute,dailyEmail:document.getElementById('dailyEmail').value.trim(),powerSupply:psRaw,solarPowered:psRaw.startsWith('solar'),mpptEnabled:psRaw.includes('mppt'),solarCharger:{enabled:psRaw==='solar_modbus_mppt'},batteryConfig:{enabled:btStr!=='none',batteryType:btEnum,batteryTypeName:btStr,nominalVoltage:(btStr==='none'||btStr==='lipo')?0:bvNominal},vinMonitor:{enabled:hasVin,pin:hasVin?parseInt(document.getElementById('vinPin').value)||0:0,r1Kohm:hasVin?parseFloat(document.getElementById('vinR1').value)||22:22,r2Kohm:hasVin?parseFloat(document.getElementById('vinR2').value)||47:47},solarOnlyConfig:{enabled:isSolarOnly,startupDebounceVoltage:(isSolarOnly&&hasVin)?parseFloat(document.getElementById('solarOnlyDebounceV').value)||10:10,startupDebounceSec:(isSolarOnly&&hasVin)?parseInt(document.getElementById('solarOnlyDebounceSec').value)||30:30,startupWarmupSec:(isSolarOnly&&!hasVin)?parseInt(document.getElementById('solarOnlyWarmupSec').value)||60:60,sensorGateVoltage:(isSolarOnly&&hasVin)?parseFloat(document.getElementById('solarOnlySensorGateV').value)||11:11,sunsetVoltage:(isSolarOnly&&hasVin)?parseFloat(document.getElementById('solarOnlySunsetV').value)||10:10,sunsetConfirmSec:isSolarOnly?parseInt(document.getElementById('solarOnlySunsetSec').value)||120:120,opportunisticReportHours:isSolarOnly?parseInt(document.getElementById('solarOnlyReportHours').value)||20:20,batteryFailureFallback:(!isSolarOnly&&psRaw.startsWith('solar')&&btStr!=='none')?!!document.getElementById('solarOnlyBatFail').checked:false,batteryFailureThreshold:parseInt(document.getElementById('solarOnlyBatFailCount').value)||10},sensors:[],clearButtonPin:-1,clearButtonActiveHigh:false};
const sensorCards=document.querySelectorAll('#sensorsContainer .sensor-card');if(!sensorCards.length){showToast('Add at least one sensor',true);return null;}
sensorCards.forEach((card,index)=>{const monitorType=card.querySelector('.monitor-type').value;const type=parseInt(card.querySelector('.sensor-type').value);const pin=parseInt(card.querySelector('.sensor-pin').value);let userNum=parseInt(card.querySelector('.tank-num').value)||0;let name=card.querySelector('.tank-name').value;const contents=card.querySelector('.tank-contents')?.value||'';if(monitorType==='gas'){if(!name)name=`Gas System ${userNum||index+1}`;}else if(monitorType==='rpm'){if(!name)name=`Engine ${userNum||index+1}`;}else{if(!name)name=`Tank ${userNum||index+1}`;}const sensor=sensorKeyFromValue(type);const pulsesPerRev=Math.max(1,Math.min(255,parseInt(card.querySelector('.pulses-per-rev').value)||1));const switchMode=card.querySelector('.switch-mode').value;const alarmSectionVisible=card.querySelector('.alarm-section').classList.contains('visible');const highAlarmEnabled=card.querySelector('.high-alarm-enabled').checked;const lowAlarmEnabled=card.querySelector('.low-alarm-enabled').checked;const highAlarmValue=card.querySelector('.high-alarm').value;const lowAlarmValue=card.querySelector('.low-alarm').value;const tank={id:String.fromCharCode(65+index),monitorType:monitorType,name:name,contents:contents,number:index+1,userNumber:userNum,sensor:sensor,primaryPin:sensor==='current'?0:pin,secondaryPin:-1,loopChannel:sensor==='current'?pin:-1,rpmPin:sensor==='rpm'?pin:-1,hysteresis:sensor==='digital'?0:2.0,daily:true,upload:true,stuckDetection:card.querySelector('.stuck-detection').checked,calibrationEnabled:card.querySelector('.calibration-enabled').checked};if(sensor==='digital'){tank.digitalSwitchMode=switchMode;}let sUnit='';if(sensor==='current'){const currentLoopType=card.querySelector('.current-loop-type').value;const sensorMountHeight=parseFloat(card.querySelector('.sensor-mount-height').value)||0;const sMin=parseFloat(card.querySelector('.sensor-range-min').value)||0;const sMax=parseFloat(card.querySelector('.sensor-range-max').value)||5;sUnit=card.querySelector('.sensor-range-unit').value||'PSI';tank.currentLoopType=currentLoopType;tank.sensorMountHeight=sensorMountHeight;tank.sensorRangeMin=sMin;tank.sensorRangeMax=sMax;tank.sensorRangeUnit=sUnit;}else if(sensor==='analog'){const sensorMountHeight=parseFloat(card.querySelector('.sensor-mount-height').value)||0;const sMin=parseFloat(card.querySelector('.sensor-range-min').value)||0;const sMax=parseFloat(card.querySelector('.sensor-range-max').value)||5;sUnit=card.querySelector('.sensor-range-unit').value||'PSI';tank.sensorMountHeight=sensorMountHeight;tank.sensorRangeMin=sMin;tank.sensorRangeMax=sMax;tank.sensorRangeUnit=sUnit;tank.analogVoltageMin=parseFloat(card.querySelector('.analog-voltage-min').value)||0;tank.analogVoltageMax=parseFloat(card.querySelector('.analog-voltage-max').value)||10;}if(monitorType==='tank'){const fluidSel=card.querySelector('.fluid-type');if(fluidSel){const fluidType=fluidSel.value;tank.fluidType=fluidType;if(fluidType==='custom'){const customSg=parseFloat(card.querySelector('.fluid-sg-custom').value);if(!isNaN(customSg)&&customSg>=0.3&&customSg<=2.0){tank.fluidSpecificGravity=customSg;}}}}if(monitorType==='gas'){tank.measurementUnit=sUnit?sUnit.toLowerCase():'psi';}else if(monitorType==='rpm'){tank.measurementUnit='rpm';}else if(monitorType==='flow'){tank.measurementUnit='gpm';}if(alarmSectionVisible){if(sensor==='digital'){const digitalTriggerState=card.querySelector('.digital-trigger-state').value;tank.digitalTrigger=digitalTriggerState;if(digitalTriggerState==='activated'){tank.highAlarm=1;}else{tank.lowAlarm=0;}tank.alarmsEnabled=true;tank.alarmSms=true;}else if(highAlarmEnabled||lowAlarmEnabled){if(highAlarmEnabled&&highAlarmValue!==''){const v=parseFloat(highAlarmValue);if(!isNaN(v))tank.highAlarm=v;}if(lowAlarmEnabled&&lowAlarmValue!==''){const v=parseFloat(lowAlarmValue);if(!isNaN(v))tank.lowAlarm=v;}tank.alarmsEnabled=true;tank.alarmSms=true;}else{tank.alarmsEnabled=false;tank.alarmSms=false;}}else{tank.alarmsEnabled=false;tank.alarmSms=false;}if(sensor==='rpm'){tank.pulsesPerRev=pulsesPerRev;}const relaySectionVisible=card.querySelector('.relay-section').classList.contains('visible');if(relaySectionVisible){let relayMask=0;['relay-1','relay-2','relay-3','relay-4'].forEach(cls=>{const cb=card.querySelector('.'+cls);if(cb.checked)relayMask|=parseInt(cb.value);});const relayTarget=card.querySelector('.relay-target').value.trim();const relayTrigger=card.querySelector('.relay-trigger').value;const relayMode=card.querySelector('.relay-mode').value;if(relayTarget){tank.relayTargetClient=relayTarget;tank.relayMask=relayMask;tank.relayTrigger=relayTrigger;tank.relayMode=relayMode;if(relayMode==='momentary'){tank.relayMomentaryDurations=[parseInt(card.querySelector('.relay-duration-1').value)||0,parseInt(card.querySelector('.relay-duration-2').value)||0,parseInt(card.querySelector('.relay-duration-3').value)||0,parseInt(card.querySelector('.relay-duration-4').value)||0];}if(relayMode==='manual_reset'){const maxOn=parseInt(card.querySelector('.relay-max-on').value)||0;if(maxOn>0)tank.relayMaxOnSeconds=Math.min(maxOn,604800);}}}const smsSectionVisible=card.querySelector('.sms-section').classList.contains('visible');if(smsSectionVisible){const smsPhones=card.querySelector('.sms-phones').value.trim();const smsTrigger=card.querySelector('.sms-trigger').value;const smsMessage=card.querySelector('.sms-message').value.trim();if(smsPhones){const phoneArray=smsPhones.split(',').map(p=>p.trim()).filter(p=>p.length>0);if(phoneArray.length>0){tank.smsAlert={phones:phoneArray,trigger:smsTrigger,message:smsMessage||'Tank alarm triggered'};}}}cfg.sensors.push(tank);});
const inputCards=document.querySelectorAll('#inputsContainer .sensor-card');let clearButtonConfigured=false;inputCards.forEach(card=>{const inputAction=card.querySelector('.input-action').value;if(inputAction==='clear_relays'&&!clearButtonConfigured){cfg.clearButtonPin=parseInt(card.querySelector('.input-pin').value)||0;cfg.clearButtonActiveHigh=(card.querySelector('.input-mode').value==='active_high');clearButtonConfigured=true;}});return cfg;}
/* SUBMIT & DOWNLOAD */
const retryBtn=document.getElementById('retryConfigBtn');
async function submitConfig(e){e.preventDefault();try{const clientUid=(els.clientUid&&els.clientUid.value?els.clientUid.value:'').trim();if(!clientUid){showToast('Device UID is required to send config',true);return;}const cfg=collectConfig();if(!cfg)return;const res=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid,config:cfg})});const resText=await res.text();if(res.status===200){if(resText&&resText.includes('WARNING')){showToast(resText,'warn',6000);}else{showToast('Configuration saved and queued for device');}retryBtn.style.display='none';if(syncBtn)syncBtn.style.display='inline-block';}else if(res.status===202){showToast(resText||'Config saved locally — Notecard send failed');retryBtn.style.display='inline-block';if(syncBtn)syncBtn.style.display='inline-block';}else{showToast('Error: '+(resText||res.statusText),true);}}catch(err){showToast('Error: '+err.message,true);}}
async function retryConfig(){const clientUid=(els.clientUid&&els.clientUid.value?els.clientUid.value:'').trim();try{const res=await fetch('/api/config/retry',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid||undefined})});const t=await res.text();if(res.ok){showToast(t||'Config dispatched');retryBtn.style.display='none';}else{showToast(t||'Retry failed — check serial monitor',true);}}catch(err){showToast('Error: '+err.message,true);}}
retryBtn.addEventListener('click',retryConfig);
const syncBtn=document.getElementById('syncRequestBtn');
async function requestSync(){const clientUid=(els.clientUid&&els.clientUid.value?els.clientUid.value:'').trim();if(!clientUid){showToast('Device UID required',true);return;}try{const res=await fetch('/api/sync-request',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid})});const t=await res.text();if(res.ok){showToast(t||'Sync request sent');syncBtn.style.display='none';}else{showToast(t||'Sync request failed',true);}}catch(err){showToast('Error: '+err.message,true);}}
if(syncBtn)syncBtn.addEventListener('click',requestSync);
if(els.form)els.form.addEventListener('submit',submitConfig);
document.getElementById('downloadBtn').addEventListener('click',()=>{const cfg=collectConfig();if(!cfg)return;const blob=new Blob([JSON.stringify(cfg,null,2)],{type:'application/json'});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download='client_config.json';document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);});
/* LOAD CONFIG */
window.loadConfig=function(c){if(els.siteName)els.siteName.value=c.site||'';if(els.clientUid)els.clientUid.value=c.deviceUid||'';if(els.deviceLabel)els.deviceLabel.value=c.deviceLabel||'';if(els.clientFleet)els.clientFleet.value=c.clientFleet||'';if(els.productUid)els.productUid.value=c.productUid||'';if(els.sampleMinutes)els.sampleMinutes.value=(c.sampleSeconds||1800)/60;if(els.dailyEmail)els.dailyEmail.value=c.dailyEmail||'';const h=String(c.reportHour||5).padStart(2,'0');const m=String(c.reportMinute||0).padStart(2,'0');if(els.reportTime)els.reportTime.value=`${h}:${m}`;
/* Power model: powerSupply + batteryConfig.batteryType + batteryConfig.nominalVoltage + vinMonitor.enabled */
let psSel=c.powerSupply||'grid';
let btSel='agm';let bvSel=12;let vinSel=(c.vinMonitor&&c.vinMonitor.enabled)?'1':'0';
if(c.batteryConfig&&c.batteryConfig.batteryTypeName){btSel=c.batteryConfig.batteryTypeName;bvSel=c.batteryConfig.nominalVoltage||12;}
else if(c.batteryConfig&&typeof c.batteryConfig.batteryType==='number'){const revMap={0:'none',1:'agm',2:'flooded',3:'gel',4:'sla',5:'lifepo4',6:'li_ion',7:'lipo',8:'custom'};btSel=revMap[c.batteryConfig.batteryType]||'agm';bvSel=c.batteryConfig.nominalVoltage||12;if(!c.batteryConfig.enabled)btSel='none';}
const psEl=document.getElementById('powerSource');if(psEl)psEl.value=psSel;
const btEl=document.getElementById('batteryType');if(btEl)btEl.value=btSel;
const bvEl=document.getElementById('batteryVoltage');if(bvEl)bvEl.value=String(bvSel||12);
const vinEl=document.getElementById('vinMonitorEnabled');if(vinEl)vinEl.value=vinSel;
updatePowerConfigInfo();
if(c.vinMonitor){if(c.vinMonitor.pin!==undefined)document.getElementById('vinPin').value=c.vinMonitor.pin;if(c.vinMonitor.r1Kohm!==undefined)document.getElementById('vinR1').value=c.vinMonitor.r1Kohm;if(c.vinMonitor.r2Kohm!==undefined)document.getElementById('vinR2').value=c.vinMonitor.r2Kohm;updateVinCalc();}if(c.solarOnlyConfig){const so=c.solarOnlyConfig;if(so.startupDebounceVoltage!==undefined)document.getElementById('solarOnlyDebounceV').value=so.startupDebounceVoltage;if(so.startupDebounceSec!==undefined)document.getElementById('solarOnlyDebounceSec').value=so.startupDebounceSec;if(so.startupWarmupSec!==undefined)document.getElementById('solarOnlyWarmupSec').value=so.startupWarmupSec;if(so.sensorGateVoltage!==undefined)document.getElementById('solarOnlySensorGateV').value=so.sensorGateVoltage;if(so.sunsetVoltage!==undefined)document.getElementById('solarOnlySunsetV').value=so.sunsetVoltage;if(so.sunsetConfirmSec!==undefined)document.getElementById('solarOnlySunsetSec').value=so.sunsetConfirmSec;if(so.opportunisticReportHours!==undefined)document.getElementById('solarOnlyReportHours').value=so.opportunisticReportHours;if(so.batteryFailureFallback!==undefined)document.getElementById('solarOnlyBatFail').checked=so.batteryFailureFallback;if(so.batteryFailureThreshold!==undefined)document.getElementById('solarOnlyBatFailCount').value=so.batteryFailureThreshold;}document.getElementById('sensorsContainer').innerHTML='';sensorCount=0;if(c.sensors)c.sensors.forEach(t=>{addSensor();const card=document.getElementById(`sensor-${sensorCount-1}`);if(card){const monitorSel=card.querySelector('.monitor-type');if(t.monitorType){monitorSel.value=t.monitorType;}else if(t.name&&t.name.match(/gas/i)){monitorSel.value='gas';}else if(t.sensor==='rpm'){monitorSel.value='rpm';}else{monitorSel.value='tank';}updateMonitorFields(sensorCount-1);card.querySelector('.tank-num').value=t.userNumber||'';card.querySelector('.tank-name').value=t.name||'';card.querySelector('.tank-contents').value=t.contents||'';const sensorVal=t.sensor==='digital'?0:(t.sensor==='current'?2:(t.sensor==='rpm'?3:1));card.querySelector('.sensor-type').value=sensorVal;updatePinOptions(sensorCount-1);if(t.sensor==='current'){card.querySelector('.sensor-pin').value=t.loopChannel||0;}else if(t.sensor==='rpm'){card.querySelector('.sensor-pin').value=t.rpmPin||0;}else{card.querySelector('.sensor-pin').value=t.primaryPin||0;}if(t.digitalSwitchMode)card.querySelector('.switch-mode').value=t.digitalSwitchMode;if(t.pulsesPerRev)card.querySelector('.pulses-per-rev').value=t.pulsesPerRev;if(t.currentLoopType)card.querySelector('.current-loop-type').value=t.currentLoopType;if(t.sensorMountHeight!==undefined)card.querySelector('.sensor-mount-height').value=t.sensorMountHeight;if(t.highAlarm!==undefined||t.lowAlarm!==undefined||t.alarmSms){toggleAlarmSection(sensorCount-1);if(t.highAlarm!==undefined){card.querySelector('.high-alarm').value=t.highAlarm;card.querySelector('.high-alarm-enabled').checked=true;}if(t.lowAlarm!==undefined){card.querySelector('.low-alarm').value=t.lowAlarm;card.querySelector('.low-alarm-enabled').checked=true;}}if(t.relayTargetClient){toggleRelaySection(sensorCount-1);card.querySelector('.relay-target').value=t.relayTargetClient;if(t.relayTrigger)card.querySelector('.relay-trigger').value=t.relayTrigger;if(t.relayMode)card.querySelector('.relay-mode').value=t.relayMode;if(t.relayMask){if(t.relayMask&1)card.querySelector('.relay-1').checked=true;if(t.relayMask&2)card.querySelector('.relay-2').checked=true;if(t.relayMask&4)card.querySelector('.relay-3').checked=true;if(t.relayMask&8)card.querySelector('.relay-4').checked=true;}if(t.relayMomentaryDurations){card.querySelector('.relay-duration-1').value=t.relayMomentaryDurations[0]||0;card.querySelector('.relay-duration-2').value=t.relayMomentaryDurations[1]||0;card.querySelector('.relay-duration-3').value=t.relayMomentaryDurations[2]||0;card.querySelector('.relay-duration-4').value=t.relayMomentaryDurations[3]||0;}if(t.relayMaxOnSeconds){card.querySelector('.relay-max-on').value=t.relayMaxOnSeconds;}toggleRelayDurations(sensorCount-1);}if(t.smsAlert&&t.smsAlert.phones){toggleSmsSection(sensorCount-1);card.querySelector('.sms-phones').value=t.smsAlert.phones.join(',');if(t.smsAlert.trigger)card.querySelector('.sms-trigger').value=t.smsAlert.trigger;if(t.smsAlert.message)card.querySelector('.sms-message').value=t.smsAlert.message;}updateSensorTypeFields(sensorCount-1);if(t.sensorRangeMin!==undefined)card.querySelector('.sensor-range-min').value=t.sensorRangeMin;if(t.sensorRangeMax!==undefined)card.querySelector('.sensor-range-max').value=t.sensorRangeMax;if(t.sensorRangeUnit)card.querySelector('.sensor-range-unit').value=t.sensorRangeUnit;if(t.analogVoltageMin!==undefined)card.querySelector('.analog-voltage-min').value=t.analogVoltageMin;if(t.analogVoltageMax!==undefined)card.querySelector('.analog-voltage-max').value=t.analogVoltageMax;if(t.stuckDetection!==undefined)card.querySelector('.stuck-detection').checked=t.stuckDetection;if(t.calibrationEnabled!==undefined)card.querySelector('.calibration-enabled').checked=t.calibrationEnabled;if(t.fluidType){const fSel=card.querySelector('.fluid-type');if(fSel){fSel.value=t.fluidType;updateFluidTypeFields(sensorCount-1);}}if(t.fluidSpecificGravity!==undefined&&t.fluidSpecificGravity>0){const fSel=card.querySelector('.fluid-type');const fIn=card.querySelector('.fluid-sg-custom');if(fSel&&fIn){fSel.value='custom';fIn.value=t.fluidSpecificGravity;updateFluidTypeFields(sensorCount-1);}}updateFluidVisibility(sensorCount-1);}});showToast('Configuration loaded');};
/* CLOUD & IMPORT */
function closeSelectClientModal(){els.selectClientModal.classList.add('hidden');}window.closeSelectClientModal=closeSelectClientModal;
els.loadFromCloudBtn.addEventListener('click',async()=>{els.selectClientModal.classList.remove('hidden');els.clientList.innerHTML='Loading...';try{const r=await fetch('/api/clients?summary=1');const d=await r.json();const clients=d.cs||[];els.clientList.innerHTML=clients.map(c=>{const uid=c.c||'';const label=c.n||uid;const site=c.s||'';return `<div class="client-item" onclick="fetchClientConfig('${escapeHtml(uid)}')"><strong>${escapeHtml(label)}</strong> (${escapeHtml(uid)})<br>${escapeHtml(site)}</div>`;}).join('');}catch(e){els.clientList.innerHTML='Error loading clients';}});
window.fetchClientConfig=async(uid)=>{closeSelectClientModal();try{const r=await fetch('/api/client?uid='+encodeURIComponent(uid));if(!r.ok)throw new Error('Failed');const c=await r.json();if(els.clientUid)els.clientUid.value=uid;if(c.config)loadConfig(c.config);else showToast('No config found for client',true);}catch(e){showToast('Error loading config',true);}};
const importInput=document.getElementById('importFileInput');document.getElementById('importBtn').addEventListener('click',()=>importInput.click());importInput.addEventListener('change',(e)=>{const f=e.target.files[0];if(!f)return;const r=new FileReader();r.onload=(evt)=>{try{loadConfig(JSON.parse(evt.target.result));}catch(err){showToast('Invalid JSON',true);}};r.readAsText(f);});
function getQueryParam(name){const params=new URLSearchParams(window.location.search);return params.get(name)||'';}
async function basicInit(){try{const r=await fetch('/api/clients?summary=1');const d=await r.json();if(d&&d.srv&&d.srv.pu)els.productUid.value=d.srv.pu;if(d&&d.srv&&d.srv.cf)els.clientFleet.value=d.srv.cf;window._siteClients=d;populateRelayTargets(d);}catch(e){}const urlUid=getQueryParam('uid');if(urlUid&&els.clientUid)els.clientUid.value=urlUid;const urlSite=getQueryParam('site');if(urlSite&&els.siteName)els.siteName.value=urlSite;if(urlUid){await fetchClientConfig(urlUid);}else{addSensor();}}function populateRelayTargets(d){const dl=document.getElementById('relayTargetSuggestions');if(!dl||!d||!d.cs)return;const seen=new Set();d.cs.forEach(c=>{const uid=c.c||'';if(uid&&!seen.has(uid)){seen.add(uid);const opt=document.createElement('option');const label=c.n||c.s||'';opt.value=uid;opt.label=label?label+' ('+uid+')':uid;dl.appendChild(opt);}});}basicInit();
})();
</script></body></html>)HTML";

static const char SERIAL_MONITOR_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Serial Monitor - Tank Alarm Server</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Server Serial Output</h2><div class="controls"><button id="refreshServerBtn">Refresh Server Logs</button><button class="secondary" id="clearServerBtn">Clear Display</button><span style="color: var(--muted); font-size: 0.9rem;">Auto-refreshes every 5 seconds</span></div><div class="log-container" id="serverLogs"><div class="empty-state">Loading server logs...</div></div></div><div class="card"><h2>Client Serial Output</h2><div class="controls-grid"><label class="control-group"><span>Site Filter</span><select id="siteSelect"><option value="all">All sites</option></select></label><label class="control-group"><span>Client</span><select id="clientSelect"><option value="">-- Select a client --</option></select></label><)HTML" R"HTML(div class="control-group compact" style="flex: 1 1 340px;"><button id="pinClientBtn" disabled>Add Client Panel</button><button id="pinSiteBtn" class="secondary" disabled>Add Site Clients</button><button id="clearClientBtn" class="secondary">Clear Panels</button></div></div><div class="panel-grid" id="clientPanels"><div class="empty-state">Pin one or more clients to view their serial output alongside the server.</div></div></div></main><div id="toast"></div><script>(async () => {const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);/* PAUSE LOGIC */const pause_els={btn:document.getElementById('pauseBtn'),toast:document.getElementById('toast')};
const pause_state={paused:false};if(pause_els.btn)pause_els.btn.addEventListener('click',togglePauseFlow);if(pause_els.btn)pause_els.btn.addEventListener('mouseenter',()=>{if(pause_state.paused)pause_els.btn.textContent='Resume';});if(pause_els.btn)pause_els.btn.addEventListener('mouseleave',()=>{renderPauseBtn();});await fetch('/api/clients?summary=1').then(r=>r.json()).then(d=>{if(d&&d.srv){pause_state.paused=!!d.srv.ps;renderPauseBtn();}}).catch(e=>console.error('Failed to load pause state',e));
funct)HTML" R"HTML(ion showPauseToast(message,isError){if(!pause_els.toast)return;const t=pause_els.toast;t.textContent=message;t.style.background=isError?'#dc2626':'#0284c7';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500);}
funct)HTML" R"HTML(ion renderPauseBtn(){const btn=pause_els.btn;if(!btn)return;if(pause_state.paused){btn.classList.add('paused');btn.style.display='';btn.textContent='Unpause';btn.title='Resume data flow';}else{btn.classList.remove('paused');btn.style.display='none';}}
async funct)HTML" R"HTML(ion togglePauseFlow(){const targetPaused=!pause_state.paused;try{const res=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:targetPaused})});if(!res.ok){const text=await res.text();throw new Error(text||'Pause toggle failed');}const data=await res.json();pause_state.paused=!!data.paused;renderPauseBtn();showPauseToast(pause_state.paused?'Paused for maintenance':'Resumed');}catch(err){showPauseToast(err.message||'Pause toggle failed',true);}}(async ()=>{funct)HTML" R"HTML(ion escapeHtml(str){if(!str)return'';const div=document.createElement('div');div.textContent=str;return div.innerHTML;}const state ={siteFilter:'all',clients:[],clientsByUid:new Map(),clientsBySite:new Map()};
const clientPanels = new Map();const els ={serverLogs:document.getElementById('serverLogs'),refreshServerBtn:document.getElementById('refreshServerBtn'),clearServerBtn:document.getElementById('clearServerBtn'),siteSelect:document.getElementById('siteSelect'),clientSelect:document.getElementById('clientSelect'),pinClientBtn:document.getElementById('pinClientBtn'),pinSiteBtn:document.getElementById('pinSiteBtn'),clearClientBtn:document.getElementById('clearClientBtn'),clientPanels:document.getElementById('clientPanels'),toast:document.getElementById('toast')};
let serverRefreshTimer = null;funct)HTML" R"HTML(ion showToast(message,isError){if(els.toast)els.toast.textContent= message;if(els.toast)els.toast.style.background = isError ? '#dc2626':'#0284c7';if(els.toast)els.toast.classList.add('show');setTimeout(()=>{if(els.toast)els.toast.classList.remove('show')},2500);}
funct)HTML" R"HTML(ion formatTime(epoch){if(!epoch)return '--';const date = new Date(epoch * 1000);return date.toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',second:'2-digit'});}
funct)HTML" R"HTML(ion formatRelative(epoch){if(!epoch)return '';const diff = Math.max(0,(Date.now()/ 1000)- epoch);if(diff < 60)return `${Math.floor(diff)}s ago`;if(diff < 3600)return `${Math.floor(diff / 60)}m ago`;if(diff < 86400)return `${Math.floor(diff / 3600)}h ago`;return `${Math.floor(diff / 86400)}d ago`;}
funct)HTML" R"HTML(ion renderLogs(container,logs){if(!logs || logs.length === 0){container.innerHTML = '<div class="empty-state">No logs available</div>';return;}container.innerHTML = '';logs.forEach(entry =>{const div = document.createElement('div');div.className = 'log-entry';const time = document.createElement('span');time.className = 'log-time';time.textContent = formatTime(entry.timestamp);const meta = document.createElement('span');meta.className = 'log-meta';const level =(entry.level || 'info').toUpperCase();const source = entry.source ? ` - ${entry.source}`:'';meta.textContent = `[${level}${source}]`;const msg = document.createElement('span');msg.className = 'log-message';msg.textContent = entry.message;div.appendChild(time);div.appendChild(meta);div.appendChild(msg);container.appendChild(div);});container.scrollTop = container.scrollHeight;}
async funct)HTML" R"HTML(ion refreshServerLogs(){try{const res = await fetch('/api/serial-logs?source=server');if(!res.ok)throw new Error('Failed to fetch server logs');const data = await res.json();renderLogs(els.serverLogs,data.logs || []);}catch(err){console.error('Server logs error:',err);}}
funct)HTML" R"HTML(ion ensureClientPanelPlaceholder(){if(clientPanels.size === 0){els.clientPanels.innerHTML = '<div class="empty-state">Pin one or more clients to view their serial output alongside the server.</div>';}}
funct)HTML" R"HTML(ion clearClientPanelPlaceholder(){if(clientPanels.size === 0){els.clientPanels.innerHTML = '';}}
funct)HTML" R"HTML(ion setClients(list){state.clients = list;state.clientsByUid = new Map(list.map(c => [c.uid,c]));state.clientsBySite = new Map();list.forEach(c =>{const site = c.site || 'Unassigned';if(!state.clientsBySite.has(site)){state.clientsBySite.set(site,[]);}state.clientsBySite.get(site).push(c);});}
funct)HTML" R"HTML(ion renderSiteOptions(){const current = state.siteFilter;const sites = Array.from(state.clientsBySite.keys()).sort((a,b)=> a.localeCompare(b));els.siteSelect.innerHTML = '';const allOption = document.createElement('option');allOption.value = 'all';allOption.textContent = 'All sites';els.siteSelect.appendChild(allOption);sites.forEach(site =>{const opt = document.createElement('option');opt.value = site;opt.textContent = site;els.siteSelect.appendChild(opt);});if(current !== 'all' && sites.includes(current)){els.siteSelect.value = current;}else{state.siteFilter = 'all';els.siteSelect.value = 'all';}updateSiteButtons();}
funct)HTML" R"HTML(ion renderClientOptions(){const previous = els.clientSelect.value;const candidates = state.siteFilter === 'all' ? state.clients:(state.clientsBySite.get(state.siteFilter)|| []);els.clientSelect.innerHTML = '';const placeholder = document.createElement('option');placeholder.value = '';placeholder.textContent = '-- Select a client --';els.clientSelect.appendChild(placeholder);candidates.forEach(c =>{const opt = document.createElement('option');opt.value = c.uid;const label = c.label ? `${c.label}(${c.uid})`:c.uid;opt.textContent = `${c.site || 'Unassigned'} - ${label}`;els.clientSelect.appendChild(opt);});if(previous && candidates.some(c => c.uid === previous)){els.clientSelect.value = previous;els.pinClientBtn.disabled = false;}else{els.clientSelect.value = '';els.pinClientBtn.disabled = true;}}
funct)HTML" R"HTML(ion getClientInfo(uid){return state.clientsByUid.get(uid);}
funct)HTML" R"HTML(ion updatePanelHeader(uid){const info = getClientInfo(uid);const panelState = clientPanels.get(uid);if(!info || !panelState){return;}panelState.title.textContent = info.site || 'Unknown Site';panelState.subtitle.textContent = info.label || uid;panelState.meta.textContent = info.uid || uid;}
funct)HTML" R"HTML(ion updateSiteButtons(){if(state.siteFilter === 'all'){els.pinSiteBtn.disabled = true;return;}const count = state.clientsBySite.get(state.siteFilter)?.length || 0;els.pinSiteBtn.disabled = count === 0;}
funct)HTML" R"HTML(ion pinClient(uid){if(!uid){showToast('Select a client first',true);return;}if(clientPanels.has(uid)){showToast('Client already pinned');refreshClientLogs(uid);return;}createClientPanel(uid);}
async funct)HTML" R"HTML(ion pinSiteClients(){if(state.siteFilter === 'all'){showToast('Select a specific site first',true);return;}const clients = state.clientsBySite.get(state.siteFilter)|| [];if(!clients.length){showToast('No clients found for that site',true);return;}els.pinSiteBtn.disabled = true;for(const c of clients){if(!clientPanels.has(c.uid)){createClientPanel(c.uid);await new Promise(r => setTimeout(r,500));}}els.pinSiteBtn.disabled = false;}
funct)HTML" R"HTML(ion clearClientPanels(){clientPanels.forEach(panel => panel.root.remove());clientPanels.clear();ensureClientPanelPlaceholder();}
funct)HTML" R"HTML(ion createClientPanel(uid){const info = getClientInfo(uid);if(!info){showToast('Client metadata not available yet',true);return;}if(clientPanels.size === 0){clearClientPanelPlaceholder();}const panel = document.createElement('div');panel.className = 'client-panel';panel.dataset.client = uid;panel.innerHTML = ` <div class="panel-head"><div><div class="panel-title">${escapeHtml(info.site || 'Unknown Site')}</div><div class="panel-subtitle">${escapeHtml(info.label || uid)}</div><div class="panel-meta">${escapeHtml(uid)}</div></div><div class="panel-actions"><button class="secondary" data-action="refresh">Refresh</button><button data-action="request">Request Logs</button><button class="ghost" data-action="close" title="Remove panel">&times;</button></div></div><div class="chip-row"><span class="chip" data-role="lastLog">Last log:--</span><span class="chip" data-role="lastAck">Ack:--</span><span class="chip" data-role="status">Status:idle</span></div><div class="log-container slim"><div class="empty-state">No logs yet</div></div> `;els.clientPanels.appendChild(panel);const panelState ={root:panel,logs:panel.querySelector('.log-container'),title:panel.querySelector('.panel-title'),subtitle:panel.querySelector('.panel-subtitle'),meta:panel.querySelector('.panel-meta'),requestBtn:panel.querySelector('[data-action="request"]'),refreshBtn:panel.querySelector('[data-action="refresh"]'),chips:{lastLog:panel.querySelector('[data-role="lastLog"]'),lastAck:panel.querySelector('[data-role="lastAck"]'),status:panel.querySelector('[data-role="status"]')}};panelState.refreshBtn.addEventListener('click',()=> refreshClientLogs(uid));panelState.requestBtn.addEventListener('click',()=> requestClientLogs(uid,panelState));panel.querySelector('[data-action="close"]').addEventListener('click',()=> removeClientPanel(uid));clientPanels.set(uid,panelState);refreshClientLogs(uid);}
funct)HTML" R"HTML(ion removeClientPanel(uid){const panelState = clientPanels.get(uid);if(!panelState){return;}panelState.root.remove();clientPanels.delete(uid);ensureClientPanelPlaceholder();}
funct)HTML" R"HTML(ion updatePanelMeta(panelState,meta ={}){const lastLog = meta.lastLogEpoch || meta.lastLog;const lastAck = meta.lastAckEpoch;const ackStatus = meta.lastAckStatus || 'n/a';panelState.chips.lastLog.textContent = lastLog ? `Last log:${formatTime(lastLog)}(${formatRelative(lastLog)})`:'Last log:--';panelState.chips.lastAck.textContent = lastAck ? `Ack:${formatTime(lastAck)}(${ackStatus})`:'Ack:--';panelState.chips.status.textContent = meta.awaitingLogs ? 'Status:awaiting client response':'Status:ready';}
async funct)HTML" R"HTML(ion refreshClientLogs(clientUid){const panelState = clientPanels.get(clientUid);if(!panelState){return;}panelState.refreshBtn.disabled = true;panelState.chips.status.textContent = 'Status:refreshing...';try{const res = await fetch(`/api/serial-logs?source=client&client=${encodeURIComponent(clientUid)}`);if(!res.ok)throw new Error('Failed to fetch client logs');const data = await res.json();renderLogs(panelState.logs,data.logs || []);updatePanelMeta(panelState,data.meta ||{});}catch(err){console.error('Client logs error:',err);panelState.logs.innerHTML = '<div class="empty-state">Error loading client logs</div>';panelState.chips.status.textContent = 'Status:error fetching logs';}finally{panelState.refreshBtn.disabled = false;}}
async funct)HTML" R"HTML(ion requestClientLogs(clientUid,panelState){if(!clientUid){showToast('Select a client first',true);return;}if(panelState){panelState.requestBtn.disabled = true;panelState.chips.status.textContent = 'Status:requesting logs...';}try{const res = await fetch('/api/serial-request',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid})});if(!res.ok){const text = await res.text();throw new Error(text || 'Request failed');}showToast(`Log request sent to ${clientUid}`);if(panelState){panelState.chips.status.textContent = 'Status:awaiting client response';setTimeout(()=> refreshClientLogs(clientUid),3500);}}catch(err){console.error('Client request error:',err);showToast(err.message || 'Request failed',true);if(panelState){panelState.chips.status.textContent = 'Status:request failed';}}finally{if(panelState){setTimeout(()=>{panelState.requestBtn.disabled = false;},4000);}}}
async funct)HTML" R"HTML(ion loadClients(){try{const res = await fetch('/api/clients?summary=1');if(!res.ok)throw new Error('Failed to fetch clients');const data = await res.json();const rawClients = data.cs || [];const unique = new Map();rawClients.forEach(c =>{if(c.c){unique.set(c.c,{uid:c.c,site:c.s || 'Unassigned',label:c.n || c.c});}});setClients(Array.from(unique.values()));renderSiteOptions();renderClientOptions();clientPanels.forEach((_,uid)=> updatePanelHeader(uid));}catch(err){console.error('Failed to load clients:',err);}}if(els.refreshServerBtn)els.refreshServerBtn.addEventListener('click',refreshServerLogs);if(els.clearServerBtn)els.clearServerBtn.addEventListener('click',()=>{els.serverLogs.innerHTML = '<div class="empty-state">Logs cleared</div>';});if(els.siteSelect)els.siteSelect.addEventListener('change',()=>{state.siteFilter = els.siteSelect.value;renderClientOptions();updateSiteButtons();});if(els.clientSelect)els.clientSelect.addEventListener('change',()=>{els.pinClientBtn.disabled = !els.clientSelect.value;});if(els.pinClientBtn)els.pinClientBtn.addEventListener('click',()=> pinClient(els.clientSelect.value));if(els.pinSiteBtn)els.pinSiteBtn.addEventListener('click',pinSiteClients);if(els.clearClientBtn)els.clearClientBtn.addEventListener('click',clearClientPanels);serverRefreshTimer = setInterval(refreshServerLogs,5000);await refreshServerLogs();ensureClientPanelPlaceholder();await loadClients();})();})();
</script></body></html>)HTML";

static const char CALIBRATION_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Calibration Learning - Tank Alarm Server</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Add Calibration Reading</h2><p style="color: var(--muted); margin-bottom: 16px;"> Enter a manually verified sensor reading. For best results, take readings at different levels (e.g., 25%, 50%, 75%, full). Works with sensor levels, gas pressure gauges, or other 4-20mA sensors. </p><form id="calibrationForm"><div class="form-grid"><label class="field"><span>Select Sensor</span><select id="sensorSelect" required><option value="">-- Select a sensor --</option></select></label><label class="field"><span>Verified Level</span><div id="levelInputContainer"><div id="levelInputTank" class="level-input-group"><input type="number" id="levelFeet" min="0" max="100" placeholder="Feet"><span>'</span><input type="number" id="levelInches" min="0" max="11.9" step="0.1" placeholder="Inches"><span>"</span></div><div id="levelInputGeneric" style="display:none;"><div style="display:flex;gap:8px;align-items:center;"><input type="number" id="levelValue" step="0.01" placeholder="Value"><span id="levelUnitLabel">PSI</span></div></div></div></label><labe)HTML" R"HTML(l class="field"><span>Reading Timestamp</span><input type="datetime-local" id="readingTimestamp"></label></div><label class="field" style="margin-top: 12px;"><span>Notes (optional)</span><textarea id="notes" rows="2" placeholder="e.g., Measured with stick gauge at front of tank"></textarea></label><div style="margin-top: 16px;"><button type="submit">Submit Calibration Reading</button><button type="button" class="secondary" onclick="document.getElementById('calibrationForm').reset();">Clear Form</button></div></form><div class="info-box"><strong>How it works:</strong> Each calibration reading pairs a verified measurement (from a stick gauge, reference gauge, or other method) with the raw 4-20mA sensor reading from telemetry. With at least 2 data points at different levels, the system calculates a linear regression to determine the actual relationship between sensor output and measured value. This learned calibration replaces the theoretical range-based calculation. <br><br><strong>Temperature Compensation:</strong> When 5+ calibration readings with temperature data are collected across a 10°F+ temperature range, the system automatically learns a temperature coefficient. This allows predictions to account for thermal expansion/contraction effects on readings. The status will show "Calibrated+Temp" when temperature compensation is active. </div></div><div class="card"><h2>Calibration Status</h2><div id="calibrationStats" class="stats-row"><div class="stat-item"><span>Total Sensors</span><strong id="statTotalTanks">0</strong></div><div class="stat-item"><span>Calibrated</span><strong id="statCalibrated">0</strong></div><div class="stat-item"><span>Learning (1 point)</span><strong id="statLearning">0</strong></div><div class="stat-item"><span>Uncalibrated</span><strong id="statUncalibrated">0</strong></div></div><table><thead><tr><th>Sensor</th><th>Site</th><th>Status</th><th>Data Points</th><th>R2 Fit</th><th>Learned Slope</th><th>Temp Coef</th><th>Drift from Original</th><th>Last Calibration</th><th>Actions</th></tr></thead><tbody id="calibrationTableBody"></tbody></table></div><div class="card"><h2>Calibration Log</h2><div class="form-grid" style="margin-bottom: 12px;"><label class="field"><span>Filter by Sensor</span><select id="logSensorFilter"><option value="">All Sensors</option></select></label></div><table><thead><tr><th>Timestamp</th><th>Sensor</th><th>Sensor (mA)</th><th)HTML" R"HTML(>Verified Value</th><th>Temp °F</th><th>Notes</th></tr></thead><tbody id="logTableBody"></tbody></table></div><div class="card"><h2>Drift Analysis</h2><p style="color: var(--muted); margin-bottom: 16px;"> Shows how sensor accuracy has changed over time. High drift may indicate sensor degradation, tank modifications, or environmental factors. </p><div id="driftAnalysis"><p style="color: var(--muted);">Select a sensor with calibration data to view drift analysis.</p></div></div></main><div id="toast"></div><script>(async () => {const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);/* PAUSE LOGIC */const pause_els={btn:document.getElementById('pauseBtn'),toast:document.getElementById('toast')};
const pause_state={paused:false};if(pause_els.btn)pause_els.btn.addEventListener('click',togglePauseFlow);if(pause_els.btn)pause_els.btn.addEventListener('mouseenter',()=>{if(pause_state.paused)pause_els.btn.textContent='Resume';});if(pause_els.btn)pause_els.btn.addEventListener('mouseleave',()=>{renderPauseBtn();});await fetch('/api/clients?summary=1').then(r=>r.json()).then(d=>{if(d&&d.srv){pause_state.paused=!!d.srv.ps;renderPauseBtn();}}).catch(e=>console.error('Failed to load pause state',e));
funct)HTML" R"HTML(ion showPauseToast(message,isError){if(!pause_els.toast)return;const t=pause_els.toast;t.textContent=message;t.style.background=isError?'#dc2626':'#0284c7';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500);}
funct)HTML" R"HTML(ion renderPauseBtn(){const btn=pause_els.btn;if(!btn)return;if(pause_state.paused){btn.classList.add('paused');btn.style.display='';btn.textContent='Unpause';btn.title='Resume data flow';}else{btn.classList.remove('paused');btn.style.display='none';}}
async funct)HTML" R"HTML(ion togglePauseFlow(){const targetPaused=!pause_state.paused;try{const res=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:targetPaused})});if(!res.ok){const text=await res.text();throw new Error(text||'Pause toggle failed');}const data=await res.json();pause_state.paused=!!data.paused;renderPauseBtn();showPauseToast(pause_state.paused?'Paused for maintenance':'Resumed');}catch(err){showPauseToast(err.message||'Pause toggle failed',true);}}(async ()=>{funct)HTML" R"HTML(ion escapeHtml(str){if(!str)return'';const div=document.createElement('div');div.textContent=str;return div.innerHTML;}
funct)HTML" R"HTML(ion showToast(message,isError){const toast = document.getElementById('toast');toast.textContent = message;toast.style.background = isError ? '#dc2626':'#0284c7';toast.classList.add('show');setTimeout(()=> toast.classList.remove('show'),2500);}
funct)HTML" R"HTML(ion formatEpoch(epoch){if(!epoch)return '--';const date = new Date(epoch * 1000);if(isNaN(date.getTime()))return '--';return date.toLocaleString(undefined,{year:'numeric',month:'short',day:'numeric',hour:'numeric',minute:'2-digit'});}
funct)HTML" R"HTML(ion formatLevel(value,unit){if(typeof value !== 'number' || !isFinite(value)|| value < 0){return '--';}if(!unit||unit==='inches'){const feet = Math.floor(value / 12);const remainingInches = value % 12;if(feet === 0){return `${remainingInches.toFixed(1)}"`;}return `${feet}' ${remainingInches.toFixed(1)}"`;}return `${value.toFixed(2)} ${unit}`;}function getSensorUnit(clientUid,sensorIdx){const t=sensors.find(x=>x.client===clientUid&&x.sensorIndex===sensorIdx);if(t&&t.measurementUnit&&t.measurementUnit!=='inches')return t.measurementUnit;if(t&&t.objectType&&t.objectType!=='tank')return t.measurementUnit||'units';return 'inches';}let sensors = [];let calibrations = [];let calibrationLogs = [];async funct)HTML" R"HTML(ion loadSensors(){try{const response = await fetch('/api/sensors');if(!response.ok)throw new Error('Failed to load sensors');const data = await response.json();sensors = (data.sensors || []).map(t=>({client:t.c,sensorIndex:t.k,site:t.s,label:t.n,contents:t.cn||'',levelInches:t.l,sensorMa:t.ma||0,sensorType:t.st||'',objectType:t.ot||'tank',measurementUnit:t.mu||'inches',delta:t.d,previousEpoch:t.pe,alarmActive:t.a,alarmType:t.at,lastUpdate:t.u,userNumber:t.un||0}));populateSensorDropdowns();}catch(err){console.error('Error loading sensors:',err);showToast('Failed to load tank list',true);}}
funct)HTML" R"HTML(ion populateSensorDropdowns(){const sensorSelect = document.getElementById('sensorSelect');const logSensorFilter = document.getElementById('logSensorFilter');sensorSelect.innerHTML = '<option value="">-- Select a sensor --</option>';logSensorFilter.innerHTML = '<option value="">All Sensors</option>';const uniqueTanks = new Map();sensors.forEach(t =>{const key = `${t.client}:${t.sensorIndex}`;if(!uniqueTanks.has(key)){uniqueTanks.set(key,{client:t.client,sensorIndex:t.sensorIndex,site:t.site,label:t.label || `Sensor ${t.sensorIndex}`,heightInches:t.heightInches || 0,levelInches:t.levelInches || 0,sensorMa:t.sensorMa || 0,lastUpdate:t.lastUpdate || 0,objectType:t.objectType||'tank',measurementUnit:t.measurementUnit||'inches',userNumber:t.userNumber||0});}});uniqueTanks.forEach((tank,key)=>{const option = document.createElement('option');option.value = key;const typeTag=tank.objectType==='tank'?'':'['+tank.objectType.toUpperCase()+'] ';option.textContent = `${typeTag}${tank.site} - ${tank.label}${tank.userNumber?' #'+tank.userNumber:''}`;sensorSelect.appendChild(option.cloneNode(true));logSensorFilter.appendChild(option);});}function updateLevelInput(){const sel=document.getElementById('sensorSelect').value;if(!sel){document.getElementById('levelInputTank').style.display='flex';document.getElementById('levelInputGeneric').style.display='none';return;}const [uid,tn]=sel.split(':');const tank=sensors.find(t=>t.client===uid&&t.sensorIndex===parseInt(tn));const ot=tank?tank.objectType:'tank';const mu=tank?tank.measurementUnit:'inches';if(ot==='tank'||mu==='inches'){document.getElementById('levelInputTank').style.display='flex';document.getElementById('levelInputGeneric').style.display='none';}else{document.getElementById('levelInputTank').style.display='none';document.getElementById('levelInputGeneric').style.display='block';document.getElementById('levelUnitLabel').textContent=mu.toUpperCase();}}document.getElementById('sensorSelect').addEventListener('change',updateLevelInput);
async funct)HTML" R"HTML(ion loadCalibrationData(){try{const response = await fetch('/api/calibration');if(!response.ok)throw new Error('Failed to load calibration data');const data = await response.json();calibrations = data.calibrations || [];calibrationLogs = data.logs || [];updateCalibrationStats();updateCalibrationTable();updateCalibrationLog();}catch(err){console.error('Error loading calibration data:',err);}}
funct)HTML" R"HTML(ion updateCalibrationStats(){const total = sensors.length > 0 ? new Set(sensors.map(t => `${t.client}:${t.sensorIndex}`)).size:0;const calibrated = calibrations.filter(c => c.hasLearnedCalibration).length;const learning = calibrations.filter(c => !c.hasLearnedCalibration && c.entryCount > 0).length;const uncalibrated = total - calibrated - learning;document.getElementById('statTotalTanks').textContent = total;document.getElementById('statCalibrated').textContent = calibrated;document.getElementById('statLearning').textContent = learning;document.getElementById('statUncalibrated').textContent = Math.max(0,uncalibrated);}
funct)HTML" R"HTML(ion updateCalibrationTable(){const tbody = document.getElementById('calibrationTableBody');tbody.innerHTML = '';if(calibrations.length === 0){tbody.innerHTML = '<tr><td colspan="10" style="text-align:center;color:var(--muted);">No calibration data yet. Add readings to start learning.</td></tr>';return;}calibrations.forEach(cal =>{const tr = document.createElement('tr');const sensorInfo = sensors.find(t => t.client === cal.clientUid && t.sensorIndex === cal.sensorIndex);const sensorName = sensorInfo ? `${sensorInfo.label || 'Sensor ' + cal.sensorIndex}${sensorInfo.userNumber?' #'+sensorInfo.userNumber:''}`:`Sensor ${cal.sensorIndex}`;const site = sensorInfo ? sensorInfo.site:'--';let statusClass = 'uncalibrated';let statusText = 'Uncalibrated';let warnings = [];if(cal.hasLearnedCalibration){statusClass = 'calibrated';statusText = cal.hasTempCompensation ? 'Calibrated+Temp' : 'Calibrated';if(cal.rSquared < 0.95){warnings.push('Low R&sup2; fit (<95%)');}if(cal.entryCount === 2){warnings.push('Only 2 data points');}}else if(cal.entryCount > 0){statusClass = 'learning';statusText = 'Learning';if(cal.entryCount === 1){warnings.push('Need 1 more point');}}const sensorRange = cal.maxSensorMa - cal.minSensorMa;const levelRange = cal.maxLevelInches - cal.minLevelInches;if(cal.hasLearnedCalibration && sensorRange < 4){warnings.push('Narrow sensor range (<4mA)');}let driftText = '--';let driftClass = 'low';if(cal.hasLearnedCalibration && cal.originalMaxValue > 0){const originalSlope = cal.originalMaxValue / 16.0;const drift = Math.abs((cal.learnedSlope - originalSlope)/ originalSlope * 100);driftText = drift.toFixed(1)+ '%';if(drift > 10)driftClass = 'high';else if(drift > 5)driftClass = 'medium';}let tempCoefText = '--';if(cal.hasTempCompensation && cal.learnedTempCoef !== undefined){tempCoefText = cal.learnedTempCoef.toFixed(4) + ' '+getSensorUnit(cal.clientUid,cal.sensorIndex)+'/°F';}else if(cal.tempEntryCount > 0){tempCoefText = `(${cal.tempEntryCount} pts)`;}let rangeText = '--';if(cal.entryCount >= 1){rangeText = `${cal.minSensorMa.toFixed(1)}-${cal.maxSensorMa.toFixed(1)} mA`;}let warningHtml = '';if(warnings.length > 0){warningHtml = `<span class="quality-warning" title="${warnings.join(', ')}">&#x26A0;&#xFE0F;</span>`;}const sensorKey = `${cal.clientUid}:${cal.sensorIndex}`;tr.innerHTML = ` <td><a href="#" class="sensor-link" onclick="viewTankPoints('${sensorKey}');return false;" title="Click to view data points">${escapeHtml(sensorName)}</a>${warningHtml}</td><td>${escapeHtml(site)}</td><td><span class="calibration-status ${statusClass}">${statusText}</span></td><td title="Sensor range: ${rangeText}">${cal.entryCount}</td><td>${cal.hasLearnedCalibration ?(cal.rSquared * 100).toFixed(1)+ '%':'--'}</td><td>${cal.hasLearnedCalibration ? cal.learnedSlope.toFixed(3)+ ' '+getSensorUnit(cal.clientUid,cal.sensorIndex)+'/mA':'--'}</td><td title="Temperature coefficient (${getSensorUnit(cal.clientUid,cal.sensorIndex)} per °F deviation from 70°F)">${tempCoefText}</td><td><span class="drift-indicator ${driftClass}">${driftText}</span></td><td>${formatEpoch(cal.lastCalibrationEpoch)}</td><td><button class="btn-reset" onclick="resetCalibration('${cal.clientUid}',${cal.sensorIndex})" title="Reset calibration for this sensor">Reset</button></td> `;tbody.appendChild(tr);});}
funct)HTML" R"HTML(ion updateCalibrationLog(){const tbody = document.getElementById('logTableBody');const filter = document.getElementById('logSensorFilter').value;tbody.innerHTML = '';let filtered = calibrationLogs;if(filter){const [clientUid,sensorIdx] = filter.split(':');filtered = calibrationLogs.filter(log => log.clientUid === clientUid && log.sensorIndex === parseInt(sensorIdx));}if(filtered.length === 0){tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:var(--muted);">No calibration entries found.</td></tr>';return;}filtered.sort((a,b)=> b.timestamp - a.timestamp);filtered.forEach(log =>{const tr = document.createElement('tr');const sensorInfo = sensors.find(t => t.client === log.clientUid && t.sensorIndex === log.sensorIndex);const sensorName = sensorInfo ? `${sensorInfo.site} - ${sensorInfo.label || 'Sensor ' + log.sensorIndex}${sensorInfo.userNumber?' #'+sensorInfo.userNumber:''}`:`Sensor ${log.sensorIndex}`;const isValidReading = log.sensorReading >= 4 && log.sensorReading <= 20;const sensorDisplay = isValidReading ? log.sensorReading.toFixed(2)+ ' mA':(log.sensorReading ? `${log.sensorReading.toFixed(2)} mA (out of range)`:'-- (out of range)');const tempDisplay = log.temperatureF !== undefined && log.temperatureF !== null ? log.temperatureF.toFixed(1)+ '°F':'--';tr.innerHTML = ` <td>${formatEpoch(log.timestamp)}</td><td>${escapeHtml(sensorName)}</td><td title="${isValidReading ? '':'Not used for calibration(outside 4-20mA range)'}">${sensorDisplay}</td><td>${formatLevel(log.verifiedLevelInches,getSensorUnit(log.clientUid,log.sensorIndex))}</td><td>${tempDisplay}</td><td>${escapeHtml(log.notes || '--')}</td> `;if(!isValidReading){tr.style.opacity = '0.6';}tbody.appendChild(tr);});}document.getElementById('calibrationForm').addEventListener('submit',async(e)=>{e.preventDefault();const sensorKey = document.getElementById('sensorSelect').value;if(!sensorKey){showToast('Please select a sensor',true);return;}const [clientUid,sensorIndex] = sensorKey.split(':');const tank = sensors.find(t => `${t.client}:${t.sensorIndex}` === sensorKey);const isTankMode=document.getElementById('levelInputTank').style.display!=='none';let totalValue;if(isTankMode){const levelFeet = parseInt(document.getElementById('levelFeet').value)|| 0;const levelInches = parseFloat(document.getElementById('levelInches').value)|| 0;totalValue = levelFeet * 12 + levelInches;}else{totalValue = parseFloat(document.getElementById('levelValue').value)||0;}const timestampInput = document.getElementById('readingTimestamp').value;const note)HTML" R"HTML(s = document.getElementById('notes').value.trim();if(totalValue < 0){showToast('Invalid level value',true);return;}const payload ={clientUid:clientUid,sensorIndex:parseInt(sensorIndex),verifiedLevelInches:totalValue,notes:notes};if(tank && tank.sensorMa && tank.sensorMa >= 4 && tank.sensorMa <= 20){payload.sensorReading = tank.sensorMa;}if(timestampInput){payload.timestamp = Math.floor(new Date(timestampInput).getTime()/ 1000);}try{const response = await fetch('/api/calibration',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});if(!response.ok){const text = await response.text();throw new Error(text || 'Failed to submit calibration');}showToast('Calibration reading submitted successfully');document.getElementById('calibrationForm').reset();loadCalibrationData();}catch(err){console.error('Error submitting calibration:',err);showToast(err.message || 'Failed to submit calibration',true);}});document.getElementById('logSensorFilter').addEventListener('change',updateCalibrationLog);const now = new Date();now.setMinutes(now.getMinutes()- now.getTimezoneOffset());document.getElementById('readingTimestamp').value = now.toISOString().slice(0,16);await loadSensors();await loadCalibrationData();setInterval(loadCalibrationData,30000);funct)HTML" R"HTML(ion viewTankPoints(sensorKey){document.getElementById('logSensorFilter').value = sensorKey;updateCalibrationLog();document.getElementById('logTableBody').closest('.card').scrollIntoView({behavior:'smooth',block:'start'});showToast('Showing data points for selected tank');}async funct)HTML" R"HTML(ion resetCalibration(clientUid,sensorIndex){if(!confirm(`Reset calibration for sensor ${sensorIndex}? This will delete all calibration data for this sensor.`)){return;}try{const response = await fetch('/api/calibration',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({clientUid:clientUid,sensorIndex:sensorIndex})});if(!response.ok){const text = await response.text();throw new Error(text || 'Failed to reset calibration');}showToast('Calibration reset successfully');loadCalibrationData();}catch(err){console.error('Error resetting calibration:',err);showToast(err.message || 'Failed to reset calibration',true);}}})();})();
</script></body></html>)HTML";

static const char HISTORICAL_DATA_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Historical Data - Tank Alarm Server</title><script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Fleet Summary</h2><div class="stats-grid"><div class="stat-box"><div class="stat-value" id="statTotalTanks">0</div><div class="stat-label">Total Sensors</div></div><div class="stat-box"><div class="stat-value" id="statAlarmsToday">0</div><div class="stat-label">Alarms Today</div></div><div class="stat-box"><div class="stat-value" id="statAlarmsWeek">0</div><div class="stat-label">Alarms This Week</div></div></div><div id="dataSourceBanner" style="display:none;margin-top:12px;padding:8px 14px;border-radius:6px;font-size:0.85rem;background:#dbeafe;color:#1e40af;"></div></div><div class="card"><h2>Level Trends</h2><div class="controls"><select id="siteFilter"><option value="all")HTML" R"HTML(>All Sites</option></select><select id="sensorFilter"><option value="all">All Sensors</option></select><select id="rangeSelect"><option value="24h">Last 24 Hours</option><option value="7d">Last 7 Days</option><option value="30d" selected>Last 30 Days</option><option value="90d">Last 90 Days</option><option value="6mo">Last 6 Months</option><option value="1yr">Last Year</option><option value="2yr">Last 2 Years</option><option value="custom">Custom Range</option></select><input type="date" id="startDate" style="display:none;" onchange="renderLevelChart()"><input type="date" id="endDate" style="display:none;" onchange="renderLevelChart()"><button onclick="refreshData()">Refresh</button><button class="secondary" onclick="exportData()">Export CSV</button></div><div class="chart-container"><canvas id="levelChart"></canvas></div></div><div class="card"><h2>Alarm Frequency</h2><div class="chart-container"><canvas id="alarmChart"></canvas></div></div><div class="card"><h2>Sites &amp; Sensors</h2><p style="color:var(--muted);margin-bottom:16px;">Click on a site to expand and view individual tank details with mini sparklines.</p><div id="sitesContainer"></div></div><div class="card"><h2>VIN Voltage History</h2><p style="color:var(--muted);margin-bottom:16px;">Track power supply voltage trends to detect battery degradation or power issues early.</p><div class="chart-container"><canvas id="voltageChart"></canvas></div></div><div class="card" id="archivedCard" style="display:none"><h2>Archived Clients</h2><p style="color:var(--muted);margin-bottom:16px;">Previously removed clients archived to FTP. Click to load and view historical data.</p><div id="archivedList"></div><div class="chart-container" id="archivedChartWrap" style="display:none"><canvas id="archivedChart"></canvas></div></div></main><div id="toast"></div><script>(async () => {const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);/* PAUSE LOGIC */const pause_els={btn:document.getElementById('pauseBtn'),toast:document.getElementById('toast')};
const pause_state={paused:false};if(pause_els.btn)pause_els.btn.addEventListener('click',togglePauseFlow);if(pause_els.btn)pause_els.btn.addEventListener('mouseenter',()=>{if(pause_state.paused)pause_els.btn.textContent='Resume';});if(pause_els.btn)pause_els.btn.addEventListener('mouseleave',()=>{renderPauseBtn();});await fetch('/api/clients?summary=1').then(r=>r.json()).then(d=>{if(d&&d.srv){pause_state.paused=!!d.srv.ps;renderPauseBtn();}}).catch(e=>console.error('Failed to load pause state',e));
funct)HTML" R"HTML(ion showPauseToast(message,isError){if(!pause_els.toast)return;const t=pause_els.toast;t.textContent=message;t.style.background=isError?'#dc2626':'#0284c7';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500);}
funct)HTML" R"HTML(ion renderPauseBtn(){const btn=pause_els.btn;if(!btn)return;if(pause_state.paused){btn.classList.add('paused');btn.style.display='';btn.textContent='Unpause';btn.title='Resume data flow';}else{btn.classList.remove('paused');btn.style.display='none';}}
async funct)HTML" R"HTML(ion togglePauseFlow(){const targetPaused=!pause_state.paused;try{const res=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:targetPaused})});if(!res.ok){const text=await res.text();throw new Error(text||'Pause toggle failed');}const data=await res.json();pause_state.paused=!!data.paused;renderPauseBtn();showPauseToast(pause_state.paused?'Paused for maintenance':'Resumed');}catch(err){showPauseToast(err.message||'Pause toggle failed',true);}}(async ()=>{const CHART_COLORS=['#2563eb','#10b981','#f59e0b','#ef4444','#8b5cf6','#ec4899','#06b6d4','#84cc16'];function escapeHtml(s){if(!s)return'';const d=document.createElement('div');d.textContent=s;return d.innerHTML;}function escapeCsv(s){if(!s)return'';return String(s).replace(/"/g,'""');}let levelChart=null;let alarmChart=null;let voltageChart=null;let historicalData={sites:{},sensors:[],alarms:[],voltage:[]};funct)HTML" R"HTML(ion showToast(message,isError){const toast=document.getElementById('toast');toast.textContent=message;toast.style.background=isError?'#dc2626':'#0284c7';toast.classList.add('show');setTimeout(()=>toast.classList.remove('show'),2500);}
funct)HTML" R"HTML(ion formatLevel(inches){if(typeof inches!=='number'||!isFinite(inches))return'--';const feet=Math.floor(inches/12);const rem=inches%12;return feet>0?`${feet}' ${rem.toFixed(1)}"`:`${rem.toFixed(1)}"`;}
funct)HTML" R"HTML(ion formatEpoch(epoch){if(!epoch)return'--';const d=new Date(epoch*1000);return d.toLocaleString(undefined,{month:'short',day:'numeric',hour:'numeric',minute:'2-digit'});}
funct)HTML" R"HTML(ion initEmptyData(){historicalData={sites:{},sensors:[],alarms:[],voltage:[]};}
funct)HTML" R"HTML(ion updateStats(){const totalSensors=historicalData.sensors.length;const now=Date.now()/1000;const today=now-86400;const week=now-7*86400;const alarmsToday=historicalData.alarms.filter(a=>a.timestamp>today).length;const alarmsWeek=historicalData.alarms.filter(a=>a.timestamp>week).length;document.getElementById('statTotalTanks').textContent=totalSensors;document.getElementById('statAlarmsToday').textContent=alarmsToday;document.getElementById('statAlarmsWeek').textContent=alarmsWeek;}
funct)HTML" R"HTML(ion populateFilters(){const siteSelect=document.getElementById('siteFilter');const sensorSelect=document.getElementById('sensorFilter');siteSelect.innerHTML='<option value="all">All Sites</option>';Object.keys(historicalData.sites).sort().forEach(site=>{const opt=document.createElement('option');opt.value=site;opt.textContent=site;siteSelect.appendChild(opt);});sensorSelect.innerHTML='<option value="all">All Sensors</option>';historicalData.sensors.forEach((t,i)=>{const opt=document.createElement('option');opt.value=i;opt.textContent=`${t.site} - ${t.label}`;sensorSelect.appendChild(opt);});}
funct)HTML" R"HTML(ion getFilteredData(){const site=document.getElementById('siteFilter').value;const sensorIdx=document.getElementById('sensorFilter').value;const range=document.getElementById('rangeSelect').value;const now=Date.now()/1000;let cutoff=now-30*86400;let cutoffEnd=now;if(range==='24h')cutoff=now-86400;else if(range==='7d')cutoff=now-7*86400;else if(range==='90d')cutoff=now-90*86400;else if(range==='6mo')cutoff=now-180*86400;else if(range==='1yr')cutoff=now-365*86400;else if(range==='2yr')cutoff=now-730*86400;else if(range==='custom'){const sd=document.getElementById('startDate').value;const ed=document.getElementById('endDate').value;if(sd)cutoff=new Date(sd).getTime()/1000;if(ed)cutoffEnd=new Date(ed+'T23:59:59').getTime()/1000;}let sensors=historicalData.sensors;if(site!=='all')sensors=sensors.filter(t=>t.site===site);if(sensorIdx!=='all')sensors=[historicalData.sensors[parseInt(sensorIdx)]];return{sensors,cutoff,cutoffEnd};}
funct)HTML" R"HTML(ion renderLevelChart(){const ctx=document.getElementById('levelChart').getContext('2d');const{sensors,cutoff,cutoffEnd}=getFilteredData();if(levelChart)levelChart.destroy();const datasets=sensors.slice(0,8).map((tank,i)=>{const data=tank.readings.filter(r=>r.timestamp>=cutoff&&r.timestamp<=cutoffEnd).map(r=>({x:new Date(r.timestamp*1000),y:r.level}));return{label:`${tank.site} - ${tank.label}`,data,borderColor:CHART_COLORS[i%CHART_COLORS.length],backgroundColor:CHART_COLORS[i%CHART_COLORS.length]+'20',fill:false,tension:0.3,pointRadius:0};});levelChart=new Chart(ctx,{type:'line',data:{datasets},options:{responsive:true,maintainAspectRatio:false,interaction:{intersect:false,mode:'index'},scales:{x:{type:'time',time:{unit:'day',displayFormats:{hour:'MMM d, HH:mm',day:'MMM d'}},grid:{color:'var(--chart-grid)'}},y:{title:{display:true,text:'Level (inches)'},grid:{color:'var(--chart-grid)'}}},plugins:{legend:{position:'bottom'}}}});}
funct)HTML" R"HTML(ion renderAlarmChart(){const ctx=document.getElementById('alarmChart').getContext('2d');if(alarmChart)alarmChart.destroy();const sites=Object.keys(historicalData.sites);const highCounts=sites.map(s=>historicalData.alarms.filter(a=>a.site===s&&a.type==='HIGH').length);const lowCounts=sites.map(s=>historicalData.alarms.filter(a=>a.site===s&&a.type==='LOW').length);alarmChart=new Chart(ctx,{type:'bar',data:{labels:sites,datasets:[{label:'High Alarms',data:highCounts,backgroundColor:'#ef4444'},{label:'Low Alarms',data:lowCounts,backgroundColor:'#f59e0b'}]},options:{responsive:true,maintainAspectRatio:false,scales:{x:{stacked:true,grid:{display:false}},y:{stacked:true,beginAtZero:true,title:{display:true,text:'Alarm Count'}}},plugins:{legend:{position:'bottom'}}}});}
funct)HTML" R"HTML(ion renderVoltageChart(){const ctx=document.getElementById('voltageChart').getContext('2d');if(voltageChart)voltageChart.destroy();const data=historicalData.voltage.map(v=>({x:new Date(v.timestamp*1000),y:v.voltage}));if(!data.length){voltageChart=null;return;}var vals=data.map(d=>d.y);var vMin=Math.min(...vals);var vMax=Math.max(...vals);var yMin,yMax;if(vMax<=6){yMin=0;yMax=6;}else if(vMax<=15){yMin=10;yMax=15;}else{yMin=Math.floor(vMin-1);yMax=Math.ceil(vMax+1);}voltageChart=new Chart(ctx,{type:'line',data:{datasets:[{label:'VIN Voltage',data,borderColor:'#10b981',backgroundColor:'#10b98120',fill:true,tension:0.3,pointRadius:0}]},options:{responsive:true,maintainAspectRatio:false,scales:{x:{type:'time',time:{unit:'day'},grid:{color:'var(--chart-grid)'}},y:{min:yMin,max:yMax,title:{display:true,text:'Voltage (V)'},grid:{color:'var(--chart-grid)'}}},plugins:{legend:{display:false}}}});}
funct)HTML" R"HTML(ion createSparkline(container,data){const width=container.clientWidth||100;const height=40;const canvas=document.createElement('canvas');canvas.width=width;canvas.height=height;container.appendChild(canvas);const ctx=canvas.getContext('2d');if(!data||data.length<2)return;const values=data.map(d=>d.level);const min=Math.min(...values);const max=Math.max(...values);const range=max-min||1;ctx.strokeStyle='#2563eb';ctx.lineWidth=1.5;ctx.beginPath();data.forEach((d,i)=>{const x=i/(data.length-1)*width;const y=height-(d.level-min)/range*height;if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});ctx.stroke();}
funct)HTML" R"HTML(ion renderSites(){const container=document.getElementById('sitesContainer');container.innerHTML='';Object.keys(historicalData.sites).sort().forEach(siteName=>{const sensors=historicalData.sites[siteName];const alarmCount=historicalData.alarms.filter(a=>a.site===siteName).length;const siteCard=document.createElement('div');siteCard.className='site-card';siteCard.innerHTML=`<div class="site-header" onclick="this.nextElementSibling.classList.toggle('expanded')"><h3>${escapeHtml(siteName)}</h3><div class="site-stats"><span>${sensors.length} Tank${sensors.length!==1?'s':''}</span>${alarmCount>0?`<span class="alarm-badge">${alarmCount} Alarms</span>`:''}</div></div><div class="site-content"><div class="sensor-grid"></div></div>`;const grid=siteCard.querySelector('.sensor-grid');sensors.forEach(tank=>{const change=tank.change24h||0;const changeClass=change>=0?'positive':'negative';const changeSign=change>=0?'+':'';const tankCard=document.createElement('div');tankCard.className='sensor-card';tankCard.innerHTML=`<div class="tank-header"><div><div class="tank-name">${escapeHtml(tank.label)}</div><div class="sensor-level">${formatLevel(tank.currentLevel)}</div></div><div class="tank-change ${changeClass}">${changeSign}${change.toFixed(1)}" 24h</div></div><div class="sparkline"></div><div class="tank-footer"><span>Updated: ${formatEpoch(tank.readings[tank.readings.length-1]?.timestamp)}</span></div>`;grid.appendChild(tankCard);const sparklineEl=tankCard.querySelector('.sparkline');const last24h=tank.readings.slice(-24);setTimeout(()=>createSparkline(sparklineEl,last24h),0);});container.appendChild(siteCard);});}
async funct)HTML" R"HTML(ion loadHistoricalData(){const sensorIdx=document.getElementById('sensorFilter').value;const range=document.getElementById('rangeSelect').value;let apiDays=90;if(sensorIdx!=='all'){apiDays=0;}else{const rangeDays={'24h':7,'7d':7,'30d':30,'90d':90,'6mo':180,'1yr':365,'2yr':730};apiDays=rangeDays[range]||90;}const url=apiDays>0?'/api/history?days='+apiDays:'/api/history';try{const res=await fetch(url);if(!res.ok)throw new Error('Failed to load historical data');const data=await res.json();if(data.sensors&&data.sensors.length>0){historicalData=data;}else{initEmptyData();}if(data.dataInfo){const banner=document.getElementById('dataSourceBanner');if(banner){let msg='Data: RAM ('+data.settings.hotTierDays+'d)';if(data.dataInfo.warmTierAvailable)msg+=' + Flash ('+data.settings.warmTierMonths+'mo)';if(data.dataInfo.coldTierAvailable)msg+=' + FTP Archive';else msg+=' | Enable FTP for full archive';if(data.dataInfo.rangeNote)msg+=' — '+data.dataInfo.rangeNote;banner.textContent=msg;banner.style.display='';banner.style.background=data.dataInfo.coldTierAvailable?'#d1fae5':'#fef3c7';banner.style.color=data.dataInfo.coldTierAvailable?'#065f46':'#92400e';}}updateStats();populateFilters();renderLevelChart();renderAlarmChart();renderVoltageChart();renderSites();}catch(err){console.warn('No historical data available:',err.message);initEmptyData();updateStats();populateFilters();renderLevelChart();renderAlarmChart();renderVoltageChart();renderSites();}}
window.refreshData=funct)HTML" R"HTML(ion(){loadHistoricalData();showToast('Data refreshed');};window.exportData=funct)HTML" R"HTML(ion(){const{sensors,cutoff,cutoffEnd}=getFilteredData();let csv='Site,Tank,Timestamp,Level (inches)\n';sensors.forEach(tank=>{tank.readings.filter(r=>r.timestamp>=cutoff&&r.timestamp<=cutoffEnd).forEach(r=>{csv+=`"${escapeCsv(tank.site)}","${escapeCsv(tank.label)}",${new Date(r.timestamp*1000).toISOString()},${r.level.toFixed(2)}\n`;});});const blob=new Blob([csv],{type:'text/csv'});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download='tank_history.csv';a.click();URL.revokeObjectURL(url);showToast('CSV exported');};document.getElementById('siteFilter').addEventListener('change',renderLevelChart);document.getElementById('sensorFilter').addEventListener('change',function(){loadHistoricalData();});document.getElementById('rangeSelect').addEventListener('change',function(){const sd=document.getElementById('startDate');const ed=document.getElementById('endDate');if(this.value==='custom'){sd.style.display='';ed.style.display='';if(!sd.value){const d=new Date();d.setDate(d.getDate()-7);sd.value=d.toISOString().split('T')[0];}if(!ed.value){ed.value=new Date().toISOString().split('T')[0];}}else{sd.style.display='none';ed.style.display='none';}loadHistoricalData();});loadHistoricalData();/* ARCHIVED CLIENTS */let archivedChart=null;async function loadArchivedClients(){try{const res=await fetch('/api/history/archived');if(!res.ok)return;const data=await res.json();const archives=data.archives||[];if(!archives.length)return;const card=document.getElementById('archivedCard');card.style.display='';const list=document.getElementById('archivedList');list.innerHTML='';archives.forEach((a,idx)=>{const div=document.createElement('div');div.className='site-card';div.style.cursor='pointer';div.style.padding='12px 16px';div.style.marginBottom='8px';const d1=a.firstSeenEpoch?new Date(a.firstSeenEpoch*1000).toLocaleDateString(undefined,{month:'short',year:'numeric'}):'?';const d2=a.lastUpdateEpoch?new Date(a.lastUpdateEpoch*1000).toLocaleDateString(undefined,{month:'short',year:'numeric'}):'?';div.innerHTML='<div style="display:flex;justify-content:space-between;align-items:center"><div><strong>'+escHtml(a.displayLabel||a.site||a.clientUid)+'</strong><div style="font-size:0.85rem;color:var(--muted);margin-top:4px">'+a.sensorCount+' sensor'+(a.sensorCount!==1?'s':'')+' &bull; '+d1+' &mdash; '+d2+'</div></div><button class="pill secondary" style="font-size:0.8rem" data-idx="'+idx+'">Load Data</button></div>';div.querySelector('button').addEventListener('click',function(e){e.stopPropagation();loadArchivedData(a.ftpFile,a.displayLabel||a.site);});list.appendChild(div);});}catch(e){console.warn('Archived clients:',e.message);}}function escHtml(s){if(!s)return'';const d=document.createElement('div');d.textContent=s;return d.innerHTML;}async function loadArchivedData(ftpFile,label){try{showToast('Loading archived data from FTP...');const res=await fetch('/api/history/archived?file='+encodeURIComponent(ftpFile));if(!res.ok)throw new Error('Failed to load archive');const data=await res.json();const wrap=document.getElementById('archivedChartWrap');wrap.style.display='';const ctx=document.getElementById('archivedChart').getContext('2d');if(archivedChart)archivedChart.destroy();const datasets=[];const COLORS=['#2563eb','#10b981','#f59e0b','#ef4444','#8b5cf6','#ec4899'];if(data.history&&data.history.length){data.history.forEach((h,i)=>{const pts=(h.readings||[]).map(r=>({x:new Date(r[0]*1000),y:r[1]}));if(pts.length)datasets.push({label:(data.site||'')+' Sensor '+h.sensorIndex,data:pts,borderColor:COLORS[i%COLORS.length],backgroundColor:COLORS[i%COLORS.length]+'20',fill:false,tension:0.3,pointRadius:0});});}if(!datasets.length){wrap.style.display='none';showToast('No chart data in archive',true);return;}archivedChart=new Chart(ctx,{type:'line',data:{datasets},options:{responsive:true,maintainAspectRatio:false,interaction:{intersect:false,mode:'index'},scales:{x:{type:'time',time:{unit:'day'},grid:{color:'var(--chart-grid)'}},y:{title:{display:true,text:'Level (inches)'},grid:{color:'var(--chart-grid)'}}},plugins:{legend:{position:'bottom'}}}});showToast('Loaded: '+label);}catch(e){showToast(e.message||'Archive load failed',true);}}loadArchivedClients();})();})();
</script></body></html>)HTML";

static const char TRANSMISSION_LOG_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Transmission Log - Tank Alarm Server</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Outbound Transmission Log</h2><p style="color:var(--muted);margin-bottom:16px;">Log of messages sent from the server to clients and external services via Notehub. Shows the most recent 100 transmissions.</p><div class="controls"><button id="refreshBtn">Refresh</button><select id="typeFilter"><option value="all">All Types</option><option value="sms">SMS</option><option value="email">Email</option><option value="config">Config</option><option value="relay">Relay</option><option value="relay_clear">Relay Clear</option><option value="viewer_summary">Viewer Summary</option><option value="serial_request">Serial Request</option><option value="location_request">Location Request</option></select><select id="statusFilter"><option value="all">All Statuses</option><option value="outbox">Outbox</option><option value="sent">Sent</option><option value="failed">Failed</option><option value="cancelled">Cancelled</option></select><input type="text" id="searchInput" placeholder="Filter by site or client..." style="max-width:250px;"><button class="secondary" id="exportBtn">Export CSV</button><button class="secondary" id="cancelAllBtn" style="display:none;background:#fee2e2;color:#991b1b;border-color:#fca5a5;">Cancel All Pending</button></div><table><thead><tr><th>Date/Time</th><th>Site</th><th>Client ID</th><th>Type</th><th>Status</th><th>Detail</th><th style="width:80px;">Actions</th></tr></thead><tbody id="logBody"><tr><td colspan="7" class="empty-state">Loading...</td></tr></tbody></table></div></main><div id="toast"></div><script>(async ()=>{const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);)HTML"
R"HTML(const toast=document.getElementById('toast');const logBody=document.getElementById('logBody');const typeFilter=document.getElementById('typeFilter');const statusFilter=document.getElementById('statusFilter');const searchInput=document.getElementById('searchInput');let allEntries=[];function showToast(m,e){toast.textContent=m;toast.style.background=e?'#dc2626':'#0284c7';toast.classList.add('show');setTimeout(()=>toast.classList.remove('show'),2500);}function escapeHtml(s){if(!s)return'';const d=document.createElement('div');d.textContent=s;return d.innerHTML;}function formatEpoch(e){if(!e)return'--';const d=new Date(e*1000);if(isNaN(d.getTime()))return'--';return d.toLocaleString(undefined,{month:'numeric',day:'numeric',year:'numeric',hour:'numeric',minute:'2-digit',second:'2-digit',hour12:true});}function statusBadge(s){const colors={outbox:'#fef3c7;color:#92400e',sent:'#d1fae5;color:#065f46',failed:'#fee2e2;color:#991b1b',cancelled:'#e5e7eb;color:#6b7280'};const bg=colors[s]||'#e5e7eb;color:#374151';return `<span style="display:inline-block;padding:2px 8px;font-size:0.75rem;font-weight:600;background:${bg};border-radius:var(--radius);">${escapeHtml(s)}</span>`;})HTML"
R"HTML(function typeBadge(t){const colors={sms:'#dbeafe;color:#1e40af',email:'#fce7f3;color:#9d174d',config:'#e0e7ff;color:#3730a3',relay:'#fef3c7;color:#92400e',relay_clear:'#fef3c7;color:#92400e',viewer_summary:'#d1fae5;color:#065f46',serial_request:'#f3e8ff;color:#6b21a8',location_request:'#ccfbf1;color:#0f766e'};const bg=colors[t]||'#e5e7eb;color:#374151';return `<span style="display:inline-block;padding:2px 8px;font-size:0.75rem;font-weight:600;background:${bg};border-radius:var(--radius);">${escapeHtml(t)}</span>`;}function renderTable(){const tf=typeFilter.value;const sf=statusFilter.value;const search=searchInput.value.toLowerCase().trim();let filtered=allEntries;if(tf!=='all')filtered=filtered.filter(e=>e.type===tf);if(sf!=='all')filtered=filtered.filter(e=>e.status===sf);if(search)filtered=filtered.filter(e=>(e.site||'').toLowerCase().includes(search)||(e.client||'').toLowerCase().includes(search)||(e.detail||'').toLowerCase().includes(search));if(filtered.length===0){logBody.innerHTML='<tr><td colspan="7" class="empty-state">No matching transmissions found.</td></tr>';return;}logBody.innerHTML=filtered.map(e=>{const cancelBtn=(e.type==='config'&&e.status==='outbox'&&e.client)?`<button onclick="cancelPendingConfig('${escapeHtml(e.client)}')" style="padding:2px 8px;font-size:0.75rem;background:#fee2e2;color:#991b1b;border:1px solid #fca5a5;border-radius:var(--radius);cursor:pointer;">Cancel</button>`:'';return `<tr><td style="white-space:nowrap;">${formatEpoch(e.timestamp)}</td><td>${escapeHtml(e.site||'--')}</td><td style="font-size:0.85rem;font-family:ui-monospace,monospace;">${escapeHtml(e.client||'--')}</td><td>${typeBadge(e.type)}</td><td>${statusBadge(e.status)}</td><td style="font-size:0.85rem;">${escapeHtml(e.detail||'')}</td><td>${cancelBtn}</td></tr>`;}).join('');const cab=document.getElementById('cancelAllBtn');if(cab){const hasPending=allEntries.some(e=>e.type==='config'&&e.status==='outbox');cab.style.display=hasPending?'inline-block':'none';}}async function loadLog(){try{const res=await fetch('/api/transmission-log');if(!res.ok)throw new Error('Failed to load');const data=await res.json();allEntries=data.entries||[];renderTable();}catch(err){logBody.innerHTML='<tr><td colspan="7" class="empty-state">Error: '+escapeHtml(err.message)+'</td></tr>';}}document.getElementById('refreshBtn').addEventListener('click',loadLog);typeFilter.addEventListener('change',renderTable);statusFilter.addEventListener('change',renderTable);searchInput.addEventListener('input',renderTable);)HTML"
R"HTML(document.getElementById('exportBtn').addEventListener('click',()=>{let csv='Date/Time,Site,Client ID,Type,Status,Detail\n';const tf=typeFilter.value;const sf=statusFilter.value;const search=searchInput.value.toLowerCase().trim();let filtered=allEntries;if(tf!=='all')filtered=filtered.filter(e=>e.type===tf);if(sf!=='all')filtered=filtered.filter(e=>e.status===sf);if(search)filtered=filtered.filter(e=>(e.site||'').toLowerCase().includes(search)||(e.client||'').toLowerCase().includes(search));filtered.forEach(e=>{const dt=e.timestamp?new Date(e.timestamp*1000).toISOString():'';csv+=`"${dt}","${e.site||''}","${e.client||''}","${e.type||''}","${e.status||''}","${(e.detail||'').replace(/"/g,'""')}"\n`;});const blob=new Blob([csv],{type:'text/csv'});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download='transmission_log.csv';a.click();URL.revokeObjectURL(url);showToast('CSV exported');});document.getElementById('cancelAllBtn').addEventListener('click',cancelAllPendingConfigs);)HTML"
R"HTML(async function cancelPendingConfig(clientUid){if(!confirm('Cancel pending config dispatch for '+clientUid+'? This will stop auto-retries and purge unsent notes from the Notecard outbox.'))return;try{const res=await fetch('/api/config/cancel',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid})});const text=await res.text();if(res.ok){showToast(text||'Config dispatch cancelled');}else{showToast(text||'Cancel failed',true);}await loadLog();}catch(err){showToast('Error: '+err.message,true);}}async function cancelAllPendingConfigs(){if(!confirm('Cancel ALL pending config dispatches? This will stop all auto-retries.'))return;try{const res=await fetch('/api/config/cancel',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({})});const text=await res.text();if(res.ok){showToast(text||'All pending configs cancelled');}else{showToast(text||'Cancel failed',true);}await loadLog();}catch(err){showToast('Error: '+err.message,true);}}loadLog();setInterval(loadLog,15000);})();</script></body></html>)HTML";

static const char LOGIN_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>TankAlarm Login</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><div id="loading-overlay"><div class="spinner"></div></div><script>setTimeout(function(){var o=document.getElementById('loading-overlay');if(o)o.style.display='none'},5000)</script><main><h1 class="login-title"><span class="brand">TankAlarm</span> Login</h1><p>Enter your PIN to access the dashboard.</p><form id="loginForm" class="login-form"><input type="password" id="pin" class="pin-input" placeholder="Enter PIN" required autocomplete="current-password" pattern="\d{4}" title="4-digit PIN"><button type="submit" class="login-button">Login</button><div id="error" class="error">Invalid PIN</div></form></main><script>function sanitizeRedirect(value){if(!value||value.charAt(0)!=='/')return'/';if(value.startsWith('//'))return'/';if(value.toLowerCase().startsWith('/login'))return'/';return value;}window.addEventListener('load',()=>{localStorage.removeItem('tankalarm_session');const ov=document.getElementById('loading-overlay');if(ov){ov.style.display='none';ov.classList.add('hidden');}const params=new URLSearchParams(window.location.search);if(params.get('reason')==='expired'){const e=document.getElementById('error');e.textContent='Session expired \u2014 another user logged in';e.style.display='block';}});document.getElementById("loginForm").addEventListener("submit",async(e)=>{e.preventDefault();const pin=document.getElementById("pin").value;try{const res=await fetch("/api/login",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pin})});if(res.ok){const data=await res.json();localStorage.setItem("tankalarm_token","1");localStorage.setItem("tankalarm_session",data.session||"cookie");const params=new URLSearchParams(window.location.search);window.location.href=sanitizeRedirect(params.get("redirect"));}else{document.getElementById("error").textContent="Invalid PIN";document.getElementById("error").style.display="block";}}catch(err){document.getElementById("error").textContent="Connection failed: "+err.message;document.getElementById("error").style.display="block";}});</script></body></html>)HTML";

static const char CONTACTS_MANAGER_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Contacts Manager - Tank Alarm Server</title><link rel="stylesheet" href="/style.css"></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><h2>Contacts</h2><div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;"><div class="filter-section"><div class="filter-group"><label>Filter by View</label><select id="viewFilter"><option value="all">All Contacts</option><option value="site">By Site</option><option value="alarm">By Alarm</option></select></div><div class="filter-group" id="siteFilterGroup" style="display: none;"><label>Select Site</label><select id="siteSelect"><option value="">All Sites</option></select></div><div class="filter-group" id="alarmFilterGroup" style="display: none;"><label>Select Alarm</label><select id="alarmSelect"><option value="">All Alarms</option></select></div></div><)HTML" R"HTML(button class="btn btn-primary" onclick="openAddContactModal()">+ Add Contact</button></div><div id="contactsList" class="contact-list"><div class="empty-state">No contacts configured. Click "+ Add Contact" to get started.</div></div></div><div class="card"><h2>Daily Report Recipients</h2><p style="color: var(--muted); margin-bottom: 16px;">Contacts who receive the daily tank level summary email.</p><div class="daily-report-section"><div id="dailyReportList" class="daily-report-list"><div class="empty-state">No daily report recipients configured.</div></div><button class="btn btn-secondary" onclick="openAddDailyReportModal()">+ Add Recipient</button></div></div></main><div id="contactModal" class="modal hidden"><div class="modal-content"><div class="modal-header"><h2 id="modalTitle">Add Contact</h2><button class="modal-close" onclick="closeContactModal()">&times;</button></div><form id="contactForm"><div class="form-grid"><div class="form-field"><label>Name *</label><input type="text" id="contactName" required></div><div class="form-field"><label>Phone Number</label><input type="tel" id="contactPhone" placeholder="+15551234567"></div><div class="form-field"><label>Email Address</label><input type="email" id="contactEmail" placeholder="contact@example.com"></div></div><h3>Alarm Associations</h3><p style="color: var(--muted); font-size: 0.9rem; margin-bottom: 12px;">Select which alarms should trigger notifications to this contact.</p><div id="alarmAssociations" class="form-grid"></div><div style="display: flex; gap: 12px; justify-content: flex-end; margin-top: 24px;"><button type="button" class="btn btn-secondary" onclick="closeContactModal()">Cancel</button><button type="submit" class="btn btn-primary">Save Contact</button></div></form></div></div><div id="dailyReportModal" class="modal hidden"><div class="modal-content"><div class="modal-header"><h2>Add Daily Report Recipient</h2><button class="modal-close" onclick="closeDailyReportModal()">&times;</button></div><for)HTML" R"HTML(m id="dailyReportForm"><div class="form-grid"><div class="form-field"><label>Select Contact</label><select id="dailyReportContactSelect" required><option value="">Choose a contact...</option></select></div></div><div style="display: flex; gap: 12px; justify-content: flex-end; margin-top: 24px;"><button type="button" class="btn btn-secondary" onclick="closeDailyReportModal()">Cancel</button><button type="submit" class="btn btn-primary">Add Recipient</button></div></form></div></div><div id="toast"></div><script>(async ()=>{const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);/* PAUSE LOGIC */const pause_els={btn:document.getElementById('pauseBtn'),toast:document.getElementById('toast')};
const pause_state={paused:false};if(pause_els.btn)pause_els.btn.addEventListener('click',togglePauseFlow);if(pause_els.btn)pause_els.btn.addEventListener('mouseenter',()=>{if(pause_state.paused)pause_els.btn.textContent='Resume';});if(pause_els.btn)pause_els.btn.addEventListener('mouseleave',()=>{renderPauseBtn();});await fetch('/api/clients?summary=1').then(r=>r.json()).then(d=>{if(d&&d.srv){pause_state.paused=!!d.srv.ps;renderPauseBtn();}}).catch(e=>console.error('Failed to load pause state',e));
funct)HTML" R"HTML(ion showPauseToast(message,isError){if(!pause_els.toast)return;const t=pause_els.toast;t.textContent=message;t.style.background=isError?'#dc2626':'#0284c7';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500);}
funct)HTML" R"HTML(ion renderPauseBtn(){const btn=pause_els.btn;if(!btn)return;if(pause_state.paused){btn.classList.add('paused');btn.style.display='';btn.textContent='Unpause';btn.title='Resume data flow';}else{btn.classList.remove('paused');btn.style.display='none';}}
async funct)HTML" R"HTML(ion togglePauseFlow(){const targetPaused=!pause_state.paused;try{const res=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:targetPaused})});if(!res.ok){const text=await res.text();throw new Error(text||'Pause toggle failed');}const data=await res.json();pause_state.paused=!!data.paused;renderPauseBtn();showPauseToast(pause_state.paused?'Paused for maintenance':'Resumed');}catch(err){showPauseToast(err.message||'Pause toggle failed',true);}}let contacts = [];let dailyReportRecipients = [];let sites = [];let alarms = [];let editingContactId = null;funct)HTML" R"HTML(ion showToast(message){const toast = document.getElementById('toast');toast.textContent = message;toast.classList.add('show');setTimeout(()=>{toast.classList.remove('show');},3000);}
funct)HTML" R"HTML(ion loadData(){fetch('/api/contacts').then(response => response.json()).then(data =>{contacts = data.contacts || [];dailyReportRecipients = data.dailyReportRecipients || [];sites = data.sites || [];alarms = data.alarms || [];renderContacts();renderDailyReportRecipients();updateFilters();}).catch(err =>{console.error('Failed to load contacts:',err);showToast('Failed to load contacts data:' +(err && err.message ? err.message:err)+ '. Please check your network connection and try again.');});}
funct)HTML" R"HTML(ion saveData(){fetch('/api/contacts',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({contacts:contacts,dailyReportRecipients:dailyReportRecipients})}).then(response => response.json()).then(data =>{if(data.success){showToast('Changes saved successfully');loadData();}else{showToast('Failed to save changes:' +(data.error || 'Unknown error'));}}).catch(err =>{console.error('Failed to save contacts:',err);showToast('Failed to save changes:' +(err && err.message ? err.message:err));});}document.getElementById('viewFilter').addEventListener('change',(e)=>{const view = e.target.value;document.getElementById('siteFilterGroup').style.display = view === 'site' ? 'block':'none';document.getElementById('alarmFilterGroup').style.display = view === 'alarm' ? 'block':'none';renderContacts();});document.getElementById('siteSelect').addEventListener('change',()=> renderContacts());document.getElementById('alarmSelect').addEventListener('change',()=> renderContacts());
funct)HTML" R"HTML(ion updateFilters(){const siteSelect = document.getElementById('siteSelect');const alarmSelect = document.getElementById('alarmSelect');siteSelect.innerHTML = '<option value="">All Sites</option>';sites.forEach(site =>{const option = document.createElement('option');option.value = site;option.textContent = site;siteSelect.appendChild(option);});alarmSelect.innerHTML = '<option value="">All Alarms</option>';alarms.forEach(alarm =>{const option = document.createElement('option');option.value = alarm.id;option.textContent = `${alarm.site}- ${alarm.label}(${alarm.type})`;alarmSelect.appendChild(option);});}
funct)HTML" R"HTML(ion renderContacts(){const container = document.getElementById('contactsList');const viewFilter = document.getElementById('viewFilter').value;const siteFilter = document.getElementById('siteSelect').value;const alarmFilter = document.getElementById('alarmSelect').value;let filteredContacts = contacts;if(viewFilter === 'site' && siteFilter){filteredContacts = contacts.filter(c => c.alarmAssociations && c.alarmAssociations.some(a =>{const alarm = alarms.find(al => al.id === a);return alarm && alarm.site === siteFilter;}));}else if(viewFilter === 'alarm' && alarmFilter){filteredContacts = contacts.filter(c => c.alarmAssociations && c.alarmAssociations.includes(alarmFilter));}if(filteredContacts.length === 0){container.innerHTML = '<div class="empty-state">No contacts match the current filter.</div>';return;}container.innerHTML = filteredContacts.map(contact =>{const associatedAlarms =(contact.alarmAssociations || []).map(alarmId => alarms.find(a => a.id === alarmId)).filter(a => a);const groupedBySite ={};associatedAlarms.forEach(alarm =>{if(!groupedBySite[alarm.site]){groupedBySite[alarm.site] = [];}groupedBySite[alarm.site].push(alarm);});return ` <div class="contact-card"><div class="contact-header"><div class="contact-info"><div class="contact-name">${escapeHtml(contact.name)}</div><div class="contact-details"> ${contact.phone ? `<div>Phone: ${escapeHtml(contact.phone)}</div>`:''}${contact.email ? `<div>Email: ${escapeHtml(contact.email)}</div>`:''}</div></div><div class="contact-actions"><button class="btn btn-small btn-secondary" data-contact-id="${escapeHtml(contact.id)}" data-action="edit">Edit</button><button class="btn btn-small btn-danger" data-contact-id="${escapeHtml(contact.id)}" data-action="delete">Delete</button></div></div> ${associatedAlarms.length > 0 ? ` <div class="associations"> ${Object.keys(groupedBySite).map(site => ` <div class="association-section"><h4>${escapeHtml(site)}</h4><div class="association-list"> ${groupedBySite[site)HTML" R"HTML(].map(alarm => ` <div class="association-tag"> ${escapeHtml(alarm.label)}(${escapeHtml(alarm.type)})<button class="remove-tag" data-contact-id="${escapeHtml(contact.id)}" data-alarm-id="${escapeHtml(alarm.id)}">&times;</button></div> `).join('')}</div></div> `).join('')}</div> `:''}</div> `;}).join('');container.querySelectorAll('[data-action="edit"]').forEach(btn =>{btn.addEventListener('click',()=> editContact(btn.dataset.contactId));});container.querySelectorAll('[data-action="delete"]').forEach(btn =>{btn.addEventListener('click',()=> deleteContact(btn.dataset.contactId));});container.querySelectorAll('.remove-tag').forEach(btn =>{btn.addEventListener('click',funct)HTML" R"HTML(ion(){removeAlarmAssociation(this.dataset.contactId,this.dataset.alarmId);});});}
funct)HTML" R"HTML(ion renderDailyReportRecipients(){const container = document.getElementById('dailyReportList');if(dailyReportRecipients.length === 0){container.innerHTML = '<div class="empty-state">No daily report recipients configured.</div>';return;}container.innerHTML = dailyReportRecipients.map(recipientId =>{const contact = contacts.find(c => c.id === recipientId);if(!contact)return '';return ` <div class="daily-report-item"><div><strong>${escapeHtml(contact.name)}</strong> ${contact.email ? ` - ${escapeHtml(contact.email)}`:''}</div><button class="btn btn-small btn-danger" data-recipient-id="${escapeHtml(recipientId)}" data-action="remove-recipient">Remove</button></div> `;}).filter(Boolean).join('');container.querySelectorAll('[data-action="remove-recipient"]').forEach(btn =>{btn.addEventListener('click',()=> removeDailyReportRecipient(btn.dataset.recipientId));});}
window.openAddContactModal = funct)HTML" R"HTML(ion(){editingContactId = null;document.getElementById('modalTitle').textContent = 'Add Contact';document.getElementById('contactName').value = '';document.getElementById('contactPhone').value = '';document.getElementById('contactEmail').value = '';renderAlarmAssociations([]);document.getElementById('contactModal').classList.remove('hidden');};window.editContact = funct)HTML" R"HTML(ion(contactId){const contact = contacts.find(c => c.id === contactId);if(!contact)return;editingContactId = contactId;document.getElementById('modalTitle').textContent = 'Edit Contact';document.getElementById('contactName').value = contact.name;document.getElementById('contactPhone').value = contact.phone || '';document.getElementById('contactEmail').value = contact.email || '';renderAlarmAssociations(contact.alarmAssociations || []);document.getElementById('contactModal').classList.remove('hidden');};window.closeContactModal = funct)HTML" R"HTML(ion(){document.getElementById('contactModal').classList.add('hidden');};funct)HTML" R"HTML(ion renderAlarmAssociations(selectedAlarms){const container = document.getElementById('alarmAssociations');if(alarms.length === 0){container.innerHTML = '<p style="color:var(--muted);font-style:italic;">No alarms configured in the system.</p>';return;}const groupedBySite ={};alarms.forEach(alarm =>{if(!groupedBySite[alarm.site]){groupedBySite[alarm.site] = [];}groupedBySite[alarm.site].push(alarm);});container.innerHTML = Object.keys(groupedBySite).map(site => ` <div style="grid-column:1 / -1;"><strong style="display:block;margin-bottom:8px;">${escapeHtml(site)}</strong> ${groupedBySite[site].map((alarm,idx)=>{const checkboxId = 'alarm_' + escapeHtml(alarm.id)+ '_' + idx;return ` <label for="${checkboxId}" style="display:flex;align-items:center;gap:8px;margin-bottom:6px;"><input type="checkbox" id="${checkboxId}" name="alarmAssoc" value="${escapeHtml(alarm.id)}" ${selectedAlarms.includes(alarm.id)? 'checked':''}><span>${escapeHtml(alarm.label)}(${escapeHtml(alarm.ty)HTML" R"HTML(pe)})</span></label> `;}).join('')}</div> `).join('');}document.getElementById('contactForm').addEventListener('submit',(e)=>{e.preventDefault();const name = document.getElementById('contactName').value.trim();const phone = document.getElementById('contactPhone').value.trim();const email = document.getElementById('contactEmail').value.trim();if(!name){showToast('Contact name is required');return;}if(!phone && !email){showToast('Either phone or email is required');return;}const alarmAssociations = Array.from(document.querySelectorAll('input[name="alarmAssoc"]:checked')).map(cb => cb.value);if(editingContactId){const contact = contacts.find(c => c.id === editingContactId);if(contact){contact.name = name;contact.phone = phone;contact.email = email;contact.alarmAssociations = alarmAssociations;}}else{const newContact ={id:'contact_' + Date.now()+ '_' + Math.random().toString(36).substr(2,9),name:name,phone:phone,email:email,alarmAssociations:alarmAssociations};contacts.push(newContact);}saveData();closeContactModal();});window.deleteContact = funct)HTML" R"HTML(ion(contactId){if(!confirm('Are you sure you want to delete this contact?'))return;contacts = contacts.filter(c => c.id !== contactId);dailyReportRecipients = dailyReportRecipients.filter(r => r !== contactId);saveData();};window.removeAlarmAssociation = funct)HTML" R"HTML(ion(contactId,alarmId){const contact = contacts.find(c => c.id === contactId);if(!contact)return;contact.alarmAssociations =(contact.alarmAssociations || []).filter(a => a !== alarmId);saveData();};window.openAddDailyReportModal = funct)HTML" R"HTML(ion(){const select = document.getElementById('dailyReportContactSelect');select.innerHTML = '<option value="">Choose a contact...</option>';contacts.forEach(contact =>{if(contact.email && !dailyReportRecipients.includes(contact.id)){const option = document.createElement('option');option.value = contact.id;option.textContent = `${contact.name}(${contact.email})`;select.appendChild(option);}});if(select.options.length === 1){showToast('No con)HTML" R"HTML(tacts with email addresses available');return;}document.getElementById('dailyReportModal').classList.remove('hidden');};window.closeDailyReportModal = funct)HTML" R"HTML(ion(){document.getElementById('dailyReportModal').classList.add('hidden');};document.getElementById('dailyReportForm').addEventListener('submit',(e)=>{e.preventDefault();const contactId = document.getElementById('dailyReportContactSelect').value;if(!contactId){showToast('Please select a contact');return;}if(!dailyReportRecipients.includes(contactId)){dailyReportRecipients.push(contactId);saveData();loadData();}closeDailyReportModal();});window.removeDailyReportRecipient = funct)HTML" R"HTML(ion(recipientId){dailyReportRecipients = dailyReportRecipients.filter(r => r !== recipientId);saveData();};funct)HTML" R"HTML(ion escapeHtml(text){const div = document.createElement('div');div.textContent = text;return div.innerHTML;}loadData();})();
</script></body></html>)HTML";

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Tank Alarm Server</title><link rel="stylesheet" href="/style.css"><style>
.stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin-bottom:20px;}
.stat-card{background:var(--card-bg);border:1px solid var(--card-border);border-radius:8px;padding:14px;text-align:center;}
.stat-card span{font-size:0.8rem;color:var(--muted);}
.stat-card strong{display:block;font-size:1.6rem;margin-top:4px;}
.site-section{background:var(--card-bg);border:1px solid var(--card-border);border-radius:10px;margin-bottom:16px;overflow:hidden;}
.site-head{display:flex;justify-content:space-between;align-items:center;padding:14px 18px;border-bottom:1px solid var(--card-border);flex-wrap:wrap;gap:8px;}
.site-name{font-size:1.15rem;font-weight:700;}
.site-actions{display:flex;align-items:center;gap:8px;flex-wrap:wrap;}
.client-dots{display:flex;gap:6px;align-items:center;}
.cdot{width:12px;height:12px;border-radius:50%;cursor:pointer;border:2px solid transparent;transition:transform 0.15s;}
.cdot:hover{transform:scale(1.4);}
.cdot.green{background:#10b981;}
.cdot.yellow{background:#f59e0b;}
.cdot.red{background:#ef4444;}
.cdot-info{display:none;background:var(--bg);border:1px solid var(--card-border);border-radius:6px;padding:10px 14px;margin:0 18px 8px;font-size:0.85rem;animation:slideDown 0.15s ease-out;}
.cdot-info.open{display:block;}
@keyframes slideDown{from{opacity:0;transform:translateY(-6px);}to{opacity:1;transform:translateY(0);}}
.cdot-info code{font-size:0.8rem;}
.data-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px;padding:14px 18px;}
.data-card{background:var(--bg);border:1px solid var(--card-border);border-radius:8px;padding:14px;position:relative;}
.data-card.alarm-card{border-left:4px solid var(--danger);}
.dc-type{font-size:0.7rem;text-transform:uppercase;font-weight:600;color:var(--muted);letter-spacing:0.5px;margin-bottom:6px;}
.dc-name{font-weight:600;font-size:0.95rem;margin-bottom:2px;}
.dc-value{font-size:1.5rem;font-weight:700;line-height:1.2;}
.dc-value small{font-size:0.55em;font-weight:400;color:var(--muted);}
.dc-meta{font-size:0.8rem;color:var(--muted);margin-top:6px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:4px;}
.dc-change{font-size:0.8rem;font-weight:500;}
.dc-change.pos{color:#10b981;}
.dc-change.neg{color:#ef4444;}
.dc-alarm{font-size:0.8rem;font-weight:600;color:var(--danger);margin-top:4px;}
.dc-actions{margin-top:8px;display:flex;gap:6px;}
.dc-contents{font-size:0.78rem;color:var(--muted);font-style:italic;}
.status-pill{font-size:0.75rem;padding:2px 8px;border-radius:10px;font-weight:600;}
.status-pill.ok{background:#dcfce7;color:#166534;}
.status-pill.alarm{background:#fee2e2;color:#991b1b;}
.status-pill.stale{background:#fef3c7;color:#92400e;}
.no-data{padding:40px 18px;text-align:center;color:var(--muted);}
.dc-sparkline{margin-top:6px;height:36px;width:100%;position:relative;}
.dc-sparkline canvas{display:block;width:100%;height:100%;border-radius:4px;background:color-mix(in srgb,var(--accent) 5%,transparent);}
.dc-sparkline .spark-label{position:absolute;top:2px;right:4px;font-size:0.6rem;color:var(--muted);pointer-events:none;}
</style></head><body data-theme="light"><div id="loading-overlay"><div class="spinner"></div></div><script>setTimeout(function(){var o=document.getElementById('loading-overlay');if(o)o.style.display='none'},5000)</script><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><button class="pause-btn" id="pauseBtn" aria-label="Resume data flow" style="display:none">Unpause</button><a class="pill" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="logout()">Logout</button></div></div></header><div id="ghUpdateBanner" style="display:none;background:#fef9c3;border-bottom:2px solid #ca8a04;padding:10px 20px;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;"><div style="display:flex;flex-direction:column;gap:4px;"><span id="ghUpdateServerLine" style="display:none;">&#x26A0; Server firmware update on GitHub: <strong id="ghUpdateVersion"></strong> &mdash; <a id="ghUpdateLink" href="#" target="_blank" rel="noopener noreferrer">View release notes</a></span><span id="dfuUpdateServerLine" style="display:none;">&#x26A0; Server firmware update pending in Notehub DFU &mdash; <a href="/server-settings">Apply in Settings</a></span><span id="ghUpdateClientLine" style="display:none;">&#x1F4E1; <strong id="ghUpdateClientCount"></strong> client(s) running outdated firmware &mdash; upload v<span id="ghUpdateLatestVer"></span> to Blues Notehub to auto-update</span></div><button onclick="this.parentElement.style.display='none'" style="background:none;border:none;cursor:pointer;font-size:1.2rem;line-height:1;padding:0 4px;">&times;</button></div><main><div class="stats-grid"><div class="stat-card"><span>Total Clients</span><strong id="statClients">0</strong></div><div class="stat-card"><span>Active Sensors</span><strong id="statTanks">0</strong></div><div class="stat-card"><span>Active Alarms</span><strong id="statAlarms">0</strong></div><div class="stat-card"><span>Stale (&gt;25h)</span><strong id="statStale">0</strong></div></div><section><div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;"><h2 style="margin:0;">Fleet Telemetry</h2><span style="font-size:0.85rem;color:var(--muted);">Auto-refreshes while this page is open</span></div><div id="siteContainer"><div class="no-data">Connecting to server...</div></div></section><section class="card"><h2 style="margin:0 0 0.5rem;">Historical Data</h2><p style="color:var(--muted);margin-bottom:12px;">Review long-term trends, alarms, and site summaries.</p><div class="actions"><a class="pill" href="/historical">Open Historical Data</a></div></section></main><div id="toast"></div>)HTML" R"HTML(<script>
(()=>{setTimeout(()=>{const o=document.getElementById('loading-overlay');if(o)o.style.display='none';},8000);window.addEventListener('error',()=>{const o=document.getElementById('loading-overlay');if(o)o.style.display='none';});const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);
const STALE_MIN=2940;const DEFAULT_REFRESH_S=60;
const els={pauseBtn:document.getElementById('pauseBtn'),siteContainer:document.getElementById('siteContainer'),statClients:document.getElementById('statClients'),statTanks:document.getElementById('statTanks'),statAlarms:document.getElementById('statAlarms'),statStale:document.getElementById('statStale'),toast:document.getElementById('toast')};
const state={clients:[],sites:{},refreshing:false,timer:null,uiRefreshS:DEFAULT_REFRESH_S,paused:false,expandedDot:null,sparkData:{}};
function escapeHtml(u){if(!u)return'';return String(u).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#039;');}
function formatNum(v){return(typeof v==='number'&&isFinite(v))?v.toFixed(1):'--';}
function formatLevel(inches){if(typeof inches!=='number'||!isFinite(inches)||inches<=0)return'--';const ft=Math.floor(inches/12);const rem=inches%12;return ft>0?`${ft}' ${rem.toFixed(1)}"`:`${rem.toFixed(1)}"`;}
function formatEpoch(e){if(!e)return'--';const d=new Date(e*1000);if(isNaN(d.getTime()))return'--';return d.toLocaleString(undefined,{month:'numeric',day:'numeric',hour:'numeric',minute:'2-digit',hour12:true});}
function formatVoltage(v){return(typeof v==='number'&&isFinite(v)&&v>0)?v.toFixed(2)+' V':'--';}
function timeAgo(epoch){if(!epoch)return'never';const s=Math.max(0,Date.now()/1000-epoch);if(s<120)return'just now';if(s<3600)return Math.floor(s/60)+'m ago';if(s<86400)return Math.floor(s/3600)+'h ago';return Math.floor(s/86400)+'d ago';}
function isStale(epoch){return!epoch||(epoch*1000)<(Date.now()-STALE_MIN*60*1000);}
function hideLoading(){const ov=document.getElementById('loading-overlay');if(ov){ov.style.display='none';ov.classList.add('hidden');}}
function showToast(m,err){if(els.toast){els.toast.textContent=m;els.toast.style.background=err?'#dc2626':'#0284c7';els.toast.classList.add('show');setTimeout(()=>els.toast.classList.remove('show'),2500);}}
)HTML" R"HTML(
function objectTypeLabel(ot){const map={tank:'Tank Level',gas:'Gas Pressure',rpm:'RPM',flow:'Flow Rate',engine:'Engine',pump:'Pump'};return map[ot]||'Sensor';}
function objectTypeIcon(ot){const map={tank:'\u2BEA',gas:'\u26A1',rpm:'\u2699',flow:'\u{1F4A7}',engine:'\u2699',pump:'\u2699'};return map[ot]||'\u{1F4CA}';}
function unitLabel(mu,ot){const abr={inches:'in',psi:'psi',rpm:'rpm',gpm:'gpm'};if(mu){const k=mu.toLowerCase();return abr[k]||mu;}if(ot==='gas')return'psi';if(ot==='rpm')return'rpm';if(ot==='flow')return'gpm';return'in';}
function formatValue(val,mu,ot){if(typeof val!=='number'||!isFinite(val))return'--';if(!mu&&(!ot||ot==='tank'))return formatLevel(val);return val.toFixed(1);}
const SPARKLINE_TYPES=new Set(['tank','gas','flow']);
function drawSparkline(container,readings,color){
if(!readings||readings.length<2){container.innerHTML='<span class="spark-label">No trend data</span>';return;}
const width=container.clientWidth||200;const height=36;
const canvas=document.createElement('canvas');canvas.width=width*2;canvas.height=height*2;canvas.style.width=width+'px';canvas.style.height=height+'px';
container.appendChild(canvas);
const ctx=canvas.getContext('2d');ctx.scale(2,2);
const vals=readings.map(r=>r.level);const min=Math.min(...vals);const max=Math.max(...vals);const range=max-min||1;
const pad=2;const drawH=height-pad*2;const drawW=width-pad*2;
ctx.strokeStyle=color||'#2563eb';ctx.lineWidth=1.5;ctx.lineJoin='round';ctx.lineCap='round';ctx.beginPath();
vals.forEach((v,i)=>{const x=pad+i/(vals.length-1)*drawW;const y=pad+drawH-(v-min)/range*drawH;if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});
ctx.stroke();
const grad=ctx.createLinearGradient(0,pad,0,height);grad.addColorStop(0,(color||'#2563eb')+'30');grad.addColorStop(1,(color||'#2563eb')+'05');ctx.lineTo(pad+drawW,height);ctx.lineTo(pad,height);ctx.closePath();ctx.fillStyle=grad;ctx.fill();
const label=document.createElement('span');label.className='spark-label';const spanSec=readings.length>=2?(readings[readings.length-1].timestamp-readings[0].timestamp):0;const spanDays=Math.round(spanSec/86400);let spanText=spanDays>=60?Math.round(spanDays/30)+'mo':spanDays>=2?spanDays+'d':'<1d';label.textContent=readings.length+'pts / '+spanText;container.appendChild(label);}
async function loadSparklineData(){
try{const ac=new AbortController();const tid=setTimeout(()=>ac.abort(),10000);const res=await fetch('/api/history?days=90',{signal:ac.signal});clearTimeout(tid);if(!res.ok)return;const data=await res.json();
if(!data.sensors||!data.sensors.length)return;
const map={};data.sensors.forEach(t=>{const key=(t.client||'')+'|'+(t.sensorIndex||1);map[key]=t.readings||[];});
state.sparkData=map;renderSparklines();}catch(e){console.warn('Sparkline data unavailable:',e.message);}}
function renderSparklines(){
document.querySelectorAll('.dc-sparkline[data-spark-key]').forEach(el=>{
const key=el.dataset.sparkKey;const readings=state.sparkData[key];if(readings&&readings.length>=2){el.innerHTML='';drawSparkline(el,readings,el.dataset.sparkColor);}});}
function buildSiteModel(rawClients){const sites={};rawClients.forEach(c=>{const uid=c.c||'';const site=c.s||'Unknown Site';if(!sites[site])sites[site]={name:site,clients:{}};if(!sites[site].clients[uid])sites[site].clients[uid]={uid:uid,alarm:!!c.a,lastUpdate:c.u||0,vinVoltage:c.v,vinVoltageEpoch:c.ve,sensors:[]};const cl=sites[site].clients[uid];if(c.a)cl.alarm=true;if(c.u>cl.lastUpdate)cl.lastUpdate=c.u;const sensors=Array.isArray(c.ts)?c.ts:[];if(sensors.length){sensors.forEach((t,idx)=>{cl.sensors.push({label:t.n||c.n||'Sensor',sensorIndex:t.k||'',userNumber:t.un||0,levelInches:t.l,sensorMa:t.ma,sensorType:t.st||'',objectType:t.ot||'tank',measurementUnit:t.mu||'',contents:t.ct||'',alarm:!!t.a,alarmType:t.at||'',lastUpdate:t.u||0,change24h:t.d,sensorIdx:idx,_clientUid:uid});});}else if(c.u){cl.sensors.push({label:c.n||'Sensor',sensorIndex:c.k||'',userNumber:c.un||0,levelInches:c.l,sensorMa:c.ma,sensorType:'',objectType:'tank',measurementUnit:'',contents:'',alarm:!!c.a,alarmType:c.at||'',lastUpdate:c.u||0,change24h:undefined,sensorIdx:0,_clientUid:uid});}});return sites;}
)HTML" R"HTML(
function clientStatusColor(cl){if(cl.alarm)return'red';if(!cl.lastUpdate||isStale(cl.lastUpdate))return'yellow';return'green';}
function renderSites(){const container=els.siteContainer;container.innerHTML='';const siteNames=Object.keys(state.sites).sort();if(!siteNames.length){container.innerHTML='<div class="no-data">No telemetry available yet.</div>';return;}
siteNames.forEach(siteName=>{const site=state.sites[siteName];const section=document.createElement('div');section.className='site-section';const clientList=Object.values(site.clients);const hasAlarm=clientList.some(c=>c.alarm);const head=document.createElement('div');head.className='site-head';if(hasAlarm)head.style.borderLeft='4px solid var(--danger)';const left=document.createElement('div');left.innerHTML=`<div class="site-name">${escapeHtml(siteName)}</div>`;const right=document.createElement('div');right.className='site-actions';const dots=document.createElement('div');dots.className='client-dots';dots.title='Client status indicators - click to expand';clientList.forEach(cl=>{const dot=document.createElement('span');dot.className='cdot '+clientStatusColor(cl);dot.title=cl.uid;dot.dataset.uid=cl.uid;dot.dataset.site=siteName;dot.addEventListener('click',()=>toggleDotInfo(section,cl,siteName));dots.appendChild(dot);});right.appendChild(dots);const manageBtn=document.createElement('a');manageBtn.className='pill secondary';manageBtn.style.cssText='font-size:0.75rem;padding:3px 10px;';manageBtn.href='/site-config?site='+encodeURIComponent(siteName);manageBtn.textContent='Manage';right.appendChild(manageBtn);head.appendChild(left);head.appendChild(right);section.appendChild(head);const infoSlot=document.createElement('div');infoSlot.className='cdot-info-slot';section.appendChild(infoSlot);const grid=document.createElement('div');grid.className='data-grid';const allSensors=[];clientList.forEach(cl=>{cl.sensors.forEach(t=>{t._clientUid=cl.uid;t._vinVoltage=cl.vinVoltage;allSensors.push(t);});if(!cl.sensors.length){allSensors.push({label:'Configured Client',sensorIndex:'',objectType:'pending',lastUpdate:0,_clientUid:cl.uid,_vinVoltage:cl.vinVoltage,alarm:false});}});const grouped={};allSensors.forEach(t=>{const ot=t.objectType||'tank';if(!grouped[ot])grouped[ot]=[];grouped[ot].push(t);});const typeOrder=['tank','gas','rpm','flow','engine','pump','pending'];typeOrder.forEach(ot=>{if(!grouped[ot])return;grouped[ot].forEach(t=>{grid.appendChild(renderDataCard(t,siteName));});});Object.keys(grouped).filter(ot=>!typeOrder.includes(ot)).forEach(ot=>{grouped[ot].forEach(t=>{grid.appendChild(renderDataCard(t,siteName));});});section.appendChild(grid);container.appendChild(section);});}
)HTML" R"HTML(
function renderDataCard(t,siteName){const card=document.createElement('div');card.className='data-card';if(t.alarm)card.classList.add('alarm-card');if(t.objectType==='pending'){card.style.opacity='0.6';card.innerHTML=`<div class="dc-type">AWAITING DATA</div><div class="dc-name" style="font-size:0.9rem;">Configured Client</div><div style="font-size:0.85rem;color:var(--muted);margin-top:6px;"><code style="font-size:0.8rem;">${escapeHtml(t._clientUid)}</code></div><div class="dc-meta"><span>Awaiting first report</span><a class="pill secondary" style="font-size:0.7rem;padding:2px 8px;" href="/config-generator?uid=${encodeURIComponent(t._clientUid)}">Edit Config</a></div>`;return card;}
const ot=t.objectType||'tank';const mu=unitLabel(t.measurementUnit,ot);const val=formatValue(t.levelInches,t.measurementUnit,ot);const stale=isStale(t.lastUpdate);let changeHtml='';if(typeof t.change24h==='number'){const cls=t.change24h>=0?'pos':'neg';const sign=t.change24h>=0?'+':'';changeHtml=`<span class="dc-change ${cls}">${sign}${t.change24h.toFixed(1)} ${mu}/24h</span>`;}
let alarmHtml='';if(t.alarm)alarmHtml=`<div class="dc-alarm">ALARM: ${escapeHtml(t.alarmType)}</div>`;
let contentsHtml='';if(t.contents)contentsHtml=`<div class="dc-contents">${escapeHtml(t.contents)}</div>`;
const staleBadge=stale&&t.lastUpdate?`<span class="status-pill stale">Stale</span>`:'';
let sparkHtml='';if(SPARKLINE_TYPES.has(ot)){const sparkKey=escapeHtml(t._clientUid)+'|'+(t.sensorIndex||1);const sparkColor=ot==='gas'?'#f59e0b':ot==='flow'?'#10b981':'#2563eb';sparkHtml=`<div class="dc-sparkline" data-spark-key="${sparkKey}" data-spark-color="${sparkColor}"><span class="spark-label">Loading trend...</span></div>`;}
card.innerHTML=`<div class="dc-type">${objectTypeLabel(ot)} ${staleBadge}</div><div class="dc-name">${escapeHtml(t.label||'Sensor')}${t.userNumber?' #'+t.userNumber:''}</div>${contentsHtml}<div class="dc-value">${val} <small>${escapeHtml(mu)}</small></div>${sparkHtml}${changeHtml}${alarmHtml}<div class="dc-meta"><span>${timeAgo(t.lastUpdate)}</span>${t._vinVoltage&&t._vinVoltage>0?'<span>VIN: '+t._vinVoltage.toFixed(2)+'V</span>':''}</div><div class="dc-actions"><button class="secondary btn-small" onclick="refreshSensor('${escapeHtml(t._clientUid)}')" ${state.refreshing?'disabled':''}>Refresh</button><button class="secondary btn-small" onclick="clearRelays('${escapeHtml(t._clientUid)}',${t.sensorIdx||0})" ${state.refreshing?'disabled':''}>Clear Relay</button></div>`;return card;}
)HTML" R"HTML(
function toggleDotInfo(section,cl,siteName){const slot=section.querySelector('.cdot-info-slot');if(state.expandedDot===cl.uid){slot.innerHTML='';state.expandedDot=null;return;}state.expandedDot=cl.uid;const color=clientStatusColor(cl);const statusText=cl.alarm?'ALARM':isStale(cl.lastUpdate)?'Stale / No Data':'Online';const sensorCount=cl.sensors.length;slot.innerHTML=`<div class="cdot-info open"><div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;"><div><strong>${escapeHtml(cl.uid)}</strong><span class="status-pill ${color==='red'?'alarm':color==='yellow'?'stale':'ok'}" style="margin-left:8px;">${statusText}</span></div><div style="display:flex;gap:6px;"><a class="pill secondary" style="font-size:0.75rem;padding:2px 10px;" href="/config-generator?uid=${encodeURIComponent(cl.uid)}">Edit Config</a><a class="pill secondary" style="font-size:0.75rem;padding:2px 10px;" href="/site-config?site=${encodeURIComponent(siteName)}">Site Config</a><button class="pill secondary" style="font-size:0.75rem;padding:2px 10px;color:var(--danger);border-color:var(--danger);" onclick="deleteClient('${escapeHtml(cl.uid)}')">Remove Client</button></div></div><div style="margin-top:6px;color:var(--muted);font-size:0.85rem;">${sensorCount} sensor${sensorCount!==1?'s':''} &middot; Last update: ${formatEpoch(cl.lastUpdate)} &middot; VIN: ${formatVoltage(cl.vinVoltage)}</div></div>`;}
function updateStats(){const allClients=Object.values(state.sites).flatMap(s=>Object.values(s.clients));const allSensors=allClients.flatMap(c=>c.sensors);const clientIds=new Set(allClients.map(c=>c.uid));if(els.statClients)els.statClients.textContent=clientIds.size;if(els.statTanks)els.statTanks.textContent=allSensors.length;if(els.statAlarms)els.statAlarms.textContent=allSensors.filter(t=>t.alarm).length;const stale=allSensors.filter(t=>isStale(t.lastUpdate)).length;if(els.statStale)els.statStale.textContent=stale;}
)HTML" R"HTML(
function renderPauseButton(){const btn=els.pauseBtn;if(!btn)return;if(state.paused){btn.classList.add('paused');btn.style.display='';btn.textContent='Unpause';btn.title='Resume data flow';}else{btn.classList.remove('paused');btn.style.display='none';}}
if(els.pauseBtn){els.pauseBtn.addEventListener('click',togglePause);els.pauseBtn.addEventListener('mouseenter',()=>{if(state.paused)els.pauseBtn.textContent='Resume';});els.pauseBtn.addEventListener('mouseleave',()=>{renderPauseButton();});}
async function togglePause(){const target=!state.paused;try{const res=await fetch('/api/pause',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({paused:target})});if(!res.ok){throw new Error(await res.text()||'Pause toggle failed');}const data=await res.json();state.paused=!!data.paused;renderPauseButton();showToast(state.paused?'Paused for maintenance':'Resumed');}catch(err){showToast(err.message||'Pause toggle failed',true);}}
async function refreshSensor(clientUid){if(state.refreshing)return;state.refreshing=true;renderSites();try{const res=await fetch('/api/refresh',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid})});if(!res.ok){throw new Error(await res.text()||'Refresh failed');}applyServerData(await res.json());showToast('Refreshed');}catch(err){showToast(err.message||'Refresh failed',true);}finally{state.refreshing=false;renderSites();}}
window.refreshSensor=refreshSensor;
window.logout=function(){fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login';});};
async function clearRelays(clientUid,sensorIdx){if(state.refreshing)return;state.refreshing=true;renderSites();try{const res=await fetch('/api/relay/clear',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({clientUid:clientUid,sensorIdx:sensorIdx})});if(!res.ok){throw new Error(await res.text()||'Clear relay failed');}showToast('Relay clear command sent');setTimeout(()=>refreshData(),1000);}catch(err){showToast(err.message||'Clear relay failed',true);}finally{state.refreshing=false;renderSites();}}
window.clearRelays=clearRelays;
async function deleteClient(clientUid){if(!confirm('Remove client '+clientUid+' and all its sensor data? This cannot be undone.'))return;try{const res=await fetch('/api/client',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:clientUid})});if(!res.ok){throw new Error(await res.text()||'Delete failed');}showToast('Client removed: '+clientUid);state.expandedDot=null;refreshData();}catch(err){showToast(err.message||'Failed to remove client',true);}}
window.deleteClient=deleteClient;
function applyServerData(data){state.clients=data.cs||[];state.sites=buildSiteModel(state.clients);const srv=data.srv||{};state.paused=!!srv.ps;state.uiRefreshS=DEFAULT_REFRESH_S;renderSites();renderPauseButton();updateStats();scheduleRefresh();if(Object.keys(state.sparkData).length){renderSparklines();}else{loadSparklineData();}if(!state.sparkTimer){state.sparkTimer=setInterval(loadSparklineData,300000);}}
function scheduleRefresh(){if(state.timer)clearInterval(state.timer);state.timer=setInterval(refreshData,state.uiRefreshS*1000);}
async function refreshData(){try{const ac=new AbortController();const tid=setTimeout(()=>ac.abort(),10000);const res=await fetch('/api/clients?summary=1',{signal:ac.signal,headers:{'Cache-Control':'no-cache'}});clearTimeout(tid);if(!res.ok)throw new Error('HTTP '+res.status);applyServerData(await res.json());}catch(err){const msg=err.name==='AbortError'?'Request timed out (server busy)':err.message||'Fleet refresh failed';showToast(msg,true);if(els.siteContainer)els.siteContainer.innerHTML='<div class="no-data" style="color:#dc2626">Failed to load: '+escapeHtml(msg)+'<br><small>Retrying in a few seconds...</small></div>';if(!state.timer)setTimeout(refreshData,5000);}finally{hideLoading();}}
refreshData();(async()=>{try{const r=await fetch('/api/github/update');if(!r.ok)return;const d=await r.json();const banner=document.getElementById('ghUpdateBanner');let show=false;if(d.available){const v=document.getElementById('ghUpdateVersion');const l=document.getElementById('ghUpdateLink');const sl=document.getElementById('ghUpdateServerLine');if(v)v.textContent='v'+d.latestVersion;if(l&&d.releaseUrl)l.href=d.releaseUrl;if(sl)sl.style.display='';show=true;}if(d.dfuUpdateAvailable){const dl=document.getElementById('dfuUpdateServerLine');if(dl)dl.style.display='';show=true;}if(d.outdatedClientCount>0){const cc=document.getElementById('ghUpdateClientCount');const lv=document.getElementById('ghUpdateLatestVer');const cl=document.getElementById('ghUpdateClientLine');if(cc)cc.textContent=d.outdatedClientCount;if(lv)lv.textContent=d.latestVersion||d.currentVersion;if(cl)cl.style.display='';show=true;}if(show&&banner)banner.style.display='flex';}catch(e){}})();})();
</script></body></html>)HTML";

static const char CLIENT_CONSOLE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Tank Alarm Client Console</title><link rel="stylesheet" href="/style.css"><style>.client-list{max-height:300px;overflow-y:auto;border:1px solid var(--card-border);border-radius:8px;background:var(--card-bg);}.cl-item{padding:10px 14px;cursor:pointer;border-bottom:1px solid var(--card-border);transition:background 0.15s;}.cl-item:last-child{border-bottom:none;}.cl-item:hover{background:rgba(37,99,235,0.06);}.cl-item.selected{background:rgba(37,99,235,0.1);border-left:3px solid var(--accent);}.cl-item .cl-site{font-weight:600;font-size:0.95rem;}.cl-item .cl-label{color:var(--muted);font-size:0.85rem;margin-top:2px;}.cl-item .cl-uid{font-family:monospace;font-size:0.8rem;color:var(--muted);margin-top:2px;}</style></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><a class="pill secondary" href="/">Dashboard</a><a class="pill" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><section class="card"><h2 style="margin-top:0;">Remote Configuration</h2><p style="color:var(--muted);margin-bottom:12px;">Access remote update tools and generate new client configurations.</p><div class="actions"><a class="pill" href="/config-generator">Client Configuration Tool</a></div></section><section class="card"><h2 style="margin-top:0;">Calibration & Help</h2><p style="color:var(--muted);margin-bottom:12px;">Manage calibration entries and view setup guidance.</p><div class="actions"><a class="pill" href="/calibration">Open Calibration</a><a class="pill secondary" href="https://github.com/SenaxInc/SenaxTankAlarm/blob/master/Tutorials/Tutorials-112025/CLIENT_INSTALLATION_GUIDE.md" target="_blank" title="View Client Installation Guide">Help</a></div></section><div class="console-layout"><section class="card"><div><span style="font-weight:600;display:block;margin-bottom:8px;">Select Client</span><div id="clientList" class="client-list"></div></div><div id="clientDetails" class="details">Select a client to review configuration.</div><div class="refresh-actions"><button type="button" id="refreshSelectedBtn">Refresh Selected Site</button><button type="button" class="secondary" id="requestLocationBtn">Request GPS Location</button></div><div id="locationInfo" class="details" style="margin-top:8px;display:none;"></div></section><section class="card"><h2 style="margin-top:0;">New Sites (Unconfigured)</h2><p style="color:var(--muted);font-size:0.9rem;margin-bottom:12px;">Clients that need initial configuration. Click "Configure" to set up a new client remotely.</p><div class="actions" style="margin-bottom:12px;"><a class="pill secondary" href="https://github.com/SenaxInc/SenaxTankAlarm/blob/master/Tutorials/Tutorials-112025/CLIENT_INSTALLATION_GUIDE.md" target="_blank">Quick Start Guide</a></div><div id="newSitesContainer" class="site-list"><div class="empty-state">No unconfigured clients detected.</div></div></section><section class="card"><h2 style="margin-top:0;">Active Sites</h2><div id="telemetryContainer" class="site-list"></div></section></div></main><div id="toast"></div><script>
(function(){const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);
function escapeHtml(str){if(!str)return'';const div=document.createElement('div');div.textContent=str;return div.innerHTML;}
const state ={data:null,selected:null};
const els ={telemetryContainer:document.getElementById('telemetryContainer'),newSitesContainer:document.getElementById('newSitesContainer'),clientList:document.getElementById('clientList'),clientDetails:document.getElementById('clientDetails'),toast:document.getElementById('toast'),refreshSelectedBtn:document.getElementById('refreshSelectedBtn'),requestLocationBtn:document.getElementById('requestLocationBtn'),locationInfo:document.getElementById('locationInfo')};
function showToast(message,isError){if(els.toast)els.toast.textContent= message;if(els.toast)els.toast.style.background = isError ? '#dc2626':'#0284c7';if(els.toast)els.toast.classList.add('show');setTimeout(()=>{if(els.toast)els.toast.classList.remove('show')},2500);}
function formatNumber(value){return(typeof value === 'number' && isFinite(value))? value.toFixed(1):'--';}
function formatEpoch(epoch){if(!epoch)return '--';const date = new Date(epoch * 1000);if(isNaN(date.getTime()))return '--';return date.toLocaleString(undefined,{year:'numeric',month:'numeric',day:'numeric',hour:'numeric',minute:'2-digit',hour12:true});}
function signalIcon(sig){if(!sig||sig.bars==null||sig.bars<0)return'';const b=sig.bars;const c=b<=1?'#dc2626':b<=2?'#f59e0b':'#10b981';const lbl=b<=1?'Weak':b<=2?'Fair':b<=3?'Good':'Strong';return' <span title="'+lbl+' signal: '+b+'/4 bars'+(sig.rsrp?', RSRP: '+sig.rsrp+'dBm':'')+'" style="color:'+c+';font-size:0.7rem;font-weight:600;">&#x25A0;'+b+'/4</span>';}
function renderTelemetry(){if(!els.telemetryContainer)return;els.telemetryContainer.innerHTML = '';if(!state.data || !state.data.clients)return;const sites ={};state.data.clients.forEach(client =>{const siteName = client.site || 'Unknown Site';if(!sites[siteName])sites[siteName]=[];sites[siteName].push(client);});Object.keys(sites).sort().forEach(siteName =>{const sensors = sites[siteName];const siteCard = document.createElement('div');siteCard.className = 'site-card';const header = document.createElement('div');header.className = 'site-header';header.innerHTML = `<span>${escapeHtml(siteName)}</span><span style="display:flex;align-items:center;gap:8px;"><span style="font-size:0.8rem;font-weight:400;color:var(--muted)">${sensors.length} device${sensors.length===1?'':'s'}</span><a class="pill secondary" href="/site-config?site=${encodeURIComponent(siteName)}" style="font-size:0.75rem;padding:3px 10px;">Manage Site</a></span>`;siteCard.appendChild(header);const grid = document.createElement('div');grid.className = 'site-sensors';sensors.forEach(tank =>{const tankDiv = document.createElement('div');tankDiv.className = 'site-tank';if(tank.alarm)tankDiv.classList.add('alarm');if(!tank.lastUpdate){tankDiv.style.opacity='0.7';tankDiv.innerHTML = `<div class="tank-meta"><strong>Configured Client</strong><code>${escapeHtml(tank.client)}</code></div><div class="tank-value" style="font-size:0.85rem;color:var(--muted);">Awaiting first report</div><div class="tank-footer"><button type="button" class="secondary" style="padding:2px 10px;font-size:0.8rem;" onclick="window.location.href='/config-generator?uid=${encodeURIComponent(tank.client)}'">Edit Config</button></div>`;}else{const ot=tank.objectType||'tank';const tlbl=tank.label||(ot==='tank'?'Tank':'Sensor');const tmu=tank.measurementUnit||'';const tUnit=(function(mu,o){const abr={inches:'in',psi:'psi',rpm:'rpm',gpm:'gpm'};if(mu){const k=mu.toLowerCase();return abr[k]||mu;}if(o==='gas')return'psi';if(o==='rpm')return'rpm';if(o==='flow')return'gpm';return'in';})(tmu,ot);tankDiv.innerHTML = `<div class="tank-meta"><strong>${escapeHtml(tlbl)}${tank.userNumber?' #'+tank.userNumber:''}</strong><code>${escapeHtml(tank.client).substring(0,8)}...</code>${signalIcon(tank.signal)}</div><div class="tank-value">${formatNumber(tank.levelInches)} <small style="font-size:0.7em;color:var(--muted)">${tUnit}</small></div><div class="tank-footer">${tank.alarm ? `<span style="color:var(--danger);font-weight:600">ALARM: ${escapeHtml(tank.alarmType)}</span>`:'Normal'}<span style="float:right">${formatEpoch(tank.lastUpdate)}</span></div>`;}grid.appendChild(tankDiv);});siteCard.appendChild(grid);els.telemetryContainer.appendChild(siteCard);});}
function renderNewSites(){if(!els.newSitesContainer)return;els.newSitesContainer.innerHTML='';if(!state.data||!state.data.clients){els.newSitesContainer.innerHTML='<div class="empty-state">No clients detected yet.</div>';return;}const unconfigured=state.data.clients.filter(client=>{if(!client.lastUpdate)return false;const hasNoSite=!client.site||client.site.trim()===''||client.site==='Unknown Site';const hasNoLabel=!client.label||client.label.trim()==='';const hasDefaultLabel=client.label&&(client.label==='Client-112025'||client.label==='Unconfigured Client'||client.label.includes('Tank A'));return hasNoSite||(hasNoLabel||hasDefaultLabel);});if(unconfigured.length===0){els.newSitesContainer.innerHTML='<div class="empty-state">No unconfigured clients detected.</div>';return;}unconfigured.forEach(client=>{const card=document.createElement('div');card.className='site-card';card.style.borderLeft='4px solid var(--warning)';const firmwareVersion=client.firmwareVersion||client.version||'unknown';const lastSeenEpoch=client.lastUpdate||0;const now=Date.now()/1000;const ageSeconds=now-lastSeenEpoch;let lastSeenDisplay='';if(ageSeconds<120){lastSeenDisplay='<span style="color:#10b981;">&#x2714; Active now</span>';}else if(ageSeconds<3600){const mins=Math.floor(ageSeconds/60);lastSeenDisplay=`Last seen ${mins} min${mins!==1?'s':''} ago`;}else if(ageSeconds<86400){const hours=Math.floor(ageSeconds/3600);lastSeenDisplay=`Last seen ${hours} hour${hours!==1?'s':''} ago`;}else{const days=Math.floor(ageSeconds/86400);lastSeenDisplay=`Last seen ${days} day${days!==1?'s':''} ago - ${formatEpoch(lastSeenEpoch)}`;}card.innerHTML=`<div style="display:flex;justify-content:space-between;align-items:center;"><div><strong style="color:var(--warning)">&#x26A0; Unconfigured Client</strong><div style="font-size:0.85rem;color:var(--muted);margin-top:4px;">UID: <code>${escapeHtml(client.client)}</code></div><div style="font-size:0.85rem;margin-top:4px;">${lastSeenDisplay}</div><div style="font-size:0.85rem;margin-top:4px;color:var(--muted);">Firmware: ${escapeHtml(firmwareVersion)}</div></div><button type="button" class="configure-btn" style="white-space:nowrap;">Configure &#x2192;</button></div>`;card.querySelector)HTML" R"HTML(('.configure-btn').addEventListener('click',()=>{window.location.href='/config-generator?uid='+encodeURIComponent(client.client);});els.newSitesContainer.appendChild(card);});}
function populateClientList(){if(!els.clientList)return;els.clientList.innerHTML='';if(!state.data||!state.data.clients||!state.data.clients.length){els.clientList.innerHTML='<div style="padding:12px;color:var(--muted);text-align:center;">No clients available</div>';return;}const items=[];state.data.clients.forEach(c=>{items.push({uid:c.client,site:c.site||'Unknown Site',label:c.label||'',type:'telemetry'});});if(state.data.configs){state.data.configs.forEach(entry=>{if(!items.find(i=>i.uid===entry.client)){items.push({uid:entry.client,site:entry.site||'Site',label:'Stored config',type:'config'});}});}if(!state.selected&&items.length){state.selected=items[0].uid;}items.forEach(item=>{const div=document.createElement('div');div.className='cl-item'+(state.selected===item.uid?' selected':'');div.dataset.uid=item.uid;const shortUid=item.uid.length>12?item.uid.substring(item.uid.length-8):item.uid;div.innerHTML='<div class="cl-site">'+escapeHtml(item.site)+'</div>'+(item.label&&item.type!=='config'?'<div class="cl-label">'+escapeHtml(item.label)+'</div>':'')+(item.type==='config'?'<div class="cl-label">Stored config</div>':'')+'<div class="cl-uid">'+escapeHtml(shortUid)+'</div>';div.addEventListener('click',()=>{state.selected=item.uid;els.clientList.querySelectorAll('.cl-item').forEach(el=>el.classList.remove('selected'));div.classList.add('selected');updateClientDetails(state.selected);fetchLocationInfo(state.selected);});els.clientList.appendChild(div);});updateClientDetails(state.selected);}
function lookupClient(uid){if(!state.data || !state.data.clients)return null;return state.data.clients.find(c => c.client === uid)|| null;}
function updateClientDetails(uid){if(!uid){els.clientDetails.textContent='Select a client';return;}const client = lookupClient(uid);const detailParts = [];if(client){detailParts.push(`<strong>Site:</strong> ${escapeHtml(client.site || 'Unknown')}`);const cOt=client.objectType||'';const dtLbl=client.label||(cOt==='tank'||!cOt?'Tank':'Sensor');const dtNum=client.userNumber?' #'+escapeHtml(String(client.userNumber)):'';detailParts.push(`<strong>Latest:</strong> ${escapeHtml(dtLbl)}${dtNum} at ${formatNumber(client.levelInches)}${client.measurementUnit||'in'}`);}else{detailParts.push('Client not in telemetry data');}detailParts.push(`<strong>UID:</strong><code>${uid}</code>`);detailParts.push(`<button type="button" class="secondary" style="padding:4px 12px;font-size:0.85rem;" onclick="window.location.href='/config-generator?uid=${encodeURIComponent(uid)}'">Edit Configuration</button>`);els.clientDetails.innerHTML = detailParts.join(' - ');}
function normalizeApiData(data){var srv=data.srv||{};var result={server:{name:srv.n||'',smsPrimary:srv.sp||'',smsSecondary:srv.ss||'',productUid:srv.pu||'',pinConfigured:!!srv.pc,dailyEmail:srv.de||''},serverUid:data.si||'',nextDailyEmailEpoch:data.nde||0,clients:(data.cs||[]).map(function(c){return{client:c.c||'',site:c.s||'',label:c.n||'',sensorIndex:c.k||'',userNumber:c.un||0,alarm:!!c.a,alarmType:c.at||'',lastUpdate:c.u||0,levelInches:c.l,vinVoltage:c.v,firmwareVersion:c.fv||'',objectType:c.ot||'',measurementUnit:c.mu||'',signal:c.sig||null,sensors:(c.ts||[]).map(function(t){return{label:t.n||'',sensorIndex:t.k||'',userNumber:t.un||0,levelInches:t.l,heightInches:t.h,alarm:!!t.a,alarmType:t.at||'',lastUpdate:t.u||0,objectType:t.ot||'',measurementUnit:t.mu||''};})};})}};result.configs=(data.cfgs||[]).map(function(cfg){return{client:cfg.c||'',site:cfg.s||'',configJson:cfg.cj||''};});return result;}
function applyServerData(data,preferredUid){try{state.data=normalizeApiData(data);if(preferredUid){state.selected=preferredUid;}renderTelemetry();renderNewSites();populateClientList();}catch(e){console.error('applyServerData error:',e);if(els.telemetryContainer)els.telemetryContainer.innerHTML='<div class="empty-state" style="color:var(--danger);">Render error: '+e.message+'</div>';}}
async function refreshData(preferredUid){try{const query=preferredUid?'&client='+encodeURIComponent(preferredUid):'';const ac=new AbortController();const tid=setTimeout(()=>ac.abort(),10000);const res=await fetch('/api/clients?summary=1'+query,{signal:ac.signal,headers:{'Cache-Control':'no-cache'}});clearTimeout(tid);if(!res.ok){throw new Error('Failed to fetch server data');}const data=await res.json();console.log('API response: cs=',data.cs?data.cs.length:0,'clients, cfgs=',data.cfgs?data.cfgs.length:0);applyServerData(data,preferredUid||state.selected);}catch(err){const msg=err.name==='AbortError'?'Request timed out (server busy)':err.message||'Initialization failed';console.error('refreshData error:',err);showToast(msg,true);if(els.telemetryContainer)els.telemetryContainer.innerHTML='<div class="empty-state" style="color:var(--danger);">'+escapeHtml(msg)+'</div>';}}
async function triggerManualRefresh(targetUid){const payload = targetUid ?{client:targetUid}:{};try{const res = await fetch('/api/refresh',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});if(!res.ok){const text = await res.text();throw new Error(text || 'Refresh failed');}const data = await res.json();applyServerData(data,targetUid || state.selected);showToast(targetUid ? 'Selected site updated':'All sites updated');}catch(err){showToast(err.message || 'Refresh failed',true);}}
async function requestLocation(clientUid){if(!clientUid){showToast('Select a client first.',true);return;}try{const res = await fetch('/api/location',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({clientUid:clientUid})});if(!res.ok){const text = await res.text();throw new Error(text || 'Location request failed');}showToast('Location request sent - check back in a few minutes');await fetchLocationInfo(clientUid);}catch(err){showToast(err.message || 'Location request failed',true);}}
async function fetchLocationInfo(clientUid){if(!clientUid || !els.locationInfo)return;try{const res = await fetch(`/api/location?client=${encodeURIComponent(clientUid)}`);if(!res.ok)return;const data = await res.json();if(data.hasLocation){const date = data.locationEpoch ? new Date(data.locationEpoch * 1000).toLocaleString():'Unknown';els.locationInfo.innerHTML = `<strong>&#x1F4CD; Cached Location:</strong> ${data.latitude.toFixed(4)}, ${data.longitude.toFixed(4)} <span style="color:var(--muted)">(as of ${date})</span>`;els.locationInfo.style.display = 'block';}else{els.locationInfo.innerHTML = '<strong>&#x1F4CD; Location:</strong> <span style="color:var(--muted)">Not yet received. Click "Request GPS Location" to fetch.</span>';els.locationInfo.style.display = 'block';}}catch(err){els.locationInfo.style.display = 'none';}}
if(els.refreshSelectedBtn)els.refreshSelectedBtn.addEventListener('click',()=>{if(!state.selected){showToast('Select a client first.',true);return;}triggerManualRefresh(state.selected);});if(els.requestLocationBtn)els.requestLocationBtn.addEventListener('click',()=>{requestLocation(state.selected);});refreshData().then(()=>{if(state.selected)fetchLocationInfo(state.selected);});})();
</script></body></html>)HTML";

static const char SITE_CONFIG_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Site Configuration - Tank Alarm Server</title><link rel="stylesheet" href="/style.css"><style>.site-title{font-size:1.5rem;font-weight:700;margin:0;}.site-subtitle{color:var(--muted);font-size:0.9rem;margin-top:4px;}.client-grid{display:grid;gap:16px;margin-top:16px;}.client-card{background:var(--card-bg);border:1px solid var(--card-border);border-radius:8px;padding:16px;}.client-card.has-alarm{border-left:4px solid var(--danger);}.client-header{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:12px;}.client-name{font-weight:600;font-size:1.05rem;}.client-uid{font-family:monospace;font-size:0.8rem;color:var(--muted);margin-top:2px;}.sensor-list{display:grid;gap:8px;}.sensor-row{display:flex;justify-content:space-between;align-items:center;padding:8px 12px;background:var(--bg);border-radius:6px;font-size:0.9rem;}.sensor-row.alarm{background:rgba(220,38,38,0.08);}.sensor-level{font-weight:600;font-size:1.1rem;}.sensor-status{font-size:0.85rem;}.uid-table{width:100%;border-collapse:collapse;margin-top:12px;font-size:0.9rem;}.uid-table th,.uid-table td{padding:8px 12px;text-align:left;border-bottom:1px solid var(--card-border);}.uid-table th{font-weight:600;color:var(--muted);font-size:0.8rem;text-transform:uppercase;}.uid-table td code{font-size:0.85rem;cursor:pointer;}.uid-table td code:hover{color:var(--accent);}.copy-toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#10b981;color:#fff;padding:8px 16px;border-radius:6px;font-size:0.85rem;opacity:0;transition:opacity 0.3s;pointer-events:none;}.copy-toast.show{opacity:1;}</style></head><body data-theme="light"><header><div class="bar"><div class="brand">TankAlarm</div><div class="header-actions"><a class="pill secondary" href="/">Dashboard</a><a class="pill secondary" href="/client-console">Client Console</a><a class="pill secondary" href="/contacts">Contacts</a><a class="pill secondary" href="/server-settings">Server Settings</a><button class="pill secondary" onclick="fetch('/api/logout',{method:'POST'}).finally(()=>{localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login'})">Logout</button></div></div></header><main><div class="card"><div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;"><div><h1 class="site-title" id="siteTitle">Loading...</h1><p class="site-subtitle" id="siteSubtitle"></p></div><div style="display:flex;gap:8px;flex-wrap:wrap;"><button id="addClientBtn" class="pill">+ Add Client</button><button id="backBtn" class="pill secondary" onclick="window.location.href='/client-console'">Back to Console</button></div></div></div><div class="card"><h2 style="margin-top:0;">Client Devices</h2><div id="clientGrid" class="client-grid"><div class="empty-state">Loading site data...</div></div></div><div class="card"><h2 style="margin-top:0;">Client UID Quick Reference</h2><p style="color:var(--muted);font-size:0.9rem;margin-bottom:8px;">Click any UID to copy it. Use these UIDs for relay target configuration.</p><table class="uid-table"><thead><tr><th>Device Label</th><th>Client UID</th><th>Sensors</th><th>Status</th></tr></thead><tbody id="uidTableBody"></tbody></table></div></main><div id="toast"></div><div class="copy-toast" id="copyToast">UID copied to clipboard</div>)HTML" R"HTML(<script>(async ()=>{const token=localStorage.getItem('tankalarm_token');const _s=localStorage.getItem('tankalarm_session');if(!token||!_s){window.location.href='/login?redirect='+encodeURIComponent(window.location.pathname);return;}const _F=window.fetch;window.fetch=function(u,o){if(!o)o={};if(!o.headers)o.headers={};if(o.headers instanceof Headers)o.headers.set('X-Session',_s);else o.headers['X-Session']=_s;return _F.call(window,u,o).then(function(r){if(r.status===401){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}return r;});};async function _ckSess(){const sid=localStorage.getItem('tankalarm_session');if(!sid){window.location.href='/login?reason=expired';return;}try{const r=await fetch('/api/session/check');const d=await r.json();if(!d.valid){localStorage.removeItem('tankalarm_token');localStorage.removeItem('tankalarm_session');window.location.href='/login?reason=expired';}}catch(e){}}document.addEventListener('visibilitychange',()=>{if(!document.hidden)_ckSess();});setInterval(_ckSess,30000);function escapeHtml(s){if(!s)return'';const d=document.createElement('div');d.textContent=s;return d.innerHTML;}function formatNumber(v){return(typeof v==='number'&&isFinite(v))?v.toFixed(1):'--';}function formatEpoch(e){if(!e)return'--';const d=new Date(e*1000);if(isNaN(d.getTime()))return'--';return d.toLocaleString(undefined,{year:'numeric',month:'numeric',day:'numeric',hour:'numeric',minute:'2-digit',hour12:true});}function getQueryParam(n){return new URLSearchParams(window.location.search).get(n)||'';}const siteName=getQueryParam('site');if(!siteName){document.getElementById('siteTitle').textContent='No site specified';document.getElementById('clientGrid').innerHTML='<div class="empty-state">Use ?site=SiteName in the URL or navigate from the Client Console.</div>';return;}document.getElementById('siteTitle').textContent=siteName;document.title=siteName+' - Site Configuration';document.getElementById('addClientBtn').addEventListener('click',()=>{window.location.href='/config-generator?site='+encodeURIComponent(siteName);});function showToast(msg,isErr){const t=document.getElementById('toast');t.textContent=msg;t.style.background=isErr?'#dc2626':'#0284c7';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500);}function copyUid(uid){navigator.clipboard.writeText(uid).then(()=>{const ct=document.getElementById('copyToast');ct.classList.add('show');setTimeout(()=>ct.classList.remove('show'),1500);}).catch(()=>{showToast('Copy failed',true);});}window.copyUid=copyUid;)HTML" R"HTML(async function loadSiteData(){try{const r=await fetch('/api/clients?summary=1');if(!r.ok)throw new Error('Failed to fetch');const raw=await r.json();const srv=raw.srv||{};const allClients=(raw.cs||[]).map(c=>({uid:c.c||'',site:c.s||'Unknown Site',label:c.n||'',sensorIndex:c.k||'',alarm:!!c.a,alarmType:c.at||'',lastUpdate:c.u||0,levelInches:c.l,sensorMa:c.ma,vinVoltage:c.v,vinVoltageEpoch:c.ve,sensorCount:c.tc||1,sensors:(c.ts||[]).map(t=>({label:t.n||'',sensorIndex:t.k||'',userNumber:t.un||0,levelInches:t.l,alarm:!!t.a,alarmType:t.at||'',lastUpdate:t.u||0,sensorType:t.st||'',objectType:t.ot||'',measurementUnit:t.mu||'',contents:t.ct||'',change24h:t.d}))}));const siteClients=allClients.filter(c=>c.site===siteName);const uniqueMap=new Map();siteClients.forEach(c=>{if(!uniqueMap.has(c.uid)){uniqueMap.set(c.uid,{uid:c.uid,label:c.label,alarm:false,vinVoltage:c.vinVoltage,vinVoltageEpoch:c.vinVoltageEpoch,sensors:[]});}const entry=uniqueMap.get(c.uid);if(c.sensors&&c.sensors.length){c.sensors.forEach(t=>entry.sensors.push(t));}else{entry.sensors.push({label:c.label||'',sensorIndex:c.sensorIndex,levelInches:c.levelInches,alarm:c.alarm,alarmType:c.alarmType,lastUpdate:c.lastUpdate,objectType:'',measurementUnit:'',contents:''});}if(c.alarm)entry.alarm=true;entry.sensors.forEach(t=>{if(t.alarm)entry.alarm=true;});});const clients=Array.from(uniqueMap.values());document.getElementById('siteSubtitle').textContent=clients.length+' client device'+(clients.length===1?'':'s')+' at this site';renderClients(clients);renderUidTable(clients);}catch(e){console.error(e);document.getElementById('clientGrid').innerHTML='<div class="empty-state">Error loading site data.</div>';}}function objectTypeLabel(ot){const map={tank:'Tank',gas:'Gas Monitor',engine:'Engine',pump:'Pump',flow:'Flow Meter'};return map[ot]||'Sensor';}function sensorDisplayName(t){const ot=t.objectType||'';const lbl=t.label||objectTypeLabel(ot);return lbl+(t.userNumber?' #'+t.userNumber:'');}function unitLabel(mu,ot){const abr={inches:'in',psi:'psi',rpm:'rpm',gpm:'gpm'};if(mu){const k=mu.toLowerCase();return abr[k]||mu;}if(ot==='gas')return'psi';if(ot==='rpm')return'rpm';if(ot==='flow')return'gpm';return'in';}function renderClients(clients){const grid=document.getElementById('clientGrid');grid.innerHTML='';if(!clients.length){grid.innerHTML='<div class="empty-state">No client devices found at this site.</div>';return;})HTML" R"HTML(clients.forEach(client=>{const card=document.createElement('div');card.className='client-card'+(client.alarm?' has-alarm':'');let tanksHtml='';client.sensors.forEach(t=>{const alarmClass=t.alarm?' alarm':'';const statusHtml=t.alarm?`<span class="sensor-status" style="color:var(--danger);font-weight:600;">ALARM: ${escapeHtml(t.alarmType)}</span>`:'<span class="sensor-status" style="color:#10b981;">Normal</span>';const mu=unitLabel(t.measurementUnit,t.objectType);const changeHtml=(typeof t.change24h==='number')?`<span style="font-size:0.8rem;color:var(--muted);">${t.change24h>=0?'+':''}${t.change24h.toFixed(1)} ${mu}/24h</span>`:'';const contentsHtml=t.contents?`<span style="font-size:0.8rem;color:var(--muted);margin-left:6px;">(${escapeHtml(t.contents)})</span>`:'';tanksHtml+=`<div class="sensor-row${alarmClass}"><div><strong>${escapeHtml(sensorDisplayName(t))}</strong>${contentsHtml} ${changeHtml}</div><div style="text-align:right;"><span class="sensor-level">${formatNumber(t.levelInches)}</span> <small style="color:var(--muted)">${mu}</small><div style="font-size:0.8rem;color:var(--muted);">${formatEpoch(t.lastUpdate)}</div></div></div>`;});const voltageHtml=(typeof client.vinVoltage==='number'&&client.vinVoltage>0)?`<span style="font-size:0.85rem;color:var(--muted);">VIN: ${client.vinVoltage.toFixed(2)}V</span>`:'';card.innerHTML=`<div class="client-header"><div><div class="client-name">${escapeHtml(client.label||'Client Device')}</div><div class="client-uid">${escapeHtml(client.uid)}</div>${voltageHtml}</div><div style="display:flex;gap:6px;"><button class="pill secondary" style="font-size:0.8rem;padding:4px 12px;" onclick="window.location.href='/config-generator?uid=${encodeURIComponent(client.uid)}'">Edit Config</button><button class="pill secondary" style="font-size:0.8rem;padding:4px 12px;color:var(--danger);border-color:var(--danger);" onclick="deleteClient('${escapeHtml(client.uid)}')">Remove Client</button></div></div><div class="sensor-list">${tanksHtml}</div>`;grid.appendChild(card);});}async function deleteClient(uid){if(!confirm('Remove client '+uid+' and all its sensor data? This action cannot be undone.'))return;try{const res=await fetch('/api/client',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({client:uid})});if(!res.ok){throw new Error(await res.text()||'Delete failed');}showToast('Client removed successfully');loadSiteData();}catch(err){showToast(err.message||'Failed to remove client',true);}}function renderUidTable(clients){const tbody=document.getElementById('uidTableBody');tbody.innerHTML='';if(!clients.length){tbody.innerHTML='<tr><td colspan="4" style="text-align:center;color:var(--muted);">No clients at this site.</td></tr>';return;}clients.forEach(client=>{const tr=document.createElement('tr');const hasAlarm=client.sensors.some(t=>t.alarm);const statusHtml=hasAlarm?'<span style="color:var(--danger);font-weight:600;">ALARM</span>':'<span style="color:#10b981;">Normal</span>';tr.innerHTML=`<td>${escapeHtml(client.label||'Client Device')}</td><td><code onclick="copyUid('${escapeHtml(client.uid)}')" title="Click to copy">${escapeHtml(client.uid)}</code></td><td>${client.sensors.length}</td><td>${statusHtml}</td>`;tbody.appendChild(tr);});}loadSiteData();setInterval(loadSiteData,30000);})();</script></body></html>)HTML";

static void initializeStorage();
static void ensureConfigLoaded();
static void createDefaultConfig(ServerConfig &cfg);
static bool loadConfig(ServerConfig &cfg);
static bool saveConfig(const ServerConfig &cfg);
static bool loadContactsConfig(JsonDocument &doc);
static bool saveContactsConfig(const JsonDocument &doc);
static double loadServerHeartbeatEpoch();
static bool saveServerHeartbeatEpoch(double epoch);
static void printHardwareRequirements();
static void initializeNotecard();
static void ensureTimeSync();
static double currentEpoch();
static void scheduleNextDailyEmail();
static void scheduleNextViewerSummary();
static void initializeEthernet();
static void checkServerVoltage();
static void checkForFirmwareUpdate();
static void enableDfuMode();
static void handleWebRequests();
static bool readHttpRequest(EthernetClient &client, String &method, String &path, String &body, size_t &contentLength, bool &bodyTooLarge, char *sessionHdr, size_t sessionHdrSize);
static void respondHtml(EthernetClient &client, const String &body);
static void sendStringRange(EthernetClient &client, const String &str, size_t start, size_t end);
static void respondJson(EthernetClient &client, const String &body, int status);
static bool respondJson(EthernetClient &client, const JsonDocument &doc, int status);
static void respondJson(EthernetClient &client, const String &body);
static bool respondJson(EthernetClient &client, const JsonDocument &doc);
// Note: respondStatus is forward-declared earlier in the file (before requireValidPin)

static void beginChunkedCsvDownload(EthernetClient &client, const String &filename);
static void sendChunk(EthernetClient &client, const char *data, size_t len);
static void sendChunk(EthernetClient &client, const String &data);
static void endChunkedResponse(EthernetClient &client);
static size_t buildSerialLogCsvLine(char *out, size_t outSize, const SerialLogEntry &entry, const char *clientLabel);

static void sendSensorJson(EthernetClient &client);
static void sendUnloadLogJson(EthernetClient &client);
static void sendClientDataJson(EthernetClient &client, const String &query = String());
static void handleConfigPost(EthernetClient &client, const String &body);
static void handlePausePost(EthernetClient &client, const String &body);
static void handleFtpBackupPost(EthernetClient &client, const String &body);
static void handleFtpRestorePost(EthernetClient &client, const String &body);
static void handleFtpBackupStatusGet(EthernetClient &client);
static void handleSystemStatusGet(EthernetClient &client);
static void printMemoryBudget();
static bool readFtpBackupMarker(char *out, size_t outSize);
static void writeFtpBackupMarker(const char *stage);
static void clearFtpBackupMarker();
// Enum definitions
enum class SerialRequestResult : uint8_t {
  Sent = 0,
  Throttled,
  NotecardFailure
};

// Forward declarations
static void handlePinPost(EthernetClient &client, const String &body);
static void handleRefreshPost(EthernetClient &client, const String &body);
static void handleRelayPost(EthernetClient &client, const String &body);
static void handleRelayClearPost(EthernetClient &client, const String &body);
static void handleSerialLogsGet(EthernetClient &client, const String &queryString);
static void handleSerialLogsDownload(EthernetClient &client, const String &queryString);
static void handleSerialRequestPost(EthernetClient &client, const String &body);
static void handleCalibrationGet(EthernetClient &client);
static void handleCalibrationPost(EthernetClient &client, const String &body);
static void handleCalibrationDelete(EthernetClient &client, const String &body);
static void sendHistoryJson(EthernetClient &client, const String &query = "");
static void handleHistoryCompare(EthernetClient &client, const String &query);
static void handleHistoryYearOverYear(EthernetClient &client, const String &query);
static void handleArchivedClients(EthernetClient &client, const String &query);
static void handleServerSettingsPost(EthernetClient &client, const String &body);
static void handleContactsGet(EthernetClient &client);
static void handleContactsPost(EthernetClient &client, const String &body);
static void handleEmailFormatGet(EthernetClient &client);
static void handleEmailFormatPost(EthernetClient &client, const String &body);
static void handleDfuStatusGet(EthernetClient &client);
static void handleDfuCheckPost(EthernetClient &client, const String &body);
static void handleDfuEnablePost(EthernetClient &client, const String &body);
static bool attemptGitHubDirectInstall(String &statusMessage);
static void handleGitHubUpdateStatusGet(EthernetClient &client);
static void handleNotecardStatusGet(EthernetClient &client);
static void handleLocationRequestPost(EthernetClient &client, const String &body);
static void handleLocationGet(EthernetClient &client, const String &path);
static void handleClientConfigGet(EthernetClient &client, const String &query);
static SensorCalibration *findSensorCalibration(const char *clientUid, uint8_t sensorIndex);
static SensorCalibration *findOrCreateSensorCalibration(const char *clientUid, uint8_t sensorIndex);
static void recalculateCalibration(SensorCalibration *cal);
static void loadCalibrationData();
static void saveCalibrationData();
static void saveCalibrationEntry(const char *clientUid, uint8_t sensorIndex, double timestamp, float sensorReading, float verifiedLevelInches, float temperatureF, const char *notes);
static ConfigDispatchStatus dispatchClientConfig(const char *clientUid, JsonVariantConst cfgObj);
static bool sendRelayCommand(const char *clientUid, uint8_t relayNum, bool state, const char *source);
static void pollNotecard();
static void processNotefile(const char *fileName, void (*handler)(JsonDocument &, double));
static void handleTelemetry(JsonDocument &doc, double epoch);
static void handleAlarm(JsonDocument &doc, double epoch);
static void handleDaily(JsonDocument &doc, double epoch);
static void handleSerialLog(JsonDocument &doc, double epoch);
static void handleSerialAck(JsonDocument &doc, double epoch);
static void handleRelayForward(JsonDocument &doc, double epoch);
static void handleLocationResponse(JsonDocument &doc, double epoch);
static bool sendLocationRequest(const char *clientUid);
static void addServerSerialLog(const char *message, const char *level = "info", const char *source = "server");
static ClientSerialBuffer *findOrCreateClientSerialBuffer(const char *clientUid);
static void addClientSerialLog(const char *clientUid, const char *message, double timestamp, const char *level = "info", const char *source = "client");
static SerialRequestResult requestClientSerialLogs(const char *clientUid, String &errorMessage);
static SensorRecord *upsertSensorRecord(const char *clientUid, uint8_t sensorIndex);
static void sendSmsAlert(const char *message);
static void sendDailyEmail();
static void loadClientConfigSnapshots();
static void saveClientConfigSnapshots();
static bool cacheClientConfigFromBuffer(const char *clientUid, const char *buffer);
static ClientConfigSnapshot *findClientConfigSnapshot(const char *clientUid);
static bool sendConfigViaNotecard(const char *clientUid, const char *jsonPayload);
static void checkGitHubForUpdate();
static uint8_t purgePendingConfigNotes(const char *clientUid);
static void pruneOrphanedSensorRecords(const char *clientUid);
static void dispatchPendingConfigs();
static void handleConfigRetryPost(EthernetClient &client, const String &body);
static void handleConfigCancelPost(EthernetClient &client, const String &body);
static void handleSyncRequestPost(EthernetClient &client, const String &body);
static float convertMaToLevelWithTemp(const char *clientUid, uint8_t sensorIndex, float mA, float currentTempF);
static float convertVoltageToLevel(const char *clientUid, uint8_t sensorIndex, float voltage);
static ClientMetadata *findClientMetadata(const char *clientUid);
static ClientMetadata *findOrCreateClientMetadata(const char *clientUid);
static bool checkSmsRateLimit(SensorRecord *rec, bool bypassMinimumInterval = false);
static void publishViewerSummary();
// Persistence: sensor registry and client metadata
static void saveSensorRegistry();
static void loadSensorRegistry();
static uint8_t deduplicateSensorRecordsLinear();
static void handleDebugSensors(EthernetClient &client, const String &method, const String &body, const String &queryString);
static void saveClientMetadataCache();
static void loadClientMetadataCache();
// Stale client alerting
static void checkStaleClients();
static void pruneStaleOrphanSensors(const char *clientUid, double now);
static bool removeClientData(const char *clientUid);
static bool archiveClientToFtp(const char *clientUid);
// Config push acknowledgment
static void handleConfigAck(JsonDocument &doc, double epoch);
// Client removal
static void handleClientDeleteRequest(EthernetClient &client, const String &body);
static String getQueryParam(const String &query, const char *key);
static bool requireValidPin(EthernetClient &client, const char *pinValue);

// NWS Weather API functions (for calibration temperature data)
static bool nwsLookupGridPoint(ClientMetadata *meta);
static float nwsFetchAverageTemperature(ClientMetadata *meta, double timestamp);
static float nwsGetCalibrationTemperature(const char *clientUid, double timestamp);
static float getCachedTemperature(const char *clientUid);

static void handleRefreshPost(EthernetClient &client, const String &body) {
  char clientUid[64] = {0};
  const char *pinValue = nullptr;
  if (body.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      const char *uid = doc["client"] | "";
      if (uid && *uid) {
        strlcpy(clientUid, uid, sizeof(clientUid));
      }
      pinValue = doc["pin"].as<const char *>();
    }
  }

  if (!requireValidPin(client, pinValue)) {
    return;
  }

  if (clientUid[0]) {
    Serial.print(F("Manual refresh requested for client " ));
    Serial.println(clientUid);
  } else {
    Serial.println(F("Manual refresh requested for all clients"));
  }

  pollNotecard();
  sendClientDataJson(client);
}

static void handleSerialLogsGet(EthernetClient &client, const String &queryString) {
  String source = getQueryParam(queryString, "source");
  source.toLowerCase();
  if (source.length() == 0) {
    source = "server";
  }

  String clientUid = getQueryParam(queryString, "client");
  int maxEntries = SERIAL_DEFAULT_MAX_ENTRIES;
  String maxParam = getQueryParam(queryString, "max");
  if (maxParam.length()) {
    int requested = maxParam.toInt();
    if (requested > 0) {
      int cap = (source == "server") ? SERVER_SERIAL_BUFFER_SIZE : CLIENT_SERIAL_BUFFER_SIZE;
      maxEntries = min(requested, cap);
    }
  }

  double sinceEpoch = 0.0;
  String sinceParam = getQueryParam(queryString, "since");
  if (sinceParam.length()) {
    sinceEpoch = sinceParam.toDouble();
  }

  JsonDocument doc;
  JsonArray logsArray = doc["logs"].to<JsonArray>();
  JsonObject meta = doc["meta"].to<JsonObject>();
  meta["source"] = source;
  meta["requestedClient"] = clientUid;
  meta["staleSeconds"] = SERIAL_STALE_SECONDS;
  meta["max"] = maxEntries;
  meta["since"] = sinceEpoch;

  int added = 0;

  if (source == "server") {
    uint8_t startIdx = (gServerSerial.count < SERVER_SERIAL_BUFFER_SIZE) ? 0 : gServerSerial.writeIndex;
    for (uint8_t i = 0; i < gServerSerial.count; ++i) {
      uint8_t idx = (startIdx + i) % SERVER_SERIAL_BUFFER_SIZE;
      SerialLogEntry &entry = gServerSerial.entries[idx];
      if (entry.message[0] == '\0') {
        continue;
      }
      if (sinceEpoch > 0.0 && entry.timestamp <= sinceEpoch) {
        continue;
      }
      JsonObject row = logsArray.add<JsonObject>();
      row["timestamp"] = entry.timestamp;
      row["message"] = entry.message;
      row["level"] = entry.level;
      row["source"] = entry.source;
      row["client"] = "server";
      added++;
      if (maxEntries > 0 && added >= maxEntries) {
        break;
      }
    }
    meta["total"] = gServerSerial.count;
  } else if (source == "client" && clientUid.length() > 0) {
    ClientSerialBuffer *buf = nullptr;
    for (uint8_t b = 0; b < gClientSerialBufferCount; ++b) {
      if (strcmp(gClientSerialBuffers[b].clientUid, clientUid.c_str()) == 0) {
        buf = &gClientSerialBuffers[b];
        break;
      }
    }

    if (buf) {
      meta["lastAckEpoch"] = buf->lastAckEpoch;
      meta["lastAckStatus"] = buf->lastAckStatus;
      meta["lastRequestEpoch"] = buf->lastRequestEpoch;
      meta["lastLogEpoch"] = buf->lastLogEpoch;
      meta["awaitingLogs"] = buf->awaitingLogs;
      uint8_t startIdx = (buf->count < CLIENT_SERIAL_BUFFER_SIZE) ? 0 : buf->writeIndex;
      for (uint8_t i = 0; i < buf->count; ++i) {
        uint8_t idx = (startIdx + i) % CLIENT_SERIAL_BUFFER_SIZE;
        SerialLogEntry &entry = buf->entries[idx];
        if (entry.message[0] == '\0') {
          continue;
        }
        if (sinceEpoch > 0.0 && entry.timestamp <= sinceEpoch) {
          continue;
        }
        JsonObject row = logsArray.add<JsonObject>();
        row["timestamp"] = entry.timestamp;
        row["message"] = entry.message;
        row["level"] = entry.level;
        row["source"] = entry.source;
        row["client"] = buf->clientUid;
        added++;
        if (maxEntries > 0 && added >= maxEntries) {
          break;
        }
      }
    } else {
      meta["lastAckEpoch"] = 0;
      meta["awaitingLogs"] = false;
    }
  }

  respondJson(client, doc);
}

static void handleSerialLogsDownload(EthernetClient &client, const String &queryString) {
  String source = getQueryParam(queryString, "source");
  source.toLowerCase();
  if (source.length() == 0) {
    source = "server";
  }
  String clientUid = getQueryParam(queryString, "client");
  int maxEntries = SERIAL_DEFAULT_MAX_ENTRIES;
  String maxParam = getQueryParam(queryString, "max");
  if (maxParam.length()) {
    int requested = maxParam.toInt();
    if (requested > 0) {
      int cap = (source == "server") ? SERVER_SERIAL_BUFFER_SIZE : CLIENT_SERIAL_BUFFER_SIZE;
      maxEntries = min(requested, cap);
    }
  }
  double sinceEpoch = 0.0;
  String sinceParam = getQueryParam(queryString, "since");
  if (sinceParam.length()) {
    sinceEpoch = sinceParam.toDouble();
  }

  int added = 0;

  String filename;
  filename.reserve(72);
  filename += (source == "client" && clientUid.length()) ? clientUid : source;
  filename += F("-serial.csv");

  beginChunkedCsvDownload(client, filename);
  sendChunk(client, "timestamp,level,source,client,message\n", strlen("timestamp,level,source,client,message\n"));

  auto emitCsvLine = [&](const SerialLogEntry &entry, const char *clientLabel) {
    if (entry.message[0] == '\0') {
      return;
    }
    char line[640];
    size_t len = buildSerialLogCsvLine(line, sizeof(line), entry, clientLabel);
    if (len > 0) {
      sendChunk(client, line, len);
    }
  };

  if (source == "server") {
    uint8_t startIdx = (gServerSerial.count < SERVER_SERIAL_BUFFER_SIZE) ? 0 : gServerSerial.writeIndex;
    for (uint8_t i = 0; i < gServerSerial.count; ++i) {
      uint8_t idx = (startIdx + i) % SERVER_SERIAL_BUFFER_SIZE;
      SerialLogEntry &entry = gServerSerial.entries[idx];
      if (sinceEpoch > 0.0 && entry.timestamp <= sinceEpoch) {
        continue;
      }
      emitCsvLine(entry, "server");
      added++;
      if (maxEntries > 0 && added >= maxEntries) {
        break;
      }
    }
  } else if (source == "client" && clientUid.length() > 0) {
    for (uint8_t b = 0; b < gClientSerialBufferCount; ++b) {
      if (strcmp(gClientSerialBuffers[b].clientUid, clientUid.c_str()) == 0) {
        ClientSerialBuffer &buf = gClientSerialBuffers[b];
        uint8_t startIdx = (buf.count < CLIENT_SERIAL_BUFFER_SIZE) ? 0 : buf.writeIndex;
        for (uint8_t i = 0; i < buf.count; ++i) {
          uint8_t idx = (startIdx + i) % CLIENT_SERIAL_BUFFER_SIZE;
          SerialLogEntry &entry = buf.entries[idx];
          if (sinceEpoch > 0.0 && entry.timestamp <= sinceEpoch) {
            continue;
          }
          emitCsvLine(entry, buf.clientUid);
          added++;
          if (maxEntries > 0 && added >= maxEntries) {
            break;
          }
        }
        break;
      }
    }
  }

  endChunkedResponse(client);
}

static void handleSerialRequestPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }

  // Require PIN authentication for serial log requests
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  const char *clientUid = doc["client"].as<const char *>();
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, "Missing client UID");
    return;
  }
  // Validate client UID length to prevent buffer overflow in downstream operations
  if (strlen(clientUid) >= 48) {
    respondStatus(client, 400, "Client UID too long (max 47 chars)");
    return;
  }

  String error;
  SerialRequestResult result = requestClientSerialLogs(clientUid, error);
  if (result == SerialRequestResult::Sent) {
    respondStatus(client, 200, "OK");
    return;
  }

  if (result == SerialRequestResult::Throttled) {
    respondStatus(client, 429, error.length() ? error : F("Request throttled"));
  } else {
    respondStatus(client, 500, error.length() ? error : F("Failed to send request"));
  }
}

static void addServerSerialLog(const char *message, const char *level, const char *source) {
  if (!message || strlen(message) == 0) {
    return;
  }

  SerialLogEntry &entry = gServerSerial.entries[gServerSerial.writeIndex];
  entry.timestamp = currentEpoch();
  strlcpy(entry.message, message, sizeof(entry.message));
  strlcpy(entry.level, level ? level : "info", sizeof(entry.level));
  strlcpy(entry.source, source ? source : "server", sizeof(entry.source));

  gServerSerial.writeIndex = (gServerSerial.writeIndex + 1) % SERVER_SERIAL_BUFFER_SIZE;
  if (gServerSerial.count < SERVER_SERIAL_BUFFER_SIZE) {
    gServerSerial.count++;
  }
}

static ClientSerialBuffer *findOrCreateClientSerialBuffer(const char *clientUid) {
  if (!clientUid || strlen(clientUid) == 0) {
    return nullptr;
  }

  // Search for existing buffer
  for (uint8_t i = 0; i < gClientSerialBufferCount; ++i) {
    if (strcmp(gClientSerialBuffers[i].clientUid, clientUid) == 0) {
      return &gClientSerialBuffers[i];
    }
  }

  // Create new buffer if space available
  if (gClientSerialBufferCount < MAX_CLIENT_SERIAL_LOGS) {
    ClientSerialBuffer &buf = gClientSerialBuffers[gClientSerialBufferCount++];
    memset(&buf, 0, sizeof(ClientSerialBuffer));
    strlcpy(buf.clientUid, clientUid, sizeof(buf.clientUid));
    return &buf;
  }

  return nullptr;
}

static void addClientSerialLog(const char *clientUid, const char *message, double timestamp, const char *level, const char *source) {
  if (!message || strlen(message) == 0) {
    return;
  }

  ClientSerialBuffer *buf = findOrCreateClientSerialBuffer(clientUid);
  if (!buf) {
    return;
  }

  double serverEpoch = currentEpoch();
  SerialLogEntry &entry = buf->entries[buf->writeIndex];
  entry.timestamp = timestamp;
  strlcpy(entry.message, message, sizeof(entry.message));
  strlcpy(entry.level, level ? level : "info", sizeof(entry.level));
  strlcpy(entry.source, source ? source : clientUid, sizeof(entry.source));

  buf->writeIndex = (buf->writeIndex + 1) % CLIENT_SERIAL_BUFFER_SIZE;
  if (buf->count < CLIENT_SERIAL_BUFFER_SIZE) {
    buf->count++;
  }
  buf->lastLogEpoch = timestamp;
  buf->lastAckEpoch = serverEpoch;
  buf->awaitingLogs = false;
  strlcpy(buf->lastAckStatus, "logs_received", sizeof(buf->lastAckStatus));
}

static void handleSerialLog(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["client"] | "";
  if (!clientUid || strlen(clientUid) == 0) {
    return;
  }

  const char *defaultLevel = doc["level"] | "info";
  const char *defaultSource = doc["source"] | clientUid;

  // Handle single log entry or array of entries
  if (doc["message"]) {
    const char *message = doc["message"] | "";
    if (strlen(message) > 0) {
      const char *level = doc["level"] | defaultLevel;
      const char *source = doc["source"] | defaultSource;
      addClientSerialLog(clientUid, message, epoch, level, source);
    }
  } else if (doc["logs"]) {
    JsonArray logs = doc["logs"].as<JsonArray>();
    for (JsonVariant v : logs) {
      JsonObject logObj = v.as<JsonObject>();
      const char *message = logObj["message"] | "";
      double ts = logObj["timestamp"] | epoch;
      if (strlen(message) > 0) {
        const char *level = logObj["level"] | defaultLevel;
        const char *source = logObj["source"] | defaultSource;
        addClientSerialLog(clientUid, message, ts, level, source);
      }
    }
  }
}

static void handleSerialAck(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["client"] | "";
  if (!clientUid || strlen(clientUid) == 0) {
    return;
  }

  ClientSerialBuffer *buf = findOrCreateClientSerialBuffer(clientUid);
  if (!buf) {
    return;
  }

  const char *status = doc["status"] | "ack";
  strlcpy(buf->lastAckStatus, status, sizeof(buf->lastAckStatus));
  buf->lastAckEpoch = (epoch > 0.0) ? epoch : currentEpoch();
  
  // If we received an explicit ack, we can clear the awaiting flag
  // unless the status indicates otherwise (e.g. "processing")
  if (strcmp(status, "processing") != 0) {
    buf->awaitingLogs = false;
  }
}

// ============================================================================
// Relay Forward Handling (client-to-server-to-client)
// ============================================================================
// When a client alarm triggers relays on another client, the request is sent
// via relay_forward.qo → Route #1 → server relay_forward.qi. The server then
// re-issues the command via command.qo → Route #2 → target client relay.qi.

static void handleRelayForward(JsonDocument &doc, double epoch) {
  const char *targetClient = doc["target"] | "";
  const char *sourceClient = doc["client"] | "";

  if (!targetClient || strlen(targetClient) == 0) {
    Serial.println(F("Relay forward: missing target"));
    return;
  }

  uint8_t relayNum = doc["relay"] | 0;
  if (relayNum < 1 || relayNum > MAX_RELAYS) {
    Serial.print(F("Relay forward: invalid relay number "));
    Serial.println(relayNum);
    return;
  }

  bool state = doc["state"] | false;
  const char *source = doc["source"] | "client-alarm";

  Serial.print(F("Relay forward from "));
  Serial.print(sourceClient[0] ? sourceClient : "unknown");
  Serial.print(F(" -> "));
  Serial.print(targetClient);
  Serial.print(F(": Relay "));
  Serial.print(relayNum);
  Serial.print(F(" -> "));
  Serial.println(state ? "ON" : "OFF");

  if (!sendRelayCommand(targetClient, relayNum, state, source)) {
    Serial.println(F("Failed to forward relay command"));
  }
}

// ============================================================================
// Location Request/Response Handling
// ============================================================================
// Server can request GPS location from clients for NWS weather lookups

static void handleLocationResponse(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["client"] | "";
  if (!clientUid || strlen(clientUid) == 0) {
    return;
  }

  bool valid = doc["valid"] | false;
  if (!valid) {
    const char *error = doc["error"] | "unknown";
    Serial.print(F("Location response from "));
    Serial.print(clientUid);
    Serial.print(F(": invalid - "));
    Serial.println(error);
    return;
  }

  float latitude = doc["lat"].as<float>();
  float longitude = doc["lon"].as<float>();
  
  // Validate coordinates
  if (latitude < -90.0f || latitude > 90.0f || 
      longitude < -180.0f || longitude > 180.0f) {
    Serial.print(F("Location response from "));
    Serial.print(clientUid);
    Serial.println(F(": coordinates out of range"));
    return;
  }

  ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
  if (meta) {
    meta->latitude = latitude;
    meta->longitude = longitude;
    meta->locationEpoch = (epoch > 0.0) ? epoch : currentEpoch();
    // Invalidate cached NWS grid point if location changed
    meta->nwsGridValid = false;
    
    Serial.print(F("Location received from "));
    Serial.print(clientUid);
    Serial.print(F(": "));
    Serial.print(latitude, 4);
    Serial.print(F(", "));
    Serial.println(longitude, 4);
  }
}

/**
 * Stamp notefile schema version onto a J* body before sending.
 * Call this on each outbound note body for forward-compatibility detection.
 */
static inline void stampSchemaVersion(J *body) {
  JAddNumberToObject(body, "_sv", NOTEFILE_SCHEMA_VERSION);
}

static bool sendLocationRequest(const char *clientUid) {
  if (!clientUid || strlen(clientUid) == 0) {
    return false;
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    Serial.println(F("sendLocationRequest: Unable to allocate Notecard request"));
    return false;
  }

  // Use command.qo — Notehub Route reads _target and _type to deliver
  // as location_request.qi on the target client device
  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    Serial.println(F("sendLocationRequest: Unable to allocate body"));
    return false;
  }

  // Route Relay metadata
  JAddStringToObject(body, "_target", clientUid);
  JAddStringToObject(body, "_type", "location_request");
  JAddStringToObject(body, "request", "get_location");
  JAddNumberToObject(body, "timestamp", currentEpoch());
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);

  // Use requestAndResponse to capture Notecard errors for debugging
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println(F("sendLocationRequest: No response from Notecard"));
    return false;
  }

  const char *err = JGetString(rsp, "err");
  if (err && strlen(err) > 0) {
    Serial.print(F("sendLocationRequest: Notecard error: "));
    Serial.println(err);
    notecard.deleteResponse(rsp);
    return false;
  }
  notecard.deleteResponse(rsp);

  Serial.print(F("Location request sent to client: "));
  Serial.println(clientUid);
  return true;
}

static SerialRequestResult requestClientSerialLogs(const char *clientUid, String &errorMessage) {
  errorMessage = "";

  if (!clientUid || strlen(clientUid) == 0) {
    errorMessage = F("Missing client UID");
    return SerialRequestResult::NotecardFailure;
  }

  double now = currentEpoch();
  ClientSerialBuffer *buf = findOrCreateClientSerialBuffer(clientUid);
  if (buf && buf->awaitingLogs) {
    double sinceLastRequest = (buf->lastRequestEpoch > 0.0) ? (now - buf->lastRequestEpoch) : 0.0;
    if (sinceLastRequest < SERIAL_STALE_SECONDS) {
      errorMessage = F("Client log request already pending");
      return SerialRequestResult::Throttled;
    }
    buf->awaitingLogs = false;
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    errorMessage = F("Unable to allocate Notecard request");
    return SerialRequestResult::NotecardFailure;
  }

  // Use command.qo — Notehub Route reads _target and _type to deliver
  // as serial_request.qi on the target client device
  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    errorMessage = F("Unable to allocate Notecard body");
    return SerialRequestResult::NotecardFailure;
  }

  // Route Relay metadata
  JAddStringToObject(body, "_target", clientUid);
  JAddStringToObject(body, "_type", "serial_request");
  JAddStringToObject(body, "request", "send_logs");
  JAddNumberToObject(body, "timestamp", now);
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    errorMessage = F("No response from Notecard");
    return SerialRequestResult::NotecardFailure;
  }
  const char *err = JGetString(rsp, "err");
  if (err && err[0] != '\0') {
    errorMessage = err;
    notecard.deleteResponse(rsp);
    return SerialRequestResult::NotecardFailure;
  }
  notecard.deleteResponse(rsp);

  Serial.print(F("Serial log request sent to client: "));
  Serial.println(clientUid);
  addServerSerialLog("Serial log request sent", "info", "serial");

  if (buf) {
    buf->lastRequestEpoch = now;
    buf->awaitingLogs = true;
    strlcpy(buf->lastAckStatus, "request_sent", sizeof(buf->lastAckStatus));
  }

  return SerialRequestResult::Sent;
}

// ============================================================================
// Diagnostics Helpers
// ============================================================================

static void safeSleep(unsigned long ms) {
  if (ms == 0) {
    return;
  }

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  const unsigned long maxChunk = (WATCHDOG_TIMEOUT_SECONDS * 1000UL) / 2;
#else
  const unsigned long maxChunk = ms;
#endif

  unsigned long remaining = ms;
  while (remaining > 0) {
    unsigned long chunk = (remaining > maxChunk) ? maxChunk : remaining;
    delay(chunk);

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif

    remaining -= chunk;
  }
}

/**
 * Get current free heap bytes for field diagnostics.
 * Delegates to the shared tankalarm_freeRam() implementation.
 */
static uint32_t freeRam() { return tankalarm_freeRam(); }

static bool isNotehubDfuReady() {
  return gDfuUpdateAvailable && gDfuFirmwareLength > 0;
}

static bool isGitHubDirectUpdateReady() {
  return gGitHubUpdateAvailable && gGitHubAssetAvailable &&
         gGitHubAssetUrl[0] != '\0' && gGitHubAssetSize > 0 &&
         gGitHubAssetSha256[0] != '\0';
}

static bool isGitHubDirectTargetReady(const GitHubFirmwareTargetState &state) {
  return state.assetAvailable && state.assetUrl[0] != '\0' &&
         state.assetSize > 0 && state.assetSha256[0] != '\0';
}

static const char *firmwareTargetId(uint8_t target) {
  return (target == FIRMWARE_TARGET_FTPS_TEST) ? "ftps-test" : "server";
}

static const char *firmwareTargetLabel(uint8_t target) {
  return (target == FIRMWARE_TARGET_FTPS_TEST) ? "FTPS Test" : "TankAlarm Server";
}

static const char *firmwareTargetAssetNamingConvention(uint8_t target) {
  return (target == FIRMWARE_TARGET_FTPS_TEST)
             ? "TankAlarm-FTPS-Test-vX.Y.Z.bin"
             : "TankAlarm-Server-vX.Y.Z.bin";
}

static bool parseFirmwareTargetValue(const char *value, uint8_t &target) {
  target = FIRMWARE_TARGET_SERVER;
  if (value == nullptr || value[0] == '\0') {
    return true;
  }
  if (strcmp(value, "server") == 0 || strcmp(value, "main") == 0 ||
      strcmp(value, "main-server") == 0) {
    target = FIRMWARE_TARGET_SERVER;
    return true;
  }
  if (strcmp(value, "ftps") == 0 || strcmp(value, "ftps-test") == 0 ||
      strcmp(value, "ftps_test") == 0) {
    target = FIRMWARE_TARGET_FTPS_TEST;
    return true;
  }
  return false;
}

static void clearGitHubFirmwareTargetState(GitHubFirmwareTargetState &state, uint8_t target) {
  memset(&state, 0, sizeof(state));
  state.target = target;
}

static void setGitHubFirmwareTargetError(GitHubFirmwareTargetState &state, const char *message) {
  if (message == nullptr) {
    state.error[0] = '\0';
    return;
  }
  strlcpy(state.error, message, sizeof(state.error));
}

static bool buildExpectedFirmwareAssetName(uint8_t target,
                                           const char *version,
                                           char *out,
                                           size_t outSize) {
  if (version == nullptr || version[0] == '\0' || out == nullptr || outSize == 0) {
    return false;
  }

  int written = 0;
  if (target == FIRMWARE_TARGET_FTPS_TEST) {
    written = snprintf(out, outSize, "TankAlarm-FTPS-Test-v%s.bin", version);
  } else {
    written = snprintf(out, outSize, "TankAlarm-Server-v%s.bin", version);
  }

  return written > 0 && (size_t)written < outSize;
}

static void loadServerGitHubTargetState(GitHubFirmwareTargetState &state) {
  clearGitHubFirmwareTargetState(state, FIRMWARE_TARGET_SERVER);
  state.checked = (gLastGitHubCheckMs != 0UL);
  state.updateAvailable = gGitHubUpdateAvailable;
  state.assetAvailable = gGitHubAssetAvailable;
  strlcpy(state.latestVersion, gGitHubLatestVersion, sizeof(state.latestVersion));
  strlcpy(state.releaseUrl, gGitHubReleaseUrl, sizeof(state.releaseUrl));
  strlcpy(state.assetUrl, gGitHubAssetUrl, sizeof(state.assetUrl));
  state.assetSize = gGitHubAssetSize;
  strlcpy(state.assetSha256, gGitHubAssetSha256, sizeof(state.assetSha256));
}

static void copyGitHubFirmwareTargetStateToGlobals(const GitHubFirmwareTargetState &state) {
  gGitHubUpdateAvailable = state.updateAvailable;
  gGitHubAssetAvailable = state.assetAvailable;
  strlcpy(gGitHubLatestVersion, state.latestVersion, sizeof(gGitHubLatestVersion));
  strlcpy(gGitHubReleaseUrl, state.releaseUrl, sizeof(gGitHubReleaseUrl));
  strlcpy(gGitHubAssetUrl, state.assetUrl, sizeof(gGitHubAssetUrl));
  gGitHubAssetSize = state.assetSize;
  strlcpy(gGitHubAssetSha256, state.assetSha256, sizeof(gGitHubAssetSha256));
}

static int compareFirmwareVersions(const char *left, const char *right) {
  if (left == nullptr || left[0] == '\0') {
    return (right == nullptr || right[0] == '\0') ? 0 : -1;
  }
  if (right == nullptr || right[0] == '\0') {
    return 1;
  }

  while (left[0] != '\0' || right[0] != '\0') {
    unsigned long leftValue = 0;
    unsigned long rightValue = 0;

    while (left[0] != '\0' && left[0] != '.') {
      if (isdigit((unsigned char)left[0])) {
        leftValue = (leftValue * 10UL) + (unsigned long)(left[0] - '0');
      }
      ++left;
    }

    while (right[0] != '\0' && right[0] != '.') {
      if (isdigit((unsigned char)right[0])) {
        rightValue = (rightValue * 10UL) + (unsigned long)(right[0] - '0');
      }
      ++right;
    }

    if (leftValue < rightValue) {
      return -1;
    }
    if (leftValue > rightValue) {
      return 1;
    }

    if (left[0] == '.') {
      ++left;
    }
    if (right[0] == '.') {
      ++right;
    }
  }

  return 0;
}

static bool normalizeSha256Hex(const char *input, char *out, size_t outSize) {
  if (input == nullptr || out == nullptr || outSize < 65) {
    return false;
  }

  size_t count = 0;
  for (const char *cursor = input; *cursor != '\0'; ++cursor) {
    char ch = *cursor;
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      continue;
    }
    if (ch >= 'a' && ch <= 'f') {
      ch = (char)(ch - 'a' + 'A');
    }
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) {
      return false;
    }
    if (count >= 64) {
      return false;
    }
    out[count++] = ch;
  }

  if (count != 64) {
    return false;
  }

  out[count] = '\0';
  return true;
}

static bool parseGitHubAssetDigest(const char *input, char *out, size_t outSize) {
  if (input == nullptr || input[0] == '\0') {
    return false;
  }

  const char *hex = input;
  if (strncmp(input, "sha256:", 7) == 0 || strncmp(input, "SHA256:", 7) == 0) {
    hex = input + 7;
  }

  return normalizeSha256Hex(hex, out, outSize);
}

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
static bool writeUpperHexDigest(const unsigned char *digest,
                                size_t digestLen,
                                char *out,
                                size_t outLen) {
  if (digest == nullptr || out == nullptr || outLen < (digestLen * 2U) + 1U) {
    return false;
  }

  for (size_t index = 0; index < digestLen; ++index) {
    snprintf(out + (index * 2U), outLen - (index * 2U), "%02X", digest[index]);
  }

  out[digestLen * 2U] = '\0';
  return true;
}

static bool sha256Start(mbedtls_sha256_context &context) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x02070000)
  return mbedtls_sha256_starts_ret(&context, 0) == 0;
#else
  mbedtls_sha256_starts(&context, 0);
  return true;
#endif
}

static bool sha256Update(mbedtls_sha256_context &context, const uint8_t *data, size_t len) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x02070000)
  return mbedtls_sha256_update_ret(&context, data, len) == 0;
#else
  mbedtls_sha256_update(&context, data, len);
  return true;
#endif
}

static bool sha256Finish(mbedtls_sha256_context &context, unsigned char *digest) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x02070000)
  return mbedtls_sha256_finish_ret(&context, digest) == 0;
#else
  mbedtls_sha256_finish(&context, digest);
  return true;
#endif
}

static bool headerStartsWithIgnoreCase(const char *line, const char *prefix) {
  if (line == nullptr || prefix == nullptr) {
    return false;
  }

  while (*prefix != '\0') {
    if (*line == '\0') {
      return false;
    }

    char left = *line;
    char right = *prefix;
    if (left >= 'A' && left <= 'Z') {
      left = (char)(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = (char)(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }

    ++line;
    ++prefix;
  }

  return true;
}

static bool containsIgnoreCase(const char *text, const char *needle) {
  if (text == nullptr || needle == nullptr || needle[0] == '\0') {
    return false;
  }

  const size_t needleLen = strlen(needle);
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    size_t matched = 0;
    while (matched < needleLen && cursor[matched] != '\0') {
      char left = cursor[matched];
      char right = needle[matched];
      if (left >= 'A' && left <= 'Z') {
        left = (char)(left - 'A' + 'a');
      }
      if (right >= 'A' && right <= 'Z') {
        right = (char)(right - 'A' + 'a');
      }
      if (left != right) {
        break;
      }
      ++matched;
    }

    if (matched == needleLen) {
      return true;
    }
  }

  return false;
}

static bool parseHttpsUrl(const String &url,
                          String &host,
                          uint16_t &port,
                          String &path,
                          String &errorMessage) {
  host = "";
  path = "/";
  port = 443;
  errorMessage = "";

  if (!url.startsWith("https://")) {
    errorMessage = "Only HTTPS GitHub asset URLs are supported";
    return false;
  }

  int hostStart = 8;
  int pathStart = url.indexOf('/', hostStart);
  String authority = (pathStart >= 0) ? url.substring(hostStart, pathStart)
                                      : url.substring(hostStart);
  path = (pathStart >= 0) ? url.substring(pathStart) : "/";

  if (authority.length() == 0) {
    errorMessage = "GitHub asset URL is missing a host";
    return false;
  }

  int colon = authority.lastIndexOf(':');
  if (colon >= 0) {
    long parsedPort = authority.substring(colon + 1).toInt();
    if (parsedPort <= 0 || parsedPort > 65535) {
      errorMessage = "GitHub asset URL port is invalid";
      return false;
    }
    host = authority.substring(0, colon);
    port = (uint16_t)parsedPort;
  } else {
    host = authority;
  }

  if (host.length() == 0) {
    errorMessage = "GitHub asset URL is missing a host";
    return false;
  }

  return true;
}

static bool sendTlsRequest(TLSSocketWrapper &socket,
                           const char *data,
                           size_t len,
                           String &errorMessage) {
  size_t sent = 0;
  while (sent < len) {
    int result = socket.send(data + sent, len - sent);
    if (result <= 0) {
      errorMessage = "HTTPS request send failed";
      return false;
    }
    sent += (size_t)result;
  }
  return true;
}

static bool readHttpLine(TLSSocketWrapper &socket,
                         char *line,
                         size_t lineSize,
                         String &errorMessage) {
  if (line == nullptr || lineSize < 2) {
    errorMessage = "HTTP header buffer is invalid";
    return false;
  }

  size_t length = 0;
  while (length + 1 < lineSize) {
    char ch = 0;
    int result = socket.recv(&ch, 1);
    if (result != 1) {
      errorMessage = "HTTPS header read failed";
      return false;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      line[length] = '\0';
      return true;
    }
    line[length++] = ch;
  }

  line[lineSize - 1] = '\0';
  errorMessage = "HTTP header line exceeds buffer";
  return false;
}

static bool readHttpResponseHeaders(TLSSocketWrapper &socket,
                                    int &statusCode,
                                    String &location,
                                    uint32_t &contentLength,
                                    bool &hasContentLength,
                                    bool &chunked,
                                    String &errorMessage) {
  char line[GITHUB_DIRECT_HEADER_LINE_MAX] = {0};
  if (!readHttpLine(socket, line, sizeof(line), errorMessage)) {
    return false;
  }

  statusCode = 0;
  if (sscanf(line, "HTTP/%*d.%*d %d", &statusCode) != 1) {
    errorMessage = "Invalid HTTP status line from GitHub";
    return false;
  }

  location = "";
  contentLength = 0;
  hasContentLength = false;
  chunked = false;

  while (true) {
    if (!readHttpLine(socket, line, sizeof(line), errorMessage)) {
      return false;
    }
    if (line[0] == '\0') {
      return true;
    }

    if (headerStartsWithIgnoreCase(line, "Location:")) {
      const char *value = line + 9;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      location = value;
    } else if (headerStartsWithIgnoreCase(line, "Content-Length:")) {
      const char *value = line + 15;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      contentLength = (uint32_t)strtoul(value, nullptr, 10);
      hasContentLength = true;
    } else if (headerStartsWithIgnoreCase(line, "Transfer-Encoding:")) {
      const char *value = line + 18;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      chunked = containsIgnoreCase(value, "chunked");
    }
  }
}

static bool checkGitHubReleaseForTarget(uint8_t target,
                                        const char *currentVersion,
                                        uint8_t currentTarget,
                                        GitHubFirmwareTargetState &state,
                                        bool logResults) {
  clearGitHubFirmwareTargetState(state, target);

  J *req = notecard.newRequest("web.get");
  if (!req) {
    setGitHubFirmwareTargetError(state, "Failed to create GitHub release request");
    return false;
  }

  char url[128];
  snprintf(url,
           sizeof(url),
           "https://api.github.com/repos/%s/%s/releases/latest",
           GITHUB_REPO_OWNER,
           GITHUB_REPO_NAME);
  JAddStringToObject(req, "url", url);

  J *hdrs = JAddObjectToObject(req, "headers");
  if (hdrs) {
    char ua[48];
    snprintf(ua, sizeof(ua), "TankAlarm-Server/%s", FIRMWARE_VERSION);
    JAddStringToObject(hdrs, "User-Agent", ua);
    JAddStringToObject(hdrs, "Accept", "application/vnd.github.v3+json");
  }

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    setGitHubFirmwareTargetError(state, "GitHub release check returned no response");
    return false;
  }

  if (notecard.responseError(rsp)) {
    setGitHubFirmwareTargetError(state, JGetString(rsp, "err"));
    notecard.deleteResponse(rsp);
    return false;
  }

  int httpResult = JGetInt(rsp, "result");
  if (httpResult != 200) {
    char message[72];
    snprintf(message, sizeof(message), "GitHub Releases API returned HTTP %d", httpResult);
    setGitHubFirmwareTargetError(state, message);
    notecard.deleteResponse(rsp);
    return false;
  }

  const char *body = JGetString(rsp, "body");
  if (body == nullptr || body[0] == '\0') {
    setGitHubFirmwareTargetError(state, "GitHub release response body was empty");
    notecard.deleteResponse(rsp);
    return false;
  }

  StaticJsonDocument<512> filter;
  filter["tag_name"] = true;
  filter["html_url"] = true;
  for (uint8_t i = 0; i < 8; ++i) {
    filter["assets"][i]["name"] = true;
    filter["assets"][i]["browser_download_url"] = true;
    filter["assets"][i]["size"] = true;
    filter["assets"][i]["digest"] = true;
  }

  StaticJsonDocument<2560> doc;
  DeserializationError error = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  notecard.deleteResponse(rsp);

  if (error != DeserializationError::Ok) {
    char message[96];
    snprintf(message, sizeof(message), "GitHub release JSON parse failed: %s", error.c_str());
    setGitHubFirmwareTargetError(state, message);
    return false;
  }

  const char *tagName = doc["tag_name"];
  const char *htmlUrl = doc["html_url"];
  if (tagName == nullptr || tagName[0] == '\0') {
    setGitHubFirmwareTargetError(state, "GitHub release metadata did not include a tag");
    return false;
  }

  state.checked = true;
  const char *ghVersion = (tagName[0] == 'v' || tagName[0] == 'V') ? tagName + 1 : tagName;
  strlcpy(state.latestVersion, ghVersion, sizeof(state.latestVersion));
  if (htmlUrl != nullptr) {
    strlcpy(state.releaseUrl, htmlUrl, sizeof(state.releaseUrl));
  }

  char expectedAssetName[96];
  if (!buildExpectedFirmwareAssetName(target, ghVersion, expectedAssetName, sizeof(expectedAssetName))) {
    setGitHubFirmwareTargetError(state, "Failed to build expected firmware asset name");
    return false;
  }

  JsonArray assets = doc["assets"].as<JsonArray>();
  if (!assets.isNull()) {
    for (JsonObject asset : assets) {
      const char *assetName = asset["name"] | "";
      if (strcmp(assetName, expectedAssetName) != 0) {
        continue;
      }

      const char *assetUrl = asset["browser_download_url"] | "";
      if (assetUrl[0] != '\0') {
        state.assetAvailable = true;
        strlcpy(state.assetUrl, assetUrl, sizeof(state.assetUrl));
        state.assetSize = asset["size"] | 0;
        const char *assetDigest = asset["digest"] | "";
        if (!parseGitHubAssetDigest(assetDigest, state.assetSha256, sizeof(state.assetSha256))) {
          state.assetSha256[0] = '\0';
        }
      }
      break;
    }
  }

  const int versionComparison = compareFirmwareVersions(ghVersion, currentVersion);
  const bool targetMatchesCurrent = (target == currentTarget);
  state.updateAvailable = state.assetAvailable &&
                          (versionComparison > 0 ||
                           (versionComparison == 0 && !targetMatchesCurrent));

  if (logResults) {
    Serial.print(F("GitHub latest for "));
    Serial.print(firmwareTargetLabel(target));
    Serial.print(F(": "));
    Serial.print(ghVersion);
    Serial.print(F("  current package: "));
    Serial.println(currentVersion);

    if (state.updateAvailable) {
      addServerSerialLog("Selected firmware target is ready from GitHub", "info", "dfu");
    } else if (!state.assetAvailable) {
      addServerSerialLog("Latest GitHub release does not include the selected firmware asset", "warn", "dfu");
    }
  }

  return true;
}

static void buildSelectedFirmwareTargetStatus(const GitHubFirmwareTargetState &state,
                                              uint8_t currentTarget,
                                              const char *currentVersion,
                                              bool allowNotehubFallback,
                                              char *out,
                                              size_t outSize,
                                              bool &installEnabled) {
  const char *label = firmwareTargetLabel(state.target);
  const bool directReady = isGitHubDirectTargetReady(state);
  const bool directInstallAllowed = directReady && state.updateAvailable;
  installEnabled = directInstallAllowed || allowNotehubFallback;

  if (out == nullptr || outSize == 0) {
    return;
  }

  if (gDfuInProgress) {
    snprintf(out, outSize, "Firmware update in progress... (%s)", gDfuMode);
    return;
  }

  if (state.error[0] != '\0') {
    if (allowNotehubFallback) {
      snprintf(out,
               outSize,
               "%s GitHub check failed; Notehub DFU is still ready for the main server firmware",
               label);
    } else {
      strlcpy(out, state.error, outSize);
    }
    return;
  }

  if (!state.checked) {
    if (allowNotehubFallback) {
      snprintf(out, outSize, "%s is ready to install from Notehub DFU", label);
    } else {
      snprintf(out, outSize, "Select %s, then click Check for Update", label);
    }
    return;
  }

  if (directInstallAllowed) {
    snprintf(out,
             outSize,
             "%s v%s is ready to install from GitHub",
             label,
             state.latestVersion[0] != '\0' ? state.latestVersion : currentVersion);
    return;
  }

  if (directReady && state.target == currentTarget) {
    snprintf(out, outSize, "%s is already current (v%s)", label, currentVersion);
    return;
  }

  if (directReady && state.target != currentTarget) {
    snprintf(out,
             outSize,
             "Latest %s package (v%s) is older than the running firmware package",
             label,
             state.latestVersion[0] != '\0' ? state.latestVersion : currentVersion);
    return;
  }

  if (state.assetAvailable) {
    snprintf(out,
             outSize,
             "%s asset was found on GitHub, but its digest metadata is incomplete",
             label);
    return;
  }

  if (allowNotehubFallback) {
    snprintf(out,
             outSize,
             "No GitHub asset found for %s; Notehub DFU is still ready for the main server firmware",
             label);
    return;
  }

  if (state.latestVersion[0] != '\0') {
    snprintf(out,
             outSize,
             "Latest GitHub release v%s does not include a %s asset",
             state.latestVersion,
             label);
    return;
  }

  snprintf(out, outSize, "No published %s release asset is ready to install", label);
}

static bool attemptGitHubDirectInstallForTarget(const GitHubFirmwareTargetState &state,
                                                String &statusMessage) {
  const bool previousUpdateAvailable = gGitHubUpdateAvailable;
  const bool previousAssetAvailable = gGitHubAssetAvailable;
  const uint32_t previousAssetSize = gGitHubAssetSize;
  char previousLatestVersion[sizeof(gGitHubLatestVersion)] = {0};
  char previousReleaseUrl[sizeof(gGitHubReleaseUrl)] = {0};
  char previousAssetUrl[sizeof(gGitHubAssetUrl)] = {0};
  char previousAssetSha256[sizeof(gGitHubAssetSha256)] = {0};
  strlcpy(previousLatestVersion, gGitHubLatestVersion, sizeof(previousLatestVersion));
  strlcpy(previousReleaseUrl, gGitHubReleaseUrl, sizeof(previousReleaseUrl));
  strlcpy(previousAssetUrl, gGitHubAssetUrl, sizeof(previousAssetUrl));
  strlcpy(previousAssetSha256, gGitHubAssetSha256, sizeof(previousAssetSha256));

  copyGitHubFirmwareTargetStateToGlobals(state);
  const bool success = attemptGitHubDirectInstall(statusMessage);

  if (!success) {
    gGitHubUpdateAvailable = previousUpdateAvailable;
    gGitHubAssetAvailable = previousAssetAvailable;
    gGitHubAssetSize = previousAssetSize;
    strlcpy(gGitHubLatestVersion, previousLatestVersion, sizeof(gGitHubLatestVersion));
    strlcpy(gGitHubReleaseUrl, previousReleaseUrl, sizeof(gGitHubReleaseUrl));
    strlcpy(gGitHubAssetUrl, previousAssetUrl, sizeof(gGitHubAssetUrl));
    strlcpy(gGitHubAssetSha256, previousAssetSha256, sizeof(gGitHubAssetSha256));
  }

  return success;
}

static bool failGitHubDirectInstall(String &statusMessage, const String &message) {
  statusMessage = message;
  gDfuInProgress = false;
  strlcpy(gDfuMode, "error", sizeof(gDfuMode));
  strlcpy(gDfuError, statusMessage.c_str(), sizeof(gDfuError));
  Serial.print(F("GitHub Direct: "));
  Serial.println(statusMessage);
  addServerSerialLog(statusMessage.c_str(), "error", "dfu");
  return false;
}

static bool attemptGitHubDirectInstall(String &statusMessage) {
  return failGitHubDirectInstall(statusMessage,
                                 "GitHub Direct installation is disabled for safety. Please use Notehub OTA firmware update.");
}

/* Lines 3704-4109 omitted from compilation */
#if 0
static bool attemptGitHubDirectInstall_disabled(String &statusMessage) {

  String currentUrl = gGitHubAssetUrl;
  TLSSocketWrapper *activeTls = nullptr;
  TCPSocket *activeTcp = nullptr;
  uint32_t contentLength = 0;
  bool contentLengthKnown = false;

  for (int redirectCount = 0; redirectCount <= GITHUB_DIRECT_MAX_REDIRECTS; ++redirectCount) {
    String host;
    String path;
    String parseError;
    uint16_t port = 443;
    if (!parseHttpsUrl(currentUrl, host, port, path, parseError)) {
      return failGitHubDirectInstall(statusMessage,
                                     String("GitHub asset URL parse failed: ") + parseError);
    }

    SocketAddress address;
    if (network->gethostbyname(host.c_str(), &address) != NSAPI_ERROR_OK) {
      return failGitHubDirectInstall(statusMessage,
                                     String("DNS lookup failed for ") + host);
    }
    address.set_port(port);

    TCPSocket tcp;
    if (tcp.open(network) != NSAPI_ERROR_OK) {
      return failGitHubDirectInstall(statusMessage, "Failed to open TCP socket for GitHub download");
    }
    tcp.set_timeout(GITHUB_DIRECT_HTTP_TIMEOUT_MS);
    if (tcp.connect(address) != NSAPI_ERROR_OK) {
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("TCP connect failed for ") + host);
    }

    TLSSocketWrapper tls(&tcp, host.c_str(), TLSSocketWrapper::TRANSPORT_KEEP);
    mbedtls_ssl_config *sslConfig = tls.get_ssl_config();
    if (sslConfig == nullptr) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "TLS configuration was unavailable for GitHub download");
    }
    mbedtls_ssl_conf_authmode(sslConfig, MBEDTLS_SSL_VERIFY_NONE);
    tls.set_timeout(GITHUB_DIRECT_HTTP_TIMEOUT_MS);
    if (tls.connect() != NSAPI_ERROR_OK) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("TLS handshake failed for ") + host);
    }

    char userAgent[48];
    snprintf(userAgent, sizeof(userAgent), "TankAlarm-Server/%s", FIRMWARE_VERSION);

    if (!sendTlsRequest(tls, "GET ", 4, statusMessage) ||
        !sendTlsRequest(tls, path.c_str(), path.length(), statusMessage) ||
        !sendTlsRequest(tls, " HTTP/1.1\r\nHost: ", 17, statusMessage) ||
        !sendTlsRequest(tls, host.c_str(), host.length(), statusMessage) ||
        !sendTlsRequest(tls,
                        "\r\nUser-Agent: ",
                        14,
                        statusMessage) ||
        !sendTlsRequest(tls, userAgent, strlen(userAgent), statusMessage) ||
        !sendTlsRequest(tls,
                        "\r\nAccept: application/octet-stream\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                        84,
                        statusMessage)) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage, statusMessage);
    }

    int statusCode = 0;
    String location;
    bool chunked = false;
    if (!readHttpResponseHeaders(tls,
                                 statusCode,
                                 location,
                                 contentLength,
                                 contentLengthKnown,
                                 chunked,
                                 statusMessage)) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage, statusMessage);
    }

    if (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
        statusCode == 307 || statusCode == 308) {
      tls.close();
      tcp.close();
      if (location.length() == 0) {
        return failGitHubDirectInstall(statusMessage,
                                       "GitHub redirect response did not include a Location header");
      }
      currentUrl = location.startsWith("/") ? (String("https://") + host + location)
                                             : location;
      continue;
    }

    if (statusCode != 200) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("GitHub asset download returned HTTP ") + String(statusCode));
    }
    if (chunked) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "GitHub asset download used chunked transfer encoding; direct install requires Content-Length");
    }
    if (!contentLengthKnown) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "GitHub asset download did not include Content-Length");
    }
    if (contentLength != gGitHubAssetSize) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("GitHub asset size mismatch: expected ") +
                                             String(gGitHubAssetSize) + String(", got ") +
                                             String(contentLength));
    }

    activeTls = &tls;
    activeTcp = &tcp;

    mbed::FlashIAP flash;
    int flashResult = flash.init();
    if (flashResult != 0) {
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("FlashIAP init failed: ") + String(flashResult));
    }

    uint32_t flashStart = flash.get_flash_start();
    uint32_t flashSize = flash.get_flash_size();
    uint32_t pageSize = flash.get_page_size();
    uint32_t appStart = flashStart + 0x40000UL;

    if (gGitHubAssetSize > (flashStart + flashSize - appStart)) {
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "GitHub firmware image is too large for application flash region");
    }

    uint32_t eraseSize = 0;
    uint32_t eraseAddress = appStart;
    while (eraseSize < gGitHubAssetSize) {
      uint32_t sectorSize = flash.get_sector_size(eraseAddress + eraseSize);
      eraseSize += sectorSize;
    }

    Serial.print(F("GitHub Direct: erasing "));
    Serial.print(eraseSize / 1024UL);
    Serial.println(F("KB of application flash"));
    dfuKickWatchdog();
    flashResult = flash.erase(appStart, eraseSize);
    if (flashResult != 0) {
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("Flash erase failed: ") + String(flashResult));
    }

    const uint8_t eraseValue = flash.get_erase_value();
    uint32_t bufferSize = ((GITHUB_DIRECT_DOWNLOAD_CHUNK_SIZE + pageSize - 1U) / pageSize) * pageSize;
    uint8_t *programBuffer = (uint8_t *)malloc(bufferSize);
    if (programBuffer == nullptr) {
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to allocate GitHub Direct download buffer");
    }
    memset(programBuffer, eraseValue, bufferSize);

    mbedtls_sha256_context downloadSha;
    mbedtls_sha256_init(&downloadSha);
    if (!sha256Start(downloadSha)) {
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to initialize SHA-256 for GitHub download");
    }

    uint32_t downloaded = 0;
    uint32_t flashed = 0;
    uint32_t buffered = 0;
    uint32_t lastProgressPct = 0;
    while (downloaded < gGitHubAssetSize) {
      dfuKickWatchdog();
      size_t receiveSize = bufferSize - buffered;
      uint32_t remaining = gGitHubAssetSize - downloaded;
      if (receiveSize > remaining) {
        receiveSize = remaining;
      }

      int receiveResult = tls.recv(programBuffer + buffered, receiveSize);
      if (receiveResult <= 0) {
        mbedtls_sha256_free(&downloadSha);
        free(programBuffer);
        flash.deinit();
        tls.close();
        tcp.close();
        return failGitHubDirectInstall(statusMessage,
                                       "GitHub asset body read failed during firmware download");
      }

      if (!sha256Update(downloadSha, programBuffer + buffered, (size_t)receiveResult)) {
        mbedtls_sha256_free(&downloadSha);
        free(programBuffer);
        flash.deinit();
        tls.close();
        tcp.close();
        return failGitHubDirectInstall(statusMessage,
                                       "Failed to update SHA-256 digest during GitHub download");
      }

      buffered += (uint32_t)receiveResult;
      downloaded += (uint32_t)receiveResult;

      if (buffered == bufferSize || downloaded == gGitHubAssetSize) {
        uint32_t programSize = ((buffered + pageSize - 1U) / pageSize) * pageSize;
        if (programSize > buffered) {
          memset(programBuffer + buffered, eraseValue, programSize - buffered);
        }

        flashResult = flash.program(programBuffer, appStart + flashed, programSize);
        if (flashResult != 0) {
          mbedtls_sha256_free(&downloadSha);
          free(programBuffer);
          flash.deinit();
          tls.close();
          tcp.close();
          return failGitHubDirectInstall(statusMessage,
                                         String("Flash program failed: ") + String(flashResult));
        }

        flashed += buffered;
        buffered = 0;
        memset(programBuffer, eraseValue, bufferSize);
      }

      uint32_t pct = (downloaded * 100UL) / gGitHubAssetSize;
      if (pct >= lastProgressPct + 10UL) {
        lastProgressPct = pct;
        Serial.print(F("GitHub Direct: "));
        Serial.print(pct);
        Serial.print(F("% ("));
        Serial.print(downloaded);
        Serial.print(F("/"));
        Serial.print(gGitHubAssetSize);
        Serial.println(F(")"));
      }
    }

    unsigned char downloadDigest[32] = {0};
    if (!sha256Finish(downloadSha, downloadDigest)) {
      mbedtls_sha256_free(&downloadSha);
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to finalize GitHub download SHA-256 digest");
    }
    mbedtls_sha256_free(&downloadSha);

    char actualSha256[65] = {0};
    if (!writeUpperHexDigest(downloadDigest, sizeof(downloadDigest), actualSha256, sizeof(actualSha256))) {
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to format GitHub download SHA-256 digest");
    }
    if (strcmp(actualSha256, gGitHubAssetSha256) != 0) {
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("GitHub asset SHA-256 mismatch: expected ") +
                                             String(gGitHubAssetSha256) + String(", got ") +
                                             String(actualSha256));
    }

    Serial.println(F("GitHub Direct: verifying flashed SHA-256..."));
    mbedtls_sha256_context flashSha;
    mbedtls_sha256_init(&flashSha);
    if (!sha256Start(flashSha)) {
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to initialize SHA-256 for flash verification");
    }

    const uint8_t *flashPointer = (const uint8_t *)appStart;
    uint32_t verifyOffset = 0;
    while (verifyOffset < gGitHubAssetSize) {
      dfuKickWatchdog();
      uint32_t verifyChunk = gGitHubAssetSize - verifyOffset;
      if (verifyChunk > GITHUB_DIRECT_DOWNLOAD_CHUNK_SIZE) {
        verifyChunk = GITHUB_DIRECT_DOWNLOAD_CHUNK_SIZE;
      }
      if (!sha256Update(flashSha, flashPointer + verifyOffset, verifyChunk)) {
        mbedtls_sha256_free(&flashSha);
        free(programBuffer);
        flash.deinit();
        tls.close();
        tcp.close();
        return failGitHubDirectInstall(statusMessage,
                                       "Failed to update flash verification SHA-256 digest");
      }
      verifyOffset += verifyChunk;
    }

    unsigned char flashDigest[32] = {0};
    if (!sha256Finish(flashSha, flashDigest)) {
      mbedtls_sha256_free(&flashSha);
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to finalize flash verification SHA-256 digest");
    }
    mbedtls_sha256_free(&flashSha);

    char flashSha256[65] = {0};
    if (!writeUpperHexDigest(flashDigest, sizeof(flashDigest), flashSha256, sizeof(flashSha256))) {
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     "Failed to format flash verification SHA-256 digest");
    }
    if (strcmp(flashSha256, gGitHubAssetSha256) != 0) {
      free(programBuffer);
      flash.deinit();
      tls.close();
      tcp.close();
      return failGitHubDirectInstall(statusMessage,
                                     String("Flash SHA-256 mismatch after programming: expected ") +
                                             String(gGitHubAssetSha256) + String(", got ") +
                                             String(flashSha256));
    }

    free(programBuffer);
    flash.deinit();
    tls.close();
    tcp.close();

    Serial.println(F("========================================"));
    Serial.println(F("GitHub Direct: UPDATE COMPLETE - REBOOTING"));
    Serial.println(F("========================================"));
    addServerSerialLog("GitHub Direct update complete; rebooting", "info", "dfu");
    Serial.flush();
    safeSleep(500);
    NVIC_SystemReset();
    return true;
  }

  return failGitHubDirectInstall(statusMessage,
                                 "GitHub asset redirect chain exceeded the supported limit");
}
#endif // Close our #if 0 block
#endif // Close the platform block

static void attemptAutoGitHubInstall() {
  if (!gGitHubUpdateAvailable) {
    return;
  }

  if (isGitHubDirectUpdateReady()) {
    String autoStatus;
    if (!attemptGitHubDirectInstall(autoStatus) && isNotehubDfuReady()) {
      Serial.println(F("GitHub Direct install failed; falling back to Notehub DFU"));
      enableDfuMode();
    }
    return;
  }

  if (isNotehubDfuReady()) {
    Serial.println(F("GitHub Direct metadata not ready; falling back to Notehub DFU"));
    addServerSerialLog("GitHub Direct metadata incomplete; using Notehub DFU", "warn", "dfu");
    enableDfuMode();
  } else {
    Serial.println(F("GitHub update available but no installable asset is ready yet"));
    addServerSerialLog("GitHub update available but installable asset is not ready", "warn", "dfu");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {
    safeSleep(10);
  }

  Serial.println();
  Serial.print(F("Tank Alarm Server 112025 v"));
  Serial.print(F(FIRMWARE_VERSION));
  Serial.print(F(" ("));
  Serial.print(F(FIRMWARE_BUILD_DATE));
  Serial.println(F(")"));

  // Initialize serial log buffers
  memset(&gServerSerial, 0, sizeof(ServerSerialBuffer));
  gClientSerialBufferCount = 0;
  
  // Initialize hash table for sensor lookups
  initSensorHashTable();

  initializeStorage();

  // Clean up orphaned .tmp files from interrupted atomic writes
#ifdef TANKALARM_POSIX_FILE_IO_AVAILABLE
  {
    static const char * const tmpCleanupPaths[] = {
      "/fs/server_config.json",
      "/fs/server_heartbeat.json",
      "/fs/client_config_cache.txt",
      "/fs/history_settings.json",
      "/fs/history/hot_tier.json",
      "/fs/contacts_config.json",
      "/fs/email_format.json",
      "/fs/calibration_data.txt",
      "/fs/calibration_log.txt"
    };
    tankalarm_posix_cleanup_tmp_files(tmpCleanupPaths,
      sizeof(tmpCleanupPaths) / sizeof(tmpCleanupPaths[0]));
  }
#endif

  ensureConfigLoaded();
  gLastHeartbeatFileEpoch = loadServerHeartbeatEpoch();
  loadCalibrationData();  // Load calibration learning data
  loadSensorRegistry();     // Restore sensor records from LittleFS
  loadHistorySettings();  // Restore history tier settings from LittleFS
  loadHotTierSnapshot();  // Restore hot tier ring buffer from LittleFS (survive reboot)
  loadClientMetadataCache();  // Restore client metadata from LittleFS
  printHardwareRequirements();

  Wire.begin();
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);  // Guard against indefinite blocking on bus hang

  // I2C bus scan: verify Notecard is present
  {
    const uint8_t expectedAddrs[] = { NOTECARD_I2C_ADDRESS };
    const char *expectedNames[] = { "Notecard" };
    tankalarm_scanI2CBus(expectedAddrs, expectedNames, 1);
  }

  initializeNotecard();
#if defined(TANKALARM_DFU_MCUBOOT)
  tankalarm_resolvePendingOta(notecard);
#endif
  ensureTimeSync();
  scheduleNextDailyEmail();
  if (gConfig.viewerEnabled) {
    scheduleNextViewerSummary();
  }

  initializeEthernet();
  gWebServer.begin();

  if (gConfig.ftpEnabled && gConfig.ftpRestoreOnBoot) {
    char err[128];
    if (performFtpRestore(err, sizeof(err))) {
      Serial.println(F("FTP restore on boot completed"));
      ensureConfigLoaded();
      loadClientConfigSnapshots();
      loadCalibrationData();
      scheduleNextDailyEmail();
      if (gConfig.viewerEnabled) {
        scheduleNextViewerSummary();
      }
    } else {
      Serial.print(F("FTP restore on boot skipped/failed: "));
      Serial.println(err);
    }
  }

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Initialize watchdog timer
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    uint32_t timeoutMs = WATCHDOG_TIMEOUT_SECONDS * 1000;
    if (mbedWatchdog.start(timeoutMs)) {
      Serial.print(F("Mbed Watchdog enabled: "));
      Serial.print(WATCHDOG_TIMEOUT_SECONDS);
      Serial.println(F(" seconds"));
    } else {
      Serial.println(F("Warning: Watchdog initialization failed"));
    }
  #else
    IWatchdog.begin(WATCHDOG_TIMEOUT_SECONDS * 1000000UL);
    Serial.print(F("Watchdog timer enabled: "));
    Serial.print(WATCHDOG_TIMEOUT_SECONDS);
    Serial.println(F(" seconds"));
  #endif
#else
  Serial.println(F("Warning: Watchdog timer not available on this platform"));
#endif

  tankalarm_printHeapStats();
  printMemoryBudget();

  Serial.println(F("Server setup complete"));
  Serial.println(F("----------------------------------"));
  Serial.print(F("Local IP Address: "));
  Serial.println(Ethernet.localIP());
  Serial.print(F("Gateway: "));
  Serial.println(Ethernet.gatewayIP());
  Serial.print(F("Subnet Mask: "));
  Serial.println(Ethernet.subnetMask());
  Serial.println(F("----------------------------------"));
  
  // Initial voltage and DFU check
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Reset watchdog before blocking Notecard I2C calls — card.voltage and
  // dfu.status can each block for several seconds if the modem is busy.
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
  checkServerVoltage();
  checkForFirmwareUpdate();
  
  // Dedup sensor records after all boot-time data sources have loaded.
  // Catches duplicates from corrupted registry files or Notecard note backlogs.
  deduplicateSensorRecordsLinear();
  
  addServerSerialLog("Server started", "info", "lifecycle");
}

void loop() {
#if defined(TANKALARM_DFU_MCUBOOT)
  // 15.6 Server Offline-Safe Local Health Gate
  if (isStorageAvailable() && (gNotecardAvailable || Ethernet.linkStatus() == LinkON)) {
    tankalarm_markFirmwareHealthy();
  }
#endif

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Reset watchdog timer to prevent system reset
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif

  // Maintain DHCP lease and check link status
  Ethernet.maintain();

  // Check for link state changes and display IP when link comes up
  unsigned long now = millis();
  if (now - gLastLinkCheckMillis > 5000UL) {
    gLastLinkCheckMillis = now;
    bool linkUp = (Ethernet.linkStatus() == LinkON);
    if (linkUp && !gLastLinkState) {
      // Link just came up
      Serial.println(F("----------------------------------"));
      Serial.println(F("Network link established!"));
      Serial.print(F("Local IP Address: "));
      Serial.println(Ethernet.localIP());
      Serial.print(F("Gateway: "));
      Serial.println(Ethernet.gatewayIP());
      Serial.print(F("Subnet Mask: "));
      Serial.println(Ethernet.subnetMask());
      Serial.println(F("----------------------------------"));
    } else if (!linkUp && gLastLinkState) {
      // Link just went down
      Serial.println(F("WARNING: Network link lost!"));
    }
    gLastLinkState = linkUp;
  }

  handleWebRequests();

  // ---- Notecard I2C health check (with exponential backoff) ----
  {
    static unsigned long lastNcHealthCheck = 0;
    static unsigned long ncHealthInterval = NOTECARD_HEALTH_CHECK_BASE_INTERVAL_MS;
    if (!gNotecardAvailable && (now - lastNcHealthCheck > ncHealthInterval)) {
      lastNcHealthCheck = now;
      J *hcReq = notecard.newRequest("card.version");
      if (hcReq) {
        J *hcRsp = notecard.requestAndResponse(hcReq);
        if (hcRsp) {
          notecard.deleteResponse(hcRsp);
          gNotecardAvailable = true;
          gNotecardFailureCount = 0;
          gLastSuccessfulNotecardComm = millis();
          tankalarm_ensureNotecardBinding(notecard);
          ncHealthInterval = NOTECARD_HEALTH_CHECK_BASE_INTERVAL_MS;
          Serial.println(F("Notecard recovered - online (backoff reset)"));
        } else {
          gNotecardFailureCount++;
          if (gNotecardFailureCount >= I2C_NOTECARD_RECOVERY_THRESHOLD) {
            tankalarm_recoverI2CBus(gDfuInProgress, [](){
              #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
                mbedWatchdog.kick();
              #endif
            });
            Serial.print(F("I2C recovery event (trigger=HEALTH_CHECK, count="));
            Serial.print(gI2cBusRecoveryCount);
            Serial.println(F(")"));
            tankalarm_ensureNotecardBinding(notecard);
            gNotecardFailureCount = 0;
          }
          // Exponential backoff up to max
          if (ncHealthInterval < NOTECARD_HEALTH_CHECK_MAX_INTERVAL_MS) {
            ncHealthInterval *= 2;
            if (ncHealthInterval > NOTECARD_HEALTH_CHECK_MAX_INTERVAL_MS) {
              ncHealthInterval = NOTECARD_HEALTH_CHECK_MAX_INTERVAL_MS;
            }
          }
          Serial.print(F("Notecard health check backoff: next in "));
          Serial.print(ncHealthInterval / 60000UL);
          Serial.println(F(" min"));
        }
      }
    }
  }

  now = millis();
  if (now - gLastPollMillis > 5000UL) {
    gLastPollMillis = now;
    if (!gPaused) {
      pollNotecard();
    }
  }

  // Auto-retry pending config dispatches every 60 minutes
  static unsigned long gLastPendingDispatchMillis = 0;
  if (!gPaused && (now - gLastPendingDispatchMillis > 3600000UL)) {
    gLastPendingDispatchMillis = now;
    dispatchPendingConfigs();
  }

  ensureTimeSync();

  double nowEpoch = currentEpoch();
  if (nowEpoch > 0.0) {
    if (!gServerDownChecked) {
      if (gLastHeartbeatFileEpoch > 0.0) {
        double offlineSeconds = nowEpoch - gLastHeartbeatFileEpoch;
        if (offlineSeconds >= SERVER_DOWN_THRESHOLD_SECONDS && gConfig.serverDownSmsEnabled) {
          char message[160];
          double hours = offlineSeconds / 3600.0;
          snprintf(message, sizeof(message),
                   "Server power loss detected. Offline for %.1f hours.", hours);
          sendSmsAlert(message);
          addServerSerialLog("Server down alert dispatched", "warn", "lifecycle");
          Serial.print(F("Server down alert sent: "));
          Serial.println(message);
        }
      }
      gServerDownChecked = true;
    }

    if (gLastHeartbeatPersistEpoch <= 0.0 || (nowEpoch - gLastHeartbeatPersistEpoch) >= SERVER_HEARTBEAT_INTERVAL_SECONDS) {
      if (saveServerHeartbeatEpoch(nowEpoch)) {
        gLastHeartbeatPersistEpoch = nowEpoch;
      }
    }
  }

  if (gNextDailyEmailEpoch > 0.0 && currentEpoch() >= gNextDailyEmailEpoch) {
    sendDailyEmail();
    scheduleNextDailyEmail();
  }

  if (gConfig.viewerEnabled && gNextViewerSummaryEpoch > 0.0 && currentEpoch() >= gNextViewerSummaryEpoch) {
    publishViewerSummary();
    scheduleNextViewerSummary();
  }
  
  // Periodic history maintenance - prune old data and archive to FTP
  static unsigned long lastHistoryMaintenance = 0;
  const unsigned long HISTORY_MAINTENANCE_INTERVAL = 3600000UL;  // 1 hour
  if (now - lastHistoryMaintenance > HISTORY_MAINTENANCE_INTERVAL) {
    lastHistoryMaintenance = now;
    double epoch = currentEpoch();
    
    // Prune hot tier data older than retention period
    pruneHotTierIfNeeded();
    
    // Roll up yesterday's hot tier snapshots into LittleFS daily summaries (warm tier)
    rollupDailySummaries();
    
    // Prune daily summary files older than retention period
    pruneDailySummaryFiles();
    
    // Persist hot tier ring buffer to flash (survive reboot)
    saveHotTierSnapshot();
    
    // Check if we need to archive last month to FTP (cold tier)
    if (gHistorySettings.ftpArchiveEnabled && gConfig.ftpEnabled) {
      time_t nowTime = (time_t)epoch;
      struct tm *nowTm = gmtime(&nowTime);
      if (nowTm && nowTm->tm_hour == gHistorySettings.ftpSyncHour) {
        // Get previous month
        int archiveYear = nowTm->tm_year + 1900;
        int archiveMonth = nowTm->tm_mon;  // Current month (0-11), so this is prev month
        if (archiveMonth == 0) {
          archiveMonth = 12;
          archiveYear--;
        }
        archiveMonthToFtp(archiveYear, archiveMonth);
      }
    }
    
    // Persist history settings after maintenance
    saveHistorySettings();
  }
  
  // Periodic firmware update check via Notecard DFU
  if (now - gLastDfuCheckMillis > DFU_CHECK_INTERVAL_MS) {
    gLastDfuCheckMillis = now;
    if (!gDfuInProgress) {
      checkForFirmwareUpdate();
      if (gDfuUpdateAvailable && gConfig.updatePolicy == UPDATE_POLICY_AUTO_DFU) {
        enableDfuMode();
      }
    }
  }

  // Periodic GitHub release check (60s after boot, then every 24 hours)
  if (gConfig.updatePolicy == UPDATE_POLICY_ALERT_GITHUB ||
      gConfig.updatePolicy == UPDATE_POLICY_AUTO_GITHUB) {
    const unsigned long GITHUB_BOOT_DELAY_MS = 60000UL;
    if (!gGitHubBootCheckDone && (now >= GITHUB_BOOT_DELAY_MS) && gNotecardAvailable) {
      gGitHubBootCheckDone = true;
      gLastGitHubCheckMs = now;
      checkGitHubForUpdate();
      if (gConfig.updatePolicy == UPDATE_POLICY_AUTO_GITHUB && gGitHubUpdateAvailable) {
        attemptAutoGitHubInstall();
      }
    } else if (gGitHubBootCheckDone && gNotecardAvailable &&
               (now - gLastGitHubCheckMs) >= GITHUB_CHECK_INTERVAL_MS) {
      gLastGitHubCheckMs = now;
      checkGitHubForUpdate();
      if (gConfig.updatePolicy == UPDATE_POLICY_AUTO_GITHUB && gGitHubUpdateAvailable) {
        attemptAutoGitHubInstall();
      }
    }
  }

  // Periodic voltage check (Notecard supply + optional analog Vin)
  if (now - gLastVoltageCheckMillis > VOLTAGE_CHECK_INTERVAL_MS) {
    gLastVoltageCheckMillis = now;
    checkServerVoltage();
  }
  
  // Periodic sensor registry save (when dirty, every 5 minutes)
  if (gSensorRegistryDirty && (now - gLastRegistrySaveMillis > REGISTRY_SAVE_INTERVAL_MS)) {
    gLastRegistrySaveMillis = now;
    saveSensorRegistry();
    gSensorRegistryDirty = false;
  }
  
  // Periodic client metadata save (when dirty, piggyback on same interval)
  if (gClientMetadataDirty && (now - gLastRegistrySaveMillis > REGISTRY_SAVE_INTERVAL_MS)) {
    saveClientMetadataCache();
    gClientMetadataDirty = false;
  }
  
  // Periodic stale client check (every hour)
  if (now - gLastStaleCheckMillis > STALE_CHECK_INTERVAL_MS) {
    gLastStaleCheckMillis = now;
    checkStaleClients();
  }

  if (gConfigDirty) {
    if (saveConfig(gConfig)) {
      gConfigDirty = false;
      if (gConfig.ftpEnabled && gConfig.ftpBackupOnChange) {
        gPendingFtpBackup = true;
      }
    }
  }

#ifndef FTP_AUTO_TEST_ON_BOOT
#define FTP_AUTO_TEST_ON_BOOT 0
#endif
#if FTP_AUTO_TEST_ON_BOOT
  {
    static bool sAutoBackupFired = false;
    static unsigned long sAutoBackupArmMs = 0;
    if (!sAutoBackupFired) {
      if (sAutoBackupArmMs == 0 && Ethernet.linkStatus() == LinkON && gConfig.ftpEnabled) {
        sAutoBackupArmMs = now;
        Serial.println(F("[ftp-auto-test] armed — backup will trigger in 20s"));
      }
      if (sAutoBackupArmMs != 0 && now - sAutoBackupArmMs > 20000UL && !gBackupInProgress && !gPendingFtpBackup) {
        Serial.println(F("[ftp-auto-test] firing one-shot FTP backup"));
        gPendingFtpBackup = true;
        sAutoBackupFired = true;
      }
    }
  }
#endif

  if (gPendingFtpBackup) {
    char error[128];
    gBackupInProgress = true;
    gWebServer.end();  // Free LISTEN PCB so FTPS data sockets have headroom (Opta LWIP pool=4)
    // SPEED-OPT (background backup): performFtpBackup is synchronous and
    // currently blocks the main loop for ~N*65 seconds. Moving it to a
    // ticker that runs one file per loop pass would let the device keep
    // serving telemetry/web requests during a long backup. See
    // CODE REVIEW/MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md item 3.
    bool ok = performFtpBackup(error, sizeof(error));
    gWebServer.begin();
    gBackupInProgress = false;
    gPendingFtpBackup = false;
    if (!ok) {
      Serial.print(F("FTP auto-backup failed: "));
      Serial.println(error);
    }
  }
}

/**
 * Recover from interrupted atomic writes at boot.
 * Called once during initializeStorage() after filesystem mount.
 *
 * If a .tmp file exists but the target does NOT, the rename failed
 * after a write completed — complete the rename now.
 * If BOTH exist, the original is still valid — delete stale .tmp.
 */
#ifdef POSIX_FILE_IO_AVAILABLE
static void recoverOrphanedTmpFiles() {
  static const char * const criticalFiles[] = {
    "/fs/server_config.json",
    "/fs/contacts_config.json",
    "/fs/sensor_registry.json",
    "/fs/client_metadata.json",
    "/fs/client_config_cache.txt",
    "/fs/calibration_data.txt",
    "/fs/email_format.json",
    "/fs/history_settings.json",
    "/fs/server_heartbeat.json",
    "/fs/history/hot_tier.json",
    "/fs/archived_clients.json",
    nullptr
  };

  for (int i = 0; criticalFiles[i] != nullptr; ++i) {
    char tmpPath[256];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", criticalFiles[i]);

    if (tankalarm_posix_file_exists(tmpPath)) {
      if (!tankalarm_posix_file_exists(criticalFiles[i])) {
        // Target missing + tmp exists → rename was interrupted; complete it
        if (rename(tmpPath, criticalFiles[i]) == 0) {
          Serial.print(F("Recovered config from .tmp: "));
          Serial.println(criticalFiles[i]);
        } else {
          Serial.print(F("ERROR: Could not recover: "));
          Serial.println(criticalFiles[i]);
        }
      } else {
        // Both exist → original is valid; clean up stale tmp
        remove(tmpPath);
        #ifdef DEBUG_MODE
        Serial.print(F("Cleaned stale .tmp: "));
        Serial.println(tmpPath);
        #endif
      }
    }
  }
}
#endif // POSIX_FILE_IO_AVAILABLE

static void initializeStorage() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Mbed OS LittleFileSystem initialization
    mbedBD = BlockDevice::get_default_instance();
    if (!mbedBD) {
      Serial.println(F("Error: No default block device found"));
      Serial.println(F("Warning: Filesystem not available - configuration will not persist"));
      return;
    }
    
    mbedFS = new LittleFileSystem("fs");
    int err = mbedFS->mount(mbedBD);
    if (err) {
      // Try to reformat if mount fails
      Serial.println(F("Filesystem mount failed, attempting to reformat..."));
      err = mbedFS->reformat(mbedBD);
      if (err) {
        Serial.println(F("LittleFS format failed; continuing without filesystem"));
        delete mbedFS;
        mbedFS = nullptr;
      }
    }
    if (mbedFS) {
      Serial.println(F("Mbed OS LittleFileSystem initialized"));
      // Recover from any interrupted atomic writes (power loss during rename)
      recoverOrphanedTmpFiles();
    }
  #else
    // STM32duino LittleFS
    if (!LittleFS.begin()) {
      Serial.println(F("LittleFS init failed; continuing without filesystem"));
    }
  #endif
#else
  Serial.println(F("Warning: Filesystem not available on this platform - configuration will not persist"));
#endif
}

static void ensureConfigLoaded() {
  if (!loadConfig(gConfig)) {
    createDefaultConfig(gConfig);
    saveConfig(gConfig);
    Serial.println(F("Default server configuration created"));
    gPaused = true;  // Start paused on fresh install to allow safe setup/restore
  }

  loadClientConfigSnapshots();
}

static void createDefaultConfig(ServerConfig &cfg) {
  memset(&cfg, 0, sizeof(ServerConfig));
  strlcpy(cfg.serverName, "Tank Alarm Server", sizeof(cfg.serverName));
  strlcpy(cfg.serverFleet, "tankalarm-server", sizeof(cfg.serverFleet));
  strlcpy(cfg.clientFleet, "tankalarm-clients", sizeof(cfg.clientFleet));
  strlcpy(cfg.productUid, DEFAULT_SERVER_PRODUCT_UID, sizeof(cfg.productUid));  // From ServerConfig.h or Server Settings web page
  strlcpy(cfg.smsPrimary, "+15555555555", sizeof(cfg.smsPrimary));
  strlcpy(cfg.smsSecondary, "+15555555555", sizeof(cfg.smsSecondary));
  strlcpy(cfg.dailyEmail, "reports@example.com", sizeof(cfg.dailyEmail));
  strlcpy(cfg.configPin, DEFAULT_ADMIN_PIN, sizeof(cfg.configPin));
  cfg.dailyHour = DAILY_EMAIL_HOUR_DEFAULT;
  cfg.dailyMinute = DAILY_EMAIL_MINUTE_DEFAULT;
  cfg.webRefreshSeconds = 21600;
  cfg.useStaticIp = false;  // Use DHCP by default for easier deployment
  cfg.smsOnHigh = true;
  cfg.smsOnLow = true;
  cfg.smsOnClear = false;
  cfg.serverDownSmsEnabled = true;
  cfg.viewerEnabled = false;  // Viewer device disabled by default
  cfg.updatePolicy = UPDATE_POLICY_DISABLED;  // No update checking by default
  cfg.checkClientVersionAlerts = true;
  cfg.checkViewerVersionAlerts = true;
  cfg.ftpEnabled = false;
  cfg.ftpPassive = true;
  cfg.ftpBackupOnChange = false;
  cfg.ftpRestoreOnBoot = false;
  cfg.ftpPort = FTP_PORT_DEFAULT;
  strlcpy(cfg.ftpHost, "", sizeof(cfg.ftpHost));
  strlcpy(cfg.ftpUser, "", sizeof(cfg.ftpUser));
  strlcpy(cfg.ftpPass, "", sizeof(cfg.ftpPass));
  strlcpy(cfg.ftpPath, FTP_PATH_DEFAULT, sizeof(cfg.ftpPath));
  cfg.ftpsEnabled = false;
  cfg.ftpsTrustMode = 0;
  cfg.ftpsFingerprint[0] = '\0';
  cfg.ftpsTlsServerName[0] = '\0';
}

static void buildFtpCredentialKey(const char *productUid, const char *configPin, char *out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }

  const char *uidPart = (productUid && productUid[0] != '\0') ? productUid : "tankalarm";
  const char *pinPart = (configPin && isValidPin(configPin)) ? configPin : DEFAULT_ADMIN_PIN;
  snprintf(out, outSize, "%s|%s|ftp", uidPart, pinPart);
}

static char ftpCredentialHexDigit(uint8_t value) {
  value &= 0x0F;
  return (value < 10) ? (char)('0' + value) : (char)('A' + (value - 10));
}

static int ftpCredentialHexValue(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return 10 + (value - 'a');
  }
  if (value >= 'A' && value <= 'F') {
    return 10 + (value - 'A');
  }
  return -1;
}

static bool encodeFtpCredential(const char *plainText, const char *productUid, const char *configPin, char *encoded, size_t encodedSize) {
  if (!encoded || encodedSize == 0) {
    return false;
  }

  encoded[0] = '\0';
  if (!plainText || plainText[0] == '\0') {
    return true;
  }

  // Must be large enough for the longest obfuscated field: the 64-char
  // SHA-256 FTPS fingerprint plus the 4-char "TA1:" prefix + NUL (69 bytes).
  // Bumping from the legacy 40-byte size prevents silent saveConfig failures
  // when an FTPS fingerprint is configured.
  static const char kPrefix[] = "TA1:";
  char payload[96];
  int payloadLen = snprintf(payload, sizeof(payload), "%s%s", kPrefix, plainText);
  if (payloadLen <= 0 || (size_t)payloadLen >= sizeof(payload)) {
    return false;
  }

  char key[96];
  buildFtpCredentialKey(productUid, configPin, key, sizeof(key));
  size_t keyLen = strlen(key);
  if (keyLen == 0 || encodedSize < ((size_t)payloadLen * 2U) + 1U) {
    return false;
  }

  for (size_t i = 0; i < (size_t)payloadLen; ++i) {
    uint8_t obfuscated = ((uint8_t)payload[i]) ^ ((uint8_t)key[i % keyLen]);
    encoded[(i * 2U)] = ftpCredentialHexDigit((uint8_t)(obfuscated >> 4));
    encoded[(i * 2U) + 1U] = ftpCredentialHexDigit(obfuscated);
  }
  encoded[(size_t)payloadLen * 2U] = '\0';
  return true;
}

static bool decodeFtpCredential(const char *encoded, const char *productUid, const char *configPin, char *plainText, size_t plainTextSize) {
  if (!plainText || plainTextSize == 0) {
    return false;
  }

  plainText[0] = '\0';
  if (!encoded || encoded[0] == '\0') {
    return true;
  }

  size_t encodedLen = strlen(encoded);
  if ((encodedLen & 1U) != 0U) {
    return false;
  }

  // Must match the encode-side buffer size so a 64-char FTPS fingerprint
  // round-trips successfully (64 + "TA1:" + NUL).
  static const char kPrefix[] = "TA1:";
  char payload[96];
  size_t payloadLen = encodedLen / 2U;
  if (payloadLen <= strlen(kPrefix) || payloadLen >= sizeof(payload)) {
    return false;
  }

  char key[96];
  buildFtpCredentialKey(productUid, configPin, key, sizeof(key));
  size_t keyLen = strlen(key);
  if (keyLen == 0) {
    return false;
  }

  for (size_t i = 0; i < payloadLen; ++i) {
    int high = ftpCredentialHexValue(encoded[i * 2U]);
    int low = ftpCredentialHexValue(encoded[(i * 2U) + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    uint8_t obfuscated = (uint8_t)((high << 4) | low);
    payload[i] = (char)(obfuscated ^ ((uint8_t)key[i % keyLen]));
  }
  payload[payloadLen] = '\0';

  if (strncmp(payload, kPrefix, strlen(kPrefix)) != 0) {
    return false;
  }

  strlcpy(plainText, payload + strlen(kPrefix), plainTextSize);
  return true;
}

static bool loadConfig(ServerConfig &cfg) {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return false;
    
    FILE *file = fopen("/fs/server_config.json", "r");
    if (!file) {
      return false;
    }
    
    // Read file into buffer
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (fileSize <= 0 || fileSize > 4096) {
      fclose(file);
      return false;
    }
    
    char *buffer = (char *)malloc(fileSize + 1);
    if (!buffer) {
      fclose(file);
      return false;
    }
    
    size_t bytesRead = fread(buffer, 1, fileSize, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    
    JsonDocument doc;  // ArduinoJson v7: auto-sizing
    DeserializationError err = deserializeJson(doc, buffer);
    free(buffer);
  #else
    if (!LittleFS.exists(SERVER_CONFIG_PATH)) {
      return false;
    }

    File file = LittleFS.open(SERVER_CONFIG_PATH, "r");
    if (!file) {
      return false;
    }

    JsonDocument doc;  // ArduinoJson v7: auto-sizing
    DeserializationError err = deserializeJson(doc, file);
    file.close();
  #endif

  if (err) {
    Serial.println(F("Server config parse failed"));
    return false;
  }

  memset(&cfg, 0, sizeof(ServerConfig));

  strlcpy(cfg.serverName, doc["serverName"].as<const char *>() ? doc["serverName"].as<const char *>() : "Tank Alarm Server", sizeof(cfg.serverName));
  strlcpy(cfg.serverFleet, doc["serverFleet"].as<const char *>() ? doc["serverFleet"].as<const char *>() : "tankalarm-server", sizeof(cfg.serverFleet));
  strlcpy(cfg.clientFleet, doc["clientFleet"].as<const char *>() ? doc["clientFleet"].as<const char *>() : "tankalarm-clients", sizeof(cfg.clientFleet));
  strlcpy(cfg.productUid, doc["productUid"].as<const char *>() ? doc["productUid"].as<const char *>() : "", sizeof(cfg.productUid));
  strlcpy(cfg.smsPrimary, doc["smsPrimary"].as<const char *>() ? doc["smsPrimary"].as<const char *>() : "+15555555555", sizeof(cfg.smsPrimary));
  strlcpy(cfg.smsSecondary, doc["smsSecondary"].as<const char *>() ? doc["smsSecondary"].as<const char *>() : "+15555555555", sizeof(cfg.smsSecondary));
  strlcpy(cfg.dailyEmail, doc["dailyEmail"].as<const char *>() ? doc["dailyEmail"].as<const char *>() : "reports@example.com", sizeof(cfg.dailyEmail));
  bool pinFromFile = false;
  const char *filePin = doc["configPin"].as<const char *>();
  if (filePin && isValidPin(filePin)) {
    strlcpy(cfg.configPin, filePin, sizeof(cfg.configPin));
    pinFromFile = true;
  } else {
    strlcpy(cfg.configPin, DEFAULT_ADMIN_PIN, sizeof(cfg.configPin));
  }
  if (!pinFromFile) {
    gConfigDirty = true;  // Persist the default PIN when none was stored
  }
  cfg.dailyHour = doc["dailyHour"].is<uint8_t>() ? doc["dailyHour"].as<uint8_t>() : DAILY_EMAIL_HOUR_DEFAULT;
  cfg.dailyMinute = doc["dailyMinute"].is<uint8_t>() ? doc["dailyMinute"].as<uint8_t>() : DAILY_EMAIL_MINUTE_DEFAULT;
  if (doc["webRefreshSeconds"].is<uint16_t>() || doc["webRefreshSeconds"].is<uint32_t>()) {
    cfg.webRefreshSeconds = doc["webRefreshSeconds"].as<uint16_t>();
  } else {
    cfg.webRefreshSeconds = 21600;
  }
  cfg.useStaticIp = doc["useStaticIp"].is<bool>() ? doc["useStaticIp"].as<bool>() : false;
  cfg.smsOnHigh = doc["smsOnHigh"].is<bool>() ? doc["smsOnHigh"].as<bool>() : true;
  cfg.smsOnLow = doc["smsOnLow"].is<bool>() ? doc["smsOnLow"].as<bool>() : true;
  cfg.smsOnClear = doc["smsOnClear"].is<bool>() ? doc["smsOnClear"].as<bool>() : false;
  cfg.serverDownSmsEnabled = doc["serverDownSmsEnabled"].is<bool>() ? doc["serverDownSmsEnabled"].as<bool>() : true;
  cfg.viewerEnabled = doc["viewerEnabled"].is<bool>() ? doc["viewerEnabled"].as<bool>() : false;
  if (doc["updatePolicy"].is<uint8_t>()) {
    cfg.updatePolicy = doc["updatePolicy"].as<uint8_t>();
    if (cfg.updatePolicy > UPDATE_POLICY_AUTO_DFU) cfg.updatePolicy = UPDATE_POLICY_DISABLED;
  } else {
    // Migrate from legacy fields (githubUpdateCheckEnabled / manualUpdateSource)
    bool guc = doc["githubUpdateCheckEnabled"].is<bool>() ? doc["githubUpdateCheckEnabled"].as<bool>() : false;
    uint8_t mus = doc["manualUpdateSource"].is<uint8_t>() ? doc["manualUpdateSource"].as<uint8_t>() : 0;
    cfg.updatePolicy = guc ? UPDATE_POLICY_ALERT_GITHUB
                           : (mus == DFU_SOURCE_GITHUB_DIRECT ? UPDATE_POLICY_AUTO_GITHUB
                                                              : UPDATE_POLICY_DISABLED);
  }
  cfg.checkClientVersionAlerts = doc["checkClientVersionAlerts"].is<bool>() ? doc["checkClientVersionAlerts"].as<bool>() : true;
  cfg.checkViewerVersionAlerts = doc["checkViewerVersionAlerts"].is<bool>() ? doc["checkViewerVersionAlerts"].as<bool>() : true;

  JsonObject ftpObj = doc["ftp"].as<JsonObject>();
  bool migrateLegacyFtpSecrets = false;
  cfg.ftpEnabled = ftpObj ? (ftpObj["enabled"].is<bool>() ? ftpObj["enabled"].as<bool>() : false) : (doc["ftpEnabled"].is<bool>() ? doc["ftpEnabled"].as<bool>() : false);
  cfg.ftpPassive = ftpObj ? (ftpObj["passive"].is<bool>() ? ftpObj["passive"].as<bool>() : true) : true;
  cfg.ftpBackupOnChange = ftpObj ? (ftpObj["backupOnChange"].is<bool>() ? ftpObj["backupOnChange"].as<bool>() : false) : (doc["ftpBackupOnChange"].is<bool>() ? doc["ftpBackupOnChange"].as<bool>() : false);
  cfg.ftpRestoreOnBoot = ftpObj ? (ftpObj["restoreOnBoot"].is<bool>() ? ftpObj["restoreOnBoot"].as<bool>() : false) : (doc["ftpRestoreOnBoot"].is<bool>() ? doc["ftpRestoreOnBoot"].as<bool>() : false);
  cfg.ftpPort = ftpObj ? (ftpObj["port"].is<uint16_t>() ? ftpObj["port"].as<uint16_t>() : FTP_PORT_DEFAULT) : (doc["ftpPort"].is<uint16_t>() ? doc["ftpPort"].as<uint16_t>() : FTP_PORT_DEFAULT);
  if (ftpObj && ftpObj["host"]) {
    strlcpy(cfg.ftpHost, ftpObj["host"], sizeof(cfg.ftpHost));
  }
  const char *ftpUserObf = ftpObj ? ftpObj["userObf"].as<const char *>() : nullptr;
  if (ftpUserObf && ftpUserObf[0] != '\0') {
    if (!decodeFtpCredential(ftpUserObf, cfg.productUid, cfg.configPin, cfg.ftpUser, sizeof(cfg.ftpUser))) {
      Serial.println(F("Failed to decode stored FTP username"));
    }
  }
  const char *ftpPassObf = ftpObj ? ftpObj["passObf"].as<const char *>() : nullptr;
  if (ftpPassObf && ftpPassObf[0] != '\0') {
    if (!decodeFtpCredential(ftpPassObf, cfg.productUid, cfg.configPin, cfg.ftpPass, sizeof(cfg.ftpPass))) {
      Serial.println(F("Failed to decode stored FTP password"));
    }
  }
  if (ftpObj && ftpObj["path"]) {
    strlcpy(cfg.ftpPath, ftpObj["path"], sizeof(cfg.ftpPath));
  }
  if ((!ftpObj || !ftpObj["host"]) && doc["ftpHost"].as<const char *>()) {
    strlcpy(cfg.ftpHost, doc["ftpHost"], sizeof(cfg.ftpHost));
  }
  const char *legacyFtpUser = nullptr;
  if (ftpObj && ftpObj["user"]) {
    legacyFtpUser = ftpObj["user"].as<const char *>();
  }
  if ((!cfg.ftpUser[0]) && legacyFtpUser && legacyFtpUser[0] != '\0') {
    strlcpy(cfg.ftpUser, legacyFtpUser, sizeof(cfg.ftpUser));
    migrateLegacyFtpSecrets = true;
  } else if ((!cfg.ftpUser[0]) && ((!ftpObj || !ftpObj["user"]) && doc["ftpUser"].as<const char *>())) {
    strlcpy(cfg.ftpUser, doc["ftpUser"], sizeof(cfg.ftpUser));
    migrateLegacyFtpSecrets = true;
  }
  const char *legacyFtpPass = nullptr;
  if (ftpObj && ftpObj["pass"]) {
    legacyFtpPass = ftpObj["pass"].as<const char *>();
  }
  if ((!cfg.ftpPass[0]) && legacyFtpPass && legacyFtpPass[0] != '\0') {
    strlcpy(cfg.ftpPass, legacyFtpPass, sizeof(cfg.ftpPass));
    migrateLegacyFtpSecrets = true;
  } else if ((!cfg.ftpPass[0]) && ((!ftpObj || !ftpObj["pass"]) && doc["ftpPass"].as<const char *>())) {
    strlcpy(cfg.ftpPass, doc["ftpPass"], sizeof(cfg.ftpPass));
    migrateLegacyFtpSecrets = true;
  }
  if ((!ftpObj || !ftpObj["path"]) && doc["ftpPath"].as<const char *>()) {
    strlcpy(cfg.ftpPath, doc["ftpPath"], sizeof(cfg.ftpPath));
  }
  if (migrateLegacyFtpSecrets) {
    gConfigDirty = true;
  }

  // FTPS fields (default: disabled, fingerprint mode, empty fingerprint/server name)
  cfg.ftpsEnabled = ftpObj ? (ftpObj["ftpsEnabled"].is<bool>() ? ftpObj["ftpsEnabled"].as<bool>() : false) : false;
  cfg.ftpsTrustMode = ftpObj ? (ftpObj["ftpsTrustMode"].is<uint8_t>() ? ftpObj["ftpsTrustMode"].as<uint8_t>() : 0) : 0;
  const char *ftpsFingerprintObf = ftpObj ? ftpObj["ftpsFingerprintObf"].as<const char *>() : nullptr;
  if (ftpsFingerprintObf && ftpsFingerprintObf[0] != '\0') {
    if (!decodeFtpCredential(ftpsFingerprintObf, cfg.productUid, cfg.configPin, cfg.ftpsFingerprint, sizeof(cfg.ftpsFingerprint))) {
      Serial.println(F("Failed to decode stored FTPS fingerprint"));
      cfg.ftpsFingerprint[0] = '\0';
    }
  } else if (ftpObj && ftpObj["ftpsFingerprint"]) {
    strlcpy(cfg.ftpsFingerprint, ftpObj["ftpsFingerprint"], sizeof(cfg.ftpsFingerprint));
  } else {
    cfg.ftpsFingerprint[0] = '\0';
  }
  if (ftpObj && ftpObj["ftpsTlsServerName"]) {
    strlcpy(cfg.ftpsTlsServerName, ftpObj["ftpsTlsServerName"], sizeof(cfg.ftpsTlsServerName));
  } else {
    cfg.ftpsTlsServerName[0] = '\0';
  }

  if (doc["staticIp"]) {
    JsonArrayConst ip = doc["staticIp"].as<JsonArrayConst>();
    if (ip.size() == 4) {
      gStaticIp = IPAddress(ip[0], ip[1], ip[2], ip[3]);
    }
  }
  if (doc["gateway"]) {
    JsonArrayConst gw = doc["gateway"].as<JsonArrayConst>();
    if (gw.size() == 4) {
      gStaticGateway = IPAddress(gw[0], gw[1], gw[2], gw[3]);
    }
  }
  if (doc["subnet"]) {
    JsonArrayConst sn = doc["subnet"].as<JsonArrayConst>();
    if (sn.size() == 4) {
      gStaticSubnet = IPAddress(sn[0], sn[1], sn[2], sn[3]);
    }
  }
  if (doc["dns"]) {
    JsonArrayConst dns = doc["dns"].as<JsonArrayConst>();
    if (dns.size() == 4) {
      gStaticDns = IPAddress(dns[0], dns[1], dns[2], dns[3]);
    }
  }

  return true;
#else
  return false; // Filesystem not available
#endif
}

static bool saveConfig(const ServerConfig &cfg) {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return false;
  #endif
  
  char ftpUserObf[80];
  char ftpPassObf[80];
  // Must hold 2*(strlen("TA1:") + 64) + 1 = 137 bytes of hex for a full
  // SHA-256 fingerprint; previous 136-byte size was one byte short and
  // caused encodeFtpCredential() to return false, failing saveConfig().
  char ftpsFingerprintObf[160];
  if (!encodeFtpCredential(cfg.ftpUser, cfg.productUid, cfg.configPin, ftpUserObf, sizeof(ftpUserObf)) ||
      !encodeFtpCredential(cfg.ftpPass, cfg.productUid, cfg.configPin, ftpPassObf, sizeof(ftpPassObf)) ||
      !encodeFtpCredential(cfg.ftpsFingerprint, cfg.productUid, cfg.configPin, ftpsFingerprintObf, sizeof(ftpsFingerprintObf))) {
    Serial.println(F("Failed to obfuscate FTP credentials for storage"));
    return false;
  }

  JsonDocument doc;
  doc["serverName"] = cfg.serverName;
  doc["serverFleet"] = cfg.serverFleet;
  doc["clientFleet"] = cfg.clientFleet;
  doc["productUid"] = cfg.productUid;
  doc["smsPrimary"] = cfg.smsPrimary;
  doc["smsSecondary"] = cfg.smsSecondary;
  doc["dailyEmail"] = cfg.dailyEmail;
  doc["configPin"] = cfg.configPin;
  doc["dailyHour"] = cfg.dailyHour;
  doc["dailyMinute"] = cfg.dailyMinute;
  doc["webRefreshSeconds"] = cfg.webRefreshSeconds;
  doc["useStaticIp"] = cfg.useStaticIp;
  doc["smsOnHigh"] = cfg.smsOnHigh;
  doc["smsOnLow"] = cfg.smsOnLow;
  doc["smsOnClear"] = cfg.smsOnClear;
  doc["serverDownSmsEnabled"] = cfg.serverDownSmsEnabled;
  doc["viewerEnabled"] = cfg.viewerEnabled;
  doc["updatePolicy"] = cfg.updatePolicy;
  doc["checkClientVersionAlerts"] = cfg.checkClientVersionAlerts;
  doc["checkViewerVersionAlerts"] = cfg.checkViewerVersionAlerts;

  JsonObject ftp = doc["ftp"].to<JsonObject>();
  ftp["enabled"] = cfg.ftpEnabled;
  ftp["passive"] = cfg.ftpPassive;
  ftp["backupOnChange"] = cfg.ftpBackupOnChange;
  ftp["restoreOnBoot"] = cfg.ftpRestoreOnBoot;
  ftp["port"] = cfg.ftpPort;
  ftp["host"] = cfg.ftpHost;
  if (ftpUserObf[0] != '\0') {
    ftp["userObf"] = ftpUserObf;
  }
  if (ftpPassObf[0] != '\0') {
    ftp["passObf"] = ftpPassObf;
  }
  ftp["path"] = cfg.ftpPath;
  ftp["ftpsEnabled"] = cfg.ftpsEnabled;
  ftp["ftpsTrustMode"] = cfg.ftpsTrustMode;
  if (ftpsFingerprintObf[0] != '\0') {
    ftp["ftpsFingerprintObf"] = ftpsFingerprintObf;
  }
  if (cfg.ftpsTlsServerName[0] != '\0') {
    ftp["ftpsTlsServerName"] = cfg.ftpsTlsServerName;
  }

  JsonArray ip = doc["staticIp"].to<JsonArray>();
  ip.add(gStaticIp[0]);
  ip.add(gStaticIp[1]);
  ip.add(gStaticIp[2]);
  ip.add(gStaticIp[3]);

  JsonArray gw = doc["gateway"].to<JsonArray>();
  gw.add(gStaticGateway[0]);
  gw.add(gStaticGateway[1]);
  gw.add(gStaticGateway[2]);
  gw.add(gStaticGateway[3]);

  JsonArray sn = doc["subnet"].to<JsonArray>();
  sn.add(gStaticSubnet[0]);
  sn.add(gStaticSubnet[1]);
  sn.add(gStaticSubnet[2]);
  sn.add(gStaticSubnet[3]);

  JsonArray dns = doc["dns"].to<JsonArray>();
  dns.add(gStaticDns[0]);
  dns.add(gStaticDns[1]);
  dns.add(gStaticDns[2]);
  dns.add(gStaticDns[3]);

  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Mbed OS — atomic write-to-temp-then-rename
    String jsonStr;
    size_t len = serializeJson(doc, jsonStr);
    if (len == 0) {
      Serial.println(F("Failed to serialize server config"));
      return false;
    }
    if (!serverWriteFileAtomic("/fs/server_config.json",
                               jsonStr.c_str(), jsonStr.length())) {
      Serial.println(F("Failed to write server config"));
      return false;
    }
    return true;
  #else
    String jsonStr;
    size_t len = serializeJson(doc, jsonStr);
    if (len == 0) {
      Serial.println(F("Failed to serialize server config"));
      return false;
    }
    if (!tankalarm_littlefs_write_file_atomic(SERVER_CONFIG_PATH,
            (const uint8_t *)jsonStr.c_str(), jsonStr.length())) {
      Serial.println(F("Failed to write server config"));
      return false;
    }
    return true;
  #endif
#else
  return false; // Filesystem not available
#endif
}

static double loadServerHeartbeatEpoch() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return 0.0;
    }

    char buffer[128] = {0};
    const char *path = "/fs/server_heartbeat.json";
    ssize_t bytesRead = posix_read_file(path, buffer, sizeof(buffer));
    if (bytesRead <= 0) {
      return 0.0;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buffer);
    if (err) {
      return 0.0;
    }

    return doc["epoch"].as<double>();
  #else
    if (!LittleFS.exists(SERVER_HEARTBEAT_PATH)) {
      return 0.0;
    }

    File file = LittleFS.open(SERVER_HEARTBEAT_PATH, "r");
    if (!file) {
      return 0.0;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
      return 0.0;
    }

    return doc["epoch"].as<double>();
  #endif
#else
  return 0.0;
#endif
}

static bool saveServerHeartbeatEpoch(double epoch) {
#ifdef FILESYSTEM_AVAILABLE
  if (epoch <= 0.0) {
    return false;
  }

  JsonDocument doc;
  doc["epoch"] = epoch;

  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return false;
    }

    String jsonStr;
    size_t len = serializeJson(doc, jsonStr);
    if (len == 0) {
      return false;
    }
    return serverWriteFileAtomic("/fs/server_heartbeat.json",
                                 jsonStr.c_str(), jsonStr.length());
  #else
    String jsonStr;
    size_t len = serializeJson(doc, jsonStr);
    if (len == 0) {
      return false;
    }
    return tankalarm_littlefs_write_file_atomic(SERVER_HEARTBEAT_PATH,
            (const uint8_t *)jsonStr.c_str(), jsonStr.length());
  #endif
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// FTP backup/restore helpers
// ---------------------------------------------------------------------------

struct BackupFileEntry {
  const char *localPath;
  const char *remoteName;
  bool required;
};

static const BackupFileEntry kBackupFiles[] = {
  { SERVER_CONFIG_PATH, "server_config.json", true },
  { CONTACTS_CONFIG_PATH, "contacts_config.json", false },
  { "/email_format.json", "email_format.json", false },
  { CLIENT_CONFIG_CACHE_PATH, "client_config_cache.txt", false },
  { CALIBRATION_LOG_PATH, "calibration_log.txt", false },
  { "/calibration_data.txt", "calibration_data.txt", false },
  { SENSOR_REGISTRY_PATH, "sensor_registry.json", false },
  { CLIENT_METADATA_CACHE_PATH, "client_metadata.json", false },
  { "/history_settings.json", "history_settings.json", true }
};

static bool prepareLocalBackupFilesForFtp(FtpResult &result) {
#ifndef FILESYSTEM_AVAILABLE
  strlcpy(result.errorMessage,
          "Local filesystem not available; cannot upload backup files",
          sizeof(result.errorMessage));
  return false;
#else
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      strlcpy(result.errorMessage,
              "Local filesystem not mounted; cannot read /fs backup files",
              sizeof(result.errorMessage));
      return false;
    }
  #endif

  // Gate the pre-backup config save on the dirty flag. Avoids burning a
  // flash erase/program cycle on every backup when nothing has changed in
  // memory since the last save. saveHistorySettings() is hash-gated
  // internally, so it can be called unconditionally with the same effect.
  if (gConfigDirty) {
    if (!saveConfig(gConfig)) {
      strlcpy(result.errorMessage,
              "Failed to save server_config.json before FTP backup",
              sizeof(result.errorMessage));
      return false;
    }
    gConfigDirty = false;
  }

  if (!saveHistorySettings()) {
    strlcpy(result.errorMessage,
            "Failed to save history_settings.json before FTP backup",
            sizeof(result.errorMessage));
    return false;
  }

  return true;
#endif
}static void buildLocalPath(const char *relativePath, char *out, size_t outSize) {
  if (!relativePath || !out || outSize == 0) {
    return;
  }
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (strncmp(relativePath, "/fs", 3) == 0) {
    strlcpy(out, relativePath, outSize);
  } else {
    snprintf(out, outSize, "/fs%s", (relativePath[0] == '/') ? relativePath : "/");
  }
#else
  strlcpy(out, relativePath, outSize);
#endif
}

static bool readFileToBuffer(const char *relativePath, char *out, size_t outMax, size_t &outLen) {
#ifndef FILESYSTEM_AVAILABLE
  return false;
#else
  char fullPath[96];
  buildLocalPath(relativePath, fullPath, sizeof(fullPath));

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  FILE *file = fopen(fullPath, "rb");
  if (!file) {
    Serial.print(F("readFileToBuffer: fopen failed for "));
    Serial.print(fullPath);
    Serial.print(F(" errno="));
    Serial.println(errno);
    return false;
  }

  fseek(file, 0, SEEK_END);
  long fileSize = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (fileSize < 0 || fileSize >= (long)outMax) {
    fclose(file);
    return false;
  }

  outLen = fread(out, 1, fileSize, file);
  out[outLen] = 0;
  fclose(file);
  return true;
#else
  if (!LittleFS.exists(fullPath)) {
    return false;
  }

  File file = LittleFS.open(fullPath, "r");
  if (!file) {
    return false;
  }

  size_t fileSize = file.size();
  if (fileSize >= outMax) {
    file.close();
    return false;
  }

  outLen = file.read((uint8_t*)out, fileSize);
  out[outLen] = 0;
  file.close();
  return true;
#endif
#endif
}

static bool writeBufferToFile(const char *relativePath, const uint8_t *data, size_t len) {
#ifndef FILESYSTEM_AVAILABLE
  (void)relativePath;
  (void)data;
  (void)len;
  return false;
#else
  if (!data || len > FTP_MAX_FILE_BYTES) {
    return false;
  }

  char fullPath[96];
  buildLocalPath(relativePath, fullPath, sizeof(fullPath));

#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  return serverWriteFileAtomic(fullPath, (const char *)data, len);
#else
  return tankalarm_littlefs_write_file_atomic(fullPath, data, len);
#endif
#endif
}

static bool ftpReadResponse(EthernetClient &client, int &code, char *message, size_t maxLen, uint32_t timeoutMs = FTP_TIMEOUT_MS) {
  if (maxLen > 0) message[0] = '\0';
  char line[128];
  size_t linePos = 0;
  unsigned long start = millis();
  int multilineCode = -1;

  while (millis() - start < timeoutMs) {
    while (client.available()) {
      char c = client.read();
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        line[linePos] = '\0';
        
        if (linePos >= 3 && isdigit(line[0]) && isdigit(line[1]) && isdigit(line[2])) {
          char codeStr[4] = {line[0], line[1], line[2], '\0'};
          int thisCode = atoi(codeStr);
          
          if (linePos > 3 && line[3] == '-') {
            multilineCode = thisCode;
            // Append to message if space allows (bounded copy, no strcat)
            size_t currentLen = strlen(message);
            size_t needed = currentLen + linePos + 2;  // +1 for \n, +1 for \0
            if (needed <= maxLen) {
              memcpy(message + currentLen, line, linePos);
              message[currentLen + linePos] = '\n';
              message[currentLen + linePos + 1] = '\0';
            }
          } else if (multilineCode == -1 || thisCode == multilineCode) {
            code = thisCode;
            // Append last line (bounded copy, no strcat)
            size_t currentLen = strlen(message);
            size_t needed = currentLen + linePos + 1;  // +1 for \0
            if (needed <= maxLen) {
              memcpy(message + currentLen, line, linePos);
              message[currentLen + linePos] = '\0';
            }
            return true;
          }
        }
        linePos = 0;
      } else {
        if (linePos < sizeof(line) - 1) {
          line[linePos++] = c;
        }
      }
    }
    safeSleep(5);
  }
  return false;
}

static bool ftpSendCommand(FtpSession &session, const char *command, int &code, char *message, size_t maxLen) {
  session.ctrl.print(command);
  session.ctrl.print("\r\n");
  return ftpReadResponse(session.ctrl, code, message, maxLen);
}

static bool ftpConnectAndLogin(FtpSession &session, char *error, size_t errorSize) {
  if (!gConfig.ftpEnabled || strlen(gConfig.ftpHost) == 0) {
    snprintf(error, errorSize, "FTP disabled or host missing");
    return false;
  }

  if (!session.ctrl.connect(gConfig.ftpHost, gConfig.ftpPort)) {
    snprintf(error, errorSize, "FTP connect failed");
    return false;
  }

  int code = 0;
  char msg[128];
  if (!ftpReadResponse(session.ctrl, code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "No welcome banner");
    return false;
  }

  char cmdBuffer[96];
  snprintf(cmdBuffer, sizeof(cmdBuffer), "USER %s", (strlen(gConfig.ftpUser) > 0) ? gConfig.ftpUser : "anonymous");
  if (!ftpSendCommand(session, cmdBuffer, code, msg, sizeof(msg)) || (code != 230 && code != 331)) {
    snprintf(error, errorSize, "USER rejected");
    return false;
  }
  if (code == 331) {
    snprintf(cmdBuffer, sizeof(cmdBuffer), "PASS %s", (strlen(gConfig.ftpPass) > 0) ? gConfig.ftpPass : "guest");
    if (!ftpSendCommand(session, cmdBuffer, code, msg, sizeof(msg)) || code != 230) {
      snprintf(error, errorSize, "PASS rejected");
      return false;
    }
  }

  if (!ftpSendCommand(session, "TYPE I", code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "TYPE failed");
    return false;
  }

  return true;
}

static bool ftpEnterPassive(FtpSession &session, IPAddress &dataHost, uint16_t &dataPort, char *error, size_t errorSize) {
  if (!gConfig.ftpPassive) {
    snprintf(error, errorSize, "Only passive FTP supported");
    return false;
  }
  int code = 0;
  char msg[128];
  if (!ftpSendCommand(session, "PASV", code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "PASV failed");
    return false;
  }

  int parts[6] = {0};
  int idx = 0;
  const char *pasvStart = strchr(msg, '(');
  if (!pasvStart) {
    snprintf(error, errorSize, "PASV parse error");
    return false;
  }
  pasvStart++;
  for (const char *cursor = pasvStart; *cursor != '\0' && idx < 6; ++cursor) {
    if (isdigit(*cursor)) {
      parts[idx] = parts[idx] * 10 + (*cursor - '0');
    } else if (*cursor == ',' || *cursor == ')') {
      idx++;
    }
  }

  if (idx < 6) {
    snprintf(error, errorSize, "PASV parse error");
    return false;
  }

  dataHost = IPAddress(parts[0], parts[1], parts[2], parts[3]);
  dataPort = (parts[4] << 8) | parts[5];
  return true;
}

// Return the per-server remote base directory.
// Replaces ':' with '_' in the server UID so the path is valid on both Windows
// FTP servers (where ':' is reserved) and Linux.
static void buildRemoteBaseDir(char *out, size_t outLen) {
  const char *base = (strlen(gConfig.ftpPath) > 0) ? gConfig.ftpPath : FTP_PATH_DEFAULT;
  const char *uid = (strlen(gServerUid) > 0) ? gServerUid : "server";
  bool baseHasSlash = base[strlen(base) - 1] == '/';
  char safeUid[64];
  strlcpy(safeUid, uid, sizeof(safeUid));
  for (char *p = safeUid; *p; ++p) {
    if (*p == ':') *p = '_';
  }
  snprintf(out, outLen, "%s%s%s", base, baseHasSlash ? "" : "/", safeUid);
}

static void buildRemotePath(char *out, size_t outLen, const char *fileName) {
  char baseDir[128];
  buildRemoteBaseDir(baseDir, sizeof(baseDir));
  snprintf(out, outLen, "%s/%s", baseDir, fileName);
}

static bool ftpStoreBuffer(FtpSession &session, const char *remoteFile, const uint8_t *data, size_t len, char *error, size_t errorSize) {
  if (!data || len == 0) {
    snprintf(error, errorSize, "No data to upload");
    return false;
  }

  IPAddress dataHost;
  uint16_t dataPort = 0;
  if (!ftpEnterPassive(session, dataHost, dataPort, error, errorSize)) {
    return false;
  }

  EthernetClient dataClient;
  if (!dataClient.connect(dataHost, dataPort)) {
    snprintf(error, errorSize, "Data connect failed");
    return false;
  }

  char cmd[160];
  snprintf(cmd, sizeof(cmd), "STOR %s", remoteFile);
  int code = 0;
  char msg[128];
  if (!ftpSendCommand(session, cmd, code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "STOR rejected");
    dataClient.stop();
    return false;
  }

  size_t written = dataClient.write(data, len);
  dataClient.stop();
  if (written != len) {
    snprintf(error, errorSize, "Short write");
    return false;
  }

  if (!ftpReadResponse(session.ctrl, code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "STOR completion failed");
    return false;
  }

  return true;
}

static bool ftpRetrieveBuffer(FtpSession &session, const char *remoteFile, char *out, size_t outMax, size_t &outLen, char *error, size_t errorSize) {
  IPAddress dataHost;
  uint16_t dataPort = 0;
  if (!ftpEnterPassive(session, dataHost, dataPort, error, errorSize)) {
    return false;
  }

  EthernetClient dataClient;
  if (!dataClient.connect(dataHost, dataPort)) {
    snprintf(error, errorSize, "Data connect failed");
    return false;
  }

  char cmd[160];
  snprintf(cmd, sizeof(cmd), "RETR %s", remoteFile);
  int code = 0;
  char msg[128];
  if (!ftpSendCommand(session, cmd, code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "RETR rejected");
    dataClient.stop();
    return false;
  }

  outLen = 0;
  unsigned long start = millis();
  while (millis() - start < FTP_TIMEOUT_MS) {
    while (dataClient.available()) {
      int c = dataClient.read();
      if (c != -1) {
        if (outLen < outMax - 1) {
          out[outLen++] = (char)c;
        } else {
          snprintf(error, errorSize, "File too large");
          dataClient.stop();
          return false;
        }
      }
    }
    if (!dataClient.connected()) {
      break;
    }
    safeSleep(2);
  }
  out[outLen] = 0;
  dataClient.stop();

  if (!ftpReadResponse(session.ctrl, code, msg, sizeof(msg)) || code >= 400) {
    snprintf(error, errorSize, "RETR completion failed");
    return false;
  }

  return true;
}

static void ftpQuit(FtpSession &session) {
  if (session.ctrl.connected()) {
    int code = 0;
    char msg[64];
    ftpSendCommand(session, "QUIT", code, msg, sizeof(msg));
    session.ctrl.stop();
  }
}

static void serviceTransferWatchdog() {
#if FTP_BACKUP_DISABLE_WATCHDOG_KICK_DIAG
  if (gFtpBackupCriticalPathActive) {
    return;
  }
#endif

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
}

static void ftpsProgressKick() {
  serviceTransferWatchdog();
}

static const char *ftpsErrorName(FtpsError error) {
  switch (error) {
    case FtpsError::None: return "None";
    case FtpsError::NetworkNotInitialized: return "NetworkNotInitialized";
    case FtpsError::BannerReadFailed: return "BannerReadFailed";
    case FtpsError::PbszRejected: return "PbszRejected";
    case FtpsError::ProtPRejected: return "ProtPRejected";
    case FtpsError::TypeRejected: return "TypeRejected";
    case FtpsError::PasvParseFailed: return "PasvParseFailed";
    case FtpsError::DataConnectionFailed: return "DataConnectionFailed";
    case FtpsError::QuitFailed: return "QuitFailed";
    case FtpsError::AuthTlsRejected: return "AuthTlsRejected";
    case FtpsError::ControlTlsHandshakeFailed: return "ControlTlsHandshakeFailed";
    case FtpsError::CertValidationFailed: return "CertValidationFailed";
    case FtpsError::DataTlsHandshakeFailed: return "DataTlsHandshakeFailed";
    case FtpsError::SessionReuseRequired: return "SessionReuseRequired";
    case FtpsError::LoginRejected: return "LoginRejected";
    case FtpsError::TransferFailed: return "TransferFailed";
    case FtpsError::FinalReplyFailed: return "FinalReplyFailed";
    case FtpsError::PassiveModeRejected: return "PassiveModeRejected";
    case FtpsError::ConnectionFailed: return "ConnectionFailed";
    case FtpsError::DirectoryCreateFailed: return "DirectoryCreateFailed";
    case FtpsError::SizeQueryFailed: return "SizeQueryFailed";
    default: return "Unknown";
  }
}

static bool ftpsSessionLikelyDead(FtpsError error) {
  switch (error) {
    case FtpsError::ConnectionFailed:
    case FtpsError::PassiveModeRejected:
    case FtpsError::DataConnectionFailed:
    case FtpsError::FinalReplyFailed:
      return true;
    default:
      return false;
  }
}

// Compatibility classifiers used by backup retry logic.
// These mirror FTPSclientOPTA helper behavior so this sketch builds
// against both older and newer FTPS library snapshots.
// --- Refined classifiers (PER_FILE_RETRY_PLAN_04172026.md, Phase 0) ---------
// Standardized in FTPSclientOPTA as:
//   ftpsIsSessionDead(err)                  -> abort the entire backup batch
//   ftpsIsTransferRetriable(err, nsapiCode) -> retry just this file
//
// A failure that is neither dead-session nor retriable (e.g. server 5xx on
// STOR, connect timeout) means "skip this file but keep the batch going".

// Persistent ring buffer of recent FTPS trace phases so we can read them over
// HTTP after a failure (USB CDC sometimes detaches during FTPS work).
// NOTE: Kept small (64 bytes) to stay within the available stack/heap budget.
// The ring-buffer drop-oldest-half logic below handles overflow safely.
static char gFtpsTraceBuf[1024] = {0};
static size_t gFtpsTraceLen = 0;
static char gFtpBackupStage[32] = "idle";
static unsigned long gFtpBackupStageMs = 0;
static bool gFtpBackupCriticalPathActive = false;

static void ftpsTraceReset() {
  gFtpsTraceBuf[0] = '\0';
  gFtpsTraceLen = 0;
}

static bool readFtpBackupMarker(char *out, size_t outSize) {
  if (!out || outSize == 0) {
    return false;
  }
  out[0] = '\0';

#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return false;
    }
    FILE *f = fopen("/fs" FTP_BACKUP_MARKER_PATH, "rb");
    if (!f) {
      return false;
    }
    size_t n = fread(out, 1, outSize - 1, f);
    fclose(f);
    out[n] = '\0';
    return n > 0;
  #else
    if (!LittleFS.exists(FTP_BACKUP_MARKER_PATH)) {
      return false;
    }
    File f = LittleFS.open(FTP_BACKUP_MARKER_PATH, "r");
    if (!f) {
      return false;
    }
    size_t n = f.read((uint8_t *)out, outSize - 1);
    f.close();
    out[n] = '\0';
    return n > 0;
  #endif
#else
  return false;
#endif
}

static void writeFtpBackupMarker(const char *stage) {
  const char *s = (stage && stage[0] != '\0') ? stage : "unknown";
  char marker[96];
  snprintf(marker, sizeof(marker), "%lu:%s", (unsigned long)millis(), s);

#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    (void)serverWriteFileAtomic("/fs" FTP_BACKUP_MARKER_PATH,
                                marker,
                                strlen(marker));
  #else
    (void)tankalarm_littlefs_write_file_atomic(FTP_BACKUP_MARKER_PATH,
                                               (const uint8_t *)marker,
                                               strlen(marker));
  #endif
#endif
}

static void clearFtpBackupMarker() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    remove("/fs" FTP_BACKUP_MARKER_PATH);
  #else
    if (LittleFS.exists(FTP_BACKUP_MARKER_PATH)) {
      LittleFS.remove(FTP_BACKUP_MARKER_PATH);
    }
  #endif
#endif
}

static void setFtpBackupStage(const char *stage) {
  const char *value = (stage && stage[0] != '\0') ? stage : "unknown";
  strlcpy(gFtpBackupStage, value, sizeof(gFtpBackupStage));
  gFtpBackupStageMs = millis();
  writeFtpBackupMarker(gFtpBackupStage);
  addServerSerialLog(gFtpBackupStage, "info", "ftp");
  Serial.print(F("/api/ftp-backup: stage="));
  Serial.println(gFtpBackupStage);
}

static void handleFtpBackupStatusGet(EthernetClient &client) {
  JsonDocument doc;
  char marker[96] = {0};
  bool hasMarker = readFtpBackupMarker(marker, sizeof(marker));
  doc["stage"] = gFtpBackupStage;
  doc["stageMs"] = gFtpBackupStageMs;
  doc["nowMs"] = millis();
  doc["trace"] = gFtpsTraceBuf;
  doc["marker"] = hasMarker ? marker : "";
  doc["backupCriticalPath"] = gFtpBackupCriticalPathActive;
  respondJson(client, doc, 200);
}

// ============================================================================
// Memory Budget + System Status (Flash/RAM safety net)
// ============================================================================
// Logs the static memory footprint of the largest in-RAM pools at boot. Pairs
// with tankalarm_printHeapStats() to give a complete picture of where the
// 247 KB heap budget on Opta Lite is being spent. The numbers here are
// compile-time constants, but printing them next to the runtime free-heap
// figure makes regressions (a struct grew, a pool was sized up) immediately
// obvious in the serial log.

static void printMemoryBudget() {
  Serial.println(F("Memory budget (static pools):"));
  Serial.print(F("  gSensorHistory ["));
  Serial.print((unsigned)MAX_HISTORY_SENSORS);
  Serial.print(F("]: "));
  Serial.print((unsigned long)sizeof(gSensorHistory));
  Serial.println(F(" bytes"));

  Serial.print(F("  gSensorRecords ["));
  Serial.print((unsigned)MAX_SENSOR_RECORDS);
  Serial.print(F("]: "));
  Serial.print((unsigned long)sizeof(gSensorRecords));
  Serial.println(F(" bytes"));

  Serial.print(F("  gClientConfigs ["));
  Serial.print((unsigned)MAX_CLIENT_CONFIG_SNAPSHOTS);
  Serial.print(F("]: "));
  Serial.print((unsigned long)sizeof(gClientConfigs));
  Serial.println(F(" bytes"));

  Serial.print(F("  gClientMetadata ["));
  Serial.print((unsigned)MAX_CLIENT_METADATA);
  Serial.print(F("]: "));
  Serial.print((unsigned long)sizeof(gClientMetadata));
  Serial.println(F(" bytes"));

  Serial.print(F("  FTP_MAX_FILE_BYTES: "));
  Serial.print((unsigned long)FTP_MAX_FILE_BYTES);
  Serial.println(F(" bytes (per-file backup buffer cap)"));

  Serial.print(F("  Free heap now: "));
  Serial.print((unsigned long)tankalarm_freeRam());
  Serial.println(F(" bytes"));
}

// /api/system-status: surfaces the flash-write counters, free-heap snapshot,
// uptime, and pool capacities. Intended for operators running the dashboard
// to spot (a) flash wear (writes/sec trending up), (b) the hash-gate
// throttle firing as expected, and (c) heap headroom over time. Cheap
// (no flash I/O), so safe to poll from the UI.

static void handleSystemStatusGet(EthernetClient &client) {
  JsonDocument doc;
  doc["uptimeSec"] = (uint32_t)(millis() / 1000UL);
  doc["nowMs"] = (uint32_t)millis();
  doc["freeHeap"] = (uint32_t)tankalarm_freeRam();

  JsonObject flash = doc["flash"].to<JsonObject>();
  flash["writes"] = gFlashWritesSinceBoot;
  flash["failures"] = gFlashWriteFailuresSinceBoot;
  flash["skippedHash"] = gFlashWritesSkippedHash;
  flash["bytesWritten"] = gFlashBytesWrittenSinceBoot;
  flash["lastPath"] = gLastFlashWritePath ? gLastFlashWritePath : "";
  flash["lastMs"] = (uint32_t)gLastFlashWriteMillis;
  flash["lastFailurePath"] =
      gLastFlashWriteFailurePath ? gLastFlashWriteFailurePath : "";
  flash["lastFailureMs"] = (uint32_t)gLastFlashWriteFailureMillis;

  JsonObject pools = doc["pools"].to<JsonObject>();
  pools["sensorHistoryBytes"] = (uint32_t)sizeof(gSensorHistory);
  pools["sensorHistorySlots"] = (uint16_t)MAX_HISTORY_SENSORS;
  pools["sensorRecordsBytes"] = (uint32_t)sizeof(gSensorRecords);
  pools["sensorRecordsSlots"] = (uint16_t)MAX_SENSOR_RECORDS;
  pools["clientConfigBytes"] = (uint32_t)sizeof(gClientConfigs);
  pools["clientConfigSlots"] = (uint16_t)MAX_CLIENT_CONFIG_SNAPSHOTS;
  pools["clientMetadataBytes"] = (uint32_t)sizeof(gClientMetadata);
  pools["clientMetadataSlots"] = (uint16_t)MAX_CLIENT_METADATA;
  pools["ftpMaxFileBytes"] = (uint32_t)FTP_MAX_FILE_BYTES;

  JsonObject features = doc["features"].to<JsonObject>();
  features["ftpEnabled"] = gConfig.ftpEnabled;
  features["filesystemAvailable"] =
#ifdef FILESYSTEM_AVAILABLE
      true;
#else
      false;
#endif

  respondJson(client, doc, 200);
}

static void ftpsTraceCallback(const char *phase) {
  const char *p = phase ? phase : "?";
  Serial.print(F("FTPS phase: "));
  Serial.println(p);
  // DIAG: Persist every phase to the on-disk backup marker so we can recover
  // the last-executed FTPS phase across a watchdog reboot. Marker writes are
  // atomic and rare (one per store()/connect() milestone), so this does not
  // materially affect FTPS timing. NOTE: do NOT kick the watchdog here --
  // kicking during closeData() converts a watchdog reset (recoverable) into
  // a permanent indefinite hang because the underlying Mbed close still
  // blocks forever.
  writeFtpBackupMarker(p);
  // Also append to RAM buffer, with elapsed millis, separated by '|'.
  char line[96];
  int n = snprintf(line, sizeof(line), "%lu:%s|", (unsigned long)millis(), p);
  if (n <= 0) return;
  if (gFtpsTraceLen + (size_t)n + 1 >= sizeof(gFtpsTraceBuf)) {
    // Drop the oldest half to make room.
    size_t keep = sizeof(gFtpsTraceBuf) / 2;
    if (gFtpsTraceLen > keep) {
      memmove(gFtpsTraceBuf, gFtpsTraceBuf + (gFtpsTraceLen - keep), keep);
      gFtpsTraceLen = keep;
      gFtpsTraceBuf[gFtpsTraceLen] = '\0';
    }
  }
  if (gFtpsTraceLen + (size_t)n + 1 < sizeof(gFtpsTraceBuf)) {
    memcpy(gFtpsTraceBuf + gFtpsTraceLen, line, n);
    gFtpsTraceLen += n;
    gFtpsTraceBuf[gFtpsTraceLen] = '\0';
  }
}

static void ftpsAppendDiagnostics(char *error, size_t errorSize) {
  if (!error || errorSize == 0) {
    return;
  }
  char suffix[96];
  snprintf(suffix, sizeof(suffix), " [phase=%s err=%s]",
           gFtpsClient.lastPhase(), ftpsErrorName(gFtpsClient.lastError()));
  size_t used = strlen(error);
  if (used + 1 < errorSize) {
    snprintf(error + used, errorSize - used, "%s", suffix);
  }
}

static bool ftpsConnectAndLogin(char *error, size_t errorSize) {
  if (!gConfig.ftpEnabled || strlen(gConfig.ftpHost) == 0) {
    snprintf(error, errorSize, "FTPS disabled or host missing");
    return false;
  }

  if (gConfig.ftpsTrustMode > 1) {
    snprintf(error, errorSize, "FTPS trust mode invalid");
    return false;
  }

  NetworkInterface *network = Ethernet.getNetwork();
  if (network == nullptr) {
    snprintf(error, errorSize, "FTPS network unavailable");
    return false;
  }

  if (!gFtpsClient.begin(network, error, errorSize)) {
    ftpsAppendDiagnostics(error, errorSize);
    return false;
  }

  FtpsServerConfig serverConfig;
  serverConfig.host = gConfig.ftpHost;
  serverConfig.port = gConfig.ftpPort;
  serverConfig.user = (gConfig.ftpUser[0] != '\0') ? gConfig.ftpUser : "anonymous";
  serverConfig.password = (gConfig.ftpPass[0] != '\0') ? gConfig.ftpPass : "guest";
  serverConfig.tlsServerName = (gConfig.ftpsTlsServerName[0] != '\0') ? gConfig.ftpsTlsServerName : nullptr;
  serverConfig.trustMode = (gConfig.ftpsTrustMode == 1) ? FtpsTrustMode::ImportedCert : FtpsTrustMode::Fingerprint;
  serverConfig.fingerprint = (gConfig.ftpsFingerprint[0] != '\0') ? gConfig.ftpsFingerprint : nullptr;
  serverConfig.rootCaPem = nullptr;
  serverConfig.validateServerCert = true;

  if (serverConfig.trustMode == FtpsTrustMode::ImportedCert) {
    snprintf(error, errorSize, "FTPS imported cert mode not configured");
    return false;
  }
  if (serverConfig.fingerprint == nullptr || serverConfig.fingerprint[0] == '\0') {
    snprintf(error, errorSize, "FTPS fingerprint required");
    return false;
  }

  gFtpsClient.setTraceCallback(ftpsTraceCallback);
  setFtpsTransportTraceHook(ftpsTraceCallback);
  setFtpsClientProgressHook(ftpsProgressKick);
  writeFtpBackupMarker("ftps-connect:start");
  if (!gFtpsClient.connect(serverConfig, error, errorSize)) {
    writeFtpBackupMarker("ftps-connect:fail");
    ftpsAppendDiagnostics(error, errorSize);
    return false;
  }
  // Per-file control reconnect was originally enabled to work around an
  // Mbed-OS close-path issue that left the control channel in a zombie
  // state after the first STOR. The root cause has since been fixed in
  // the FTPS library (full TLS+TCP cleanup with reordered teardown), so
  // per-file reconnect is no longer needed and now actively harms us by
  // exhausting the LWIP socket pool with TIME_WAIT slots after 1-2
  // files. Leave control alive for the whole backup batch.
  gFtpsClient.setReconnectBetweenStores(false);
  writeFtpBackupMarker("ftps-connect:ok");
  return true;
}

static bool ftpsStoreBuffer(const char *remoteFile, const uint8_t *data, size_t len, char *error, size_t errorSize) {
  if (!data || len == 0) {
    snprintf(error, errorSize, "No data to upload");
    return false;
  }
  writeFtpBackupMarker("ftps-store:start");
  serviceTransferWatchdog();
  if (!gFtpsClient.store(remoteFile, data, len, error, errorSize)) {
    writeFtpBackupMarker("ftps-store:fail");
    ftpsAppendDiagnostics(error, errorSize);
    return false;
  }
  writeFtpBackupMarker("ftps-store:ok");
  serviceTransferWatchdog();

  // Note: the post-STOR isControlAlive() NOOP probe was removed 2026-04-17.
  // It mutated the session (15 s reply timeout fires on slow servers) and
  // could mark a healthy session dead, poisoning the per-file retry decision.
  // See CODE REVIEW/PER_FILE_RETRY_PLAN_04172026.md (Phase 0, P0-1).

  return true;
}

static bool ftpsRetrieveBuffer(const char *remoteFile, char *out, size_t outMax, size_t &outLen, char *error, size_t errorSize) {
  if (!out || outMax < 2) {
    snprintf(error, errorSize, "Buffer too small");
    return false;
  }
  serviceTransferWatchdog();
  if (!gFtpsClient.retrieve(remoteFile, (uint8_t *)out, outMax - 1, outLen, error, errorSize)) {
    ftpsAppendDiagnostics(error, errorSize);
    return false;
  }
  out[outLen] = '\0';
  serviceTransferWatchdog();
  return true;
}

static void ftpsQuit() {
  gFtpsClient.quit();
}

// ============================================================================
// National Weather Service API Integration
// ============================================================================
// Fetches weather data for calibration temperature compensation.
// Uses the NWS public API: https://api.weather.gov
// Flow: /points/{lat},{lon} -> get grid office/X/Y -> /gridpoints/{office}/{X},{Y} -> get temperature

// Look up NWS grid point for a client's location and cache it
static bool nwsLookupGridPoint(ClientMetadata *meta) {
  if (!meta || meta->latitude == 0.0f || meta->longitude == 0.0f) {
    return false;
  }
  
  // If already cached, don't re-fetch
  if (meta->nwsGridValid && meta->nwsGridOffice[0] != '\0') {
    return true;
  }
  
  EthernetClient client;
  if (!client.connect(NWS_API_HOST, NWS_API_PORT)) {
    Serial.println(F("NWS: Failed to connect to api.weather.gov"));
    return false;
  }
  
  // Build request URL: /points/{lat},{lon}
  char path[64];
  // Format with 4 decimal places (NWS max precision)
  snprintf(path, sizeof(path), "/points/%.4f,%.4f", meta->latitude, meta->longitude);
  
  // Send HTTP GET request with required User-Agent header
  client.print(F("GET "));
  client.print(path);
  client.println(F(" HTTP/1.1"));
  client.print(F("Host: "));
  client.println(F(NWS_API_HOST));
  client.println(F("User-Agent: TankAlarm/1.0 (sensor monitoring system)"));
  client.println(F("Accept: application/json"));
  client.println(F("Connection: close"));
  client.println();
  
  // Wait for response
  unsigned long start = millis();
  while (!client.available() && millis() - start < NWS_API_TIMEOUT_MS) {
    safeSleep(10);
  }
  
  if (!client.available()) {
    Serial.println(F("NWS: Timeout waiting for /points response"));
    client.stop();
    return false;
  }
  
  // Read response and parse JSON to find grid office, X, Y
  // Skip HTTP headers
  bool headersComplete = false;
  while (client.available() && !headersComplete) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) {
      headersComplete = true;
    }
  }
  
  // Read JSON body (limited to 4KB for safety)
  char buffer[4096];
  size_t len = 0;
  while (client.available() && len < sizeof(buffer) - 1 && millis() - start < NWS_API_TIMEOUT_MS) {
    buffer[len++] = client.read();
  }
  buffer[len] = '\0';
  client.stop();
  
  // Parse JSON response
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buffer);
  if (error) {
    Serial.print(F("NWS: JSON parse error: "));
    Serial.println(error.c_str());
    return false;
  }
  
  // Extract grid office and coordinates from properties
  // Response format: { "properties": { "gridId": "LWX", "gridX": 96, "gridY": 70 } }
  const char *gridId = doc["properties"]["gridId"];
  int gridX = doc["properties"]["gridX"] | -1;
  int gridY = doc["properties"]["gridY"] | -1;
  
  if (!gridId || gridX < 0 || gridY < 0) {
    Serial.println(F("NWS: Missing grid data in response"));
    return false;
  }
  
  // Cache the grid point
  strlcpy(meta->nwsGridOffice, gridId, sizeof(meta->nwsGridOffice));
  meta->nwsGridX = (uint16_t)gridX;
  meta->nwsGridY = (uint16_t)gridY;
  meta->nwsGridValid = true;
  
  Serial.print(F("NWS: Grid point cached: "));
  Serial.print(gridId);
  Serial.print(F(" ("));
  Serial.print(gridX);
  Serial.print(F(","));
  Serial.print(gridY);
  Serial.println(F(")"));
  
  return true;
}

// Fetch hourly temperature observations and calculate average for past N hours
static float nwsFetchAverageTemperature(ClientMetadata *meta, double timestamp) {
  if (!meta || !meta->nwsGridValid) {
    return TEMPERATURE_UNAVAILABLE;
  }
  
  EthernetClient client;
  if (!client.connect(NWS_API_HOST, NWS_API_PORT)) {
    Serial.println(F("NWS: Failed to connect for gridpoints"));
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Build request URL: /gridpoints/{office}/{X},{Y}
  char path[64];
  snprintf(path, sizeof(path), "/gridpoints/%s/%u,%u", 
           meta->nwsGridOffice, meta->nwsGridX, meta->nwsGridY);
  
  // Send HTTP GET request
  client.print(F("GET "));
  client.print(path);
  client.println(F(" HTTP/1.1"));
  client.print(F("Host: "));
  client.println(F(NWS_API_HOST));
  client.println(F("User-Agent: TankAlarm/1.0 (sensor monitoring system)"));
  client.println(F("Accept: application/json"));
  client.println(F("Connection: close"));
  client.println();
  
  // Wait for response
  unsigned long start = millis();
  while (!client.available() && millis() - start < NWS_API_TIMEOUT_MS) {
    safeSleep(10);
  }
  
  if (!client.available()) {
    Serial.println(F("NWS: Timeout waiting for gridpoints response"));
    client.stop();
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Skip HTTP headers
  bool headersComplete = false;
  while (client.available() && !headersComplete) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) {
      headersComplete = true;
    }
  }
  
  // Read JSON body (this can be large, limit to 16KB)
  char *buffer = (char *)malloc(16384);
  if (!buffer) {
    Serial.println(F("NWS: Failed to allocate buffer"));
    client.stop();
    return TEMPERATURE_UNAVAILABLE;
  }
  
  size_t len = 0;
  while (client.available() && len < 16383 && millis() - start < NWS_API_TIMEOUT_MS) {
    buffer[len++] = client.read();
  }
  buffer[len] = '\0';
  client.stop();
  
  // Parse JSON response
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, buffer);
  free(buffer);
  
  if (error) {
    Serial.print(F("NWS: JSON parse error: "));
    Serial.println(error.c_str());
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Extract temperature time series from properties.temperature.values
  // Format: { "properties": { "temperature": { "uom": "wmoUnit:degC", "values": [ { "validTime": "...", "value": 25.0 }, ... ] } } }
  JsonArray tempValues = doc["properties"]["temperature"]["values"];
  const char *tempUom = doc["properties"]["temperature"]["uom"];
  
  if (tempValues.isNull() || tempValues.size() == 0) {
    Serial.println(F("NWS: No temperature data in response"));
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Determine if temperatures are in Celsius (they usually are from NWS)
  bool isCelsius = (tempUom && strstr(tempUom, "degC") != nullptr);
  
  // Average the first NWS_AVG_HOURS temperature values (recent near-term conditions)
  float tempSum = 0.0f;
  int tempCount = 0;
  
  // The NWS data is forecast data, but recent values are essentially observations
  // We'll use any values that overlap with our window
  for (JsonObject entry : tempValues) {
    float tempValue = entry["value"] | TEMPERATURE_UNAVAILABLE;
    if (tempValue == TEMPERATURE_UNAVAILABLE) continue;
    
    // For simplicity, use the first several hourly values
    // (NWS gridpoint data represents recent/near-term conditions)
    if (tempCount < NWS_AVG_HOURS) {
      // Convert to Fahrenheit if needed
      float tempF = isCelsius ? (tempValue * 9.0f / 5.0f + 32.0f) : tempValue;
      tempSum += tempF;
      tempCount++;
    }
  }
  
  if (tempCount == 0) {
    Serial.println(F("NWS: No valid temperature values found"));
    return TEMPERATURE_UNAVAILABLE;
  }
  
  float avgTempF = tempSum / tempCount;
  
  Serial.print(F("NWS: Average temperature ("));
  Serial.print(tempCount);
  Serial.print(F(" readings): "));
  Serial.print(avgTempF, 1);
  Serial.println(F("°F"));
  
  return avgTempF;
}

// Main entry point: get calibration temperature for a client at a given time
static float nwsGetCalibrationTemperature(const char *clientUid, double timestamp) {
  if (!clientUid || strlen(clientUid) == 0) {
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Find client metadata with location
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (!meta || meta->latitude == 0.0f || meta->longitude == 0.0f) {
    Serial.print(F("NWS: No location data for client "));
    Serial.print(clientUid);
    Serial.println(F(" - requesting location from device"));
    
    // Request location from the client - it will respond asynchronously
    // Temperature will be available for future calibration entries
    sendLocationRequest(clientUid);
    
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Look up grid point (caches result)
  if (!nwsLookupGridPoint(meta)) {
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Fetch average temperature
  return nwsFetchAverageTemperature(meta, timestamp);
}

// Get current temperature for real-time telemetry compensation
// Uses cached value if fresh (< TEMP_CACHE_TTL_SECONDS old), otherwise fetches from NWS
static float getCachedTemperature(const char *clientUid) {
  if (!clientUid || strlen(clientUid) == 0) {
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Find client metadata
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (!meta) {
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Check if we have a valid cached temperature
  double now = currentEpoch();
  if (meta->cachedTemperatureF > TEMPERATURE_UNAVAILABLE + 1.0f &&
      meta->tempCacheEpoch > 0.0 &&
      (now - meta->tempCacheEpoch) < TEMP_CACHE_TTL_SECONDS) {
    // Cached temperature is still fresh
    return meta->cachedTemperatureF;
  }
  
  // Need to fetch fresh temperature - check if we have location
  if (meta->latitude == 0.0f || meta->longitude == 0.0f) {
    // No location - can't fetch temperature
    // Don't spam location requests here since telemetry comes frequently
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Look up grid point if needed
  if (!nwsLookupGridPoint(meta)) {
    return TEMPERATURE_UNAVAILABLE;
  }
  
  // Fetch current temperature (use current time)
  float temp = nwsFetchAverageTemperature(meta, now);
  
  // Cache the result
  if (temp > TEMPERATURE_UNAVAILABLE + 1.0f) {
    meta->cachedTemperatureF = temp;
    meta->tempCacheEpoch = now;
    Serial.print(F("Temperature cached for "));
    Serial.print(clientUid);
    Serial.print(F(": "));
    Serial.print(temp, 1);
    Serial.println(F("°F"));
  }
  
  return temp;
}

// Upload per-client configs (from in-memory snapshots) as individual files plus a manifest.
static bool ftpBackupClientConfigs(FtpSession &session, bool useFtps, char *error, size_t errorSize, uint8_t &uploadedFiles) {
  uploadedFiles = 0;
  if (gClientConfigCount == 0) {
    return true;  // Nothing to do
  }

  // Ensure the remote clients subdirectory exists before uploading snapshots.
  if (useFtps) {
    char clientsDir[192];
    buildRemotePath(clientsDir, sizeof(clientsDir), "clients");
    if (!gFtpsClient.mkd(clientsDir, error, errorSize)) {
      Serial.print(F("FTP backup: MKD clients warning (non-fatal): "));
      Serial.println(error);
      if (error && errorSize > 0) {
        error[0] = '\0';
      }
    }
  }

  // Build and upload manifest listing client UIDs (and optional site for readability)
  char manifest[2048];
  manifest[0] = 0;
  
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    strncat(manifest, gClientConfigs[i].uid, sizeof(manifest) - strlen(manifest) - 1);
    strncat(manifest, "\t", sizeof(manifest) - strlen(manifest) - 1);
    strncat(manifest, gClientConfigs[i].site, sizeof(manifest) - strlen(manifest) - 1);
    strncat(manifest, "\n", sizeof(manifest) - strlen(manifest) - 1);
  }

  char manifestPath[192];
  buildRemotePath(manifestPath, sizeof(manifestPath), "clients_manifest.txt");
  serviceTransferWatchdog();
  if ((useFtps && !ftpsStoreBuffer(manifestPath, (const uint8_t *)manifest, strlen(manifest), error, errorSize)) ||
      (!useFtps && !ftpStoreBuffer(session, manifestPath, (const uint8_t *)manifest, strlen(manifest), error, errorSize))) {
    return false;
  }
  uploadedFiles++;
  Serial.println(F("FTP backup: clients_manifest.txt"));

  // Upload each cached client config as its own file
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    ClientConfigSnapshot &snap = gClientConfigs[i];
    size_t len = strlen(snap.payload);
    if (len == 0 || len > FTP_MAX_FILE_BYTES) {
      continue;  // Skip empty/oversized
    }

    char remotePath[192];
    char fileName[96];
    snprintf(fileName, sizeof(fileName), "clients/%s.json", snap.uid);
    buildRemotePath(remotePath, sizeof(remotePath), fileName);

    serviceTransferWatchdog();
    if ((useFtps && ftpsStoreBuffer(remotePath, (const uint8_t *)snap.payload, len, error, errorSize)) ||
        (!useFtps && ftpStoreBuffer(session, remotePath, (const uint8_t *)snap.payload, len, error, errorSize))) {
      uploadedFiles++;
      Serial.print(F("FTP backup client config: "));
      Serial.println(remotePath);
    }
  }

  return true;
}

// Download per-client configs if present and rebuild the cache file locally.
static bool ftpRestoreClientConfigs(FtpSession &session, bool useFtps, char *error, size_t errorSize, uint8_t &restoredFiles) {
  restoredFiles = 0;

  char manifestPath[192];
  buildRemotePath(manifestPath, sizeof(manifestPath), "clients_manifest.txt");

  char manifest[2048];
  size_t manifestLen = 0;
  serviceTransferWatchdog();
  if ((useFtps && !ftpsRetrieveBuffer(manifestPath, manifest, sizeof(manifest), manifestLen, error, errorSize)) ||
      (!useFtps && !ftpRetrieveBuffer(session, manifestPath, manifest, sizeof(manifest), manifestLen, error, errorSize))) {
    // Manifest not found; treat as optional and succeed silently.
    return true;
  }

  // Accumulate cache content into a String, then write atomically
  String cacheContent;
  cacheContent.reserve(2048);

  // Parse manifest lines: uid \t site
  char *lineStart = manifest;
  char *manifestEnd = manifest + manifestLen;
  
  while (lineStart < manifestEnd) {
    char *lineEnd = strchr(lineStart, '\n');
    if (!lineEnd) lineEnd = manifestEnd;
    
    char savedChar = *lineEnd;
    *lineEnd = 0;
    
    char *line = lineStart;
    while(isspace(*line)) line++;
    
    if (*line == 0) {
        *lineEnd = savedChar;
        lineStart = lineEnd + 1;
        continue;
    }

    char *sep = strchr(line, '\t');
    if (sep) *sep = 0; 
    
    char *uid = line;
    // Trim UID
    char *p = uid + strlen(uid) - 1;
    while(p >= uid && isspace(*p)) *p-- = 0;

    if (strlen(uid) > 0) {
        char remotePath[192];
        char fileName[96];
        snprintf(fileName, sizeof(fileName), "clients/%s.json", uid);
        buildRemotePath(remotePath, sizeof(remotePath), fileName);
        
        // Use 4KB buffer to accommodate larger client configs (was 1KB, risked truncation)
        char cfg[4096];
        size_t cfgLen = 0;
        serviceTransferWatchdog();
        if ((useFtps && ftpsRetrieveBuffer(remotePath, cfg, sizeof(cfg), cfgLen, error, errorSize)) ||
          (!useFtps && ftpRetrieveBuffer(session, remotePath, cfg, sizeof(cfg), cfgLen, error, errorSize))) {
            // Trim cfg
            char *cStart = cfg;
            while(*cStart && isspace(*cStart)) cStart++;
            
            char *cEnd = cfg + cfgLen - 1;
            while(cEnd >= cStart && isspace(*cEnd)) *cEnd-- = 0;
            
            if (strlen(cStart) > 0) {
                cacheContent += uid;
                cacheContent += '\t';
                cacheContent += cStart;
                cacheContent += '\n';
                restoredFiles++;
                Serial.print(F("FTP restore client config: "));
                Serial.println(remotePath);
            }
        }
    }
    
    if (sep) *sep = '\t';
    *lineEnd = savedChar;
    lineStart = lineEnd + 1;
  }
  
  // Write accumulated cache content atomically
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!serverWriteFileAtomic("/fs/client_config_cache.txt",
                             cacheContent.c_str(), cacheContent.length())) {
    snprintf(error, errorSize, "Failed to write cache file");
    return false;
  }
#else
  if (!tankalarm_littlefs_write_file_atomic(CLIENT_CONFIG_CACHE_PATH,
          (const uint8_t *)cacheContent.c_str(), cacheContent.length())) {
    snprintf(error, errorSize, "Failed to write cache file");
    return false;
  }
#endif
  return true;
}

// FTP backup with detailed result reporting
//
// SECURITY WARNING: If FTPS is disabled, this function transmits sensitive
// configuration data over unencrypted FTP, which is vulnerable to interception.
//
// RECOMMENDED SECURITY MEASURES:
// - Use FTP only on physically isolated/trusted networks (e.g., dedicated management VLAN)
// - Enable firewall rules to restrict FTP access to specific hosts
// - Consider using VPN/IPsec for network-layer encryption
// - Rotate FTP credentials regularly and use strong passwords
// - Monitor FTP server logs for unauthorized access attempts
//
// FTPS mode is supported through gConfig.ftpsEnabled.
static FtpResult performFtpBackupDetailed() {
  FtpResult result;
  bool abortRemainingTransfers = false;
  
  if (!gConfig.ftpEnabled) {
    strlcpy(result.errorMessage, "FTP disabled", sizeof(result.errorMessage));
    return result;
  }

  if (!prepareLocalBackupFilesForFtp(result)) {
    return result;
  }

  // Patch C: pre-scan /fs before opening an FTP/FTPS session. If there is
  // nothing to upload, bail immediately instead of burning seconds on TLS
  // handshake + 9 failed reads + teardown while the HTTP peer is waiting.
#if defined(FILESYSTEM_AVAILABLE) && (defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED))
  uint8_t readable = 0;
  for (size_t i = 0; i < sizeof(kBackupFiles) / sizeof(kBackupFiles[0]); ++i) {
    char probe[96];
    buildLocalPath(kBackupFiles[i].localPath, probe, sizeof(probe));
    FILE *f = fopen(probe, "rb");
    if (f) {
      readable++;
      fclose(f);
    }
  }
  Serial.print(F("performFtpBackup: pre-scan readable="));
  Serial.print(readable);
  Serial.print(F("/"));
  Serial.print((unsigned)(sizeof(kBackupFiles) / sizeof(kBackupFiles[0])));
  Serial.print(F(" clientSnapshots="));
  Serial.println(gClientConfigCount);
  Serial.flush();
  if (readable == 0 && gClientConfigCount == 0) {
    strlcpy(result.errorMessage,
            "No local files available to upload (check /fs)",
            sizeof(result.errorMessage));
    return result;
  }
#endif

  serviceTransferWatchdog();

  const bool useFtps = gConfig.ftpsEnabled && (FTP_BACKUP_FORCE_PLAIN_FTP_DIAG == 0);
  const bool reconnectPerFile = useFtps && (FTP_BACKUP_FTPS_RECONNECT_PER_FILE_DIAG != 0);
  FtpSession session;
  char err[128];
  err[0] = '\0';
  bool connected = true;
  if (!reconnectPerFile) {
    connected = useFtps ? ftpsConnectAndLogin(err, sizeof(err))
                        : ftpConnectAndLogin(session, err, sizeof(err));
    if (!connected) {
      strlcpy(result.errorMessage, err, sizeof(result.errorMessage));
      return result;
    }

    // Create the per-server directory if it does not already exist.
    // Best-effort: a failure here is logged but does not abort the backup.
    if (useFtps) {
      char remoteBaseDir[128];
      buildRemoteBaseDir(remoteBaseDir, sizeof(remoteBaseDir));
      if (!gFtpsClient.mkd(remoteBaseDir, err, sizeof(err))) {
        Serial.print(F("FTP backup: MKD warning (non-fatal): "));
        Serial.println(err);
      }
    }
  }

  uint8_t consecutiveFailFiles = 0;
  bool forcedRetriableInjected = false;
  for (size_t i = 0; i < sizeof(kBackupFiles) / sizeof(kBackupFiles[0]); ++i) {
    if (abortRemainingTransfers) {
      break;
    }

    // Inter-file delay: Opta LWIP pool is hard-baked at 4 PCBs (libmbed.a is
    // precompiled so MBED_CONF_LWIP_SOCKET_MAX cannot be raised). SO_LINGER is
    // unsupported (-3002) so each closed data socket sits in TIME_WAIT for
    // ~60s. Without waiting, the 2nd or 3rd file fails -3005 NO_SOCKET.
    // Sleep FTP_BACKUP_INTER_FILE_DELAY_MS (default 65s) between files
    // (after the first) to let TIME_WAIT drain; service the 30s watchdog
    // every 100ms so we don't reset.
    //
    // SPEED-OPT (inter-file delay): The retry loop below recovers from
    // occasional -3005 failures, so this delay can be reduced once retry
    // telemetry shows it is safe. Target 45000UL after a few clean cycles.
    // See CODE REVIEW/PER_FILE_RETRY_PLAN_04172026.md (Phase 3).
    if (i > 0 && useFtps) {
      Serial.println(F("FTPS phase: inter-file:wait-tw"));
      uint32_t waitStart = millis();
      while (millis() - waitStart < FTP_BACKUP_INTER_FILE_DELAY_MS) {
        serviceTransferWatchdog();
        delay(100);
      }
      Serial.println(F("FTPS phase: inter-file:wait-done"));
    }

    serviceTransferWatchdog();

    const BackupFileEntry &entry = kBackupFiles[i];
    static char contents[FTP_MAX_FILE_BYTES + 1];
    size_t len = 0;
    if (!readFileToBuffer(entry.localPath, contents, sizeof(contents), len)) {
      // Log every skip so we can see why a backup produced no STORs.
      // Common causes: file absent on /fs, larger than buffer, or FS error.
      Serial.print(F("FTP backup skip (unreadable): "));
      Serial.print(entry.localPath);
      Serial.print(F(" -> "));
      Serial.println(entry.remoteName);
      if (entry.required) {
        result.filesFailed++;
        result.addFailedFile(entry.remoteName);
      }
      continue;
    }

    char remotePath[192];
    buildRemotePath(remotePath, sizeof(remotePath), entry.remoteName);
    if (reconnectPerFile) {
      if (!ftpsConnectAndLogin(err, sizeof(err))) {
        abortRemainingTransfers = true;
        strlcpy(result.errorMessage, err, sizeof(result.errorMessage));
        break;
      }
      char remoteBaseDir[128];
      buildRemoteBaseDir(remoteBaseDir, sizeof(remoteBaseDir));
      if (!gFtpsClient.mkd(remoteBaseDir, err, sizeof(err))) {
        Serial.print(F("FTP backup: MKD warning (non-fatal): "));
        Serial.println(err);
      }
    }

    // --- Per-file bounded retry (FTPS only; plain FTP keeps single-shot) ---
    // Plan: PER_FILE_RETRY_PLAN_04172026.md.
    // - Retry only on transient data-channel faults (DataConnectionFailed
    //   with NSAPI -3005, or DataTlsHandshakeFailed).
    // - Stepped backoff: 20s, then 40s.
    // - Abort the batch if the control channel is gone, or if too many
    //   files in a row exhaust their retry budget.
    bool stored = false;
    uint8_t attempts = 0;
    while (attempts < FTP_BACKUP_PER_FILE_MAX_ATTEMPTS) {
      attempts++;
      err[0] = '\0';
      stored = useFtps
                 ? ftpsStoreBuffer(remotePath, (const uint8_t *)contents, len, err, sizeof(err))
                 : ftpStoreBuffer(session, remotePath, (const uint8_t *)contents, len, err, sizeof(err));

      FtpsError lastErr = gFtpsClient.lastError();
      int lastNsapi = gFtpsClient.lastNsapiError();
      if (useFtps && FTP_BACKUP_TEST_FORCE_ONE_RETRY != 0 && !forcedRetriableInjected &&
          attempts == 1 && stored && strcmp(entry.remoteName, FTP_BACKUP_TEST_FORCE_ONE_RETRY_TARGET) == 0) {
        forcedRetriableInjected = true;
        stored = false;
        strlcpy(err, "injected retriable failure (test)", sizeof(err));
        lastErr = FtpsError::DataConnectionFailed;
        lastNsapi = -3005;
        Serial.print(F("ftp-retry-test: injected retriable failure for "));
        Serial.println(entry.remoteName);
      }

      if (stored) break;
      if (!useFtps) break;  // Plain-FTP path keeps single-shot semantics.

      if (ftpsIsSessionDead(lastErr)) {
        // Control channel gone: no point retrying or continuing the batch.
        break;
      }
      if (!ftpsIsTransferRetriable(lastErr, lastNsapi)) {
        // Permanent transfer failure (server reject, persistent network
        // condition). Skip this file but keep the batch.
        break;
      }
      if (attempts >= FTP_BACKUP_PER_FILE_MAX_ATTEMPTS) break;

      const uint32_t waitMs = (attempts == 1) ? FTP_BACKUP_RETRY_BACKOFF_1_MS
                                              : FTP_BACKUP_RETRY_BACKOFF_2_MS;
      Serial.print(F("ftp-retry: file="));
      Serial.print(entry.remoteName);
      Serial.print(F(" attempt="));
      Serial.print(attempts + 1);
      Serial.print(F("/"));
      Serial.print(FTP_BACKUP_PER_FILE_MAX_ATTEMPTS);
      Serial.print(F(" nsapi="));
      Serial.print(lastNsapi);
      Serial.print(F(" wait_ms="));
      Serial.println(waitMs);
      result.retryAttemptsTotal++;
      uint32_t retryStart = millis();
      while (millis() - retryStart < waitMs) {
        serviceTransferWatchdog();
        delay(100);
      }
    }

    // Workaround for control-channel resets immediately after STOR on some
    // stacks: if reconnect-per-file mode is active, reconnect and verify SIZE.
    if (!stored && reconnectPerFile && useFtps && len > 0 &&
        ftpsSessionLikelyDead(gFtpsClient.lastError())) {
      char verifyErr[128] = {0};
      size_t remoteBytes = 0;
      if (ftpsConnectAndLogin(verifyErr, sizeof(verifyErr)) &&
          gFtpsClient.size(remotePath, remoteBytes, verifyErr, sizeof(verifyErr)) &&
          remoteBytes == len) {
        stored = true;
        Serial.print(F("FTP backup verified via SIZE after reset: "));
        Serial.println(remotePath);
      }
      ftpsQuit();
    }

    if (reconnectPerFile) {
      ftpsQuit();
    }
    if (stored) {
      result.filesProcessed++;
      if (attempts > 1) {
        result.filesRecoveredByRetry++;
        Serial.print(F("ftp-retry: recovered file="));
        Serial.print(remotePath);
        Serial.print(F(" after attempts="));
        Serial.println(attempts);
      }
      consecutiveFailFiles = 0;
      Serial.print(F("FTP backup: "));
      Serial.println(remotePath);
    } else {
      result.filesFailed++;
      result.addFailedFile(entry.remoteName);
      Serial.print(F("FTP upload failed for "));
      Serial.print(remotePath);
      if (err[0] != '\0') {
        Serial.print(F(" err='"));
        Serial.print(err);
        Serial.print(F("'"));
      }
      Serial.println();

      if (useFtps) {
        const FtpsError lastErr = gFtpsClient.lastError();
        if (ftpsIsSessionDead(lastErr)) {
          abortRemainingTransfers = true;
          if (result.errorMessage[0] == '\0') {
            strlcpy(result.errorMessage,
                    "FTPS control channel dropped; aborted remaining uploads",
                    sizeof(result.errorMessage));
          }
        } else if (attempts >= FTP_BACKUP_PER_FILE_MAX_ATTEMPTS &&
                   ftpsIsTransferRetriable(lastErr, gFtpsClient.lastNsapiError())) {
          // File burned its full retry budget on a retriable error. Track
          // consecutive occurrences; abort the batch if it keeps happening
          // because the LWIP pool / server is sustained-saturated and the
          // remaining files would only burn more retry waits.
          result.filesRetryExhausted++;
          consecutiveFailFiles++;
          if (consecutiveFailFiles >= FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES) {
            abortRemainingTransfers = true;
            if (result.errorMessage[0] == '\0') {
              snprintf(result.errorMessage, sizeof(result.errorMessage),
                       "Aborted: %u consecutive files exhausted retries",
                       (unsigned)consecutiveFailFiles);
            }
            Serial.print(F("ftp-retry: aborting batch after "));
            Serial.print(consecutiveFailFiles);
            Serial.println(F(" consecutive retry-exhausted files"));
          }
        } else {
          // Non-retriable but session-alive failure (server 5xx, etc.):
          // keep batch going, reset the consecutive counter so a single
          // bad file does not trip the abort threshold.
          consecutiveFailFiles = 0;
        }
      }
    }
  }

  // Also back up per-client cached configs (manifest + per-uid JSON)
  uint8_t clientUploaded = 0;
  if (!abortRemainingTransfers && reconnectPerFile) {
    // Keep this diagnostic mode scoped to baseline file uploads only. Client
    // snapshots can involve many transfers and would reintroduce mixed behavior.
    Serial.println(F("FTP backup: client snapshot uploads skipped (diag reconnect mode)"));
  } else if (!abortRemainingTransfers &&
      ftpBackupClientConfigs(session, useFtps, err, sizeof(err), clientUploaded)) {
    result.filesProcessed += clientUploaded;
  } else if (!abortRemainingTransfers && err[0] != '\0' && result.errorMessage[0] == '\0') {
    strlcpy(result.errorMessage, err, sizeof(result.errorMessage));
  }

  if (useFtps && !reconnectPerFile) {
    ftpsQuit();
  } else if (!useFtps) {
    ftpQuit(session);
  }
  serviceTransferWatchdog();

  result.success = (result.filesProcessed > 0);
  if (result.success && result.filesFailed > 0 && result.errorMessage[0] == '\0') {
    snprintf(result.errorMessage, sizeof(result.errorMessage),
             "%u files uploaded, %u failed",
             (unsigned)result.filesProcessed, (unsigned)result.filesFailed);
  }
  if (!result.success && result.errorMessage[0] == '\0') {
    // Connection + login succeeded, but no file was readable from /fs — make
    // that visible instead of returning an empty "Backup failed".
    strlcpy(result.errorMessage,
            "Connected, but no local files available to upload (check /fs)",
            sizeof(result.errorMessage));
  }
  return result;
}

// Simplified wrapper for performFtpBackupDetailed()
static bool performFtpBackup(char *errorOut, size_t errorSize) {
  FtpResult result = performFtpBackupDetailed();
  if (errorOut && errorSize > 0) {
    strlcpy(errorOut, result.errorMessage, errorSize);
  }
  return result.success;
}

// FTP restore with detailed result reporting
// 
// SECURITY WARNING: If FTPS is disabled, this function transmits sensitive
// configuration data over unencrypted FTP, which is vulnerable to interception.
//
// RECOMMENDED SECURITY MEASURES:
// - Use FTP only on physically isolated/trusted networks (e.g., dedicated management VLAN)
// - Enable firewall rules to restrict FTP access to specific hosts
// - Consider using VPN/IPsec for network-layer encryption
// - Rotate FTP credentials regularly and use strong passwords
// - Monitor FTP server logs for unauthorized access attempts
//
// FTPS mode is supported through gConfig.ftpsEnabled.
static FtpResult performFtpRestoreDetailed() {
  FtpResult result;
  
  if (!gConfig.ftpEnabled) {
    strlcpy(result.errorMessage, "FTP disabled", sizeof(result.errorMessage));
    return result;
  }

  serviceTransferWatchdog();

  const bool useFtps = gConfig.ftpsEnabled;
  FtpSession session;
  char err[128];
  bool connected = useFtps ? ftpsConnectAndLogin(err, sizeof(err))
                           : ftpConnectAndLogin(session, err, sizeof(err));
  if (!connected) {
    strlcpy(result.errorMessage, err, sizeof(result.errorMessage));
    return result;
  }

  for (size_t i = 0; i < sizeof(kBackupFiles) / sizeof(kBackupFiles[0]); ++i) {
    serviceTransferWatchdog();

    const BackupFileEntry &entry = kBackupFiles[i];
    char contents[2048];
    size_t len = 0;

    char remotePath[192];
    buildRemotePath(remotePath, sizeof(remotePath), entry.remoteName);
    bool retrieved = useFtps
                       ? ftpsRetrieveBuffer(remotePath, contents, sizeof(contents), len, err, sizeof(err))
                       : ftpRetrieveBuffer(session, remotePath, contents, sizeof(contents), len, err, sizeof(err));
    if (!retrieved) {
      result.filesFailed++;
      result.addFailedFile(entry.remoteName);
      continue;
    }

    if (writeBufferToFile(entry.localPath, (const uint8_t *)contents, len)) {
      result.filesProcessed++;
      Serial.print(F("FTP restore: "));
      Serial.println(entry.localPath);
    } else {
      result.filesFailed++;
      result.addFailedFile(entry.remoteName);
    }
  }

  // Attempt to restore per-client cached configs (optional)
  uint8_t clientRestored = 0;
  if (ftpRestoreClientConfigs(session, useFtps, err, sizeof(err), clientRestored)) {
    result.filesProcessed += clientRestored;
  }

  if (useFtps) {
    ftpsQuit();
  } else {
    ftpQuit(session);
  }
  serviceTransferWatchdog();

  result.success = (result.filesProcessed > 0);
  if (!result.success) {
    strlcpy(result.errorMessage, "No files restored", sizeof(result.errorMessage));
  } else if (result.filesFailed > 0) {
    snprintf(result.errorMessage, sizeof(result.errorMessage), 
             "%d files restored, %d failed", result.filesProcessed, result.filesFailed);
  }

  return result;
}

// Legacy wrapper for backward compatibility
static bool performFtpRestore(char *errorOut, size_t errorSize) {
  FtpResult result = performFtpRestoreDetailed();
  if (errorOut && errorSize > 0) {
    strlcpy(errorOut, result.errorMessage, errorSize);
  }
  return result.success;
}

// ============================================================================
// Historical Data & Tiered Storage Implementation
// ============================================================================

// Log an alarm event to the ring buffer
static void logAlarmEvent(const char *clientUid, const char *siteName, uint8_t sensorIndex, float level, bool isHigh) {
  AlarmLogEntry &entry = alarmLog[alarmLogWriteIndex];
  
  // Get current time from Notecard
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  
  entry.timestamp = now;
  strlcpy(entry.siteName, siteName ? siteName : "Unknown", sizeof(entry.siteName));
  strlcpy(entry.clientUid, clientUid ? clientUid : "", sizeof(entry.clientUid));
  entry.sensorIndex = sensorIndex;
  entry.level = level;
  entry.isHigh = isHigh;
  entry.cleared = false;
  entry.clearedTimestamp = 0.0;
  
  // Advance ring buffer
  alarmLogWriteIndex = (alarmLogWriteIndex + 1) % MAX_ALARM_LOG_ENTRIES;
  if (alarmLogCount < MAX_ALARM_LOG_ENTRIES) {
    alarmLogCount++;
  }
  
  Serial.print(F("Alarm logged: "));
  Serial.print(siteName);
  Serial.print(F(" Sensor "));
  Serial.print(sensorIndex);
  Serial.println(isHigh ? F(" HIGH") : F(" LOW"));
}

// Mark an alarm as cleared
static void clearAlarmEvent(const char *clientUid, uint8_t sensorIndex) {
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  
  // Find most recent uncleared alarm for this sensor
  for (int i = alarmLogCount - 1; i >= 0; i--) {
    int idx = (alarmLogWriteIndex - 1 - (alarmLogCount - 1 - i) + MAX_ALARM_LOG_ENTRIES) % MAX_ALARM_LOG_ENTRIES;
    if (strcmp(alarmLog[idx].clientUid, clientUid) == 0 && 
        alarmLog[idx].sensorIndex == sensorIndex && 
        !alarmLog[idx].cleared) {
      alarmLog[idx].cleared = true;
      alarmLog[idx].clearedTimestamp = now;
      break;
    }
  }
}

// ============================================================================
// Transmission Log Helper
// ============================================================================
static void logTransmission(const char *clientUid, const char *site, const char *messageType, const char *status, const char *detail) {
  TransmissionLogEntry &entry = gTransmissionLog[gTransmissionLogWriteIndex];

  // Get current time from Notecard epoch tracker
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }

  entry.timestamp = now;
  strlcpy(entry.siteName, site ? site : "", sizeof(entry.siteName));
  strlcpy(entry.clientUid, clientUid ? clientUid : "", sizeof(entry.clientUid));
  strlcpy(entry.messageType, messageType ? messageType : "unknown", sizeof(entry.messageType));
  strlcpy(entry.status, status ? status : "outbox", sizeof(entry.status));
  strlcpy(entry.detail, detail ? detail : "", sizeof(entry.detail));

  // Advance ring buffer
  gTransmissionLogWriteIndex = (gTransmissionLogWriteIndex + 1) % MAX_TRANSMISSION_LOG_ENTRIES;
  if (gTransmissionLogCount < MAX_TRANSMISSION_LOG_ENTRIES) {
    gTransmissionLogCount++;
  }

  Serial.print(F("TX Log: "));
  Serial.print(messageType);
  Serial.print(F(" -> "));
  Serial.print(clientUid ? clientUid : "(server)");
  Serial.print(F(" ["));
  Serial.print(status);
  Serial.println(F("]"));
}

// Find or create a sensor history entry
static SensorHourlyHistory *findOrCreateSensorHistory(const char *clientUid, uint8_t sensorIndex) {
  // Search existing entries
  for (uint8_t i = 0; i < gSensorHistoryCount; i++) {
    if (strcmp(gSensorHistory[i].clientUid, clientUid) == 0 && 
        gSensorHistory[i].sensorIndex == sensorIndex) {
      return &gSensorHistory[i];
    }
  }
  
  // Create new entry if space available
  if (gSensorHistoryCount < MAX_HISTORY_SENSORS) {
    SensorHourlyHistory &hist = gSensorHistory[gSensorHistoryCount];
    strlcpy(hist.clientUid, clientUid, sizeof(hist.clientUid));
    hist.sensorIndex = sensorIndex;
    hist.siteName[0] = '\0';
    hist.heightInches = 120.0f;
    hist.snapshotCount = 0;
    hist.writeIndex = 0;
    gSensorHistoryCount++;
    return &hist;
  }
  
  return nullptr;
}

// Record a telemetry snapshot (called hourly or on significant change)
static void recordTelemetrySnapshot(const char *clientUid, const char *siteName, uint8_t sensorIndex, float heightInches, float level, float voltage) {
  SensorHourlyHistory *hist = findOrCreateSensorHistory(clientUid, sensorIndex);
  if (!hist) {
    static unsigned long lastHistFullWarn = 0;
    if (millis() - lastHistFullWarn > 300000UL) {
      lastHistFullWarn = millis();
      Serial.print(F("WARNING: History slots full ("));
      Serial.print(MAX_HISTORY_SENSORS);
      Serial.print(F("/"));
      Serial.print(MAX_HISTORY_SENSORS);
      Serial.println(F(") — new sensor history dropped"));
      addServerSerialLog("History slots full — sensor history dropped", "warn", "history");
    }
    return;
  }
  
  // Update metadata
  if (siteName && siteName[0] != '\0') {
    strlcpy(hist->siteName, siteName, sizeof(hist->siteName));
  }
  hist->heightInches = heightInches;
  
  // Get current time
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  
  // Add snapshot to ring buffer
  TelemetrySnapshot &snap = hist->snapshots[hist->writeIndex];
  snap.timestamp = now;
  snap.level = level;
  snap.voltage = voltage;
  
  hist->writeIndex = (hist->writeIndex + 1) % MAX_HOURLY_HISTORY_PER_SENSOR;
  if (hist->snapshotCount < MAX_HOURLY_HISTORY_PER_SENSOR) {
    hist->snapshotCount++;
  }
}

// Check LittleFS usage and prune old data if needed
static void pruneHotTierIfNeeded() {
  // Check if it's been at least 24 hours since last prune
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  
  if (now > 0.0 && (now - gHistorySettings.lastPruneEpoch) < 86400.0) {
    return;  // Prune at most once per day
  }
  
  // Calculate hot tier retention threshold
  double cutoffEpoch = now - (gHistorySettings.hotTierRetentionDays * 86400.0);
  uint32_t pruned = 0;
  
  // Prune old telemetry snapshots from each sensor
  for (uint8_t i = 0; i < gSensorHistoryCount; i++) {
    SensorHourlyHistory &hist = gSensorHistory[i];
    uint16_t newCount = 0;
    
    // Keep only snapshots newer than cutoff
    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      if (hist.snapshots[idx].timestamp >= cutoffEpoch) {
        newCount++;
      } else {
        pruned++;
      }
    }
    
    // Update count — this is correct because timestamps are monotonically
    // increasing, so all pruned entries are contiguous at the start of the
    // logical sequence. Reducing snapshotCount shifts the logical start
    // forward while writeIndex remains valid for the next write.
    hist.snapshotCount = newCount;
  }
  
  // Prune old alarms (keep alarms for longer - 30 days)
  double alarmCutoff = now - (30.0 * 86400.0);
  uint8_t newAlarmCount = 0;
  for (uint8_t i = 0; i < alarmLogCount; i++) {
    int idx = (alarmLogWriteIndex - alarmLogCount + i + MAX_ALARM_LOG_ENTRIES) % MAX_ALARM_LOG_ENTRIES;
    if (alarmLog[idx].timestamp >= alarmCutoff) {
      newAlarmCount++;
    }
  }
  alarmLogCount = newAlarmCount;
  
  gHistorySettings.lastPruneEpoch = now;
  gHistorySettings.totalRecordsPruned += pruned;
  
  if (pruned > 0) {
    Serial.print(F("Pruned "));
    Serial.print(pruned);
    Serial.println(F(" old history records"));
  }
}

// Archive a specific month's data to FTP (cold tier)
// Filters snapshots and alarms to only include data from the target month.
// Idempotent: skips if already archived this month (based on lastFtpSyncEpoch).
static bool archiveMonthToFtp(uint16_t year, uint8_t month) {
  if (!gConfig.ftpEnabled) {
    return false;
  }
  
  // Idempotency: skip if we already archived after the target month ended
  // Calculate first epoch of the month AFTER the target month
  struct tm targetEnd = {};
  targetEnd.tm_year = year - 1900;
  targetEnd.tm_mon = month;  // month is 1-based, so this is actually next month (0-based)
  targetEnd.tm_mday = 1;
  if (month == 12) {
    targetEnd.tm_year++;
    targetEnd.tm_mon = 0;
  }
  double nextMonthEpoch = (double)mktime(&targetEnd);
  if (gHistorySettings.lastFtpSyncEpoch >= nextMonthEpoch) {
    // Already archived for this month or later
    return true;
  }
  
  // Calculate epoch range for the target month
  struct tm targetStart = {};
  targetStart.tm_year = year - 1900;
  targetStart.tm_mon = month - 1;  // 0-based
  targetStart.tm_mday = 1;
  double monthStartEpoch = (double)mktime(&targetStart);
  double monthEndEpoch = nextMonthEpoch;
  
  // Build JSON document with monthly summary
  JsonDocument doc;
  doc["year"] = year;
  doc["month"] = month;
  doc["generated"] = gLastSyncedEpoch > 0.0 ? gLastSyncedEpoch : 0.0;
  
  JsonArray sensorsArray = doc["sensorSummaries"].to<JsonArray>();
  uint8_t sensorsWithData = 0;
  
  // For each sensor, compute monthly summary from hourly data filtered by month
  for (uint8_t i = 0; i < gSensorHistoryCount; i++) {
    SensorHourlyHistory &hist = gSensorHistory[i];
    if (hist.snapshotCount == 0) continue;
    
    // Calculate summary stats — only for snapshots within the target month
    float minLevel = 999999.0f;
    float maxLevel = -999999.0f;
    float sumLevel = 0.0f;
    float sumVoltage = 0.0f;
    uint16_t count = 0;
    uint16_t voltageCount = 0;
    
    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      TelemetrySnapshot &snap = hist.snapshots[idx];
      
      // Filter: only include snapshots from the target month
      if (snap.timestamp < monthStartEpoch || snap.timestamp >= monthEndEpoch) continue;
      
      if (snap.level < minLevel) minLevel = snap.level;
      if (snap.level > maxLevel) maxLevel = snap.level;
      sumLevel += snap.level;
      count++;
      
      if (snap.voltage > 0) {
        sumVoltage += snap.voltage;
        voltageCount++;
      }
    }
    
    // Only emit sensor entry if it had data in the target month
    if (count == 0) continue;
    
    JsonObject sensorObj = sensorsArray.add<JsonObject>();
    sensorObj["clientUid"] = hist.clientUid;
    sensorObj["site"] = hist.siteName;
    sensorObj["sensorIndex"] = hist.sensorIndex;
    sensorObj["heightInches"] = hist.heightInches;
    sensorObj["minLevel"] = roundTo(minLevel, 1);
    sensorObj["maxLevel"] = roundTo(maxLevel, 1);
    sensorObj["avgLevel"] = roundTo(sumLevel / count, 1);
    sensorObj["avgVoltage"] = voltageCount > 0 ? roundTo(sumVoltage / voltageCount, 2) : 0.0f;
    sensorObj["readings"] = count;
    sensorsWithData++;
  }
  
  // If hot tier had no data, try warm tier daily summaries for this month
  if (sensorsWithData == 0) {
    JsonDocument warmDoc;
    if (loadDailySummaryMonth(year, month, warmDoc) && warmDoc.is<JsonArray>()) {
      // Aggregate daily summaries per sensor into monthly summaries
      struct WarmSummary { char clientUid[48]; uint8_t sensorIndex; float minL; float maxL; float sumL; float sumV; uint16_t count; uint16_t voltCount; };
      WarmSummary warmed[MAX_HISTORY_SENSORS];
      uint8_t warmCount = 0;

      for (JsonObject de : warmDoc.as<JsonArray>()) {
        const char *wUid = de["c"] | "";
        uint8_t wIdx = de["k"] | 0;
        float dMin = de["mn"] | 999999.0f;
        float dMax = de["mx"] | -999999.0f;
        float dAvg = de["av"] | 0.0f;
        float dVt  = de["vt"] | 0.0f;
        uint16_t dN = de["n"] | (uint16_t)1;

        // Find or create warm summary slot
        WarmSummary *ws = nullptr;
        for (uint8_t w = 0; w < warmCount; ++w) {
          if (strcmp(warmed[w].clientUid, wUid) == 0 && warmed[w].sensorIndex == wIdx) { ws = &warmed[w]; break; }
        }
        if (!ws && warmCount < MAX_HISTORY_SENSORS) {
          ws = &warmed[warmCount++];
          strlcpy(ws->clientUid, wUid, sizeof(ws->clientUid));
          ws->sensorIndex = wIdx;
          ws->minL = 999999.0f; ws->maxL = -999999.0f;
          ws->sumL = 0.0f; ws->sumV = 0.0f;
          ws->count = 0; ws->voltCount = 0;
        }
        if (!ws) continue;

        if (dMin < ws->minL) ws->minL = dMin;
        if (dMax > ws->maxL) ws->maxL = dMax;
        ws->sumL += dAvg * dN;
        ws->count += dN;
        if (dVt > 0.0f) { ws->sumV += dVt * dN; ws->voltCount += dN; }
      }

      for (uint8_t w = 0; w < warmCount; ++w) {
        WarmSummary &ws = warmed[w];
        if (ws.count == 0) continue;
        JsonObject sensorObj = sensorsArray.add<JsonObject>();
        sensorObj["clientUid"] = ws.clientUid;
        sensorObj["sensorIndex"] = ws.sensorIndex;
        sensorObj["minLevel"] = roundTo(ws.minL, 1);
        sensorObj["maxLevel"] = roundTo(ws.maxL, 1);
        sensorObj["avgLevel"] = roundTo(ws.sumL / ws.count, 1);
        sensorObj["avgVoltage"] = ws.voltCount > 0 ? roundTo(ws.sumV / ws.voltCount, 2) : 0.0f;
        sensorObj["readings"] = ws.count;
        sensorObj["dataSource"] = "warm";
        sensorsWithData++;
      }
    }
  }

  doc["sensors"] = sensorsWithData;
  
  // No data for this month — nothing to archive
  if (sensorsWithData == 0) {
    Serial.print(F("FTP archive skipped (no data): "));
    Serial.print(year);
    Serial.print(F("/"));
    Serial.println(month);
    return false;
  }
  
  // Add alarm summary — only alarms from the target month
  JsonArray alarmsArray = doc["alarms"].to<JsonArray>();
  for (uint8_t i = 0; i < alarmLogCount; i++) {
    int idx = (alarmLogWriteIndex - alarmLogCount + i + MAX_ALARM_LOG_ENTRIES) % MAX_ALARM_LOG_ENTRIES;
    // Filter: only include alarms from the target month
    if (alarmLog[idx].timestamp < monthStartEpoch || alarmLog[idx].timestamp >= monthEndEpoch) continue;
    
    JsonObject alarmObj = alarmsArray.add<JsonObject>();
    alarmObj["timestamp"] = alarmLog[idx].timestamp;
    alarmObj["site"] = alarmLog[idx].siteName;
    alarmObj["clientUid"] = alarmLog[idx].clientUid;
    alarmObj["sensorIndex"] = alarmLog[idx].sensorIndex;
    alarmObj["level"] = alarmLog[idx].level;
    alarmObj["type"] = alarmLog[idx].isHigh ? "HIGH" : "LOW";
    alarmObj["cleared"] = alarmLog[idx].cleared;
    if (alarmLog[idx].cleared) {
      alarmObj["clearedAt"] = alarmLog[idx].clearedTimestamp;
    }
  }
  
  // Serialize to buffer
  String jsonOut;
  serializeJson(doc, jsonOut);
  
  // Upload to FTP
  const bool useFtps = gConfig.ftpsEnabled;
  FtpSession session;
  char err[128];
  bool connected = useFtps ? ftpsConnectAndLogin(err, sizeof(err))
                           : ftpConnectAndLogin(session, err, sizeof(err));
  if (!connected) {
    Serial.print(F("FTP archive failed: "));
    Serial.println(err);
    return false;
  }
  
  // Build remote path: /ftpPath/history/YYYYMM_history.json
  char remotePath[192];
  snprintf(remotePath, sizeof(remotePath), "%s/history/%04d%02d_history.json", 
           gConfig.ftpPath, year, month);
  
  bool stored = useFtps
                  ? ftpsStoreBuffer(remotePath, (const uint8_t *)jsonOut.c_str(), jsonOut.length(), err, sizeof(err))
                  : ftpStoreBuffer(session, remotePath, (const uint8_t *)jsonOut.c_str(), jsonOut.length(), err, sizeof(err));
  if (!stored) {
    Serial.print(F("FTP store failed: "));
    Serial.println(err);
    if (useFtps) {
      ftpsQuit();
    } else {
      ftpQuit(session);
    }
    return false;
  }
  
  if (useFtps) {
    ftpsQuit();
  } else {
    ftpQuit(session);
  }
  
  gHistorySettings.lastFtpSyncEpoch = gLastSyncedEpoch > 0.0 ? gLastSyncedEpoch : 0.0;
  saveHistorySettings();
  
  Serial.print(F("Archived month to FTP: "));
  Serial.println(remotePath);
  
  return true;
}

// Load archived month from FTP for comparison
static bool loadArchivedMonth(uint16_t year, uint8_t month, JsonDocument &doc) {
  if (!gConfig.ftpEnabled) {
    return false;
  }
  
  const bool useFtps = gConfig.ftpsEnabled;
  FtpSession session;
  char err[128];
  bool connected = useFtps ? ftpsConnectAndLogin(err, sizeof(err))
                           : ftpConnectAndLogin(session, err, sizeof(err));
  if (!connected) {
    return false;
  }
  
  char remotePath[192];
  snprintf(remotePath, sizeof(remotePath), "%s/history/%04d%02d_history.json", 
           gConfig.ftpPath, year, month);
  
  // Use static buffer to avoid 16KB stack allocation (Mbed OS stack is only 4-8KB)
  static char buffer[16384];
  size_t len = 0;
  bool retrieved = useFtps
                     ? ftpsRetrieveBuffer(remotePath, buffer, sizeof(buffer), len, err, sizeof(err))
                     : ftpRetrieveBuffer(session, remotePath, buffer, sizeof(buffer), len, err, sizeof(err));
  if (!retrieved) {
    if (useFtps) {
      ftpsQuit();
    } else {
      ftpQuit(session);
    }
    return false;
  }
  
  if (useFtps) {
    ftpsQuit();
  } else {
    ftpQuit(session);
  }
  
  // Parse JSON
  if (deserializeJson(doc, buffer, len) != DeserializationError::Ok) {
    return false;
  }
  
  return true;
}

// Helper to populate history settings JSON document
static void populateHistorySettingsJson(JsonDocument &doc) {
  doc["hotDays"] = gHistorySettings.hotTierRetentionDays;
  doc["warmMonths"] = gHistorySettings.warmTierRetentionMonths;
  doc["ftpArchive"] = gHistorySettings.ftpArchiveEnabled;
  doc["ftpHour"] = gHistorySettings.ftpSyncHour;
  doc["lastSync"] = gHistorySettings.lastFtpSyncEpoch;
  doc["lastPrune"] = gHistorySettings.lastPruneEpoch;
  doc["pruned"] = gHistorySettings.totalRecordsPruned;
  doc["lastRollup"] = gLastDailyRollupDate;
}

// Save history settings to LittleFS. Hash-gated: skips the write if the
// serialized payload is byte-identical to the last successful save. This lets
// callers (including FTP backup prep) invoke this freely without burning a
// flash erase/program cycle when nothing has actually changed.
static uint32_t gHistorySettingsLastHash = 0;
static bool saveHistorySettings() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return false;
    
    JsonDocument doc;
    populateHistorySettingsJson(doc);
    
    String output;
    serializeJson(doc, output);

    uint32_t newHash = fnv1a32(output.c_str(), output.length());
    if (newHash == gHistorySettingsLastHash && gHistorySettingsLastHash != 0) {
      gFlashWritesSkippedHash++;
      return true;  // No-op success: the on-disk file already matches.
    }

    if (!serverWriteFileAtomic("/fs/history_settings.json",
                               output.c_str(), output.length())) {
      Serial.println(F("Failed to write history settings"));
      return false;
    }
    gHistorySettingsLastHash = newHash;
    return true;
  #else
    JsonDocument doc;
    populateHistorySettingsJson(doc);
    
    String output;
    serializeJson(doc, output);
    
    if (!tankalarm_littlefs_write_file_atomic("/history_settings.json",
            (const uint8_t *)output.c_str(), output.length())) {
      Serial.println(F("Failed to write history settings"));
      return false;
    }
    return true;
  #endif
#else
  return false;
#endif
}

// Helper to apply history settings from JSON document
static void applyHistorySettingsFromJson(const JsonDocument &doc) {
  gHistorySettings.hotTierRetentionDays = doc["hotDays"] | 90;
  gHistorySettings.warmTierRetentionMonths = doc["warmMonths"] | 24;
  gHistorySettings.ftpArchiveEnabled = doc["ftpArchive"] | false;
  gHistorySettings.ftpSyncHour = doc["ftpHour"] | 3;
  gHistorySettings.lastFtpSyncEpoch = doc["lastSync"] | 0.0;
  gHistorySettings.lastPruneEpoch = doc["lastPrune"] | 0.0;
  gHistorySettings.totalRecordsPruned = doc["pruned"] | 0;
  gLastDailyRollupDate = doc["lastRollup"] | (uint32_t)0;
}

// Load history settings from LittleFS
static void loadHistorySettings() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return;
    
    FILE *file = fopen("/fs/history_settings.json", "r");
    if (!file) return;  // No file yet — not an error on first boot
    
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (fileSize <= 0 || fileSize > MAX_HISTORY_SETTINGS_FILE_SIZE) {
      fclose(file);
      Serial.println(F("History settings file size invalid"));
      return;
    }
    
    char *buffer = (char *)malloc(fileSize + 1);
    if (!buffer) {
      fclose(file);
      Serial.println(F("ERROR: Cannot allocate history settings load buffer"));
      return;
    }
    
    size_t bytesRead = fread(buffer, 1, fileSize, file);
    fclose(file);
    
    // Check if we read the expected amount
    if (bytesRead != (size_t)fileSize) {
      free(buffer);
      Serial.println(F("History settings: short read"));
      return;
    }
    buffer[bytesRead] = '\0';
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buffer);
    free(buffer);
    
    if (err == DeserializationError::Ok) {
      applyHistorySettingsFromJson(doc);
    } else {
      Serial.print(F("ERROR: History settings parse failed: "));
      Serial.println(err.c_str());
    }
  #else
    File f = LittleFS.open("/history_settings.json", "r");
    if (!f) return;
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err == DeserializationError::Ok) {
      applyHistorySettingsFromJson(doc);
    } else {
      Serial.print(F("ERROR: History settings parse failed: "));
      Serial.println(err.c_str());
    }
  #endif
#endif
}

// ============================================================================
// LittleFS Daily Summary System (Warm Tier Implementation)
// ============================================================================
// Rolls up hot-tier snapshots into compact daily summaries stored on flash.
// Files: /fs/history/daily_YYYYMM.json  (one per month)
// Each file contains an array of per-sensor daily summary objects.
// Automatic pruning keeps only MAX_DAILY_SUMMARY_MONTHS on flash.

// Helper: get YYYYMMDD integer from epoch
static uint32_t epochToYYYYMMDD(double epoch) {
  if (epoch <= 0.0) return 0;
  time_t t = (time_t)epoch;
  struct tm *tm = gmtime(&t);
  if (!tm) return 0;
  return (uint32_t)(tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
}

// Roll up yesterday's hot-tier snapshots into a daily summary and persist to LittleFS
static void rollupDailySummaries() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS) return;
  
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  if (now <= 0.0) return;
  
  // Determine yesterday's date
  double yesterdayEpoch = now - 86400.0;
  uint32_t yesterdayDate = epochToYYYYMMDD(yesterdayEpoch);
  if (yesterdayDate == 0 || yesterdayDate == gLastDailyRollupDate) return;
  
  uint16_t year = yesterdayDate / 10000;
  uint8_t month = (yesterdayDate / 100) % 100;
  
  // Calculate epoch range for yesterday (00:00 - 23:59:59 UTC)
  struct tm dayStart = {};
  dayStart.tm_year = year - 1900;
  dayStart.tm_mon = month - 1;
  dayStart.tm_mday = yesterdayDate % 100;
  time_t dayStartEpoch = mktime(&dayStart);
  // Adjust for timezone (mktime uses local, we want UTC)
  double dayBegin = (double)dayStartEpoch;
  double dayEnd = dayBegin + 86400.0;
  
  // Load existing month file if present
  JsonDocument monthDoc;
  char filePath[64];
  snprintf(filePath, sizeof(filePath), "/fs/history/daily_%04d%02d.json", year, month);
  
  FILE *existing = fopen(filePath, "r");
  if (existing) {
    fseek(existing, 0, SEEK_END);
    long sz = ftell(existing);
    fseek(existing, 0, SEEK_SET);
    if (sz > 0 && sz < 8192) {
      char *buf = (char *)malloc(sz + 1);
      if (buf) {
        size_t bytesRead = fread(buf, 1, sz, existing);
        buf[bytesRead] = '\0';
        if (bytesRead == (size_t)sz) {
          deserializeJson(monthDoc, buf);
        }
        free(buf);
      }
    }
    fclose(existing);
  }
  
  // Ensure root is array
  if (!monthDoc.is<JsonArray>()) {
    monthDoc.to<JsonArray>();
  }
  JsonArray entries = monthDoc.as<JsonArray>();
  
  // Dedup: remove any existing entries for yesterdayDate (prevents duplicates on reboot)
  for (int e = (int)entries.size() - 1; e >= 0; --e) {
    if ((entries[e]["d"] | (uint32_t)0) == yesterdayDate) {
      entries.remove(e);
    }
  }
  
  // For each sensor in hot tier, compute yesterday's summary
  uint8_t addedCount = 0;
  for (uint8_t i = 0; i < gSensorHistoryCount; i++) {
    SensorHourlyHistory &hist = gSensorHistory[i];
    if (hist.snapshotCount == 0) continue;
    
    float minLevel = 999999.0f, maxLevel = -999999.0f;
    float sumLevel = 0.0f, sumVoltage = 0.0f;
    float openingLevel = 0.0f, closingLevel = 0.0f;
    double oldestTs = 1e18, newestTs = 0.0;
    uint16_t count = 0, voltCount = 0;
    uint8_t alarms = 0;
    
    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      TelemetrySnapshot &snap = hist.snapshots[idx];
      
      if (snap.timestamp < dayBegin || snap.timestamp >= dayEnd) continue;
      
      if (snap.level < minLevel) minLevel = snap.level;
      if (snap.level > maxLevel) maxLevel = snap.level;
      sumLevel += snap.level;
      count++;
      
      if (snap.timestamp < oldestTs) { oldestTs = snap.timestamp; openingLevel = snap.level; }
      if (snap.timestamp > newestTs) { newestTs = snap.timestamp; closingLevel = snap.level; }
      
      if (snap.voltage > 0.0f) { sumVoltage += snap.voltage; voltCount++; }
    }
    
    if (count == 0) continue;  // No data for this sensor yesterday
    
    // Count alarms for this sensor on this day
    for (uint8_t a = 0; a < alarmLogCount; a++) {
      int aIdx = (alarmLogWriteIndex - alarmLogCount + a + MAX_ALARM_LOG_ENTRIES) % MAX_ALARM_LOG_ENTRIES;
      if (strcmp(alarmLog[aIdx].clientUid, hist.clientUid) == 0 &&
          alarmLog[aIdx].sensorIndex == hist.sensorIndex &&
          alarmLog[aIdx].timestamp >= dayBegin && alarmLog[aIdx].timestamp < dayEnd) {
        alarms++;
      }
    }
    
    // Add to month document
    JsonObject entry = entries.add<JsonObject>();
    entry["d"] = yesterdayDate;
    entry["c"] = hist.clientUid;
    entry["k"] = hist.sensorIndex;
    entry["mn"] = roundTo(minLevel, 1);
    entry["mx"] = roundTo(maxLevel, 1);
    entry["av"] = roundTo(sumLevel / count, 1);
    entry["op"] = roundTo(openingLevel, 1);
    entry["cl"] = roundTo(closingLevel, 1);
    entry["al"] = alarms;
    entry["vt"] = voltCount > 0 ? roundTo(sumVoltage / voltCount, 2) : 0.0f;
    entry["n"] = count;
    addedCount++;
  }
  
  if (addedCount == 0) {
    gLastDailyRollupDate = yesterdayDate;
    return;
  }
  
  // Serialize and write to file
  String output;
  serializeJson(monthDoc, output);
  
  // Ensure directory exists
  mkdir("/fs/history", 0777);
  
  if (serverWriteFileAtomic(filePath, output.c_str(), output.length())) {
    gLastDailyRollupDate = yesterdayDate;
    Serial.print(F("Daily summary rollup: "));
    Serial.print(addedCount);
    Serial.print(F(" sensors for "));
    Serial.println(yesterdayDate);
  }
  #endif
#endif
}

// Load a month's daily summaries from LittleFS
static bool loadDailySummaryMonth(uint16_t year, uint8_t month, JsonDocument &doc) {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS) return false;
  
  char filePath[64];
  snprintf(filePath, sizeof(filePath), "/fs/history/daily_%04d%02d.json", year, month);
  
  FILE *f = fopen(filePath, "r");
  if (!f) return false;
  
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  
  if (sz <= 0 || sz > 16384) {
    fclose(f);
    return false;
  }
  
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { fclose(f); return false; }
  
  size_t bytesRead = fread(buf, 1, sz, f);
  fclose(f);
  if (bytesRead != (size_t)sz) {
    free(buf);
    return false;
  }
  buf[bytesRead] = '\0';
  
  DeserializationError err = deserializeJson(doc, buf);
  free(buf);
  if (err == DeserializationError::Ok) { gWarmTierDataExists = true; }
  return (err == DeserializationError::Ok);
  #endif
#endif
  return false;
}

// Prune old daily summary files beyond MAX_DAILY_SUMMARY_MONTHS
static void pruneDailySummaryFiles() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS) return;
  
  double now = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    now = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  if (now <= 0.0) return;
  
  time_t nowTime = (time_t)now;
  struct tm *nowTm = gmtime(&nowTime);
  if (!nowTm) return;
  
  int curYear = nowTm->tm_year + 1900;
  int curMonth = nowTm->tm_mon + 1;
  
  // Calculate oldest allowed month
  int oldYear = curYear;
  int oldMonth = curMonth - MAX_DAILY_SUMMARY_MONTHS;
  while (oldMonth <= 0) { oldMonth += 12; oldYear--; }
  
  // Scan for and remove files older than retention
  // Try a range of possible old files (24 months back)
  for (int delta = MAX_DAILY_SUMMARY_MONTHS + 1; delta <= 24; delta++) {
    int delYear = curYear;
    int delMonth = curMonth - delta;
    while (delMonth <= 0) { delMonth += 12; delYear--; }
    
    char filePath[64];
    snprintf(filePath, sizeof(filePath), "/fs/history/daily_%04d%02d.json", delYear, delMonth);
    
    FILE *check = fopen(filePath, "r");
    if (check) {
      fclose(check);
      remove(filePath);
      Serial.print(F("Pruned old daily summary: "));
      Serial.println(filePath);
    }
  }
  #endif
#endif
}

// ============================================================================
// Hot Tier Persistence (survive reboots)
// ============================================================================
// Saves the current hot-tier ring buffer to LittleFS so trending data
// survives power cycles. Written periodically and on graceful shutdown.
//
// Hash-gated: if the serialized payload hashes (FNV-1a32) to the same value
// as the last successful save, the write is skipped to avoid burning a flash
// erase/program cycle on data that has not changed. The skip is recorded in
// gFlashWritesSkippedHash so /api/system-status can show how often the
// throttle is firing.

static bool saveHotTierSnapshot() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS || gSensorHistoryCount == 0) return false;
  
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  
  for (uint8_t i = 0; i < gSensorHistoryCount; i++) {
    SensorHourlyHistory &hist = gSensorHistory[i];
    if (hist.snapshotCount == 0) continue;
    
    JsonObject sensorObj = arr.add<JsonObject>();
    sensorObj["c"] = hist.clientUid;
    sensorObj["s"] = hist.siteName;
    sensorObj["k"] = hist.sensorIndex;
    sensorObj["h"] = hist.heightInches;
    
    JsonArray snaps = sensorObj["d"].to<JsonArray>();
    // Write snapshots in chronological order
    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      TelemetrySnapshot &snap = hist.snapshots[idx];
      JsonArray entry = snaps.add<JsonArray>();
      entry.add(snap.timestamp);
      entry.add(roundTo(snap.level, 2));
      entry.add(roundTo(snap.voltage, 2));
    }
  }
  
  size_t jsonLen = measureJson(doc);
  char *buf = (char *)malloc(jsonLen + 1);
  if (!buf) return false;
  serializeJson(doc, buf, jsonLen + 1);

  // Hash-gate: skip the write if the payload is byte-identical to the last
  // successful save. This is the single biggest wear reduction on devices
  // with a stable hot-tier ring (no new telemetry between save ticks).
  uint32_t newHash = fnv1a32(buf, jsonLen);
  if (newHash == gHotTierLastHash && gHotTierLastHash != 0) {
    free(buf);
    gFlashWritesSkippedHash++;
    return true;  // No-op success: the on-disk file already matches.
  }

  mkdir("/fs/history", 0777);
  bool ok = serverWriteFileAtomic("/fs/history/hot_tier.json", buf, jsonLen);
  if (ok) {
    gHotTierLastHash = newHash;
    Serial.print(F("Hot tier saved: "));
    Serial.print(gSensorHistoryCount);
    Serial.println(F(" sensors"));
  }
  free(buf);
  return ok;
  #else
  return false;
  #endif
#else
  return false;
#endif
}

static void loadHotTierSnapshot() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS) return;
  
  FILE *f = fopen("/fs/history/hot_tier.json", "r");
  if (!f) return;
  
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  
  // Hot tier JSON could be large with 90 snapshots × 20 sensors
  if (sz <= 0 || sz > 65536) {
    fclose(f);
    return;
  }
  
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { fclose(f); return; }
  
  size_t bytesRead = fread(buf, 1, sz, f);
  fclose(f);
  if (bytesRead != (size_t)sz) {
    free(buf);
    return;
  }
  buf[bytesRead] = '\0';
  
  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) {
    free(buf);
    return;
  }
  free(buf);
  
  JsonArray arr = doc.as<JsonArray>();
  gSensorHistoryCount = 0;
  
  for (JsonObject sensorObj : arr) {
    if (gSensorHistoryCount >= MAX_HISTORY_SENSORS) break;
    
    SensorHourlyHistory &hist = gSensorHistory[gSensorHistoryCount];
    strlcpy(hist.clientUid, sensorObj["c"] | "", sizeof(hist.clientUid));
    strlcpy(hist.siteName, sensorObj["s"] | "", sizeof(hist.siteName));
    hist.sensorIndex = sensorObj["k"] | 0;
    hist.heightInches = sensorObj["h"] | 120.0f;
    hist.snapshotCount = 0;
    hist.writeIndex = 0;
    
    JsonArray snaps = sensorObj["d"].as<JsonArray>();
    for (JsonArray entry : snaps) {
      if (hist.snapshotCount >= MAX_HOURLY_HISTORY_PER_SENSOR) break;
      
      TelemetrySnapshot &snap = hist.snapshots[hist.writeIndex];
      snap.timestamp = entry[0].as<double>();
      snap.level = entry[1].as<float>();
      snap.voltage = entry[2].as<float>();
      
      hist.writeIndex = (hist.writeIndex + 1) % MAX_HOURLY_HISTORY_PER_SENSOR;
      hist.snapshotCount++;
    }
    
    if (hist.clientUid[0] != '\0') {
      gSensorHistoryCount++;
    }
  }
  
  Serial.print(F("Hot tier restored: "));
  Serial.print(gSensorHistoryCount);
  Serial.println(F(" sensors from flash"));
  #endif
#endif
}

// ============================================================================
// FTP Archive Retrieval with Caching
// ============================================================================
// Loads a month from FTP and caches the result for repeated API access.
// Cache is invalidated when a different month is requested.

static bool loadFtpArchiveCached(uint16_t year, uint8_t month) {
  // Return cached if matching
  if (gFtpArchiveCache.valid && gFtpArchiveCache.cachedYear == year && gFtpArchiveCache.cachedMonth == month) {
    return true;
  }
  
  // Need FTP
  if (!gConfig.ftpEnabled) return false;
  
  JsonDocument doc;
  if (!loadArchivedMonth(year, month, doc)) {
    return false;
  }
  
  // Parse into cache
  gFtpArchiveCache.cachedYear = year;
  gFtpArchiveCache.cachedMonth = month;
  gFtpArchiveCache.sensorCount = 0;
  
  JsonArray summaries = doc["sensorSummaries"].as<JsonArray>();
  for (JsonObject sensorObj : summaries) {
    if (gFtpArchiveCache.sensorCount >= MAX_HISTORY_SENSORS) break;
    
    auto &cached = gFtpArchiveCache.sensors[gFtpArchiveCache.sensorCount];
    strlcpy(cached.clientUid, sensorObj["clientUid"] | "", sizeof(cached.clientUid));
    cached.sensorIndex = sensorObj["sensorIndex"] | 0;
    cached.minLevel = sensorObj["minLevel"] | 0.0f;
    cached.maxLevel = sensorObj["maxLevel"] | 0.0f;
    cached.avgLevel = sensorObj["avgLevel"] | 0.0f;
    cached.avgVoltage = sensorObj["avgVoltage"] | 0.0f;
    cached.readings = sensorObj["readings"] | 0;
    gFtpArchiveCache.sensorCount++;
  }
  
  gFtpArchiveCache.valid = true;
  Serial.print(F("FTP archive cached: "));
  Serial.print(year);
  Serial.print(F("/"));
  Serial.println(month);
  return true;
}

// Populate a JSON stats object from FTP cache for a specific sensor
static void populateStatsFromFtpCache(const char *clientUid, uint8_t sensorIndex, JsonObject &statsObj) {
  for (uint8_t i = 0; i < gFtpArchiveCache.sensorCount; i++) {
    auto &cached = gFtpArchiveCache.sensors[i];
    if (strcmp(cached.clientUid, clientUid) == 0 && cached.sensorIndex == sensorIndex) {
      statsObj["min"] = roundTo(cached.minLevel, 1);
      statsObj["max"] = roundTo(cached.maxLevel, 1);
      statsObj["avg"] = roundTo(cached.avgLevel, 1);
      statsObj["readings"] = cached.readings;
      statsObj["available"] = true;
      statsObj["dataSource"] = "ftp";
      return;
    }
  }
  statsObj["available"] = false;
  statsObj["message"] = "Sensor not found in archive";
}

// Populate stats from LittleFS daily summaries for a specific sensor+month
static void populateStatsFromDailySummary(uint16_t year, uint8_t month,
                                           const char *clientUid, uint8_t sensorIndex,
                                           JsonObject &statsObj) {
  JsonDocument monthDoc;
  if (!loadDailySummaryMonth(year, month, monthDoc)) {
    statsObj["available"] = false;
    statsObj["message"] = "No daily summary data on flash";
    return;
  }
  
  JsonArray entries = monthDoc.as<JsonArray>();
  float minLevel = 999999.0f, maxLevel = -999999.0f, sumAvg = 0.0f;
  uint16_t dayCount = 0;
  
  for (JsonObject entry : entries) {
    const char *uid = entry["c"] | "";
    uint8_t sensorIdx = entry["k"] | 0;
    if (strcmp(uid, clientUid) != 0 || sensorIdx != sensorIndex) continue;
    
    float mn = entry["mn"] | 0.0f;
    float mx = entry["mx"] | 0.0f;
    float av = entry["av"] | 0.0f;
    if (mn < minLevel) minLevel = mn;
    if (mx > maxLevel) maxLevel = mx;
    sumAvg += av;
    dayCount++;
  }
  
  if (dayCount > 0) {
    statsObj["min"] = roundTo(minLevel, 1);
    statsObj["max"] = roundTo(maxLevel, 1);
    statsObj["avg"] = roundTo(sumAvg / dayCount, 1);
    statsObj["readings"] = dayCount;
    statsObj["available"] = true;
    statsObj["dataSource"] = "flash";
  } else {
    statsObj["available"] = false;
    statsObj["message"] = "No data for this sensor in daily summary";
  }
}

static void printHardwareRequirements() {
  Serial.println(F("--- Hardware Requirements ---"));
  Serial.println(F("Base: Arduino Opta Lite"));
  Serial.println(F("Networking: Integrated Ethernet (static or DHCP)"));
  Serial.println(F("Cellular relay: Blues Notecard (I2C 0x17)"));
  Serial.println(F("Storage: LittleFS internal flash for configuration"));
  Serial.println(F("-----------------------------"));
}

static void initializeNotecard() {
#ifdef DEBUG_MODE
  notecard.setDebugOutputStream(Serial);
#endif
  notecard.begin(NOTECARD_I2C_ADDRESS);
  gLastSuccessfulNotecardComm = millis();
  Serial.println(F("Notecard initialized"));

  J *req = notecard.newRequest("hub.set");
  if (req) {
    // Use configurable product UID - must be set via Server Settings web page
    if (gConfig.productUid[0] != '\0') {
      JAddStringToObject(req, "product", gConfig.productUid);
      JAddStringToObject(req, "mode", "continuous");
      // Join the server fleet so fleet-targeted notes from clients are delivered
      const char *fleet = (gConfig.serverFleet[0] != '\0') ? gConfig.serverFleet : "tankalarm-server";
      JAddStringToObject(req, "fleet", fleet);
      J *hubRsp = notecard.requestAndResponse(req);
      if (hubRsp) {
        const char *hubErr = JGetString(hubRsp, "err");
        if (hubErr && hubErr[0] != '\0') {
          Serial.print(F("WARNING: hub.set failed: "));
          Serial.println(hubErr);
        } else {
          Serial.print(F("Product UID: "));
          Serial.println(gConfig.productUid);
          Serial.print(F("Server Fleet: "));
          Serial.println(fleet);
        }
        notecard.deleteResponse(hubRsp);
      } else {
        Serial.println(F("WARNING: hub.set returned no response"));
      }
    } else {
      notecard.deleteResponse(notecard.requestAndResponse(req));
      Serial.println(F("WARNING: Product UID not configured! Set it in Server Settings."));
    }
  }

  // Retrieve the Notecard device UID via hub.get (returns "device" field)
  req = notecard.newRequest("hub.get");
  if (req) {
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) {
      const char *err = JGetString(rsp, "err");
      if (err && err[0] != '\0') {
        Serial.print(F("WARNING: hub.get failed: "));
        Serial.println(err);
      }
      const char *uid = JGetString(rsp, "device");
      if (uid && uid[0] != '\0') {
        strlcpy(gServerUid, uid, sizeof(gServerUid));
      }
      notecard.deleteResponse(rsp);
    }
  }

  // Fallback: try card.version if hub.get didn't return a device UID
  if (gServerUid[0] == '\0') {
    req = notecard.newRequest("card.version");
    if (req) {
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) {
        const char *uid = JGetString(rsp, "device");
        if (uid && uid[0] != '\0') {
          strlcpy(gServerUid, uid, sizeof(gServerUid));
        }
        notecard.deleteResponse(rsp);
      }
    }
  }

  Serial.print(F("Server Device UID: "));
  Serial.println(gServerUid[0] != '\0' ? gServerUid : "(not available)");

  // Wireless for Opta uses IAP DFU, not ODFU. Enable Notehub downloads with
  // dfu.status so the server can later read chunks via dfu.get and flash them.
  tankalarm_enableIapDfu(notecard);
  Serial.println(F("IAP DFU enabled for host firmware updates"));
}

static void ensureTimeSync() {
  if (gLastSyncedEpoch <= 0.0 || (uint32_t)(millis() - gLastSyncMillis) > 6UL * 60UL * 60UL * 1000UL) {
    J *req = notecard.newRequest("card.time");
    if (!req) {
      return;
    }
    J *rsp = notecard.requestAndResponse(req);
    if (!rsp) {
      return;
    }
    // Check for error response (e.g., "time is not yet set {no-time}")
    // This is normal during startup before Notecard syncs with cloud
    const char *err = JGetString(rsp, "err");
    if (err && strlen(err) > 0) {
      // Time not yet available - this is expected during startup
      // Will retry on next call
      notecard.deleteResponse(rsp);
      return;
    }
    double time = JGetNumber(rsp, "time");
    if (time > 0) {
      gLastSyncedEpoch = time;
      gLastSyncMillis = millis();
    }
    notecard.deleteResponse(rsp);
  }
}

static double currentEpoch() {
  if (gLastSyncedEpoch <= 0.0) {
    return 0.0;
  }
  uint32_t delta = (uint32_t)(millis() - gLastSyncMillis);  // Handles millis() rollover
  return gLastSyncedEpoch + (double)delta / 1000.0;
}

static void scheduleNextDailyEmail() {
  double epoch = currentEpoch();
  if (epoch <= 0.0) {
    gNextDailyEmailEpoch = 0.0;
    return;
  }
  double dayStart = floor(epoch / 86400.0) * 86400.0;
  double scheduled = dayStart + (double)gConfig.dailyHour * 3600.0 + (double)gConfig.dailyMinute * 60.0;
  if (scheduled <= epoch) {
    scheduled += 86400.0;
  }
  gNextDailyEmailEpoch = scheduled;
}

// ============================================================================
// Device Firmware Update (DFU) via Blues Notecard
// ============================================================================

// Read actual input voltage via Opta analog pin with voltage divider
static float readAnalogVinVoltage() {
#if VIN_ANALOG_ENABLED
  // Average 8 samples for stability
  float total = 0.0f;
  const uint8_t samples = 8;
  for (uint8_t i = 0; i < samples; i++) {
    int raw = analogRead(VIN_ANALOG_PIN);
    float pinVoltage = (float)raw / VIN_ADC_MAX * VIN_ADC_REF_VOLTAGE;
    total += pinVoltage / VIN_DIVIDER_RATIO;  // Reverse the divider to get actual Vin
    safeSleep(2);
  }
  return total / (float)samples;
#else
  return 0.0f;  // No voltage divider hardware connected
#endif
}

// Poll server voltages: Notecard V+ rail and optional analog Vin
static void checkServerVoltage() {
  // Source 1: Notecard card.voltage - reads the ~5V regulated power rail
  J *req = notecard.newRequest("card.voltage");
  if (req) {
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) {
      double voltage = JGetNumber(rsp, "value");
      if (voltage > 0.0) {
        gNotecardVoltage = (float)voltage;
        gVoltageEpoch = currentEpoch();
        
        static float lastLoggedNcVoltage = 0.0f;
        if (abs(gNotecardVoltage - lastLoggedNcVoltage) > 0.5f) {
          Serial.print(F("Notecard supply: "));
          Serial.print(gNotecardVoltage, 2);
          Serial.println(F("V (V+ power rail)"));
          lastLoggedNcVoltage = gNotecardVoltage;
        }
      }
      notecard.deleteResponse(rsp);
    }
  }
  
  // Source 2: Analog input with voltage divider (actual battery/Vin)
  float analogVin = readAnalogVinVoltage();
  if (analogVin > 0.5f) {  // Minimum threshold to filter noise
    gInputVoltage = analogVin;
    gVoltageEpoch = currentEpoch();
    
    static float lastLoggedVin = 0.0f;
    if (abs(gInputVoltage - lastLoggedVin) > 0.5f) {
      Serial.print(F("Input voltage (analog): "));
      Serial.print(gInputVoltage, 2);
      Serial.println(F("V"));
      lastLoggedVin = gInputVoltage;
    }
  }
}

// Queries the GitHub Releases API via Notecard web proxy to detect whether a newer
// firmware version exists. Sets gGitHubUpdateAvailable / gGitHubLatestVersion on change.
// Called once 60 s after boot and then every GITHUB_CHECK_INTERVAL_MS.
static void checkGitHubForUpdate() {
  Serial.println(F("Checking GitHub for firmware updates..."));

  GitHubFirmwareTargetState state;
  if (!checkGitHubReleaseForTarget(FIRMWARE_TARGET_SERVER,
                                   FIRMWARE_VERSION,
                                   FIRMWARE_TARGET_SERVER,
                                   state,
                                   true)) {
    Serial.print(F("GitHub check failed: "));
    Serial.println(state.error[0] != '\0' ? state.error : "unknown error");
    return;
  }

  copyGitHubFirmwareTargetStateToGlobals(state);
  if (!state.updateAvailable) {
    if (compareFirmwareVersions(state.latestVersion, FIRMWARE_VERSION) < 0) {
      Serial.println(F("GitHub release is older than the running firmware"));
    } else {
      Serial.println(F("Firmware is current with GitHub"));
    }
  }
}

static void checkForFirmwareUpdate() {
  TankAlarmDfuStatus status;
  if (!tankalarm_checkDfuStatus(notecard, status)) {
    return;
  }

  // Check blacklist first
  if (status.updateAvailable && status.version[0] != '\0') {
#if defined(TANKALARM_DFU_MCUBOOT)
    if (tankalarm_isVersionBlacklisted(status.version)) {
      Serial.print(F("DFU: Version "));
      Serial.print(status.version);
      Serial.println(F(" is locally blacklisted. Skipping check."));
      return;
    }
#endif
  }

  gDfuStatus = status;

  strlcpy(gDfuMode, status.mode[0] != '\0' ? status.mode : "idle", sizeof(gDfuMode));

  if (status.error) {
    strlcpy(gDfuError,
            status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown DFU error",
            sizeof(gDfuError));
    Serial.print(F("DFU error from Notecard: "));
    Serial.println(gDfuError);
  } else {
    gDfuError[0] = '\0';
  }

  if (status.downloading) {
    gDfuUpdateAvailable = false;
    gDfuFirmwareLength = 0;
    return;
  }

  if (status.updateAvailable && status.version[0] != '\0') {
    bool isNewUpdate = !gDfuUpdateAvailable || strcmp(gDfuVersion, status.version) != 0;
    gDfuUpdateAvailable = true;
    gDfuFirmwareLength = status.firmwareLength;
    strlcpy(gDfuVersion, status.version, sizeof(gDfuVersion));

    if (isNewUpdate) {
      Serial.println(F("========================================"));
      Serial.print(F("FIRMWARE UPDATE AVAILABLE: v"));
      Serial.println(gDfuVersion);
      Serial.print(F("Current version: "));
      Serial.println(F(FIRMWARE_VERSION));
      Serial.print(F("DFU mode: "));
      Serial.println(gDfuMode);
      Serial.print(F("Size: "));
      Serial.print(gDfuFirmwareLength);
      Serial.println(F(" bytes"));
      Serial.println(F("Use web UI 'Install Update' button to apply"));
      Serial.println(F("========================================"));

      addServerSerialLog("Firmware update available", "info", "dfu");
    }
  } else {
    gDfuUpdateAvailable = false;
    gDfuFirmwareLength = 0;
    gDfuVersion[0] = '\0';
  }
}

static void enableDfuMode() {
  if (gDfuInProgress) {
    Serial.println(F("DFU already in progress"));
    return;
  }

  if (!gDfuUpdateAvailable || gDfuFirmwareLength == 0) {
    Serial.println(F("ERROR: No ready IAP firmware update available"));
    strlcpy(gDfuMode, "error", sizeof(gDfuMode));
    strlcpy(gDfuError, "No ready IAP firmware update available", sizeof(gDfuError));
    return;
  }
  
  Serial.println(F("========================================"));
  Serial.println(F("ENABLING IAP DFU MODE"));
  Serial.println(F("Device will apply firmware already downloaded by Notehub"));
  Serial.println(F("System will reset when complete"));
  Serial.println(F("========================================"));
  
  // Mark DFU in progress to prevent multiple triggers
  gDfuInProgress = true;
  strlcpy(gDfuMode, "applying", sizeof(gDfuMode));
  gDfuError[0] = '\0';
  addServerSerialLog("IAP DFU mode enabled - updating firmware", "info", "dfu");
  
  // Save all pending data before rebooting
  if (gConfigDirty) {
    saveConfig(gConfig);
    gConfigDirty = false;
  }
  if (gSensorRegistryDirty) {
    saveSensorRegistry();
    gSensorRegistryDirty = false;
  }
  if (gClientMetadataDirty) {
    saveClientMetadataCache();
    gClientMetadataDirty = false;
  }
  saveHotTierSnapshot();
  saveClientConfigSnapshots();
  saveHistorySettings();

#if defined(TANKALARM_DFU_MCUBOOT)
  bool success = tankalarm_performMcubootUpdate(
      notecard,
      gDfuStatus,
      "continuous",
      DEVICE_ROLE,
      dfuKickWatchdog);
#else
  bool success = false;
  // MCUboot DFU support is not compiled in — stop the pending update so the
  // Server does not repeatedly attempt to apply it on every DFU check cycle.
  {
    J *req = notecard.newRequest("dfu.status");
    if (req) {
      JAddBoolToObject(req, "stop", true);
      JAddStringToObject(req, "status", "MCUboot DFU not supported in this build");
      JAddStringToObject(req, "name", "user");
      J *rsp = notecard.requestAndResponse(req);
      if (rsp) notecard.deleteResponse(rsp);
    }
  }
#endif

  gDfuInProgress = false;
  if (!success) {
    Serial.println(F("IAP DFU failed - resuming normal operation"));
    strlcpy(gDfuMode, "error", sizeof(gDfuMode));
    strlcpy(gDfuError, "IAP firmware apply failed", sizeof(gDfuError));
  }
}

static void scheduleNextViewerSummary() {
  double epoch = currentEpoch();
  gNextViewerSummaryEpoch = tankalarm_computeNextAlignedEpoch(epoch, VIEWER_SUMMARY_BASE_HOUR, VIEWER_SUMMARY_INTERVAL_SECONDS);
}

static void initializeEthernet() {
  Serial.print(F("Initializing Ethernet..."));
  
  // Check if Ethernet hardware is present
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println(F(" FAILED - No Ethernet hardware detected!"));
    Serial.println(F("ERROR: Cannot continue without Ethernet. Please check hardware."));
    // Don't halt, just return and let the main loop handle the lack of network
    return;
  }

  // Retrieve hardware MAC address if not set. On PortentaEthernet the
  // factory MAC is not readable until AFTER Ethernet.begin() has brought
  // the PHY up, so before first begin() gMacAddress may still be zero.
  bool usingFactoryMac = true;
  if (!(gMacAddress[0] == 0 && gMacAddress[1] == 0 && gMacAddress[2] == 0 &&
        gMacAddress[3] == 0 && gMacAddress[4] == 0 && gMacAddress[5] == 0)) {
    // Application-provided MAC overrides the factory one.
    usingFactoryMac = false;
  }

  // Retry DHCP a few times. The first attempt often fails because the
  // PHY link has not finished auto-negotiating yet; spacing the retries
  // out by 1.5s lets the switch/router learn the port and respond.
  const int kMaxAttempts = 4;
  int status = 0;
  for (int attempt = 1; attempt <= kMaxAttempts && status == 0; ++attempt) {
    if (gConfig.useStaticIp) {
      status = Ethernet.begin(gMacAddress, gStaticIp, gStaticDns, gStaticGateway, gStaticSubnet);
    } else if (usingFactoryMac) {
      // No caller-supplied MAC: let PortentaEthernet use the factory MAC.
      status = Ethernet.begin();
    } else {
      status = Ethernet.begin(gMacAddress);
    }
    if (status == 0) {
      Serial.print(F(" attempt "));
      Serial.print(attempt);
      Serial.print(F(" failed"));
      if (attempt < kMaxAttempts) {
        Serial.print(F(", retrying..."));
        safeSleep(1500);
      } else {
        Serial.println();
      }
    }
  }

  if (status == 0) {
    Serial.println(F(" FAILED - Could not configure Ethernet!"));
    if (!gConfig.useStaticIp) {
      Serial.println(F("ERROR: DHCP failed after retries. Check network cable and DHCP server."));
    } else {
      Serial.println(F("ERROR: Static IP configuration failed."));
    }
    // Don't halt, just return and let the main loop handle the lack of network
    return;
  }

  // If the caller didn't supply a MAC, pull the factory MAC now (post-begin)
  // so we can log it and reuse it elsewhere.
  if (usingFactoryMac) {
    Ethernet.MACAddress(gMacAddress);
  }
  
  // Check link status
  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.println(F(" WARNING - No network cable connected!"));
    Serial.println(F("Continuing, but web server will not be accessible."));
  } else {
    Serial.println(F(" ok"));
    Serial.print(F("Using MAC: "));
    for (int i=0; i<6; i++) {
        if (i>0) Serial.print(":");
        if (gMacAddress[i] < 16) Serial.print("0");
        Serial.print(gMacAddress[i], HEX);
    }
    Serial.println(usingFactoryMac ? " (Hardware)" : " (Static)");
    Serial.print(F("IP Address: "));
    Serial.println(Ethernet.localIP());
    Serial.print(F("Gateway: "));
    Serial.println(Ethernet.gatewayIP());
    Serial.print(F("Subnet: "));
    Serial.println(Ethernet.subnetMask());
  }
}

static void serveCss(EthernetClient &client) {
  size_t cssLen = strlen_P(STYLE_CSS);
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/css"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(cssLen);
  client.println(F("Cache-Control: public, max-age=3600"));
  client.println();

  const char* ptr = STYLE_CSS;
  size_t remaining = cssLen;
  const size_t bufSize = 64; 
  uint8_t buffer[bufSize];

  while (remaining > 0) {
    size_t chunk = (remaining < bufSize) ? remaining : bufSize;
    for (size_t i = 0; i < chunk; i++) {
        buffer[i] = pgm_read_byte_near(ptr++);
    }
    client.write(buffer, chunk);
    remaining -= chunk;
  }
}

static void serveFile(EthernetClient &client, const char* htmlContent) {
  size_t htmlLen = strlen_P(htmlContent);
  
  // For very large HTML files, serve directly from PROGMEM without building a String
  // to avoid memory exhaustion. Skip loading overlay injection for these files.
  const size_t LARGE_FILE_THRESHOLD = 32768;  // 32KB threshold
  if (htmlLen > LARGE_FILE_THRESHOLD) {
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: text/html"));
    client.println(F("Connection: close"));
    client.println(F("Cache-Control: no-store"));
    client.print(F("Content-Length: "));
    client.println(htmlLen);
    client.println();
    
    // Send HTML content in chunks directly from PROGMEM
    // Use static buffer to avoid stack overflow on memory-constrained devices
    const size_t chunkSize = 512;
    static char buffer[chunkSize];
    size_t remaining = htmlLen;
    size_t offset = 0;
    
    while (remaining > 0) {
      size_t toRead = (remaining < chunkSize) ? remaining : chunkSize;
      for (size_t i = 0; i < toRead; ++i) {
        buffer[i] = (char)pgm_read_byte_near(htmlContent + offset + i);
      }
      client.write((const uint8_t*)buffer, toRead);
      offset += toRead;
      remaining -= toRead;
    }
    return;
  }
  
  // For smaller files, use the original method with loading overlay injection
  String body;
  body.reserve(htmlLen + 128);
  for (size_t i = 0; i < htmlLen; ++i) {
    body += (char)pgm_read_byte_near(htmlContent + i);
  }
  respondHtml(client, body);
}

static void serveServerSettingsPage(EthernetClient &client) {
  const size_t htmlLen = strlen_P(SERVER_SETTINGS_HTML);
  const size_t injectionLen = strlen_P(SERVER_SETTINGS_DFU_TARGET_INJECTION);
  String body;
  body.reserve(htmlLen + injectionLen + 64);

  for (size_t i = 0; i < htmlLen; ++i) {
    body += (char)pgm_read_byte_near(SERVER_SETTINGS_HTML + i);
  }

  String injection;
  injection.reserve(injectionLen + 1);
  for (size_t i = 0; i < injectionLen; ++i) {
    injection += (char)pgm_read_byte_near(SERVER_SETTINGS_DFU_TARGET_INJECTION + i);
  }

  const int insertAt = body.lastIndexOf(F("</body>"));
  if (insertAt >= 0) {
    String merged;
    merged.reserve(body.length() + injection.length() + 1);
    merged = body.substring(0, insertAt);
    merged += injection;
    merged += body.substring(insertAt);
    body = merged;
  } else {
    body += injection;
  }

  respondHtml(client, body);
}

static void handleSessionCheck(EthernetClient &client, const char *sessionHdr) {
  bool valid = sessionTokenMatches(sessionHdr);
  if (valid) {
    respondJson(client, String(F("{\"valid\":true}")));
  } else {
    respondJson(client, String(F("{\"valid\":false}")));
  }
}

static void emitSessionCookie(EthernetClient &client, const char *sessionValue) {
  client.print(F("Set-Cookie: tankalarm_session="));
  if (sessionValue && sessionValue[0] != '\0') {
    client.print(sessionValue);
  }
  client.print(F("; Path=/; HttpOnly; SameSite=Strict"));
  if (!sessionValue || sessionValue[0] == '\0') {
    client.print(F("; Max-Age=0"));
  }
  client.println();
}

static void handleLogoutPost(EthernetClient &client) {
  // Invalidate the current server-side session
  memset(gSessionToken, 0, sizeof(gSessionToken));
  static const char kLogoutBody[] = "{\"success\":true}";
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  emitSessionCookie(client, nullptr);
  client.println(F("Connection: close"));
  client.println(F("Cache-Control: no-store"));
  client.print(F("Content-Length: "));
  client.println(strlen(kLogoutBody));
  client.println();
  client.print(kLogoutBody);
}

static void handleLoginPost(EthernetClient &client, const String &body) {
  // Check rate limiting first (non-blocking)
  if (isAuthRateLimited(client)) {
    return;
  }
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }

  const char* pin = doc["pin"];
  bool valid = false;

  // If no PIN is configured yet (fresh install), accept any valid 4-digit PIN
  // and set it as the admin PIN
  if (!isValidPin(gConfig.configPin)) {
    if (pin && isValidPin(pin)) {
      strlcpy(gConfig.configPin, pin, sizeof(gConfig.configPin));
      saveConfig(gConfig);
      valid = true;
      Serial.println(F("Admin PIN set via first login"));
    } else {
      // Reject blank/invalid PIN on fresh install — must set a valid 4-digit PIN
      respondStatus(client, 400, "Please set a 4-digit PIN on first login");
      return;
    }
  } else if (pin && pinMatches(pin)) {
     valid = true;
  }

  if (valid) {
    // Successful login - reset failure counter
    resetAuthFailures();
    // Generate a new session token, invalidating any previous session
    generateSessionToken();
    static const char kLoginBody[] = "{\"success\":true,\"session\":\"cookie\"}";
    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    emitSessionCookie(client, gSessionToken);
    client.println(F("Connection: close"));
    client.println(F("Cache-Control: no-store"));
    client.print(F("Content-Length: "));
    client.println(strlen(kLoginBody));
    client.println();
    client.print(kLoginBody);
  } else {
    // Failed login - record failure (non-blocking)
    recordAuthFailure();
    
    client.println(F("HTTP/1.1 401 Unauthorized"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Connection: close"));
    client.println();
    client.println(F("{\"success\":false}"));
  }
}

static void handleWebRequests() {
  EthernetClient client = gWebServer.available();
  if (!client) {
    return;
  }

  // While an FTP backup is running, drop new web requests immediately so
  // browser polling does not pin LWIP socket slots that the FTPS data
  // channels need. The originating /api/ftp-backup request is not on this
  // path — it is already inside its handler waiting for performFtpBackup
  // to return — so this only affects subsequent connections.
  if (gBackupInProgress) {
    client.stop();
    return;
  }

  String method;
  String path;
  String body;
  size_t contentLength = 0;
  bool bodyTooLarge = false;

  char sessionHdr[17] = "";

  if (!readHttpRequest(client, method, path, body, contentLength, bodyTooLarge, sessionHdr, sizeof(sessionHdr))) {
    respondStatus(client, 400, "Bad Request");
    client.stop();
    return;
  }

  if (bodyTooLarge) {
    respondStatus(client, 413, "Payload Too Large");
    client.stop();
    return;
  }

  // Session validation middleware: all /api/ routes except login and session-check
  // must carry a valid session via X-Session or the HttpOnly cookie.
  if (path.startsWith("/api/") &&
      path != "/api/login" &&
      path != "/api/session/check") {
    if (!sessionTokenMatches(sessionHdr)) {
      respondStatus(client, 401, "Session expired");
      client.stop();
      return;
    }
  }

  if (method == "GET" && path.startsWith("/login")) {
    serveFile(client, LOGIN_HTML);
  } else if (method == "GET" && path == "/") {
    serveFile(client, DASHBOARD_HTML);
  } else if (method == "GET" && path == "/style.css") {
    serveCss(client);
  } else if (method == "GET" && path == "/client-console") {
    serveFile(client, CLIENT_CONSOLE_HTML);
  } else if (method == "GET" && (path == "/config-generator" || path.startsWith("/config-generator?"))) {
    serveFile(client, CONFIG_GENERATOR_HTML);
  } else if (method == "GET" && (path == "/site-config" || path.startsWith("/site-config?"))) {
    serveFile(client, SITE_CONFIG_HTML);
  } else if (method == "GET" && path == "/serial-monitor") {
    serveFile(client, SERIAL_MONITOR_HTML);
  } else if (method == "GET" && path == "/calibration") {
    serveFile(client, CALIBRATION_HTML);
  } else if (method == "GET" && path == "/contacts") {
    serveFile(client, CONTACTS_MANAGER_HTML);
  } else if (method == "GET" && path == "/server-settings") {
    serveServerSettingsPage(client);
  } else if (method == "GET" && path == "/email-format") {
    serveFile(client, EMAIL_FORMAT_HTML);
  } else if (method == "GET" && path == "/historical") {
    serveFile(client, HISTORICAL_DATA_HTML);
  } else if (method == "GET" && path == "/transmission-log") {
    serveFile(client, TRANSMISSION_LOG_HTML);
  } else if (method == "POST" && path == "/api/login") {
    handleLoginPost(client, body);
  } else if (method == "POST" && path == "/api/logout") {
    handleLogoutPost(client);
  } else if (method == "GET" && path.startsWith("/api/session/check")) {
    handleSessionCheck(client, sessionHdr);
  } else if (method == "GET" && (path == "/api/history" || path.startsWith("/api/history?"))) {
    String histQuery = "";
    int histQs = path.indexOf('?');
    if (histQs >= 0) histQuery = path.substring(histQs + 1);
    sendHistoryJson(client, histQuery);
  } else if (method == "GET" && path.startsWith("/api/history/compare")) {
    // Parse query params: ?current=YYYYMM&previous=YYYYMM
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleHistoryCompare(client, queryString);
  } else if (method == "GET" && path.startsWith("/api/history/archived")) {
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleArchivedClients(client, queryString);
  } else if (method == "GET" && path.startsWith("/api/history/yoy")) {
    // Parse query params: ?year=YYYY or ?sensor=X&years=3
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleHistoryYearOverYear(client, queryString);
  } else if (method == "GET" && path == "/api/sensors") {
    sendSensorJson(client);
  } else if (method == "GET" && path == "/api/unloads") {
    sendUnloadLogJson(client);
  } else if (method == "GET" && path.startsWith("/api/clients")) {
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    sendClientDataJson(client, queryString);
  } else if (method == "GET" && path.startsWith("/api/client?")) {
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleClientConfigGet(client, queryString);
  } else if (method == "GET" && path == "/api/calibration") {
    handleCalibrationGet(client);
  } else if (method == "GET" && path == "/api/contacts") {
    // Note: PII (phone/email) exposed on LAN — add auth if ever internet-exposed
    handleContactsGet(client);
  } else if (method == "GET" && path == "/api/email-format") {
    handleEmailFormatGet(client);
  } else if (method == "GET" && path.startsWith("/api/serial-logs")) {
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleSerialLogsGet(client, queryString);
  } else if (method == "GET" && path.startsWith("/api/serial-export")) {
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleSerialLogsDownload(client, queryString);
  } else if (method == "POST" && path == "/api/serial-request") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleSerialRequestPost(client, body);
    }
  } else if (method == "POST" && path == "/api/calibration") {
    if (contentLength > 512) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleCalibrationPost(client, body);
    }
  } else if (method == "DELETE" && path == "/api/calibration") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleCalibrationDelete(client, body);
    }
  } else if (method == "DELETE" && path == "/api/client") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleClientDeleteRequest(client, body);
    }
  } else if (method == "POST" && path == "/api/contacts") {
    if (contentLength > 8192) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleContactsPost(client, body);
    }
  } else if (method == "POST" && path == "/api/email-format") {
    if (contentLength > 2048) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleEmailFormatPost(client, body);
    }
  } else if (method == "POST" && path == "/api/server-settings") {
    if (contentLength > 2048) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleServerSettingsPost(client, body);
    }
  } else if (method == "POST" && path == "/api/config") {
    if (contentLength > MAX_HTTP_BODY_BYTES) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleConfigPost(client, body);
    }
  } else if (method == "POST" && path == "/api/config/retry") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleConfigRetryPost(client, body);
    }
  } else if (method == "POST" && path == "/api/config/cancel") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleConfigCancelPost(client, body);
    }
  } else if (method == "POST" && path == "/api/sync-request") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleSyncRequestPost(client, body);
    }
  } else if (method == "POST" && path == "/api/pin") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handlePinPost(client, body);
    }
  } else if (method == "POST" && path == "/api/refresh") {
    if (contentLength > 512) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleRefreshPost(client, body);
    }
  } else if (method == "POST" && path == "/api/relay") {
    if (contentLength > 512) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleRelayPost(client, body);
    }
  } else if (method == "POST" && path == "/api/relay/clear") {
    if (contentLength > 512) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleRelayClearPost(client, body);
    }
  } else if (method == "POST" && path == "/api/pause") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handlePausePost(client, body);
    }
  } else if (method == "POST" && path == "/api/ftp-backup") {
    if (contentLength > 512) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleFtpBackupPost(client, body);
    }
  } else if (method == "POST" && path == "/api/ftp-restore") {
    if (contentLength > 512) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleFtpRestorePost(client, body);
    }
  } else if (method == "GET" && path == "/api/ftp-backup-status") {
    handleFtpBackupStatusGet(client);
  } else if (method == "GET" && path == "/api/system-status") {
    handleSystemStatusGet(client);
  } else if (method == "GET" && path == "/api/transmission-log") {
    handleTransmissionLogGet(client);
  } else if (method == "GET" && path == "/api/notecard/status") {
    handleNotecardStatusGet(client);
  } else if (method == "GET" && path == "/api/github/update") {
    handleGitHubUpdateStatusGet(client);
  } else if (method == "GET" && path == "/api/dfu/status") {
    handleDfuStatusGet(client);
  } else if (path.startsWith("/api/debug/sensors")) {
    String queryString = "";
    int queryStart = path.indexOf('?');
    if (queryStart >= 0) {
      queryString = path.substring(queryStart + 1);
    }
    handleDebugSensors(client, method, body, queryString);
  } else if (method == "POST" && path == "/api/dfu/check") {
    handleDfuCheckPost(client, body);
  } else if (method == "POST" && path == "/api/dfu/enable") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleDfuEnablePost(client, body);
    }
  } else if (method == "POST" && path == "/api/location") {
    if (contentLength > 256) {
      respondStatus(client, 413, "Payload Too Large");
    } else {
      handleLocationRequestPost(client, body);
    }
  } else if (method == "GET" && path.startsWith("/api/location")) {
    handleLocationGet(client, path);
  } else {
    respondStatus(client, 404, F("Not Found"));
  }

  safeSleep(1);
  client.stop();
}

static bool extractCookieValue(const char *cookieHeader, const char *cookieName, char *out, size_t outSize) {
  if (!cookieHeader || !cookieName || !out || outSize == 0) {
    return false;
  }

  out[0] = '\0';
  size_t nameLen = strlen(cookieName);
  const char *cursor = cookieHeader;
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == ';') {
      cursor++;
    }
    if (strncmp(cursor, cookieName, nameLen) == 0 && cursor[nameLen] == '=') {
      cursor += nameLen + 1;
      size_t valueLen = 0;
      while (cursor[valueLen] != '\0' && cursor[valueLen] != ';') {
        valueLen++;
      }
      while (valueLen > 0 && cursor[valueLen - 1] == ' ') {
        valueLen--;
      }
      if (valueLen >= outSize) {
        valueLen = outSize - 1;
      }
      memcpy(out, cursor, valueLen);
      out[valueLen] = '\0';
      return valueLen > 0;
    }
    const char *next = strchr(cursor, ';');
    if (!next) {
      break;
    }
    cursor = next + 1;
  }

  return false;
}

static bool readHttpRequest(EthernetClient &client, String &method, String &path, String &body, size_t &contentLength, bool &bodyTooLarge, char *sessionHdr, size_t sessionHdrSize) {
  method = "";
  path = "";
  contentLength = 0;
  body = "";
  bodyTooLarge = false;
  sessionHdr[0] = '\0';

  // Fixed buffer for header line parsing — avoids heap-fragmenting String concatenation.
  // 514 bytes: 512 max header line + CR + NUL.
  static char lineBuf[514];
  size_t lineLen = 0;
  bool firstLine = true;
  int headerCount = 0;
  static const int MAX_HEADERS = 64;  // Prevent excessive header processing
  char sessionCookie[17] = "";

  unsigned long start = millis();
  while (client.connected() && millis() - start < 5000UL) {
    if (!client.available()) {
      safeSleep(1);
      continue;
    }

    char c = client.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen == 0) {
        break; // end of headers
      }
      if (firstLine) {
        // Parse: "METHOD /path HTTP/1.1"
        char *sp1 = strchr(lineBuf, ' ');
        if (!sp1) {
          return false;
        }
        *sp1 = '\0';
        method = lineBuf;
        char *pathStart = sp1 + 1;
        char *sp2 = strchr(pathStart, ' ');
        if (!sp2) {
          return false;
        }
        *sp2 = '\0';
        path = pathStart;
        firstLine = false;
      } else {
        // Parse header: "Key: Value"
        headerCount++;
        if (headerCount > MAX_HEADERS) {
          return false;  // Too many headers — reject request
        }
        char *colon = strchr(lineBuf, ':');
        if (colon && colon > lineBuf) {
          *colon = '\0';
          // Trim key (in-place, key is short)
          char *key = lineBuf;
          // Trim value
          char *val = colon + 1;
          while (*val == ' ') val++;
          // Check Content-Length (case-insensitive)
          if (strcasecmp(key, "Content-Length") == 0) {
            contentLength = (size_t)atol(val);
            if (contentLength > MAX_HTTP_BODY_BYTES) {
              bodyTooLarge = true;
              contentLength = MAX_HTTP_BODY_BYTES;
            }
          } else if (strcasecmp(key, "X-Session") == 0) {
            strlcpy(sessionHdr, val, sessionHdrSize);
          } else if (strcasecmp(key, "Cookie") == 0) {
            extractCookieValue(val, "tankalarm_session", sessionCookie, sizeof(sessionCookie));
          }
        }
      }
      lineLen = 0;
    } else {
      if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      } else {
        return false;  // Header line too long
      }
    }
  }

  if (sessionCookie[0] != '\0') {
    strlcpy(sessionHdr, sessionCookie, sessionHdrSize);
  }

  // If the body is already too large, don't read it into RAM.
  // We'll respond with 413 and close the connection.
  if (bodyTooLarge) {
    return true;
  }

  if (contentLength > 0) {
    body.reserve(contentLength);  // Pre-allocate to avoid O(n²) reallocation
    size_t readBytes = 0;
    unsigned long bodyStart = millis();
    while (readBytes < contentLength && client.connected() && millis() - bodyStart < 5000UL) {
      while (client.available() && readBytes < contentLength) {
        char c = client.read();
        body += c;
        readBytes++;
      }
      if (readBytes >= MAX_HTTP_BODY_BYTES) {
        bodyTooLarge = true;
        break;
      }
      if (readBytes < contentLength) {
        safeSleep(1);  // Yield CPU + kick watchdog while waiting for more data
      }
    }
  }

  return true;
}

static String urlDecode(const String &value) {
  String decoded;
  decoded.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%' && i + 2 < value.length()) {
      char hi = value.charAt(i + 1);
      char lo = value.charAt(i + 2);
      char hex[3] = {hi, lo, '\0'};
      decoded += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

static String getQueryParam(const String &query, const char *key) {
  size_t keyLen = strlen(key);
  size_t start = 0;
  while (start < query.length()) {
    size_t end = query.indexOf('&', start);
    if (end == (size_t)-1) {
      end = query.length();
    }
    String pair = query.substring(start, end);
    size_t eq = pair.indexOf('=');
    if (eq != (size_t)-1) {
      String k = pair.substring(0, eq);
      if (k == key) {
        String value = pair.substring(eq + 1);
        return urlDecode(value);
      }
    } else if (pair == key) {
      return String("true");
    }
    if (end == query.length()) {
      break;
    }
    start = end + 1;
  }
  return String();
}

/**
 * Stream a substring of a String to the client in 512-byte chunks.
 * Avoids creating a temporary String copy for the substring.
 */
static void sendStringRange(EthernetClient &client, const String &str, size_t start, size_t end) {
  const size_t chunkSize = 512;
  const char *data = str.c_str();
  size_t offset = start;
  while (offset < end) {
    size_t toSend = end - offset;
    if (toSend > chunkSize) toSend = chunkSize;
    client.write((const uint8_t*)(data + offset), toSend);
    offset += toSend;
  }
}

/**
 * Send an HTML response, injecting the loading-overlay spinner inline.
 *
 * Previous implementation created up to 3 String copies (output = body,
 * rebuilt = reassembled, output = rebuilt). This version streams the
 * original body directly, injecting the overlay and hide-script between
 * chunks so peak RAM stays at ~1x body size instead of ~3x.
 */
static void respondHtml(EthernetClient &client, const String &body) {
  const char *overlayMarkup = "<div id=\"loading-overlay\"><div class=\"spinner\"></div></div>";
  const char *hideScript = "<script>setTimeout(function(){var o=document.getElementById('loading-overlay');if(o)o.style.display='none'},5000);window.addEventListener('load',()=>{const ov=document.getElementById('loading-overlay');if(ov){ov.style.display='none';ov.classList.add('hidden');}});</script>";

  const size_t overlayLen = strlen(overlayMarkup);
  const size_t scriptLen  = strlen(hideScript);

  // Check if overlay is already embedded (e.g. by update_html.py)
  bool needsOverlay = (body.indexOf("loading-overlay") < 0);

  // Find injection points in the original body
  int bodyStart = -1, bodyEnd = -1, bodyClose = -1;
  if (needsOverlay) {
    bodyStart = body.indexOf("<body");
    bodyEnd   = (bodyStart >= 0) ? body.indexOf('>', bodyStart) : -1;
    bodyClose = body.lastIndexOf("</body>");
  }

  // Calculate total content length without building a new String
  size_t totalLen = body.length();
  bool injectBoth  = (bodyEnd >= 0 && bodyClose > bodyEnd);
  bool injectAfter = (bodyEnd >= 0 && bodyClose <= bodyEnd);  // <body> but no </body>
  if (needsOverlay) {
    if (injectBoth) {
      totalLen += overlayLen + scriptLen;
    } else if (injectAfter) {
      totalLen += overlayLen + scriptLen;
    } else {
      totalLen += scriptLen;  // append script at end
    }
  }

  // Send HTTP headers
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println(F("Cache-Control: no-store"));
  client.print(F("Content-Length: "));
  client.println(totalLen);
  client.println();

  // Stream body parts, injecting overlay inline
  if (needsOverlay && injectBoth) {
    // Part 1: everything up to and including <body...>
    sendStringRange(client, body, 0, (size_t)(bodyEnd + 1));
    // Part 2: overlay markup (from flash-string literal)
    client.write((const uint8_t*)overlayMarkup, overlayLen);
    // Part 3: content between <body...> and </body>
    sendStringRange(client, body, (size_t)(bodyEnd + 1), (size_t)bodyClose);
    // Part 4: hide script
    client.write((const uint8_t*)hideScript, scriptLen);
    // Part 5: </body> onwards
    sendStringRange(client, body, (size_t)bodyClose, body.length());
  } else if (needsOverlay && injectAfter) {
    sendStringRange(client, body, 0, (size_t)(bodyEnd + 1));
    client.write((const uint8_t*)overlayMarkup, overlayLen);
    sendStringRange(client, body, (size_t)(bodyEnd + 1), body.length());
    client.write((const uint8_t*)hideScript, scriptLen);
  } else if (needsOverlay) {
    // No <body> tag found — just append the script
    sendStringRange(client, body, 0, body.length());
    client.write((const uint8_t*)hideScript, scriptLen);
  } else {
    // Overlay already present — stream as-is
    sendStringRange(client, body, 0, body.length());
  }
}

static void respondJson(EthernetClient &client, const String &body, int status) {
  client.print(F("HTTP/1.1 "));
  client.print(status);
  switch (status) {
    case 200: client.println(F(" OK")); break;
    case 400: client.println(F(" Bad Request")); break;
    case 401: client.println(F(" Unauthorized")); break;
    case 403: client.println(F(" Forbidden")); break;
    case 404: client.println(F(" Not Found")); break;
    case 413: client.println(F(" Payload Too Large")); break;
    case 500: client.println(F(" Internal Server Error")); break;
    default:  client.println(F(" Error")); break;
  }
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(body.length());
  client.println();
  
  // Send in chunks to avoid memory issues with large strings
  const size_t chunkSize = 512;
  size_t remaining = body.length();
  size_t offset = 0;
  
  while (remaining > 0) {
    size_t toSend = (remaining < chunkSize) ? remaining : chunkSize;
    client.write((const uint8_t*)body.c_str() + offset, toSend);
    offset += toSend;
    remaining -= toSend;
  }
  client.flush();
}

static bool respondJson(EthernetClient &client, const JsonDocument &doc, int status) {
  const size_t length = measureJson(doc);
  client.print(F("HTTP/1.1 "));
  client.print(status);
  if (status == 200) {
    client.println(F(" OK"));
  } else {
    client.println();
  }
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(length);
  client.println();
  return serializeJson(doc, client) > 0;
}

// Convenience overloads defaulting to HTTP 200
static void respondJson(EthernetClient &client, const String &body) {
  respondJson(client, body, 200);
}

static bool respondJson(EthernetClient &client, const JsonDocument &doc) {
  return respondJson(client, doc, 200);
}

static void beginChunkedCsvDownload(EthernetClient &client, const String &filename) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/csv"));
  client.println(F("Connection: close"));
  client.print(F("Content-Disposition: attachment; filename=\""));
  client.print(filename);
  client.println(F("\""));
  client.println(F("Transfer-Encoding: chunked"));
  client.println();
}

static void sendChunk(EthernetClient &client, const char *data, size_t len) {
  if (!data || len == 0) {
    return;
  }
  client.print(len, HEX);
  client.print("\r\n");
  client.write(reinterpret_cast<const uint8_t *>(data), len);
  client.print("\r\n");
}

static void sendChunk(EthernetClient &client, const String &data) {
  sendChunk(client, data.c_str(), data.length());
}

static void endChunkedResponse(EthernetClient &client) {
  client.print("0\r\n\r\n");
}

static size_t buildSerialLogCsvLine(char *out, size_t outSize, const SerialLogEntry &entry, const char *clientLabel) {
  if (!out || outSize == 0) {
    return 0;
  }

  String ts(entry.timestamp, 3);
  size_t pos = 0;

  auto appendRaw = [&](const char *s, size_t n) {
    if (!s || n == 0) {
      return true;
    }
    if (pos + n >= outSize) {
      return false;
    }
    memcpy(out + pos, s, n);
    pos += n;
    return true;
  };

  auto appendCStr = [&](const char *s) {
    return appendRaw(s, s ? strlen(s) : 0);
  };

  auto appendChar = [&](char c) {
    if (pos + 1 >= outSize) {
      return false;
    }
    out[pos++] = c;
    return true;
  };

  if (!appendCStr(ts.c_str())) return 0;
  if (!appendChar(',')) return 0;
  if (!appendCStr(entry.level)) return 0;
  if (!appendChar(',')) return 0;
  if (!appendCStr(entry.source)) return 0;
  if (!appendChar(',')) return 0;
  if (!appendCStr(clientLabel ? clientLabel : "")) return 0;
  if (!appendRaw(",\"", 2)) return 0;

  const char *msg = entry.message;
  for (size_t i = 0; msg && msg[i] != '\0'; ++i) {
    if (msg[i] == '"') {
      if (!appendRaw("\"\"", 2)) return 0;
    } else {
      if (!appendChar(msg[i])) return 0;
    }
  }

  if (!appendRaw("\"\n", 2)) return 0;
  out[pos] = '\0';
  return pos;
}

static void respondStatus(EthernetClient &client, int status, const char *message) {
  client.print(F("HTTP/1.1 "));
  client.print(status);
  client.print(F(" "));
  if (status == 200) {
    client.println(F("OK"));
  } else if (status == 400) {
    client.println(F("Bad Request"));
  } else if (status == 404) {
    client.println(F("Not Found"));
  } else if (status == 500) {
    client.println(F("Internal Server Error"));
  } else {
    client.println(F("Error"));
  }
  client.println(F("Content-Type: text/plain"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(strlen(message));
  client.println();
  client.print(message);
}

static void respondStatus(EthernetClient &client, int status, const String &message) {
  respondStatus(client, status, message.c_str());
}



static void sendSensorJson(EthernetClient &client) {
  JsonDocument doc;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    JsonObject obj = arr.add<JsonObject>();
    obj["c"] = gSensorRecords[i].clientUid;
    obj["s"] = gSensorRecords[i].site;
    obj["n"] = gSensorRecords[i].label;
    if (gSensorRecords[i].contents[0] != '\0') {
      obj["cn"] = gSensorRecords[i].contents;
    }
    obj["k"] = gSensorRecords[i].sensorIndex;
    if (gSensorRecords[i].userNumber > 0) {
      obj["un"] = gSensorRecords[i].userNumber;
    }
    obj["l"] = gSensorRecords[i].levelInches;
    // Only include sensorMa for current-loop sensors (valid range is 4-20mA)
    if (gSensorRecords[i].sensorMa >= 4.0f) {
      obj["ma"] = gSensorRecords[i].sensorMa;
    }
    // Include sensor type if known
    if (gSensorRecords[i].sensorType[0] != '\0') {
      obj["st"] = gSensorRecords[i].sensorType;
    }
    // Include object type and measurement unit for calibration page
    if (gSensorRecords[i].objectType[0] != '\0') {
      obj["ot"] = gSensorRecords[i].objectType;
    }
    if (gSensorRecords[i].measurementUnit[0] != '\0') {
      obj["mu"] = gSensorRecords[i].measurementUnit;
    }
    // Calculate and include 24hr change if we have previous data
    if (gSensorRecords[i].previousLevelEpoch > 0.0) {
      float delta = gSensorRecords[i].levelInches - gSensorRecords[i].previousLevelInches;
      obj["d"] = delta;  // 24hr delta in inches
      obj["pe"] = gSensorRecords[i].previousLevelEpoch;  // when previous reading was taken
    }
    obj["a"] = gSensorRecords[i].alarmActive;
    obj["at"] = gSensorRecords[i].alarmType;
    obj["u"] = gSensorRecords[i].lastUpdateEpoch;
  }

  // Detect if we ran out of memory while building the document
  if (doc.overflowed()) {
    Serial.println(F("[ERROR] Sensor JSON document overflowed - too many sensor records for available memory"));
    respondStatus(client, 500, F("Sensor data too large"));
    return;
  }

  respondJson(client, doc);
}

// ============================================================================
// Unload Log JSON API
// ============================================================================
static void sendUnloadLogJson(EthernetClient &client) {
  // ArduinoJson v7: JsonDocument auto-sizes
  JsonDocument doc;
  doc["count"] = gUnloadLogCount;
  JsonArray arr = doc["unloads"].to<JsonArray>();
  
  // Return entries in reverse chronological order (newest first)
  for (uint8_t i = 0; i < gUnloadLogCount; ++i) {
    // Calculate index going backwards from write pointer
    uint8_t idx = (gUnloadLogWriteIndex + MAX_UNLOAD_LOG_ENTRIES - 1 - i) % MAX_UNLOAD_LOG_ENTRIES;
    const UnloadLogEntry &entry = gUnloadLog[idx];
    
    JsonObject obj = arr.add<JsonObject>();
    obj["t"] = entry.eventTimestamp;        // Event timestamp
    obj["pt"] = entry.peakTimestamp;         // Peak timestamp
    obj["s"] = entry.siteName;               // Site name
    obj["c"] = entry.clientUid;              // Client UID
    obj["n"] = entry.tankLabel;              // Tank label
    obj["k"] = entry.sensorIndex;             // Sensor index
    obj["pk"] = entry.peakInches;            // Peak height
    obj["em"] = entry.emptyInches;           // Empty height
    obj["dl"] = entry.peakInches - entry.emptyInches;  // Delivered amount
    if (entry.peakSensorMa >= 4.0f) {
      obj["pma"] = entry.peakSensorMa;       // Peak sensor mA
    }
    if (entry.emptySensorMa >= 4.0f) {
      obj["ema"] = entry.emptySensorMa;      // Empty sensor mA
    }
    if (entry.measurementUnit[0] != '\0') {
      obj["mu"] = entry.measurementUnit;      // Measurement unit
    }
    obj["sms"] = entry.smsNotified;          // SMS notification sent
  }
  
  if (doc.overflowed()) {
    Serial.println(F("[ERROR] Unload log JSON document overflowed"));
    respondStatus(client, 500, F("Unload data too large"));
    return;
  }

  respondJson(client, doc);
}

static void sendClientDataJson(EthernetClient &client, const String &query) {
  // Large JSON document; ArduinoJson allocates the backing store on the heap.
  JsonDocument doc;

  // Parse optional query parameters to allow summary/filtered responses.
  String clientFilter;
  if (query.length() > 0) {
    clientFilter = getQueryParam(query, "client");
  }
  const bool summaryOnly = (query.indexOf(F("summary=1")) >= 0) || (query.indexOf(F("thin=1")) >= 0);

  JsonObject serverObj = doc["srv"].to<JsonObject>();
  serverObj["n"] = gConfig.serverName;
  serverObj["cf"] = gConfig.clientFleet;
  serverObj["pu"] = gConfig.productUid;
  serverObj["sp"] = gConfig.smsPrimary;
  serverObj["ss"] = gConfig.smsSecondary;
  serverObj["de"] = gConfig.dailyEmail;
  serverObj["dh"] = gConfig.dailyHour;
  serverObj["dm"] = gConfig.dailyMinute;
  serverObj["wrs"] = gConfig.webRefreshSeconds;
  serverObj["soh"] = gConfig.smsOnHigh;
  serverObj["sol"] = gConfig.smsOnLow;
  serverObj["soc"] = gConfig.smsOnClear;
  serverObj["sds"] = gConfig.serverDownSmsEnabled;
  serverObj["ve"] = gConfig.viewerEnabled;
  serverObj["up"] = gConfig.updatePolicy;
  serverObj["ccva"] = gConfig.checkClientVersionAlerts;
  serverObj["cvva"] = gConfig.checkViewerVersionAlerts;
  serverObj["pc"] = isValidPin(gConfig.configPin);
  serverObj["ps"] = gPaused;

  JsonObject ftpObj = serverObj["ftp"].to<JsonObject>();
  ftpObj["enabled"] = gConfig.ftpEnabled;
  ftpObj["passive"] = gConfig.ftpPassive;
  ftpObj["backupOnChange"] = gConfig.ftpBackupOnChange;
  ftpObj["restoreOnBoot"] = gConfig.ftpRestoreOnBoot;
  ftpObj["port"] = gConfig.ftpPort;
  ftpObj["host"] = gConfig.ftpHost;
  ftpObj["user"] = gConfig.ftpUser;
  ftpObj["path"] = gConfig.ftpPath;
  ftpObj["passwordSet"] = (gConfig.ftpPass[0] != '\0');
  ftpObj["ftpsEnabled"] = gConfig.ftpsEnabled;
  ftpObj["ftpsTrustMode"] = gConfig.ftpsTrustMode;
  if (gConfig.ftpsFingerprint[0] != '\0') {
    ftpObj["ftpsFingerprint"] = gConfig.ftpsFingerprint;
  }
  if (gConfig.ftpsTlsServerName[0] != '\0') {
    ftpObj["ftpsTlsServerName"] = gConfig.ftpsTlsServerName;
  }

  // Legacy abbreviated keys retained for compatibility with old consumers.
  ftpObj["en"] = gConfig.ftpEnabled;
  ftpObj["pas"] = gConfig.ftpPassive;
  ftpObj["boc"] = gConfig.ftpBackupOnChange;
  ftpObj["rob"] = gConfig.ftpRestoreOnBoot;
  ftpObj["pt"] = gConfig.ftpPort;
  ftpObj["hst"] = gConfig.ftpHost;
  ftpObj["usr"] = gConfig.ftpUser;
  ftpObj["pth"] = gConfig.ftpPath;
  ftpObj["pset"] = (gConfig.ftpPass[0] != '\0');
  ftpObj["ftps"] = gConfig.ftpsEnabled;
  ftpObj["tm"] = gConfig.ftpsTrustMode;
  if (gConfig.ftpsFingerprint[0] != '\0') {
    ftpObj["fp"] = gConfig.ftpsFingerprint;
  }
  if (gConfig.ftpsTlsServerName[0] != '\0') {
    ftpObj["tsn"] = gConfig.ftpsTlsServerName;
  }

  doc["si"] = gServerUid;
  doc["nde"] = gNextDailyEmailEpoch;
  doc["lse"] = gLastSyncedEpoch;
  // Force RAM copies so fwv/fwb always serialize, even if the macros are PROGMEM literals.
  doc["fwv"] = String((const char*)FIRMWARE_VERSION);
  doc["fwb"] = String((const char*)FIRMWARE_BUILD_DATE);
  
  // Server voltage readings
  // "sv" = best available voltage (analog Vin if wired, else Notecard supply)
  // "ncv" = Notecard V+ rail voltage (~5V regulated, NOT the battery)
  // "inv" = analog input voltage (actual battery, 0 if not wired)
  float bestVoltage = (gInputVoltage > 0.5f) ? gInputVoltage : gNotecardVoltage;
  if (bestVoltage > 0.0f) {
    doc["sv"] = bestVoltage;
    doc["sve"] = gVoltageEpoch;
  }
  if (gNotecardVoltage > 0.0f) {
    doc["ncv"] = gNotecardVoltage;
  }
  if (gInputVoltage > 0.5f) {
    doc["inv"] = gInputVoltage;
  }
  
  // DFU status
  doc["dfuAvail"] = gDfuUpdateAvailable;
  if (gDfuUpdateAvailable && gDfuVersion[0] != '\0') {
    doc["dfuVer"] = gDfuVersion;
  }
  doc["dfuProg"] = gDfuInProgress;

  // Build client list from sensor records (needed for both summary and full responses)
  JsonArray clientsArr = doc["cs"].to<JsonArray>();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    const SensorRecord &rec = gSensorRecords[i];

    if (clientFilter.length() > 0 && strcmp(rec.clientUid, clientFilter.c_str()) != 0) {
      continue;  // Skip non-matching clients when filter is applied
    }

    JsonObject clientObj;
    for (JsonObject existing : clientsArr) {
      const char *uid = existing["c"];
      if (uid && strcmp(uid, rec.clientUid) == 0) {
        clientObj = existing;
        break;
      }
    }

    if (!clientObj) {
      clientObj = clientsArr.add<JsonObject>();
      clientObj["c"] = rec.clientUid;
      clientObj["s"] = rec.site;
      clientObj["a"] = false;
      clientObj["u"] = 0.0;
      
      // Add VIN voltage from client metadata if available
      ClientMetadata *meta = findClientMetadata(rec.clientUid);
      if (meta && meta->vinVoltage > 0.0f) {
        clientObj["v"] = meta->vinVoltage;
        clientObj["ve"] = meta->vinVoltageEpoch;
      }
      // Add firmware version from client metadata
      if (meta && meta->firmwareVersion[0] != '\0') {
        clientObj["fv"] = meta->firmwareVersion;
      }
      // Add cellular signal strength from client metadata
      if (meta && meta->signalBars >= 0) {
        JsonObject sigObj = clientObj["sig"].to<JsonObject>();
        sigObj["bars"] = meta->signalBars;
        if (meta->signalRssi != 0) sigObj["rssi"] = meta->signalRssi;
        if (meta->signalRsrp != 0) sigObj["rsrp"] = meta->signalRsrp;
        if (meta->signalRsrq != 0) sigObj["rsrq"] = meta->signalRsrq;
        if (meta->signalRat[0] != '\0') sigObj["rat"] = meta->signalRat;
        sigObj["epoch"] = meta->signalEpoch;
      }
    }

    const char *existingSite = clientObj["s"] ? clientObj["s"].as<const char *>() : nullptr;
    if (!existingSite || strlen(existingSite) == 0) {
      clientObj["s"] = rec.site;
    }

    if (rec.alarmActive) {
      clientObj["a"] = true;
      clientObj["at"] = rec.alarmType;
    }

    double previousUpdate = clientObj["u"].is<double>() ? clientObj["u"].as<double>() : 0.0;
    if (rec.lastUpdateEpoch > previousUpdate) {
      clientObj["n"] = rec.label;
      clientObj["k"] = rec.sensorIndex;
      if (rec.userNumber > 0) {
        clientObj["un"] = rec.userNumber;
      }
      clientObj["l"] = rec.levelInches;
      // Only include sensorMa for current-loop sensors (valid range is 4-20mA)
      if (rec.sensorMa >= 4.0f) {
        clientObj["ma"] = rec.sensorMa;
      }
      clientObj["u"] = rec.lastUpdateEpoch;
      clientObj["at"] = rec.alarmType;
      // Include object type and measurement unit at client level for dashboard
      if (rec.objectType[0] != '\0') {
        clientObj["ot"] = rec.objectType;
      }
      if (rec.measurementUnit[0] != '\0') {
        clientObj["mu"] = rec.measurementUnit;
      }
    }

    JsonArray sensorList;
    if (!clientObj["ts"]) {
      sensorList = clientObj["ts"].to<JsonArray>();
    } else {
      sensorList = clientObj["ts"].as<JsonArray>();
    }
    JsonObject sensorObj = sensorList.add<JsonObject>();
    sensorObj["n"] = rec.label;
    sensorObj["k"] = rec.sensorIndex;
    if (rec.userNumber > 0) {
      sensorObj["un"] = rec.userNumber;
    }
    sensorObj["l"] = rec.levelInches;
    // Only include sensorMa for current-loop sensors (valid range is 4-20mA)
    if (rec.sensorMa >= 4.0f) {
      sensorObj["ma"] = rec.sensorMa;
    }
    // Include sensor type if known
    if (rec.sensorType[0] != '\0') {
      sensorObj["st"] = rec.sensorType;
    }
    // Include object type (tank, gas, rpm, etc.)
    if (rec.objectType[0] != '\0') {
      sensorObj["ot"] = rec.objectType;
    }
    // Include measurement unit
    if (rec.measurementUnit[0] != '\0') {
      sensorObj["mu"] = rec.measurementUnit;
    }
    // Include contents (e.g. "Diesel", "Water", "Propane")
    if (rec.contents[0] != '\0') {
      sensorObj["ct"] = rec.contents;
    }
    // Include 24hr change if available
    if (rec.previousLevelEpoch > 0.0) {
      sensorObj["d"] = rec.levelInches - rec.previousLevelInches;
    }
    sensorObj["a"] = rec.alarmActive;
    sensorObj["at"] = rec.alarmType;
    sensorObj["u"] = rec.lastUpdateEpoch;

    clientObj["tc"] = sensorList.size();
  }

  // Include clients that have cached configs but no telemetry data yet.
  // This ensures they appear in Active Sites even before their first report.
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    const ClientConfigSnapshot &snap = gClientConfigs[i];
    if (snap.uid[0] == '\0') continue;

    if (clientFilter.length() > 0 && strcmp(snap.uid, clientFilter.c_str()) != 0) {
      continue;
    }

    // Check if this client is already in the cs array from telemetry
    bool alreadyInList = false;
    for (JsonObject existing : clientsArr) {
      const char *uid = existing["c"];
      if (uid && strcmp(uid, snap.uid) == 0) {
        alreadyInList = true;
        break;
      }
    }
    if (!alreadyInList) {
      JsonObject clientObj = clientsArr.add<JsonObject>();
      clientObj["c"] = snap.uid;
      clientObj["s"] = snap.site;
      clientObj["a"] = false;
      clientObj["u"] = 0.0;
      clientObj["tc"] = 0;
    }
  }

  // In summary mode, return server metadata + client list but skip config payloads
  if (summaryOnly) {
    if (doc.overflowed()) {
      Serial.println(F("[ERROR] Client summary JSON overflowed"));
      respondStatus(client, 500, F("Client data too large"));
      return;
    }
    respondJson(client, doc);
    return;
  }

  JsonArray configsArr = doc["cfgs"].to<JsonArray>();
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    ClientConfigSnapshot &snap = gClientConfigs[i];

    if (clientFilter.length() > 0 && strcmp(snap.uid, clientFilter.c_str()) != 0) {
      continue;
    }

    JsonObject cfgEntry = configsArr.add<JsonObject>();
    cfgEntry["c"] = snap.uid;
    cfgEntry["s"] = snap.site;
    cfgEntry["cj"] = snap.payload;
    cfgEntry["pd"] = snap.pendingDispatch;
    if (snap.dispatchAttempts > 0) {
      cfgEntry["da"] = snap.dispatchAttempts;
      cfgEntry["mr"] = MAX_CONFIG_DISPATCH_RETRIES;
    }
    if (snap.lastDispatchEpoch > 0.0) {
      cfgEntry["de"] = snap.lastDispatchEpoch;
    }
    if (snap.configVersion[0] != '\0') {
      cfgEntry["cv"] = snap.configVersion;
    }
    if (snap.lastAckEpoch > 0.0) {
      cfgEntry["ae"] = snap.lastAckEpoch;
      cfgEntry["as"] = snap.lastAckStatus;
    }
  }

  // Detect if we ran out of memory while building the document
  if (doc.overflowed()) {
    Serial.println(F("[ERROR] Client data JSON document overflowed"));
    respondStatus(client, 500, F("Client data too large"));
    return;
  }

  respondJson(client, doc);
}

static void handleConfigPost(EthernetClient &client, const String &body) {
  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  if (doc.containsKey("server") && doc["server"].is<JsonObject>()) {
    JsonObject serverObj = doc["server"].as<JsonObject>();
    if (serverObj.containsKey("smsPrimary")) {
      strlcpy(gConfig.smsPrimary, serverObj["smsPrimary"], sizeof(gConfig.smsPrimary));
    }
    if (serverObj.containsKey("smsSecondary")) {
      strlcpy(gConfig.smsSecondary, serverObj["smsSecondary"], sizeof(gConfig.smsSecondary));
    }
    if (serverObj.containsKey("dailyEmail")) {
      strlcpy(gConfig.dailyEmail, serverObj["dailyEmail"], sizeof(gConfig.dailyEmail));
    }
    if (serverObj.containsKey("dailyHour")) {
      gConfig.dailyHour = serverObj["dailyHour"].as<uint8_t>();
    }
    if (serverObj.containsKey("dailyMinute")) {
      gConfig.dailyMinute = serverObj["dailyMinute"].as<uint8_t>();
    }
    if (serverObj.containsKey("webRefreshSeconds")) {
      gConfig.webRefreshSeconds = serverObj["webRefreshSeconds"].as<uint16_t>();
    }
    if (serverObj.containsKey("smsOnHigh")) {
      gConfig.smsOnHigh = serverObj["smsOnHigh"].as<bool>();
    }
    if (serverObj.containsKey("smsOnLow")) {
      gConfig.smsOnLow = serverObj["smsOnLow"].as<bool>();
    }
    if (serverObj.containsKey("smsOnClear")) {
      gConfig.smsOnClear = serverObj["smsOnClear"].as<bool>();
    }

    if (serverObj.containsKey("ftp") && serverObj["ftp"].is<JsonObject>()) {
      JsonObject ftpObj = serverObj["ftp"].as<JsonObject>();
      if (ftpObj.containsKey("enabled")) {
        gConfig.ftpEnabled = ftpObj["enabled"].as<bool>();
      }
      if (ftpObj.containsKey("passive")) {
        gConfig.ftpPassive = ftpObj["passive"].as<bool>();
      }
      if (ftpObj.containsKey("backupOnChange")) {
        gConfig.ftpBackupOnChange = ftpObj["backupOnChange"].as<bool>();
      }
      if (ftpObj.containsKey("restoreOnBoot")) {
        gConfig.ftpRestoreOnBoot = ftpObj["restoreOnBoot"].as<bool>();
      }
      if (ftpObj.containsKey("port")) {
        gConfig.ftpPort = ftpObj["port"].as<uint16_t>();
      }
      if (ftpObj.containsKey("host")) {
        strlcpy(gConfig.ftpHost, ftpObj["host"], sizeof(gConfig.ftpHost));
      }
      if (ftpObj.containsKey("user")) {
        strlcpy(gConfig.ftpUser, ftpObj["user"], sizeof(gConfig.ftpUser));
      }
      if (ftpObj.containsKey("pass")) {
        const char *passVal = ftpObj["pass"].as<const char *>();
        if (passVal && strlen(passVal) > 0) {
          strlcpy(gConfig.ftpPass, passVal, sizeof(gConfig.ftpPass));
        }
      }
      if (ftpObj.containsKey("path")) {
        strlcpy(gConfig.ftpPath, ftpObj["path"], sizeof(gConfig.ftpPath));
      }
      if (ftpObj.containsKey("ftpsEnabled")) {
        gConfig.ftpsEnabled = ftpObj["ftpsEnabled"].as<bool>();
      }
      if (ftpObj.containsKey("ftpsTrustMode")) {
        uint8_t mode = ftpObj["ftpsTrustMode"].as<uint8_t>();
        gConfig.ftpsTrustMode = (mode <= 1) ? mode : 0;
      }
      if (ftpObj.containsKey("ftpsFingerprint")) {
        strlcpy(gConfig.ftpsFingerprint, ftpObj["ftpsFingerprint"] | "", sizeof(gConfig.ftpsFingerprint));
      }
      if (ftpObj.containsKey("ftpsTlsServerName")) {
        strlcpy(gConfig.ftpsTlsServerName, ftpObj["ftpsTlsServerName"] | "", sizeof(gConfig.ftpsTlsServerName));
      }
    }
    gConfigDirty = true;
    scheduleNextDailyEmail();
  }

  if (doc["client"] && doc["config"]) {
    const char *clientUid = doc["client"].as<const char *>();
    if (clientUid && strlen(clientUid) > 0) {
      ConfigDispatchStatus status = dispatchClientConfig(clientUid, doc["config"]);
      if (status == ConfigDispatchStatus::PayloadTooLarge) {
        respondStatus(client, 413, F("Config payload too large"));
        return;
      }
      if (status == ConfigDispatchStatus::NotecardFailure) {
        respondStatus(client, 500, F("Failed to queue config"));
        return;
      }
      if (status == ConfigDispatchStatus::CachedOnly) {
        respondStatus(client, 202, F("Config saved locally — Notecard send failed (auto-retry every 60 min). Check serial monitor for details."));
        return;
      }
      if (status == ConfigDispatchStatus::OkWithPurge) {
        // Config queued successfully but older pending configs were purged
        char msg[128];
        snprintf(msg, sizeof(msg),
          "Config queued. WARNING: %u older pending config(s) were replaced in the outbox.",
          gLastConfigPurgeCount);
        respondStatus(client, 200, msg);
        return;
      }
    }
  }

  respondStatus(client, 200, F("OK"));
}

// Manually retry dispatching cached configs to Notecard.
// POST /api/config/retry  { "pin": "1234" }                 → retry all pending
// POST /api/config/retry  { "pin": "1234", "client": "uid" } → retry one client
static void handleConfigRetryPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) return;

  const char *clientUid = doc["client"].as<const char *>();
  if (clientUid && clientUid[0] != '\0') {
    // Retry specific client
    ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
    if (!snap || snap->payload[0] == '\0') {
      respondStatus(client, 404, F("No cached config for this client"));
      return;
    }

    if (sendConfigViaNotecard(clientUid, snap->payload)) {
      // BugFix v1.6.2 (I-6/M-10): Keep pendingDispatch true until client ACK arrives.
      // Previously, manual retry cleared the flag immediately, so if the Notecard
      // note was lost in transit, auto-retry would never pick it up again.
      snap->pendingDispatch = true;
      snap->dispatchAttempts = 1;  // Reset so auto-retry quota restarts
      respondStatus(client, 200, F("Config dispatched to Notecard (pending ACK)"));
    } else {
      respondStatus(client, 502, F("Notecard send failed — check serial monitor for details"));
    }
    return;
  }

  // Retry all pending
  uint8_t dispatched = 0;
  uint8_t stillPending = 0;
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    if (gClientConfigs[i].payload[0] == '\0') continue;
    if (sendConfigViaNotecard(gClientConfigs[i].uid, gClientConfigs[i].payload)) {
      // BugFix v1.6.2 (I-6/M-10): Keep pending until client ACK (same as single retry)
      gClientConfigs[i].pendingDispatch = true;
      gClientConfigs[i].dispatchAttempts = 1;
      dispatched++;
    } else {
      gClientConfigs[i].pendingDispatch = true;
      stillPending++;
    }
  }

  if (stillPending == 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "All %d config(s) dispatched successfully", dispatched);
    respondStatus(client, 200, msg);
  } else {
    char msg[80];
    snprintf(msg, sizeof(msg), "%d dispatched, %d still failed — check serial monitor", dispatched, stillPending);
    respondStatus(client, 202, msg);
  }
}

// POST /api/config/cancel  { "pin": "1234", "client": "uid" } → cancel one client
// POST /api/config/cancel  { "pin": "1234" }                  → cancel all pending
static void handleConfigCancelPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) return;

  const char *clientUid = doc["client"].as<const char *>();
  if (clientUid && clientUid[0] != '\0') {
    // Cancel specific client
    ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
    if (!snap) {
      respondStatus(client, 404, F("No cached config for this client"));
      return;
    }
    snap->pendingDispatch = false;
    snap->dispatchAttempts = 0;
    saveClientConfigSnapshots();
    // Purge any pending config notes from Notecard outbox
    uint8_t purged = purgePendingConfigNotes(clientUid);
    char msg[96];
    snprintf(msg, sizeof(msg), "Config dispatch cancelled for %s (%u note(s) purged from outbox)", clientUid, purged);
    Serial.println(msg);
    logTransmission(clientUid, "", "config", "cancelled", "Pending config dispatch cancelled by user");
    respondStatus(client, 200, msg);
    return;
  }

  // Cancel all pending
  uint8_t cancelled = 0;
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    if (gClientConfigs[i].pendingDispatch) {
      gClientConfigs[i].pendingDispatch = false;
      gClientConfigs[i].dispatchAttempts = 0;
      purgePendingConfigNotes(gClientConfigs[i].uid);
      logTransmission(gClientConfigs[i].uid, "", "config", "cancelled", "Pending config dispatch cancelled by user");
      cancelled++;
    }
  }
  saveClientConfigSnapshots();

  char msg[64];
  snprintf(msg, sizeof(msg), "Cancelled %u pending config dispatch(es)", cancelled);
  Serial.println(msg);
  respondStatus(client, 200, msg);
}

// ============================================================================
// POST /api/sync-request — Request Sync-on-Demand (Phase 3)
// ============================================================================
// Sends a sync command to a specific client via command.qo → sync_request.qi
// to force the client's Notecard to perform an immediate hub.sync.
// This helps push pending inbound notes (config, relay) through weak cellular links.
static void handleSyncRequestPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) return;

  const char *clientUid = doc["client"].as<const char *>();
  if (!clientUid || clientUid[0] == '\0') {
    respondStatus(client, 400, F("Missing 'client' UID"));
    return;
  }

  // Verify the product UID is set (required for routing)
  if (gConfig.productUid[0] == '\0') {
    respondStatus(client, 500, F("Product UID not configured — cannot send sync request"));
    return;
  }

  // Send sync command via command.qo with _type = "sync_request"
  J *req = notecard.newRequest("note.add");
  if (!req) {
    respondStatus(client, 500, F("Failed to create Notecard request"));
    return;
  }

  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *cmdBody = JCreateObject();
  if (!cmdBody) {
    JDelete(req);
    respondStatus(client, 500, F("Failed to create command body"));
    return;
  }
  JAddStringToObject(cmdBody, "_target", clientUid);
  JAddStringToObject(cmdBody, "_type", "sync_request");
  JAddStringToObject(cmdBody, "request", "sync");
  stampSchemaVersion(cmdBody);
  JAddItemToObject(req, "body", cmdBody);

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    respondStatus(client, 500, F("No response from Notecard"));
    return;
  }

  const char *err = JGetString(rsp, "err");
  if (err && err[0] != '\0') {
    notecard.deleteResponse(rsp);
    respondStatus(client, 500, F("Notecard error sending sync request"));
    return;
  }

  notecard.deleteResponse(rsp);

  Serial.print(F("Sync request sent to client "));
  Serial.println(clientUid);
  logTransmission(clientUid, "", "sync_request", "outbox", "Sync-on-demand request sent");

  // Log signal strength for context
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (meta && meta->signalBars >= 0) {
    char detail[48];
    snprintf(detail, sizeof(detail), "Client signal: %d/4 bars", meta->signalBars);
    Serial.println(detail);
  }

  respondStatus(client, 200, F("Sync request sent to client"));
}

static void sendPinResponse(EthernetClient &client, const __FlashStringHelper *message) {
  JsonDocument resp;
  resp["pinConfigured"] = isValidPin(gConfig.configPin);
  String msg(message);
  resp["message"] = msg;
  String json;
  serializeJson(resp, json);
  respondJson(client, json);
}

static void handlePinPost(EthernetClient &client, const String &body) {
  // Body is capped at 256 bytes upstream; a small static buffer is sufficient here
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *currentPin = doc["pin"].as<const char *>();
  const char *newPin = doc["newPin"].as<const char *>();
  // A PIN is only considered "configured" if it's a valid 4-digit PIN
  // This prevents garbage data from blocking first-time PIN setup
  bool configured = isValidPin(gConfig.configPin);

  if (!configured) {
    if (!isValidPin(newPin)) {
      respondStatus(client, 400, F("PIN must be 4 digits"));
      return;
    }
    strlcpy(gConfig.configPin, newPin, sizeof(gConfig.configPin));
    gConfigDirty = true;
    sendPinResponse(client, F("PIN set"));
    return;
  }

  if (!requireExplicitPinMatch(client, currentPin)) {
    return;
  }

  if (newPin) {
    if (!isValidPin(newPin)) {
      respondStatus(client, 400, F("PIN must be 4 digits"));
      return;
    }
    strlcpy(gConfig.configPin, newPin, sizeof(gConfig.configPin));
    gConfigDirty = true;
    sendPinResponse(client, F("PIN updated"));
  } else {
    sendPinResponse(client, F("PIN verified"));
  }
}

static void handleRelayPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  const char *clientUid = doc["clientUid"].as<const char *>();
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, F("Missing clientUid"));
    return;
  }

  uint8_t relayNum = doc["relay"].as<uint8_t>();
  if (relayNum < 1 || relayNum > MAX_RELAYS) {
    char errMsg[32];
    snprintf(errMsg, sizeof(errMsg), "relay must be 1-%d", MAX_RELAYS);
    respondStatus(client, 400, errMsg);
    return;
  }

  bool state = doc["state"].as<bool>();

  if (sendRelayCommand(clientUid, relayNum, state, "server")) {
    respondStatus(client, 200, F("OK"));
  } else {
    respondStatus(client, 500, F("Failed to send relay command"));
  }
}

static bool sendRelayCommand(const char *clientUid, uint8_t relayNum, bool state, const char *source) {
  if (!clientUid || relayNum < 1 || relayNum > MAX_RELAYS) {
    return false;
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    return false;
  }

  // Use command.qo — Notehub Route reads _target and _type to deliver
  // as relay.qi on the target client device
  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    return false;
  }

  // Route Relay metadata
  JAddStringToObject(body, "_target", clientUid);
  JAddStringToObject(body, "_type", "relay");
  JAddNumberToObject(body, "relay", relayNum);
  JAddBoolToObject(body, "state", state);
  JAddStringToObject(body, "source", source);
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);
  
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println(F("ERROR: No response from Notecard for relay command"));
    return false;
  }
  const char *err = JGetString(rsp, "err");
  if (err && err[0] != '\0') {
    Serial.print(F("ERROR: Relay command rejected: "));
    Serial.println(err);
    notecard.deleteResponse(rsp);
    return false;
  }
  notecard.deleteResponse(rsp);

  Serial.print(F("Queued relay command for client "));
  Serial.print(clientUid);
  Serial.print(F(": Relay "));
  Serial.print(relayNum);
  Serial.print(F(" -> "));
  Serial.println(state ? "ON" : "OFF");

  // Log to transmission log
  char detail[64];
  snprintf(detail, sizeof(detail), "Relay %u %s (%s)", relayNum, state ? "ON" : "OFF", source ? source : "");
  logTransmission(clientUid, "", "relay", "outbox", detail);

  return true;
}

// Send a command to clear/reset relay alarms for a specific sensor on a client
static bool sendRelayClearCommand(const char *clientUid, uint8_t sensorIdx) {
  if (!clientUid) {
    return false;
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    return false;
  }

  // Use command.qo — Notehub Route reads _target and _type to deliver
  // as relay.qi on the target client device
  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JCreateObject();
  if (!body) {
    return false;
  }

  // Route Relay metadata
  JAddStringToObject(body, "_target", clientUid);
  JAddStringToObject(body, "_type", "relay");
  // Use relay_reset_sensor command format that the client already supports
  JAddNumberToObject(body, "relay_reset_sensor", sensorIdx);
  JAddStringToObject(body, "source", "server-dashboard");
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);
  
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println(F("ERROR: No response from Notecard for relay clear command"));
    return false;
  }
  const char *clearErr = JGetString(rsp, "err");
  if (clearErr && clearErr[0] != '\0') {
    Serial.print(F("ERROR: Relay clear command rejected: "));
    Serial.println(clearErr);
    notecard.deleteResponse(rsp);
    return false;
  }
  notecard.deleteResponse(rsp);

  Serial.print(F("Queued relay clear command for client "));
  Serial.print(clientUid);
  Serial.print(F(", sensor "));
  Serial.println(sensorIdx);

  // Log to transmission log
  char detail[64];
  snprintf(detail, sizeof(detail), "Reset relay alarms sensor %u", sensorIdx);
  logTransmission(clientUid, "", "relay_clear", "outbox", detail);

  return true;
}

static void handleRelayClearPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (isValidPin(gConfig.configPin)) {
    if (!requireValidPin(client, pinValue)) {
      return;
    }
  }

  const char *clientUid = doc["clientUid"].as<const char *>();
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, F("Missing clientUid"));
    return;
  }

  uint8_t sensorIdx = doc["sensorIdx"].as<uint8_t>();
  // Note: sensorIdx validation is handled by the client device based on its actual sensor configuration

  if (sendRelayClearCommand(clientUid, sensorIdx)) {
    respondStatus(client, 200, F("OK"));
  } else {
    respondStatus(client, 500, F("Failed to send relay clear command"));
  }
}

static void handlePausePost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (isValidPin(gConfig.configPin)) {
    if (!requireValidPin(client, pinValue)) {
      return;
    }
  }

  bool paused = doc["paused"].is<bool>() ? doc["paused"].as<bool>() : true;
  gPaused = paused;

  JsonDocument resp;
  resp["paused"] = gPaused;
  String json;
  serializeJson(resp, json);
  respondJson(client, json, 200);

  Serial.print(F("Server pause state: "));
  Serial.println(gPaused ? F("paused") : F("running"));
}

static void handleFtpBackupPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  Serial.println(F("/api/ftp-backup: start"));
  unsigned long t0 = millis();
  gFtpBackupCriticalPathActive = true;
  setFtpBackupStage("start");
  ftpsTraceReset();

  // Use detailed result for comprehensive error reporting
  setFtpBackupStage("backup-running");
  gBackupInProgress = true;
  gWebServer.end();  // Free LISTEN PCB so FTPS data sockets have headroom (Opta LWIP pool=4)
  FtpResult result = performFtpBackupDetailed();
  gWebServer.begin();
  gBackupInProgress = false;
  gFtpBackupCriticalPathActive = false;
  setFtpBackupStage("backup-done");

  Serial.print(F("/api/ftp-backup: done success="));
  Serial.print(result.success ? 1 : 0);
  Serial.print(F(" proc="));
  Serial.print(result.filesProcessed);
  Serial.print(F(" failed="));
  Serial.print(result.filesFailed);
  Serial.print(F(" retries="));
  Serial.print(result.retryAttemptsTotal);
  Serial.print(F(" recovered="));
  Serial.print(result.filesRecoveredByRetry);
  Serial.print(F(" exhausted="));
  Serial.print(result.filesRetryExhausted);
  Serial.print(F(" elapsed_ms="));
  Serial.print(millis() - t0);
  Serial.print(F(" err='"));
  Serial.print(result.errorMessage);
  Serial.println(F("'"));

  JsonDocument resp;
  resp["ok"] = result.success;
  resp["filesUploaded"] = result.filesProcessed;
  resp["filesFailed"] = result.filesFailed;
  resp["retryAttempts"] = result.retryAttemptsTotal;
  resp["filesRecoveredByRetry"] = result.filesRecoveredByRetry;
  resp["filesRetryExhausted"] = result.filesRetryExhausted;
  if (result.success) {
    if (result.filesFailed > 0) {
      resp["message"] = result.errorMessage;  // "X files uploaded, Y failed"
      resp["failedFiles"] = result.failedFiles;
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "%d files backed up to FTP", result.filesProcessed);
      resp["message"] = msg;
    }
  } else {
    resp["error"] = (strlen(result.errorMessage) > 0) ? result.errorMessage : "Backup failed";
    if (strlen(result.failedFiles) > 0) {
      resp["failedFiles"] = result.failedFiles;
    }
  }
  resp["trace"] = gFtpsTraceBuf;
  String json;
  serializeJson(resp, json);

  Serial.print(F("/api/ftp-backup: respondJson status="));
  Serial.print(result.success ? 200 : 500);
  Serial.print(F(" len="));
  Serial.print(json.length());
  Serial.print(F(" client.connected="));
  Serial.println(client.connected() ? 1 : 0);

  if (!client.connected()) {
    setFtpBackupStage("client-gone");
    Serial.println(F("/api/ftp-backup: client gone, skipping response write"));
    return;
  }
  setFtpBackupStage("response-writing");
  respondJson(client, json, result.success ? 200 : 500);
  setFtpBackupStage("response-sent");
  clearFtpBackupMarker();
  Serial.println(F("/api/ftp-backup: response sent"));
}

static void handleFtpRestorePost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  // Use detailed result for comprehensive error reporting
  FtpResult result = performFtpRestoreDetailed();

  if (result.success) {
    // Reload in-memory views now that on-disk state changed
    ensureConfigLoaded();
    loadClientConfigSnapshots();
    loadCalibrationData();
    scheduleNextDailyEmail();
    if (gConfig.viewerEnabled) {
      scheduleNextViewerSummary();
    }
  }

  JsonDocument resp;
  resp["ok"] = result.success;
  resp["filesRestored"] = result.filesProcessed;
  resp["filesFailed"] = result.filesFailed;
  if (result.success) {
    if (result.filesFailed > 0) {
      resp["message"] = result.errorMessage;  // "X files restored, Y failed"
      resp["failedFiles"] = result.failedFiles;
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "%d files restored from FTP", result.filesProcessed);
      resp["message"] = msg;
    }
  } else {
    resp["error"] = (strlen(result.errorMessage) > 0) ? result.errorMessage : "Restore failed";
    if (strlen(result.failedFiles) > 0) {
      resp["failedFiles"] = result.failedFiles;
    }
  }
  String json;
  serializeJson(resp, json);
  respondJson(client, json, result.success ? 200 : 500);
}

// ============================================================================
// Notecard Config Dispatch Helpers
// ============================================================================

// Purge any pending config notes for a specific client from the local Notecard outbox.
// Uses note.changes to enumerate pending notes in command.qo, then deletes any
// that have _type=="config" and _target==clientUid.  Returns number purged.
static uint8_t purgePendingConfigNotes(const char *clientUid) {
  if (!clientUid || clientUid[0] == '\0') return 0;

  uint8_t purged = 0;

  // Ask for pending notes in the command outbox
  J *req = notecard.newRequest("note.changes");
  if (!req) return 0;
  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);

  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) return 0;

  // Check for error (e.g. file doesn't exist yet)
  const char *err = JGetString(rsp, "err");
  if (err && err[0] != '\0') {
    notecard.deleteResponse(rsp);
    return 0;
  }

  J *notes = JGetObject(rsp, "notes");
  if (!notes) {
    notecard.deleteResponse(rsp);
    return 0;
  }

  // Collect note IDs to delete (we can't modify the list while iterating)
  // Max 20 stale configs is more than enough for any realistic scenario
  static const uint8_t MAX_PURGE = 20;
  char noteIds[MAX_PURGE][48];
  uint8_t toDelete = 0;

  J *note = notes->child;
  while (note && toDelete < MAX_PURGE) {
    const char *noteId = note->string;  // The note ID is the key name
    J *body = JGetObject(note, "body");
    if (body && noteId) {
      const char *type = JGetString(body, "_type");
      const char *target = JGetString(body, "_target");
      if (type && target && strcmp(type, "config") == 0 && strcmp(target, clientUid) == 0) {
        strlcpy(noteIds[toDelete], noteId, sizeof(noteIds[toDelete]));
        toDelete++;
      }
    }
    note = note->next;
  }
  notecard.deleteResponse(rsp);

  // Now delete each stale config note
  for (uint8_t i = 0; i < toDelete; i++) {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
    // Each note.delete is a blocking I2C round-trip; kick watchdog to
    // prevent starvation when purging many stale config notes.
    #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
    #else
      IWatchdog.reload();
    #endif
#endif
    J *delReq = notecard.newRequest("note.delete");
    if (!delReq) continue;
    JAddStringToObject(delReq, "file", COMMAND_OUTBOX_FILE);
    JAddStringToObject(delReq, "note", noteIds[i]);
    J *delRsp = notecard.requestAndResponse(delReq);
    if (delRsp) {
      const char *delErr = JGetString(delRsp, "err");
      if (!delErr || delErr[0] == '\0') {
        purged++;
        Serial.print(F("PURGED stale config note "));
        Serial.print(noteIds[i]);
        Serial.print(F(" for client "));
        Serial.println(clientUid);
      }
      notecard.deleteResponse(delRsp);
    }
  }

  if (purged > 0) {
    char detail[64];
    snprintf(detail, sizeof(detail), "Purged %u stale config(s) from outbox", purged);
    logTransmission(clientUid, "", "config", "outbox", detail);
  }

  return purged;
}

// Send a cached config JSON payload to a specific client via Notecard note.add.
// Returns true on success, false on failure (with error logged to Serial).
// Automatically purges any stale pending config notes for the same client first.
static bool sendConfigViaNotecard(const char *clientUid, const char *jsonPayload) {
  // Pre-flight: check that Product UID is configured (required for Notehub routing)
  if (gConfig.productUid[0] == '\0') {
    Serial.println(F("ERROR: Cannot dispatch config - Product UID not set in Server Settings"));
    return false;
  }

  // Purge any older pending config notes for this client before adding the new one
  gLastConfigPurgeCount = purgePendingConfigNotes(clientUid);
  if (gLastConfigPurgeCount > 0) {
    Serial.print(F("WARNING: Replaced "));
    Serial.print(gLastConfigPurgeCount);
    Serial.println(F(" older pending config(s) in outbox"));
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    Serial.println(F("ERROR: Failed to create Notecard request (heap exhausted?)"));
    return false;
  }

  // Use command.qo — Notehub Route reads _target and _type to deliver
  // as config.qi on the target client device
  JAddStringToObject(req, "file", COMMAND_OUTBOX_FILE);
  JAddBoolToObject(req, "sync", true);

  J *body = JParse(jsonPayload);
  if (!body) {
    JDelete(req);  // Avoid memory leak
    Serial.println(F("ERROR: Failed to parse config JSON for Notecard send"));
    return false;
  }
  // Inject Route Relay metadata into the config payload
  JAddStringToObject(body, "_target", clientUid);
  JAddStringToObject(body, "_type", "config");
  // Inject config version hash so client can echo it back in ACK
  ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
  if (snap && snap->configVersion[0] != '\0') {
    JAddStringToObject(body, "_cv", snap->configVersion);
  }
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);

  // Use requestAndResponse to capture detailed Notecard error messages
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) {
    Serial.println(F("ERROR: No response from Notecard (I2C communication failure?)"));
    return false;
  }

  const char *err = JGetString(rsp, "err");
  if (err && err[0] != '\0') {
    Serial.print(F("ERROR: Notecard rejected note.add: "));
    Serial.println(err);
    notecard.deleteResponse(rsp);
    return false;
  }

  notecard.deleteResponse(rsp);
  Serial.print(F("Queued config update for client "));
  Serial.println(clientUid);

  // Log to transmission log
  logTransmission(clientUid, "", "config", "outbox", "Config update dispatched");

  return true;
}

static ConfigDispatchStatus dispatchClientConfig(const char *clientUid, JsonVariantConst cfgObj) {
  // Use static buffer to avoid 8KB stack allocation (Mbed OS stack is only 4-8KB)
  static char buffer[8192];
  size_t len = serializeJson(cfgObj, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) {
    Serial.println(F("Client config payload too large"));
    return ConfigDispatchStatus::PayloadTooLarge;
  }
  buffer[len] = '\0';

  // Always cache locally first so config is preserved even if Notecard is down
  if (!cacheClientConfigFromBuffer(clientUid, buffer)) {
    Serial.println(F("Config payload too large for persistent cache"));
    return ConfigDispatchStatus::PayloadTooLarge;
  }
  
  // Prune orphaned sensor records that are no longer in the new config
  pruneOrphanedSensorRecords(clientUid);

  // Clear sensor-stuck alarms for sensors where stuckDetection was disabled
  JsonArrayConst sensors = cfgObj["sensors"].as<JsonArrayConst>();
  if (!sensors.isNull()) {
    for (JsonObjectConst t : sensors) {
      if (t["stuckDetection"].is<bool>() && !t["stuckDetection"].as<bool>()) {
        uint8_t sensorIdx = t["number"] | (uint8_t)0;
        if (sensorIdx == 0) continue;
        SensorRecord *rec = findSensorByHash(clientUid, sensorIdx);
        if (rec && rec->alarmActive && strcmp(rec->alarmType, "sensor-stuck") == 0) {
          rec->alarmActive = false;
          strlcpy(rec->alarmType, "clear", sizeof(rec->alarmType));
          clearAlarmEvent(clientUid, sensorIdx);
          gSensorRegistryDirty = true;
          Serial.print(F("Cleared sensor-stuck alarm for sensor "));
          Serial.print(sensorIdx);
          Serial.print(F(" on "));
          Serial.println(clientUid);
        }
      }
    }
  }

  // Generate config version hash for ACK tracking
  // Simple hash of the payload to create a short version identifier
  ClientConfigSnapshot *snapPre = findClientConfigSnapshot(clientUid);
  if (snapPre) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; ++i) {
      hash = ((hash << 5) + hash) ^ (uint8_t)buffer[i];
    }
    snprintf(snapPre->configVersion, sizeof(snapPre->configVersion), "%08lX", (unsigned long)hash);
    snapPre->pendingDispatch = true;
    snapPre->dispatchAttempts = 1;           // First dispatch attempt
    snapPre->lastDispatchEpoch = currentEpoch();
    saveClientConfigSnapshots();
  }

  // Phase 2: Log signal strength warning for weak-signal clients
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (meta && meta->signalBars >= 0 && meta->signalBars <= 1) {
    Serial.print(F("WARNING: Client "));
    Serial.print(clientUid);
    Serial.print(F(" has weak signal ("));
    Serial.print(meta->signalBars);
    Serial.println(F("/4 bars) — config delivery may be delayed or fail"));
    addServerSerialLog("Config pushed to weak-signal client", "warn", "config");
  }

  // Attempt to send via Notecard
  if (sendConfigViaNotecard(clientUid, buffer)) {
    // Mark as dispatched but still pending ACK
    ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
    if (snap) {
      // pendingDispatch stays true until ACK received
    }
    return (gLastConfigPurgeCount > 0) ? ConfigDispatchStatus::OkWithPurge : ConfigDispatchStatus::Ok;
  }

  // Send failed — mark for auto-retry (pendingDispatch already true)
  return ConfigDispatchStatus::CachedOnly;
}

// Retry sending any cached configs that previously failed Notecard dispatch.
// Called automatically from loop() every 60 seconds and manually via /api/config/retry.
static void dispatchPendingConfigs() {
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    if (!gClientConfigs[i].pendingDispatch) continue;
    if (gClientConfigs[i].payload[0] == '\0') continue;

    // Phase 2: Auto-cancel after max retry attempts to prevent infinite retry loops
    // This protects against low-signal clients that can never receive inbound notes.
    if (gClientConfigs[i].dispatchAttempts >= MAX_CONFIG_DISPATCH_RETRIES) {
      Serial.print(F("AUTO-CANCEL: Config dispatch for "));
      Serial.print(gClientConfigs[i].uid);
      Serial.print(F(" failed after "));
      Serial.print(gClientConfigs[i].dispatchAttempts);
      Serial.println(F(" attempts — check client signal strength"));
      gClientConfigs[i].pendingDispatch = false;
      logTransmission(gClientConfigs[i].uid, gClientConfigs[i].site, "config", "failed",
                       "Auto-cancelled: max retries exceeded (weak signal?)");
      addServerSerialLog("Config auto-cancelled: delivery failed", "warn", "config");
      saveClientConfigSnapshots();
      continue;
    }

#ifdef TANKALARM_WATCHDOG_AVAILABLE
    // sendConfigViaNotecard chains purgePendingConfigNotes + note.add;
    // kick watchdog before each client to prevent starvation.
    #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
    #else
      IWatchdog.reload();
    #endif
#endif
    Serial.print(F("Auto-retrying config dispatch for "));
    Serial.print(gClientConfigs[i].uid);
    Serial.print(F(" (attempt "));
    Serial.print(gClientConfigs[i].dispatchAttempts + 1);
    Serial.print(F("/"));
    Serial.print(MAX_CONFIG_DISPATCH_RETRIES);
    Serial.println(F(")"));

    // Increment attempt counter BEFORE sending so it advances even on failure.
    // This ensures auto-cancel triggers even if the Notecard I2C is down.
    gClientConfigs[i].dispatchAttempts++;
    gClientConfigs[i].lastDispatchEpoch = currentEpoch();

    if (sendConfigViaNotecard(gClientConfigs[i].uid, gClientConfigs[i].payload)) {
      // pendingDispatch stays true until config ACK received from client
      Serial.print(F("SUCCESS: Dispatched pending config for "));
      Serial.println(gClientConfigs[i].uid);
    } else {
      Serial.print(F("WARNING: Config dispatch failed for "));
      Serial.println(gClientConfigs[i].uid);
    }
    saveClientConfigSnapshots();
  }
}

static void pollNotecard() {
  // Server reads inbound .qi notefiles delivered by ClientToServerRelay Route
  processNotefile(TELEMETRY_INBOX_FILE, handleTelemetry);
  processNotefile(ALARM_INBOX_FILE, handleAlarm);
  processNotefile(DAILY_INBOX_FILE, handleDaily);
  processNotefile(UNLOAD_INBOX_FILE, handleUnload);
  processNotefile(SERIAL_LOG_FILE, handleSerialLog);
  processNotefile(SERIAL_ACK_FILE, handleSerialAck);
  processNotefile(RELAY_FORWARD_FILE, handleRelayForward);
  processNotefile(LOCATION_RESPONSE_FILE, handleLocationResponse);
  processNotefile(CONFIG_ACK_INBOX_FILE, handleConfigAck);
}

struct NotefileParseFailureTracker {
  char fileName[32];
  uint8_t failures;
};

static uint8_t *notefileParseFailureCounter(const char *fileName) {
  static NotefileParseFailureTracker trackers[12] = {};
  if (!fileName || fileName[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < (sizeof(trackers) / sizeof(trackers[0])); ++i) {
    if (trackers[i].fileName[0] != '\0' && strcmp(trackers[i].fileName, fileName) == 0) {
      return &trackers[i].failures;
    }
  }
  for (size_t i = 0; i < (sizeof(trackers) / sizeof(trackers[0])); ++i) {
    if (trackers[i].fileName[0] == '\0') {
      strlcpy(trackers[i].fileName, fileName, sizeof(trackers[i].fileName));
      trackers[i].failures = 0;
      return &trackers[i].failures;
    }
  }
  return nullptr;
}

static void processNotefile(const char *fileName, void (*handler)(JsonDocument &, double)) {
  uint8_t processed = 0;
  while (processed < MAX_NOTES_PER_FILE_PER_POLL) {
    J *req = notecard.newRequest("note.get");
    if (!req) {
      gNotecardFailureCount++;
      if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD && gNotecardAvailable) {
        gNotecardAvailable = false;
        Serial.println(F("Notecard unavailable - I2C health check will attempt recovery"));
      }
      return;
    }
    JAddStringToObject(req, "file", fileName);
    // Peek without deleting — delete after successful processing for crash safety
    J *rsp = notecard.requestAndResponse(req);
    if (!rsp) {
      gNotecardFailureCount++;
      if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD && gNotecardAvailable) {
        gNotecardAvailable = false;
        Serial.println(F("Notecard unavailable - I2C health check will attempt recovery"));
      }
      return;
    }

    // Notecard responded — reset failure tracking
    if (!gNotecardAvailable) {
      gNotecardAvailable = true;
      gNotecardFailureCount = 0;
      Serial.println(F("Notecard recovered"));
    }
    gNotecardFailureCount = 0;
    gLastSuccessfulNotecardComm = millis();

    J *body = JGetObject(rsp, "body");
    if (!body) {
      notecard.deleteResponse(rsp);
      break;
    }

    char *json = JConvertToJSONString(body);
    double epoch = JGetNumber(rsp, "time");
    bool processedOk = false;
    if (json) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, json);
      NoteFree(json);
      if (!err) {
        handler(doc, epoch);
        processedOk = true;
      } else {
        Serial.print(F("Skipping malformed note in "));
        Serial.print(fileName);
        Serial.print(F(": "));
        Serial.println(err.f_str());
      }
    }

    notecard.deleteResponse(rsp);

    uint8_t *failureCounter = notefileParseFailureCounter(fileName);
    if (processedOk) {
      if (failureCounter) {
        *failureCounter = 0;
      }
      J *delReq = notecard.newRequest("note.get");
      if (delReq) {
        JAddStringToObject(delReq, "file", fileName);
        JAddBoolToObject(delReq, "delete", true);
        J *delRsp = notecard.requestAndResponse(delReq);
        if (delRsp) notecard.deleteResponse(delRsp);
      }
    } else {
      if (failureCounter) {
        (*failureCounter)++;
        if (*failureCounter >= 3) {
          Serial.print(F("Deleting poison note from "));
          Serial.println(fileName);
          J *delReq = notecard.newRequest("note.get");
          if (delReq) {
            JAddStringToObject(delReq, "file", fileName);
            JAddBoolToObject(delReq, "delete", true);
            J *delRsp = notecard.requestAndResponse(delReq);
            if (delRsp) notecard.deleteResponse(delRsp);
          }
          *failureCounter = 0;
        }
      }
      break;
    }

    processed++;

    // Yield to keep the loop responsive and allow watchdog kicks elsewhere
    safeSleep(1);
  }
}

static void handleTelemetry(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["c"] | "";
  uint8_t sensorIndex = doc["k"].as<uint8_t>();
  SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex);
  if (!rec) {
    return;
  }
  
  // Store optional user-assigned display number (0 = unset)
  if (doc.containsKey("un")) {
    rec->userNumber = doc["un"].as<uint8_t>();
  }
  
  // Extract client firmware version if provided
  const char *fwVer = doc["fv"] | "";
  if (fwVer && strlen(fwVer) > 0) {
    ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
    if (meta && strcmp(meta->firmwareVersion, fwVer) != 0) {
      strlcpy(meta->firmwareVersion, fwVer, sizeof(meta->firmwareVersion));
      gClientMetadataDirty = true;
    }
  }

  strlcpy(rec->site, doc["s"] | "", sizeof(rec->site));
  
  // Only update label if provided in message (optional field)
  const char *label = doc["n"] | "";
  if (label && strlen(label) > 0) {
    strlcpy(rec->label, label, sizeof(rec->label));
  } else if (rec->label[0] == '\0') {
    strlcpy(rec->label, "Tank", sizeof(rec->label)); // Default only if empty
  }
  
  // Store contents if provided - optional field (not used for RPM/engine monitors)
  const char *contents = doc["cn"] | "";
  if (contents && strlen(contents) > 0) {
    strlcpy(rec->contents, contents, sizeof(rec->contents));
  }
  
  // Store object type if provided (tank, engine, pump, gas, flow)
  const char *objectType = doc["ot"] | "";
  if (objectType && strlen(objectType) > 0) {
    strlcpy(rec->objectType, objectType, sizeof(rec->objectType));
  } else if (rec->objectType[0] == '\0') {
    // Fallback: look up from cached client config
    ClientConfigSnapshot *cfgSnap = findClientConfigSnapshot(clientUid);
    if (cfgSnap && cfgSnap->payload[0] != '\0') {
      JsonDocument cfgDoc;
      if (deserializeJson(cfgDoc, cfgSnap->payload) == DeserializationError::Ok) {
        JsonArray cfgSensors = cfgDoc["sensors"].as<JsonArray>();
        if (cfgSensors) {
          for (JsonVariant ct : cfgSensors) {
            uint8_t ctn = ct["number"] | 0;
            if (ctn == sensorIndex) {
              const char *cfgOt = ct["monitorType"] | "";
              if (cfgOt && strlen(cfgOt) > 0) {
                strlcpy(rec->objectType, cfgOt, sizeof(rec->objectType));
              }
              const char *cfgMu = ct["sensorRangeUnit"] | "";
              const char *cfgMt = ct["monitorType"] | "";
              // Derive measurement unit from config context
              // Also override stale "inches" default for non-tank monitor types
              if (rec->measurementUnit[0] == '\0' ||
                  (strcmp(rec->measurementUnit, "inches") == 0 && strcmp(cfgMt, "tank") != 0)) {
                if (strcmp(cfgMt, "gas") == 0 && cfgMu[0] != '\0') {
                  strlcpy(rec->measurementUnit, cfgMu, sizeof(rec->measurementUnit));
                } else if (strcmp(cfgMt, "gas") == 0) {
                  strlcpy(rec->measurementUnit, "psi", sizeof(rec->measurementUnit));
                } else if (strcmp(cfgMt, "rpm") == 0) {
                  strlcpy(rec->measurementUnit, "rpm", sizeof(rec->measurementUnit));
                } else if (strcmp(cfgMt, "flow") == 0) {
                  strlcpy(rec->measurementUnit, "gpm", sizeof(rec->measurementUnit));
                }
              }
              break;
            }
          }
        }
      }
    }
    if (rec->objectType[0] == '\0') {
      strlcpy(rec->objectType, "tank", sizeof(rec->objectType)); // Final default
    }
  }
  
  // Store sensor interface type if provided (digital, analog, currentLoop, pulse)
  const char *sensorType = doc["st"] | "";
  if (sensorType && strlen(sensorType) > 0) {
    // Normalize "rpm" to "pulse" for consistency
    if (strcmp(sensorType, "rpm") == 0) {
      strlcpy(rec->sensorType, "pulse", sizeof(rec->sensorType));
    } else {
      strlcpy(rec->sensorType, sensorType, sizeof(rec->sensorType));
    }
  }
  
  // Store measurement unit if provided
  const char *unit = doc["mu"] | "";
  if (unit && strlen(unit) > 0 && isValidMeasurementUnit(unit)) {
    strlcpy(rec->measurementUnit, unit, sizeof(rec->measurementUnit));
  }
  
  // Handle raw sensor readings
  float mA = 0.0f;
  float voltage = 0.0f;
  
  // Raw mA for current-loop sensors
  if (doc["ma"]) {
    mA = doc["ma"].as<float>();
    rec->sensorMa = (mA >= 4.0f) ? mA : 0.0f;
  } else if (doc["sensorMa"]) {
    mA = doc["sensorMa"].as<float>();
    rec->sensorMa = (mA >= 4.0f) ? mA : 0.0f;
  }
  
  // Raw voltage for analog sensors
  if (doc["vt"]) {
    voltage = doc["vt"].as<float>();
    rec->sensorVoltage = (voltage > 0.0f) ? voltage : 0.0f;
  }
  
  // Get level/value reading based on sensor type
  float newLevel = 0.0f;
  bool isCurrentLoop = (strcmp(rec->sensorType, "currentLoop") == 0);
  bool isAnalog = (strcmp(rec->sensorType, "analog") == 0);
  bool isDigital = (strcmp(rec->sensorType, "digital") == 0);
  bool isPulse = (strcmp(rec->sensorType, "pulse") == 0);
  
  if (isCurrentLoop && mA >= 4.0f) {
    // Current-loop sensor: convert raw mA to level using config
    // Use temperature compensation if available
    float currentTemp = getCachedTemperature(clientUid);
    newLevel = convertMaToLevelWithTemp(clientUid, sensorIndex, mA, currentTemp);
  } else if (isAnalog && voltage > 0.0f) {
    // Analog voltage sensor: convert raw voltage to level using config
    newLevel = convertVoltageToLevel(clientUid, sensorIndex, voltage);
  } else if (isDigital && doc["fl"]) {
    // Digital float switch: use fl field
    newLevel = doc["fl"].as<float>();
  } else if (isPulse && doc["rm"]) {
    // Pulse/RPM sensor: use rm field
    newLevel = doc["rm"].as<float>();
  }
  
  double now = (epoch > 0.0) ? epoch : currentEpoch();
  
  // Update 24-hour tracking: if current level is >22 hours old, roll it to previous
  const double HOURS_22 = 22.0 * 3600.0;  // 22 hours in seconds
  if (rec->lastUpdateEpoch > 0.0 && (now - rec->lastUpdateEpoch) >= HOURS_22) {
    rec->previousLevelInches = rec->levelInches;
    rec->previousLevelEpoch = rec->lastUpdateEpoch;
  } else if (rec->previousLevelEpoch == 0.0 && rec->lastUpdateEpoch > 0.0) {
    // Initialize previous level on first update
    rec->previousLevelInches = rec->levelInches;
    rec->previousLevelEpoch = rec->lastUpdateEpoch;
  }
  
  rec->levelInches = newLevel;
  rec->lastUpdateEpoch = now;
  gSensorRegistryDirty = true;
  
  // Record telemetry snapshot for historical charting
  // Get site name and tank height for history record
  const char *siteName = doc["s"] | "";
  float recordHeight = doc["h"].as<float>();
  if (recordHeight <= 0) recordHeight = 48.0f; // Default tank height
  
  // Get voltage from client metadata if available
  float vinVoltage = 0.0f;
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (meta && meta->vinVoltage > 0) {
    vinVoltage = meta->vinVoltage;
  }
  
  recordTelemetrySnapshot(clientUid, siteName, sensorIndex, recordHeight, newLevel, vinVoltage);
}

static void handleAlarm(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["c"] | "";
  const char *type = doc["y"] | "";

  // System alarms (solar/battery/power) don't reference a specific sensor.
  // Store on ClientMetadata instead of creating a phantom sensorIndex=0 SensorRecord.
  bool isSystemAlarm = (strcmp(type, "solar") == 0) ||
                       (strcmp(type, "battery") == 0) ||
                       (strcmp(type, "power") == 0);
  if (isSystemAlarm) {
    ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
    if (meta) {
      strlcpy(meta->lastSystemAlarmType, type, sizeof(meta->lastSystemAlarmType));
      meta->lastSystemAlarmEpoch = (epoch > 0.0) ? epoch : currentEpoch();
      gClientMetadataDirty = true;
    }
    Serial.print(F("System alarm from "));
    Serial.print(clientUid);
    Serial.print(F(": "));
    Serial.println(type);
    addServerSerialLog("System alarm received", "warn", "alarm");
    // SMS for system alarms if client flagged as critical (se=true)
    // BugFix v1.6.2 (I-14): Rate-limit system alarm SMS — they previously bypassed
    // the per-sensor rate limiter, allowing SMS floods from rapid power transitions.
    static double sLastSystemSmsSentEpoch = 0.0;
    bool smsRequested = doc["se"] | false;
    if (smsRequested) {
      double smsNow = currentEpoch();
      if (smsNow - sLastSystemSmsSentEpoch >= MIN_SMS_ALERT_INTERVAL_SECONDS) {
        const char *siteName = doc["s"] | "";
        char message[160];
        if (strcmp(type, "solar") == 0) {
          const char *alert = doc["alert"] | "unknown";
          float bv = doc["bv"] | 0.0f;
          snprintf(message, sizeof(message), "%s Solar: %s (%.1fV)", siteName, alert, bv);
        } else if (strcmp(type, "battery") == 0) {
          const char *alert = doc["alert"] | "unknown";
          float v = doc["v"] | 0.0f;
          snprintf(message, sizeof(message), "%s Battery: %s (%.1fV)", siteName, alert, v);
        } else if (strcmp(type, "power") == 0) {
          const char *from = doc["from"] | "?";
          const char *to = doc["to"] | "?";
          float v = doc["v"] | 0.0f;
          snprintf(message, sizeof(message), "%s Power: %s->%s (%.1fV)", siteName, from, to, v);
        }
        sendSmsAlert(message);
        sLastSystemSmsSentEpoch = smsNow;
      } else {
        Serial.println(F("System alarm SMS suppressed by rate limit"));
      }
    }
    return;
  }

  uint8_t sensorIndex = doc["k"].as<uint8_t>();
  SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex);
  if (!rec) {
    return;
  }
  
  // Store optional user-assigned display number (0 = unset)
  if (doc.containsKey("un")) {
    rec->userNumber = doc["un"].as<uint8_t>();
  }
  
  // Extract client firmware version if provided
  const char *fwVer = doc["fv"] | "";
  if (fwVer && strlen(fwVer) > 0) {
    ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
    if (meta && strcmp(meta->firmwareVersion, fwVer) != 0) {
      strlcpy(meta->firmwareVersion, fwVer, sizeof(meta->firmwareVersion));
      gClientMetadataDirty = true;
    }
  }

  // Store object type if provided
  const char *objectType = doc["ot"] | "";
  if (objectType && strlen(objectType) > 0) {
    strlcpy(rec->objectType, objectType, sizeof(rec->objectType));
  }
  
  // Store measurement unit if provided
  const char *unit = doc["mu"] | "";
  if (unit && strlen(unit) > 0 && isValidMeasurementUnit(unit)) {
    strlcpy(rec->measurementUnit, unit, sizeof(rec->measurementUnit));
  }
  
  // Handle raw sensor readings
  float mA = 0.0f;
  float voltage = 0.0f;
  
  if (doc["ma"]) {
    mA = doc["ma"].as<float>();
    rec->sensorMa = (mA >= 4.0f) ? mA : 0.0f;
  }
  if (doc["vt"]) {
    voltage = doc["vt"].as<float>();
    rec->sensorVoltage = (voltage > 0.0f) ? voltage : 0.0f;
  }
  
  // Get level - convert from raw sensor data if no level provided
  float level = 0.0f;
  bool isCurrentLoop = (strcmp(rec->sensorType, "currentLoop") == 0);
  bool isAnalog = (strcmp(rec->sensorType, "analog") == 0);
  bool isDigital = (strcmp(rec->sensorType, "digital") == 0);
  bool isPulse = (strcmp(rec->sensorType, "pulse") == 0);
  
  if (isCurrentLoop && mA >= 4.0f) {
    float currentTemp = getCachedTemperature(clientUid);
    level = convertMaToLevelWithTemp(clientUid, sensorIndex, mA, currentTemp);
  } else if (isAnalog && voltage > 0.0f) {
    level = convertVoltageToLevel(clientUid, sensorIndex, voltage);
  } else if (isDigital && doc["fl"]) {
    level = doc["fl"].as<float>();
  } else if (isPulse && doc["rm"]) {
    level = doc["rm"].as<float>();
  }
  
  bool isDiagnostic = (strcmp(type, "sensor-fault") == 0) ||
                      (strcmp(type, "sensor-stuck") == 0) ||
                      (strcmp(type, "sensor-recovered") == 0);
  bool isRecovery = (strcmp(type, "sensor-recovered") == 0);
  // Digital sensor (float switch) alarm types
  bool isDigitalAlarm = (strcmp(type, "triggered") == 0) ||
                        (strcmp(type, "not_triggered") == 0);
  // Relay safety timeout — relay was forced off after exceeding max ON duration
  // This is an operational event, NOT a sensor alarm clear — do not clear alarmActive
  bool isRelayTimeout = (strcmp(type, "relay_timeout") == 0);

  if (strcmp(type, "clear") == 0 || isRecovery) {
    rec->alarmActive = false;
    strlcpy(rec->alarmType, "clear", sizeof(rec->alarmType));
    if (isRecovery) {
      strlcpy(rec->alarmType, type, sizeof(rec->alarmType));
    }
    // Log alarm clear event
    clearAlarmEvent(clientUid, sensorIndex);
  } else if (isRelayTimeout) {
    // Record the timeout event but do NOT clear the underlying alarm state
    // The sensor alarm condition may still be active
    strlcpy(rec->alarmType, "relay_timeout", sizeof(rec->alarmType));
    Serial.print(F("Relay safety timeout for "));
    Serial.print(clientUid);
    Serial.print(F(" sensor "));
    Serial.println(sensorIndex);
  } else {
    rec->alarmActive = true;
    strlcpy(rec->alarmType, type, sizeof(rec->alarmType));
    const char *siteName = doc["s"] | "";
    bool isHigh = (strcmp(type, "high") == 0 || strcmp(type, "triggered") == 0);
    logAlarmEvent(clientUid, siteName, sensorIndex, level, isHigh);
  }
  rec->levelInches = level;
  // Record historical snapshot from alarm so trend data captures alarm events
  if (level > 0.0f) {
    const char *alarmSiteName = doc["s"] | rec->site;
    float alarmVin = 0.0f;
    ClientMetadata *alarmMeta = findOrCreateClientMetadata(clientUid);
    if (alarmMeta) alarmVin = alarmMeta->vinVoltage;
    recordTelemetrySnapshot(clientUid, alarmSiteName, sensorIndex,
                            rec->levelInches, level, alarmVin);
  }
  rec->lastUpdateEpoch = (epoch > 0.0) ? epoch : currentEpoch();
  gSensorRegistryDirty = true;

  // Check rate limit before sending SMS
  bool clientWantsSms = false;
  if (doc["se"]) {
    clientWantsSms = doc["se"].as<bool>();
  } else if (doc["smsEnabled"]) {
    clientWantsSms = doc["smsEnabled"].as<bool>();
  }
  // For sensor-level alarms (high/low/clear/digital), SMS is controlled by server policy
  bool smsAllowedByServer = false;
  if (strcmp(type, "high") == 0) {
    smsAllowedByServer = gConfig.smsOnHigh;
  } else if (strcmp(type, "low") == 0) {
    smsAllowedByServer = gConfig.smsOnLow;
  } else if (strcmp(type, "clear") == 0) {
    smsAllowedByServer = gConfig.smsOnClear;
  } else if (isDigitalAlarm) {
    smsAllowedByServer = gConfig.smsOnHigh;
  } else if (isRelayTimeout) {
    smsAllowedByServer = gConfig.smsOnClear;  // Relay timeout uses clear SMS policy
  } else if (isRecovery) {
    smsAllowedByServer = gConfig.smsOnClear;
  }

  bool bypassMinimumInterval = (strcmp(type, "clear") == 0) || isRecovery;
  bool suppressSmsForDiagnostic = isDiagnostic && !isRecovery;
  if (!suppressSmsForDiagnostic && clientWantsSms && smsAllowedByServer && checkSmsRateLimit(rec, bypassMinimumInterval)) {
    // BugFix v1.6.2 (M-16): Pre-truncate site name to leave room for alarm details.
    // Long site names can consume the entire 160-char SMS budget.
    char shortSite[24];
    strlcpy(shortSite, rec->site, sizeof(shortSite));
    char message[160];
    if (isRelayTimeout) {
      snprintf(message, sizeof(message), "%s%s%d Relay safety timeout - relay forced OFF", shortSite, rec->userNumber > 0 ? " #" : " sensor ", rec->userNumber > 0 ? rec->userNumber : rec->sensorIndex);
    } else if (isDigitalAlarm) {
      const char *stateDesc = (strcmp(type, "triggered") == 0) ? "ACTIVATED" : "NOT ACTIVATED";
      snprintf(message, sizeof(message), "%s%s%d Float Switch %s", shortSite, rec->userNumber > 0 ? " #" : " sensor ", rec->userNumber > 0 ? rec->userNumber : rec->sensorIndex, stateDesc);
    } else {
      snprintf(message, sizeof(message), "%s%s%d %s alarm %.1f %s", shortSite, rec->userNumber > 0 ? " #" : " sensor ", rec->userNumber > 0 ? rec->userNumber : rec->sensorIndex, rec->alarmType, level, rec->measurementUnit[0] ? rec->measurementUnit : "in");
    }
    sendSmsAlert(message);
  }
}

// Helper function to find client metadata entry (read-only, does not create)
static ClientMetadata *findClientMetadata(const char *clientUid) {
  if (!clientUid || strlen(clientUid) == 0) {
    return nullptr;
  }
  
  for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
    if (strcmp(gClientMetadata[i].clientUid, clientUid) == 0) {
      return &gClientMetadata[i];
    }
  }
  
  return nullptr;
}

// Helper function to find or create client metadata entry
static ClientMetadata *findOrCreateClientMetadata(const char *clientUid) {
  if (!clientUid || strlen(clientUid) == 0) {
    return nullptr;
  }
  
  // Search for existing entry
  ClientMetadata *existing = findClientMetadata(clientUid);
  if (existing) {
    return existing;
  }
  
  // Create new entry if space available
  if (gClientMetadataCount < MAX_CLIENT_METADATA) {
    ClientMetadata *meta = &gClientMetadata[gClientMetadataCount++];
    memset(meta, 0, sizeof(ClientMetadata));
    strlcpy(meta->clientUid, clientUid, sizeof(meta->clientUid));
    meta->cachedTemperatureF = TEMPERATURE_UNAVAILABLE;  // Initialize to sentinel value
    meta->signalBars = -1;  // Unknown until daily report with signal data arrives
    gClientMetadataDirty = true;
    return meta;
  }
  
  // Maximum capacity reached - evict stalest entry (oldest vinVoltageEpoch)
  uint8_t evictIdx = 0;
  double oldestEpoch = 1e18;
  for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
    double epoch = gClientMetadata[i].vinVoltageEpoch;
    if (epoch <= 0.0) epoch = 0.0;  // Prioritize removing entries with no voltage data
    if (epoch < oldestEpoch) {
      oldestEpoch = epoch;
      evictIdx = i;
    }
  }
  
  Serial.print(F("WARNING: Client metadata full ("));
  Serial.print(MAX_CLIENT_METADATA);
  Serial.print(F(") - evicting stale entry: "));
  Serial.println(gClientMetadata[evictIdx].clientUid);
  addServerSerialLog("Client metadata evicted (capacity full)", "warn", "registry");
  
  // Reuse evicted slot
  ClientMetadata *meta = &gClientMetadata[evictIdx];
  memset(meta, 0, sizeof(ClientMetadata));
  strlcpy(meta->clientUid, clientUid, sizeof(meta->clientUid));
  meta->cachedTemperatureF = TEMPERATURE_UNAVAILABLE;
  gClientMetadataDirty = true;
  return meta;
}

static void handleDaily(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["c"] | "";
  if (!clientUid || strlen(clientUid) == 0) {
    return;
  }
  
  // Extract client firmware version if provided
  const char *fwVer = doc["fv"] | "";
  if (fwVer && strlen(fwVer) > 0) {
    ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
    if (meta && strcmp(meta->firmwareVersion, fwVer) != 0) {
      strlcpy(meta->firmwareVersion, fwVer, sizeof(meta->firmwareVersion));
      gClientMetadataDirty = true;
    }
  }

  // Extract VIN voltage from daily report if present
  // Check if this is part 0 (new) or part 1 (legacy) of the daily report
  uint8_t part = doc["p"].as<uint8_t>();
  bool isFirstPart = (part == 0 || part == 1);
  float vinVoltage = doc["v"].as<float>();
  if (isFirstPart && vinVoltage > 0.0f) {
    ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
    if (meta) {
      meta->vinVoltage = vinVoltage;
      meta->vinVoltageEpoch = (epoch > 0.0) ? epoch : currentEpoch();
      Serial.print(F("VIN voltage received from "));
      Serial.print(clientUid);
      Serial.print(F(": "));
      Serial.print(vinVoltage);
      Serial.println(F(" V"));
      gClientMetadataDirty = true;
    }
  }

  // Extract cellular signal strength from daily report (part 0 only)
  JsonObject sigObj = doc["sig"];
  if (isFirstPart && !sigObj.isNull()) {
    ClientMetadata *meta = findOrCreateClientMetadata(clientUid);
    if (meta) {
      int bars = sigObj["bars"] | -1;
      if (bars >= 0 && bars <= 4) meta->signalBars = (int8_t)bars;
      int rssi = sigObj["rssi"] | 0;
      if (rssi != 0) meta->signalRssi = (int16_t)rssi;
      int rsrp = sigObj["rsrp"] | 0;
      if (rsrp != 0) meta->signalRsrp = (int16_t)rsrp;
      int rsrq = sigObj["rsrq"] | 0;
      if (rsrq != 0) meta->signalRsrq = (int16_t)rsrq;
      const char *rat = sigObj["rat"] | "";
      if (rat && rat[0] != '\0') strlcpy(meta->signalRat, rat, sizeof(meta->signalRat));
      meta->signalEpoch = (epoch > 0.0) ? epoch : currentEpoch();
      gClientMetadataDirty = true;

      Serial.print(F("Signal strength from "));
      Serial.print(clientUid);
      Serial.print(F(": "));
      Serial.print(bars);
      Serial.print(F(" bars, RSRP="));
      Serial.print(meta->signalRsrp);
      Serial.println(F(" dBm"));

      // Warn on weak signal
      if (bars >= 0 && bars <= 1) {
        Serial.print(F("WARNING: Weak cellular signal for "));
        Serial.print(clientUid);
        Serial.println(F(" — inbound note delivery may be unreliable"));
        addServerSerialLog("Weak signal warning", "warn", "signal");
      }
    }
  }

  // Process alarm summary from daily report (backup notification path).
  // If the original alarm note was lost due to weak signal, detect active
  // alarms here and cross-reference with server-side SensorRecord state.
  JsonArray dailyAlarms = doc["alarms"];
  if (isFirstPart && dailyAlarms) {
    for (JsonObject a : dailyAlarms) {
      uint8_t sensorIdx = a["k"] | 0;
      bool hiAlarm = a["hi"] | false;
      bool loAlarm = a["lo"] | false;
      if (hiAlarm || loAlarm) {
        // Check if server already knows about this alarm (search without upserting)
        SensorRecord *rec = nullptr;
        for (uint8_t ri = 0; ri < gSensorRecordCount; ++ri) {
          if (strcmp(gSensorRecords[ri].clientUid, clientUid) == 0 && gSensorRecords[ri].sensorIndex == sensorIdx) {
            rec = &gSensorRecords[ri];
            break;
          }
        }
        if (rec && !rec->alarmActive) {
          Serial.print(F("WARNING: Client "));
          Serial.print(clientUid);
          Serial.print(F(" sensor "));
          Serial.print(sensorIdx);
          Serial.print(hiAlarm ? F(" has HIGH alarm") : F(" has LOW alarm"));
          Serial.println(F(" — server was unaware (alarm note may have been lost)"));
          addServerSerialLog("Missed alarm detected via daily report", "warn", "alarm");
          // Update server's alarm state from daily report backup data
          rec->alarmActive = true;
          strlcpy(rec->alarmType, hiAlarm ? "high" : "low", sizeof(rec->alarmType));
          rec->lastUpdateEpoch = (epoch > 0.0) ? epoch : currentEpoch();
          gSensorRegistryDirty = true;
        }
      }
    }

    // Reconciliation: clear alarms on server for sensors that the client
    // reports as NOT alarming.  This catches orphaned server-side alarms
    // where the "clear" note was lost or rate-limited.
    for (uint8_t ri = 0; ri < gSensorRecordCount; ++ri) {
      if (strcmp(gSensorRecords[ri].clientUid, clientUid) != 0) continue;
      if (!gSensorRecords[ri].alarmActive) continue;
      // Skip system alarm types — those aren't in the daily alarms array
      if (strcmp(gSensorRecords[ri].alarmType, "solar") == 0 ||
          strcmp(gSensorRecords[ri].alarmType, "battery") == 0 ||
          strcmp(gSensorRecords[ri].alarmType, "power") == 0) continue;
      // Check if this sensorIndex has an active alarm in the daily report
      bool foundInDaily = false;
      for (JsonObject a : dailyAlarms) {
        uint8_t dailyIdx = a["k"] | 0;
        if (dailyIdx == gSensorRecords[ri].sensorIndex) {
          bool hiAlarm = a["hi"] | false;
          bool loAlarm = a["lo"] | false;
          if (hiAlarm || loAlarm) foundInDaily = true;
          break;
        }
      }
      if (!foundInDaily) {
        Serial.print(F("Reconciliation: Clearing orphaned alarm on sensor "));
        Serial.print(gSensorRecords[ri].sensorIndex);
        Serial.print(F(" for client "));
        Serial.println(clientUid);
        addServerSerialLog("Orphaned alarm cleared via daily reconciliation", "info", "alarm");
        gSensorRecords[ri].alarmActive = false;
        strlcpy(gSensorRecords[ri].alarmType, "clear", sizeof(gSensorRecords[ri].alarmType));
        clearAlarmEvent(gSensorRecords[ri].clientUid, gSensorRecords[ri].sensorIndex);
        gSensorRegistryDirty = true;
      }
    }
  }

  // Get site name
  const char *siteName = doc["s"] | "";

  // Process sensor records in the daily report
  JsonArray sensors = doc["sensors"];
  for (JsonObject t : sensors) {
    uint8_t sensorIndex = t["k"].as<uint8_t>();
    SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex);
    if (!rec) {
      continue;
    }
    
    // Store optional user-assigned display number (0 = unset)
    if (t.containsKey("un")) {
      rec->userNumber = t["un"].as<uint8_t>();
    }

    strlcpy(rec->site, siteName, sizeof(rec->site));
    const char *label = t["n"] | "";
    if (label && strlen(label) > 0) {
      strlcpy(rec->label, label, sizeof(rec->label));
    } else if (rec->label[0] == '\0') {
      strlcpy(rec->label, "Tank", sizeof(rec->label)); // Default only if empty
    }
    
    // Store contents if provided - optional field (not used for RPM/engine monitors)
    const char *contents = t["cn"] | "";
    if (contents && strlen(contents) > 0) {
      strlcpy(rec->contents, contents, sizeof(rec->contents));
    }
    
    // Store object type if provided (tank, engine, pump, gas, flow)
    const char *objectType = t["ot"] | "";
    if (objectType && strlen(objectType) > 0) {
      strlcpy(rec->objectType, objectType, sizeof(rec->objectType));
    } else if (rec->objectType[0] == '\0') {
      strlcpy(rec->objectType, "tank", sizeof(rec->objectType)); // Default
    }
    
    // Store sensor interface type if provided
    const char *sensorType = t["st"] | "";
    if (sensorType && strlen(sensorType) > 0) {
      // Normalize "rpm" to "pulse" for consistency
      if (strcmp(sensorType, "rpm") == 0) {
        strlcpy(rec->sensorType, "pulse", sizeof(rec->sensorType));
      } else {
        strlcpy(rec->sensorType, sensorType, sizeof(rec->sensorType));
      }
    }
    
    // Store measurement unit if provided
    const char *unit = t["mu"] | "";
    if (unit && strlen(unit) > 0 && isValidMeasurementUnit(unit)) {
      strlcpy(rec->measurementUnit, unit, sizeof(rec->measurementUnit));
    }
    
    // Handle raw sensor readings
    float mA = 0.0f;
    float voltage = 0.0f;
    
    if (t["ma"]) {
      mA = t["ma"].as<float>();
      rec->sensorMa = (mA >= 4.0f) ? mA : 0.0f;
    } else if (t["sensorMa"]) {
      mA = t["sensorMa"].as<float>();
      rec->sensorMa = (mA >= 4.0f) ? mA : 0.0f;
    }
    if (t["vt"]) {
      voltage = t["vt"].as<float>();
      rec->sensorVoltage = (voltage > 0.0f) ? voltage : 0.0f;
    }
    
    // Get level - convert from raw sensor data if no level provided
    float newLevel = 0.0f;
    bool isCurrentLoop = (strcmp(rec->sensorType, "currentLoop") == 0);
    bool isAnalog = (strcmp(rec->sensorType, "analog") == 0);
    bool isDigital = (strcmp(rec->sensorType, "digital") == 0);
    bool isPulse = (strcmp(rec->sensorType, "pulse") == 0);
    
    if (isCurrentLoop && mA >= 4.0f) {
      float currentTemp = getCachedTemperature(clientUid);
      newLevel = convertMaToLevelWithTemp(clientUid, sensorIndex, mA, currentTemp);
    } else if (isAnalog && voltage > 0.0f) {
      newLevel = convertVoltageToLevel(clientUid, sensorIndex, voltage);
    } else if (isDigital && t["fl"]) {
      newLevel = t["fl"].as<float>();
    } else if (isPulse && t["rm"]) {
      newLevel = t["rm"].as<float>();
    }
    
    double now = (epoch > 0.0) ? epoch : currentEpoch();
    
    // Update 24-hour tracking: if current level is >22 hours old, roll it to previous
    // This ensures we always have a ~24hr baseline for change calculation
    const double HOURS_22 = 22.0 * 3600.0;  // 22 hours in seconds
    if (rec->lastUpdateEpoch > 0.0 && (now - rec->lastUpdateEpoch) >= HOURS_22) {
      rec->previousLevelInches = rec->levelInches;
      rec->previousLevelEpoch = rec->lastUpdateEpoch;
    } else if (rec->previousLevelEpoch == 0.0 && rec->lastUpdateEpoch > 0.0) {
      // Initialize previous level on first update after a gap
      rec->previousLevelInches = rec->levelInches;
      rec->previousLevelEpoch = rec->lastUpdateEpoch;
    }
    
    rec->levelInches = newLevel;
    rec->lastUpdateEpoch = now;
    gSensorRegistryDirty = true;
    
    // Record historical snapshot from daily report so sparklines/charts have data
    // even when change-based telemetry is disabled (levelChangeThreshold = 0)
    if (newLevel > 0.0f) {
      recordTelemetrySnapshot(clientUid, siteName, sensorIndex,
                              rec->levelInches, newLevel, vinVoltage);
    }
  }

  // --- Daily report part-loss detection ---
  // Track which parts arrive to detect gaps caused by weak cellular signal.
  bool moreParts = doc["m"] | false;
  ClientMetadata *partMeta = findOrCreateClientMetadata(clientUid);
  if (partMeta) {
    double reportTime = doc["t"] | 0.0;
    if (reportTime <= 0.0) reportTime = (epoch > 0.0) ? epoch : currentEpoch();

    // Detect new report batch: if report time differs by >30 min from last, reset tracking
    if (partMeta->dailyReportEpoch == 0.0 ||
        (reportTime - partMeta->dailyReportEpoch) > 1800.0) {
      // Check if previous batch was incomplete
      if (partMeta->dailyReportEpoch > 0.0 && !partMeta->dailyComplete && partMeta->dailyPartsReceived > 0) {
        Serial.print(F("WARNING: Previous daily report from "));
        Serial.print(clientUid);
        Serial.print(F(" was incomplete (received parts mask: 0x"));
        Serial.print(partMeta->dailyPartsReceived, HEX);
        Serial.println(F(")"));
        addServerSerialLog("Daily report incomplete - parts missing", "warn", "daily");
      }
      partMeta->dailyReportEpoch = reportTime;
      partMeta->dailyPartsReceived = 0;
      partMeta->dailyComplete = false;
      partMeta->dailyExpectedParts = 0;
    }

    // Mark this part as received (bits 0-7 for parts 0-7)
    if (part < 8) {
      partMeta->dailyPartsReceived |= (1 << part);
    }

    // If this is the final part (m=false), check completeness
    if (!moreParts) {
      partMeta->dailyComplete = true;
      partMeta->dailyExpectedParts = part + 1;

      // Verify all parts 0..part were received
      uint8_t expectedMask = (uint8_t)((1 << (part + 1)) - 1);
      if ((partMeta->dailyPartsReceived & expectedMask) != expectedMask) {
        Serial.print(F("WARNING: Daily report from "));
        Serial.print(clientUid);
        Serial.print(F(" missing parts! Expected mask=0x"));
        Serial.print(expectedMask, HEX);
        Serial.print(F(", received=0x"));
        Serial.println(partMeta->dailyPartsReceived, HEX);
        addServerSerialLog("Daily report missing parts", "warn", "daily");
      }
    }
  }
}

// ============================================================================
// Sensor Unload Event Handler
// ============================================================================
// Processes unload events from clients (fill-and-empty sensors)
static void handleUnload(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["c"] | "";
  const char *siteName = doc["s"] | "";
  const char *tankLabel = doc["n"] | "Tank";
  uint8_t sensorIndex = doc["k"].as<uint8_t>();
  
  if (!clientUid || strlen(clientUid) == 0) {
    Serial.println(F("Unload event missing client UID"));
    return;
  }
  
  // Extract unload event data
  float peakInches = doc["pk"].as<float>();
  float emptyInches = doc["em"].as<float>();
  double peakEpoch = doc["pt"].as<double>();
  double eventEpoch = doc["t"].as<double>();
  if (eventEpoch <= 0.0) {
    eventEpoch = (epoch > 0.0) ? epoch : currentEpoch();
  }
  
  // Extract raw sensor data if available
  float peakSensorMa = doc["pma"].as<float>();
  float emptySensorMa = doc["ema"].as<float>();
  
  // Extract notification preferences
  bool wantsSms = doc["sms"].as<bool>();
  bool wantsEmail = doc["email"].as<bool>();
  
  // Get measurement unit if provided
  const char *unit = doc["mu"] | "inches";
  
  Serial.print(F("Unload event received: "));
  Serial.print(siteName);
  Serial.print(F(" #"));
  Serial.print(sensorIndex);
  Serial.print(F(" ("));
  Serial.print(tankLabel);
  Serial.print(F(") peak="));
  Serial.print(peakInches);
  Serial.print(F(unit));
  Serial.print(F(", empty="));
  Serial.print(emptyInches);
  Serial.println(unit);
  
  // Create unload log entry
  UnloadLogEntry entry;
  memset(&entry, 0, sizeof(entry));
  entry.eventTimestamp = eventEpoch;
  entry.peakTimestamp = peakEpoch;
  strlcpy(entry.siteName, siteName, sizeof(entry.siteName));
  strlcpy(entry.clientUid, clientUid, sizeof(entry.clientUid));
  strlcpy(entry.tankLabel, tankLabel, sizeof(entry.tankLabel));
  entry.sensorIndex = sensorIndex;
  entry.peakInches = peakInches;
  entry.emptyInches = emptyInches;
  entry.peakSensorMa = peakSensorMa;
  entry.emptySensorMa = emptySensorMa;
  strlcpy(entry.measurementUnit, unit, sizeof(entry.measurementUnit));
  entry.smsNotified = false;
  entry.emailNotified = wantsEmail;
  
  // Log the unload event
  logUnloadEvent(entry);
  
  // Send SMS notification if requested
  if (wantsSms && (strlen(gConfig.smsPrimary) > 0 || strlen(gConfig.smsSecondary) > 0)) {
    sendUnloadSms(entry);
  }
  
  // Update sensor record with current level
  SensorRecord *rec = upsertSensorRecord(clientUid, sensorIndex);
  if (rec) {
    strlcpy(rec->site, siteName, sizeof(rec->site));
    strlcpy(rec->label, tankLabel, sizeof(rec->label));
    rec->levelInches = emptyInches;
    rec->lastUpdateEpoch = eventEpoch;
  }
}

static void logUnloadEvent(const UnloadLogEntry &entry) {
  // Store in ring buffer
  gUnloadLog[gUnloadLogWriteIndex] = entry;
  gUnloadLogWriteIndex = (gUnloadLogWriteIndex + 1) % MAX_UNLOAD_LOG_ENTRIES;
  if (gUnloadLogCount < MAX_UNLOAD_LOG_ENTRIES) {
    gUnloadLogCount++;
  }
  
  Serial.print(F("Unload logged: "));
  Serial.print(entry.siteName);
  Serial.print(F(" #"));
  Serial.print(entry.sensorIndex);
  Serial.print(F(" delivered "));
  Serial.print(entry.peakInches - entry.emptyInches, 1);
  Serial.print(F(" "));
  Serial.println(entry.measurementUnit[0] != '\0' ? entry.measurementUnit : "inches");
}

static void sendUnloadSms(const UnloadLogEntry &entry) {
  char message[160];
  float delivered = entry.peakInches - entry.emptyInches;
  const char *u = entry.measurementUnit[0] != '\0' ? entry.measurementUnit : "in";
  
  snprintf(message, sizeof(message), 
           "%s #%d unloaded: %.1f %s delivered (peak %.1f, now %.1f)",
           entry.siteName, entry.sensorIndex, delivered, u,
           entry.peakInches, entry.emptyInches);
  
  sendSmsAlert(message);
  Serial.print(F("Unload SMS sent: "));
  Serial.println(message);
}

static SensorRecord *upsertSensorRecord(const char *clientUid, uint8_t sensorIndex) {
  // Validate UID length to prevent silent truncation issues
  if (!isValidClientUid(clientUid)) {
    Serial.println(F("ERROR: Invalid client UID, skipping sensor record"));
    return nullptr;
  }
  
  // Use O(1) hash lookup instead of O(n) linear search
  SensorRecord *existing = findSensorByHash(clientUid, sensorIndex);
  if (existing) {
    return existing;
  }
  
  // Fallback: linear scan in case hash table is out of sync.
  // This prevents duplicate records if the hash table becomes corrupted.
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    if (strcmp(gSensorRecords[i].clientUid, clientUid) == 0 &&
        gSensorRecords[i].sensorIndex == sensorIndex) {
      Serial.println(F("WARNING: Hash miss — fallback linear scan found existing record. Rebuilding hash table."));
      rebuildSensorHashTable();
      return &gSensorRecords[i];
    }
  }
  
  if (gSensorRecordCount >= MAX_SENSOR_RECORDS) {
    // Evict the stalest non-alarm sensor record (LRU policy).
    // Alarmed records are protected only if updated within the last 72 hours;
    // stale alarms (likely orphaned) lose their eviction protection.
    uint8_t evictIdx = 0xFF;
    double oldestEpoch = 1e18;
    double now = currentEpoch();
    for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
      bool recentAlarm = gSensorRecords[i].alarmActive &&
                         (now - gSensorRecords[i].lastUpdateEpoch) < 259200.0;  // 72h
      if (!recentAlarm && gSensorRecords[i].lastUpdateEpoch < oldestEpoch) {
        oldestEpoch = gSensorRecords[i].lastUpdateEpoch;
        evictIdx = i;
      }
    }
    if (evictIdx == 0xFF) {
      // All records have recent active alarms - evict absolute stalest
      for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
        if (gSensorRecords[i].lastUpdateEpoch < oldestEpoch) {
          oldestEpoch = gSensorRecords[i].lastUpdateEpoch;
          evictIdx = i;
        }
      }
    }
    if (evictIdx == 0xFF) {
      Serial.println(F("ERROR: Cannot evict any sensor record"));
      return nullptr;
    }
    
    Serial.print(F("WARNING: Sensor capacity full ("));
    Serial.print(MAX_SENSOR_RECORDS);
    Serial.print(F(") - evicting stale record: "));
    Serial.print(gSensorRecords[evictIdx].site);
    Serial.print(F(" #"));
    Serial.println(gSensorRecords[evictIdx].sensorIndex);
    addServerSerialLog("Sensor record evicted (capacity full)", "warn", "registry");
    
    // Move last record into the evicted slot, then decrement count
    if (evictIdx < gSensorRecordCount - 1) {
      memcpy(&gSensorRecords[evictIdx], &gSensorRecords[gSensorRecordCount - 1], sizeof(SensorRecord));
    }
    gSensorRecordCount--;
    rebuildSensorHashTable();
    gSensorRegistryDirty = true;
  }
  
  // Create new record
  uint8_t newIndex = gSensorRecordCount;
  SensorRecord &rec = gSensorRecords[gSensorRecordCount++];
  memset(&rec, 0, sizeof(SensorRecord));
  strlcpy(rec.clientUid, clientUid, sizeof(rec.clientUid));
  rec.sensorIndex = sensorIndex;
  rec.lastUpdateEpoch = currentEpoch();
  rec.firstSeenEpoch = rec.lastUpdateEpoch;
  rec.lastSmsAlertEpoch = 0.0;
  rec.smsAlertsInLastHour = 0;
  for (uint8_t i = 0; i < 10; ++i) {
    rec.smsAlertTimestamps[i] = 0.0;
  }
  
  // Insert into hash table
  insertSensorIntoHash(newIndex);
  gSensorRegistryDirty = true;
  
  return &rec;
}

static bool checkSmsRateLimit(SensorRecord *rec, bool bypassMinimumInterval) {
  if (!rec) {
    return false;
  }

  double now = currentEpoch();
  if (now <= 0.0) {
    return false;  // No time sync yet, deny SMS until clock is available
  }

  // Check minimum interval since last SMS for this sensor
  if (!bypassMinimumInterval && now - rec->lastSmsAlertEpoch < MIN_SMS_ALERT_INTERVAL_SECONDS) {
    Serial.print(F("SMS rate limit: Too soon since last alert for "));
    Serial.print(rec->site);
    Serial.print(F(" #"));
    Serial.println(rec->sensorIndex);
    return false;
  }

  // Clean up old timestamps (older than 1 hour)
  double oneHourAgo = now - 3600.0;
  uint8_t validCount = 0;
  // Ensure we don't exceed array bounds (smsAlertTimestamps has 10 elements)
  uint8_t countToCheck = (rec->smsAlertsInLastHour > 10) ? 10 : rec->smsAlertsInLastHour;
  for (uint8_t i = 0; i < countToCheck; ++i) {
    if (rec->smsAlertTimestamps[i] > oneHourAgo) {
      rec->smsAlertTimestamps[validCount++] = rec->smsAlertTimestamps[i];
    }
  }
  rec->smsAlertsInLastHour = validCount;

  // Check hourly limit
  if (rec->smsAlertsInLastHour >= MAX_SMS_ALERTS_PER_HOUR) {
    Serial.print(F("SMS rate limit: Hourly limit exceeded for "));
    Serial.print(rec->site);
    Serial.print(F(" #"));
    Serial.print(rec->sensorIndex);
    Serial.print(F(" ("));
    Serial.print(rec->smsAlertsInLastHour);
    Serial.print(F("/"));
    Serial.print(MAX_SMS_ALERTS_PER_HOUR);
    Serial.println(F(")"));
    return false;
  }

  // Update tracking
  rec->lastSmsAlertEpoch = now;
  if (rec->smsAlertsInLastHour < 10) {
    rec->smsAlertTimestamps[rec->smsAlertsInLastHour++] = now;
  }

  return true;
}

static void sendSmsAlert(const char *message) {
  // Build list of phone numbers from contacts config
  JsonDocument contactsDoc;
  bool loaded = loadContactsConfig(contactsDoc);
  
  JsonDocument doc;
  doc["message"] = message;
  JsonArray numbers = doc["numbers"].to<JsonArray>();
  
  // Add phone numbers from smsAlertRecipients in contacts config
  if (loaded && contactsDoc["smsAlertRecipients"].is<JsonArray>()) {
    JsonArray recipients = contactsDoc["smsAlertRecipients"].as<JsonArray>();
    JsonArray contacts = contactsDoc["contacts"].as<JsonArray>();
    
    for (JsonVariant recipientId : recipients) {
      const char *id = recipientId.as<const char *>();
      if (!id) continue;
      
      // Find contact with this ID
      for (JsonVariant contactVar : contacts) {
        JsonObject contact = contactVar.as<JsonObject>();
        if (strcmp(contact["id"] | "", id) == 0) {
          const char *phone = contact["phone"] | "";
          if (strlen(phone) > 0) {
            numbers.add(phone);
          }
          break;
        }
      }
    }
  }
  
  // Fall back to legacy config if no contacts configured
  if (numbers.size() == 0) {
    if (strlen(gConfig.smsPrimary) > 0) {
      numbers.add(gConfig.smsPrimary);
    }
    if (strlen(gConfig.smsSecondary) > 0) {
      numbers.add(gConfig.smsSecondary);
    }
  }
  
  // No recipients configured
  if (numbers.size() == 0) {
    return;
  }

  char buffer[512];
  size_t len = serializeJson(doc, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) {
    return;
  }
  buffer[len] = '\0';

  J *req = notecard.newRequest("note.add");
  if (!req) {
    return;
  }
  JAddStringToObject(req, "file", "sms.qo");
  JAddBoolToObject(req, "sync", true);
  J *body = JParse(buffer);
  if (!body) {
    JDelete(req);  // Free the request (use JDelete for newRequest objects, not deleteResponse)
    return;
  }
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);

  J *smsRsp = notecard.requestAndResponse(req);
  if (smsRsp) {
    const char *smsErr = JGetString(smsRsp, "err");
    if (smsErr && smsErr[0] != '\0') {
      Serial.print(F("WARNING: SMS note.add failed: "));
      Serial.println(smsErr);
      logTransmission("", "", "sms", "error", smsErr);
    } else {
      logTransmission("", "", "sms", "outbox", message ? message : "SMS alert");
      Serial.print(F("SMS alert dispatched: "));
      Serial.println(message);
    }
    notecard.deleteResponse(smsRsp);
  } else {
    Serial.println(F("WARNING: SMS note.add returned no response"));
    logTransmission("", "", "sms", "error", "No response from Notecard");
  }
}

static void sendDailyEmail() {
  // Build list of email addresses from contacts config
  JsonDocument contactsDoc;
  bool loaded = loadContactsConfig(contactsDoc);
  
  // Collect email addresses from dailyReportRecipients
  String emailList = "";
  if (loaded && contactsDoc["dailyReportRecipients"].is<JsonArray>()) {
    JsonArray recipients = contactsDoc["dailyReportRecipients"].as<JsonArray>();
    JsonArray contacts = contactsDoc["contacts"].as<JsonArray>();
    
    for (JsonVariant recipientId : recipients) {
      const char *id = recipientId.as<const char *>();
      if (!id) continue;
      
      // Find contact with this ID
      for (JsonVariant contactVar : contacts) {
        JsonObject contact = contactVar.as<JsonObject>();
        if (strcmp(contact["id"] | "", id) == 0) {
          const char *email = contact["email"] | "";
          if (strlen(email) > 0) {
            if (emailList.length() > 0) {
              emailList += ",";
            }
            emailList += email;
          }
          break;
        }
      }
    }
  }
  
  // Fall back to legacy config if no contacts configured
  if (emailList.length() == 0 && strlen(gConfig.dailyEmail) > 0) {
    emailList = gConfig.dailyEmail;
  }
  
  // No recipients configured
  if (emailList.length() == 0) {
    return;
  }

  // Check rate limit to prevent email spam
  double now = currentEpoch();
  if (now > 0.0 && (now - gLastDailyEmailSentEpoch) < MIN_DAILY_EMAIL_INTERVAL_SECONDS) {
    Serial.print(F("Daily email rate limited ("));
    Serial.print((int)((now - gLastDailyEmailSentEpoch) / 60.0));
    Serial.println(F(" minutes since last)"));
    return;
  }

  // ArduinoJson v7: JsonDocument auto-sizes
  JsonDocument doc;
  doc["to"] = emailList;
  doc["subject"] = "Daily Sensor Summary";
  JsonArray sensors = doc["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    JsonObject obj = sensors.add<JsonObject>();
    obj["client"] = gSensorRecords[i].clientUid;
    obj["site"] = gSensorRecords[i].site;
    obj["label"] = gSensorRecords[i].label;
    obj["sensorIndex"] = gSensorRecords[i].sensorIndex;
    if (gSensorRecords[i].userNumber > 0) {
      obj["userNumber"] = gSensorRecords[i].userNumber;
    }
    obj["levelInches"] = roundTo(gSensorRecords[i].levelInches, 1);
    obj["sensorMa"] = roundTo(gSensorRecords[i].sensorMa, 2);
    obj["alarm"] = gSensorRecords[i].alarmActive;
    obj["alarmType"] = gSensorRecords[i].alarmType;
  }

  // Check if JSON document overflowed during population
  if (doc.overflowed()) {
    Serial.println(F("Daily email JSON document overflowed"));
    return;
  }

  // Use static buffer to avoid 16KB stack allocation (Mbed OS stack is only 4-8KB)
  static char buffer[MAX_EMAIL_BUFFER];
  size_t len = serializeJson(doc, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) {
    Serial.println(F("Daily email payload too large"));
    return;
  }
  buffer[len] = '\0';

  J *req = notecard.newRequest("note.add");
  if (!req) {
    return;
  }
  JAddStringToObject(req, "file", "email.qo");
  JAddBoolToObject(req, "sync", true);
  J *body = JParse(buffer);
  if (!body) {
    JDelete(req);  // Free the request to prevent memory leak
    Serial.println(F("Daily email JSON parse failed"));
    return;
  }

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  // Kick watchdog before the potentially long Notecard transaction:
  // JSON construction + I2C note.add can exceed 30s with many sensors.
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);

  J *emailRsp = notecard.requestAndResponse(req);
  if (emailRsp) {
    const char *emailErr = JGetString(emailRsp, "err");
    if (emailErr && emailErr[0] != '\0') {
      Serial.print(F("WARNING: Email note.add failed: "));
      Serial.println(emailErr);
      logTransmission("", "", "email", "error", emailErr);
    } else {
      gLastDailyEmailSentEpoch = now;
      Serial.println(F("Daily email queued"));
      logTransmission("", "", "email", "outbox", "Daily sensor summary email");
    }
    notecard.deleteResponse(emailRsp);
  } else {
    Serial.println(F("WARNING: Email note.add returned no response"));
    logTransmission("", "", "email", "error", "No response from Notecard");
  }
}

static void publishViewerSummary() {
  if (!gConfig.viewerEnabled) {
    return;  // Viewer device not enabled
  }
  JsonDocument doc;
  doc["sn"] = gConfig.serverName;
  doc["si"] = gServerUid;
  double now = currentEpoch();
  doc["ge"] = now;
  doc["rs"] = VIEWER_SUMMARY_INTERVAL_SECONDS;
  doc["bh"] = VIEWER_SUMMARY_BASE_HOUR;
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    JsonObject obj = arr.add<JsonObject>();
    obj["c"] = gSensorRecords[i].clientUid;
    obj["s"] = gSensorRecords[i].site;
    obj["n"] = gSensorRecords[i].label;
    obj["k"] = gSensorRecords[i].sensorIndex;
    if (gSensorRecords[i].userNumber > 0) {
      obj["un"] = gSensorRecords[i].userNumber;
    }
    obj["l"] = roundTo(gSensorRecords[i].levelInches, 1);
    // Include raw sensor readings if available
    if (gSensorRecords[i].sensorMa >= 4.0f) {
      obj["ma"] = roundTo(gSensorRecords[i].sensorMa, 2);
    }
    if (gSensorRecords[i].sensorVoltage > 0.0f) {
      obj["vt"] = roundTo(gSensorRecords[i].sensorVoltage, 3);
    }
    // Include object type if known (tank, engine, pump, gas, flow)
    if (gSensorRecords[i].objectType[0] != '\0') {
      obj["ot"] = gSensorRecords[i].objectType;
    }
    // Include sensor interface type if known (digital, analog, currentLoop, pulse)
    if (gSensorRecords[i].sensorType[0] != '\0') {
      obj["st"] = gSensorRecords[i].sensorType;
    }
    // Include measurement unit if known (inches, rpm, psi, gpm, etc.)
    if (gSensorRecords[i].measurementUnit[0] != '\0') {
      obj["mu"] = gSensorRecords[i].measurementUnit;
    }
    // Include 24hr change if available
    if (gSensorRecords[i].previousLevelEpoch > 0.0) {
      obj["d"] = roundTo(gSensorRecords[i].levelInches - gSensorRecords[i].previousLevelInches, 1);
    }
    obj["a"] = gSensorRecords[i].alarmActive;
    obj["at"] = gSensorRecords[i].alarmType;
    obj["u"] = gSensorRecords[i].lastUpdateEpoch;
    
    // Add VIN voltage from client metadata if available
    ClientMetadata *meta = findClientMetadata(gSensorRecords[i].clientUid);
    if (meta && meta->vinVoltage > 0.0f) {
      obj["v"] = roundTo(meta->vinVoltage, 2);
    }
  }

  String json;
  if (serializeJson(doc, json) == 0) {
    Serial.println(F("Viewer summary serialization failed"));
    return;
  }

  J *req = notecard.newRequest("note.add");
  if (!req) {
    return;
  }
  JAddStringToObject(req, "file", VIEWER_SUMMARY_FILE);
  JAddBoolToObject(req, "sync", true);
  J *body = JParse(json.c_str());
  if (!body) {
    JDelete(req);  // Free the request to prevent memory leak
    Serial.println(F("Viewer summary JSON parse failed"));
    return;
  }
  stampSchemaVersion(body);
  JAddItemToObject(req, "body", body);
  bool queued = notecard.sendRequest(req);
  if (queued) {
    gLastViewerSummaryEpoch = now;
    Serial.println(F("Viewer summary queued"));
    // Log to transmission log
    logTransmission("", gConfig.serverName, "viewer_summary", "outbox", "Viewer summary published");
  } else {
    Serial.println(F("Viewer summary queue failed"));
    logTransmission("", gConfig.serverName, "viewer_summary", "failed", "Viewer summary queue failed");
  }
}

static ClientConfigSnapshot *findClientConfigSnapshot(const char *clientUid) {
  if (!clientUid) {
    return nullptr;
  }
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    if (strcmp(gClientConfigs[i].uid, clientUid) == 0) {
      return &gClientConfigs[i];
    }
  }
  return nullptr;
}

// GET /api/client?uid=dev:xxx  → return cached config for a single client
static void handleClientConfigGet(EthernetClient &client, const String &query) {
  String uid = getQueryParam(query, "uid");
  if (uid.length() == 0) {
    respondStatus(client, 400, F("Missing uid parameter"));
    return;
  }

  ClientConfigSnapshot *snap = findClientConfigSnapshot(uid.c_str());
  if (!snap || snap->payload[0] == '\0') {
    respondStatus(client, 404, F("No config found for client"));
    return;
  }

  // Send the raw stored JSON directly to avoid JsonDocument overhead
  // truncating nested fields (e.g. monitorType inside sensors array).
  // BugFix v1.6.2 (M-11): Include dispatch/ACK metadata so the Config Generator
  // can display delivery status without checking serial logs.
  String response = F("{\"config\":");
  response += snap->payload;
  response += F(",\"dispatch\":{");
  response += F("\"pending\":");
  response += snap->pendingDispatch ? F("true") : F("false");
  response += F(",\"attempts\":");
  response += String(snap->dispatchAttempts);
  response += F(",\"lastDispatchEpoch\":");
  response += String(snap->lastDispatchEpoch, 0);
  response += F(",\"lastAckEpoch\":");
  response += String(snap->lastAckEpoch, 0);
  response += F(",\"lastAckStatus\":\"");
  response += snap->lastAckStatus;
  response += F("\",\"configVersion\":\"");
  response += snap->configVersion;
  response += F("\"}}");
  respondJson(client, response);
}

// Look up the preset SG for a fluid name. Returns 1.0 (water) for unknown names.
// Mirror of the table on the client. Keep these in sync.
static float getPresetSpecificGravityServer(const char *fluidName) {
  if (!fluidName) return 1.0f;
  if (strcmp(fluidName, "water") == 0)        return 1.00f;
  if (strcmp(fluidName, "diesel") == 0)       return 0.85f;
  if (strcmp(fluidName, "gasoline") == 0)     return 0.74f;
  if (strcmp(fluidName, "heatingOil") == 0)   return 0.85f;
  if (strcmp(fluidName, "propane") == 0 || strcmp(fluidName, "lpg") == 0) return 0.50f;
  if (strcmp(fluidName, "brine") == 0)        return 1.20f;
  if (strcmp(fluidName, "crudeOil") == 0)     return 0.83f;
  if (strcmp(fluidName, "usedOil") == 0)      return 0.92f;
  if (strcmp(fluidName, "glycol50") == 0)     return 1.07f;
  if (strcmp(fluidName, "def") == 0 || strcmp(fluidName, "adblue") == 0) return 1.09f;
  if (strcmp(fluidName, "ethanol") == 0)      return 0.79f;
  return 1.0f;
}

// After learnedSlope is computed, back out an implied fluid SG from the difference
// between the learned mA->inches slope and the theoretical water slope predicted by
// the sensor's configured pressure range. Stores 0.0f if not derivable (e.g., sensor
// is ultrasonic, gas, missing config, or non-pressure unit).
//
//   theoretical_water_slope = (rangeMax - rangeMin) * pressureFactor / 16   [inches per mA, water]
//   impliedSG               = theoretical_water_slope / learnedSlope
//
// A diesel tank should yield impliedSG ~ 0.85; brine ~ 1.20; etc. Drift from the
// admin's selected fluid preset by > 0.05 likely indicates wrong fluid label, sensor
// drift, or contamination.
static void computeImpliedSpecificGravity(SensorCalibration *cal) {
  if (!cal) return;
  cal->impliedSpecificGravity = 0.0f;
  cal->specificGravityDriftFromPreset = 0.0f;

  if (cal->learnedSlope <= 0.0f) return;
  if (strcmp(cal->sensorType, "pressure") != 0) return;

  ClientConfigSnapshot *snap = findClientConfigSnapshot(cal->clientUid);
  if (!snap || strlen(snap->payload) == 0) return;

  JsonDocument doc;
  if (deserializeJson(doc, snap->payload)) return;
  JsonArray sensors = doc["sensors"];
  if (!sensors) return;

  for (JsonVariant t : sensors) {
    uint8_t tn = t["number"] | 0;
    if (tn != cal->sensorIndex) continue;

    const char *monitorType = t["monitorType"] | "tank";
    if (strcmp(monitorType, "gas") == 0) return;  // SG is meaningless for gas

    float rangeMin = t["sensorRangeMin"] | 0.0f;
    float rangeMax = t["sensorRangeMax"] | 0.0f;
    if (rangeMax <= rangeMin) return;

    const char *rangeUnit = t["sensorRangeUnit"] | "PSI";
    float pressureFactor = 27.68f;
    if      (strcmp(rangeUnit, "bar") == 0)   pressureFactor = 401.46f;
    else if (strcmp(rangeUnit, "kPa") == 0)   pressureFactor = 4.0146f;
    else if (strcmp(rangeUnit, "mbar") == 0)  pressureFactor = 0.40146f;
    else if (strcmp(rangeUnit, "inH2O") == 0) pressureFactor = 1.0f;

    float theoreticalWaterSlope = (rangeMax - rangeMin) * pressureFactor / 16.0f;  // inches/mA assuming water
    float impliedSg = theoreticalWaterSlope / cal->learnedSlope;
    if (impliedSg < 0.3f || impliedSg > 2.0f) return;  // implausible -- bad cal data
    cal->impliedSpecificGravity = impliedSg;

    // Compare to the admin-selected fluid preset (or override) for drift warning.
    float presetSg = t["fluidSpecificGravity"] | 0.0f;
    if (presetSg < 0.3f || presetSg > 2.0f) {
      const char *fluidTypeName = t["fluidType"] | "water";
      presetSg = getPresetSpecificGravityServer(fluidTypeName);
    }
    cal->specificGravityDriftFromPreset = fabs(impliedSg - presetSg);
    return;
  }
}

// Convert raw 4-20mA reading to level with optional temperature compensation
static float convertMaToLevelWithTemp(const char *clientUid, uint8_t sensorIndex, float mA, float currentTempF) {
  if (mA < 4.0f || mA > 20.0f) {
    return 0.0f;  // Invalid mA reading
  }
  
  // Check if we have learned calibration data for this sensor
  SensorCalibration *cal = findSensorCalibration(clientUid, sensorIndex);
  if (cal && cal->hasLearnedCalibration) {
    // Use learned calibration: level = slope * mA + offset
    float level = cal->learnedSlope * mA + cal->learnedOffset;
    
    // Apply temperature compensation if available
    if (cal->hasTempCompensation && currentTempF > TEMPERATURE_UNAVAILABLE + 1.0f) {
      // Temperature coefficient is in inches per °F deviation from 70°F
      float tempDeviation = currentTempF - TEMP_REFERENCE_F;
      level += cal->learnedTempCoef * tempDeviation;
    }
    
    // Ensure level is non-negative
    if (level < 0.0f) level = 0.0f;
    return level;
  }
  
  // No calibration available - use theoretical calculation from config
  ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
  if (!snap || strlen(snap->payload) == 0) {
    // No config snapshot available - use simple linear 4-20mA mapping
    // Assume 0-100 range as fallback
    float level = ((mA - 4.0f) / 16.0f) * 100.0f;
    if (level < 0.0f) level = 0.0f;
    return level;
  }
  
  // Parse the config snapshot to find the sensor settings
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, snap->payload);
  if (err) {
    float level = ((mA - 4.0f) / 16.0f) * 100.0f;
    if (level < 0.0f) level = 0.0f;
    return level;  // Fallback
  }
  
  // Find the sensor in the config
  JsonArray sensors = doc["sensors"];
  if (!sensors) {
    float level = ((mA - 4.0f) / 16.0f) * 100.0f;
    if (level < 0.0f) level = 0.0f;
    return level;  // Fallback
  }
  
  for (JsonVariant t : sensors) {
    uint8_t tn = t["number"] | 0;
    if (tn == sensorIndex) {
      // Found the sensor - get sensor range settings
      float rangeMin = t["sensorRangeMin"] | 0.0f;
      float rangeMax = t["sensorRangeMax"] | 5.0f;  // Default 0-5 PSI
      float mountHeight = t["sensorMountHeight"] | 0.0f;
      const char *currentLoopType = t["currentLoopType"] | "pressure";
      const char *monitorType = t["monitorType"] | "tank";
      const char *rangeUnit = t["sensorRangeUnit"] | "PSI";
      // Fluid characterization (added 2026-04 for non-water tanks).
      // SG defaults to 1.0 (water). When > 0, divides the PSI->inches conversion so
      // diesel/propane/brine/etc. report correct head height before learned calibration
      // takes over. Falls back to water if missing/implausible.
      float fluidSg = t["fluidSpecificGravity"] | 0.0f;
      if (fluidSg < 0.3f || fluidSg > 2.0f) {
        // Try to derive from preset name. If missing, default water.
        const char *fluidTypeName = t["fluidType"] | "water";
        if      (strcmp(fluidTypeName, "diesel") == 0)       fluidSg = 0.85f;
        else if (strcmp(fluidTypeName, "gasoline") == 0)     fluidSg = 0.74f;
        else if (strcmp(fluidTypeName, "heatingOil") == 0)   fluidSg = 0.85f;
        else if (strcmp(fluidTypeName, "propane") == 0 || strcmp(fluidTypeName, "lpg") == 0) fluidSg = 0.50f;
        else if (strcmp(fluidTypeName, "brine") == 0)        fluidSg = 1.20f;
        else if (strcmp(fluidTypeName, "crudeOil") == 0)     fluidSg = 0.83f;
        else if (strcmp(fluidTypeName, "usedOil") == 0)      fluidSg = 0.92f;
        else if (strcmp(fluidTypeName, "glycol50") == 0)     fluidSg = 1.07f;
        else if (strcmp(fluidTypeName, "def") == 0 || strcmp(fluidTypeName, "adblue") == 0) fluidSg = 1.09f;
        else if (strcmp(fluidTypeName, "ethanol") == 0)      fluidSg = 0.79f;
        else                                                  fluidSg = 1.00f;
      }

      // Calculate the fraction within 4-20mA range
      float fraction = (mA - 4.0f) / 16.0f;

      float sensorValue;
      if (strcmp(currentLoopType, "ultrasonic") == 0) {
        // Ultrasonic: 4mA = full (rangeMin distance), 20mA = empty (rangeMax distance)
        // Level = mountHeight - distance
        float distance = rangeMin + fraction * (rangeMax - rangeMin);
        sensorValue = mountHeight - distance;
        if (sensorValue < 0.0f) sensorValue = 0.0f;
      } else if (strcmp(monitorType, "gas") == 0) {
        // Gas pressure monitor: report raw pressure in its native unit (PSI/bar/...).
        // No PSI->inches conversion, no mount-height add. Mirrors the client's gas path.
        sensorValue = rangeMin + fraction * (rangeMax - rangeMin);
        if (sensorValue < 0.0f) sensorValue = 0.0f;
      } else {
        // Pressure sensor on a liquid tank: convert PSI (or other pressure unit) to
        // inches of fluid, dividing by SG so non-water fluids size correctly.
        float pressure = rangeMin + fraction * (rangeMax - rangeMin);
        float pressureFactor = 27.68f; // PSI -> inches of water
        if      (strcmp(rangeUnit, "bar") == 0)   pressureFactor = 401.46f;
        else if (strcmp(rangeUnit, "kPa") == 0)   pressureFactor = 4.0146f;
        else if (strcmp(rangeUnit, "mbar") == 0)  pressureFactor = 0.40146f;
        else if (strcmp(rangeUnit, "inH2O") == 0) pressureFactor = 1.0f;
        sensorValue = (pressure * pressureFactor) / fluidSg + mountHeight;
        if (sensorValue < 0.0f) sensorValue = 0.0f;
      }
      return sensorValue;
    }
  }
  
  // Sensor not found in config - use fallback
  float level = ((mA - 4.0f) / 16.0f) * 100.0f;
  if (level < 0.0f) level = 0.0f;
  return level;
}

// Convert raw voltage reading to level/value using sensor config from client config snapshot
// Returns the computed level, or 0.0 if config not found or invalid voltage
static float convertVoltageToLevel(const char *clientUid, uint8_t sensorIndex, float voltage) {
  if (voltage < 0.0f || voltage > 12.0f) {
    return 0.0f;  // Invalid voltage reading (allow up to 12V for headroom)
  }
  
  ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
  if (!snap || strlen(snap->payload) == 0) {
    // No config snapshot available - use simple linear 0-10V mapping
    // Assume 0-100 range as fallback
    return (voltage / 10.0f) * 100.0f;
  }
  
  // Parse the config snapshot to find the sensor settings
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, snap->payload);
  if (err) {
    return (voltage / 10.0f) * 100.0f;  // Fallback
  }
  
  // Find the sensor in the config
  JsonArray sensors = doc["sensors"];
  if (!sensors) {
    return (voltage / 10.0f) * 100.0f;  // Fallback
  }
  
  for (JsonVariant t : sensors) {
    uint8_t tn = t["number"] | 0;
    if (tn == sensorIndex) {
      // Found the sensor - get sensor range settings
      float voltageMin = t["analogVoltageMin"] | 0.0f;   // e.g., 0V or 1V
      float voltageMax = t["analogVoltageMax"] | 10.0f;  // e.g., 10V or 5V
      float rangeMin = t["sensorRangeMin"] | 0.0f;       // e.g., 0 PSI
      float rangeMax = t["sensorRangeMax"] | 100.0f;     // e.g., 100 inches or 5 PSI
      float mountHeight = t["sensorMountHeight"] | 0.0f;
      
      // Validate voltage range
      float voltageRange = voltageMax - voltageMin;
      if (voltageRange <= 0.0f) {
        return (voltage / 10.0f) * 100.0f;  // Invalid config fallback
      }
      
      // Calculate the fraction within the voltage range
      float fraction = (voltage - voltageMin) / voltageRange;
      if (fraction < 0.0f) fraction = 0.0f;
      if (fraction > 1.0f) fraction = 1.0f;
      
      // Map voltage to sensor's native range
      float sensorValue = rangeMin + fraction * (rangeMax - rangeMin);
      
      // Add mount height offset (for pressure sensors measuring liquid column)
      // Note: for pure pressure monitoring applications, mountHeight is typically 0
      return sensorValue + mountHeight;
    }
  }
  
  // Sensor not found in config - use fallback
  return (voltage / 10.0f) * 100.0f;
}

// ============================================================================
// Sensor Registry Persistence (LittleFS JSON)
// ============================================================================

static void saveSensorRegistry() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS || gSensorRecordCount == 0) {
      return;
    }
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
      const SensorRecord &rec = gSensorRecords[i];
      if (rec.clientUid[0] == '\0') continue;
      JsonObject obj = arr.add<JsonObject>();
      obj["c"] = rec.clientUid;
      obj["k"] = rec.sensorIndex;
      if (rec.userNumber > 0) obj["un"] = rec.userNumber;
      obj["s"] = rec.site;
      obj["n"] = rec.label;
      if (rec.contents[0] != '\0') obj["cn"] = rec.contents;
      if (rec.objectType[0] != '\0') obj["ot"] = rec.objectType;
      if (rec.sensorType[0] != '\0') obj["st"] = rec.sensorType;
      if (rec.measurementUnit[0] != '\0') obj["mu"] = rec.measurementUnit;
      obj["l"] = rec.levelInches;
      if (rec.sensorMa >= 4.0f) obj["ma"] = rec.sensorMa;
      if (rec.sensorVoltage > 0.0f) obj["vt"] = rec.sensorVoltage;
      obj["a"] = rec.alarmActive;
      if (rec.alarmType[0] != '\0') obj["at"] = rec.alarmType;
      obj["u"] = rec.lastUpdateEpoch;
      if (rec.firstSeenEpoch > 0.0) obj["fs"] = rec.firstSeenEpoch;
      if (rec.previousLevelEpoch > 0.0) {
        obj["pl"] = rec.previousLevelInches;
        obj["pe"] = rec.previousLevelEpoch;
      }
      // Persist SMS rate-limit state to survive reboots
      if (rec.lastSmsAlertEpoch > 0.0) {
        obj["se"] = rec.lastSmsAlertEpoch;
        obj["sa"] = rec.smsAlertsInLastHour;
      }
    }
    
    // Serialize to buffer and write to file
    size_t jsonLen = measureJson(doc);
    char *buf = (char *)malloc(jsonLen + 1);
    if (!buf) {
      Serial.println(F("ERROR: Cannot allocate sensor registry save buffer"));
      return;
    }
    serializeJson(doc, buf, jsonLen + 1);
    
    if (posix_write_file("/fs" SENSOR_REGISTRY_PATH, buf, jsonLen)) {
      Serial.print(F("Sensor registry saved: "));
      Serial.print(gSensorRecordCount);
      Serial.println(F(" records"));
    }
    free(buf);
  #endif
#endif
}

static void loadSensorRegistry() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return;
    
    if (!posix_file_exists("/fs" SENSOR_REGISTRY_PATH)) {
      Serial.println(F("No sensor registry file found - starting fresh"));
      return;
    }
    
    // Read file into buffer
    FILE *fp = fopen("/fs" SENSOR_REGISTRY_PATH, "r");
    if (!fp) {
      posix_log_error("fopen", "/fs" SENSOR_REGISTRY_PATH);
      return;
    }
    long fileSize = posix_file_size(fp);
    if (fileSize <= 0 || fileSize > 32768) {
      fclose(fp);
      Serial.println(F("Sensor registry file size invalid"));
      return;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(fileSize + 1);
    if (!buf) {
      fclose(fp);
      Serial.println(F("ERROR: Cannot allocate sensor registry load buffer"));
      return;
    }
    size_t bytesRead = fread(buf, 1, fileSize, fp);
    fclose(fp);
    buf[bytesRead] = '\0';
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    free(buf);
    if (err) {
      Serial.print(F("ERROR: Sensor registry parse failed: "));
      Serial.println(err.c_str());
      return;
    }
    
    gSensorRecordCount = 0;
    initSensorHashTable();
    
    JsonArray arr = doc.as<JsonArray>();
    uint8_t dupsMerged = 0;
    for (JsonObject obj : arr) {
      if (gSensorRecordCount >= MAX_SENSOR_RECORDS) break;
      
      const char *loadedUid = obj["c"] | "";
      uint8_t loadedSensorIdx = obj["k"] | 0;
      double loadedEpoch = obj["u"] | 0.0;
      
      // Deduplicate: if a record with this clientUid+sensorIndex already exists,
      // keep the one with the most recent lastUpdateEpoch and merge fields.
      SensorRecord *existing = findSensorByHash(loadedUid, loadedSensorIdx);
      if (existing) {
        if (loadedEpoch > existing->lastUpdateEpoch) {
          // Newer record — overwrite into the existing slot
          strlcpy(existing->site, obj["s"] | "", sizeof(existing->site));
          strlcpy(existing->label, obj["n"] | "", sizeof(existing->label));
          strlcpy(existing->contents, obj["cn"] | "", sizeof(existing->contents));
          strlcpy(existing->objectType, obj["ot"] | "", sizeof(existing->objectType));
          strlcpy(existing->sensorType, obj["st"] | "", sizeof(existing->sensorType));
          strlcpy(existing->measurementUnit, obj["mu"] | "", sizeof(existing->measurementUnit));
          existing->levelInches = obj["l"] | 0.0f;
          existing->sensorMa = obj["ma"] | 0.0f;
          existing->sensorVoltage = obj["vt"] | 0.0f;
          existing->alarmActive = obj["a"] | false;
          strlcpy(existing->alarmType, obj["at"] | "", sizeof(existing->alarmType));
          existing->lastUpdateEpoch = loadedEpoch;
          existing->previousLevelInches = obj["pl"] | 0.0f;
          existing->previousLevelEpoch = obj["pe"] | 0.0;
          existing->userNumber = obj["un"] | 0;
          // Preserve the earliest firstSeenEpoch across duplicates
          double loadedFs = obj["fs"] | 0.0;
          if (loadedFs > 0.0 && (existing->firstSeenEpoch <= 0.0 || loadedFs < existing->firstSeenEpoch)) {
            existing->firstSeenEpoch = loadedFs;
          }
        }
        dupsMerged++;
        continue;  // Skip — already have this clientUid+sensorIndex
      }
      
      SensorRecord &rec = gSensorRecords[gSensorRecordCount];
      memset(&rec, 0, sizeof(SensorRecord));
      strlcpy(rec.clientUid, loadedUid, sizeof(rec.clientUid));
      rec.sensorIndex = loadedSensorIdx;
      rec.userNumber = obj["un"] | 0;
      strlcpy(rec.site, obj["s"] | "", sizeof(rec.site));
      strlcpy(rec.label, obj["n"] | "", sizeof(rec.label));
      strlcpy(rec.contents, obj["cn"] | "", sizeof(rec.contents));
      strlcpy(rec.objectType, obj["ot"] | "", sizeof(rec.objectType));
      strlcpy(rec.sensorType, obj["st"] | "", sizeof(rec.sensorType));
      strlcpy(rec.measurementUnit, obj["mu"] | "", sizeof(rec.measurementUnit));
      rec.levelInches = obj["l"] | 0.0f;
      rec.sensorMa = obj["ma"] | 0.0f;
      rec.sensorVoltage = obj["vt"] | 0.0f;
      rec.alarmActive = obj["a"] | false;
      strlcpy(rec.alarmType, obj["at"] | "", sizeof(rec.alarmType));
      rec.lastUpdateEpoch = loadedEpoch;
      rec.previousLevelInches = obj["pl"] | 0.0f;
      rec.previousLevelEpoch = obj["pe"] | 0.0;
      rec.firstSeenEpoch = obj["fs"] | 0.0;
      rec.lastSmsAlertEpoch = obj["se"] | 0.0;
      rec.smsAlertsInLastHour = obj["sa"] | 0;
      
      insertSensorIntoHash(gSensorRecordCount);
      gSensorRecordCount++;
    }
    
    if (dupsMerged > 0) {
      Serial.print(F("Sensor registry: merged "));
      Serial.print(dupsMerged);
      Serial.println(F(" duplicate records"));
      gSensorRegistryDirty = true;  // Re-save to clean the file
    }
    
    Serial.print(F("Sensor registry loaded: "));
    Serial.print(gSensorRecordCount);
    Serial.println(F(" records"));
  #endif
#endif
}

// ============================================================================
// Client Metadata Persistence (LittleFS JSON)
// ============================================================================

static void saveClientMetadataCache() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS || gClientMetadataCount == 0) {
      return;
    }
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
      const ClientMetadata &meta = gClientMetadata[i];
      if (meta.clientUid[0] == '\0') continue;
      JsonObject obj = arr.add<JsonObject>();
      obj["c"] = meta.clientUid;
      if (meta.vinVoltage > 0.0f) {
        obj["v"] = meta.vinVoltage;
        obj["ve"] = meta.vinVoltageEpoch;
      }
      if (meta.latitude != 0.0f || meta.longitude != 0.0f) {
        obj["lat"] = meta.latitude;
        obj["lon"] = meta.longitude;
        obj["le"] = meta.locationEpoch;
      }
      if (meta.nwsGridValid) {
        obj["go"] = meta.nwsGridOffice;
        obj["gx"] = meta.nwsGridX;
        obj["gy"] = meta.nwsGridY;
      }
      if (meta.firmwareVersion[0] != '\0') {
        obj["fv"] = meta.firmwareVersion;
      }
      if (meta.signalBars >= 0) {
        obj["sb"] = meta.signalBars;
        obj["sr"] = meta.signalRssi;
        obj["sp"] = meta.signalRsrp;
        obj["sq"] = meta.signalRsrq;
        if (meta.signalRat[0] != '\0') obj["sn"] = meta.signalRat;
        obj["se"] = meta.signalEpoch;
      }
      if (meta.lastSystemAlarmType[0] != '\0') {
        obj["sa"] = meta.lastSystemAlarmType;
        obj["sae"] = meta.lastSystemAlarmEpoch;
      }
      // Note: cachedTemperatureF and staleAlertSent are runtime-only, not persisted
    }
    
    size_t jsonLen = measureJson(doc);
    char *buf = (char *)malloc(jsonLen + 1);
    if (!buf) {
      Serial.println(F("ERROR: Cannot allocate metadata save buffer"));
      return;
    }
    serializeJson(doc, buf, jsonLen + 1);
    
    if (posix_write_file("/fs" CLIENT_METADATA_CACHE_PATH, buf, jsonLen)) {
      Serial.print(F("Client metadata saved: "));
      Serial.print(gClientMetadataCount);
      Serial.println(F(" entries"));
    }
    free(buf);
  #endif
#endif
}

static void loadClientMetadataCache() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return;
    
    if (!posix_file_exists("/fs" CLIENT_METADATA_CACHE_PATH)) {
      Serial.println(F("No client metadata cache found - starting fresh"));
      return;
    }
    
    FILE *fp = fopen("/fs" CLIENT_METADATA_CACHE_PATH, "r");
    if (!fp) {
      posix_log_error("fopen", "/fs" CLIENT_METADATA_CACHE_PATH);
      return;
    }
    long fileSize = posix_file_size(fp);
    if (fileSize <= 0 || fileSize > 16384) {
      fclose(fp);
      Serial.println(F("Client metadata file size invalid"));
      return;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc(fileSize + 1);
    if (!buf) {
      fclose(fp);
      Serial.println(F("ERROR: Cannot allocate metadata load buffer"));
      return;
    }
    size_t bytesRead = fread(buf, 1, fileSize, fp);
    fclose(fp);
    buf[bytesRead] = '\0';
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    free(buf);
    if (err) {
      Serial.print(F("ERROR: Client metadata parse failed: "));
      Serial.println(err.c_str());
      return;
    }
    
    gClientMetadataCount = 0;
    
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
      if (gClientMetadataCount >= MAX_CLIENT_METADATA) break;
      
      ClientMetadata &meta = gClientMetadata[gClientMetadataCount++];
      memset(&meta, 0, sizeof(ClientMetadata));
      strlcpy(meta.clientUid, obj["c"] | "", sizeof(meta.clientUid));
      meta.vinVoltage = obj["v"] | 0.0f;
      meta.vinVoltageEpoch = obj["ve"] | 0.0;
      meta.latitude = obj["lat"] | 0.0f;
      meta.longitude = obj["lon"] | 0.0f;
      meta.locationEpoch = obj["le"] | 0.0;
      strlcpy(meta.nwsGridOffice, obj["go"] | "", sizeof(meta.nwsGridOffice));
      meta.nwsGridX = obj["gx"] | 0;
      meta.nwsGridY = obj["gy"] | 0;
      meta.nwsGridValid = (meta.nwsGridOffice[0] != '\0');
      strlcpy(meta.firmwareVersion, obj["fv"] | "", sizeof(meta.firmwareVersion));
      meta.signalBars = obj["sb"] | (int)-1;
      meta.signalRssi = obj["sr"] | (int)0;
      meta.signalRsrp = obj["sp"] | (int)0;
      meta.signalRsrq = obj["sq"] | (int)0;
      strlcpy(meta.signalRat, obj["sn"] | "", sizeof(meta.signalRat));
      meta.signalEpoch = obj["se"] | 0.0;
      strlcpy(meta.lastSystemAlarmType, obj["sa"] | "", sizeof(meta.lastSystemAlarmType));
      meta.lastSystemAlarmEpoch = obj["sae"] | 0.0;
      meta.cachedTemperatureF = TEMPERATURE_UNAVAILABLE;
      meta.staleAlertSent = false;
    }
    
    Serial.print(F("Client metadata loaded: "));
    Serial.print(gClientMetadataCount);
    Serial.println(F(" entries"));
  #endif
#endif
}

// ============================================================================
// Stale Client Alerting & Auto-Pruning
// ============================================================================

// Prune individual stale sensors within an active client. Called when a client
// has a mix of fresh and stale sensors, indicating that the client was
// reconfigured and old sensor records are orphaned. Only prunes sensors that
// have been stale for at least ORPHAN_SENSOR_PRUNE_SECONDS (default 72h).
static void pruneStaleOrphanSensors(const char *clientUid, double now) {
  // First, try config-based pruning (safe, reuses existing logic)
  pruneOrphanedSensorRecords(clientUid);

  // Then, handle sensors stale beyond the orphan threshold even without
  // config data (e.g., client was reflashed externally)
  // Safety check: ensure client still has at least one fresh sensor
  bool hasFresh = false;
  for (uint8_t t = 0; t < gSensorRecordCount; ++t) {
    if (strcmp(gSensorRecords[t].clientUid, clientUid) == 0) {
      if (gSensorRecords[t].lastUpdateEpoch > 0.0 &&
          (now - gSensorRecords[t].lastUpdateEpoch) < STALE_CLIENT_THRESHOLD_SECONDS) {
        hasFresh = true;
        break;
      }
    }
  }
  if (!hasFresh) return;  // All stale — handled by dead-client auto-removal

  uint8_t writeIdx = 0;
  uint8_t pruned = 0;
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    bool keep = true;
    if (strcmp(gSensorRecords[i].clientUid, clientUid) == 0) {
      double age = (gSensorRecords[i].lastUpdateEpoch > 0.0)
                       ? (now - gSensorRecords[i].lastUpdateEpoch)
                       : now;  // Never reported — treat as maximally stale
      if (age >= ORPHAN_SENSOR_PRUNE_SECONDS) {
        keep = false;
        pruned++;
        Serial.print(F("Auto-pruned stale orphan sensor: "));
        Serial.print(gSensorRecords[i].label);
        Serial.print(F(" #"));
        Serial.println(gSensorRecords[i].sensorIndex);
      }
    }
    if (keep) {
      if (writeIdx != i) {
        memcpy(&gSensorRecords[writeIdx], &gSensorRecords[i], sizeof(SensorRecord));
      }
      writeIdx++;
    }
  }

  if (pruned > 0) {
    gSensorRecordCount = writeIdx;
    rebuildSensorHashTable();
    gSensorRegistryDirty = true;

    char detail[96];
    snprintf(detail, sizeof(detail),
             "Auto-pruned %u stale orphan sensor(s) from active client %s",
             pruned, clientUid);
    Serial.println(detail);
    addServerSerialLog(detail, "info", "stale");
    logTransmission(clientUid, "", "stale", "pruned", detail);
  }
}

static void checkStaleClients() {
  double now = currentEpoch();
  if (now <= 0.0) return;  // No time sync yet

  // Proactive dedup: catch any duplicate records that slipped past the hash table.
  deduplicateSensorRecordsLinear();

  // Collect UIDs of clients to auto-remove after the main scan.
  // Deferring removal avoids modifying the metadata array during iteration.
  char clientsToRemove[MAX_CLIENT_METADATA][48];
  uint8_t removeCount = 0;

  for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
    ClientMetadata &meta = gClientMetadata[i];
    if (meta.clientUid[0] == '\0') continue;

    // --- Phase 1: Analyze per-sensor freshness for this client ---
    double latestUpdate = 0.0;
    const char *siteName = "";
    uint8_t totalSensors = 0;
    uint8_t staleSensors = 0;  // Stale beyond ORPHAN threshold (72h)

    for (uint8_t t = 0; t < gSensorRecordCount; ++t) {
      if (strcmp(gSensorRecords[t].clientUid, meta.clientUid) == 0) {
        totalSensors++;
        if (gSensorRecords[t].lastUpdateEpoch > latestUpdate) {
          latestUpdate = gSensorRecords[t].lastUpdateEpoch;
          siteName = gSensorRecords[t].site;
        }
        double age = (gSensorRecords[t].lastUpdateEpoch > 0.0)
                         ? (now - gSensorRecords[t].lastUpdateEpoch)
                         : now;
        if (age >= ORPHAN_SENSOR_PRUNE_SECONDS) {
          staleSensors++;
        }
      }
    }

    if (totalSensors == 0 || latestUpdate <= 0.0) continue;

    double offlineSeconds = now - latestUpdate;

    // --- Phase 2: Per-sensor orphan pruning within active clients ---
    // If some sensors are fresh and some are stale beyond the orphan
    // threshold, the stale ones are likely orphans from a reconfiguration.
    if (staleSensors > 0 && staleSensors < totalSensors) {
      pruneStaleOrphanSensors(meta.clientUid, now);
    }

    // --- Phase 3: Existing stale client SMS alerting ---
    if (offlineSeconds >= STALE_CLIENT_THRESHOLD_SECONDS) {
      if (!meta.staleAlertSent) {
        char message[160];
        double hours = offlineSeconds / 3600.0;
        snprintf(message, sizeof(message),
                 "Client stale: %s (%s) - no data for %.1f hours.",
                 siteName, meta.clientUid, hours);
        sendSmsAlert(message);
        addServerSerialLog(message, "warn", "stale");
        Serial.print(F("Stale alert sent for client: "));
        Serial.println(meta.clientUid);
        meta.staleAlertSent = true;
      }

      // --- Phase 4: Auto-remove fully stale clients after extended period ---
      if (STALE_CLIENT_PRUNE_SECONDS > 0 &&
          offlineSeconds >= STALE_CLIENT_PRUNE_SECONDS &&
          removeCount < MAX_CLIENT_METADATA) {
        strlcpy(clientsToRemove[removeCount], meta.clientUid, 48);
        removeCount++;
      }
    } else {
      // Client is reporting again — reset stale alert flag
      if (meta.staleAlertSent) {
        meta.staleAlertSent = false;
        // BugFix v1.6.2 (M-5): Notify operator that a previously-stale client has recovered.
        char recoveryMsg[160];
        snprintf(recoveryMsg, sizeof(recoveryMsg),
                 "Client recovered: %s (%s) - reporting again.",
                 siteName, meta.clientUid);
        sendSmsAlert(recoveryMsg);
        addServerSerialLog(recoveryMsg, "info", "stale");
        Serial.print(F("Stale alert cleared for client: "));
        Serial.println(meta.clientUid);
      }
    }
  }

  // --- Phase 5: Execute deferred client removals ---
  for (uint8_t r = 0; r < removeCount; ++r) {
    char logMsg[128];
    snprintf(logMsg, sizeof(logMsg),
             "Auto-removed stale client %s (no data for >%lu days)",
             clientsToRemove[r],
             (unsigned long)(STALE_CLIENT_PRUNE_SECONDS / 86400UL));
    Serial.println(logMsg);
    addServerSerialLog(logMsg, "warn", "stale");
    logTransmission(clientsToRemove[r], "", "stale", "auto-removed", logMsg);

    archiveClientToFtp(clientsToRemove[r]);  // Archive to FTP before removal (if eligible)
    removeClientData(clientsToRemove[r]);
  }

  // If any clients were removed, force persistence save
  if (removeCount > 0) {
    saveSensorRegistry();
    saveClientMetadataCache();
    gSensorRegistryDirty = false;
    gClientMetadataDirty = false;
  }
}

// ============================================================================
// Orphaned Sensor Record Pruning
// ============================================================================
// When a client's config changes (e.g., sensors removed), stale SensorRecords
// for removed sensor indices persist in the registry. This function parses the
// cached config payload, extracts valid sensor indices, and removes any
// SensorRecords whose sensorIndex is no longer in the active config.

static void pruneOrphanedSensorRecords(const char *clientUid) {
  ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
  if (!snap || snap->payload[0] == '\0') return;

  // Parse the cached config JSON to extract valid sensor indices
  JsonDocument cfgDoc;
  if (deserializeJson(cfgDoc, snap->payload) != DeserializationError::Ok) {
    return;  // Can't parse — skip pruning
  }

  JsonArray sensors = cfgDoc["sensors"].as<JsonArray>();
  if (!sensors) return;  // No sensors array — skip

  // Build a set of valid sensor indices from the config
  // MAX_SENSOR_RECORDS is the upper bound; typical configs have 1-8 sensors
  uint8_t validNumbers[MAX_SENSOR_RECORDS];
  uint8_t validCount = 0;
  for (JsonVariant t : sensors) {
    uint8_t num = t["number"] | 0;
    if (num > 0 && validCount < MAX_SENSOR_RECORDS) {
      validNumbers[validCount++] = num;
    }
  }

  if (validCount == 0) return;  // Config has no numbered sensors — don't prune

  // Remove sensor records for this client whose sensorIndex is not in the valid set
  uint8_t writeIdx = 0;
  uint8_t pruned = 0;
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    bool keep = true;
    if (strcmp(gSensorRecords[i].clientUid, clientUid) == 0) {
      // Check if this sensor index is still in the config
      bool found = false;
      for (uint8_t v = 0; v < validCount; ++v) {
        if (gSensorRecords[i].sensorIndex == validNumbers[v]) {
          found = true;
          break;
        }
      }
      if (!found) {
        keep = false;
        pruned++;
        Serial.print(F("Pruned orphaned sensor record: "));
        Serial.print(gSensorRecords[i].label);
        Serial.print(F(" #"));
        Serial.println(gSensorRecords[i].sensorIndex);
      }
    }
    if (keep) {
      if (writeIdx != i) {
        memcpy(&gSensorRecords[writeIdx], &gSensorRecords[i], sizeof(SensorRecord));
      }
      writeIdx++;
    }
  }

  if (pruned > 0) {
    gSensorRecordCount = writeIdx;
    rebuildSensorHashTable();
    gSensorRegistryDirty = true;

    char detail[64];
    snprintf(detail, sizeof(detail), "Pruned %u orphaned sensor(s) after config update", pruned);
    Serial.println(detail);
    addServerSerialLog(detail, "info", "config");
    logTransmission(clientUid, snap->site, "config", "pruned", detail);
  }
}

// ============================================================================
// Config ACK Handler
// ============================================================================

static void handleConfigAck(JsonDocument &doc, double epoch) {
  const char *clientUid = doc["c"] | "";
  if (!clientUid || strlen(clientUid) == 0) return;
  
  const char *version = doc["cv"] | "";
  const char *status = doc["st"] | "unknown";
  
  ClientConfigSnapshot *snap = findClientConfigSnapshot(clientUid);
  if (!snap) {
    Serial.print(F("Config ACK from unknown client: "));
    Serial.println(clientUid);
    return;
  }
  
  // Update ACK tracking fields
  snap->lastAckEpoch = (epoch > 0.0) ? epoch : currentEpoch();
  strlcpy(snap->lastAckStatus, status, sizeof(snap->lastAckStatus));
  
  // If config version matches, clear pending flag
  if (version[0] != '\0' && strcmp(version, snap->configVersion) == 0) {
    snap->pendingDispatch = false;
    snap->dispatchAttempts = 0;  // Reset retry counter on successful delivery
  }
  
  Serial.print(F("Config ACK received from "));
  Serial.print(clientUid);
  Serial.print(F(": status="));
  Serial.print(status);
  if (version[0] != '\0') {
    Serial.print(F(", version="));
    Serial.print(version);
  }
  Serial.println();
  
  // Prune orphaned sensor records when config is successfully applied
  if (strcmp(status, "applied") == 0) {
    pruneOrphanedSensorRecords(clientUid);
  }

  addServerSerialLog("Config ACK received", "info", "config");
  saveClientConfigSnapshots();
}

// ============================================================================
// Client Data FTP Archive
// ============================================================================
// Archives a client's sensor records and hot-tier history to FTP before removal.
// Only archives if: FTP is enabled, client has been active for >30 days, and
// has at least one sensor record. The archive filename includes the site name
// and date range (first seen → last update) so archived entries are uniquely
// identifiable even if a new client with the same name is created later.
// File path: {ftpPath}/archived_clients/{Site}_{YYYYMM}-{YYYYMM}_{uid_suffix}.json

static bool archiveClientToFtp(const char *clientUid) {
  if (!gConfig.ftpEnabled) return false;

  // Gather all sensor records for this client
  double earliestSeen = 1e18;
  double latestUpdate = 0.0;
  const char *siteName = "";
  uint8_t sensorCount = 0;

  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    if (strcmp(gSensorRecords[i].clientUid, clientUid) != 0) continue;
    sensorCount++;
    if (gSensorRecords[i].site[0] != '\0') siteName = gSensorRecords[i].site;
    double fs = gSensorRecords[i].firstSeenEpoch;
    if (fs <= 0.0) fs = gSensorRecords[i].lastUpdateEpoch;  // Fallback for pre-upgrade records
    if (fs < earliestSeen) earliestSeen = fs;
    if (gSensorRecords[i].lastUpdateEpoch > latestUpdate) {
      latestUpdate = gSensorRecords[i].lastUpdateEpoch;
    }
  }
  if (sensorCount == 0 || latestUpdate <= 0.0) return false;

  // Only archive clients active for at least MIN_ARCHIVE_AGE_SECONDS
  double clientAge = latestUpdate - earliestSeen;
  if (clientAge < (double)MIN_ARCHIVE_AGE_SECONDS) {
    Serial.print(F("Skipping FTP archive (too new): "));
    Serial.println(clientUid);
    return false;
  }

  // Build date strings for filename (YYYYMM format)
  char startYM[8], endYM[8];
  {
    time_t startT = (time_t)earliestSeen;
    time_t endT = (time_t)latestUpdate;
    struct tm *tmStart = gmtime(&startT);
    snprintf(startYM, sizeof(startYM), "%04d%02d", tmStart->tm_year + 1900, tmStart->tm_mon + 1);
    struct tm *tmEnd = gmtime(&endT);
    snprintf(endYM, sizeof(endYM), "%04d%02d", tmEnd->tm_year + 1900, tmEnd->tm_mon + 1);
  }

  // Extract short UID suffix for filename (last 8 chars of clientUid)
  const char *uidSuffix = clientUid;
  size_t uidLen = strlen(clientUid);
  if (uidLen > 8) uidSuffix = clientUid + uidLen - 8;

  // Build JSON archive document
  JsonDocument doc;
  doc["clientUid"] = clientUid;
  doc["site"] = siteName;
  doc["archiveEpoch"] = currentEpoch();

  // Human-readable date range label: "Site (Mar 2025 - Mar 2026)"
  {
    const char *monthNames[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t startT = (time_t)earliestSeen;
    time_t endT = (time_t)latestUpdate;
    struct tm *tsStart = gmtime(&startT);
    int sy = tsStart->tm_year + 1900, sm = tsStart->tm_mon;
    struct tm *tsEnd = gmtime(&endT);
    int ey = tsEnd->tm_year + 1900, em = tsEnd->tm_mon;
    char rangeLabel[64];
    snprintf(rangeLabel, sizeof(rangeLabel), "%s (%s %d - %s %d)",
             siteName, monthNames[sm], sy, monthNames[em], ey);
    doc["displayLabel"] = rangeLabel;
  }
  doc["firstSeenEpoch"] = earliestSeen;
  doc["lastUpdateEpoch"] = latestUpdate;

  // Archive all sensor records
  JsonArray sensorsArr = doc["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    if (strcmp(gSensorRecords[i].clientUid, clientUid) != 0) continue;
    const SensorRecord &rec = gSensorRecords[i];
    JsonObject obj = sensorsArr.add<JsonObject>();
    obj["sensorIndex"] = rec.sensorIndex;
    if (rec.userNumber > 0) obj["userNumber"] = rec.userNumber;
    obj["site"] = rec.site;
    obj["label"] = rec.label;
    if (rec.contents[0] != '\0') obj["contents"] = rec.contents;
    if (rec.objectType[0] != '\0') obj["objectType"] = rec.objectType;
    if (rec.sensorType[0] != '\0') obj["sensorType"] = rec.sensorType;
    if (rec.measurementUnit[0] != '\0') obj["measurementUnit"] = rec.measurementUnit;
    obj["lastLevel"] = roundTo(rec.levelInches, 1);
    if (rec.sensorMa >= 4.0f) obj["lastSensorMa"] = roundTo(rec.sensorMa, 2);
    if (rec.sensorVoltage > 0.0f) obj["lastSensorVoltage"] = roundTo(rec.sensorVoltage, 3);
    obj["lastUpdateEpoch"] = rec.lastUpdateEpoch;
    double fs = rec.firstSeenEpoch > 0.0 ? rec.firstSeenEpoch : rec.lastUpdateEpoch;
    obj["firstSeenEpoch"] = fs;
  }

  // Archive hot-tier history snapshots for this client
  JsonArray historyArr = doc["history"].to<JsonArray>();
  for (uint8_t h = 0; h < gSensorHistoryCount; ++h) {
    SensorHourlyHistory &hist = gSensorHistory[h];
    if (strcmp(hist.clientUid, clientUid) != 0) continue;
    if (hist.snapshotCount == 0) continue;

    JsonObject hObj = historyArr.add<JsonObject>();
    hObj["sensorIndex"] = hist.sensorIndex;
    hObj["heightInches"] = hist.heightInches;
    JsonArray readings = hObj["readings"].to<JsonArray>();
    for (uint16_t j = 0; j < hist.snapshotCount; ++j) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      TelemetrySnapshot &snap = hist.snapshots[idx];
      JsonArray pt = readings.add<JsonArray>();
      pt.add(snap.timestamp);
      pt.add(roundTo(snap.level, 1));
      pt.add(roundTo(snap.voltage, 2));
    }
  }

  // Archive client metadata if available
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (meta) {
    JsonObject metaObj = doc["metadata"].to<JsonObject>();
    if (meta->vinVoltage > 0.0f) metaObj["vinVoltage"] = roundTo(meta->vinVoltage, 2);
    if (meta->firmwareVersion[0] != '\0') metaObj["firmwareVersion"] = meta->firmwareVersion;
    if (meta->latitude != 0.0f) metaObj["latitude"] = meta->latitude;
    if (meta->longitude != 0.0f) metaObj["longitude"] = meta->longitude;
    if (meta->signalBars >= 0) metaObj["signalBars"] = meta->signalBars;
  }

  // Serialize to string
  String jsonOut;
  serializeJson(doc, jsonOut);

  // Build sanitized site name for filename (replace spaces/special chars)
  char safeSite[24];
  {
    size_t si = 0;
    for (size_t c = 0; siteName[c] != '\0' && si < sizeof(safeSite) - 1; ++c) {
      char ch = siteName[c];
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
        safeSite[si++] = ch;
      } else if (ch == ' ') {
        safeSite[si++] = '_';
      }
    }
    safeSite[si] = '\0';
    if (si == 0) strlcpy(safeSite, "unknown", sizeof(safeSite));
  }

  // Upload to FTP: {ftpPath}/{serverUid}/archived_clients/{safeSite}_{startYM}-{endYM}_{uidSuffix}.json
  char remotePath[256];
  const char *base = (strlen(gConfig.ftpPath) > 0) ? gConfig.ftpPath : FTP_PATH_DEFAULT;
  const char *uid = (strlen(gServerUid) > 0) ? gServerUid : "server";
  snprintf(remotePath, sizeof(remotePath), "%s/%s/archived_clients/%s_%s-%s_%s.json",
           base, uid, safeSite, startYM, endYM, uidSuffix);

  const bool useFtps = gConfig.ftpsEnabled;
  FtpSession session;
  char err[128];
  bool connected = useFtps ? ftpsConnectAndLogin(err, sizeof(err))
                           : ftpConnectAndLogin(session, err, sizeof(err));
  if (!connected) {
    Serial.print(F("FTP archive client failed (login): "));
    Serial.println(err);
    return false;
  }

  bool ok = useFtps
              ? ftpsStoreBuffer(remotePath,
                                (const uint8_t *)jsonOut.c_str(), jsonOut.length(),
                                err, sizeof(err))
              : ftpStoreBuffer(session, remotePath,
                               (const uint8_t *)jsonOut.c_str(), jsonOut.length(),
                               err, sizeof(err));
  if (useFtps) {
    ftpsQuit();
  } else {
    ftpQuit(session);
  }

  if (ok) {
    char detail[128];
    snprintf(detail, sizeof(detail), "Archived client %s to FTP: %s", clientUid, remotePath);
    Serial.println(detail);
    addServerSerialLog(detail, "info", "archive");
    logTransmission(clientUid, siteName, "archive", "sent", detail);

    // Append entry to local archive manifest (/fs/archived_clients.json)
    #ifdef FILESYSTEM_AVAILABLE
    {
      // Read existing manifest
      JsonDocument manifest;
      FILE *mf = fopen("/fs/archived_clients.json", "r");
      if (mf) {
        char buf[2048];
        size_t nRead = fread(buf, 1, sizeof(buf) - 1, mf);
        fclose(mf);
        buf[nRead] = '\0';
        deserializeJson(manifest, buf);
      }
      JsonArray entries = manifest["archives"].is<JsonArray>()
                            ? manifest["archives"].as<JsonArray>()
                            : manifest["archives"].to<JsonArray>();
      JsonObject entry = entries.add<JsonObject>();
      entry["clientUid"] = clientUid;
      entry["site"] = siteName;
      entry["displayLabel"] = doc["displayLabel"];
      entry["firstSeenEpoch"] = earliestSeen;
      entry["lastUpdateEpoch"] = latestUpdate;
      entry["archiveEpoch"] = currentEpoch();
      entry["ftpFile"] = remotePath;
      entry["sensorCount"] = sensorCount;

      // Write updated manifest atomically (write to temp, rename)
      mf = fopen("/fs/archived_clients.json.tmp", "w");
      if (mf) {
        String manifestJson;
        serializeJson(manifest, manifestJson);
        fwrite(manifestJson.c_str(), 1, manifestJson.length(), mf);
        fclose(mf);
        remove("/fs/archived_clients.json");
        rename("/fs/archived_clients.json.tmp", "/fs/archived_clients.json");
      }
    }
    #endif
  } else {
    Serial.print(F("FTP archive client failed (store): "));
    Serial.println(err);
  }

  return ok;
}

// ============================================================================
// Client Data Removal Helper
// ============================================================================
// Removes all data for a given client: sensor records, metadata, config
// snapshots, and serial buffers. Used by both the manual "Remove Client"
// API and the automatic stale-client pruning system. Returns true if any
// data was removed.

static bool removeClientData(const char *clientUid) {
  bool removedAny = false;

  // Remove sensor records for this client
  uint8_t writeIdx = 0;
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    if (strcmp(gSensorRecords[i].clientUid, clientUid) != 0) {
      if (writeIdx != i) {
        memcpy(&gSensorRecords[writeIdx], &gSensorRecords[i], sizeof(SensorRecord));
      }
      writeIdx++;
    } else {
      removedAny = true;
    }
  }
  if (writeIdx != gSensorRecordCount) {
    gSensorRecordCount = writeIdx;
    rebuildSensorHashTable();
    gSensorRegistryDirty = true;
  }

  // Remove client metadata
  for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
    if (strcmp(gClientMetadata[i].clientUid, clientUid) == 0) {
      for (uint8_t j = i; j < gClientMetadataCount - 1; ++j) {
        memcpy(&gClientMetadata[j], &gClientMetadata[j + 1], sizeof(ClientMetadata));
      }
      gClientMetadataCount--;
      gClientMetadataDirty = true;
      removedAny = true;
      break;
    }
  }

  // Remove client config snapshot
  for (uint8_t i = 0; i < gClientConfigCount; ++i) {
    if (strcmp(gClientConfigs[i].uid, clientUid) == 0) {
      for (uint8_t j = i; j < gClientConfigCount - 1; ++j) {
        memcpy(&gClientConfigs[j], &gClientConfigs[j + 1], sizeof(ClientConfigSnapshot));
      }
      gClientConfigCount--;
      saveClientConfigSnapshots();
      removedAny = true;
      break;
    }
  }

  // Remove client serial buffer
  for (uint8_t i = 0; i < gClientSerialBufferCount; ++i) {
    if (strcmp(gClientSerialBuffers[i].clientUid, clientUid) == 0) {
      for (uint8_t j = i; j < gClientSerialBufferCount - 1; ++j) {
        memcpy(&gClientSerialBuffers[j], &gClientSerialBuffers[j + 1], sizeof(ClientSerialBuffer));
      }
      gClientSerialBufferCount--;
      break;
    }
  }

  return removedAny;
}

// ============================================================================
// Client Removal (DELETE /api/client)
// ============================================================================

static void handleClientDeleteRequest(EthernetClient &client, const String &body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }
  
  const char *pin = doc["pin"] | "";
  if (!requireValidPin(client, pin)) {
    return;
  }
  
  const char *clientUid = doc["client"] | "";
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, F("Missing client UID"));
    return;
  }
  
  archiveClientToFtp(clientUid);  // Archive to FTP before removal (if eligible)
  bool removedAny = removeClientData(clientUid);

  if (removedAny) {
    char logMsg[80];
    snprintf(logMsg, sizeof(logMsg), "Client removed: %s", clientUid);
    addServerSerialLog(logMsg, "warn", "admin");
    Serial.print(F("Client removed: "));
    Serial.println(clientUid);
    
    // Force immediate persistence save
    saveSensorRegistry();
    saveClientMetadataCache();
    gSensorRegistryDirty = false;
    gClientMetadataDirty = false;
    
    respondStatus(client, 200, F("Client removed"));
  } else {
    respondStatus(client, 404, F("Client not found"));
  }
}

static void loadClientConfigSnapshots() {
  gClientConfigCount = 0;
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    
    FILE *file = fopen("/fs/client_config_cache.txt", "r");
    if (!file) {
      return;
    }
    
    // Buffer size: uid + tab + payload + extended fields + newline + null terminator
    // Extended fields: \tpd\tconfigVersion\tepoch\tackStatus\tdispatchAttempts\tdispatchEpoch
    // Account for all struct field sizes plus tab separators to avoid silent truncation
    static const size_t EXTENDED_FIELDS_MAX = 1 + 1 + 1 + 16 + 1 + 14 + 1 + 16 + 1 + 3 + 1 + 14;  // ~70 bytes
    char lineBuffer[sizeof(((ClientConfigSnapshot*)0)->uid) + 1 + sizeof(((ClientConfigSnapshot*)0)->payload) + EXTENDED_FIELDS_MAX + 2];
    while (fgets(lineBuffer, sizeof(lineBuffer), file) != nullptr && gClientConfigCount < MAX_CLIENT_CONFIG_SNAPSHOTS) {
      // Check if line was truncated (no newline at end of non-empty buffer)
      size_t buflen = strlen(lineBuffer);
      if (buflen == sizeof(lineBuffer) - 1 && lineBuffer[sizeof(lineBuffer) - 2] != '\n') {
        Serial.println(F("Warning: truncated line in client config cache"));
        // Skip the rest of the truncated line
        int c;
        while ((c = fgetc(file)) != '\n' && c != EOF) { }
        continue;
      }
      String line = String(lineBuffer);
      line.trim();
      if (line.length() == 0) {
        continue;
      }
      int sep = line.indexOf('\t');
      if (sep <= 0) {
        continue;
      }

      String uid = line.substring(0, sep);
      String json = line.substring(sep + 1);

      ClientConfigSnapshot &snap = gClientConfigs[gClientConfigCount++];
      memset(&snap, 0, sizeof(ClientConfigSnapshot));
      strlcpy(snap.uid, uid.c_str(), sizeof(snap.uid));

      size_t len = json.length();
      if (len >= sizeof(snap.payload)) {
        len = sizeof(snap.payload) - 1;
      }
      memcpy(snap.payload, json.c_str(), len);
      snap.payload[len] = '\0';

      JsonDocument doc;
      if (deserializeJson(doc, snap.payload) == DeserializationError::Ok) {
        const char *site = doc["site"] | "";
        strlcpy(snap.site, site, sizeof(snap.site));
      } else {
        snap.site[0] = '\0';
      }
      
      // Parse extended fields (pendingDispatch, configVersion, lastAckEpoch, lastAckStatus, dispatchAttempts, lastDispatchEpoch)
      // These are appended as additional tab-separated fields after the payload
      int sep2 = json.indexOf('\t');
      if (sep2 > 0) {
        // Trim payload to remove extended fields
        snap.payload[sep2] = '\0';
        String extended = json.substring(sep2 + 1);
        // Format: pendingDispatch\tconfigVersion\tlastAckEpoch\tlastAckStatus[\tdispatchAttempts\tlastDispatchEpoch]
        int s1 = extended.indexOf('\t');
        if (s1 >= 0) {
          snap.pendingDispatch = (extended.substring(0, s1) == "1");
          String rest = extended.substring(s1 + 1);
          int s2 = rest.indexOf('\t');
          if (s2 >= 0) {
            strlcpy(snap.configVersion, rest.substring(0, s2).c_str(), sizeof(snap.configVersion));
            String rest2 = rest.substring(s2 + 1);
            int s3 = rest2.indexOf('\t');
            if (s3 >= 0) {
              snap.lastAckEpoch = rest2.substring(0, s3).toDouble();
              String rest3 = rest2.substring(s3 + 1);
              // Check for dispatchAttempts and lastDispatchEpoch (v3.1+ format)
              int s4 = rest3.indexOf('\t');
              if (s4 >= 0) {
                strlcpy(snap.lastAckStatus, rest3.substring(0, s4).c_str(), sizeof(snap.lastAckStatus));
                String rest4 = rest3.substring(s4 + 1);
                int s5 = rest4.indexOf('\t');
                if (s5 >= 0) {
                  snap.dispatchAttempts = (uint8_t)rest4.substring(0, s5).toInt();
                  snap.lastDispatchEpoch = rest4.substring(s5 + 1).toDouble();
                } else {
                  snap.dispatchAttempts = (uint8_t)rest4.toInt();
                }
              } else {
                // Old format: no dispatch fields, rest3 is just lastAckStatus
                strlcpy(snap.lastAckStatus, rest3.c_str(), sizeof(snap.lastAckStatus));
              }
            }
          }
        }
      }
    }

    fclose(file);
  #else
    if (!LittleFS.exists(CLIENT_CONFIG_CACHE_PATH)) {
      return;
    }

    File file = LittleFS.open(CLIENT_CONFIG_CACHE_PATH, "r");
    if (!file) {
      return;
    }

    while (file.available() && gClientConfigCount < MAX_CLIENT_CONFIG_SNAPSHOTS) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) {
        continue;
      }
      int sep = line.indexOf('\t');
      if (sep <= 0) {
        continue;
      }

      String uid = line.substring(0, sep);
      String json = line.substring(sep + 1);

      ClientConfigSnapshot &snap = gClientConfigs[gClientConfigCount++];
      memset(&snap, 0, sizeof(ClientConfigSnapshot));
      strlcpy(snap.uid, uid.c_str(), sizeof(snap.uid));

      size_t len = json.length();
      if (len >= sizeof(snap.payload)) {
        len = sizeof(snap.payload) - 1;
      }
      memcpy(snap.payload, json.c_str(), len);
      snap.payload[len] = '\0';

      JsonDocument doc;
      if (deserializeJson(doc, snap.payload) == DeserializationError::Ok) {
        const char *site = doc["site"] | "";
        strlcpy(snap.site, site, sizeof(snap.site));
      } else {
        snap.site[0] = '\0';
      }
      
      // Parse extended fields (pendingDispatch, configVersion, lastAckEpoch, lastAckStatus, dispatchAttempts, lastDispatchEpoch)
      int sep2 = json.indexOf('\t');
      if (sep2 > 0) {
        snap.payload[sep2] = '\0';
        String extended = json.substring(sep2 + 1);
        int s1 = extended.indexOf('\t');
        if (s1 >= 0) {
          snap.pendingDispatch = (extended.substring(0, s1) == "1");
          String rest = extended.substring(s1 + 1);
          int s2 = rest.indexOf('\t');
          if (s2 >= 0) {
            strlcpy(snap.configVersion, rest.substring(0, s2).c_str(), sizeof(snap.configVersion));
            String rest2 = rest.substring(s2 + 1);
            int s3 = rest2.indexOf('\t');
            if (s3 >= 0) {
              snap.lastAckEpoch = rest2.substring(0, s3).toDouble();
              String rest3 = rest2.substring(s3 + 1);
              // Check for dispatchAttempts and lastDispatchEpoch (v3.1+ format)
              int s4 = rest3.indexOf('\t');
              if (s4 >= 0) {
                strlcpy(snap.lastAckStatus, rest3.substring(0, s4).c_str(), sizeof(snap.lastAckStatus));
                String rest4 = rest3.substring(s4 + 1);
                int s5 = rest4.indexOf('\t');
                if (s5 >= 0) {
                  snap.dispatchAttempts = (uint8_t)rest4.substring(0, s5).toInt();
                  snap.lastDispatchEpoch = rest4.substring(s5 + 1).toDouble();
                } else {
                  snap.dispatchAttempts = (uint8_t)rest4.toInt();
                }
              } else {
                // Old format: no dispatch fields, rest3 is just lastAckStatus
                strlcpy(snap.lastAckStatus, rest3.c_str(), sizeof(snap.lastAckStatus));
              }
            }
          }
        }
      }
    }

    file.close();
  #endif
#endif
}

static void saveClientConfigSnapshots() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) {
      return;
    }
    
    // Accumulate output into a buffer first, then write atomically.
    // Each line: uid(48) + tab + payload(1536) + tab + flag(1) + tab
    //            + version(16) + tab + epoch(14) + tab + status(16)
    //            + tab + dispatchAttempts(3) + tab + dispatchEpoch(14) + newline
    // ≈ 1660 bytes per entry. Reserve generously.
    const size_t bufSize = (size_t)gClientConfigCount * 1700 + 64;
    char *buf = (char *)malloc(bufSize);
    if (!buf) {
      Serial.println(F("ERROR: Cannot allocate config cache buffer"));
      return;
    }

    size_t pos = 0;
    for (uint8_t i = 0; i < gClientConfigCount; ++i) {
      // Format: uid\tpayload\tpendingDispatch\tconfigVersion\tlastAckEpoch\tlastAckStatus\tdispatchAttempts\tlastDispatchEpoch
      int n = snprintf(buf + pos, bufSize - pos, "%s\t%s\t%d\t%s\t%.0f\t%s\t%u\t%.0f\n",
                       gClientConfigs[i].uid,
                       gClientConfigs[i].payload,
                       gClientConfigs[i].pendingDispatch ? 1 : 0,
                       gClientConfigs[i].configVersion,
                       gClientConfigs[i].lastAckEpoch,
                       gClientConfigs[i].lastAckStatus,
                       (unsigned)gClientConfigs[i].dispatchAttempts,
                       gClientConfigs[i].lastDispatchEpoch);
      if (n < 0 || pos + (size_t)n >= bufSize) {
        Serial.println(F("Config cache buffer overflow"));
        break;
      }
      pos += (size_t)n;
    }

    if (!serverWriteFileAtomic("/fs/client_config_cache.txt", buf, pos)) {
      Serial.println(F("Failed to write client config cache"));
    }
    free(buf);
  #else
    // LittleFS branch: accumulate into String, then atomic write
    String output;
    output.reserve(gClientConfigCount * 1700);
    for (uint8_t i = 0; i < gClientConfigCount; ++i) {
      // Format: uid\tpayload\tpendingDispatch\tconfigVersion\tlastAckEpoch\tlastAckStatus\tdispatchAttempts\tlastDispatchEpoch
      char line[1700];
      snprintf(line, sizeof(line), "%s\t%s\t%d\t%s\t%lu\t%s\t%u\t%.0f\n",
               gClientConfigs[i].uid,
               gClientConfigs[i].payload,
               gClientConfigs[i].pendingDispatch ? 1 : 0,
               gClientConfigs[i].configVersion,
               (unsigned long)gClientConfigs[i].lastAckEpoch,
               gClientConfigs[i].lastAckStatus,
               (unsigned)gClientConfigs[i].dispatchAttempts,
               gClientConfigs[i].lastDispatchEpoch);
      output += line;
    }
    tankalarm_littlefs_write_file_atomic(CLIENT_CONFIG_CACHE_PATH,
            (const uint8_t *)output.c_str(), output.length());
  #endif
#endif
}

static bool cacheClientConfigFromBuffer(const char *clientUid, const char *buffer) {
  if (!clientUid || !buffer) {
    return false;
  }

  // Validate buffer length before processing
  size_t bufferLen = strlen(buffer);
  if (bufferLen == 0 || bufferLen >= sizeof(((ClientConfigSnapshot*)0)->payload)) {
    Serial.print(F("Config too large for cache: "));
    Serial.print(bufferLen);
    Serial.print(F(" bytes (max "));
    Serial.print(sizeof(((ClientConfigSnapshot*)0)->payload) - 1);
    Serial.println(F(")"));
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, buffer) != DeserializationError::Ok) {
    return false;
  }

  ClientConfigSnapshot *snapshot = findClientConfigSnapshot(clientUid);
  if (!snapshot) {
    if (gClientConfigCount >= MAX_CLIENT_CONFIG_SNAPSHOTS) {
      // Evict oldest non-pending config snapshot
      uint8_t evictIdx = 0xFF;
      for (uint8_t i = 0; i < gClientConfigCount; ++i) {
        if (!gClientConfigs[i].pendingDispatch) {
          evictIdx = i;
          break;
        }
      }
      if (evictIdx == 0xFF) evictIdx = 0;  // If all pending, evict first
      
      Serial.print(F("WARNING: Config cache full - evicting: "));
      Serial.println(gClientConfigs[evictIdx].uid);
      addServerSerialLog("Config snapshot evicted (capacity full)", "warn", "config");
      
      // Shift entries down to remove evicted slot
      for (uint8_t j = evictIdx; j < gClientConfigCount - 1; ++j) {
        memcpy(&gClientConfigs[j], &gClientConfigs[j + 1], sizeof(ClientConfigSnapshot));
      }
      gClientConfigCount--;
    }
    snapshot = &gClientConfigs[gClientConfigCount++];
    memset(snapshot, 0, sizeof(ClientConfigSnapshot));
    strlcpy(snapshot->uid, clientUid, sizeof(snapshot->uid));
  }

  const char *site = doc["site"] | "";
  strlcpy(snapshot->site, site, sizeof(snapshot->site));

  size_t len = bufferLen;
  if (len >= sizeof(snapshot->payload)) {
    len = sizeof(snapshot->payload) - 1;
  }
  memcpy(snapshot->payload, buffer, len);
  snapshot->payload[len] = '\0';

  saveClientConfigSnapshots();
  return true;
}

// ============================================================================
// Calibration Learning System Implementation
// ============================================================================



static void sendHistoryJson(EthernetClient &client, const String &query) {
  // Build JSON response with historical sensor data for charting
  // Supports query params: ?days=N (limit readings age), ?sensor=CLIENT:NUM (single sensor, full range)
  // When viewing all sensors, limit to 90 days to reduce payload; single sensor gets full range
  JsonDocument doc;
  // doc.isNull() removed; in ArduinoJson 7 it returns true for new/empty docs
  
  // Parse query parameters
  String sensorFilter = getQueryParam(query, "sensor");   // e.g. "dev:abc123:1"
  String daysParam = getQueryParam(query, "days");    // e.g. "90", "365", "730"
  int maxDays = 0;  // 0 = no limit
  if (daysParam.length() > 0) {
    maxDays = daysParam.toInt();
    if (maxDays <= 0) maxDays = 0;
  }
  
  // Default: 90 days for all-sensors overview, full range for single sensor
  double cutoffEpoch = 0.0;
  double nowEpoch = 0.0;
  
  JsonArray sensorsArray = doc["sensors"].to<JsonArray>();
  JsonArray alarmsArray = doc["alarms"].to<JsonArray>();
  JsonArray voltageArray = doc["voltage"].to<JsonArray>();
  
  // Get current epoch
  if (gLastSyncedEpoch > 0.0) {
    nowEpoch = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  
  // Calculate cutoff based on days parameter
  if (maxDays > 0 && nowEpoch > 0.0) {
    cutoffEpoch = nowEpoch - (double)maxDays * 86400.0;
  }
  
  // Use stored sensor history for trend data
  for (uint8_t h = 0; h < gSensorHistoryCount; h++) {
    SensorHourlyHistory &hist = gSensorHistory[h];
    if (hist.snapshotCount == 0) continue;
    
    // Apply single-sensor filter if specified (format: "clientUid:sensorIndex")
    if (sensorFilter.length() > 0) {
      int colonIdx = sensorFilter.lastIndexOf(':');
      if (colonIdx > 0) {
        String filterClient = sensorFilter.substring(0, colonIdx);
        int filterSensorIdx = sensorFilter.substring(colonIdx + 1).toInt();
        if (strcmp(hist.clientUid, filterClient.c_str()) != 0 || hist.sensorIndex != filterSensorIdx) {
          continue;
        }
      }
    }
    
    JsonObject sensorObj = sensorsArray.add<JsonObject>();
    sensorObj["client"] = hist.clientUid;
    sensorObj["site"] = strlen(hist.siteName) > 0 ? (const char*)hist.siteName : "Unknown Site";
    sensorObj["sensorIndex"] = hist.sensorIndex;
    sensorObj["label"] = "Sensor";  // Would get from clientData
    sensorObj["heightInches"] = hist.heightInches;
    
    // Find most recent level
    uint16_t latestIdx = (hist.writeIndex - 1 + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
    sensorObj["currentLevel"] = hist.snapshots[latestIdx].level;
    
    // Include readings, filtered by cutoff if set
    JsonArray readings = sensorObj["readings"].to<JsonArray>();
    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      TelemetrySnapshot &snap = hist.snapshots[idx];
      
      // Skip readings older than cutoff
      if (cutoffEpoch > 0.0 && snap.timestamp < cutoffEpoch) continue;
      
      JsonObject reading = readings.add<JsonObject>();
      reading["timestamp"] = snap.timestamp;
      reading["level"] = snap.level;
    }
    
    // Calculate 24h change
    // BugFix v1.6.2 (M-9): Interpolate between the two snapshots bracketing the
    // 24h-ago mark, instead of using the first snapshot inside the window.
    // This gives a much more accurate delta when snapshot intervals are wide.
    double yesterday = nowEpoch - 86400.0;
    float level24hAgo = hist.snapshots[latestIdx].level;  // fallback: no change
    {
      // Walk chronologically to find the pair bracketing 'yesterday'
      float prevLevel = 0.0f;
      double prevTime = 0.0;
      bool foundBracket = false;
      for (uint16_t j = 0; j < hist.snapshotCount; j++) {
        uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
        double t = hist.snapshots[idx].timestamp;
        float l = hist.snapshots[idx].level;
        if (t >= yesterday) {
          if (j > 0 && prevTime < yesterday && prevTime > 0.0) {
            // Interpolate between prev and current
            double frac = (yesterday - prevTime) / (t - prevTime);
            level24hAgo = prevLevel + (float)(frac * (l - prevLevel));
          } else {
            // No earlier snapshot — use the first one in the window
            level24hAgo = l;
          }
          foundBracket = true;
          break;
        }
        prevLevel = l;
        prevTime = t;
      }
      // If all snapshots are older than yesterday, use the most recent one
      if (!foundBracket && hist.snapshotCount > 0) {
        uint16_t lastIdx = (hist.writeIndex - 1 + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
        level24hAgo = hist.snapshots[lastIdx].level;
      }
    }
    sensorObj["change24h"] = roundTo(hist.snapshots[latestIdx].level - level24hAgo, 1);
  }
  
  // Add stored voltage history from sensor snapshots
  for (uint8_t h = 0; h < gSensorHistoryCount; h++) {
    SensorHourlyHistory &hist = gSensorHistory[h];
    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
      TelemetrySnapshot &snap = hist.snapshots[idx];
      if (cutoffEpoch > 0.0 && snap.timestamp < cutoffEpoch) continue;
      if (snap.voltage > 0) {
        JsonObject voltObj = voltageArray.add<JsonObject>();
        voltObj["timestamp"] = snap.timestamp;
        voltObj["voltage"] = snap.voltage;
        voltObj["client"] = hist.clientUid;
      }
    }
  }
  
  // Add recent alarms from ring buffer
  for (uint8_t i = 0; i < alarmLogCount; i++) {
    int idx = (alarmLogWriteIndex - alarmLogCount + i + MAX_ALARM_LOG_ENTRIES) % MAX_ALARM_LOG_ENTRIES;
    AlarmLogEntry &entry = alarmLog[idx];
    
    JsonObject alarmObj = alarmsArray.add<JsonObject>();
    alarmObj["timestamp"] = entry.timestamp;
    alarmObj["site"] = strlen(entry.siteName) > 0 ? (const char*)entry.siteName : "Unknown";
    alarmObj["clientUid"] = entry.clientUid;
    alarmObj["sensorIndex"] = entry.sensorIndex;
    alarmObj["type"] = entry.isHigh ? "HIGH" : "LOW";
    alarmObj["level"] = entry.level;
    alarmObj["cleared"] = entry.cleared;
    if (entry.cleared) {
      alarmObj["clearedAt"] = entry.clearedTimestamp;
    }
  }
  
  // Add history settings info
  JsonObject settings = doc["settings"].to<JsonObject>();
  settings["hotTierDays"] = gHistorySettings.hotTierRetentionDays;
  settings["warmTierMonths"] = gHistorySettings.warmTierRetentionMonths;
  settings["ftpArchiveEnabled"] = gHistorySettings.ftpArchiveEnabled && gConfig.ftpEnabled;
  settings["lastFtpSync"] = gHistorySettings.lastFtpSyncEpoch;
  settings["lastPrune"] = gHistorySettings.lastPruneEpoch;
  settings["totalPruned"] = gHistorySettings.totalRecordsPruned;
  
  // Data source and tier availability metadata
  JsonObject dataInfo = doc["dataInfo"].to<JsonObject>();
  dataInfo["source"] = "hot";  // Primary data from RAM ring buffer
  dataInfo["hotTierSnapshots"] = (gSensorHistoryCount > 0 && gSensorHistory[0].snapshotCount > 0);
  dataInfo["warmTierAvailable"] = (gHistorySettings.warmTierRetentionMonths > 0 && gWarmTierDataExists);
  dataInfo["coldTierAvailable"] = (gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled);
  
  // Indicate max available range without FTP
  int maxHotDays = gHistorySettings.hotTierRetentionDays;
  int maxWarmDays = gHistorySettings.warmTierRetentionMonths * 30;
  dataInfo["maxRangeWithoutFtp"] = maxHotDays + maxWarmDays;
  dataInfo["maxRangeLabel"] = (gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled)
                               ? "Unlimited (FTP archive)" : "~" + String(maxHotDays + maxWarmDays) + " days";
  
  // If requesting more data than hot tier holds and FTP is not available,
  // indicate limited data and suggest range reduction
  if (maxDays > maxHotDays && !(gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled)) {
    dataInfo["rangeNote"] = "Requested range exceeds hot tier. Daily summaries available for " +
                            String(gHistorySettings.warmTierRetentionMonths) + " months. Enable FTP for full archive.";
  }
  
  // Serialize and send
  String jsonOut;
  serializeJson(doc, jsonOut);
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(jsonOut.length());
  client.println(F("Cache-Control: no-cache"));
  client.println();
  client.print(jsonOut);
}

// Helper to parse YYYYMM format into year and month
static bool parseYearMonth(const char *str, int &year, int &month) {
  if (strlen(str) != 6) return false;
  char yearStr[5] = {str[0], str[1], str[2], str[3], '\0'};
  char monthStr[3] = {str[4], str[5], '\0'};
  year = atoi(yearStr);
  month = atoi(monthStr);
  return (year >= 2020 && year <= 2100 && month >= 1 && month <= 12);
}

// Helper to extract query parameter value
// Month-over-month comparison endpoint
// GET /api/history/compare?current=YYYYMM&previous=YYYYMM
static void handleHistoryCompare(EthernetClient &client, const String &query) {
  String currentStr = getQueryParam(query, "current");
  String previousStr = getQueryParam(query, "previous");
  
  int currYear, currMonth, prevYear, prevMonth;
  if (!parseYearMonth(currentStr.c_str(), currYear, currMonth) ||
      !parseYearMonth(previousStr.c_str(), prevYear, prevMonth)) {
    respondStatus(client, 400, F("Invalid date format. Use YYYYMM"));
    return;
  }
  
  // Build comparison JSON (~32KB; ArduinoJson v7 auto-sizes)
  JsonDocument doc;
  
  doc["current"]["year"] = currYear;
  doc["current"]["month"] = currMonth;
  doc["previous"]["year"] = prevYear;
  doc["previous"]["month"] = prevMonth;
  
  JsonArray comparisons = doc["sensors"].to<JsonArray>();
  
  // Determine current time for hot tier range check
  double nowEpoch = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    nowEpoch = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  // Pre-compute epoch boundaries for month filtering
  struct tm currStartTm = {0};
  currStartTm.tm_year = currYear - 1900;
  currStartTm.tm_mon = currMonth - 1;
  currStartTm.tm_mday = 1;
  double currMonthStart = (double)mktime(&currStartTm);
  struct tm currEndTm = {0};
  currEndTm.tm_year = (currMonth == 12) ? currYear - 1900 + 1 : currYear - 1900;
  currEndTm.tm_mon = (currMonth == 12) ? 0 : currMonth;
  currEndTm.tm_mday = 1;
  double currMonthEnd = (double)mktime(&currEndTm);

  struct tm prevStartTm = {0};
  prevStartTm.tm_year = prevYear - 1900;
  prevStartTm.tm_mon = prevMonth - 1;
  prevStartTm.tm_mday = 1;
  double prevMonthStart = (double)mktime(&prevStartTm);
  struct tm prevEndTm = {0};
  prevEndTm.tm_year = (prevMonth == 12) ? prevYear - 1900 + 1 : prevYear - 1900;
  prevEndTm.tm_mon = (prevMonth == 12) ? 0 : prevMonth;
  prevEndTm.tm_mday = 1;
  double prevMonthEnd = (double)mktime(&prevEndTm);

  // Hot tier covers last N days — check if requested months overlap retained data
  double hotTierCutoff = (nowEpoch > 0) ? nowEpoch - (double)gHistorySettings.hotTierRetentionDays * 86400.0 : 0;
  bool currInHotTier = (nowEpoch > 0 && currMonthEnd > hotTierCutoff && currMonthStart <= nowEpoch);
  bool prevInHotTier = (nowEpoch > 0 && prevMonthEnd > hotTierCutoff && prevMonthStart <= nowEpoch);
  
  // Try to load previous month from warm tier (LittleFS) or cold tier (FTP)
  bool prevFromWarm = false;
  bool prevFromCold = false;
  if (!prevInHotTier) {
    // First try LittleFS daily summaries (warm tier)
    JsonDocument warmDoc;
    prevFromWarm = loadDailySummaryMonth(prevYear, prevMonth, warmDoc);
    
    // If warm tier not available, try FTP archive (cold tier)
    if (!prevFromWarm && gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled) {
      prevFromCold = loadFtpArchiveCached(prevYear, prevMonth);
    }
  }

  for (uint8_t h = 0; h < gSensorHistoryCount; h++) {
    SensorHourlyHistory &hist = gSensorHistory[h];
    if (hist.snapshotCount == 0) continue;
    
    JsonObject sensorComp = comparisons.add<JsonObject>();
    sensorComp["client"] = hist.clientUid;
    sensorComp["site"] = hist.siteName;
    sensorComp["sensorIndex"] = hist.sensorIndex;
    
    // Current period stats from hot tier
    float currMin = 9999.0, currMax = -9999.0, currSum = 0.0;
    int currCount = 0;
    
    if (currInHotTier) {
      for (uint16_t j = 0; j < hist.snapshotCount; j++) {
        uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
        TelemetrySnapshot &snap = hist.snapshots[idx];
        if (snap.timestamp < currMonthStart || snap.timestamp >= currMonthEnd) continue;
        if (snap.level < currMin) currMin = snap.level;
        if (snap.level > currMax) currMax = snap.level;
        currSum += snap.level;
        currCount++;
      }
    }
    
    JsonObject currStats = sensorComp["currentStats"].to<JsonObject>();
    if (currCount > 0) {
      currStats["min"] = roundTo(currMin, 1);
      currStats["max"] = roundTo(currMax, 1);
      currStats["avg"] = roundTo(currSum / currCount, 1);
      currStats["readings"] = currCount;
      currStats["available"] = true;
      currStats["dataSource"] = "hot";
    } else if (!currInHotTier) {
      // Current month not in hot tier — try warm/cold
      populateStatsFromDailySummary(currYear, currMonth, hist.clientUid, hist.sensorIndex, currStats);
      if (!currStats["available"].as<bool>() && gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled) {
        if (loadFtpArchiveCached(currYear, currMonth)) {
          populateStatsFromFtpCache(hist.clientUid, hist.sensorIndex, currStats);
        }
      }
    } else {
      currStats["available"] = false;
      currStats["message"] = "No data for current month";
    }
    
    // Previous period stats from appropriate tier
    JsonObject prevStats = sensorComp["previousStats"].to<JsonObject>();
    if (prevInHotTier) {
      // Rare case: previous month still in hot tier (if retention > 30 days)
      float prevMin = 9999.0, prevMax = -9999.0, prevSum = 0.0;
      int prevCount = 0;
      for (uint16_t j = 0; j < hist.snapshotCount; j++) {
        uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
        TelemetrySnapshot &snap = hist.snapshots[idx];
        if (snap.timestamp < prevMonthStart || snap.timestamp >= prevMonthEnd) continue;
        if (snap.level < prevMin) prevMin = snap.level;
        if (snap.level > prevMax) prevMax = snap.level;
        prevSum += snap.level;
        prevCount++;
      }
      if (prevCount > 0) {
        prevStats["min"] = roundTo(prevMin, 1);
        prevStats["max"] = roundTo(prevMax, 1);
        prevStats["avg"] = roundTo(prevSum / prevCount, 1);
        prevStats["readings"] = prevCount;
        prevStats["available"] = true;
        prevStats["dataSource"] = "hot";
      } else {
        prevStats["available"] = false;
        prevStats["message"] = "No data for previous month in hot tier";
      }
    } else if (prevFromWarm) {
      populateStatsFromDailySummary(prevYear, prevMonth, hist.clientUid, hist.sensorIndex, prevStats);
    } else if (prevFromCold) {
      populateStatsFromFtpCache(hist.clientUid, hist.sensorIndex, prevStats);
    } else {
      prevStats["available"] = false;
      if (gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled) {
        prevStats["message"] = "Archive not found on FTP for this month";
      } else {
        prevStats["message"] = "FTP archive not enabled — previous month data unavailable";
      }
    }
    
    // Calculate delta if both periods have data
    bool currAvail = currStats["available"].as<bool>();
    bool prevAvail = prevStats["available"].as<bool>();
    if (currAvail && prevAvail) {
      float cAvg = currStats["avg"].as<float>();
      float pAvg = prevStats["avg"].as<float>();
      sensorComp["deltaAvg"] = roundTo(cAvg - pAvg, 1);
      sensorComp["deltaAvailable"] = true;
    } else {
      sensorComp["deltaAvailable"] = false;
      if (!prevAvail) {
        sensorComp["deltaMessage"] = "Previous month data not available for comparison";
      }
    }
  }
  
  // Include archive availability info
  JsonObject archiveInfo = doc["archiveInfo"].to<JsonObject>();
  archiveInfo["ftpEnabled"] = gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled;
  archiveInfo["lastSync"] = gHistorySettings.lastFtpSyncEpoch;
  archiveInfo["warmTierMonths"] = gHistorySettings.warmTierRetentionMonths;
  
  // Serialize and send
  String jsonOut;
  serializeJson(doc, jsonOut);
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(jsonOut.length());
  client.println(F("Cache-Control: no-cache"));
  client.println();
  client.print(jsonOut);
}

// Year-over-year comparison endpoint
// GET /api/history/yoy?sensor=CLIENT:SENSOR_INDEX&years=3
static void handleHistoryYearOverYear(EthernetClient &client, const String &query) {
  String sensorParam = getQueryParam(query, "sensor");  // Format: "CLIENT_UID:1"
  String yearsStr = getQueryParam(query, "years");
  int yearsToCompare = yearsStr.length() > 0 ? atoi(yearsStr.c_str()) : 3;
  if (yearsToCompare < 1) yearsToCompare = 1;
  if (yearsToCompare > 5) yearsToCompare = 5;  // Limit to 5 years
  
  // Parse sensor parameter
  String clientUid = "";
  int sensorIdx = 0;
  int colonPos = sensorParam.indexOf(':');
  if (colonPos > 0) {
    clientUid = sensorParam.substring(0, colonPos);
    sensorIdx = atoi(sensorParam.substring(colonPos + 1).c_str());
  }
  
  // Build YoY comparison JSON
  JsonDocument doc;
  
  // Get current year/month
  double nowEpoch = 0.0;
  if (gLastSyncedEpoch > 0.0) {
    nowEpoch = gLastSyncedEpoch + (double)(millis() - gLastSyncMillis) / 1000.0;
  }
  time_t nowTime = (time_t)nowEpoch;
  struct tm *nowTm = gmtime(&nowTime);
  int currentYear = nowTm ? nowTm->tm_year + 1900 : 2025;
  int currentMonth = nowTm ? nowTm->tm_mon + 1 : 1;
  
  doc["currentYear"] = currentYear;
  doc["currentMonth"] = currentMonth;
  doc["yearsCompared"] = yearsToCompare;
  
  // Pre-compute epoch boundaries for year filtering in hot tier
  struct tm yoyStartTm = {0};
  yoyStartTm.tm_year = currentYear - 1900;
  yoyStartTm.tm_mon = 0;
  yoyStartTm.tm_mday = 1;
  double currentYearStart = (double)mktime(&yoyStartTm);
  struct tm yoyEndTm = {0};
  yoyEndTm.tm_year = currentYear - 1900 + 1;
  yoyEndTm.tm_mon = 0;
  yoyEndTm.tm_mday = 1;
  double currentYearEnd = (double)mktime(&yoyEndTm);

  // Epoch boundaries for current month filtering (single-sensor branch)
  struct tm yoyCurrMStartTm = {0};
  yoyCurrMStartTm.tm_year = currentYear - 1900;
  yoyCurrMStartTm.tm_mon = currentMonth - 1;
  yoyCurrMStartTm.tm_mday = 1;
  double currentMonthStart = (double)mktime(&yoyCurrMStartTm);
  struct tm yoyCurrMEndTm = {0};
  yoyCurrMEndTm.tm_year = (currentMonth == 12) ? currentYear - 1900 + 1 : currentYear - 1900;
  yoyCurrMEndTm.tm_mon = (currentMonth == 12) ? 0 : currentMonth;
  yoyCurrMEndTm.tm_mday = 1;
  double currentMonthEnd = (double)mktime(&yoyCurrMEndTm);

  if (sensorParam.length() == 0) {
    // Return summary for all sensors
    JsonArray sensorSummaries = doc["sensors"].to<JsonArray>();
    
    for (uint8_t h = 0; h < gSensorHistoryCount; h++) {
      SensorHourlyHistory &hist = gSensorHistory[h];
      if (hist.snapshotCount == 0) continue;
      
      JsonObject sensorSum = sensorSummaries.add<JsonObject>();
      sensorSum["client"] = hist.clientUid;
      sensorSum["site"] = hist.siteName;
      sensorSum["sensorIndex"] = hist.sensorIndex;
      
      // Current year stats from hot tier (filtered to currentYear only)
      float yearMin = 9999.0, yearMax = -9999.0, yearSum = 0.0;
      int yearCount = 0;
      
      for (uint16_t j = 0; j < hist.snapshotCount; j++) {
        uint16_t idx = (hist.writeIndex - hist.snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
        TelemetrySnapshot &snap = hist.snapshots[idx];
        if (snap.timestamp < currentYearStart || snap.timestamp >= currentYearEnd) continue;
        if (snap.level < yearMin) yearMin = snap.level;
        if (snap.level > yearMax) yearMax = snap.level;
        yearSum += snap.level;
        yearCount++;
      }
      
      JsonObject currentYearStats = sensorSum["currentYear"].to<JsonObject>();
      if (yearCount > 0) {
        currentYearStats["min"] = roundTo(yearMin, 1);
        currentYearStats["max"] = roundTo(yearMax, 1);
        currentYearStats["avg"] = roundTo(yearSum / yearCount, 1);
        currentYearStats["readings"] = yearCount;
        currentYearStats["dataSource"] = "hot";
      }
      
      // Previous years — attempt warm tier (monthly aggregation) or cold tier (FTP)
      JsonArray prevYears = sensorSum["previousYears"].to<JsonArray>();
      for (int y = 1; y <= yearsToCompare; y++) {
        int targetYear = currentYear - y;
        JsonObject yearInfo = prevYears.add<JsonObject>();
        yearInfo["year"] = targetYear;
        
        // Aggregate all 12 months of the target year from warm/cold tiers
        float yMin = 9999.0, yMax = -9999.0, ySum = 0.0;
        int yDays = 0;
        bool foundAnyMonth = false;
        
        for (int m = 1; m <= 12; m++) {
          // Try warm tier first (LittleFS daily summaries)
          JsonDocument mDoc;
          if (loadDailySummaryMonth(targetYear, m, mDoc)) {
            JsonArray entries = mDoc.as<JsonArray>();
            for (JsonObject entry : entries) {
              const char *uid = entry["c"] | "";
              uint8_t tk = entry["k"] | 0;
              if (strcmp(uid, hist.clientUid) != 0 || tk != hist.sensorIndex) continue;
              float mn = entry["mn"] | 0.0f;
              float mx = entry["mx"] | 0.0f;
              float av = entry["av"] | 0.0f;
              if (mn < yMin) yMin = mn;
              if (mx > yMax) yMax = mx;
              ySum += av;
              yDays++;
              foundAnyMonth = true;
            }
          } else if (gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled) {
            // Try cold tier (FTP archive)
            if (loadFtpArchiveCached(targetYear, m)) {
              for (uint8_t ci = 0; ci < gFtpArchiveCache.sensorCount; ci++) {
                auto &cached = gFtpArchiveCache.sensors[ci];
                if (strcmp(cached.clientUid, hist.clientUid) != 0 || cached.sensorIndex != hist.sensorIndex) continue;
                if (cached.minLevel < yMin) yMin = cached.minLevel;
                if (cached.maxLevel > yMax) yMax = cached.maxLevel;
                ySum += cached.avgLevel;
                yDays++;
                foundAnyMonth = true;
              }
            }
          }
        }
        
        if (foundAnyMonth && yDays > 0) {
          yearInfo["available"] = true;
          yearInfo["min"] = roundTo(yMin, 1);
          yearInfo["max"] = roundTo(yMax, 1);
          yearInfo["avg"] = roundTo(ySum / yDays, 1);
          yearInfo["monthsWithData"] = yDays;
          yearInfo["dataSource"] = "archive";
        } else {
          yearInfo["available"] = false;
          if (!(gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled)) {
            yearInfo["message"] = "FTP archive not enabled";
          } else {
            yearInfo["message"] = "No archived data found for this year";
          }
        }
      }
    }
  } else {
    // Return detailed monthly history for specific sensor
    doc["sensor"]["client"] = clientUid;
    doc["sensor"]["sensorIndex"] = sensorIdx;
    
    // Find sensor in hot tier
    SensorHourlyHistory *hist = nullptr;
    for (uint8_t h = 0; h < gSensorHistoryCount; h++) {
      if (strcmp(gSensorHistory[h].clientUid, clientUid.c_str()) == 0 && 
          gSensorHistory[h].sensorIndex == sensorIdx) {
        hist = &gSensorHistory[h];
        break;
      }
    }
    
    if (hist && hist->snapshotCount > 0) {
      doc["sensor"]["site"] = hist->siteName;
      doc["sensor"]["found"] = true;
      
      JsonArray monthlyData = doc["sensor"]["monthlyData"].to<JsonArray>();
      
      // Current month from hot tier
      JsonObject currMonthObj = monthlyData.add<JsonObject>();
      currMonthObj["year"] = currentYear;
      currMonthObj["month"] = currentMonth;
      
      float min = 9999.0, max = -9999.0, sum = 0.0;
      int count = 0;
      for (uint16_t j = 0; j < hist->snapshotCount; j++) {
        uint16_t idx = (hist->writeIndex - hist->snapshotCount + j + MAX_HOURLY_HISTORY_PER_SENSOR) % MAX_HOURLY_HISTORY_PER_SENSOR;
        TelemetrySnapshot &snap = hist->snapshots[idx];
        if (snap.timestamp < currentMonthStart || snap.timestamp >= currentMonthEnd) continue;
        if (snap.level < min) min = snap.level;
        if (snap.level > max) max = snap.level;
        sum += snap.level;
        count++;
      }
      if (count > 0) {
        currMonthObj["min"] = roundTo(min, 1);
        currMonthObj["max"] = roundTo(max, 1);
        currMonthObj["avg"] = roundTo(sum / count, 1);
        currMonthObj["readings"] = count;
        currMonthObj["dataSource"] = "hot";
      }
      
      // Previous months from warm/cold tiers (go back up to yearsToCompare*12 months)
      int totalMonthsBack = yearsToCompare * 12;
      int mYear = currentYear;
      int mMonth = currentMonth - 1;  // Start from previous month
      if (mMonth < 1) { mMonth = 12; mYear--; }
      
      for (int i = 0; i < totalMonthsBack && mYear >= 2020; i++) {
        JsonObject mObj = monthlyData.add<JsonObject>();
        mObj["year"] = mYear;
        mObj["month"] = mMonth;
        
        // Try warm tier
        populateStatsFromDailySummary(mYear, mMonth, clientUid.c_str(), sensorIdx, mObj);
        
        // If warm tier failed, try cold tier
        if (!mObj["available"].as<bool>() && gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled) {
          if (loadFtpArchiveCached(mYear, mMonth)) {
            // Clear previous failed attempt fields
            mObj.remove("available");
            mObj.remove("message");
            populateStatsFromFtpCache(clientUid.c_str(), sensorIdx, mObj);
          }
        }
        
        // Move to previous month
        mMonth--;
        if (mMonth < 1) { mMonth = 12; mYear--; }
      }
    } else {
      doc["sensor"]["found"] = false;
      doc["sensor"]["message"] = "Sensor not found in hot tier";
    }
  }
  
  // Include archive availability info
  JsonObject archiveInfo = doc["archiveInfo"].to<JsonObject>();
  archiveInfo["ftpEnabled"] = gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled;
  archiveInfo["warmTierMonths"] = gHistorySettings.warmTierRetentionMonths;
  if (!(gConfig.ftpEnabled && gHistorySettings.ftpArchiveEnabled)) {
    archiveInfo["note"] = "Enable FTP for full historical archive access";
  }
  
  // Serialize and send
  String jsonOut;
  serializeJson(doc, jsonOut);
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(jsonOut.length());
  client.println(F("Cache-Control: no-cache"));
  client.println();
  client.print(jsonOut);
}

// ============================================================================
// Archived Clients API
// ============================================================================
// GET /api/history/archived         — Returns manifest of all archived clients
// GET /api/history/archived?file=X  — Downloads specific archive from FTP

static void handleArchivedClients(EthernetClient &client, const String &query) {
  String fileParam = getQueryParam(query, "file");

  if (fileParam.length() > 0) {
    // Load specific archive from FTP
    if (!gConfig.ftpEnabled) {
      respondStatus(client, 503, F("FTP not enabled"));
      return;
    }

    // Validate filename: only allow alphanumeric, dash, underscore, dot, slash
    for (unsigned int i = 0; i < fileParam.length(); ++i) {
      char ch = fileParam.charAt(i);
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '/')) {
        respondStatus(client, 400, F("Invalid filename"));
        return;
      }
    }
    // Block path traversal
    if (fileParam.indexOf("..") >= 0) {
      respondStatus(client, 400, F("Invalid filename"));
      return;
    }

    FtpSession session;
    char err[128];
    if (!ftpConnectAndLogin(session, err, sizeof(err))) {
      respondStatus(client, 502, F("FTP connection failed"));
      return;
    }

    // Archive files may be up to ~8KB; use a stack buffer
    char buf[8192];
    size_t bufLen = 0;
    bool ok = ftpRetrieveBuffer(session, fileParam.c_str(), buf, sizeof(buf), bufLen, err, sizeof(err));
    ftpQuit(session);

    if (!ok || bufLen == 0) {
      respondStatus(client, 404, F("Archive not found on FTP"));
      return;
    }

    client.println(F("HTTP/1.1 200 OK"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Connection: close"));
    client.print(F("Content-Length: "));
    client.println(bufLen);
    client.println(F("Cache-Control: no-cache"));
    client.println();
    client.write((const uint8_t *)buf, bufLen);
    return;
  }

  // Return manifest of archived clients from LittleFS
  JsonDocument doc;
  doc["ftpEnabled"] = gConfig.ftpEnabled;

  #ifdef FILESYSTEM_AVAILABLE
  {
    FILE *mf = fopen("/fs/archived_clients.json", "r");
    if (mf) {
      char buf[2048];
      size_t nRead = fread(buf, 1, sizeof(buf) - 1, mf);
      fclose(mf);
      buf[nRead] = '\0';
      JsonDocument manifest;
      if (!deserializeJson(manifest, buf) && manifest["archives"].is<JsonArray>()) {
        doc["archives"] = manifest["archives"];
      }
    }
  }
  #endif

  if (!doc["archives"].is<JsonArray>()) {
    doc["archives"].to<JsonArray>();  // Empty array
  }

  String jsonOut;
  serializeJson(doc, jsonOut);

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(jsonOut.length());
  client.println(F("Cache-Control: no-cache"));
  client.println();
  client.print(jsonOut);
}

static bool loadContactsConfig(JsonDocument &doc) {
#ifdef FILESYSTEM_AVAILABLE
  if (gContactsCacheValid) {
    if (!deserializeJson(doc, gContactsCache)) {
      return true;
    }
  }
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return false;
    FILE *file = fopen("/fs/contacts_config.json", "r");
    if (!file) {
      if (gContactsCacheValid && !deserializeJson(doc, gContactsCache)) {
        return true;
      }
      return false;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 32768) {
      fclose(file);
      if (gContactsCacheValid && !deserializeJson(doc, gContactsCache)) {
        return true;
      }
      return false;
    }

    char *buffer = (char *)malloc(fileSize + 1);
    if (!buffer) {
      fclose(file);
      if (gContactsCacheValid && !deserializeJson(doc, gContactsCache)) {
        return true;
      }
      return false;
    }

    size_t bytesRead = fread(buffer, 1, fileSize, file);
    buffer[bytesRead] = '\0';
    fclose(file);

    DeserializationError err = deserializeJson(doc, buffer);
    free(buffer);
  #else
    if (!LittleFS.exists(CONTACTS_CONFIG_PATH)) {
      if (gContactsCacheValid && !deserializeJson(doc, gContactsCache)) {
        return true;
      }
      return false;
    }

    File file = LittleFS.open(CONTACTS_CONFIG_PATH, "r");
    if (!file) {
      if (gContactsCacheValid && !deserializeJson(doc, gContactsCache)) {
        return true;
      }
      return false;
    }

    DeserializationError err = deserializeJson(doc, file);
    file.close();
  #endif

  if (err) {
    if (gContactsCacheValid && !deserializeJson(doc, gContactsCache)) {
      return true;
    }
    return false;
  }

  return true;
#else
  return false;
#endif
}

static bool saveContactsConfig(const JsonDocument &doc) {
#ifdef FILESYSTEM_AVAILABLE
  String output;
  serializeJson(doc, output);
  gContactsCache = output;
  gContactsCacheValid = true;

  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    if (!mbedFS) return false;
    return serverWriteFileAtomic("/fs/contacts_config.json",
                                 output.c_str(), output.length());
  #else
    return tankalarm_littlefs_write_file_atomic(CONTACTS_CONFIG_PATH,
            (const uint8_t *)output.c_str(), output.length());
  #endif
#else
  return false;
#endif
}

static void handleContactsGet(EthernetClient &client) {
  JsonDocument doc;
  bool loaded = loadContactsConfig(doc);
  
  // Use existing arrays if loaded, otherwise create empty ones
  // Note: .to<JsonArray>() would overwrite existing data, so we use conditional creation
  if (!loaded || !doc["contacts"].is<JsonArray>()) {
    doc["contacts"].to<JsonArray>();
  }
  if (!loaded || !doc["dailyReportRecipients"].is<JsonArray>()) {
    doc["dailyReportRecipients"].to<JsonArray>();
  }
  if (!loaded || !doc["smsAlertRecipients"].is<JsonArray>()) {
    doc["smsAlertRecipients"].to<JsonArray>();
  }
  
  // Build list of unique sites from sensor records
  // Use simple linear scan - with typical fleet sizes (< 100 sensors), performance is adequate
  JsonArray sitesArray = doc["sites"].to<JsonArray>();
  sitesArray.clear();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    if (strlen(gSensorRecords[i].site) == 0) continue;
    
    bool alreadySeen = false;
    for (uint8_t j = 0; j < i; ++j) {
      if (strcmp(gSensorRecords[i].site, gSensorRecords[j].site) == 0) {
        alreadySeen = true;
        break;
      }
    }
    if (!alreadySeen) {
      sitesArray.add(gSensorRecords[i].site);
    }
  }
  
  // Build list of alarms (sensors with alarm configurations)
  JsonArray alarmsArray = doc["alarms"].to<JsonArray>();
  alarmsArray.clear();
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    SensorRecord &rec = gSensorRecords[i];
    if (strlen(rec.alarmType) > 0) {
      JsonObject alarmObj = alarmsArray.add<JsonObject>();
      char alarmId[64];
      snprintf(alarmId, sizeof(alarmId), "%s_%d", rec.clientUid, rec.sensorIndex);
      alarmObj["id"] = alarmId;
      alarmObj["site"] = rec.site;
      alarmObj["label"] = rec.label;
      alarmObj["type"] = rec.alarmType;
    }
  }
  
  String response;
  serializeJson(doc, response);
  // unique_ptr automatically handles cleanup
  respondJson(client, response);
}

static void handleContactsPost(EthernetClient &client, const String &body) {
  // Use larger buffer to match MAX_HTTP_BODY_BYTES (16KB) for large contact lists
  size_t capacity = body.length() + 1024;  // allow some headroom for parsing
  if (capacity < 2048) {
    capacity = 2048;
  }
  if (capacity > 32768) {
    capacity = 32768;  // hard ceiling to avoid runaway allocation
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  // Require PIN authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }
  
  // Validate contacts structure even though not persisted yet
  if (doc["contacts"] && doc["contacts"].is<JsonArray>()) {
    JsonArray contactsArray = doc["contacts"].as<JsonArray>();
    
    // Limit number of contacts to prevent memory issues
    if (contactsArray.size() > 100) {
      respondStatus(client, 400, F("Too many contacts (max 100)"));
      return;
    }
    
    // Basic validation of each contact
    for (JsonVariant contactVar : contactsArray) {
      JsonObject contact = contactVar.as<JsonObject>();
      
      // Validate required fields
      if (!contact["name"] || !contact["name"].is<const char*>()) {
        respondStatus(client, 400, F("Contact missing required 'name' field"));
        return;
      }
      
      // Validate that at least phone or email is present
      bool hasPhone = contact["phone"] && contact["phone"].is<const char*>() && strlen(contact["phone"]) > 0;
      bool hasEmail = contact["email"] && contact["email"].is<const char*>() && strlen(contact["email"]) > 0;
      if (!hasPhone && !hasEmail) {
        respondStatus(client, 400, F("Contact must have phone or email"));
        return;
      }
      
      // Validate alarm associations array if present
      if (contact["alarmAssociations"] && !contact["alarmAssociations"].is<JsonArray>()) {
        respondStatus(client, 400, F("alarmAssociations must be an array"));
        return;
      }
    }
  }
  
  // Validate daily report recipients array if present
  if (doc["dailyReportRecipients"] && !doc["dailyReportRecipients"].is<JsonArray>()) {
    respondStatus(client, 400, F("dailyReportRecipients must be an array"));
    return;
  }
  
  // Validate SMS alert recipients array if present
  if (doc["smsAlertRecipients"] && !doc["smsAlertRecipients"].is<JsonArray>()) {
    respondStatus(client, 400, F("smsAlertRecipients must be an array"));
    return;
  }

  if (!doc["contacts"].is<JsonArray>()) {
    doc["contacts"].to<JsonArray>();
  }
  if (!doc["dailyReportRecipients"].is<JsonArray>()) {
    doc["dailyReportRecipients"].to<JsonArray>();
  }
  if (!doc["smsAlertRecipients"].is<JsonArray>()) {
    doc["smsAlertRecipients"].to<JsonArray>();
  }

  String cachePayload;
  serializeJson(doc, cachePayload);
  gContactsCache = cachePayload;
  gContactsCacheValid = true;

  bool saved = saveContactsConfig(doc);

  JsonDocument response;
  response["success"] = true;
  response["message"] = saved ? "Contacts saved successfully" : "Contacts saved in memory; persistence unavailable";
  response["saved"] = saved;
  
  String responseStr;
  serializeJson(response, responseStr);
  respondJson(client, responseStr);
}

// ============================================================================
// Email Format Handlers
// ============================================================================

#define EMAIL_FORMAT_PATH "/email_format.json"
// Allow up to 32KB for email format JSON (matches contacts config and JSON capacity)
#define MAX_EMAIL_FORMAT_SIZE 32768

static bool loadEmailFormat(JsonDocument &doc) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS) return false;
  
  FILE *file = fopen("/fs/email_format.json", "r");
  if (!file) {
    return false;
  }
  
  fseek(file, 0, SEEK_END);
  long fileSize = ftell(file);
  fseek(file, 0, SEEK_SET);
  
  if (fileSize <= 0 || fileSize > MAX_EMAIL_FORMAT_SIZE) {
    fclose(file);
    return false;
  }
  
  char *buffer = (char *)malloc(fileSize + 1);
  if (!buffer) {
    fclose(file);
    return false;
  }
  
  size_t bytesRead = fread(buffer, 1, fileSize, file);
  fclose(file);
  
  // Check if we read the expected amount
  if (bytesRead != (size_t)fileSize) {
    free(buffer);
    return false;
  }
  buffer[bytesRead] = '\0';
  
  DeserializationError error = deserializeJson(doc, buffer);
  free(buffer);
  return error == DeserializationError::Ok;
#else
  if (!LittleFS.exists(EMAIL_FORMAT_PATH)) {
    return false;
  }
  
  File file = LittleFS.open(EMAIL_FORMAT_PATH, "r");
  if (!file) {
    return false;
  }
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  return error == DeserializationError::Ok;
#endif
}

static bool saveEmailFormat(const JsonDocument &doc) {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!mbedFS) return false;
  
  String output;
  serializeJson(doc, output);
  
  return serverWriteFileAtomic("/fs/email_format.json",
                               output.c_str(), output.length());
#else
  String output;
  serializeJson(doc, output);
  
  return tankalarm_littlefs_write_file_atomic(EMAIL_FORMAT_PATH,
          (const uint8_t *)output.c_str(), output.length());
#endif
}

static void handleEmailFormatGet(EthernetClient &client) {
  JsonDocument doc;
  bool loaded = loadEmailFormat(doc);
  
  // Set defaults if not loaded or missing fields
  if (!loaded) {
    doc["emailSubject"] = "Daily Sensor Summary - {date}";
    doc["companyName"] = "";
    doc["includeDate"] = true;
    doc["includeTime"] = false;
    doc["includeServerName"] = true;
    doc["groupBySite"] = true;
    doc["showSiteSummary"] = true;
    doc["showSiteCapacity"] = false;
    doc["fieldSensorIndex"] = true;
    doc["fieldSensorName"] = true;
    doc["fieldContents"] = true;
    doc["fieldLevelInches"] = true;
    doc["fieldLevelPercent"] = false;
    doc["fieldLevelChange"] = true;
    doc["fieldDeliveryIndicator"] = true;
    doc["fieldLastReading"] = false;
    doc["fieldSensorMa"] = false;
    doc["fieldAlarmStatus"] = true;
    doc["fieldCapacity"] = false;
    doc["includeFleetSummary"] = true;
    doc["summaryTotalSensors"] = true;
    doc["summaryActiveAlarms"] = true;
    doc["summaryDeliveries"] = true;
    doc["summaryStaleWarning"] = true;
    doc["numberFormat"] = "decimal";
    doc["dateFormat"] = "us";
    doc["changeStyle"] = "arrow";
    doc["useColors"] = true;
    doc["useDividers"] = true;
  }
  
  String response;
  serializeJson(doc, response);
  respondJson(client, response);
}

static void handleEmailFormatPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  // Require PIN authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }
  
  bool saved = saveEmailFormat(doc);
  
  JsonDocument response;
  response["success"] = true;
  response["saved"] = saved;
  response["message"] = saved ? "Email format saved" : "Save failed";
  
  String responseStr;
  serializeJson(response, responseStr);
  respondJson(client, responseStr);
}

static void handleServerSettingsPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }

  // Require valid PIN for authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  // Extract server settings from JSON.
  // Support both flat structure and nested "server" object. The UI historically
  // wrapped the payload under {"server":{...}}, so this branch must work.
  // ArduinoJson v7: use .is<JsonObject>()/.as<JsonObject>() instead of
  // containsKey()+implicit cast, which could leave `settings` null.
  JsonObject settings;
  if (doc["server"].is<JsonObject>()) {
    settings = doc["server"].as<JsonObject>();
  } else {
    settings = doc.as<JsonObject>();
  }
  if (settings.isNull()) {
    respondStatus(client, 400, "Missing settings object");
    return;
  }
    
  // Update Notecard Product UID (requires reinitialization)
  bool productUidChanged = false;
  
  const char *newProductUid = settings["productUid"] | "";
  // Only update if value is provided/non-empty or if we want to allow clearing it?
  // JS always sends a value (trimmed).
  if (settings.containsKey("productUid") && strcmp(gConfig.productUid, newProductUid) != 0) {
    strlcpy(gConfig.productUid, newProductUid, sizeof(gConfig.productUid));
    productUidChanged = true;
  }

  // Update server fleet (requires reinitialization if changed)
  if (settings.containsKey("serverFleet")) {
    const char *newFleet = settings["serverFleet"] | "tankalarm-server";
    if (strcmp(gConfig.serverFleet, newFleet) != 0) {
      strlcpy(gConfig.serverFleet, newFleet, sizeof(gConfig.serverFleet));
      productUidChanged = true;  // Reinitialize Notecard to apply new fleet
    }
  }

  // Update SMS settings
  if (settings.containsKey("smsPrimary")) {
    strlcpy(gConfig.smsPrimary, settings["smsPrimary"] | "", sizeof(gConfig.smsPrimary));
  }
  if (settings.containsKey("smsSecondary")) {
    strlcpy(gConfig.smsSecondary, settings["smsSecondary"] | "", sizeof(gConfig.smsSecondary));
  }
  if (settings.containsKey("smsOnHigh")) {
    gConfig.smsOnHigh = settings["smsOnHigh"] | false;
  }
  if (settings.containsKey("smsOnLow")) {
    gConfig.smsOnLow = settings["smsOnLow"] | false;
  }
  if (settings.containsKey("smsOnClear")) {
    gConfig.smsOnClear = settings["smsOnClear"] | false;
  }
  if (settings.containsKey("serverDownSmsEnabled")) {
    gConfig.serverDownSmsEnabled = settings["serverDownSmsEnabled"] | true;
  }

  // Update viewer device setting
  bool viewerChanged = false;
  if (settings.containsKey("viewerEnabled")) {
    bool newVal = settings["viewerEnabled"] | false;
    if (newVal != gConfig.viewerEnabled) {
      viewerChanged = true;
    }
    gConfig.viewerEnabled = newVal;
  }

  // Update firmware update policy
  if (settings.containsKey("updatePolicy")) {
    uint8_t pol = settings["updatePolicy"] | 0;
    gConfig.updatePolicy = (pol <= UPDATE_POLICY_AUTO_DFU) ? pol : UPDATE_POLICY_DISABLED;
  }
  if (settings.containsKey("checkClientVersionAlerts")) {
    gConfig.checkClientVersionAlerts = settings["checkClientVersionAlerts"] | true;
  }
  if (settings.containsKey("checkViewerVersionAlerts")) {
    gConfig.checkViewerVersionAlerts = settings["checkViewerVersionAlerts"] | true;
  }

  // Update daily email settings with validation and schedule tracking
  bool dailyScheduleChanged = false;
  if (settings.containsKey("dailyHour")) {
    int hour = settings["dailyHour"] | 5;
    // Validate hour range (0-23)
    if (hour < 0) {
      hour = 0;
    } else if (hour > 23) {
      hour = 23;
    }
    if (hour != gConfig.dailyHour) {
      dailyScheduleChanged = true;
    }
    gConfig.dailyHour = hour;
  }
  if (settings.containsKey("dailyMinute")) {
    int minute = settings["dailyMinute"] | 0;
    // Validate minute range (0-59)
    if (minute < 0) {
      minute = 0;
    } else if (minute > 59) {
      minute = 59;
    }
    if (minute != gConfig.dailyMinute) {
      dailyScheduleChanged = true;
    }
    gConfig.dailyMinute = minute;
  }
  if (settings.containsKey("dailyEmail")) {
    strlcpy(gConfig.dailyEmail, settings["dailyEmail"] | "", sizeof(gConfig.dailyEmail));
  }

  // Update FTP settings
  if (settings["ftp"]) {
    JsonObject ftpObj = settings["ftp"];
    if (ftpObj.containsKey("enabled")) {
      gConfig.ftpEnabled = ftpObj["enabled"] | false;
    }
    if (ftpObj.containsKey("passive")) {
      gConfig.ftpPassive = ftpObj["passive"] | true;
    }
    if (ftpObj.containsKey("backupOnChange")) {
      gConfig.ftpBackupOnChange = ftpObj["backupOnChange"] | false;
    }
    if (ftpObj.containsKey("restoreOnBoot")) {
      gConfig.ftpRestoreOnBoot = ftpObj["restoreOnBoot"] | false;
    }
    if (ftpObj.containsKey("host")) {
      strlcpy(gConfig.ftpHost, ftpObj["host"] | "", sizeof(gConfig.ftpHost));
    }
    if (ftpObj.containsKey("port")) {
      uint32_t port = ftpObj["port"] | 21;
      // Validate port range (1-65535)
      if (port > 0 && port <= 65535UL) {
        gConfig.ftpPort = (uint16_t)port;
      } else {
        gConfig.ftpPort = 21;
      }
    }
    if (ftpObj.containsKey("user")) {
      strlcpy(gConfig.ftpUser, ftpObj["user"] | "", sizeof(gConfig.ftpUser));
    }
    if (ftpObj.containsKey("pass")) {
      strlcpy(gConfig.ftpPass, ftpObj["pass"] | "", sizeof(gConfig.ftpPass));
    }
    if (ftpObj.containsKey("path")) {
      strlcpy(gConfig.ftpPath, ftpObj["path"] | "/tankalarm/server", sizeof(gConfig.ftpPath));
    }
    if (ftpObj.containsKey("ftpsEnabled")) {
      gConfig.ftpsEnabled = ftpObj["ftpsEnabled"] | false;
    }
    if (ftpObj.containsKey("ftpsTrustMode")) {
      uint8_t mode = ftpObj["ftpsTrustMode"] | 0;
      gConfig.ftpsTrustMode = (mode <= 1) ? mode : 0;
    }
    if (ftpObj.containsKey("ftpsFingerprint")) {
      strlcpy(gConfig.ftpsFingerprint, ftpObj["ftpsFingerprint"] | "", sizeof(gConfig.ftpsFingerprint));
    }
    if (ftpObj.containsKey("ftpsTlsServerName")) {
      strlcpy(gConfig.ftpsTlsServerName, ftpObj["ftpsTlsServerName"] | "", sizeof(gConfig.ftpsTlsServerName));
    }
  }

  if (!saveConfig(gConfig)) {
    respondStatus(client, 500, "Failed to save settings to local storage");
    return;
  }
  gConfigDirty = false;
  if (gConfig.ftpEnabled && gConfig.ftpBackupOnChange) {
    gPendingFtpBackup = true;
  }
  
  // Reschedule daily email if time changed
  if (dailyScheduleChanged) {
    scheduleNextDailyEmail();
  }

  // Handle viewer enable/disable
  if (viewerChanged) {
    if (gConfig.viewerEnabled) {
      scheduleNextViewerSummary();
      Serial.println(F("Viewer device enabled"));
    } else {
      gNextViewerSummaryEpoch = 0.0;
      Serial.println(F("Viewer device disabled"));
    }
  }
  
  // Reinitialize Notecard if Product UID changed
  if (productUidChanged) {
    Serial.println(F("Product UID changed, reinitializing Notecard..."));
    initializeNotecard();
  }

  // Respond with success
  String responseStr = "{\"success\":true,\"message\":\"Settings saved\"}";
  respondJson(client, responseStr);
}

// Find existing sensor calibration (read-only, does not create)
static SensorCalibration *findSensorCalibration(const char *clientUid, uint8_t sensorIndex) {
  if (!clientUid || strlen(clientUid) == 0) {
    return nullptr;
  }
  
  // Search for existing calibration
  for (uint8_t i = 0; i < gSensorCalibrationCount; ++i) {
    if (strcmp(gSensorCalibrations[i].clientUid, clientUid) == 0 && 
        gSensorCalibrations[i].sensorIndex == sensorIndex) {
      return &gSensorCalibrations[i];
    }
  }
  
  return nullptr;
}

static SensorCalibration *findOrCreateSensorCalibration(const char *clientUid, uint8_t sensorIndex) {
  if (!clientUid || strlen(clientUid) == 0) {
    return nullptr;
  }
  
  // Search for existing calibration
  for (uint8_t i = 0; i < gSensorCalibrationCount; ++i) {
    if (strcmp(gSensorCalibrations[i].clientUid, clientUid) == 0 && 
        gSensorCalibrations[i].sensorIndex == sensorIndex) {
      return &gSensorCalibrations[i];
    }
  }
  
  // Create new entry if space available
  if (gSensorCalibrationCount < MAX_CALIBRATION_SENSORS) {
    SensorCalibration *cal = &gSensorCalibrations[gSensorCalibrationCount++];
    memset(cal, 0, sizeof(SensorCalibration));
    strlcpy(cal->clientUid, clientUid, sizeof(cal->clientUid));
    cal->sensorIndex = sensorIndex;
    cal->hasLearnedCalibration = false;
    cal->hasTempCompensation = false;
    cal->entryCount = 0;
    cal->tempEntryCount = 0;
    cal->learnedSlope = 0.0f;
    cal->learnedOffset = 0.0f;
    cal->learnedTempCoef = 0.0f;
    cal->rSquared = 0.0f;
    return cal;
  }
  
  return nullptr;
}

// Helper struct to hold calibration data points during regression calculation
struct CalibrationPoint {
  float sensorReading;  // X1: mA reading
  float temperature;    // X2: temperature in °F (or TEMPERATURE_UNAVAILABLE)
  float verifiedLevel;  // Y: measured level in inches
  bool hasTemp;         // True if temperature data is valid
};

// Multiple linear regression using calibration log entries
// Model: level = slope * sensorReading + tempCoef * (temp - refTemp) + offset
// Falls back to simple linear regression if insufficient temperature data
static void recalculateCalibration(SensorCalibration *cal) {
  if (!cal) {
    return;
  }
  
#ifdef FILESYSTEM_AVAILABLE
  // Collect data points from calibration log
  static CalibrationPoint points[MAX_CALIBRATION_ENTRIES];
  int totalCount = 0;
  int tempCount = 0;
  
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    FILE *file = fopen("/fs/calibration_log.txt", "r");
    if (!file) {
      cal->hasLearnedCalibration = false;
      cal->hasTempCompensation = false;
      return;
    }
    
    char lineBuffer[256];
    while (fgets(lineBuffer, sizeof(lineBuffer), file) != nullptr && totalCount < MAX_CALIBRATION_ENTRIES) {
      String line = String(lineBuffer);
      line.trim();
      if (line.length() == 0) continue;
      
      // Parse: clientUid\tsensorIndex\ttimestamp\tsensorReading\tverifiedLevel\ttemperatureF\tnotes
      int pos1 = line.indexOf('\t');
      if (pos1 < 0) continue;
      String uid = line.substring(0, pos1);
      
      int pos2 = line.indexOf('\t', pos1 + 1);
      if (pos2 < 0) continue;
      int sensorIdx = line.substring(pos1 + 1, pos2).toInt();
      
      // Check if this entry matches our sensor
      if (uid != String(cal->clientUid) || sensorIdx != cal->sensorIndex) continue;
      
      int pos3 = line.indexOf('\t', pos2 + 1);
      if (pos3 < 0) continue;
      // pos3+1 is timestamp
      
      int pos4 = line.indexOf('\t', pos3 + 1);
      if (pos4 < 0) continue;
      float sensorReading = line.substring(pos3 + 1, pos4).toFloat();
      
      int pos5 = line.indexOf('\t', pos4 + 1);
      float verifiedLevel = 0.0f;
      float temperature = TEMPERATURE_UNAVAILABLE;
      
      if (pos5 < 0) {
        // Old format without temperature
        verifiedLevel = line.substring(pos4 + 1).toFloat();
      } else {
        verifiedLevel = line.substring(pos4 + 1, pos5).toFloat();
        // Check for temperature field
        int pos6 = line.indexOf('\t', pos5 + 1);
        if (pos6 < 0) {
          // Could be temp only or notes only (old format)
          String remaining = line.substring(pos5 + 1);
          float tempVal = remaining.toFloat();
          if (tempVal != 0.0f || remaining.startsWith("0") || remaining.startsWith("-")) {
            temperature = tempVal;
          }
        } else {
          // New format with temperature and notes
          temperature = line.substring(pos5 + 1, pos6).toFloat();
        }
      }
      
      // Only include valid sensor readings (4-20mA range)
      if (sensorReading >= 4.0f && sensorReading <= 20.0f && verifiedLevel >= 0.0f) {
        points[totalCount].sensorReading = sensorReading;
        points[totalCount].verifiedLevel = verifiedLevel;
        points[totalCount].temperature = temperature;
        points[totalCount].hasTemp = (temperature != TEMPERATURE_UNAVAILABLE && 
                                       temperature > -50.0f && temperature < 150.0f);
        if (points[totalCount].hasTemp) {
          tempCount++;
        }
        totalCount++;
      }
    }
    
    fclose(file);
  #else
    if (!LittleFS.exists(CALIBRATION_LOG_PATH)) {
      cal->hasLearnedCalibration = false;
      cal->hasTempCompensation = false;
      return;
    }
    
    File file = LittleFS.open(CALIBRATION_LOG_PATH, "r");
    if (!file) {
      cal->hasLearnedCalibration = false;
      cal->hasTempCompensation = false;
      return;
    }
    
    while (file.available() && totalCount < MAX_CALIBRATION_ENTRIES) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      
      // Parse: clientUid\tsensorIndex\ttimestamp\tsensorReading\tverifiedLevel\ttemperatureF\tnotes
      int pos1 = line.indexOf('\t');
      if (pos1 < 0) continue;
      String uid = line.substring(0, pos1);
      
      int pos2 = line.indexOf('\t', pos1 + 1);
      if (pos2 < 0) continue;
      int sensorIdx = line.substring(pos1 + 1, pos2).toInt();
      
      // Check if this entry matches our sensor
      if (uid != String(cal->clientUid) || sensorIdx != cal->sensorIndex) continue;
      
      int pos3 = line.indexOf('\t', pos2 + 1);
      if (pos3 < 0) continue;
      
      int pos4 = line.indexOf('\t', pos3 + 1);
      if (pos4 < 0) continue;
      float sensorReading = line.substring(pos3 + 1, pos4).toFloat();
      
      int pos5 = line.indexOf('\t', pos4 + 1);
      float verifiedLevel = 0.0f;
      float temperature = TEMPERATURE_UNAVAILABLE;
      
      if (pos5 < 0) {
        verifiedLevel = line.substring(pos4 + 1).toFloat();
      } else {
        verifiedLevel = line.substring(pos4 + 1, pos5).toFloat();
        int pos6 = line.indexOf('\t', pos5 + 1);
        if (pos6 < 0) {
          String remaining = line.substring(pos5 + 1);
          float tempVal = remaining.toFloat();
          if (tempVal != 0.0f || remaining.startsWith("0") || remaining.startsWith("-")) {
            temperature = tempVal;
          }
        } else {
          temperature = line.substring(pos5 + 1, pos6).toFloat();
        }
      }
      
      if (sensorReading >= 4.0f && sensorReading <= 20.0f && verifiedLevel >= 0.0f) {
        points[totalCount].sensorReading = sensorReading;
        points[totalCount].verifiedLevel = verifiedLevel;
        points[totalCount].temperature = temperature;
        points[totalCount].hasTemp = (temperature != TEMPERATURE_UNAVAILABLE && 
                                       temperature > -50.0f && temperature < 150.0f);
        if (points[totalCount].hasTemp) {
          tempCount++;
        }
        totalCount++;
      }
    }
    
    file.close();
  #endif
  
  cal->entryCount = totalCount;
  cal->tempEntryCount = tempCount;
  
  // Calculate quality metrics (min/max sensor and level range)
  if (totalCount > 0) {
    cal->minSensorMa = 999.0f;
    cal->maxSensorMa = -999.0f;
    cal->minLevelInches = 99999.0f;
    cal->maxLevelInches = -99999.0f;
    for (int i = 0; i < totalCount; i++) {
      if (points[i].sensorReading < cal->minSensorMa) cal->minSensorMa = points[i].sensorReading;
      if (points[i].sensorReading > cal->maxSensorMa) cal->maxSensorMa = points[i].sensorReading;
      if (points[i].verifiedLevel < cal->minLevelInches) cal->minLevelInches = points[i].verifiedLevel;
      if (points[i].verifiedLevel > cal->maxLevelInches) cal->maxLevelInches = points[i].verifiedLevel;
    }
  } else {
    cal->minSensorMa = 0.0f;
    cal->maxSensorMa = 0.0f;
    cal->minLevelInches = 0.0f;
    cal->maxLevelInches = 0.0f;
  }
  
  if (totalCount < 2) {
    cal->hasLearnedCalibration = false;
    cal->hasTempCompensation = false;
    return;
  }
  
  // Decide whether to use temperature compensation
  // Need at least MIN_TEMP_ENTRIES_FOR_COMPENSATION entries with temp data
  // and those entries should have some temperature variation
  bool useTempCompensation = false;
  if (tempCount >= MIN_TEMP_ENTRIES_FOR_COMPENSATION) {
    // Check for temperature variation (at least 10°F range)
    float minTemp = 999.0f, maxTemp = -999.0f;
    for (int i = 0; i < totalCount; i++) {
      if (points[i].hasTemp) {
        if (points[i].temperature < minTemp) minTemp = points[i].temperature;
        if (points[i].temperature > maxTemp) maxTemp = points[i].temperature;
      }
    }
    useTempCompensation = (maxTemp - minTemp >= 10.0f);
  }
  
  if (useTempCompensation) {
    // Multiple linear regression: Y = b0 + b1*X1 + b2*X2
    // Where X1 = sensorReading, X2 = (temperature - TEMP_REFERENCE_F)
    // Only use points that have temperature data
    
    // Calculate sums for multiple regression (only temp-enabled points)
    float n = 0;
    float sumX1 = 0, sumX2 = 0, sumY = 0;
    float sumX1X1 = 0, sumX2X2 = 0, sumX1X2 = 0;
    float sumX1Y = 0, sumX2Y = 0, sumYY = 0;
    
    for (int i = 0; i < totalCount; i++) {
      if (!points[i].hasTemp) continue;
      
      float x1 = points[i].sensorReading;
      float x2 = points[i].temperature - TEMP_REFERENCE_F;  // Normalize around reference
      float y = points[i].verifiedLevel;
      
      n += 1.0f;
      sumX1 += x1;
      sumX2 += x2;
      sumY += y;
      sumX1X1 += x1 * x1;
      sumX2X2 += x2 * x2;
      sumX1X2 += x1 * x2;
      sumX1Y += x1 * y;
      sumX2Y += x2 * y;
      sumYY += y * y;
    }
    
    // Solve normal equations using Cramer's rule for 3x3 system
    // [n      sumX1   sumX2 ] [b0]   [sumY  ]
    // [sumX1  sumX1X1 sumX1X2] [b1] = [sumX1Y]
    // [sumX2  sumX1X2 sumX2X2] [b2]   [sumX2Y]
    
    // Calculate determinant of coefficient matrix
    float det = n * (sumX1X1 * sumX2X2 - sumX1X2 * sumX1X2)
              - sumX1 * (sumX1 * sumX2X2 - sumX1X2 * sumX2)
              + sumX2 * (sumX1 * sumX1X2 - sumX1X1 * sumX2);
    
    if (fabs(det) < 0.0001f) {
      // Matrix is singular, fall back to simple regression
      useTempCompensation = false;
    } else {
      // Solve for b0 (offset), b1 (slope), b2 (tempCoef)
      float detB0 = sumY * (sumX1X1 * sumX2X2 - sumX1X2 * sumX1X2)
                  - sumX1 * (sumX1Y * sumX2X2 - sumX1X2 * sumX2Y)
                  + sumX2 * (sumX1Y * sumX1X2 - sumX1X1 * sumX2Y);
      
      float detB1 = n * (sumX1Y * sumX2X2 - sumX1X2 * sumX2Y)
                  - sumY * (sumX1 * sumX2X2 - sumX1X2 * sumX2)
                  + sumX2 * (sumX1 * sumX2Y - sumX1Y * sumX2);
      
      float detB2 = n * (sumX1X1 * sumX2Y - sumX1Y * sumX1X2)
                  - sumX1 * (sumX1 * sumX2Y - sumX1Y * sumX2)
                  + sumY * (sumX1 * sumX1X2 - sumX1X1 * sumX2);
      
      cal->learnedOffset = detB0 / det;
      cal->learnedSlope = detB1 / det;
      cal->learnedTempCoef = detB2 / det;
      
      // Calculate R-squared for multiple regression
      float meanY = sumY / n;
      float ssTotal = sumYY - n * meanY * meanY;
      float ssResid = 0;
      for (int i = 0; i < totalCount; i++) {
        if (!points[i].hasTemp) continue;
        float x1 = points[i].sensorReading;
        float x2 = points[i].temperature - TEMP_REFERENCE_F;
        float y = points[i].verifiedLevel;
        float yPred = cal->learnedOffset + cal->learnedSlope * x1 + cal->learnedTempCoef * x2;
        float resid = y - yPred;
        ssResid += resid * resid;
      }
      
      if (ssTotal > 0.0001f) {
        cal->rSquared = 1.0f - (ssResid / ssTotal);
        if (cal->rSquared < 0.0f) cal->rSquared = 0.0f;
        if (cal->rSquared > 1.0f) cal->rSquared = 1.0f;
      } else {
        cal->rSquared = 0.0f;
      }
      
      cal->hasTempCompensation = true;
      cal->hasLearnedCalibration = true;
      cal->lastCalibrationEpoch = currentEpoch();
      computeImpliedSpecificGravity(cal);
      
      Serial.print(F("Calibration updated (with temp) for "));
      Serial.print(cal->clientUid);
      Serial.print(F(" sensor "));
      Serial.print(cal->sensorIndex);
      Serial.print(F(": slope="));
      Serial.print(cal->learnedSlope, 4);
      Serial.print(F(" unit/mA, tempCoef="));
      Serial.print(cal->learnedTempCoef, 4);
      Serial.print(F(" unit/°F, offset="));
      Serial.print(cal->learnedOffset, 2);
      Serial.print(F(", R2="));
      Serial.print(cal->rSquared, 3);
      Serial.print(F(" ("));
      Serial.print(tempCount);
      Serial.println(F(" temp points)"));
      return;
    }
  }
  
  // Simple linear regression (no temperature compensation)
  // Use all data points
  float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0, sumY2 = 0;
  for (int i = 0; i < totalCount; i++) {
    sumX += points[i].sensorReading;
    sumY += points[i].verifiedLevel;
    sumXY += points[i].sensorReading * points[i].verifiedLevel;
    sumX2 += points[i].sensorReading * points[i].sensorReading;
    sumY2 += points[i].verifiedLevel * points[i].verifiedLevel;
  }
  
  float n = (float)totalCount;
  float denominator = n * sumX2 - sumX * sumX;
  
  if (fabs(denominator) < 0.0001f) {
    cal->hasLearnedCalibration = false;
    cal->hasTempCompensation = false;
    return;
  }
  
  cal->learnedSlope = (n * sumXY - sumX * sumY) / denominator;
  cal->learnedOffset = (sumY - cal->learnedSlope * sumX) / n;
  cal->learnedTempCoef = 0.0f;  // No temperature compensation
  
  // Calculate R-squared
  float meanX = sumX / n;
  float meanY = sumY / n;
  float ssTotal = sumY2 - n * meanY * meanY;
  float ssX = sumX2 - n * meanX * meanX;
  float ssCovXY = sumXY - n * meanX * meanY;
  
  if (ssTotal > 0.0001f && ssX > 0.0001f) {
    cal->rSquared = (ssCovXY * ssCovXY) / (ssX * ssTotal);
    if (cal->rSquared < 0.0f) cal->rSquared = 0.0f;
    if (cal->rSquared > 1.0f) cal->rSquared = 1.0f;
  } else {
    cal->rSquared = 0.0f;
  }
  
  cal->hasTempCompensation = false;
  cal->hasLearnedCalibration = true;
  cal->lastCalibrationEpoch = currentEpoch();
  computeImpliedSpecificGravity(cal);
  
  Serial.print(F("Calibration updated for "));
  Serial.print(cal->clientUid);
  Serial.print(F(" sensor "));
  Serial.print(cal->sensorIndex);
  Serial.print(F(": slope="));
  Serial.print(cal->learnedSlope, 4);
  Serial.print(F(" unit/mA, offset="));
  Serial.print(cal->learnedOffset, 2);
  Serial.print(F(", R2="));
  Serial.println(cal->rSquared, 3);
  
#else
  cal->hasLearnedCalibration = false;
  cal->hasTempCompensation = false;
#endif
}

static void saveCalibrationEntry(const char *clientUid, uint8_t sensorIndex, double timestamp, 
                                  float sensorReading, float verifiedLevelInches, float temperatureF, const char *notes) {
#ifdef FILESYSTEM_AVAILABLE
  // Format: clientUid\tsensorIndex\ttimestamp\tsensorReading\tverifiedLevel\ttemperatureF\tnotes
  char entry[256];
  snprintf(entry, sizeof(entry), "%s\t%d\t%.0f\t%.2f\t%.2f\t%.1f\t%s\n",
           clientUid, sensorIndex, timestamp, sensorReading, verifiedLevelInches, 
           temperatureF, notes ? notes : "");
  
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    FILE *file = fopen("/fs/calibration_log.txt", "a");
    if (file) {
      fputs(entry, file);
      fclose(file);
    }
  #else
    File file = LittleFS.open(CALIBRATION_LOG_PATH, "a");
    if (file) {
      file.print(entry);
      file.close();
    }
  #endif
  
  // Update calibration for this sensor
  SensorCalibration *cal = findOrCreateSensorCalibration(clientUid, sensorIndex);
  if (cal) {
    // recalculateCalibration will read the file and update entryCount
    recalculateCalibration(cal);
    saveCalibrationData();
  }
#endif
}

static void loadCalibrationData() {
  gSensorCalibrationCount = 0;
  
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    FILE *file = fopen("/fs/calibration_data.txt", "r");
    if (!file) return;
    
    char lineBuffer[384];
    while (fgets(lineBuffer, sizeof(lineBuffer), file) != nullptr && gSensorCalibrationCount < MAX_CALIBRATION_SENSORS) {
      String line = String(lineBuffer);
      line.trim();
      if (line.length() == 0) continue;
      
      // Parse: clientUid\tsensorIndex\tslope\toffset\trSquared\tentryCount\thasCalibration\tlastEpoch\ttempCoef\thasTempComp\ttempEntryCount
      SensorCalibration &cal = gSensorCalibrations[gSensorCalibrationCount];
      memset(&cal, 0, sizeof(SensorCalibration));
      
      int pos1 = line.indexOf('\t');
      if (pos1 < 0) continue;
      strlcpy(cal.clientUid, line.substring(0, pos1).c_str(), sizeof(cal.clientUid));
      
      int pos2 = line.indexOf('\t', pos1 + 1);
      if (pos2 < 0) continue;
      cal.sensorIndex = line.substring(pos1 + 1, pos2).toInt();
      
      int pos3 = line.indexOf('\t', pos2 + 1);
      if (pos3 < 0) continue;
      cal.learnedSlope = line.substring(pos2 + 1, pos3).toFloat();
      
      int pos4 = line.indexOf('\t', pos3 + 1);
      if (pos4 < 0) continue;
      cal.learnedOffset = line.substring(pos3 + 1, pos4).toFloat();
      
      int pos5 = line.indexOf('\t', pos4 + 1);
      if (pos5 < 0) continue;
      cal.rSquared = line.substring(pos4 + 1, pos5).toFloat();
      
      int pos6 = line.indexOf('\t', pos5 + 1);
      if (pos6 < 0) continue;
      cal.entryCount = line.substring(pos5 + 1, pos6).toInt();
      
      int pos7 = line.indexOf('\t', pos6 + 1);
      if (pos7 < 0) continue;
      cal.hasLearnedCalibration = line.substring(pos6 + 1, pos7).toInt() == 1;
      
      int pos8 = line.indexOf('\t', pos7 + 1);
      if (pos8 < 0) continue;
      cal.lastCalibrationEpoch = atof(line.substring(pos7 + 1, pos8).c_str());
      
      int pos9 = line.indexOf('\t', pos8 + 1);
      if (pos9 < 0) continue;
      cal.learnedTempCoef = line.substring(pos8 + 1, pos9).toFloat();
      
      int pos10 = line.indexOf('\t', pos9 + 1);
      if (pos10 < 0) continue;
      cal.hasTempCompensation = line.substring(pos9 + 1, pos10).toInt() == 1;
      
      cal.tempEntryCount = line.substring(pos10 + 1).toInt();
      
      gSensorCalibrationCount++;
    }
    
    fclose(file);
  #else
    if (!LittleFS.exists("/calibration_data.txt")) return;
    
    File file = LittleFS.open("/calibration_data.txt", "r");
    if (!file) return;
    
    while (file.available() && gSensorCalibrationCount < MAX_CALIBRATION_SENSORS) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      
      // Parse: clientUid\tsensorIndex\tslope\toffset\trSquared\tentryCount\thasCalibration\tlastEpoch\ttempCoef\thasTempComp\ttempEntryCount
      SensorCalibration &cal = gSensorCalibrations[gSensorCalibrationCount];
      memset(&cal, 0, sizeof(SensorCalibration));
      
      int pos1 = line.indexOf('\t');
      if (pos1 < 0) continue;
      strlcpy(cal.clientUid, line.substring(0, pos1).c_str(), sizeof(cal.clientUid));
      
      int pos2 = line.indexOf('\t', pos1 + 1);
      if (pos2 < 0) continue;
      cal.sensorIndex = line.substring(pos1 + 1, pos2).toInt();
      
      int pos3 = line.indexOf('\t', pos2 + 1);
      if (pos3 < 0) continue;
      cal.learnedSlope = line.substring(pos2 + 1, pos3).toFloat();
      
      int pos4 = line.indexOf('\t', pos3 + 1);
      if (pos4 < 0) continue;
      cal.learnedOffset = line.substring(pos3 + 1, pos4).toFloat();
      
      int pos5 = line.indexOf('\t', pos4 + 1);
      if (pos5 < 0) continue;
      cal.rSquared = line.substring(pos4 + 1, pos5).toFloat();
      
      int pos6 = line.indexOf('\t', pos5 + 1);
      if (pos6 < 0) continue;
      cal.entryCount = line.substring(pos5 + 1, pos6).toInt();
      
      int pos7 = line.indexOf('\t', pos6 + 1);
      if (pos7 < 0) continue;
      cal.hasLearnedCalibration = line.substring(pos6 + 1, pos7).toInt() == 1;
      
      int pos8 = line.indexOf('\t', pos7 + 1);
      if (pos8 < 0) continue;
      cal.lastCalibrationEpoch = atof(line.substring(pos7 + 1, pos8).c_str());
      
      int pos9 = line.indexOf('\t', pos8 + 1);
      if (pos9 < 0) continue;
      cal.learnedTempCoef = line.substring(pos8 + 1, pos9).toFloat();
      
      int pos10 = line.indexOf('\t', pos9 + 1);
      if (pos10 < 0) continue;
      cal.hasTempCompensation = line.substring(pos9 + 1, pos10).toInt() == 1;
      
      cal.tempEntryCount = line.substring(pos10 + 1).toInt();
      
      gSensorCalibrationCount++;
    }
    
    file.close();
  #endif
#endif
  
  Serial.print(F("Loaded "));
  Serial.print(gSensorCalibrationCount);
  Serial.println(F(" sensor calibration records"));
}

static void saveCalibrationData() {
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Accumulate into String, then atomic write (~80 bytes/line)
    String output;
    output.reserve(gSensorCalibrationCount * 100);
    for (uint8_t i = 0; i < gSensorCalibrationCount; ++i) {
      SensorCalibration &cal = gSensorCalibrations[i];
      // Format: uid\tsensorIndex\tslope\toffset\trSquared\tentryCount\thasLearned\tlastEpoch\ttempCoef\thasTempComp\ttempEntryCount
      char line[128];
      snprintf(line, sizeof(line), "%s\t%d\t%.6f\t%.2f\t%.4f\t%d\t%d\t%.0f\t%.6f\t%d\t%d\n",
               cal.clientUid, cal.sensorIndex, cal.learnedSlope, cal.learnedOffset,
               cal.rSquared, cal.entryCount, cal.hasLearnedCalibration ? 1 : 0,
               cal.lastCalibrationEpoch, cal.learnedTempCoef,
               cal.hasTempCompensation ? 1 : 0, cal.tempEntryCount);
      output += line;
    }
    serverWriteFileAtomic("/fs/calibration_data.txt",
                          output.c_str(), output.length());
  #else
    // LittleFS branch: accumulate into String, then atomic write
    String output;
    output.reserve(gSensorCalibrationCount * 100);
    for (uint8_t i = 0; i < gSensorCalibrationCount; ++i) {
      SensorCalibration &cal = gSensorCalibrations[i];
      char line[128];
      snprintf(line, sizeof(line), "%s\t%d\t%.6f\t%.2f\t%.4f\t%d\t%d\t%.0f\t%.6f\t%d\t%d\n",
               cal.clientUid, cal.sensorIndex, cal.learnedSlope, cal.learnedOffset,
               cal.rSquared, cal.entryCount, cal.hasLearnedCalibration ? 1 : 0,
               cal.lastCalibrationEpoch, cal.learnedTempCoef,
               cal.hasTempCompensation ? 1 : 0, cal.tempEntryCount);
      output += line;
    }
    tankalarm_littlefs_write_file_atomic("/calibration_data.txt",
            (const uint8_t *)output.c_str(), output.length());
  #endif
#endif
}

static void handleCalibrationGet(EthernetClient &client) {
  // Size: calibrations(20 x ~200 bytes) + logs(50 x ~200 bytes) + overhead
  // ~4000 + ~10000 + 512 = ~15KB; ArduinoJson v7 auto-sizes
  JsonDocument doc;
  
  // Add calibration status for each sensor
  JsonArray calibrationsArr = doc["calibrations"].to<JsonArray>();
  for (uint8_t i = 0; i < gSensorCalibrationCount; ++i) {
    SensorCalibration &cal = gSensorCalibrations[i];
    JsonObject obj = calibrationsArr.add<JsonObject>();
    obj["clientUid"] = cal.clientUid;
    obj["sensorIndex"] = cal.sensorIndex;
    obj["learnedSlope"] = cal.learnedSlope;
    obj["learnedOffset"] = cal.learnedOffset;
    obj["learnedTempCoef"] = cal.learnedTempCoef;
    obj["hasLearnedCalibration"] = cal.hasLearnedCalibration;
    obj["hasTempCompensation"] = cal.hasTempCompensation;
    obj["entryCount"] = cal.entryCount;
    obj["tempEntryCount"] = cal.tempEntryCount;
    obj["rSquared"] = cal.rSquared;
    obj["lastCalibrationEpoch"] = cal.lastCalibrationEpoch;
    obj["originalMaxValue"] = cal.originalMaxValue;
    obj["sensorType"] = cal.sensorType;
    // Quality metrics for warnings
    obj["minSensorMa"] = cal.minSensorMa;
    obj["maxSensorMa"] = cal.maxSensorMa;
    obj["minLevelInches"] = cal.minLevelInches;
    obj["maxLevelInches"] = cal.maxLevelInches;
    // Implied SG (back-derived from learnedSlope) and drift from admin's fluid preset.
    // 0.0 means "not derivable" (e.g., gas monitor, ultrasonic, or missing config).
    obj["impliedSpecificGravity"] = cal.impliedSpecificGravity;
    obj["sgDriftFromPreset"] = cal.specificGravityDriftFromPreset;
  }
  
  // Add recent calibration log entries
  // Format: clientUid\tsensorIndex\ttimestamp\tsensorReading\tverifiedLevel\ttemperatureF\tnotes
  JsonArray logsArr = doc["logs"].to<JsonArray>();
  
#ifdef FILESYSTEM_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    FILE *file = fopen("/fs/calibration_log.txt", "r");
    if (file) {
      char lineBuffer[256];
      int count = 0;
      while (fgets(lineBuffer, sizeof(lineBuffer), file) != nullptr && count < 50) {
        String line = String(lineBuffer);
        line.trim();
        if (line.length() == 0) continue;
        
        // Parse entry
        int pos1 = line.indexOf('\t');
        if (pos1 < 0) continue;
        String uid = line.substring(0, pos1);
        
        int pos2 = line.indexOf('\t', pos1 + 1);
        if (pos2 < 0) continue;
        int sensorIdx = line.substring(pos1 + 1, pos2).toInt();
        
        int pos3 = line.indexOf('\t', pos2 + 1);
        if (pos3 < 0) continue;
        double timestamp = atof(line.substring(pos2 + 1, pos3).c_str());
        
        int pos4 = line.indexOf('\t', pos3 + 1);
        if (pos4 < 0) continue;
        float sensorReading = line.substring(pos3 + 1, pos4).toFloat();
        
        int pos5 = line.indexOf('\t', pos4 + 1);
        float verifiedLevel = 0.0f;
        float temperatureF = TEMPERATURE_UNAVAILABLE;
        String notes = "";
        
        if (pos5 < 0) {
          // Legacy format (no temperature field)
          verifiedLevel = line.substring(pos4 + 1).toFloat();
        } else {
          verifiedLevel = line.substring(pos4 + 1, pos5).toFloat();
          
          // Check for temperature field (new format) vs notes only (old format)
          int pos6 = line.indexOf('\t', pos5 + 1);
          if (pos6 < 0) {
            // Could be old format (notes only) or new format (temp only, no notes)
            String remaining = line.substring(pos5 + 1);
            // Try to parse as temperature (numeric)
            float tempVal = remaining.toFloat();
            if (tempVal != 0.0f || remaining.startsWith("0") || remaining.startsWith("-")) {
              temperatureF = tempVal;
            } else {
              // Non-numeric: treat as notes (old format)
              notes = remaining;
            }
          } else {
            // New format with both temperature and notes
            temperatureF = line.substring(pos5 + 1, pos6).toFloat();
            notes = line.substring(pos6 + 1);
          }
        }
        
        JsonObject logObj = logsArr.add<JsonObject>();
        logObj["clientUid"] = uid;
        logObj["sensorIndex"] = sensorIdx;
        logObj["timestamp"] = timestamp;
        logObj["sensorReading"] = sensorReading;
        logObj["verifiedLevelInches"] = verifiedLevel;
        if (temperatureF != TEMPERATURE_UNAVAILABLE) {
          logObj["temperatureF"] = temperatureF;
        }
        logObj["notes"] = notes;
        count++;
      }
      fclose(file);
    }
  #else
    if (LittleFS.exists(CALIBRATION_LOG_PATH)) {
      File file = LittleFS.open(CALIBRATION_LOG_PATH, "r");
      if (file) {
        int count = 0;
        while (file.available() && count < 50) {
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() == 0) continue;
          
          // Parse entry
          int pos1 = line.indexOf('\t');
          if (pos1 < 0) continue;
          String uid = line.substring(0, pos1);
          
          int pos2 = line.indexOf('\t', pos1 + 1);
          if (pos2 < 0) continue;
          int sensorIdx = line.substring(pos1 + 1, pos2).toInt();
          
          int pos3 = line.indexOf('\t', pos2 + 1);
          if (pos3 < 0) continue;
          double timestamp = atof(line.substring(pos2 + 1, pos3).c_str());
          
          int pos4 = line.indexOf('\t', pos3 + 1);
          if (pos4 < 0) continue;
          float sensorReading = line.substring(pos3 + 1, pos4).toFloat();
          
          int pos5 = line.indexOf('\t', pos4 + 1);
          float verifiedLevel = 0.0f;
          float temperatureF = TEMPERATURE_UNAVAILABLE;
          String notes = "";
          
          if (pos5 < 0) {
            verifiedLevel = line.substring(pos4 + 1).toFloat();
          } else {
            verifiedLevel = line.substring(pos4 + 1, pos5).toFloat();
            
            // Check for temperature field (new format) vs notes only (old format)
            int pos6 = line.indexOf('\t', pos5 + 1);
            if (pos6 < 0) {
              String remaining = line.substring(pos5 + 1);
              float tempVal = remaining.toFloat();
              if (tempVal != 0.0f || remaining.startsWith("0") || remaining.startsWith("-")) {
                temperatureF = tempVal;
              } else {
                notes = remaining;
              }
            } else {
              temperatureF = line.substring(pos5 + 1, pos6).toFloat();
              notes = line.substring(pos6 + 1);
            }
          }
          
          JsonObject logObj = logsArr.add<JsonObject>();
          logObj["clientUid"] = uid;
          logObj["sensorIndex"] = sensorIdx;
          logObj["timestamp"] = timestamp;
          logObj["sensorReading"] = sensorReading;
          logObj["verifiedLevelInches"] = verifiedLevel;
          if (temperatureF != TEMPERATURE_UNAVAILABLE) {
            logObj["temperatureF"] = temperatureF;
          }
          logObj["notes"] = notes;
          count++;
        }
        file.close();
      }
    }
  #endif
#endif
  
  String json;
  if (serializeJson(doc, json) == 0) {
    respondStatus(client, 500, F("Failed to encode calibration data"));
    return;
  }
  respondJson(client, json);
}

static void handleCalibrationPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  // Require PIN authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }
  
  const char *clientUid = doc["clientUid"].as<const char *>();
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, F("Missing clientUid"));
    return;
  }
  
  if (!doc["sensorIndex"]) {
    respondStatus(client, 400, F("Missing sensorIndex"));
    return;
  }
  uint8_t sensorIndex = doc["sensorIndex"].as<uint8_t>();
  
  if (!doc["verifiedLevelInches"]) {
    respondStatus(client, 400, F("Missing verifiedLevelInches"));
    return;
  }
  float verifiedLevelInches = doc["verifiedLevelInches"].as<float>();
  
  // Optional fields
  float sensorReading = doc["sensorReading"].as<float>();
  if (!doc["sensorReading"] || sensorReading < 4.0f || sensorReading > 20.0f) {
    // Try to get raw sensorMa from sensor record (sent directly from client)
    sensorReading = 0.0f;
    
    // Look up sensor record to get raw sensorMa from latest telemetry
    for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
      if (strcmp(gSensorRecords[i].clientUid, clientUid) == 0 && 
          gSensorRecords[i].sensorIndex == sensorIndex) {
        // Use raw sensorMa if available (sent directly from client device)
        if (gSensorRecords[i].sensorMa >= 4.0f && gSensorRecords[i].sensorMa <= 20.0f) {
          sensorReading = gSensorRecords[i].sensorMa;
          Serial.print(F("Using raw sensorMa from telemetry: "));
          Serial.print(sensorReading, 2);
          Serial.println(F(" mA"));
        }
        break;
      }
    }
  }
  
  double timestamp = doc["timestamp"].as<double>();
  if (timestamp <= 0.0) {
    timestamp = currentEpoch();
  }
  
  const char *notes = doc["notes"].as<const char *>();
  
  // Validate sensor reading - warn if not in valid range
  bool sensorReadingValid = (sensorReading >= 4.0f && sensorReading <= 20.0f);
  
  // Fetch NWS weather data for temperature compensation
  // This will look up the client's GPS location and get a 6-hour average temperature
  // If no location is cached, it will request it from the client
  float temperatureF = nwsGetCalibrationTemperature(clientUid, timestamp);
  bool locationRequested = false;
  
  // Check if we had to request location (no cached location)
  ClientMetadata *meta = findClientMetadata(clientUid);
  if (temperatureF == TEMPERATURE_UNAVAILABLE && meta && 
      (meta->latitude == 0.0f || meta->longitude == 0.0f)) {
    locationRequested = true;
  }
  
  // Log temperature data
  if (temperatureF != TEMPERATURE_UNAVAILABLE) {
    Serial.print(F("NWS temperature at calibration: "));
    Serial.print(temperatureF, 1);
    Serial.println(F("°F (6-hour avg)"));
  } else if (locationRequested) {
    Serial.println(F("NWS temperature: location requested from client"));
  } else {
    Serial.println(F("NWS temperature data unavailable (API error)"));
  }
  
  // Save the calibration entry with temperature
  saveCalibrationEntry(clientUid, sensorIndex, timestamp, sensorReading, verifiedLevelInches, temperatureF, notes);
  
  Serial.print(F("Calibration entry added for "));
  Serial.print(clientUid);
  Serial.print(F(" sensor "));
  Serial.print(sensorIndex);
  Serial.print(F(": "));
  Serial.print(verifiedLevelInches, 1);
  Serial.print(F(" in @ "));
  Serial.print(sensorReading, 2);
  Serial.println(F(" mA"));
  
  // Build response message including temperature info
  String responseMsg = F("Calibration entry saved");
  if (temperatureF != TEMPERATURE_UNAVAILABLE) {
    responseMsg += F(" (temp: ");
    responseMsg += String(temperatureF, 1);
    responseMsg += F("°F)");
  } else if (locationRequested) {
    responseMsg += F(" (location requested - temp will be available for future readings)");
  }
  
  if (!sensorReadingValid) {
    Serial.println(F("Warning: Sensor reading not in valid 4-20mA range, entry logged but won't be used for regression"));
    respondStatus(client, 200, F("Calibration entry saved (note: sensor reading outside 4-20mA range won't be used for calibration)"));
  } else {
    respondStatus(client, 200, responseMsg);
  }
}

static void handleCalibrationDelete(EthernetClient &client, const String &body) {
  // Parse JSON body to get clientUid and sensorIndex
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    respondStatus(client, 400, F("Invalid JSON"));
    return;
  }

  // Require PIN authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }
  
  const char *clientUid = doc["clientUid"].as<const char *>();
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, F("Missing clientUid"));
    return;
  }
  
  if (!doc["sensorIndex"]) {
    respondStatus(client, 400, F("Missing sensorIndex"));
    return;
  }
  uint8_t sensorIndex = doc["sensorIndex"].as<uint8_t>();
  
  Serial.print(F("Reset calibration requested for "));
  Serial.print(clientUid);
  Serial.print(F(" sensor "));
  Serial.println(sensorIndex);
  
  // Find and reset the SensorCalibration entry
  bool foundCalibration = false;
  for (uint8_t i = 0; i < gSensorCalibrationCount; ++i) {
    if (strcmp(gSensorCalibrations[i].clientUid, clientUid) == 0 && 
        gSensorCalibrations[i].sensorIndex == sensorIndex) {
      // Reset calibration fields but keep identity
      gSensorCalibrations[i].learnedSlope = 0.0f;
      gSensorCalibrations[i].learnedOffset = 0.0f;
      gSensorCalibrations[i].learnedTempCoef = 0.0f;
      gSensorCalibrations[i].hasLearnedCalibration = false;
      gSensorCalibrations[i].hasTempCompensation = false;
      gSensorCalibrations[i].entryCount = 0;
      gSensorCalibrations[i].tempEntryCount = 0;
      gSensorCalibrations[i].rSquared = 0.0f;
      gSensorCalibrations[i].lastCalibrationEpoch = 0.0;
      gSensorCalibrations[i].minSensorMa = 0.0f;
      gSensorCalibrations[i].maxSensorMa = 0.0f;
      gSensorCalibrations[i].minLevelInches = 0.0f;
      gSensorCalibrations[i].maxLevelInches = 0.0f;
      foundCalibration = true;
      Serial.println(F("SensorCalibration entry reset"));
      break;
    }
  }
  
#ifdef FILESYSTEM_AVAILABLE
  // Remove entries from calibration_log.txt for this sensor
  // We need to read the entire file, filter out matching entries, then rewrite
  int removedCount = 0;
  
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    // Read all entries, filter, then rewrite
    std::vector<String> keepEntries;
    
    FILE *file = fopen("/fs/calibration_log.txt", "r");
    if (file) {
      char lineBuffer[256];
      while (fgets(lineBuffer, sizeof(lineBuffer), file) != nullptr) {
        String line = String(lineBuffer);
        line.trim();
        if (line.length() == 0) continue;
        
        // Parse clientUid and sensorIndex from entry
        int pos1 = line.indexOf('\t');
        if (pos1 < 0) {
          keepEntries.push_back(line);
          continue;
        }
        String entryUid = line.substring(0, pos1);
        
        int pos2 = line.indexOf('\t', pos1 + 1);
        if (pos2 < 0) {
          keepEntries.push_back(line);
          continue;
        }
        int entrySensor = line.substring(pos1 + 1, pos2).toInt();
        
        // Check if this entry matches the sensor to delete
        if (entryUid == String(clientUid) && entrySensor == (int)sensorIndex) {
          removedCount++;
        } else {
          keepEntries.push_back(line);
        }
      }
      fclose(file);
    }
    
    // Rewrite the file without the deleted entries — atomic write
    if (removedCount > 0) {
      String output;
      output.reserve(keepEntries.size() * 128);
      for (size_t i = 0; i < keepEntries.size(); i++) {
        output += keepEntries[i];
        output += '\n';
      }
      if (!serverWriteFileAtomic("/fs/calibration_log.txt",
                                 output.c_str(), output.length())) {
        Serial.println(F("Failed to rewrite calibration log"));
      } else {
        Serial.print(F("Removed "));
        Serial.print(removedCount);
        Serial.println(F(" calibration log entries"));
      }
    }
  #else
    // LittleFS implementation
    if (LittleFS.exists(CALIBRATION_LOG_PATH)) {
      std::vector<String> keepEntries;
      
      File file = LittleFS.open(CALIBRATION_LOG_PATH, "r");
      if (file) {
        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() == 0) continue;
          
          int pos1 = line.indexOf('\t');
          if (pos1 < 0) {
            keepEntries.push_back(line);
            continue;
          }
          String entryUid = line.substring(0, pos1);
          
          int pos2 = line.indexOf('\t', pos1 + 1);
          if (pos2 < 0) {
            keepEntries.push_back(line);
            continue;
          }
          int entrySensor = line.substring(pos1 + 1, pos2).toInt();
          
          if (entryUid == String(clientUid) && entrySensor == (int)sensorIndex) {
            removedCount++;
          } else {
            keepEntries.push_back(line);
          }
        }
        file.close();
      }
      
      if (removedCount > 0) {
        // Accumulate into String, then atomic write
        String output;
        output.reserve(keepEntries.size() * 128);
        for (size_t i = 0; i < keepEntries.size(); i++) {
          output += keepEntries[i];
          output += '\n';
        }
        if (!tankalarm_littlefs_write_file_atomic(CALIBRATION_LOG_PATH,
                (const uint8_t *)output.c_str(), output.length())) {
          Serial.println(F("Failed to rewrite calibration log"));
        } else {
          Serial.print(F("Removed "));
          Serial.print(removedCount);
          Serial.println(F(" calibration log entries"));
        }
      }
    }
  #endif
#endif
  
  // Save updated calibration data
  saveCalibrationData();
  
  String responseMsg = F("Calibration reset for sensor ");
  responseMsg += String(sensorIndex);
  if (removedCount > 0) {
    responseMsg += F(", removed ");
    responseMsg += String(removedCount);
    responseMsg += F(" log entries");
  }
  
  respondStatus(client, 200, responseMsg);
}

// ============================================================================
// Notecard Status Web API Handler
// ============================================================================

static void handleNotecardStatusGet(EthernetClient &client) {
  bool connected = false;
  String syncMode = "unknown";
  String productUid = (gConfig.productUid[0] != '\0') ? String(gConfig.productUid) : "";

  // Live I2C check: send hub.status to the Notecard
  J *req = notecard.newRequest("hub.status");
  if (req) {
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) {
      const char *err = JGetString(rsp, "err");
      if (!err || err[0] == '\0') {
        connected = true;
        const char *st = JGetString(rsp, "status");
        if (st && st[0] != '\0') {
          syncMode = String(st);
        }
      }
      notecard.deleteResponse(rsp);
    }
  }

  // Build JSON response
  JsonDocument doc;
  doc["connected"] = connected;
  doc["productUid"] = productUid;
  doc["serverUid"] = String(gServerUid);
  doc["syncMode"] = syncMode;
  String responseStr;
  serializeJson(doc, responseStr);
  respondJson(client, responseStr);
}

// ============================================================================
// Transmission Log Web API Handler
// ============================================================================

static void handleTransmissionLogGet(EthernetClient &client) {
  // Build JSON response from ring buffer, newest first
  JsonDocument doc;
  JsonArray entries = doc["entries"].to<JsonArray>();

  for (int i = (int)gTransmissionLogCount - 1; i >= 0; i--) {
    int idx = (gTransmissionLogWriteIndex - 1 - ((int)gTransmissionLogCount - 1 - i) + MAX_TRANSMISSION_LOG_ENTRIES) % MAX_TRANSMISSION_LOG_ENTRIES;
    const TransmissionLogEntry &e = gTransmissionLog[idx];

    JsonObject obj = entries.add<JsonObject>();
    obj["timestamp"] = e.timestamp;
    obj["site"] = e.siteName;
    obj["client"] = e.clientUid;
    obj["type"] = e.messageType;
    obj["status"] = e.status;
    obj["detail"] = e.detail;
  }

  doc["count"] = gTransmissionLogCount;
  String responseStr;
  serializeJson(doc, responseStr);
  respondJson(client, responseStr);
}

// ============================================================================
// DFU (Device Firmware Update) Web API Handlers
// ============================================================================

static void handleGitHubUpdateStatusGet(EthernetClient &client) {
  JsonDocument doc;
  doc["available"] = gGitHubUpdateAvailable;
  doc["latestVersion"] = gGitHubLatestVersion[0] ? gGitHubLatestVersion : FIRMWARE_VERSION;
  doc["currentVersion"] = FIRMWARE_VERSION;
  doc["releaseUrl"] = gGitHubReleaseUrl;
  doc["assetAvailable"] = gGitHubAssetAvailable;
  doc["assetUrl"] = gGitHubAssetUrl;
  doc["assetSize"] = gGitHubAssetSize;
  doc["assetSha256"] = gGitHubAssetSha256;
  doc["directInstallReady"] = isGitHubDirectUpdateReady();
  doc["assetNamingConvention"] = "TankAlarm-Server-vX.Y.Z.bin / TankAlarm-FTPS-Test-vX.Y.Z.bin";
  // Count clients whose firmware lags behind the latest GitHub release.
  // Only meaningful once we have a confirmed latest version from GitHub.
  uint8_t outdatedCount = 0;
  if (gConfig.checkClientVersionAlerts && gGitHubLatestVersion[0] != '\0') {
    for (uint8_t i = 0; i < gClientMetadataCount; ++i) {
      const ClientMetadata &m = gClientMetadata[i];
      if (m.firmwareVersion[0] != '\0' &&
          strcmp(m.firmwareVersion, gGitHubLatestVersion) != 0) {
        ++outdatedCount;
      }
    }
  }
  doc["outdatedClientCount"] = outdatedCount;
  doc["outdatedViewerCount"] = 0;  // Reserved: viewer version tracking not yet implemented
  // Signal that a Notehub DFU update is pending and ALERT_DFU policy is active.
  // AUTO_DFU applies the update automatically, so no dashboard alert is needed for that policy.
  doc["dfuUpdateAvailable"] = (gConfig.updatePolicy == UPDATE_POLICY_ALERT_DFU) && gDfuUpdateAvailable;
  String responseStr;
  serializeJson(doc, responseStr);
  respondJson(client, responseStr);
}

static void handleDfuStatusGet(EthernetClient &client) {
  JsonDocument doc;
  GitHubFirmwareTargetState selectedState = gSelectedFirmwareTargetState;
  if (selectedState.target == FIRMWARE_TARGET_SERVER &&
      !selectedState.checked && selectedState.error[0] == '\0') {
    loadServerGitHubTargetState(selectedState);
  }

  const bool allowNotehubFallback =
      (selectedState.target == FIRMWARE_TARGET_SERVER) && isNotehubDfuReady();
  char selectedTargetStatusText[160] = {0};
  bool selectedTargetInstallEnabled = false;
  buildSelectedFirmwareTargetStatus(selectedState,
                                    FIRMWARE_TARGET_SERVER,
                                    FIRMWARE_VERSION,
                                    allowNotehubFallback,
                                    selectedTargetStatusText,
                                    sizeof(selectedTargetStatusText),
                                    selectedTargetInstallEnabled);

  doc["currentVersion"] = FIRMWARE_VERSION;
  doc["buildDate"] = FIRMWARE_BUILD_DATE;
  doc["buildTime"] = FIRMWARE_BUILD_TIME;
  doc["updateAvailable"] = gDfuUpdateAvailable;
  doc["availableVersion"] = gDfuUpdateAvailable ? gDfuVersion : "";
  doc["availableLength"] = gDfuUpdateAvailable ? gDfuFirmwareLength : 0;
  doc["dfuMode"] = gDfuMode;
  doc["dfuInProgress"] = gDfuInProgress;
  doc["dfuError"] = gDfuError;
  doc["updatePolicy"] = gConfig.updatePolicy;
  doc["githubAssetAvailable"] = gGitHubAssetAvailable;
  doc["githubDirectReady"] = isGitHubDirectUpdateReady();
  doc["selectedTarget"] = firmwareTargetId(selectedState.target);
  doc["selectedTargetLabel"] = firmwareTargetLabel(selectedState.target);
  doc["selectedTargetChecked"] = selectedState.checked;
  doc["selectedTargetUpdateAvailable"] = selectedState.updateAvailable;
  doc["selectedTargetAssetAvailable"] = selectedState.assetAvailable;
  doc["selectedTargetAvailableVersion"] = selectedState.latestVersion;
  doc["selectedTargetAssetNaming"] = firmwareTargetAssetNamingConvention(selectedState.target);
  doc["selectedTargetDirectReady"] = isGitHubDirectTargetReady(selectedState);
  doc["selectedTargetCanFallbackToNotehub"] = allowNotehubFallback;
  doc["selectedTargetStatusText"] = selectedTargetStatusText;
  doc["selectedTargetInstallEnabled"] = selectedTargetInstallEnabled;
  doc["selectedTargetError"] = selectedState.error;
  float bestVoltage = (gInputVoltage > 0.5f) ? gInputVoltage : gNotecardVoltage;
  doc["voltage"] = bestVoltage;
  doc["notecardVoltage"] = gNotecardVoltage;
  doc["inputVoltage"] = gInputVoltage;
  doc["voltageEpoch"] = gVoltageEpoch;
  doc["serverTime"] = currentEpoch();
  String responseStr;
  serializeJson(doc, responseStr);
  respondJson(client, responseStr);
}

// Brute-force dedup of sensor records (linear scan — does not rely on hash table)
// Keeps the most recently updated record for each clientUid+sensorIndex pair.
// Returns the number of duplicates removed.
static uint8_t deduplicateSensorRecordsLinear() {
  uint8_t removed = 0;
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    for (uint8_t j = i + 1; j < gSensorRecordCount; ) {
      if (strcmp(gSensorRecords[i].clientUid, gSensorRecords[j].clientUid) == 0 &&
          gSensorRecords[i].sensorIndex == gSensorRecords[j].sensorIndex) {
        // Duplicate found — keep the one with the newer lastUpdateEpoch
        if (gSensorRecords[j].lastUpdateEpoch > gSensorRecords[i].lastUpdateEpoch) {
          memcpy(&gSensorRecords[i], &gSensorRecords[j], sizeof(SensorRecord));
        }
        // Remove record j by moving last record into its slot
        if (j < gSensorRecordCount - 1) {
          memcpy(&gSensorRecords[j], &gSensorRecords[gSensorRecordCount - 1], sizeof(SensorRecord));
        }
        gSensorRecordCount--;
        removed++;
        // Don't increment j — re-check the slot we just moved into
      } else {
        j++;
      }
    }
  }
  if (removed > 0) {
    rebuildSensorHashTable();
    gSensorRegistryDirty = true;
    Serial.print(F("Dedup removed "));
    Serial.print(removed);
    Serial.println(F(" duplicate sensor records"));
    addServerSerialLog("Duplicate sensor records removed", "info", "registry");
  }
  return removed;
}

// Debug endpoint: POST /api/debug/sensors with {"pin":"XXXX"} — dump raw sensor records
// POST /api/debug/sensors with {"pin":"XXXX","action":"dedup"} — force dedup
static void handleDebugSensors(EthernetClient &client, const String &method, const String &body, const String &queryString) {
  if (method != "POST") {
    respondStatus(client, 405, "Use POST with {\"pin\":\"XXXX\"} in body");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  const char *action = doc["action"] | "";
  if (strcmp(action, "dedup") == 0) {
    uint8_t removed = deduplicateSensorRecordsLinear();
    if (removed > 0) {
      saveSensorRegistry();
      gSensorRegistryDirty = false;
    }
    String rsp = "{\"removed\":" + String(removed) + ",\"remaining\":" + String(gSensorRecordCount) + "}";
    respondJson(client, rsp);
    return;
  }
  String responseStr = "{\"count\":" + String(gSensorRecordCount) + ",\"records\":[";
  for (uint8_t i = 0; i < gSensorRecordCount; ++i) {
    const SensorRecord &rec = gSensorRecords[i];
    if (i > 0) responseStr += ",";
    responseStr += "{\"i\":" + String(i);
    responseStr += ",\"c\":\"" + String(rec.clientUid) + "\"";
    responseStr += ",\"k\":" + String(rec.sensorIndex);
    responseStr += ",\"n\":\"" + String(rec.label) + "\"";
    responseStr += ",\"st\":\"" + String(rec.sensorType) + "\"";
    responseStr += ",\"ot\":\"" + String(rec.objectType) + "\"";
    responseStr += ",\"mu\":\"" + String(rec.measurementUnit) + "\"";
    responseStr += ",\"l\":" + String(rec.levelInches, 3);
    responseStr += ",\"ma\":" + String(rec.sensorMa, 2);
    responseStr += ",\"a\":" + String(rec.alarmActive ? "true" : "false");
    responseStr += ",\"at\":\"" + String(rec.alarmType) + "\"";
    responseStr += ",\"u\":" + String(rec.lastUpdateEpoch, 0);
    responseStr += "}";
  }
  responseStr += "]}";
  respondJson(client, responseStr);
}

static void handleDfuCheckPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  uint8_t target = FIRMWARE_TARGET_SERVER;
  if (!parseFirmwareTargetValue(doc["target"] | "server", target)) {
    respondStatus(client, 400, "Invalid firmware target");
    return;
  }
  gSelectedFirmwareTargetState.target = target;

  Serial.print(F("Manual firmware check triggered for target: "));
  Serial.println(firmwareTargetLabel(target));

  if (target == FIRMWARE_TARGET_SERVER) {
    if (strcmp(gDfuMode, "error") == 0) {
      Serial.println(F("DFU in error state - clearing and resetting..."));

      J *stopReq = notecard.newRequest("dfu.status");
      if (stopReq) {
        JAddBoolToObject(stopReq, "stop", true);
        JAddStringToObject(stopReq, "name", "user");
        J *stopRsp = notecard.requestAndResponse(stopReq);
        if (stopRsp) {
          const char *stopErr = JGetString(stopRsp, "err");
          if (stopErr && stopErr[0] != '\0') {
            Serial.print(F("dfu.status stop error: "));
            Serial.println(stopErr);
          } else {
            Serial.println(F("DFU error state cleared via dfu.status stop"));
          }
          notecard.deleteResponse(stopRsp);
        }
      }
      safeSleep(1000);

      tankalarm_enableIapDfu(notecard);
      safeSleep(500);

      J *syncReq = notecard.newRequest("hub.sync");
      if (syncReq) {
        J *syncRsp = notecard.requestAndResponse(syncReq);
        if (syncRsp) {
          Serial.println(F("hub.sync triggered for fresh firmware catalog"));
          notecard.deleteResponse(syncRsp);
        }
      }
      safeSleep(2000);

      strlcpy(gDfuMode, "idle", sizeof(gDfuMode));
      gDfuError[0] = '\0';
      gDfuUpdateAvailable = false;
      gDfuVersion[0] = '\0';
      gDfuFirmwareLength = 0;
    }

    checkForFirmwareUpdate();
  }

  if (!checkGitHubReleaseForTarget(target,
                                   FIRMWARE_VERSION,
                                   FIRMWARE_TARGET_SERVER,
                                   gSelectedFirmwareTargetState,
                                   true)) {
    Serial.print(F("Selected firmware GitHub check failed: "));
    Serial.println(gSelectedFirmwareTargetState.error[0] != '\0'
                       ? gSelectedFirmwareTargetState.error
                       : "unknown error");
  }

  // Return the updated status
  handleDfuStatusGet(client);
}

static void handleDfuEnablePost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }

  // Require valid PIN for authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  uint8_t target = FIRMWARE_TARGET_SERVER;
  if (!parseFirmwareTargetValue(doc["target"] | firmwareTargetId(gSelectedFirmwareTargetState.target), target)) {
    respondStatus(client, 400, "Invalid firmware target");
    return;
  }

  if (gSelectedFirmwareTargetState.target != target ||
      (!gSelectedFirmwareTargetState.checked && gSelectedFirmwareTargetState.error[0] == '\0')) {
    if (!checkGitHubReleaseForTarget(target,
                                     FIRMWARE_VERSION,
                                     FIRMWARE_TARGET_SERVER,
                                     gSelectedFirmwareTargetState,
                                     true)) {
      gSelectedFirmwareTargetState.target = target;
    }
  }

  const bool allowNotehubFallback =
      (target == FIRMWARE_TARGET_SERVER) && isNotehubDfuReady();
  const bool directInstallAllowed =
      isGitHubDirectTargetReady(gSelectedFirmwareTargetState) &&
      gSelectedFirmwareTargetState.updateAvailable;

  String responseStr;
  if (directInstallAllowed) {
    responseStr = String("{\"success\":true,\"message\":\"") +
                  firmwareTargetLabel(target) +
                  " install starting from GitHub\"}";
    respondJson(client, responseStr);
    client.stop();
    safeSleep(100);

    String directStatus;
    if (!attemptGitHubDirectInstallForTarget(gSelectedFirmwareTargetState, directStatus) &&
        allowNotehubFallback) {
      Serial.println(F("Selected GitHub install failed; falling back to Notehub DFU"));
      enableDfuMode();
    }
    return;
  }

  if (allowNotehubFallback) {
    responseStr = "{\"success\":true,\"message\":\"TankAlarm Server install starting from Notehub DFU\"}";
    respondJson(client, responseStr);
    client.stop();
    safeSleep(100);
    enableDfuMode();
    return;
  }

  if (gSelectedFirmwareTargetState.error[0] != '\0') {
    respondStatus(client, 409, gSelectedFirmwareTargetState.error);
    return;
  }

  if (target == FIRMWARE_TARGET_FTPS_TEST) {
    respondStatus(client, 409, "No installable FTPS Test firmware asset is ready on GitHub");
    return;
  }

  if (gConfig.updatePolicy == UPDATE_POLICY_ALERT_GITHUB ||
      gConfig.updatePolicy == UPDATE_POLICY_AUTO_GITHUB) {
    if (!isGitHubDirectUpdateReady()) {
      checkGitHubForUpdate();
    }

    if (isGitHubDirectUpdateReady()) {
      responseStr = "{\"success\":true,\"message\":\"GitHub Direct install starting\"}";
      respondJson(client, responseStr);
      client.stop();
      safeSleep(100);

      String directStatus;
      if (!attemptGitHubDirectInstall(directStatus) && isNotehubDfuReady()) {
        Serial.println(F("GitHub Direct install failed; falling back to Notehub DFU"));
        enableDfuMode();
      }
      return;
    }

    respondStatus(client, 409, "No ready GitHub or Notehub firmware update available");
    return;
  }

  if (!isNotehubDfuReady()) {
    respondStatus(client, 409, "No ready Notehub firmware update available");
    return;
  }

  // Notehub DFU (default)
  responseStr = "{\"success\":true,\"message\":\"DFU mode enabled - device will update and restart\"}";
  respondJson(client, responseStr);
  client.stop();
  safeSleep(100);
  enableDfuMode();
}

// ============================================================================
// Location Request Web API Handlers
// ============================================================================

static void handleLocationRequestPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }

  // Require PIN authentication
  const char *pinValue = doc["pin"].as<const char *>();
  if (!requireValidPin(client, pinValue)) {
    return;
  }

  const char *clientUid = doc["clientUid"].as<const char *>();
  if (!clientUid || strlen(clientUid) == 0) {
    respondStatus(client, 400, "Missing clientUid");
    return;
  }

  // Send location request to client
  if (sendLocationRequest(clientUid)) {
    String responseStr = "{\"success\":true,\"message\":\"Location request sent to client\"}";
    respondJson(client, responseStr);
  } else {
    respondStatus(client, 500, "Failed to send location request");
  }
}

static void handleLocationGet(EthernetClient &client, const String &path) {
  // Extract client UID from query string: /api/location?client=xxx
  String clientUid = "";
  int queryStart = path.indexOf('?');
  if (queryStart > 0) {
    String query = path.substring(queryStart + 1);
    int clientStart = query.indexOf("client=");
    if (clientStart >= 0) {
      int clientEnd = query.indexOf('&', clientStart);
      if (clientEnd < 0) clientEnd = query.length();
      clientUid = query.substring(clientStart + 7, clientEnd);
    }
  }

  if (clientUid.length() == 0) {
    respondStatus(client, 400, "Missing client parameter");
    return;
  }

  // Look up cached location
  ClientMetadata *meta = findClientMetadata(clientUid.c_str());
  
  JsonDocument doc;
  doc["clientUid"] = clientUid;
  
  if (meta && (meta->latitude != 0.0f || meta->longitude != 0.0f)) {
    doc["hasLocation"] = true;
    doc["latitude"] = meta->latitude;
    doc["longitude"] = meta->longitude;
    doc["locationEpoch"] = meta->locationEpoch;
    doc["nwsGridValid"] = meta->nwsGridValid;
    if (meta->nwsGridValid) {
      doc["nwsGridOffice"] = meta->nwsGridOffice;
      doc["nwsGridX"] = meta->nwsGridX;
      doc["nwsGridY"] = meta->nwsGridY;
    }
  } else {
    doc["hasLocation"] = false;
  }
  
  String responseStr;
  serializeJson(doc, responseStr);
  respondJson(client, responseStr);
}











