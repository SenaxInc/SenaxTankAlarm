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

#define DEVICE_ROLE TANKALARM_ROLE_VIEWER

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

// The viewer accepts small POSTs (contact management, update requests); cap request
// bodies to avoid memory exhaustion.
#ifndef MAX_HTTP_BODY_BYTES
#define MAX_HTTP_BODY_BYTES 8192
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

// ---- Network configuration defaults (override in ViewerConfig.h for a custom build) ----
// #313 follow-up: the viewer tries DHCP first. If the router's DHCP pool is
// unavailable (or the site router hands out no lease), it falls back to the
// static profile below. Defaults follow the Starlink router convention:
// gateway 192.168.1.1, subnet /24, DHCP pool 192.168.1.20-254 - so addresses
// .2-.19 are never auto-assigned and 192.168.1.15 is a safe manual choice.
// The server can also push a network profile over Notehub (viewer summary
// "net" object); a pushed profile overrides these compile-time defaults and
// persists across reboots when QSPI storage is available.
#ifndef VIEWER_USE_STATIC_IP
#define VIEWER_USE_STATIC_IP false // false = DHCP first with static fallback; true = static only
#endif
#ifndef VIEWER_STATIC_IP_1
#define VIEWER_STATIC_IP_1 192
#define VIEWER_STATIC_IP_2 168
#define VIEWER_STATIC_IP_3 1
#define VIEWER_STATIC_IP_4 15      // Starlink-safe manual range: .2-.19
#endif
#ifndef VIEWER_GATEWAY_1
#define VIEWER_GATEWAY_1 192
#define VIEWER_GATEWAY_2 168
#define VIEWER_GATEWAY_3 1
#define VIEWER_GATEWAY_4 1         // Starlink router IP
#endif
#ifndef VIEWER_SUBNET_1
#define VIEWER_SUBNET_1 255
#define VIEWER_SUBNET_2 255
#define VIEWER_SUBNET_3 255
#define VIEWER_SUBNET_4 0
#endif
#ifndef VIEWER_DNS_1
#define VIEWER_DNS_1 1
#define VIEWER_DNS_2 1
#define VIEWER_DNS_3 1
#define VIEWER_DNS_4 1             // Cloudflare DNS
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
  uint16_t printerPort;         // Printer TCP port (9100 = JetDirect/Raw — the only protocol implemented here)
  uint8_t printDailyHour;       // UTC hour (0–23) at which the daily report fires
  double netConfigRev;          // Revision epoch of the last server-pushed network profile (0 = none)
};

struct SensorRecord {
  char clientUid[48];
  char site[32];
  char label[24];
  uint8_t sensorIndex;
  uint8_t userNumber;           // Optional user-assigned display number (0 = unset)
  float currentValue;           // Latest reading in the sensor's own measurement unit
                                // (inches for level, psi for pressure, rpm for engines, etc.).
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
  VIEWER_NAME,                   // viewerName
  DEFAULT_VIEWER_PRODUCT_UID,    // productUid - default, can be overridden
  VIEWER_USE_STATIC_IP,          // useStaticIp - DHCP by default (override in ViewerConfig.h)
  { 0x02, 0x00, 0x01, 0x11, 0x20, 0x25 },  // macAddress
  { VIEWER_STATIC_IP_1, VIEWER_STATIC_IP_2, VIEWER_STATIC_IP_3, VIEWER_STATIC_IP_4 },  // staticIp (Starlink-safe default)
  { VIEWER_GATEWAY_1, VIEWER_GATEWAY_2, VIEWER_GATEWAY_3, VIEWER_GATEWAY_4 },  // staticGateway
  { VIEWER_SUBNET_1, VIEWER_SUBNET_2, VIEWER_SUBNET_3, VIEWER_SUBNET_4 },  // staticSubnet
  { VIEWER_DNS_1, VIEWER_DNS_2, VIEWER_DNS_3, VIEWER_DNS_4 },  // staticDns
  // Printer defaults (override in ViewerConfig.h)
  PRINT_ENABLED,                 // printEnabled
  { PRINTER_IP_1, PRINTER_IP_2, PRINTER_IP_3, PRINTER_IP_4 },  // printerIp
  PRINTER_PORT,                  // printerPort
  PRINT_DAILY_HOUR,              // printDailyHour
  0.0                            // netConfigRev - no server-pushed profile yet
};

static SensorRecord gSensorRecords[MAX_SENSOR_RECORDS];
static uint8_t gSensorRecordCount = 0;

// Printer state
// NOTE: gLastPrintDay is RAM-only and resets to 0 on every power cycle.
// If the device reboots after the scheduled print hour but before midnight UTC,
// a second print job will be dispatched once the hour condition is met again.
// This is typically acceptable for daily reporting; if duplicates are a concern,
// power the device from an uninterrupted supply to minimize unexpected reboots.
static uint32_t gLastPrintDay = 0;            // Day number (epoch/86400) of last successful print
static uint32_t gLastPrintAttemptDay = 0;     // Day number of last print attempt (for throttle reset)
static unsigned long gLastPrintAttemptMs = 0; // millis() of last print attempt (success or fail)
// Retry failed print attempts at most once every 15 minutes within the same UTC day.
#define PRINT_RETRY_INTERVAL_MS (15UL * 60UL * 1000UL)

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
static TankAlarmDfuStatus gDfuStatus;

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

// ---- Viewer-managed contacts (auxiliary contact management) ----
// Contacts added here are sent to the MAIN SERVER via viewer_contacts.qo, stored in the
// server's contact directory with category "viewer", and automatically enrolled as SMS
// alert recipients. The server pushes the authoritative list back inside every viewer
// summary (field "vc"), so admin-side edits show up here too.
struct ViewerContact {
  char id[28];
  char name[32];
  char phone[20];
  char email[48];
};
#define MAX_VIEWER_CONTACTS 12
static ViewerContact gViewerContacts[MAX_VIEWER_CONTACTS];
static uint8_t gViewerContactCount = 0;
static double gViewerContactsSyncedEpoch = 0.0;

// After a manual "Request Update", poll the summary inbox aggressively for a few minutes
// so the server's reply is applied promptly instead of waiting for the next 6-hour slot.
static uint8_t gSummaryFastPolls = 0;
static unsigned long gLastFastPollMillis = 0;

// ============================================================================
// Server-styled web pages (v2.2.0) — visual language mirrors the main server
// dashboard (same palette/card classes). Status-display focused: no config or
// settings pages; auxiliary contact management lives at /contacts.
// ============================================================================

static const char VIEWER_DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Tank Alarm Viewer</title><style> :root{--primary:#0066cc;--primary-hover:#004c99;--bg:#f2f2f2;--card-bg:#ffffff;--text:#333333;--muted:#666666;--border:#cccccc;--danger:#cc0000;--success:#28a745;--warning:#ffc107;--card-border:#d7d7d7} body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:var(--bg);color:var(--text);line-height:1.5} *{box-sizing:border-box} header{background:var(--card-bg);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:100} .bar{max-width:1200px;margin:0 auto;padding:0.75rem 1rem;display:flex;align-items:center;gap:1.25rem;flex-wrap:wrap} .brand{font-weight:700;font-size:1.1rem;white-space:nowrap;color:#201442} nav{display:flex;gap:12px;align-items:center;flex:1} nav a{color:var(--muted);text-decoration:none;font-size:0.95rem;padding:4px 8px} nav a.active{color:var(--primary);font-weight:600;border-bottom:2px solid var(--primary)} .btn{background:var(--primary);color:#fff;border:none;padding:8px 16px;font-size:0.9rem;cursor:pointer;border-radius:4px} .btn:hover{background:var(--primary-hover)} .btn:disabled{opacity:0.5;cursor:default} main{max-width:1200px;margin:0 auto;padding:1rem} .stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin-bottom:20px} .stat-card{background:var(--card-bg);border:1px solid var(--card-border);border-radius:8px;padding:14px;text-align:center} .stat-card span{font-size:0.8rem;color:var(--muted)} .stat-card strong{display:block;font-size:1.6rem;margin-top:4px} .stat-card.alarm strong{color:var(--danger)} #alarmSection{display:none;background:var(--card-bg);border:1px solid var(--danger);border-left:6px solid var(--danger);border-radius:10px;margin-bottom:16px;overflow:hidden} #alarmSection.visible{display:block} #alarmSection .site-head{background:rgba(204,0,0,0.06)} #alarmSection .site-name{color:var(--danger)} .site-section{background:var(--card-bg);border:1px solid var(--card-border);border-radius:10px;margin-bottom:16px;overflow:hidden} .site-head{display:flex;justify-content:space-between;align-items:center;padding:14px 18px;border-bottom:1px solid var(--card-border);flex-wrap:wrap;gap:8px} .site-name{font-size:1.15rem;font-weight:700} .data-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px;padding:14px 18px} .data-card{background:var(--bg);border:1px solid var(--card-border);border-radius:8px;padding:14px;position:relative} .data-card.alarm-card{border-left:4px solid var(--danger)} .dc-type{font-size:0.7rem;text-transform:uppercase;font-weight:600;color:var(--muted);letter-spacing:0.5px;margin-bottom:6px} .dc-name{font-weight:600;font-size:0.95rem;margin-bottom:2px} .dc-value{font-size:1.5rem;font-weight:700;line-height:1.2} .dc-value small{font-size:0.55em;font-weight:400;color:var(--muted)} .dc-meta{font-size:0.8rem;color:var(--muted);margin-top:6px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:4px} .dc-change{font-size:0.8rem;font-weight:500} .dc-change.pos{color:#10b981} .dc-change.neg{color:#ef4444} .dc-alarm{font-size:0.8rem;font-weight:600;color:var(--danger);margin-top:4px} .meta-line{font-size:0.85rem;color:var(--muted);margin:14px 4px;display:flex;gap:16px;flex-wrap:wrap} #updBanner{display:none;background:rgba(255,193,7,0.15);border:1px solid var(--warning);border-radius:8px;padding:10px 14px;margin-bottom:14px;font-size:0.9rem} #toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:10px 20px;border-radius:6px;font-size:0.9rem;opacity:0;transition:opacity 0.3s;pointer-events:none;z-index:200} #toast.show{opacity:1} .empty-state{color:var(--muted);padding:24px;text-align:center} footer{max-width:1200px;margin:0 auto;padding:0 1rem 1.5rem;color:var(--muted);font-size:0.85rem;text-align:center} </style></head><body> <header><div class="bar"><span class="brand">Tank Alarm Viewer</span><nav><a href="/" class="active">Dashboard</a><a href="/contacts">Contacts</a></nav><button class="btn" id="requestUpdateBtn">Request Update</button></div></header> <main> <div id="updBanner"></div> <div id="alarmSection"><div class="site-head"><span class="site-name">&#9888; Active Alarms</span><span id="alarmCount" style="font-size:0.9rem;color:var(--danger);font-weight:600;"></span></div><div class="data-grid" id="alarmGrid"></div></div> <div class="stats-grid"><div class="stat-card"><span>Sensors</span><strong id="statSensors">-</strong></div><div class="stat-card" id="statAlarmCard"><span>Active Alarms</span><strong id="statAlarms">-</strong></div><div class="stat-card"><span>Summary Age</span><strong id="statAge" style="font-size:1.1rem;">-</strong></div><div class="stat-card"><span>Next Fetch</span><strong id="statNext" style="font-size:1.1rem;">-</strong></div></div> <div id="siteSections"><div class="empty-state">Loading&hellip;</div></div> <div class="meta-line"><span id="metaViewer"></span><span id="metaServer"></span></div> </main> <footer>Viewer node &mdash; status display mirror of the Tank Alarm server. Contact changes made here sync to the server.</footer> <div id="toast"></div> <script> function escapeHtml(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));} function showToast(msg,isErr){const t=document.getElementById('toast');t.textContent=msg;t.style.background=isErr?'#b91c1c':'#333';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),3500);} function timeAgo(epoch){if(!epoch)return'never';const s=Math.max(0,Date.now()/1000-epoch);if(s<120)return'just now';if(s<3600)return Math.floor(s/60)+'m ago';return Math.floor(s/3600)+'h ago';} function timeUntil(epoch){if(!epoch)return'-';const s=epoch-Date.now()/1000;if(s<=0)return'due now';if(s<3600)return'in '+Math.ceil(s/60)+'m';return'in '+Math.floor(s/3600)+'h '+Math.round((s%3600)/60)+'m';} function typeLabel(ot){const m={tank:'Tank Level',gas:'Gas Pressure',rpm:'Engine RPM',engine:'Engine',flow:'Flow Rate',pump:'Pump'};return m[ot]||'Sensor';} function unitLabel(mu,ot){if(mu){return mu==='inches'?'in':mu;}const d={tank:'in',gas:'psi',rpm:'rpm',engine:'rpm',flow:'gpm'};return d[ot]||'';} function renderCard(t,withSite){const card=document.createElement('div');card.className='data-card';if(t.a)card.classList.add('alarm-card');const ot=t.ot||'tank';const mu=unitLabel(t.mu,ot);const val=(typeof t.l==='number')?t.l.toFixed(1):'-';let changeHtml='';if(typeof t.d==='number'){const cls=t.d>=0?'pos':'neg';const sign=t.d>=0?'+':'';changeHtml='<span class="dc-change '+cls+'">'+sign+t.d.toFixed(1)+' '+mu+'/24h</span>';} const numLabel=t.un?(' #'+t.un):'';const nameLine=(withSite?escapeHtml(t.s)+' &mdash; ':'')+escapeHtml(t.n||'Sensor')+numLabel; card.innerHTML='<div class="dc-type">'+escapeHtml(typeLabel(ot))+'</div>'+'<div class="dc-name">'+nameLine+'</div>'+'<div class="dc-value">'+val+' <small>'+escapeHtml(mu)+'</small></div>'+(t.a?'<div class="dc-alarm">ALARM: '+escapeHtml(t.at||'active')+'</div>':'')+'<div class="dc-meta"><span>'+(t.u?'Updated '+timeAgo(t.u):'No data yet')+'</span>'+changeHtml+'</div>'; return card;} function applyData(d){document.getElementById('metaViewer').textContent='Viewer: '+(d.vn||'')+' ('+(d.vi||'?')+')';document.getElementById('metaServer').textContent='Server: '+(d.sn||'?')+' ('+(d.si||'?')+')';document.getElementById('statSensors').textContent=(d.sensors||[]).length;document.getElementById('statAge').textContent=d.ge?timeAgo(d.ge):'-';document.getElementById('statNext').textContent=timeUntil(d.nf); const sensors=d.sensors||[];const alarms=sensors.filter(t=>t.a);document.getElementById('statAlarms').textContent=alarms.length;document.getElementById('statAlarmCard').classList.toggle('alarm',alarms.length>0); const alarmSection=document.getElementById('alarmSection');const alarmGrid=document.getElementById('alarmGrid');alarmGrid.innerHTML='';if(alarms.length>0){alarms.forEach(t=>alarmGrid.appendChild(renderCard(t,true)));document.getElementById('alarmCount').textContent=alarms.length+' alarm'+(alarms.length>1?'s':'');alarmSection.classList.add('visible');}else{alarmSection.classList.remove('visible');} const host=document.getElementById('siteSections');host.innerHTML='';if(sensors.length===0){host.innerHTML='<div class="empty-state">No sensor data yet. The server publishes a summary on its schedule &mdash; use Request Update to ask for one now.</div>';return;} const yourSites={};sensors.forEach(t=>{const s=t.s||'Unknown Site';(yourSites[s]=yourSites[s]||[]).push(t);}); Object.keys(yourSites).sort().forEach(site=>{const sec=document.createElement('div');sec.className='site-section';const head=document.createElement('div');head.className='site-head';head.innerHTML='<span class="site-name">'+escapeHtml(site)+'</span>';sec.appendChild(head);const grid=document.createElement('div');grid.className='data-grid';yourSites[site].forEach(t=>grid.appendChild(renderCard(t,false)));sec.appendChild(grid);host.appendChild(sec);});} function fetchData(){fetch('/api/sensors').then(r=>r.json()).then(applyData).catch(e=>console.error('fetch failed',e));} function fetchUpdateBanner(){fetch('/api/github/update').then(r=>r.json()).then(d=>{const b=document.getElementById('updBanner');if(d&&d.available){b.innerHTML='Firmware update available: <strong>'+escapeHtml(d.latestVersion||'')+'</strong>';b.style.display='block';}else{b.style.display='none';}}).catch(()=>{});} document.getElementById('requestUpdateBtn').addEventListener('click',function(){const btn=this;btn.disabled=true;fetch('/api/request-update',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.success){showToast('Update requested — the server will push fresh data shortly.');}else{showToast(d.message||'Request failed',true);}}).catch(e=>showToast('Request failed: '+e.message,true)).finally(()=>{setTimeout(()=>{btn.disabled=false;},5000);});}); fetchData();fetchUpdateBanner();setInterval(fetchData,60000);setInterval(fetchUpdateBanner,3600000); </script></body></html>)HTML";

static const char VIEWER_CONTACTS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Viewer Contacts</title><style> :root{--primary:#0066cc;--primary-hover:#004c99;--bg:#f2f2f2;--card-bg:#ffffff;--text:#333333;--muted:#666666;--border:#cccccc;--danger:#cc0000;--card-border:#d7d7d7} body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:var(--bg);color:var(--text);line-height:1.5} *{box-sizing:border-box} header{background:var(--card-bg);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:100} .bar{max-width:1200px;margin:0 auto;padding:0.75rem 1rem;display:flex;align-items:center;gap:1.25rem;flex-wrap:wrap} .brand{font-weight:700;font-size:1.1rem;white-space:nowrap;color:#201442} nav{display:flex;gap:12px;align-items:center;flex:1} nav a{color:var(--muted);text-decoration:none;font-size:0.95rem;padding:4px 8px} nav a.active{color:var(--primary);font-weight:600;border-bottom:2px solid var(--primary)} main{max-width:800px;margin:0 auto;padding:1rem} .card{background:var(--card-bg);border:1px solid var(--card-border);border-radius:10px;padding:18px;margin-bottom:16px} .card h2{margin:0 0 6px;font-size:1.15rem} .note{font-size:0.85rem;color:var(--muted);margin-bottom:12px} .contact-row{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:10px 12px;border:1px solid var(--card-border);border-radius:8px;background:var(--bg);margin-bottom:8px;flex-wrap:wrap} .contact-name{font-weight:600} .contact-details{font-size:0.85rem;color:var(--muted)} .btn{background:var(--primary);color:#fff;border:none;padding:8px 16px;font-size:0.9rem;cursor:pointer;border-radius:4px} .btn:hover{background:var(--primary-hover)} .btn.danger{background:var(--danger)} .btn:disabled{opacity:0.5;cursor:default} .form-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin-bottom:10px} .field span{display:block;font-size:0.8rem;color:var(--muted);margin-bottom:2px} .field input{width:100%;padding:8px;border:1px solid var(--border);border-radius:4px;font-size:0.95rem} .empty-state{color:var(--muted);padding:16px;text-align:center} #toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:10px 20px;border-radius:6px;font-size:0.9rem;opacity:0;transition:opacity 0.3s;pointer-events:none;z-index:200} #toast.show{opacity:1} .sync-line{font-size:0.8rem;color:var(--muted);margin-top:8px} </style></head><body> <header><div class="bar"><span class="brand">Tank Alarm Viewer</span><nav><a href="/">Dashboard</a><a href="/contacts" class="active">Contacts</a></nav></div></header> <main> <div class="card"><h2>Alarm Notification Contacts</h2><div class="note">Contacts added here are sent to the main server, stored as <strong>viewer contacts</strong> in its directory, and automatically enrolled to receive SMS alarm alerts. The server administrator can also view and edit these contacts. Changes may take a moment to sync.</div> <div id="contactList"><div class="empty-state">Loading&hellip;</div></div> <div class="sync-line" id="syncLine"></div></div> <div class="card"><h2>Add Contact</h2><div class="form-grid"><label class="field"><span>Name *</span><input id="cName" maxlength="31"></label><label class="field"><span>Phone (E.164, e.g. +15551234567)</span><input id="cPhone" maxlength="19"></label><label class="field"><span>Email</span><input id="cEmail" maxlength="47" type="email"></label></div><button class="btn" id="addBtn">+ Add Contact</button><div class="note" style="margin-top:8px;">Provide at least a phone number (for SMS alerts) or an email address. Maximum 12 viewer contacts.</div></div> </main> <div id="toast"></div> <script> function escapeHtml(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));} function showToast(msg,isErr){const t=document.getElementById('toast');t.textContent=msg;t.style.background=isErr?'#b91c1c':'#333';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),3500);} function timeAgo(epoch){if(!epoch)return'never';const s=Math.max(0,Date.now()/1000-epoch);if(s<120)return'just now';if(s<3600)return Math.floor(s/60)+'m ago';return Math.floor(s/3600)+'h ago';} let contacts=[]; function render(){const host=document.getElementById('contactList');if(contacts.length===0){host.innerHTML='<div class="empty-state">No viewer contacts yet.</div>';return;}host.innerHTML=contacts.map((c,i)=>'<div class="contact-row"><div><div class="contact-name">'+escapeHtml(c.name)+'</div><div class="contact-details">'+(c.phone?escapeHtml(c.phone):'')+(c.phone&&c.email?' &middot; ':'')+(c.email?escapeHtml(c.email):'')+'</div></div><button class="btn danger" data-i="'+i+'">Remove</button></div>').join('');host.querySelectorAll('button[data-i]').forEach(b=>b.addEventListener('click',()=>removeContact(parseInt(b.dataset.i,10))));} function load(){fetch('/api/contacts').then(r=>r.json()).then(d=>{contacts=d.contacts||[];render();document.getElementById('syncLine').textContent=d.synced?('Last synced from server '+timeAgo(d.synced)):'Not yet synced from server.';}).catch(e=>showToast('Load failed: '+e.message,true));} function save(){return fetch('/api/contacts',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({contacts:contacts})}).then(r=>r.json()).then(d=>{if(d.success){showToast('Saved — syncing to server.');}else{showToast(d.message||'Save failed',true);load();}return d.success;});} function removeContact(i){if(!confirm('Remove '+contacts[i].name+' from alarm notifications?'))return;contacts.splice(i,1);render();save();} document.getElementById('addBtn').addEventListener('click',()=>{const name=document.getElementById('cName').value.trim();const phone=document.getElementById('cPhone').value.trim();const email=document.getElementById('cEmail').value.trim(); if(!name){showToast('Name is required',true);return;} if(!phone&&!email){showToast('Provide a phone number or email',true);return;} if(phone&&!/^\+[0-9]{7,15}$/.test(phone)){showToast('Phone must be E.164 format, e.g. +15551234567',true);return;} if(contacts.length>=12){showToast('Maximum 12 viewer contacts',true);return;} contacts.push({name:name,phone:phone,email:email});render();save().then(ok=>{if(ok){document.getElementById('cName').value='';document.getElementById('cPhone').value='';document.getElementById('cEmail').value='';}});}); load();setInterval(load,120000); </script></body></html>)HTML";

static void initializeNotecard();
static void initializeEthernet();
static void initializeNetConfigStorage();
static bool loadNetConfig();
static bool saveNetConfig();
static void applyServerNetConfig(JsonObjectConst net);
static void handleWebRequests();
static bool readHttpRequest(EthernetClient &client, String &method, String &path, String &body, size_t &contentLength, bool &bodyTooLarge);
static void respondJson(EthernetClient &client, const String &body);
static void respondStatus(EthernetClient &client, int status, const char *message);
static void sendDashboard(EthernetClient &client);
static void sendContactsPage(EthernetClient &client);
static void sendHtmlPage(EthernetClient &client, const char *html);
static void sendSensorJson(EthernetClient &client);
static void handleContactsGet(EthernetClient &client);
static void handleContactsPost(EthernetClient &client, const String &body);
static void handleRequestUpdatePost(EthernetClient &client);
static bool viewerSendNote(const char *file, JsonDocument &doc);
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
static bool sendDailyPrintJob();

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

// ============================================================================
// Network profile persistence (QSPI partition 4, LittleFS)
// ============================================================================
// Mirrors the client's app-data mount: partition 4 only, never reformat the
// whole device. If the board has no MBR (never provisioned), the viewer runs
// without persistence - the server re-pushes the profile in every summary, so
// the configuration self-heals after the next fetch (cellular, not Ethernet).
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
#include "BlockDevice.h"
#include "MBRBlockDevice.h"
#include "LittleFileSystem.h"
#define VIEWER_NET_CONFIG_PATH "/vcfg/viewer_net.json"
#define VIEWER_APP_DATA_PARTITION 4
static BlockDevice *gNetCfgBD = nullptr;
static mbed::MBRBlockDevice *gNetCfgPart = nullptr;
static LittleFileSystem *gNetCfgFS = nullptr;
static bool gNetCfgStorageOk = false;
#endif
static bool gEthernetStarted = false;  // guards live re-init before first bring-up

static void initializeNetConfigStorage() {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  gNetCfgStorageOk = false;
  gNetCfgBD = BlockDevice::get_default_instance();
  if (!gNetCfgBD) {
    Serial.println(F("Net config: no block device - profile will not persist"));
    return;
  }
  gNetCfgPart = new mbed::MBRBlockDevice(gNetCfgBD, VIEWER_APP_DATA_PARTITION);
  if (gNetCfgPart->init() != 0) {
    Serial.println(F("Net config: QSPI partition 4 not found - profile will not persist"));
    delete gNetCfgPart;
    gNetCfgPart = nullptr;
    return;
  }
  gNetCfgFS = new LittleFileSystem("vcfg");
  int err = gNetCfgFS->mount(gNetCfgPart);
  if (err) {
    Serial.println(F("Net config: mount failed, formatting partition 4..."));
    err = gNetCfgFS->reformat(gNetCfgPart);
    if (err) {
      Serial.println(F("Net config: format failed - profile will not persist"));
      delete gNetCfgFS;
      gNetCfgFS = nullptr;
      return;
    }
  }
  gNetCfgStorageOk = true;
  Serial.println(F("Net config storage ready (QSPI p4)"));
#endif
}

static bool loadNetConfig() {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!gNetCfgStorageOk) return false;
  FILE *f = fopen(VIEWER_NET_CONFIG_PATH, "r");
  if (!f) return false;
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  JsonDocument doc;
  if (deserializeJson(doc, buf) != DeserializationError::Ok) return false;
  gConfig.useStaticIp = doc["m"].as<int>() == 1;
  JsonArrayConst ip = doc["ip"], gw = doc["gw"], sn = doc["sn"], dns = doc["dns"];
  for (uint8_t i = 0; i < 4; i++) {
    if (ip.size() == 4) gConfig.staticIp[i] = ip[i].as<uint8_t>();
    if (gw.size() == 4) gConfig.staticGateway[i] = gw[i].as<uint8_t>();
    if (sn.size() == 4) gConfig.staticSubnet[i] = sn[i].as<uint8_t>();
    if (dns.size() == 4) gConfig.staticDns[i] = dns[i].as<uint8_t>();
  }
  gConfig.netConfigRev = doc["rev"] | 0.0;
  Serial.print(F("Net profile loaded ("));
  Serial.print(gConfig.useStaticIp ? F("static ") : F("DHCP, fallback "));
  Serial.print(gConfig.staticIp[0]); Serial.print('.');
  Serial.print(gConfig.staticIp[1]); Serial.print('.');
  Serial.print(gConfig.staticIp[2]); Serial.print('.');
  Serial.print(gConfig.staticIp[3]);
  Serial.println(F(")"));
  return true;
#else
  return false;
#endif
}

static bool saveNetConfig() {
#if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
  if (!gNetCfgStorageOk) return false;
  JsonDocument doc;
  doc["m"] = gConfig.useStaticIp ? 1 : 0;
  JsonArray ip = doc["ip"].to<JsonArray>();
  JsonArray gw = doc["gw"].to<JsonArray>();
  JsonArray sn = doc["sn"].to<JsonArray>();
  JsonArray dns = doc["dns"].to<JsonArray>();
  for (uint8_t i = 0; i < 4; i++) {
    ip.add(gConfig.staticIp[i]);
    gw.add(gConfig.staticGateway[i]);
    sn.add(gConfig.staticSubnet[i]);
    dns.add(gConfig.staticDns[i]);
  }
  doc["rev"] = gConfig.netConfigRev;
  char buf[256];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len == 0 || len >= sizeof(buf)) return false;
  FILE *f = fopen(VIEWER_NET_CONFIG_PATH ".tmp", "w");
  if (!f) return false;
  size_t written = fwrite(buf, 1, len, f);
  fclose(f);
  if (written != len) {
    remove(VIEWER_NET_CONFIG_PATH ".tmp");
    return false;
  }
  remove(VIEWER_NET_CONFIG_PATH);
  if (rename(VIEWER_NET_CONFIG_PATH ".tmp", VIEWER_NET_CONFIG_PATH) != 0) return false;
  Serial.println(F("Net profile saved"));
  return true;
#else
  return false;
#endif
}

// Parse "a.b.c.d" into 4 octets; returns false on malformed input.
static bool parseIpString(const char *s, uint8_t out[4]) {
  if (!s || !*s) return false;
  unsigned int a, b, c, d;
  char extra;
  if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
  return true;
}

// Apply a server-pushed network profile from the viewer summary's "net" object.
// {m:0|1, ip:"a.b.c.d", gw:..., sn:..., dns:..., rev:<epoch>}
// rev guards re-application: profiles are applied once, survive reboots when
// storage is available, and are re-offered in every summary (self-healing when
// storage is unavailable).
static void applyServerNetConfig(JsonObjectConst net) {
  double rev = net["rev"] | 0.0;
  if (rev <= 0.0 || rev <= gConfig.netConfigRev) {
    return;  // nothing new
  }
  bool wantStatic = (net["m"] | 0) == 1;
  uint8_t ip[4], gw[4], sn[4], dns[4];
  if (wantStatic) {
    // Static mode requires at least a valid IP; gateway/subnet/DNS keep current
    // values when absent or malformed.
    if (!parseIpString(net["ip"] | "", ip)) {
      Serial.println(F("Server net profile rejected: bad ip"));
      return;
    }
    memcpy(gConfig.staticIp, ip, 4);
    if (parseIpString(net["gw"] | "", gw)) memcpy(gConfig.staticGateway, gw, 4);
    if (parseIpString(net["sn"] | "", sn)) memcpy(gConfig.staticSubnet, sn, 4);
    if (parseIpString(net["dns"] | "", dns)) memcpy(gConfig.staticDns, dns, 4);
  } else {
    // DHCP mode may still update the fallback profile when provided.
    if (parseIpString(net["ip"] | "", ip)) memcpy(gConfig.staticIp, ip, 4);
    if (parseIpString(net["gw"] | "", gw)) memcpy(gConfig.staticGateway, gw, 4);
    if (parseIpString(net["sn"] | "", sn)) memcpy(gConfig.staticSubnet, sn, 4);
    if (parseIpString(net["dns"] | "", dns)) memcpy(gConfig.staticDns, dns, 4);
  }
  gConfig.useStaticIp = wantStatic;
  gConfig.netConfigRev = rev;
  saveNetConfig();  // best effort; without storage the summary re-applies it

  Serial.print(F("Server net profile applied: "));
  Serial.print(wantStatic ? F("static ") : F("DHCP (fallback "));
  Serial.print(gConfig.staticIp[0]); Serial.print('.');
  Serial.print(gConfig.staticIp[1]); Serial.print('.');
  Serial.print(gConfig.staticIp[2]); Serial.print('.');
  Serial.print(gConfig.staticIp[3]);
  Serial.println(wantStatic ? F("") : F(")"));

  // Re-initialize the network with the new profile. Only after the first
  // bring-up: during boot the summary is fetched before Ethernet starts and
  // initializeEthernet() will use the freshly applied profile anyway.
  if (gEthernetStarted) {
    Serial.println(F("Re-initializing Ethernet with new profile..."));
    initializeEthernet();
    gWebServer.begin();
  }
}

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

  // Restore any persisted network profile BEFORE the first summary fetch and
  // Ethernet bring-up (rev dedupe + correct boot addressing).
  initializeNetConfigStorage();
  loadNetConfig();

  initializeNotecard();
#if defined(TANKALARM_DFU_MCUBOOT)
  tankalarm_resolvePendingOta(notecard);
#endif
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
#if defined(TANKALARM_DFU_MCUBOOT)
  // 15.6 Viewer Offline-Safe Local Health Gate
  if (gNotecardAvailable) {
    tankalarm_markFirmwareHealthy();
  }
#endif

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

  // v2.2.0: after a manual Request Update, poll the inbox every 30 s (up to 10 times) so
  // the server's fresh summary is applied within seconds-to-minutes instead of hours.
  if (gSummaryFastPolls > 0 && (millis() - gLastFastPollMillis) >= 30000UL) {
    gLastFastPollMillis = millis();
    gSummaryFastPolls--;
    fetchViewerSummary();
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
    // v2.2.0: sync:true so inbound viewer_summary.qi notes are pushed to the Notecard the
    // moment Notehub queues them — required for the Request Update round-trip to be prompt
    // (same fix as the server's v1.9.17 real-time inbound sync).
    JAddBoolToObject(req, "sync", true);
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
    // #313 follow-up: DHCP unavailable (no router lease). Fall back to the
    // static profile so the dashboard stays reachable at a predictable address
    // (default 192.168.1.15 - inside the Starlink router's never-auto-assigned
    // .2-.19 range; override via ViewerConfig.h or a server-pushed profile).
    if (status == 0 && !gConfig.useStaticIp) {
      Serial.print(F("Falling back to static IP "));
      Serial.print(staticIp);
      Serial.println(F(" ..."));
      status = Ethernet.begin(gConfig.macAddress, staticIp, staticDns, staticGateway, staticSubnet);
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
  gEthernetStarted = true;
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
  } else if (method == "GET" && path == "/contacts") {
    sendContactsPage(client);
  } else if (method == "GET" && path == "/api/sensors") {
    sendSensorJson(client);
  } else if (method == "GET" && path == "/api/contacts") {
    handleContactsGet(client);
  } else if (method == "POST" && path == "/api/contacts") {
    handleContactsPost(client, body);
  } else if (method == "POST" && path == "/api/request-update") {
    handleRequestUpdatePost(client);
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
  sendHtmlPage(client, VIEWER_DASHBOARD_HTML);
}

static void sendContactsPage(EthernetClient &client) {
  sendHtmlPage(client, VIEWER_CONTACTS_HTML);
}

static void sendHtmlPage(EthernetClient &client, const char *html) {
  size_t htmlLen = strlen_P(html);
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
  const char* ptr = html;

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
    obj["l"] = gSensorRecords[i].currentValue;
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

// ============================================================================
// Outbound notes + contact management (v2.2.0)
// ============================================================================

// Queue a note to Notehub with sync:true. Returns true when the Notecard accepted it.
// No offline buffering: callers surface failures to the UI so the user can retry.
static bool viewerSendNote(const char *file, JsonDocument &doc) {
  if (!gNotecardAvailable) {
    return false;
  }
  doc["_sv"] = NOTEFILE_SCHEMA_VERSION;
  String payload;
  if (serializeJson(doc, payload) == 0) {
    return false;
  }
  J *req = notecard.newRequest("note.add");
  if (!req) {
    gNotecardFailureCount++;
    return false;
  }
  JAddStringToObject(req, "file", file);
  JAddBoolToObject(req, "sync", true);
  J *body = JParse(payload.c_str());
  if (!body) {
    JDelete(req);
    return false;
  }
  JAddItemToObject(req, "body", body);
#ifdef TANKALARM_WATCHDOG_AVAILABLE
  #if defined(ARDUINO_OPTA) || defined(ARDUINO_ARCH_MBED)
    mbedWatchdog.kick();
  #endif
#endif
  J *rsp = notecard.requestAndResponse(req);
  bool ok = false;
  if (rsp) {
    const char *err = JGetString(rsp, "err");
    ok = !(err && err[0] != '\0');
    if (!ok) {
      Serial.print(F("viewerSendNote failed: "));
      Serial.println(err);
    }
    notecard.deleteResponse(rsp);
  } else {
    Serial.println(F("viewerSendNote: no response from Notecard"));
    gNotecardFailureCount++;
  }
  return ok;
}

// GET /api/contacts — the viewer's copy of its server-stored contacts.
static void handleContactsGet(EthernetClient &client) {
  JsonDocument doc;
  doc["synced"] = gViewerContactsSyncedEpoch;
  JsonArray arr = doc["contacts"].to<JsonArray>();
  for (uint8_t i = 0; i < gViewerContactCount; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = gViewerContacts[i].id;
    o["name"] = gViewerContacts[i].name;
    if (gViewerContacts[i].phone[0] != '\0') o["phone"] = gViewerContacts[i].phone;
    if (gViewerContacts[i].email[0] != '\0') o["email"] = gViewerContacts[i].email;
  }
  String out;
  serializeJson(doc, out);
  respondJson(client, out);
}

// POST /api/contacts — full-list replace. Validates, updates the local copy, and queues
// the list to the server (viewer_contacts.qo). The server stores these with category
// "viewer" and enrolls phones as SMS alert recipients; the authoritative list returns in
// the next viewer summary (field "vc").
static void handleContactsPost(EthernetClient &client, const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    respondStatus(client, 400, "Invalid JSON");
    return;
  }
  JsonArray incoming = doc["contacts"].as<JsonArray>();
  if (incoming.isNull()) {
    respondStatus(client, 400, "Missing contacts array");
    return;
  }
  if (incoming.size() > MAX_VIEWER_CONTACTS) {
    respondStatus(client, 400, "Too many contacts (max 12)");
    return;
  }

  ViewerContact staged[MAX_VIEWER_CONTACTS];
  uint8_t stagedCount = 0;
  double now = currentEpoch();
  for (JsonVariant v : incoming) {
    const char *name = v["name"] | "";
    const char *phone = v["phone"] | "";
    const char *email = v["email"] | "";
    if (name[0] == '\0' || (phone[0] == '\0' && email[0] == '\0')) {
      respondStatus(client, 400, "Each contact needs a name and a phone or email");
      return;
    }
    ViewerContact &c = staged[stagedCount];
    memset(&c, 0, sizeof(c));
    const char *id = v["id"] | "";
    if (id[0] != '\0') {
      strlcpy(c.id, id, sizeof(c.id));
    } else {
      snprintf(c.id, sizeof(c.id), "vc_%lu_%u", (unsigned long)(now > 0.0 ? now : millis()), stagedCount);
    }
    strlcpy(c.name, name, sizeof(c.name));
    strlcpy(c.phone, phone, sizeof(c.phone));
    strlcpy(c.email, email, sizeof(c.email));
    stagedCount++;
  }

  // Queue to the server first — only adopt locally when the note was accepted, so the
  // local view never silently diverges from what the server will store.
  JsonDocument note;
  note["vi"] = gViewerUid;
  note["t"] = now;
  JsonArray arr = note["contacts"].to<JsonArray>();
  for (uint8_t i = 0; i < stagedCount; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = staged[i].id;
    o["name"] = staged[i].name;
    if (staged[i].phone[0] != '\0') o["phone"] = staged[i].phone;
    if (staged[i].email[0] != '\0') o["email"] = staged[i].email;
  }
  bool queued = viewerSendNote(VIEWER_CONTACTS_OUTBOX_FILE, note);

  JsonDocument response;
  response["success"] = queued;
  response["queued"] = queued;
  if (queued) {
    for (uint8_t i = 0; i < stagedCount; ++i) {
      gViewerContacts[i] = staged[i];
    }
    gViewerContactCount = stagedCount;
    response["message"] = "Contacts queued to server";
    Serial.print(F("Viewer contacts queued to server ("));
    Serial.print(stagedCount);
    Serial.println(F(" contacts)"));
  } else {
    response["message"] = "Notecard unavailable - changes not saved, try again";
  }
  String out;
  serializeJson(response, out);
  respondJson(client, out);
}

// POST /api/request-update — ask the server to publish a fresh viewer summary now.
static void handleRequestUpdatePost(EthernetClient &client) {
  JsonDocument note;
  note["vi"] = gViewerUid;
  note["t"] = currentEpoch();
  bool queued = viewerSendNote(VIEWER_REQUEST_OUTBOX_FILE, note);
  if (queued) {
    // Poll the summary inbox every 30 s for the next ~5 min to catch the reply promptly.
    gSummaryFastPolls = 10;
    gLastFastPollMillis = millis();
    Serial.println(F("Update request queued to server"));
  }
  JsonDocument response;
  response["success"] = queued;
  response["message"] = queued ? "Update requested" : "Notecard unavailable - try again";
  String out;
  serializeJson(response, out);
  respondJson(client, out);
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
    if (sv != NOTEFILE_SCHEMA_VERSION) {
      Serial.print(F("WARNING: Viewer summary schema version "));
      Serial.print(sv);
      Serial.print(F(" (expected "));
      Serial.print(NOTEFILE_SCHEMA_VERSION);
      Serial.println(F(") — fields may be missing or changed"));
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
      rec.currentValue = item["l"].as<float>();
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

  // #313 follow-up: server-pushed network profile (server settings -> Notehub ->
  // viewer). Applied once per revision; persists across reboots when QSPI
  // storage is available.
  if (doc["net"].is<JsonObjectConst>()) {
    applyServerNetConfig(doc["net"].as<JsonObjectConst>());
  }
  gSummaryFastPolls = 0;  // summary received — stop any post-request fast polling

  // v2.2.0: viewer-managed contacts echoed back by the server (authoritative list,
  // including any admin-side edits made on the server's contact directory).
  if (doc["vc"].is<JsonArray>()) {
    gViewerContactCount = 0;
    for (JsonVariantConst item : doc["vc"].as<JsonArrayConst>()) {
      if (gViewerContactCount >= MAX_VIEWER_CONTACTS) break;
      ViewerContact &c = gViewerContacts[gViewerContactCount];
      memset(&c, 0, sizeof(c));
      strlcpy(c.id, item["id"] | "", sizeof(c.id));
      strlcpy(c.name, item["name"] | "", sizeof(c.name));
      strlcpy(c.phone, item["phone"] | "", sizeof(c.phone));
      strlcpy(c.email, item["email"] | "", sizeof(c.email));
      if (c.name[0] != '\0') {
        gViewerContactCount++;
      }
    }
    gViewerContactsSyncedEpoch = gLastSummaryFetchEpoch;
  }

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
    // Check blacklist first
#if defined(TANKALARM_DFU_MCUBOOT)
    if (tankalarm_isVersionBlacklisted(status.version)) {
      Serial.print(F("DFU: Version "));
      Serial.print(status.version);
      Serial.println(F(" is locally blacklisted. Skipping download."));
      return;
    }
#endif

    gDfuStatus = status;

    Serial.print(F("Firmware update available: v"));
    Serial.println(status.version);
    gDfuUpdateAvailable = true;
    strlcpy(gDfuVersion, status.version, sizeof(gDfuVersion));

    // Auto-apply: headless device with no UI — apply updates automatically
    Serial.println(F("Auto-enabling IAP DFU (MCUboot)..."));
    enableDfuMode();
  } else {
    Serial.println(F("No firmware update available"));
    gDfuUpdateAvailable = false;
    gDfuVersion[0] = '\0';
    gDfuFirmwareLength = 0;
    memset(&gDfuStatus, 0, sizeof(gDfuStatus));
  }
}

static void enableDfuMode() {
  if (!gDfuUpdateAvailable) {
    Serial.println(F("No DFU update available to enable"));
    return;
  }

  if (gDfuFirmwareLength == 0) {
    Serial.println(F("ERROR: No firmware length — run checkForFirmwareUpdate first"));
    return;
  }

  Serial.print(F("Enabling IAP DFU (MCUboot) for version: "));
  Serial.println(gDfuVersion);

  gDfuInProgress = true;

  // Viewer always uses "continuous" mode (Ethernet / High Power)
#if defined(TANKALARM_DFU_MCUBOOT)
  bool success = tankalarm_performMcubootUpdate(
      notecard, gDfuStatus, "continuous", DEVICE_ROLE, dfuKickWatchdog);
#else
  bool success = false;
  // MCUboot DFU support is not compiled in — stop the pending update so the
  // Viewer does not repeatedly attempt to apply it on every DFU check cycle.
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

  // If we get here, update failed (success path reboots via NVIC_SystemReset)
  if (!success) {
    Serial.println(F("IAP DFU update failed (MCUboot) — resuming normal operation"));
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
 * @param bufLen Size of buf (must be at least 24 bytes)
 */
static void epochToDateStr(double epoch, char *buf, size_t bufLen) {
  if (epoch < 0.0 || !buf || bufLen < 24) {
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
 * On connect failure the day is NOT marked as done; the job is retried at most
 * once every PRINT_RETRY_INTERVAL_MS (15 min) until it succeeds or a new
 * UTC day begins.
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

  // Validate configuration — warn once and bail if misconfigured
  if (gConfig.printDailyHour > 23 || gConfig.printerPort == 0) {
    static bool sWarnedBadConfig = false;
    if (!sWarnedBadConfig) {
      Serial.println(F("Daily print: invalid config — printDailyHour must be 0–23 and printerPort must be non-zero"));
      sWarnedBadConfig = true;
    }
    return;
  }

  double epoch = currentEpoch();
  if (epoch < 1000000.0) return;  // Clock not yet synced (pre-1982)

  uint32_t epochSeconds = (uint32_t)epoch;
  uint32_t today        = epochSeconds / 86400U;  // Day index since 1970-01-01
  if (today == gLastPrintDay) return;             // Already printed today

  // Only fire at or after the configured UTC hour
  uint32_t secondsOfDay = epochSeconds % 86400U;
  uint8_t  currentHour  = (uint8_t)(secondsOfDay / 3600U);
  if (currentHour < gConfig.printDailyHour) return;

  // Reset the per-attempt throttle when a new UTC day begins so the first
  // attempt on a given day is never delayed by the previous day's attempt.
  if (today != gLastPrintAttemptDay) {
    gLastPrintAttemptMs = 0;
  }

  // Throttle retries — don't hammer the printer every loop() iteration
  unsigned long now = millis();
  if (gLastPrintAttemptMs != 0 && (now - gLastPrintAttemptMs) < PRINT_RETRY_INTERVAL_MS) {
    return;
  }
  gLastPrintAttemptDay = today;
  gLastPrintAttemptMs  = now;

  if (sendDailyPrintJob()) {
    gLastPrintDay = today;  // Mark success; suppress further attempts today
  }
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
 *
 * @return true on successful send, false on connection failure.
 */
static bool sendDailyPrintJob() {
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
    Serial.println(F("Daily print: could not connect to printer — will retry in 15 min"));
    return false;
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
      if (rec.currentValue >= 0.0f && isfinite(rec.currentValue)) {
        int feet = (int)(rec.currentValue / 12.0f);
        float remIn = rec.currentValue - (float)(feet * 12);
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
  // Check that the connection is still alive after all data has been flushed.
  // A disconnected socket here means the printer closed the connection mid-job;
  // returning false causes checkDailyPrint() to retry rather than marking today done.
  if (!printer.connected()) {
    safeSleep(100);
    printer.stop();
    Serial.println(F("Daily print: connection lost during transfer — will retry in 15 min"));
    return false;
  }
  safeSleep(500);
  printer.stop();

  Serial.println(F("Daily print job sent successfully"));
  return true;
}
