# Historical Data Architecture - TankAlarm 112025 Server

## Storage Capacity & Multi-Year Strategy

### LittleFS Limitations
- **Total Capacity**: ~2MB on Arduino Opta
- **Practical Limit**: ~1.5MB for historical data (rest for config, alarms, contacts)
- **When Full**: Write operations fail silently, system continues but stops logging new history

### Tiered Storage Model

```
┌─────────────────────────────────────────────────────────────────┐
│                    TIERED DATA STORAGE                          │
├─────────────────────────────────────────────────────────────────┤
│  HOT TIER (LittleFS - 1.5MB)                                    │
│  ├─ Last 7-30 days of detailed readings                         │
│  ├─ Hourly telemetry samples                                    │
│  ├─ All recent alarms with full context                         │
│  └─ Auto-pruned when 80% full                                   │
├─────────────────────────────────────────────────────────────────┤
│  WARM TIER (FTP Server - Optional)                              │
│  ├─ Monthly rolled data files (YYYYMM_history.json)             │
│  ├─ Daily aggregated summaries                                  │
│  ├─ 2+ years of queryable data                                  │
│  └─ Downloaded on-demand to RAM for charting                    │
├─────────────────────────────────────────────────────────────────┤
│  COLD TIER (FTP Archive)                                        │
│  ├─ Yearly compressed archives (YYYY_archive.json.gz)           │
│  ├─ Full alarm history with resolution details                  │
│  └─ Accessed only for compliance/audit                          │
└─────────────────────────────────────────────────────────────────┘
```

### Data Retention Calculations

| Data Type | Size/Record | Records/Day | 30-Day Size | 1-Year Size |
|-----------|-------------|-------------|-------------|-------------|
| Telemetry | ~100 bytes | 48/tank | ~144KB | ~1.7MB |
| Alarms | ~150 bytes | ~2/tank | ~9KB | ~110KB |
| Daily Summary | ~200 bytes | 1/tank | ~6KB | ~73KB |
| Monthly Summary | ~100 bytes | 0.033/tank | ~100B | ~1.2KB |

**For 10 tanks over 30 days**: ~1.5MB ✓ (fits in LittleFS)  
**For 10 tanks over 1 year**: ~19MB ✗ (requires tiered storage)

### Automatic Data Management

When LittleFS reaches **80% capacity**:
1. **Export**: Upload oldest month to FTP server (if configured)
2. **Aggregate**: Convert hourly readings to daily summaries
3. **Prune**: Delete raw hourly data older than retention period
4. **Log**: Record pruning event for audit trail

### FTP Sync Schedule
- **Daily at 3:00 AM**: Sync previous day's detailed data to warm tier
- **Monthly on 1st**: Roll previous month into monthly archive
- **Yearly on Jan 1**: Compress previous year to cold archive

### Multi-Year Data Access

```javascript
// Historical Data page fetches data in tiers:
async function loadHistoricalData(range) {
  if (range <= '30d') {
    // HOT: Fetch from local LittleFS API (fast, <100ms)
    return await fetch('/api/history?range=' + range);
  } else if (range <= '2y') {
    // WARM: Server downloads from FTP, returns to client (1-5 sec)
    return await fetch('/api/history/archived?range=' + range);
  } else {
    // COLD: May require decompression (5-30 sec)
    return await fetch('/api/history/archive?year=' + year);
  }
}
```

### What If No FTP Server?

Without FTP, local storage is limited to ~30 days. Options:

1. **Accept limitation**: 30 days is sufficient for most operational needs
2. **Use Notehub Routes**: Push data to cloud database (see below)
3. **Manual export**: Download CSV monthly via web UI
4. **SD Card**: Add external storage via Portenta carrier

---

## Overview

The Historical Data page provides charts and graphs for visualizing collected telemetry, alarm history, and trends over time. This document outlines the architecture decisions and implementation details.

## Data Storage Strategy

### Primary: Local LittleFS Storage
- **Hot tier**: RAM ring of 90 snapshots per sensor (20 sensors fleet-wide), saved hourly to `/fs/history/hot_tier.json`
- **Daily Summary (warm tier)**: `/fs/history/daily_YYYYMM.json` - one row per sensor per day, see below
- *Not implemented*: the `/history/telemetry_YYYYMM.log` and `/history/alarms.log` files described in earlier drafts

### Warm Tier: Daily Summaries on Flash
Each month is one JSON array in `/fs/history/daily_YYYYMM.json` (UTC days). Each
row summarizes one sensor for one day, about 120 bytes:

```json
{"d":20260921,"c":"dev:864475...","k":1,"mn":41.2,"mx":44.0,"av":42.7,"op":41.2,"cl":44.0,"al":0,"vt":12.4,"n":24}
```

`d` date (YYYYMMDD), `c` client UID, `k` sensor index, `mn`/`mx`/`av` min/max/mean
level, `op`/`cl` level of the earliest/latest snapshot, `al` alarm count, `vt` mean
voltage (0 = none), `n` snapshots summarized. A row is identified by (`d`, `c`, `k`).
A full 20-sensor month is about 76 KB.

How it is maintained (S-D03, `WarmTierStore.h`; host tests in `tests/host/warm_store`):
- **Hourly rollup**, run before the hot-tier prune. It rolls every day the hot tier
  still holds that has not been rolled yet, up to yesterday, in batches of at most
  8 days within one month: at most 16 batches, 2 file rewrites and 8 s per hour.
  Missed days (outage, reboot, clock not yet set) are caught up; days before the
  oldest hot-tier snapshot, before the retained months, or more than 92 days back
  are not.
- **After a reboot** the first rollup re-checks the whole window. A day whose row is
  missing, or whose recomputed row has a larger `n`, is written; unchanged days are
  only read. A snapshot that arrives for a day already rolled up marks that day, and
  the next rollup re-rolls it.
- **Merge rules**: files are read one row at a time through a small buffer, so memory
  use does not depend on the file size. A stored row is replaced only by a row with a
  larger `n` (keeping the higher alarm count); all other rows are copied byte for byte,
  including keys this firmware does not know. If nothing changes, nothing is written.
- **Failures never empty a file.** Only a missing file (ENOENT) starts a new one. A
  read, write or allocation failure leaves the file as it was and is retried the next
  hour. A file that cannot be parsed is rebuilt from its readable rows plus the new
  rows, and the original is kept as `daily_YYYYMM.json.bad`. Writes go to
  `daily_YYYYMM.json.tmp` and are renamed over the file.
- **Retention**: the current month and the 3 months before it
  (`MAX_DAILY_SUMMARY_MONTHS`). The hourly prune removes older month files with their
  `.tmp`/`.bad` files, removes orphan `.tmp` files, and sets `warmTierAvailable`
  (also checked at boot).
- **Readers** (`/api/history/compare`, `/api/history/yoy`, and the monthly FTP archive
  when the hot tier has no data for the month) stream the file and use a month only if
  it reads completely; otherwise they fall back as if it were missing.
- **Diagnostics**: `/api/system-status` has a `warmTier` block (last rollup, next day,
  writes, unchanged batches, I/O and memory errors, quarantined files, late-data marks,
  tick time). Failures are logged to the server serial log (source `history`).

Downgrading to v2.2.15 or earlier brings back H-23 (month files of 8 KB or more are
reset to one day at the next rollup).

### Optional: FTP Server Backup
When FTP is enabled, historical data can be backed up to the FTP server:
- **Path**: `{ftpPath}/history/`
- **Files**: Same structure as local storage
- **Benefits**: Off-device storage, survives device replacement

### Why FTP is Optional (Not Mandatory)
1. **Simplicity**: Many deployments don't have FTP infrastructure
2. **Cost**: FTP server adds infrastructure cost
3. **Reliability**: Local storage works without network dependency
4. **Recovery**: Device can restore from FTP backup if configured

## Data Processing Schedule

### Real-time Processing
- Telemetry logged on receipt (every sample interval)
- Alarms logged immediately when triggered/cleared

### Daily Processing (at daily report time)
- Calculate 24-hour statistics (min, max, avg, change)
- Aggregate alarm counts per tank
- Prune old data (keep 90 days by default)

### On-demand Processing
- When historical page is requested, data is assembled from logs
- Charts use client-side rendering (Chart.js)

## Alternative Storage Options (No FTP)

### Option 1: Blues Notehub Routes (Recommended)
Configure Notehub routes to push telemetry to:
- AWS S3/CloudWatch
- Google Cloud Storage
- Azure Blob Storage
- Custom webhook endpoint

### Option 2: SD Card Storage
For Arduino Opta with SD card shield:
- Store extended historical data on SD card
- Local backup with physical media

### Option 3: Serial Export
- Download historical data via Serial Monitor CSV export
- Manual archival to external systems

## Chart Types

### 1. Tank Level Trends (Line Chart)
- Shows level over time for selected tank(s)
- Configurable time range: 24h, 7d, 30d, 90d
- Multiple tanks can be overlaid

### 2. Alarm Frequency (Bar Chart)
- Number of alarms by tank per time period
- Grouped by alarm type (High, Low, Critical)
- Helps identify problematic tanks

### 3. Daily Consumption (Area Chart)
- Net change in level per day
- Useful for usage tracking and forecasting

### 4. Fleet Overview (Gauge Charts)
- Current level as percentage of capacity
- Color-coded by status

### 5. VIN Voltage Trends (Line Chart)
- Battery/power supply voltage over time
- Early warning for power issues

## UI Organization

### Site Cards
Sites are displayed as collapsible cards containing:
- Site name and total tanks
- Individual tank cards within

### Tank Cards
Each tank shows:
- Current level and trend indicator
- Mini sparkline of last 24 hours
- Alarm count badge
- Quick link to detailed chart

## API Endpoints

### GET /api/history
Query parameters:
- `site`: Filter by site name
- `client`: Filter by client UID
- `sensor`: Filter by sensor index
- `range`: Time range (24h, 7d, 30d, 90d)
- `type`: Data type (levels, alarms, voltage)

Response includes:
- `tanks[]`: Array of tank history with readings, change24h, currentLevel
- `alarms[]`: Array of alarm events with cleared status
- `voltage[]`: Array of voltage readings over time
- `settings`: History retention settings and FTP sync status

### GET /api/history/compare
Month-over-month comparison for trending analysis.

Query parameters:
- `current`: Current period in YYYYMM format (e.g., 202501)
- `previous`: Previous period in YYYYMM format (e.g., 202412)

Response:
```json
{
  "current": { "year": 2025, "month": 1 },
  "previous": { "year": 2024, "month": 12 },
  "tanks": [
    {
      "client": "dev:client001",
      "site": "North Facility",
      "sensorIndex": 1,
      "currentStats": { "min": 25.5, "max": 95.2, "avg": 62.3, "readings": 744 },
      "previousStats": { "available": false, "archivePath": "/history/2024/12/tanks.json" }
    }
  ],
  "archiveInfo": { "ftpEnabled": true, "lastSync": 1704067200 }
}
```

### GET /api/history/yoy
Year-over-year comparison for seasonal analysis.

Query parameters:
- `sensor`: (optional) Specific sensor in format "CLIENT_UID:SENSOR_INDEX"
- `years`: (optional) Number of years to compare (default: 3, max: 5)

Response:
```json
{
  "currentYear": 2025,
  "currentMonth": 1,
  "yearsCompared": 3,
  "tanks": [
    {
      "client": "dev:client001",
      "site": "North Facility",
      "sensorIndex": 1,
      "currentYear": { "min": 25.5, "max": 95.2, "avg": 62.3, "readings": 168 },
      "previousYears": [
        { "year": 2024, "available": false, "archivePath": "/history/2024/annual_summary.json" },
        { "year": 2023, "available": false, "archivePath": "/history/2023/annual_summary.json" }
      ]
    }
  ],
  "archiveInfo": { "ftpEnabled": true, "note": "Previous year data requires FTP archive retrieval" }
}
```

### GET /api/history/summary
Returns aggregated statistics for all tanks:
- Current day metrics
- Week-over-week comparison
- Alarm counts by type

## Memory Considerations

### Arduino Opta Constraints
- Limited RAM (~500KB available)
- Data aggregation done on-device for small datasets
- Large queries return paginated results

### Optimization Strategies
1. Store only aggregated data (daily min/max/avg)
2. Use fixed-point integers instead of floats where possible
3. Compress old data (keep hourly for 7 days, daily for 90 days)

## Implementation Phases

### Phase 1: Basic Visualization
- Line chart for tank levels
- Bar chart for alarm counts
- Data from in-memory telemetry

### Phase 2: Persistent Storage
- Log telemetry to LittleFS
- Daily aggregation job
- FTP backup support

### Phase 3: Advanced Analytics
- Trend prediction
- Anomaly detection
- Custom date range queries

## Security

- Historical data API requires valid PIN
- FTP backup uses configured credentials
- No sensitive data exposed in chart responses

## Browser Compatibility

- Uses Chart.js 4.x for cross-browser support
- Works in modern browsers (Chrome, Firefox, Edge, Safari)
- Responsive design for mobile viewing
