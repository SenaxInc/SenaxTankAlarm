// Reference behaviour of the v2.2.15 server (TankAlarm-112025-Server-BluesOpta.ino
// at 4d2a12a), ported as literally as possible so the new warm-tier code can
// be compared against it. Include after WarmTierStore.h.
#ifndef WARM_LEGACY_V2215_H
#define WARM_LEGACY_V2215_H

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include <ArduinoJson.h>

// Hot-tier ring as the sketch keeps it (SensorHourlyHistory, minus metadata).
struct LegacyRing {
  std::string clientUid;
  uint8_t sensorIndex;
  std::vector<TelemetrySnapshot> snapshots;  // size = capacity
  uint16_t snapshotCount;
  uint16_t writeIndex;
  uint16_t legacyCount;  // S-D03 (not in v2.2.15): oldest entries an older firmware saved

  LegacyRing(const std::string &uid, uint8_t k, uint16_t capacity)
      : clientUid(uid), sensorIndex(k), snapshots(capacity), snapshotCount(0), writeIndex(0), legacyCount(0) {}

  uint16_t cap() const { return (uint16_t)snapshots.size(); }

  // recordTelemetrySnapshot()'s ring append (S:7663-7671), without the dedup
  void add(double ts, float level, float voltage) {
    TelemetrySnapshot &snap = snapshots[writeIndex];
    snap.timestamp = ts;
    snap.level = level;
    snap.voltage = voltage;
    writeIndex = (uint16_t)((writeIndex + 1) % cap());
    if (snapshotCount < cap()) snapshotCount++;
  }

  WarmSeries series() const {
    WarmSeries s = {clientUid.c_str(), sensorIndex, snapshots.data(), cap(), snapshotCount, writeIndex, legacyCount};
    return s;
  }
};

struct LegacyAlarm {
  std::string clientUid;
  uint8_t sensorIndex;
  double timestamp;
};

// Alarm count per day, S:8285-8292 (the ring order does not matter for a count)
inline uint8_t legacyAlarmCount(const std::vector<LegacyAlarm> &alarms, const char *uid, uint8_t k,
                                double dayBegin, double dayEnd) {
  uint8_t alarms_ = 0;
  for (const LegacyAlarm &a : alarms) {
    if (strcmp(a.clientUid.c_str(), uid) == 0 && a.sensorIndex == k &&
        a.timestamp >= dayBegin && a.timestamp < dayEnd) {
      alarms_++;
    }
  }
  return alarms_;
}

// rollupDailySummaries() body S:8239-8317 for one day, starting from an empty
// month document (no file yet). Returns the serialized month file, or "" when
// no sensor had data that day (v2.2.15 then wrote nothing).
inline std::string legacyRollupDay(const std::vector<LegacyRing> &fleet, const std::vector<LegacyAlarm> &alarmLog,
                                   uint32_t yesterdayDate, double dayBegin, WarmRoundFn roundTo) {
  const double dayEnd = dayBegin + 86400.0;
  JsonDocument monthDoc;
  if (!monthDoc.is<JsonArray>()) {
    monthDoc.to<JsonArray>();
  }
  JsonArray entries = monthDoc.as<JsonArray>();

  uint8_t addedCount = 0;
  for (size_t i = 0; i < fleet.size(); i++) {
    const LegacyRing &hist = fleet[i];
    if (hist.snapshotCount == 0) continue;

    float minLevel = 999999.0f, maxLevel = -999999.0f;
    float sumLevel = 0.0f, sumVoltage = 0.0f;
    float openingLevel = 0.0f, closingLevel = 0.0f;
    double oldestTs = 1e18, newestTs = 0.0;
    uint16_t count = 0, voltCount = 0;
    uint8_t alarms = 0;

    for (uint16_t j = 0; j < hist.snapshotCount; j++) {
      uint16_t idx = (uint16_t)((hist.writeIndex - hist.snapshotCount + j + hist.cap()) % hist.cap());
      const TelemetrySnapshot &snap = hist.snapshots[idx];

      if (snap.timestamp < dayBegin || snap.timestamp >= dayEnd) continue;

      if (snap.level < minLevel) minLevel = snap.level;
      if (snap.level > maxLevel) maxLevel = snap.level;
      sumLevel += snap.level;
      count++;

      if (snap.timestamp < oldestTs) { oldestTs = snap.timestamp; openingLevel = snap.level; }
      if (snap.timestamp > newestTs) { newestTs = snap.timestamp; closingLevel = snap.level; }

      if (snap.voltage > 0.0f) { sumVoltage += snap.voltage; voltCount++; }
    }

    if (count == 0) continue;

    alarms = legacyAlarmCount(alarmLog, hist.clientUid.c_str(), hist.sensorIndex, dayBegin, dayEnd);

    JsonObject entry = entries.add<JsonObject>();
    entry["d"] = yesterdayDate;
    entry["c"] = hist.clientUid.c_str();
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

  if (addedCount == 0) return std::string();
  std::string output;
  serializeJson(monthDoc, output);
  return output;
}

// populateStatsFromDailySummary() S:8654-8670 over a whole parsed month
inline WarmSensorStats legacySensorStats(JsonDocument &monthDoc, const char *clientUid, uint8_t sensorIndex) {
  WarmSensorStats out;
  warmSensorStatsInit(out, clientUid, sensorIndex);
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
  out.minLevel = minLevel;
  out.maxLevel = maxLevel;
  out.sumAvg = sumAvg;
  out.days = dayCount;
  return out;
}

// handleHistoryYearOverYear() S:17710-17723 for one month, starting from the
// initial year totals
struct LegacyYoy {
  float yMin, yMax, ySum;
  int yDays;
  bool foundAnyMonth;
};

inline LegacyYoy legacyYoyMonth(JsonDocument &mDoc, const char *clientUid, uint8_t sensorIndex) {
  LegacyYoy y = {9999.0f, -9999.0f, 0.0f, 0, false};
  JsonArray entries = mDoc.as<JsonArray>();
  for (JsonObject entry : entries) {
    const char *uid = entry["c"] | "";
    uint8_t tk = entry["k"] | 0;
    if (strcmp(uid, clientUid) != 0 || tk != sensorIndex) continue;
    float mn = entry["mn"] | 0.0f;
    float mx = entry["mx"] | 0.0f;
    float av = entry["av"] | 0.0f;
    if (mn < y.yMin) y.yMin = mn;
    if (mx > y.yMax) y.yMax = mx;
    y.ySum += av;
    y.yDays++;
    y.foundAnyMonth = true;
  }
  return y;
}

// archiveMonthToFtp() warm fallback S:7834-7867 over a whole parsed month
inline std::vector<WarmMonthSensor> legacyMonthSummary(JsonDocument &warmDoc, uint8_t maxSensors) {
  std::vector<WarmMonthSensor> warmed(maxSensors);
  uint8_t warmCount = 0;
  for (JsonObject de : warmDoc.as<JsonArray>()) {
    const char *wUid = de["c"] | "";
    uint8_t wIdx = de["k"] | 0;
    float dMin = de["mn"] | 999999.0f;
    float dMax = de["mx"] | -999999.0f;
    float dAvg = de["av"] | 0.0f;
    float dVt  = de["vt"] | 0.0f;
    uint16_t dN = de["n"] | (uint16_t)1;

    WarmMonthSensor *ws = nullptr;
    for (uint8_t w = 0; w < warmCount; ++w) {
      if (strcmp(warmed[w].clientUid, wUid) == 0 && warmed[w].sensorIndex == wIdx) { ws = &warmed[w]; break; }
    }
    if (!ws && warmCount < maxSensors) {
      ws = &warmed[warmCount++];
      warmCopyStr(ws->clientUid, sizeof(ws->clientUid), wUid);
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
  warmed.resize(warmCount);
  return warmed;
}

#endif  // WARM_LEGACY_V2215_H
