/*
  Tank Alarm Viewer 112025 - Arduino Opta + Blues Notecard
  Version: see FIRMWARE_VERSION in TankAlarm_Common.h

  Purpose:
  - Read-only kiosk that renders the server dashboard without exposing control paths
  - Fetches a summarized notefile produced by the server every 6 hours starting at 6 AM
  - Suitable for remote sites that cannot talk to the server over LAN

  Hardware:
  - Arduino Opta Lite (Ethernet)
  - Blues Wireless Notecard for Opta adapter

  Created: November 2025
*/

// Shared library - common constants and utilities
#include <TankAlarm_Common.h>

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <memory>
#include <new>
#if defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_H7_M7) || defined(ARDUINO_PORTENTA_H7_M4)
  #include <PortentaEthernet.h>
  #include <Ethernet.h>
#else
  #include <Ethernet.h>
#endif
#include <math.h>
#include <string.h>

// Debug mode - controls Serial output and Notecard debug logging
// For PRODUCTION: Leave commented out (default) to save power consumption
// For DEVELOPMENT: Uncomment the line below for troubleshooting and monitoring
//#define DEBUG_MODE

// Watchdog support - use shared library helper
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  static MbedWatchdogHelper mbedWatchdog;
#elif defined(ARDUINO_ARCH_STM32)
  #include <IWatchdog.h>
#endif

// Optional: Create a "ViewerConfig.h" file in this sketch folder to set
// compile-time defaults (e.g. #define DEFAULT_VIEWER_PRODUCT_UID "com.company.product:project").
// If the file does not exist, the product UID must be set via the viewer's config JSON.
#if __has_include("ViewerConfig.h")
  #include "ViewerConfig.h"
#endif

#ifndef DEFAULT_VIEWER_PRODUCT_UID
#define DEFAULT_VIEWER_PRODUCT_UID ""  // Set via ViewerConfig.h or config JSON
#endif

#ifndef VIEWER_SUMMARY_FILE
#define VIEWER_SUMMARY_FILE VIEWER_SUMMARY_INBOX_FILE  // "viewer_summary.qi" — viewer reads inbound
#endif

#ifndef VIEWER_CONFIG_PATH
#define VIEWER_CONFIG_PATH "/viewer_config.json"
#endif

#ifndef VIEWER_NAME
#define VIEWER_NAME "Tank Alarm Viewer"
#endif

#ifndef WEB_REFRESH_SECONDS
#define WEB_REFRESH_SECONDS 21600
#endif

#ifndef WEB_REFRESH_MINUTES
#define WEB_REFRESH_MINUTES (WEB_REFRESH_SECONDS / 60)
#endif

// VIEWER_SUMMARY_INTERVAL_SECONDS and VIEWER_SUMMARY_BASE_HOUR are defined in TankAlarm_Common.h
// Local aliases for backward compatibility (used in this file)
#ifndef SUMMARY_FETCH_INTERVAL_SECONDS
#define SUMMARY_FETCH_INTERVAL_SECONDS VIEWER_SUMMARY_INTERVAL_SECONDS
#endif

#ifndef SUMMARY_FETCH_BASE_HOUR
#define SUMMARY_FETCH_BASE_HOUR VIEWER_SUMMARY_BASE_HOUR
#endif

// Viewer is intended for GET-only use; cap request bodies to avoid memory exhaustion.
#ifndef MAX_HTTP_BODY_BYTES
#define MAX_HTTP_BODY_BYTES 1024
#endif

// ---- Network Printer Configuration (JetDirect / Raw port 9100) ----
// Override in ViewerConfig.h to enable daily report printing.
// The printer must be reachable on the same LAN as the Viewer Opta.
#ifndef PRINT_ENABLED
#define PRINT_ENABLED false        // Set true in ViewerConfig.h to enable printing
#endif

#ifndef PRINTER_IP_1
#define PRINTER_IP_1 0             // Printer IPv4 octet 1
#endif
#ifndef PRINTER_IP_2
#define PRINTER_IP_2 0             // Printer IPv4 octet 2
#endif
#ifndef PRINTER_IP_3
#define PRINTER_IP_3 0             // Printer IPv4 octet 3
#endif
#ifndef PRINTER_IP_4
#define PRINTER_IP_4 0             // Printer IPv4 octet 4
#endif

#ifndef PRINTER_PORT
#define PRINTER_PORT 9100          // 9100 = JetDirect / Raw socket (default for most network printers)
#endif

#ifndef PRINT_DAILY_HOUR
#define PRINT_DAILY_HOUR 8         // UTC hour (0–23) at which the daily report is printed
#endif

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// Viewer configuration - supports DHCP by default or static IP via config file
struct ViewerConfig {
  char viewerName[32];           // Display name for this viewer
  char productUid[64];           // Notehub product UID (can be customized per fleet)
  bool useStaticIp;              // false = DHCP (default), true = use static settings below
  uint8_t macAddress[6];         // MAC address for Ethernet
  uint8_t staticIp[4];           // Static IP address
  uint8_t staticGateway[4];      // Gateway IP
  uint8_t staticSubnet[4];       // Subnet mask
  uint8_t staticDns[4];          // DNS server
  // Network printer (JetDirect / Raw socket printing)
  bool printEnabled;             // true = send daily fleet-snapshot report to the printer
  uint8_t printerIp[4];         // Network printer IPv4 address
  uint16_t printerPort;         // Printer port (9100 = JetDirect, also common: 515 LPR)
  uint8_t printDailyHour;       // UTC hour (0–23) at which the daily report fires
};

struct SensorRecord {
  char clientUid[48];
  char site[32];
  char label[24];
  uint8_t sensorIndex;
  uint8_t userNumber;           // Optional user-assigned display number (0 = unset)
  float levelInches;
  bool alarmActive;
  char alarmType[24];
  double lastUpdateEpoch;
  float vinVoltage;  // Blues Notecard VIN voltage
  char objectType[16];       // e.g. "tank", "gas", "rpm"
  char sensorType[16];       // e.g. "ultrasonic", "pressure"
  char measurementUnit[16];  // e.g. "inches", "psi"
  float change24h;           // 24-hour level change
  bool hasChange24h;         // true if change24h was present in data
};

// Default network configuration lives in gConfig initializer below

// Global configuration instance with defaults
static ViewerConfig gConfig = {
  "Tank Alarm Viewer",           // viewerName
  DEFAULT_VIEWER_PRODUCT_UID,    // productUid - default, can be overridden
  false,                         // useStaticIp - DHCP by default
  { 0x02, 0x00, 0x01, 0x11, 0x20, 0x25 },  // macAddress
  { 192, 168, 1, 210 },          // staticIp
  { 192, 168, 1, 1 },            // staticGateway  
  { 255, 255, 255, 0 },          // staticSubnet
  { 8, 8, 8, 8 },                // staticDns
  // Printer defaults (override in ViewerConfig.h)
  PRINT_ENABLED,                 // printEnabled
  { PRINTER_IP_1, PRINTER_IP_2, PRINTER_IP_3, PRINTER_IP_4 },  // printerIp
  PRINTER_PORT,                  // printerPort
  PRINT_DAILY_HOUR               // printDailyHour
};

static SensorRecord gSensorRecords[MAX_SENSOR_RECORDS];
static uint8_t gSensorRecordCount = 0;

// Printer state
static uint32_t gLastPrintDay = 0;  // Day number (epoch/86400) of the last successful print job

static Notecard notecard;
static EthernetServer gWebServer(ETHERNET_PORT);
static char gViewerUid[48] = {0};
static char gSourceServerName[32] = {0};
static char gSourceServerUid[48] = {0};
static uint32_t gSourceRefreshSeconds = SUMMARY_FETCH_INTERVAL_SECONDS;
static uint8_t gSourceBaseHour = SUMMARY_FETCH_BASE_HOUR;
static double gLastSummaryGeneratedEpoch = 0.0;
static double gLastSummaryFetchEpoch = 0.0;
static double gNextSummaryFetchEpoch = 0.0;
static double gLastSyncedEpoch = 0.0;
static unsigned long gLastSyncMillis = 0;

// DFU State
static unsigned long gLastDfuCheckMillis = 0;
static bool gDfuUpdateAvailable = false;
static char gDfuVersion[32] = {0};
static bool gDfuInProgress = false;
static uint32_t gDfuFirmwareLength = 0;  // Firmware size in bytes (from dfu.status body)

// GitHub release check state
static bool gGitHubUpdateAvailable = false;
static char gGitHubLatestVersion[32] = {0};
static char gGitHubReleaseUrl[128] = {0};
static bool gGitHubAssetAvailable = false;
static char gGitHubAssetUrl[256] = {0};
static uint32_t gGitHubAssetSize = 0;
static unsigned long gLastGitHubCheckMs = 0;
static bool gGitHubBootCheckDone = false;
#define GITHUB_CHECK_INTERVAL_MS 86400000UL  // 24 hours
#define GITHUB_REPO_OWNER "SenaxInc"
#define GITHUB_REPO_NAME  "SenaxTankAlarm"

// Watchdog kick wrapper for IAP DFU callback
static void dfuKickWatchdog() {
  mbedWatchdog.kick();
}

// I2C bus health tracking (required by TankAlarm_I2C.h)
uint32_t gCurrentLoopI2cErrors = 0;    // Not used by Viewer but required by extern
uint32_t gI2cBusRecoveryCount = 0;
static bool gNotecardAvailable = true;
static uint16_t gNotecardFailureCount = 0;
static unsigned long gLastSuccessfulNotecardComm = 0;

static const char VIEWER_DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>Tank Alarm Viewer</title><style>:root{--bg:#f8fafc;--text:#0f172a;--header-bg:#ffffff;--meta-color:#475569;--card-bg:#ffffff;--table-border:rgba(15,23,42,0.08)}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--text);transition:background 0.3s,color 0.3s}header{padding:20px 28px;background:var(--header-bg);box-shadow:0 2px 10px rgba(0,0,0,0.15)}header h1{margin:0;font-size:1.7rem}header .meta{margin-top:12px;font-size:0.95rem;color:var(--meta-color);display:flex;gap:16px;flex-wrap:wrap}.title-row{display:flex;justify-content:space-between;gap:16px;flex-wrap:wrap;align-items:flex-start}.header-actions{display:flex;gap:12px;align-items:center}.icon-button{width:40px;height:40px;border:1px solid rgba(148,163,184,0.4);background:var(--card-bg);color:var(--text);font-size:1.1rem;cursor:pointer}main{padding:24px;max-width:1400px;margin:0 auto}.card{background:var(--card-bg);padding:20px;box-shadow:0 25px 60px rgba(15,23,42,0.15);border:1px solid rgba(15,23,42,0.08)}table{width:100%;border-collapse:collapse;margin-top:12px}th,td{text-align:left;padding:10px 12px;border-bottom:1px solid var(--table-border)}th{text-transform:uppercase;letter-spacing:0.05em;font-size:0.75rem;color:var(--meta-color)}tr:last-child td{border-bottom:none}tr.alarm{background:rgba(220,38,38,0.08)}.status-pill{display:inline-flex;align-items:center;gap:6px;padding:4px 12px;font-size:0.85rem}.status-pill.ok{background:rgba(16,185,129,0.15);color:#34d399}.status-pill.alarm{background:rgba(248,113,113,0.2);color:#fca5a5}.timestamp{font-feature-settings:"tnum";color:var(--meta-color);font-size:0.9rem}footer{margin-top:20px;color:var(--meta-color);font-size:0.85rem;text-align:center}</style></head><body><header><div class="title-row"><div><h1 id="viewerName">Tank Alarm Viewer</h1><div class="meta"><span>Viewer UID: <code id="viewerUid">--</code></span><span>Source: <strong id="sourceServer">--</strong> (<code id="sourceUid">--</code>)</span><span>Summary Generated: <span id="summaryGenerated">--</span></span><span>Last Fetch: <span id="lastFetch">--</span></span><span>Next Scheduled Fetch: <span id="nextFetch">--</span></span><span>Server cadence: <span id="refreshHint">6h @ 6 AM</span></span></div></div></div></header><div id="ghUpdateBanner" style="display:none;background:#fef9c3;border-bottom:2px solid #ca8a04;padding:10px 20px;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;"><span>&#x26A0; New firmware on GitHub: <strong id="ghUpdateVersion"></strong> &mdash; <a id="ghUpdateLink" href="#" target="_blank" rel="noopener noreferrer">View release notes</a> &mdash; Viewer auto-update is direct-first with Notehub fallback</span><button onclick="this.parentElement.style.display='none'" style="background:none;border:none;cursor:pointer;font-size:1.2rem;line-height:1;padding:0 4px;">&times;</button></div><main><section class="card"><div style="display:flex;justify-content:space-between;align-items:baseline;gap:12px;flex-wrap:wrap"><h2 style="margin:0;font-size:1.2rem">Fleet Snapshot</h2><span class="timestamp">Dashboard auto-refresh: )HTML" STR(WEB_REFRESH_MINUTES) R"HTML( min</span></div><table><thead><tr><th>Site</th><th>Tank</th><th>Level (ft/in)</th><th>24hr Change</th><th>Updated</th></tr></thead><tbody id="sensorBody"></tbody></table></section><footer>Viewer nodes are read-only mirrors. Configuration and permissions stay on the server fleet.</footer></main><script>(()=>{const REFRESH_SECONDS=)HTML" STR(WEB_REFRESH_SECONDS)R"HTML(;const els={viewerName:document.getElementById('viewerName'),viewerUid:document.getElementById('viewerUid'),sourceServer:document.getElementById('sourceServer'),sourceUid:document.getElementById('sourceUid'),summaryGenerated:document.getElementById('summaryGenerated'),lastFetch:document.getElementById('lastFetch'),nextFetch:document.getElementById('nextFetch'),refreshHint:document.getElementById('refreshHint'),sensorBody:document.getElementById('sensorBody')};const state={sensors:[]};function applySensorData(d){els.viewerName.textContent=d.vn||'Tank Alarm Viewer';els.viewerUid.textContent=d.vi||'--';els.sourceServer.textContent=d.sn||'Server';els.sourceUid.textContent=d.si||'--';els.summaryGenerated.textContent=formatEpoch(d.ge);els.lastFetch.textContent=formatEpoch(d.lf);els.nextFetch.textContent=formatEpoch(d.nf);els.refreshHint.textContent=describeCadence(d.rs,d.bh);state.sensors=d.sensors||[];renderSensorRows()}async function fetchSensors(){try{const res=await fetch('/api/sensors');if(!res.ok)throw new Error('HTTP '+res.status);const data=await res.json();applySensorData(data)}catch(err){console.error('Viewer refresh failed',err)}}function renderSensorRows(){const tbody=els.sensorBody;tbody.innerHTML='';const rows=state.sensors;if(!rows.length){const tr=document.createElement('tr');tr.innerHTML='<td colspan="5">No sensor data available</td>';tbody.appendChild(tr);return}const now=Date.now();const staleThresholdMs=93600000;rows.forEach(t=>{const tr=document.createElement('tr');const alarm=t.a;if(alarm)tr.classList.add('alarm');const lastUpdate=t.u;const isStale=lastUpdate&&((now-(lastUpdate*1000))>staleThresholdMs);const staleWarning=isStale?' ⚠️':'';tr.innerHTML=`<td>${escapeHtml(t.s,'--')}</td><td>${escapeHtml(t.n||'Tank')}${t.un?' #'+t.un:''}</td><td>${formatFeetInches(t.l)}</td><td>${format24hChange(t)}</td><td>${formatEpoch(lastUpdate)}${staleWarning}</td>`;if(isStale){tr.style.opacity='0.6';tr.title='Data is over 26 hours old'}tbody.appendChild(tr)})}function statusBadge(t){const alarm=t.a;if(!alarm){return'<span class="status-pill ok">Normal</span>'}const label=escapeHtml(t.at||'Alarm','Alarm');return`<span class="status-pill alarm">${label}</span>`}function formatFeetInches(inches){if(typeof inches!=='number'||!isFinite(inches)||inches<0)return'--';const feet=Math.floor(inches/12);const remainingInches=inches-(feet*12);return`${feet}' ${remainingInches.toFixed(1)}"`}function format24hChange(t){const d=t.d;if(d===undefined||d===null||typeof d!=='number'||!isFinite(d))return'--';const sign=d>0?'+':'';return`${sign}${d.toFixed(1)}"`}function formatEpoch(epoch){if(!epoch)return'--';const date=new Date(epoch*1000);if(isNaN(date.getTime()))return'--';return date.toLocaleString()}function describeCadence(seconds,baseHour){const hours=seconds?(seconds/3600).toFixed(1).replace(/\.0$/,''):'6';const hourLabel=(typeof baseHour==='number')?baseHour:6;return`${hours}h cadence · starts ${hourLabel}:00`}function escapeHtml(value,fallback=''){if(value===undefined||value===null||value==='')return fallback;const entityMap={'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'};return String(value).replace(/[&<>"']/g,c=>entityMap[c]||c)}(async()=>{try{const r=await fetch('/api/github/update');if(!r.ok)return;const d=await r.json();if(d.available){const b=document.getElementById('ghUpdateBanner');const v=document.getElementById('ghUpdateVersion');const l=document.getElementById('ghUpdateLink');if(b&&v){v.textContent='v'+d.latestVersion;if(l&&d.releaseUrl)l.href=d.releaseUrl;b.style.display='flex';}}}catch(e){}})();fetchSensors();setInterval(()=>fetchSensors(),REFRESH_SECONDS*1000)})();</script></body></html>)HTML";

static void initializeNotecard();
static void initializeEthernet();
static void handleWebRequests();
static bool readHttpRequest(EthernetClient &client, String &method, String &path, String &body, size_t &contentLength, bool &bodyTooLarge);
static void respondJson(EthernetClient &client, const String &body);
static void respondStatus(EthernetClient &client, int status, const char *message);
static void sendDashboard(EthernetClient &client);
static void sendSensorJson(EthernetClient &client);
static void ensureTimeSync();
static double currentEpoch();
static double computeNextAlignedEpoch(double epoch, uint8_t baseHour, uint32_t intervalSeconds);
static bool deriveMacFromUid();
static void scheduleNextSummaryFetch();
static void fetchViewerSummary();
static void handleViewerSummary(JsonDocument &doc, double epoch);
static void checkForFirmwareUpdate();
static void enableDfuMode();
static void checkGitHubForUpdate();
static void handleGitHubUpdateGet(EthernetClient &client);
static bool attemptGitHubDirectInstall(String &statusMessage);
static void epochToDateStr(double epoch, char *buf, size_t bufLen);
static void checkDailyPrint();
static void sendDailyPrintJob();

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

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {
    safeSleep(10);
  }
  Serial.println();
  Serial.print(F("Tank Alarm Viewer 112025 v"));
  Serial.print(F(FIRMWARE_VERSION));
  Serial.print(F(" ("));
  Serial.print(F(FIRMWARE_BUILD_DATE));
  Serial.println(F(")"));

  Wire.begin();
  Wire.setTimeout(I2C_WIRE_TIMEOUT_MS);  // Guard against indefinite blocking on bus hang

  // I2C bus scan: verify Notecard is present
  {
    const uint8_t expectedAddrs[] = { NOTECARD_I2C_ADDRESS };
    const char *expectedNames[] = { "Notecard" };
    tankalarm_scanI2CBus(expectedAddrs, expectedNames, 1);
  }

  initializeNotecard();
  ensureTimeSync();
  fetchViewerSummary();  // Drain any queued summaries before serving UI
  scheduleNextSummaryFetch();
  initializeEthernet();
  gWebServer.begin();

#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    uint32_t timeoutMs = WATCHDOG_TIMEOUT_SECONDS * 1000;
    if (mbedWatchdog.start(timeoutMs)) {
      Serial.print(F("Mbed Watchdog enabled ("));
      Serial.print(WATCHDOG_TIMEOUT_SECONDS);
      Serial.println(F(" s)"));
    } else {
      Serial.println(F("Warning: Watchdog initialization failed"));
    }
  #else
    IWatchdog.begin(WATCHDOG_TIMEOUT_SECONDS * 1000000UL);
    Serial.print(F("Watchdog enabled ("));
    Serial.print(WATCHDOG_TIMEOUT_SECONDS);
    Serial.println(F(" s)"));
  #endif
#else
  Serial.println(F("Watchdog not available on this platform"));
#endif

  tankalarm_printHeapStats();

  Serial.println(F("Viewer setup complete"));
}

void loop() {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #else
    IWatchdog.reload();
  #endif
#endif

  Ethernet.maintain();  // Renew DHCP lease if needed

  handleWebRequests();
  ensureTimeSync();

  // ---- Notecard I2C health check (with exponential backoff) ----
  {
    static unsigned long lastNcHealthCheck = 0;
    static unsigned long ncHealthInterval = NOTECARD_HEALTH_CHECK_BASE_INTERVAL_MS;
    unsigned long now = millis();
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

  if (gNextSummaryFetchEpoch > 0.0 && currentEpoch() >= gNextSummaryFetchEpoch) {
    fetchViewerSummary();
    scheduleNextSummaryFetch();
  }

  // Daily report printing (if printer is configured)
  checkDailyPrint();

  // Check for firmware updates every hour
  unsigned long currentMillis = millis();
  if (!gDfuInProgress && (currentMillis - gLastDfuCheckMillis >= DFU_CHECK_INTERVAL_MS)) {
    gLastDfuCheckMillis = currentMillis;
    bool attemptedDirect = false;
    if (gGitHubUpdateAvailable && gGitHubAssetAvailable) {
      String directStatus;
      attemptedDirect = attemptGitHubDirectInstall(directStatus);
      if (!attemptedDirect) {
        Serial.print(F("Viewer GitHub Direct unavailable: "));
        Serial.println(directStatus);
      }
    }
    if (!attemptedDirect) {
      checkForFirmwareUpdate();
    }
  }

  // Periodic GitHub release check (60s after boot, then every 24 hours).
  // Uses Notecard web.get proxy — works over cellular with or without Ethernet.
  {
    const unsigned long GITHUB_BOOT_DELAY_MS = 60000UL;
    if (!gGitHubBootCheckDone && (currentMillis >= GITHUB_BOOT_DELAY_MS) && gNotecardAvailable) {
      gGitHubBootCheckDone = true;
      gLastGitHubCheckMs = currentMillis;
      checkGitHubForUpdate();
    } else if (gGitHubBootCheckDone && gNotecardAvailable &&
               (currentMillis - gLastGitHubCheckMs) >= GITHUB_CHECK_INTERVAL_MS) {
      gLastGitHubCheckMs = currentMillis;
      checkGitHubForUpdate();
    }
  }
}

static void initializeNotecard() {
#ifdef DEBUG_MODE
  notecard.setDebugOutputStream(Serial);
#endif
  notecard.begin(NOTECARD_I2C_ADDRESS);
  Serial.println(F("Notecard initialized"));

  J *req = notecard.newRequest("hub.set");
  if (req) {
    // Use configurable product UID (allows fleet-specific deployments without recompilation)
    JAddStringToObject(req, "product", gConfig.productUid);
    JAddStringToObject(req, "mode", "continuous");
    // Join the viewer fleet for fleet-scoped DFU, route filtering, and device management
    JAddStringToObject(req, "fleet", "tankalarm-viewer");
    J *hubRsp = notecard.requestAndResponse(req);
    if (hubRsp) {
      const char *hubErr = JGetString(hubRsp, "err");
      if (hubErr && hubErr[0] != '\0') {
        Serial.print(F("WARNING: hub.set failed: "));
        Serial.println(hubErr);
      }
      notecard.deleteResponse(hubRsp);
    } else {
      Serial.println(F("WARNING: hub.set returned no response"));
    }
  }
  
  Serial.print(F("Product UID: "));
  Serial.println(gConfig.productUid);

  // Retrieve the Notecard's unique device identifier (e.g., "dev:860322068012345").
  // hub.get returns the device serial in the "device" field.
  // card.uuid does NOT exist in the Notecard API.
  req = notecard.newRequest("hub.get");
  if (req) {
    J *rsp = notecard.requestAndResponse(req);
    if (rsp) {
      const char *uid = JGetString(rsp, "device");
      if (uid && uid[0] != '\0') {
        strlcpy(gViewerUid, uid, sizeof(gViewerUid));
      }
      notecard.deleteResponse(rsp);
    }
  }

  Serial.print(F("Viewer Device UID: "));
  Serial.println(gViewerUid);

  // Enable IAP DFU — Wireless for Opta carrier does NOT route AUX pins
  // (BOOT0, NRST, UART) needed for outboard DFU (ODFU). Use IAP instead.
  tankalarm_enableIapDfu(notecard);
}

static void ensureTimeSync() {
  tankalarm_ensureTimeSync(notecard, gLastSyncedEpoch, gLastSyncMillis);
}

static double currentEpoch() {
  return tankalarm_currentEpoch(gLastSyncedEpoch, gLastSyncMillis);
}

static double computeNextAlignedEpoch(double epoch, uint8_t baseHour, uint32_t intervalSeconds) {
  return tankalarm_computeNextAlignedEpoch(epoch, baseHour, intervalSeconds);
}

static void scheduleNextSummaryFetch() {
  double epoch = currentEpoch();
  uint32_t interval = (gSourceRefreshSeconds > 0) ? gSourceRefreshSeconds : SUMMARY_FETCH_INTERVAL_SECONDS;
  uint8_t baseHour = gSourceBaseHour;
  gNextSummaryFetchEpoch = computeNextAlignedEpoch(epoch, baseHour, interval);
}

/**
 * Derive a unique MAC address from the Notecard device UID.
 * Uses a simple hash of the UID string to populate the last 4 bytes.
 * Byte 0 is 0x02 (locally administered, unicast).
 * Byte 1 is 0x00 (vendor padding).
 * Returns true when a UID-derived MAC is written, false if UID is unavailable.
 */
static bool deriveMacFromUid() {
  if (gViewerUid[0] == '\0') return false;  // No UID available

  // DJB2 hash of UID string
  uint32_t hash = 5381;
  for (const char *p = gViewerUid; *p; p++) {
    hash = ((hash << 5) + hash) + (uint8_t)*p;
  }

  gConfig.macAddress[0] = 0x02;  // Locally administered, unicast
  gConfig.macAddress[1] = 0x00;
  gConfig.macAddress[2] = (uint8_t)(hash >> 24);
  gConfig.macAddress[3] = (uint8_t)(hash >> 16);
  gConfig.macAddress[4] = (uint8_t)(hash >> 8);
  gConfig.macAddress[5] = (uint8_t)(hash);

  Serial.print(F("MAC derived from UID: "));
  for (uint8_t i = 0; i < 6; i++) {
    if (i > 0) Serial.print(':');
    if (gConfig.macAddress[i] < 0x10) Serial.print('0');
    Serial.print(gConfig.macAddress[i], HEX);
  }
  Serial.println();
  return true;
}

static void initializeEthernet() {
  Serial.print(F("Initializing Ethernet..."));

  const uint8_t defaultViewerMac[6] = { 0x02, 0x00, 0x01, 0x11, 0x20, 0x25 };
  const bool macAllZero =
      (gConfig.macAddress[0] == 0 && gConfig.macAddress[1] == 0 && gConfig.macAddress[2] == 0 &&
       gConfig.macAddress[3] == 0 && gConfig.macAddress[4] == 0 && gConfig.macAddress[5] == 0);
  const bool macIsDefault = (memcmp(gConfig.macAddress, defaultViewerMac, sizeof(defaultViewerMac)) == 0);
  const bool hasConfiguredMacOverride = (!macAllZero && !macIsDefault);

  const char *macSource = "Configured";
  if (!hasConfiguredMacOverride) {
    uint8_t hwMac[6] = {0};
    Ethernet.MACAddress(hwMac);
    const bool hwMacAllZero =
        (hwMac[0] == 0 && hwMac[1] == 0 && hwMac[2] == 0 && hwMac[3] == 0 && hwMac[4] == 0 && hwMac[5] == 0);
    if (!hwMacAllZero) {
      memcpy(gConfig.macAddress, hwMac, sizeof(hwMac));
      macSource = "Hardware";
    } else if (deriveMacFromUid()) {
      macSource = "UID-derived";
    } else {
      memcpy(gConfig.macAddress, defaultViewerMac, sizeof(defaultViewerMac));
      macSource = "Default";
    }
  }
  
  // Prepare IP addresses from config
  IPAddress staticIp(gConfig.staticIp[0], gConfig.staticIp[1], gConfig.staticIp[2], gConfig.staticIp[3]);
  IPAddress staticGateway(gConfig.staticGateway[0], gConfig.staticGateway[1], gConfig.staticGateway[2], gConfig.staticGateway[3]);
  IPAddress staticSubnet(gConfig.staticSubnet[0], gConfig.staticSubnet[1], gConfig.staticSubnet[2], gConfig.staticSubnet[3]);
  IPAddress staticDns(gConfig.staticDns[0], gConfig.staticDns[1], gConfig.staticDns[2], gConfig.staticDns[3]);
  
  int status;
  if (gConfig.useStaticIp) {
    Serial.print(F(" (static) "));
    status = Ethernet.begin(gConfig.macAddress, staticIp, staticDns, staticGateway, staticSubnet);
  } else {
    Serial.print(F(" (DHCP) "));
    status = Ethernet.begin(gConfig.macAddress);
  }
  
  if (status == 0) {
    Serial.println(F(" FAILED"));
    if (!gConfig.useStaticIp) {
      Serial.println(F("DHCP failed - retrying..."));
    }
    // Retry up to 3 times with increasing delays
    for (uint8_t attempt = 1; attempt <= 3 && status == 0; attempt++) {
      unsigned long retryDelay = (unsigned long)attempt * 5000UL;
      Serial.print(F("Ethernet retry "));
      Serial.print(attempt);
      Serial.print(F("/3 in "));
      Serial.print(retryDelay / 1000);
      Serial.println(F("s..."));
      safeSleep(retryDelay);
      if (gConfig.useStaticIp) {
        status = Ethernet.begin(gConfig.macAddress, staticIp, staticDns, staticGateway, staticSubnet);
      } else {
        status = Ethernet.begin(gConfig.macAddress);
      }
    }
    if (status == 0) {
      Serial.println(F("Ethernet initialization failed after retries"));
    }
  }

  if (status != 0) {
    Serial.println(F(" ok"));
    Serial.print(F("Using MAC: "));
    for (uint8_t i = 0; i < 6; i++) {
      if (i > 0) Serial.print(':');
      if (gConfig.macAddress[i] < 0x10) Serial.print('0');
      Serial.print(gConfig.macAddress[i], HEX);
    }
    Serial.print(F(" ("));
    Serial.print(macSource);
    Serial.println(F(")"));
    Serial.print(F("IP Address: "));
    Serial.println(Ethernet.localIP());
    Serial.print(F("Gateway: "));
    Serial.println(Ethernet.gatewayIP());
  }
}

static void handleWebRequests() {
  EthernetClient client = gWebServer.available();
  if (!client) {
    return;
  }

  String method;
  String path;
  String body;
  size_t contentLength = 0;
  bool bodyTooLarge = false;

  if (!readHttpRequest(client, method, path, body, contentLength, bodyTooLarge)) {
    respondStatus(client, 400, "Bad Request");
    client.stop();
    return;
  }

  if (bodyTooLarge) {
    respondStatus(client, 413, "Payload Too Large");
    client.stop();
    return;
  }

  if (method == "GET" && path == "/") {
    sendDashboard(client);
  } else if (method == "GET" && path == "/api/sensors") {
    sendSensorJson(client);
  } else if (method == "GET" && path == "/api/github/update") {
    handleGitHubUpdateGet(client);
  } else {
    respondStatus(client, 404, "Not Found");
  }

  safeSleep(1);
  client.stop();
}

static bool readHttpRequest(EthernetClient &client, String &method, String &path, String &body, size_t &contentLength, bool &bodyTooLarge) {
  method = "";
  path = "";
  contentLength = 0;
  body = "";
  bodyTooLarge = false;

  String line;
  bool firstLine = true;
  // BugFix v1.6.2 (M-15): Cap header count to prevent memory exhaustion
  // from malformed or malicious requests with excessive headers.
  uint8_t headerCount = 0;

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
      if (line.length() == 0) {
        break;
      }
      if (firstLine) {
        int space = line.indexOf(' ');
        if (space < 0) {
          return false;
        }
        method = line.substring(0, space);
        int nextSpace = line.indexOf(' ', space + 1);
        if (nextSpace < 0) {
          return false;
        }
        path = line.substring(space + 1, nextSpace);
        firstLine = false;
      } else {
        if (++headerCount > 32) {
          return false;
        }
        int colonPos = line.indexOf(':');
        if (colonPos > 0) {
          String headerKey = line.substring(0, colonPos);
          headerKey.trim();
          String headerValue = line.substring(colonPos + 1);
          headerValue.trim();
          if (headerKey.equalsIgnoreCase("Content-Length")) {
            contentLength = headerValue.toInt();
            if (contentLength > MAX_HTTP_BODY_BYTES) {
              bodyTooLarge = true;
              contentLength = MAX_HTTP_BODY_BYTES;
            }
          }
        }
      }
      line = "";
    } else {
      line += c;
      if (line.length() > 512) {
        return false;
      }
    }
  }

  if (contentLength > 0) {
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

static void respondJson(EthernetClient &client, const String &body) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(body.length());
  client.println(F("Cache-Control: no-cache, no-store, must-revalidate"));
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
}

static void respondStatus(EthernetClient &client, int status, const char *message) {
  const char *msg = message ? message : "";
  size_t len = strlen(msg);

  client.print(F("HTTP/1.1 "));
  client.print(status);
  client.print(F(" "));
  switch (status) {
    case 200: client.println(F("OK")); break;
    case 400: client.println(F("Bad Request")); break;
    case 404: client.println(F("Not Found")); break;
    case 413: client.println(F("Payload Too Large")); break;
    case 500: client.println(F("Internal Server Error")); break;
    default: client.println(F("Error")); break;
  }
  client.println(F("Content-Type: text/plain"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(len);
  client.println();
  client.print(msg);
}

static void sendDashboard(EthernetClient &client) {
  size_t htmlLen = strlen_P(VIEWER_DASHBOARD_HTML);
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html; charset=utf-8"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(htmlLen);
  client.println(F("Cache-Control: no-cache, no-store, must-revalidate"));
  client.println();

  const size_t bufSize = 128;
  uint8_t buffer[bufSize];
  size_t remaining = htmlLen;
  const char* ptr = VIEWER_DASHBOARD_HTML;

  while (remaining > 0) {
    size_t chunk = (remaining < bufSize) ? remaining : bufSize;
    for (size_t i = 0; i < chunk; i++) {
        buffer[i] = pgm_read_byte_near(ptr++);
    }
    client.write(buffer, chunk);
    remaining -= chunk;
  }
}

static void sendSensorJson(EthernetClient &client) {
  std::unique_ptr<JsonDocument> docPtr(new (std::nothrow) JsonDocument());
  if (!docPtr) {
    respondStatus(client, 500, "Out of Memory");
    return;
  }
  JsonDocument &doc = *docPtr;

  doc["vn"] = VIEWER_NAME;
  doc["vi"] = gViewerUid;
  doc["sn"] = gSourceServerName;
  doc["si"] = gSourceServerUid;
  doc["ge"] = gLastSummaryGeneratedEpoch;
  doc["lf"] = gLastSummaryFetchEpoch;
  doc["nf"] = gNextSummaryFetchEpoch;
  doc["rs"] = gSourceRefreshSeconds;
  doc["bh"] = gSourceBaseHour;
  doc["sf"] = VIEWER_SUMMARY_FILE;
  doc["rc"] = gSensorRecordCount;
  doc["ls"] = gLastSyncedEpoch;

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
    obj["l"] = gSensorRecords[i].levelInches;
    obj["a"] = gSensorRecords[i].alarmActive;
    obj["at"] = gSensorRecords[i].alarmType;
    obj["u"] = gSensorRecords[i].lastUpdateEpoch;
    if (gSensorRecords[i].vinVoltage > 0.0f) {
      obj["v"] = gSensorRecords[i].vinVoltage;
    }
    if (gSensorRecords[i].objectType[0] != '\0') {
      obj["ot"] = gSensorRecords[i].objectType;
    }
    if (gSensorRecords[i].sensorType[0] != '\0') {
      obj["st"] = gSensorRecords[i].sensorType;
    }
    if (gSensorRecords[i].measurementUnit[0] != '\0') {
      obj["mu"] = gSensorRecords[i].measurementUnit;
    }
    if (gSensorRecords[i].hasChange24h) {
      obj["d"] = gSensorRecords[i].change24h;
    }
  }

  // BugFix v1.6.2 (M-14): Stream JSON directly to client instead of materializing
  // the entire response in a String. measureJson() provides Content-Length, then
  // serializeJson() writes directly to the EthernetClient socket.
  size_t jsonLen = measureJson(doc);
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: "));
  client.println(jsonLen);
  client.println(F("Cache-Control: no-cache, no-store, must-revalidate"));
  client.println();
  serializeJson(doc, client);
}

static void fetchViewerSummary() {
  uint8_t notesProcessed = 0;
  while (true) {
    // Kick watchdog between iterations — each note.get is a blocking I2C transaction
    #ifdef TANKALARM_WATCHDOG_AVAILABLE
      #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
        mbedWatchdog.kick();
      #endif
    #endif
    // Safety cap: don't drain more than 20 notes per call to stay responsive
    if (++notesProcessed > 20) {
      Serial.println(F("fetchViewerSummary: cap reached, will drain remaining next cycle"));
      break;
    }
    J *req = notecard.newRequest("note.get");
    if (!req) {
      gNotecardFailureCount++;
      if (gNotecardFailureCount >= NOTECARD_FAILURE_THRESHOLD && gNotecardAvailable) {
        gNotecardAvailable = false;
        Serial.println(F("Notecard unavailable - I2C health check will attempt recovery"));
      }
      return;
    }
    JAddStringToObject(req, "file", VIEWER_SUMMARY_FILE);
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
      std::unique_ptr<JsonDocument> docPtr(new (std::nothrow) JsonDocument());
      if (docPtr) {
        JsonDocument &doc = *docPtr;
        DeserializationError err = deserializeJson(doc, json);
        NoteFree(json);
        if (!err) {
          handleViewerSummary(doc, epoch);
          processedOk = true;
        } else {
          Serial.print(F("Summary parse failed: "));
          Serial.println(err.f_str());
        }
      } else {
        NoteFree(json);
        Serial.println(F("OOM processing summary"));
      }
    }

    notecard.deleteResponse(rsp);

    // Only consume the note after successful processing
    if (processedOk) {
      J *delReq = notecard.newRequest("note.get");
      if (delReq) {
        JAddStringToObject(delReq, "file", VIEWER_SUMMARY_FILE);
        JAddBoolToObject(delReq, "delete", true);
        J *delRsp = notecard.requestAndResponse(delReq);
        if (delRsp) notecard.deleteResponse(delRsp);
      }
    } else {
      Serial.println(F("Deleting malformed summary note"));
      J *delReq = notecard.newRequest("note.get");
      if (delReq) {
        JAddStringToObject(delReq, "file", VIEWER_SUMMARY_FILE);
        JAddBoolToObject(delReq, "delete", true);
        J *delRsp = notecard.requestAndResponse(delReq);
        if (delRsp) notecard.deleteResponse(delRsp);
      }
      break;
    }
  }
}

static void handleViewerSummary(JsonDocument &doc, double epoch) {
  // Schema version check — warn if unexpected version
  if (doc.containsKey("_sv")) {
    uint8_t sv = doc["_sv"].as<uint8_t>();
    if (sv != 1) {
      Serial.print(F("WARNING: Viewer summary schema version "));
      Serial.print(sv);
      Serial.println(F(" (expected 1) — fields may be missing or changed"));
    }
  }

  const char *serverName = doc["sn"] | doc["serverName"] | "Tank Alarm Server";
  const char *serverUid = doc["si"] | doc["serverUid"] | "";
  strlcpy(gSourceServerName, serverName, sizeof(gSourceServerName));
  strlcpy(gSourceServerUid, serverUid, sizeof(gSourceServerUid));

  if (doc.containsKey("rs")) {
    gSourceRefreshSeconds = doc["rs"].as<uint32_t>();
  } else if (doc.containsKey("refreshSeconds")) {
    gSourceRefreshSeconds = doc["refreshSeconds"].as<uint32_t>();
  }
  if (gSourceRefreshSeconds == 0) {
    gSourceRefreshSeconds = SUMMARY_FETCH_INTERVAL_SECONDS;
  }
  // Clamp refresh interval to sane bounds (1 hour to 24 hours)
  if (gSourceRefreshSeconds < 3600UL) gSourceRefreshSeconds = 3600UL;
  if (gSourceRefreshSeconds > 86400UL) gSourceRefreshSeconds = 86400UL;

  if (doc.containsKey("bh")) {
    gSourceBaseHour = doc["bh"].as<uint8_t>();
  } else if (doc.containsKey("baseHour")) {
    gSourceBaseHour = doc["baseHour"].as<uint8_t>();
  }
  // Clamp base hour to valid range (0–23)
  if (gSourceBaseHour > 23) gSourceBaseHour = 6;

  if (doc.containsKey("ge")) {
    gLastSummaryGeneratedEpoch = doc["ge"].as<double>();
  } else if (doc.containsKey("generatedEpoch")) {
    gLastSummaryGeneratedEpoch = doc["generatedEpoch"].as<double>();
  } else {
    gLastSummaryGeneratedEpoch = (epoch > 0.0) ? epoch : currentEpoch();
  }

  gSensorRecordCount = 0;
  JsonArray arr = doc["sensors"].as<JsonArray>();
  if (arr) {
    for (JsonVariantConst item : arr) {
      if (gSensorRecordCount >= MAX_SENSOR_RECORDS) {
        break;
      }
      SensorRecord &rec = gSensorRecords[gSensorRecordCount++];
      memset(&rec, 0, sizeof(SensorRecord));
      strlcpy(rec.clientUid, item["c"] | "", sizeof(rec.clientUid));
      strlcpy(rec.site, item["s"] | "", sizeof(rec.site));
      strlcpy(rec.label, item["n"] | "Tank", sizeof(rec.label));
      rec.sensorIndex = item["k"].is<uint8_t>() ? item["k"].as<uint8_t>() : gSensorRecordCount;
      rec.userNumber = item["un"].is<uint8_t>() ? item["un"].as<uint8_t>() : 0;
      rec.levelInches = item["l"].as<float>();
      rec.alarmActive = item["a"].as<bool>();
      strlcpy(rec.alarmType, item["at"] | (rec.alarmActive ? "alarm" : "clear"), sizeof(rec.alarmType));
      rec.lastUpdateEpoch = item["u"].as<double>();
      rec.vinVoltage = item["v"].as<float>();
      strlcpy(rec.objectType, item["ot"] | "", sizeof(rec.objectType));
      strlcpy(rec.sensorType, item["st"] | "", sizeof(rec.sensorType));
      strlcpy(rec.measurementUnit, item["mu"] | "", sizeof(rec.measurementUnit));
      if (!item["d"].isNull()) {
        rec.change24h = item["d"].as<float>();
        rec.hasChange24h = true;
      }
    }
  }

  gLastSummaryFetchEpoch = currentEpoch();
  Serial.print(F("Viewer summary applied ("));
  Serial.print(gSensorRecordCount);
  Serial.println(F(" sensors)"));
}

// ========================== DFU Functions ==========================

/**
 * @brief Check GitHub releases API for a newer firmware version.
 *
 * Uses the Notecard web.get proxy so this works over cellular regardless of
 * whether Ethernet is connected.  Updates gGitHubUpdateAvailable and friends.
 */
static void checkGitHubForUpdate() {
  Serial.println(F("Checking GitHub for firmware updates..."));
  J *req = notecard.newRequest("web.get");
  if (!req) { return; }
  JAddStringToObject(req, "url",
    "https://api.github.com/repos/" GITHUB_REPO_OWNER "/" GITHUB_REPO_NAME "/releases/latest");
  JAddNumberToObject(req, "timeout", 30);
  J *hdrs = JCreateObject();
  if (hdrs) {
    JAddStringToObject(hdrs, "User-Agent", "TankAlarm-Viewer/" FIRMWARE_VERSION);
    JAddStringToObject(hdrs, "Accept",     "application/vnd.github.v3+json");
    JAddItemToObject(req, "headers", hdrs);
  }
  J *rsp = notecard.requestAndResponse(req);
  if (!rsp) { return; }
  int result = JGetInt(rsp, "result");
  if (result != 200) {
    Serial.print(F("GitHub API HTTP "));
    Serial.println(result);
    notecard.deleteResponse(rsp);
    return;
  }
  const char *bodyStr = JGetString(rsp, "body");
  if (!bodyStr || bodyStr[0] == '\0') {
    notecard.deleteResponse(rsp);
    return;
  }
  StaticJsonDocument<384> filter;
  filter["tag_name"] = true;
  filter["html_url"] = true;
  for (uint8_t i = 0; i < 8; ++i) {
    filter["assets"][i]["name"] = true;
    filter["assets"][i]["browser_download_url"] = true;
    filter["assets"][i]["size"] = true;
  }
  StaticJsonDocument<2048> doc;
  auto err = deserializeJson(doc, bodyStr, DeserializationOption::Filter(filter));
  notecard.deleteResponse(rsp);
  if (err) { return; }
  const char *tag = doc["tag_name"] | "";
  const char *url = doc["html_url"]  | "";
  if (tag[0] == 'v' || tag[0] == 'V') { ++tag; }
  if (tag[0] == '\0') { return; }
  gGitHubAssetAvailable = false;
  gGitHubAssetUrl[0] = '\0';
  gGitHubAssetSize = 0;
  char expectedAssetName[96];
  snprintf(expectedAssetName, sizeof(expectedAssetName), "TankAlarm-Viewer-v%s.bin", tag);
  JsonArray assets = doc["assets"].as<JsonArray>();
  if (!assets.isNull()) {
    for (JsonObject asset : assets) {
      const char *assetName = asset["name"] | "";
      if (strcmp(assetName, expectedAssetName) == 0) {
        const char *assetUrl = asset["browser_download_url"] | "";
        if (assetUrl[0] != '\0') {
          strlcpy(gGitHubAssetUrl, assetUrl, sizeof(gGitHubAssetUrl));
          gGitHubAssetSize = asset["size"] | 0;
          gGitHubAssetAvailable = true;
        }
        break;
      }
    }
  }
  strlcpy(gGitHubReleaseUrl, url, sizeof(gGitHubReleaseUrl));
  if (strcmp(tag, FIRMWARE_VERSION) != 0) {
    strlcpy(gGitHubLatestVersion, tag, sizeof(gGitHubLatestVersion));
    if (!gGitHubUpdateAvailable) {
      Serial.print(F("GitHub update available: v"));
      Serial.println(gGitHubLatestVersion);
    }
    gGitHubUpdateAvailable = true;
  } else {
    gGitHubUpdateAvailable = false;
    gGitHubLatestVersion[0] = '\0';
    gGitHubAssetAvailable = false;
    gGitHubAssetUrl[0] = '\0';
    gGitHubAssetSize = 0;
    Serial.println(F("Viewer firmware is up to date."));
  }
}

static bool attemptGitHubDirectInstall(String &statusMessage) {
  // Placeholder for direct Ethernet HTTPS install path.
  // Viewer policy is direct-first then Notehub fallback; this build currently
  // falls back to Notehub when direct install is unavailable.
  if (!gGitHubAssetAvailable || gGitHubAssetUrl[0] == '\0') {
    statusMessage = "matching Viewer .bin asset is missing";
    return false;
  }
  statusMessage = "direct HTTPS installer unavailable on current build";
  return false;
}

static void handleGitHubUpdateGet(EthernetClient &client) {
  JsonDocument doc;
  doc["available"]      = gGitHubUpdateAvailable;
  doc["latestVersion"]  = gGitHubLatestVersion[0] ? gGitHubLatestVersion : FIRMWARE_VERSION;
  doc["currentVersion"] = FIRMWARE_VERSION;
  doc["releaseUrl"]     = gGitHubReleaseUrl;
  doc["assetAvailable"] = gGitHubAssetAvailable;
  doc["assetUrl"] = gGitHubAssetUrl;
  doc["assetSize"] = gGitHubAssetSize;
  doc["assetNamingConvention"] = "TankAlarm-Viewer-vX.Y.Z.bin";
  String responseStr;
  serializeJson(doc, responseStr);
  respondJson(client, responseStr);
}

/**
 * @brief Check for firmware updates via Blues Notecard IAP DFU
 *
 * Queries the Notecard for available firmware updates. If an update is found
 * and ready, auto-applies it (headless device with no UI).
 */
static void checkForFirmwareUpdate() {
  Serial.println(F("Checking for firmware update..."));

  TankAlarmDfuStatus status;
  if (!tankalarm_checkDfuStatus(notecard, status)) {
    Serial.println(F("DFU status request failed (no response)"));
    return;
  }

  if (status.error) {
    Serial.print(F("DFU status error: "));
    Serial.println(status.errorMsg);
    return;
  }

  if (status.downloading) {
    Serial.println(F("DFU download in progress..."));
    gDfuInProgress = true;
    return;
  }

  if (status.firmwareLength > 0) {
    gDfuFirmwareLength = status.firmwareLength;
  }

  if (status.updateAvailable && status.version[0] != '\0') {
    Serial.print(F("Firmware update available: v"));
    Serial.println(status.version);
    gDfuUpdateAvailable = true;
    strlcpy(gDfuVersion, status.version, sizeof(gDfuVersion));

    // Auto-apply: headless device with no UI — apply updates automatically
    Serial.println(F("Auto-enabling IAP DFU..."));
    enableDfuMode();
  } else {
    Serial.println(F("No firmware update available"));
    gDfuUpdateAvailable = false;
    gDfuVersion[0] = '\0';
    gDfuFirmwareLength = 0;
  }
}

/**
 * @brief Apply firmware update via IAP (In-Application Programming)
 *
 * Reads firmware chunks from Notecard via dfu.get, writes to internal flash
 * via FlashIAP, then reboots. Replaces ODFU card.dfu which doesn't work on
 * Wireless for Opta (no AUX pin routing).
 */
static void enableDfuMode() {
  if (!gDfuUpdateAvailable) {
    Serial.println(F("No DFU update available to enable"));
    return;
  }

  if (gDfuFirmwareLength == 0) {
    Serial.println(F("ERROR: No firmware length — run checkForFirmwareUpdate first"));
    return;
  }

  Serial.print(F("Enabling IAP DFU for version: "));
  Serial.println(gDfuVersion);

  gDfuInProgress = true;

  // Viewer always uses "continuous" mode (Ethernet)
  bool success = tankalarm_performIapUpdate(notecard, gDfuFirmwareLength, "continuous", dfuKickWatchdog);

  // If we get here, update failed (success path reboots via NVIC_SystemReset)
  if (!success) {
    Serial.println(F("IAP DFU update failed — resuming normal operation"));
    gDfuInProgress = false;
  }
}

// ============================================================================
// Network Printing (JetDirect / Raw socket, port 9100)
// ============================================================================

/**
 * Convert a Unix epoch (UTC seconds) to a human-readable "YYYY-MM-DD HH:MM:SS UTC" string.
 * Uses Howard Hinnant's civil_from_days algorithm; no stdlib time functions required.
 *
 * @param epoch  Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
 * @param buf    Output character buffer
 * @param bufLen Size of buf (at least 24 bytes recommended)
 */
static void epochToDateStr(double epoch, char *buf, size_t bufLen) {
  if (epoch < 0.0 || !buf || bufLen < 20) {
    if (buf && bufLen > 0) strlcpy(buf, "--", bufLen);
    return;
  }
  uint32_t t = (uint32_t)epoch;
  uint32_t sec  = t % 60;  t /= 60;
  uint32_t min  = t % 60;  t /= 60;
  uint32_t hour = t % 24;  t /= 24;
  uint32_t days = t;

  // Howard Hinnant civil_from_days
  uint32_t z   = days + 719468UL;
  uint32_t era = z / 146097UL;
  uint32_t doe = z - era * 146097UL;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t y   = yoe + era * 400UL;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp  = (5 * doy + 2) / 153;
  uint32_t d   = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m   = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;

  snprintf(buf, bufLen, "%04u-%02u-%02u %02u:%02u:%02u UTC",
           (unsigned)y, (unsigned)m, (unsigned)d,
           (unsigned)hour, (unsigned)min, (unsigned)sec);
}

/**
 * Check whether it is time to send a daily print job and, if so, dispatch one.
 *
 * The job fires once per UTC calendar day when:
 *   1. gConfig.printEnabled is true
 *   2. gConfig.printerIp is not 0.0.0.0
 *   3. The current UTC hour >= gConfig.printDailyHour
 *   4. A job has not already been sent today (gLastPrintDay tracks the day)
 *
 * Called from loop().
 */
static void checkDailyPrint() {
  if (!gConfig.printEnabled) return;

  // Require a non-zero printer IP
  if (gConfig.printerIp[0] == 0 && gConfig.printerIp[1] == 0 &&
      gConfig.printerIp[2] == 0 && gConfig.printerIp[3] == 0) {
    return;
  }

  double epoch = currentEpoch();
  if (epoch < 1000000.0) return;  // Clock not yet synced (pre-1982)

  uint32_t today = (uint32_t)(epoch / 86400.0);  // Day index since 1970-01-01
  if (today == gLastPrintDay) return;             // Already printed today

  // Only fire at or after the configured UTC hour
  uint32_t secondsOfDay = (uint32_t)fmod(epoch, 86400.0);
  uint8_t  currentHour  = (uint8_t)(secondsOfDay / 3600);
  if (currentHour < gConfig.printDailyHour) return;

  sendDailyPrintJob();
  gLastPrintDay = today;
}

/**
 * Connect to the configured network printer and transmit a plain-text daily
 * fleet-snapshot report via Raw / JetDirect printing (TCP port 9100).
 *
 * Most network-capable laser, inkjet, and thermal printers accept plain ASCII
 * on port 9100 without any additional driver or protocol overhead.  A form-feed
 * character (0x0C) is appended so that the page is automatically ejected.
 *
 * The function is intentionally synchronous: it blocks until the job is
 * transmitted or the connection fails.  Watchdog kicks are inserted around
 * the potentially slow connect() call to prevent a hardware reset.
 */
static void sendDailyPrintJob() {
  IPAddress printerAddr(gConfig.printerIp[0], gConfig.printerIp[1],
                        gConfig.printerIp[2], gConfig.printerIp[3]);

  Serial.print(F("Daily print: connecting to "));
  Serial.print(printerAddr);
  Serial.print(':');
  Serial.println(gConfig.printerPort);

  // Kick watchdog before the blocking connect() call
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #endif
#endif

  EthernetClient printer;
  if (!printer.connect(printerAddr, gConfig.printerPort)) {
    Serial.println(F("Daily print: could not connect to printer — will retry tomorrow"));
    return;
  }

  // ---- Report header ----
  char dateBuf[28];
  epochToDateStr(currentEpoch(), dateBuf, sizeof(dateBuf));

  printer.println(F("================================"));
  printer.println(F("   TANK ALARM DAILY REPORT"));
  printer.print(F("   "));   printer.println(gConfig.viewerName);
  printer.print(F("   "));   printer.println(dateBuf);
  printer.println(F("================================"));
  printer.println();

  // ---- Sensor rows ----
  if (gSensorRecordCount == 0) {
    printer.println(F("   No sensor data available."));
  } else {
    for (uint8_t i = 0; i < gSensorRecordCount; i++) {
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
      mbedWatchdog.kick();
  #endif
#endif
      const SensorRecord &rec = gSensorRecords[i];

      printer.println(F("--------------------------------"));

      // Site / label / user number
      printer.print(F("   "));
      printer.print(rec.site[0] ? rec.site : "Unknown");
      if (rec.label[0]) {
        printer.print(F(" / "));
        printer.print(rec.label);
      }
      if (rec.userNumber > 0) {
        printer.print(F(" #"));
        printer.print((int)rec.userNumber);
      }
      printer.println();

      // Level in feet and inches
      if (rec.levelInches >= 0.0f && isfinite(rec.levelInches)) {
        int feet = (int)(rec.levelInches / 12.0f);
        float remIn = rec.levelInches - (float)(feet * 12);
        char levelBuf[16];
        snprintf(levelBuf, sizeof(levelBuf), "%d' %.1f\"", feet, remIn);
        printer.print(F("   Level:  "));
        printer.println(levelBuf);
      } else {
        printer.println(F("   Level:  --"));
      }

      // 24-hour change
      if (rec.hasChange24h) {
        char changeBuf[16];
        snprintf(changeBuf, sizeof(changeBuf), "%+.1f\"", rec.change24h);
        printer.print(F("   Change: "));
        printer.println(changeBuf);
      }

      // Alarm status
      printer.print(F("   Status: "));
      if (rec.alarmActive) {
        printer.print(rec.alarmType[0] ? rec.alarmType : "ALARM");
        printer.println(F("  *ALARM*"));
      } else {
        printer.println(F("Normal"));
      }

      // Last updated
      char updBuf[28];
      epochToDateStr(rec.lastUpdateEpoch, updBuf, sizeof(updBuf));
      printer.print(F("   Upd:    "));
      printer.println(updBuf);
    }
    printer.println(F("--------------------------------"));
  }

  // ---- Report footer ----
  printer.println();
  printer.println(F("================================"));
  printer.print(F("   "));
  printer.println(gViewerUid[0] ? gViewerUid : "Viewer");
  printer.print(F("   Firmware v"));
  printer.println(F(FIRMWARE_VERSION));
  printer.println(F("================================"));
  printer.println('\f');  // Form-feed — ejects the page on most printers

  printer.flush();
  safeSleep(500);
  printer.stop();

  Serial.println(F("Daily print job sent successfully"));
}
