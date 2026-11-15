// NwsTemperature.h - outdoor temperature for the History page's VIN Voltage History chart (server)
//
// For each client whose location is known (the Notecard's cell-tower fix, from the
// location_request/location_response round trip), the server asks api.weather.gov:
//   1. GET /points/{lat},{lon}            -> properties.observationStations (a URL)
//   2. GET <that path>?limit=2            -> the two nearest observation stations
//   3. GET /stations/{id}/observations?start=<t - 60 min>&end=<t + 60 min>
//                                         -> the observation nearest reading time t
// and keeps one sample per reading: the observation's own time and temperature.
//
// History rules (S-D03): a sample comes only from an observation taken within
// NWS_OBS_MATCH_SEC of a real reading, and is stored at the observation's time.
// Nothing is interpolated, carried forward or estimated; a reading NWS has no
// observation for stays without a temperature. NWS keeps about seven days of
// observations, so a reading can be matched only while it is younger than
// NWS_OBS_MAX_AGE_SEC; readings from before that never get a temperature.
//
// The HTTP reader decodes Content-Length, chunked and close-delimited bodies from
// any byte source, so tests/host/nws_temp drives it with canned responses. This
// header has no Arduino or mbed dependency.

#ifndef TANKALARM_NWS_TEMPERATURE_H
#define TANKALARM_NWS_TEMPERATURE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ArduinoJson.h>

// ============================================================================
// Limits
// ============================================================================
#define NWS_HOST "api.weather.gov"
#define NWS_PORT 443

#ifndef NWS_TEMP_MAX_CLIENTS
#define NWS_TEMP_MAX_CLIENTS 20
#endif
#ifndef NWS_TEMP_SAMPLES
#define NWS_TEMP_SAMPLES 90            // per client; the hot tier keeps 90 readings per sensor
#endif
#define NWS_UID_MAX 48                 // as SensorRecord::clientUid
#define NWS_STATION_ID_MAX 12          // ICAO ids are 4 characters; some networks use up to 11
#define NWS_STATIONS 2                 // nearest first; the second is asked when the first has no report
#define NWS_PATH_MAX 128
#ifndef NWS_HTTP_BUF
#define NWS_HTTP_BUF 256               // bytes read from the socket at a time
#endif
#ifndef NWS_HTTP_LINE_MAX
#define NWS_HTTP_LINE_MAX 192          // longer header lines are cut (only status, length and encodings are read)
#endif
#ifndef NWS_HTTP_BODY_MAX
#define NWS_HTTP_BODY_MAX 262144UL     // a body longer than this is abandoned
#endif

// ============================================================================
// Timing (seconds, UTC epochs)
// ============================================================================
#ifndef NWS_OBS_MATCH_SEC
// An observation this close to a reading matches it, and a reading this close to a stored
// sample needs no lookup of its own. Also the query half-window. One value for all three,
// so a reading that has been matched is always covered and is never asked about again.
#define NWS_OBS_MATCH_SEC 3600.0
#endif
#ifndef NWS_OBS_SETTLE_SEC
#define NWS_OBS_SETTLE_SEC 1200.0      // wait this long after a reading: observations are published late
#endif
#ifndef NWS_OBS_EMPTY_RETRY_SEC
#define NWS_OBS_EMPTY_RETRY_SEC 10800.0  // a reading younger than this with no observation yet is asked again
#endif
#ifndef NWS_OBS_EMPTY_WAIT_SEC
#define NWS_OBS_EMPTY_WAIT_SEC 1800.0  // ... after this long
#endif
#ifndef NWS_OBS_MAX_AGE_SEC
#define NWS_OBS_MAX_AGE_SEC 561600.0   // 6.5 days: NWS keeps about seven days of observations
#endif
#ifndef NWS_STATION_REFRESH_SEC
#define NWS_STATION_REFRESH_SEC 2592000.0  // look the stations up again after 30 days
#endif
#ifndef NWS_LOCATION_REQUEST_SEC
#define NWS_LOCATION_REQUEST_SEC 86400.0   // ask a client without a location at most once a day
#endif
#ifndef NWS_RETRY_BASE_SEC
#define NWS_RETRY_BASE_SEC 600.0       // first retry after a failed request; doubles per failure
#endif
#ifndef NWS_RETRY_MAX_SEC
#define NWS_RETRY_MAX_SEC 21600.0
#endif
#ifndef NWS_UNCOVERED_RETRY_SEC
#define NWS_UNCOVERED_RETRY_SEC 604800.0   // NWS does not cover the location (outside the US): ask weekly
#endif
#define NWS_MIN_VALID_EPOCH 1704067200.0   // 2024-01-01Z: below this the clock is not set
#define NWS_MAX_VALID_EPOCH 4102444800.0   // 2100-01-01Z
#define NWS_LOCATION_SAME_DEG 0.005f       // stations found for a location this close still apply
#define NWS_TEMP_MIN_F -80.0
#define NWS_TEMP_MAX_F 140.0

// ============================================================================
// Dates (UTC, no mktime/TZ dependence)
// ============================================================================

static inline int32_t nwsDaysFromCivil(int y, int m, int d) {
  // Howard Hinnant's days_from_civil
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int mp = (m > 2) ? m - 3 : m + 9;
  const int doy = (153 * mp + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int32_t)(era * 146097 + doe - 719468);
}

static inline void nwsCivilFromDays(int32_t dn, int *y, int *m, int *d) {
  const int32_t z = dn + 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int32_t doe = z - era * 146097;
  const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int32_t mp = (5 * doy + 2) / 153;
  const int32_t dd = doy - (153 * mp + 2) / 5 + 1;
  const int32_t mm = (mp < 10) ? mp + 3 : mp - 9;
  *y = (int)(yoe + era * 400 + (mm <= 2 ? 1 : 0));
  *m = (int)mm;
  *d = (int)dd;
}

static inline bool nwsDigits(const char *s, int n, int *out) {
  int v = 0;
  for (int i = 0; i < n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return true;
}

// "2026-11-13T18:52:00+00:00", "...Z", "...T18:52:00.25Z" or "...-05:00" -> UTC epoch seconds.
// Rejects anything else, including impossible dates (Feb 30) and years outside 2000-2099.
static inline bool nwsParseIso8601(const char *s, double *epoch) {
  if (!s || !epoch) return false;
  int y, mo, d, h, mi, se;
  if (!nwsDigits(s, 4, &y) || s[4] != '-' || !nwsDigits(s + 5, 2, &mo) || s[7] != '-' ||
      !nwsDigits(s + 8, 2, &d) || (s[10] != 'T' && s[10] != 't') || !nwsDigits(s + 11, 2, &h) ||
      s[13] != ':' || !nwsDigits(s + 14, 2, &mi) || s[16] != ':' || !nwsDigits(s + 17, 2, &se)) {
    return false;
  }
  if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
  const char *p = s + 19;
  if (*p == '.') {
    ++p;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
  }
  long offset = 0;
  if (*p == 'Z' || *p == 'z') {
    ++p;
  } else if (*p == '+' || *p == '-') {
    const long sign = (*p == '-') ? -1 : 1;
    int oh, om;
    ++p;
    if (!nwsDigits(p, 2, &oh)) return false;
    p += 2;
    if (*p == ':') ++p;
    if (!nwsDigits(p, 2, &om)) return false;
    p += 2;
    if (oh > 23 || om > 59) return false;
    offset = sign * (oh * 3600L + om * 60L);
  } else {
    return false;
  }
  if (*p != '\0') return false;
  const int32_t dn = nwsDaysFromCivil(y, mo, d);
  int cy, cm, cd;
  nwsCivilFromDays(dn, &cy, &cm, &cd);
  if (cy != y || cm != mo || cd != d) return false;
  if (se == 60) se = 59;  // a leap second stays in its minute
  *epoch = (double)dn * 86400.0 + h * 3600.0 + mi * 60.0 + se - (double)offset;
  return true;
}

// UTC epoch -> "YYYY-MM-DDTHH:MM:SSZ" (20 characters). Needs cap >= 21.
static inline bool nwsFormatIso8601(double epoch, char *out, size_t cap) {
  if (!out || cap < 21 || !(epoch >= 0.0 && epoch < NWS_MAX_VALID_EPOCH)) return false;
  const double whole = floor(epoch);
  const int32_t dn = (int32_t)floor(whole / 86400.0);
  const int32_t sod = (int32_t)(whole - (double)dn * 86400.0);
  int y, m, d;
  nwsCivilFromDays(dn, &y, &m, &d);
  const int n = snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, (int)(sod / 3600),
                         (int)((sod / 60) % 60), (int)(sod % 60));
  return n == 20;
}

// ============================================================================
// Values
// ============================================================================

// NWS quantities carry a WMO unit code ("wmoUnit:degC"). Returns false for an
// unknown unit, a non-finite value or a temperature outside NWS_TEMP_MIN_F..MAX_F.
static inline bool nwsTemperatureF(double value, const char *unitCode, float *outF) {
  if (!unitCode || !outF || !isfinite(value)) return false;
  const char *u = strrchr(unitCode, ':');
  u = u ? u + 1 : unitCode;
  double f;
  if (strcmp(u, "degC") == 0) {
    f = value * 9.0 / 5.0 + 32.0;
  } else if (strcmp(u, "degF") == 0) {
    f = value;
  } else if (strcmp(u, "K") == 0) {
    f = (value - 273.15) * 9.0 / 5.0 + 32.0;
  } else {
    return false;
  }
  if (!(f >= NWS_TEMP_MIN_F && f <= NWS_TEMP_MAX_F)) return false;
  *outF = (float)f;
  return true;
}

// NWS quality-control flags: X = rejected, B = judged bad. Every other flag
// (Z, C, S, V, Q, G, T) and a missing flag are accepted.
static inline bool nwsQualityUsable(const char *qc) {
  if (!qc || qc[0] == '\0' || qc[1] != '\0') return true;
  return qc[0] != 'X' && qc[0] != 'B';
}

static inline int16_t nwsTenths(float tempF) {
  return (int16_t)lround((double)tempF * 10.0);
}

static inline bool nwsValidStationId(const char *s) {
  if (!s) return false;
  size_t n = 0;
  for (; s[n] != '\0'; ++n) {
    const char c = s[n];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    if (n + 1 >= NWS_STATION_ID_MAX) return false;
  }
  return n >= 3;
}

// Client UIDs are written into the saved file without escaping, so only these characters are kept
static inline bool nwsValidUid(const char *s) {
  if (!s || s[0] == '\0') return false;
  for (size_t n = 0; s[n] != '\0'; ++n) {
    const char c = s[n];
    if (n + 1 >= NWS_UID_MAX) return false;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' ||
          c == '-' || c == '_' || c == '.')) {
      return false;
    }
  }
  return true;
}

// ============================================================================
// Request paths
// ============================================================================

// NWS answers coordinates with more than 4 decimals with a 301, so exactly 4 are sent
static inline bool nwsPointsPath(char *out, size_t cap, float lat, float lon) {
  if (!out || !(lat >= -90.0f && lat <= 90.0f && lon >= -180.0f && lon <= 180.0f)) return false;
  if (lat == 0.0f && lon == 0.0f) return false;
  const int n = snprintf(out, cap, "/points/%.4f,%.4f", (double)lat, (double)lon);
  return n > 0 && (size_t)n < cap;
}

// "https://api.weather.gov/gridpoints/LWX/97,71/stations" -> "/gridpoints/LWX/97,71/stations?limit=2".
// Only a path on NWS_HOST made of [A-Za-z0-9/,._-] (no "..") is accepted.
static inline bool nwsStationsPathFromUrl(const char *url, char *out, size_t cap) {
  static const char kOrigin[] = "https://" NWS_HOST;
  if (!url || !out || strncmp(url, kOrigin, sizeof(kOrigin) - 1) != 0) return false;
  const char *path = url + sizeof(kOrigin) - 1;
  if (path[0] != '/' || strstr(path, "..") != nullptr) return false;  // also rejects "api.weather.gov.example"
  for (const char *p = path; *p; ++p) {
    const char c = *p;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
          c == ',' || c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  const int n = snprintf(out, cap, "%s?limit=%d", path, NWS_STATIONS);
  return n > 0 && (size_t)n < cap;
}

static inline bool nwsObservationsPath(char *out, size_t cap, const char *station, double start, double end) {
  char s[24], e[24];
  if (!out || !nwsValidStationId(station) || !(end >= start)) return false;
  if (!nwsFormatIso8601(start, s, sizeof(s)) || !nwsFormatIso8601(end, e, sizeof(e))) return false;
  const int n = snprintf(out, cap, "/stations/%s/observations?start=%s&end=%s", station, s, e);
  return n > 0 && (size_t)n < cap;
}

// Returns the request length, or 0 when it does not fit. NWS refuses requests
// without a User-Agent (403). identity: the reader cannot decompress.
static inline size_t nwsBuildGet(char *out, size_t cap, const char *path, const char *userAgent) {
  if (!out || !path || path[0] != '/' || !userAgent || userAgent[0] == '\0') return 0;
  const int n = snprintf(out, cap,
                         "GET %s HTTP/1.1\r\nHost: " NWS_HOST "\r\nUser-Agent: %s\r\n"
                         "Accept: application/geo+json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                         path, userAgent);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// ============================================================================
// HTTP/1.1 response reader
// ============================================================================

// The byte source: returns bytes read (> 0), 0 when the peer closed, < 0 on an error or timeout
typedef int (*NwsReadFn)(void *ctx, uint8_t *buf, size_t len);

enum NwsChunkState : uint8_t { NWS_CHUNK_SIZE = 0, NWS_CHUNK_DATA, NWS_CHUNK_CRLF, NWS_CHUNK_DONE };

static inline bool nwsStartsWithNoCase(const char *s, const char *prefix) {
  for (; *prefix; ++s, ++prefix) {
    char a = *s, b = *prefix;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

static inline bool nwsContainsNoCase(const char *s, const char *word) {
  const size_t n = strlen(word);
  for (; *s; ++s) {
    if (nwsStartsWithNoCase(s, word)) return true;
  }
  return n == 0;
}

// Reads the status line and headers, then serves the body to ArduinoJson
// (read()/readBytes() is its custom-reader contract). The body ends at its
// Content-Length, at the last chunk, or when the peer closes; bodyError is set
// for a framing error, a body cut short, an encoding it cannot decode, or more
// than bodyMax bytes, and the body then ends early so the JSON parse fails.
struct NwsHttpReader {
  NwsReadFn fn;
  void *ctx;
  uint8_t buf[NWS_HTTP_BUF];
  uint16_t pos, len;
  bool eof, ioError;
  int status;
  bool chunked, hasLength, bodyError, bodyDone;
  uint8_t chunkState;
  uint32_t length, chunkLeft, bodyRead, bodyMax;

  NwsHttpReader() { begin(nullptr, nullptr, 0); }

  void begin(NwsReadFn f, void *c, uint32_t maxBody) {
    fn = f;
    ctx = c;
    pos = len = 0;
    eof = ioError = false;
    status = 0;
    chunked = hasLength = bodyError = bodyDone = false;
    chunkState = NWS_CHUNK_SIZE;
    length = chunkLeft = bodyRead = 0;
    bodyMax = maxBody;
  }

  int rawByte() {
    if (pos >= len) {
      if (eof || ioError || !fn) return -1;
      const int got = fn(ctx, buf, sizeof(buf));
      if (got == 0) { eof = true; return -1; }
      if (got < 0 || got > (int)sizeof(buf)) { ioError = true; return -1; }
      pos = 0;
      len = (uint16_t)got;
    }
    return buf[pos++];
  }

  // One line without its CR/LF. A line longer than cap-1 is cut, and the rest
  // is skipped. False when the input ends before the '\n'.
  bool readLine(char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
      const int c = rawByte();
      if (c < 0) {
        if (cap > 0) line[n < cap ? n : cap - 1] = '\0';
        return false;
      }
      if (c == '\n') break;
      if (c == '\r') continue;
      if (n + 1 < cap) line[n++] = (char)c;
    }
    if (cap > 0) line[n] = '\0';
    return true;
  }

  // Status line and headers; skips 1xx interim responses. False on a malformed or cut-off head.
  bool readHead() {
    char line[NWS_HTTP_LINE_MAX];
    for (;;) {
      if (!readLine(line, sizeof(line))) return false;
      int code = 0;
      if (strncmp(line, "HTTP/1.", 7) != 0 || (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
          !nwsDigits(line + 9, 3, &code) || (line[12] != ' ' && line[12] != '\0')) {
        return false;
      }
      status = code;
      chunked = hasLength = false;
      length = 0;
      bool badEncoding = false;
      for (;;) {
        if (!readLine(line, sizeof(line))) return false;
        if (line[0] == '\0') break;
        if (nwsStartsWithNoCase(line, "content-length:")) {
          const char *v = line + 15;
          while (*v == ' ' || *v == '\t') ++v;
          uint32_t value = 0;
          bool any = false;
          for (; *v >= '0' && *v <= '9'; ++v) {
            const uint32_t digit = (uint32_t)(*v - '0');
            if (value > (UINT32_MAX - digit) / 10u) { badEncoding = true; break; }
            value = value * 10u + digit;
            any = true;
          }
          if (!any) badEncoding = true;
          length = value;
          hasLength = true;
        } else if (nwsStartsWithNoCase(line, "transfer-encoding:")) {
          const char *v = line + 18;
          chunked = nwsContainsNoCase(v, "chunked");
          if (nwsContainsNoCase(v, "gzip") || nwsContainsNoCase(v, "deflate") || nwsContainsNoCase(v, "compress")) {
            badEncoding = true;
          }
        } else if (nwsStartsWithNoCase(line, "content-encoding:")) {
          const char *v = line + 17;
          while (*v == ' ' || *v == '\t') ++v;
          if (*v != '\0' && !nwsStartsWithNoCase(v, "identity")) badEncoding = true;
        }
      }
      if (status >= 100 && status < 200) continue;
      if (chunked) hasLength = false;  // RFC 7230 3.3.3: chunked wins over Content-Length
      bodyError = badEncoding;
      bodyDone = badEncoding || (hasLength && length == 0);
      return true;
    }
  }

  bool readChunkSize() {
    char line[40];
    if (!readLine(line, sizeof(line))) return false;
    uint32_t size = 0;
    bool any = false;
    for (const char *p = line; *p && *p != ';' && *p != ' ' && *p != '\t'; ++p) {
      int v;
      if (*p >= '0' && *p <= '9') v = *p - '0';
      else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
      else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
      else return false;
      if (size > 0x00FFFFFFu) return false;
      size = (size << 4) | (uint32_t)v;
      any = true;
    }
    if (!any) return false;
    chunkLeft = size;
    return true;
  }

  int read() {
    if (bodyDone || bodyError) return -1;
    if (bodyRead >= bodyMax) {
      bodyError = true;
      return -1;
    }
    if (chunked) {
      for (;;) {
        if (chunkState == NWS_CHUNK_SIZE) {
          if (!readChunkSize()) { bodyError = true; return -1; }
          if (chunkLeft == 0) {
            chunkState = NWS_CHUNK_DONE;  // trailers are not read: the socket is closed next
            bodyDone = true;
            return -1;
          }
          chunkState = NWS_CHUNK_DATA;
        }
        if (chunkState == NWS_CHUNK_DATA) {
          if (chunkLeft == 0) {
            chunkState = NWS_CHUNK_CRLF;
            continue;
          }
          const int c = rawByte();
          if (c < 0) { bodyError = true; return -1; }
          chunkLeft--;
          bodyRead++;
          return c;
        }
        if (chunkState == NWS_CHUNK_CRLF) {
          char line[8];
          if (!readLine(line, sizeof(line)) || line[0] != '\0') { bodyError = true; return -1; }
          chunkState = NWS_CHUNK_SIZE;
          continue;
        }
        bodyDone = true;
        return -1;
      }
    }
    if (hasLength) {
      if (bodyRead >= length) { bodyDone = true; return -1; }
      const int c = rawByte();
      if (c < 0) { bodyError = true; return -1; }
      bodyRead++;
      return c;
    }
    const int c = rawByte();  // close-delimited
    if (c < 0) {
      if (ioError) bodyError = true;
      else bodyDone = true;
      return -1;
    }
    bodyRead++;
    return c;
  }

  size_t readBytes(char *dst, size_t n) {
    size_t i = 0;
    for (; i < n; ++i) {
      const int c = read();
      if (c < 0) break;
      dst[i] = (char)c;
    }
    return i;
  }
};

// ============================================================================
// NWS responses (parsed with these filters so only the needed fields are kept)
// ============================================================================

static inline void nwsPointsFilter(JsonDocument &f) {
  f.clear();
  f["properties"]["observationStations"] = true;
}

static inline bool nwsParsePoints(JsonVariantConst root, char *stationsPath, size_t cap) {
  const char *url = root["properties"]["observationStations"].as<const char *>();
  return nwsStationsPathFromUrl(url, stationsPath, cap);
}

static inline void nwsStationsFilter(JsonDocument &f) {
  f.clear();
  f["features"][0]["properties"]["stationIdentifier"] = true;
}

// Copies up to max valid station ids, in the order NWS lists them (nearest first)
static inline uint8_t nwsParseStations(JsonVariantConst root, char out[][NWS_STATION_ID_MAX], uint8_t max) {
  uint8_t n = 0;
  JsonArrayConst features = root["features"].as<JsonArrayConst>();
  for (JsonVariantConst f : features) {
    if (n >= max) break;
    const char *id = f["properties"]["stationIdentifier"].as<const char *>();
    if (!nwsValidStationId(id)) continue;
    bool dup = false;
    for (uint8_t i = 0; i < n; ++i) dup = dup || strcmp(out[i], id) == 0;
    if (dup) continue;
    snprintf(out[n], NWS_STATION_ID_MAX, "%s", id);
    n++;
  }
  return n;
}

static inline void nwsObservationsFilter(JsonDocument &f) {
  f.clear();
  JsonObject p = f["features"][0]["properties"].to<JsonObject>();
  p["timestamp"] = true;
  JsonObject t = p["temperature"].to<JsonObject>();
  t["value"] = true;
  t["unitCode"] = true;
  t["qualityControl"] = true;
}

// The usable observation nearest target, at most maxDiff seconds away (the earlier one on a tie).
static inline bool nwsNearestObservation(JsonVariantConst root, double target, double maxDiff, float *tempF,
                                         double *obsEpoch) {
  bool found = false;
  double bestDiff = 0.0, bestTs = 0.0;
  float bestF = 0.0f;
  JsonArrayConst features = root["features"].as<JsonArrayConst>();
  for (JsonVariantConst f : features) {
    JsonVariantConst p = f["properties"];
    double ts;
    if (!nwsParseIso8601(p["timestamp"].as<const char *>(), &ts)) continue;
    const double diff = fabs(ts - target);
    if (!(diff <= maxDiff)) continue;
    JsonVariantConst t = p["temperature"];
    if (!t["value"].is<double>() || !nwsQualityUsable(t["qualityControl"].as<const char *>())) continue;
    float value;
    if (!nwsTemperatureF(t["value"].as<double>(), t["unitCode"].as<const char *>(), &value)) continue;
    if (!found || diff < bestDiff || (diff == bestDiff && ts < bestTs)) {
      found = true;
      bestDiff = diff;
      bestTs = ts;
      bestF = value;
    }
  }
  if (found) {
    if (tempF) *tempF = bestF;
    if (obsEpoch) *obsEpoch = bestTs;
  }
  return found;
}

// ============================================================================
// Per-client store
// ============================================================================

struct NwsClientTemp {
  char uid[NWS_UID_MAX];
  char stations[NWS_STATIONS][NWS_STATION_ID_MAX];  // nearest first; "" = not looked up
  float stationsLat, stationsLon;                   // the location they were found for
  uint32_t stationsEpoch;                           // when they were found
  uint16_t count;                                   // samples, oldest first
  uint32_t sampleEpoch[NWS_TEMP_SAMPLES];           // observation time
  int16_t sampleTenthsF[NWS_TEMP_SAMPLES];          // tenths of a degree F
  // Readings at or before this time need no more lookups: NWS had no observation for
  // them, or their sample could not be kept. Readings are handled oldest first.
  uint32_t settledThrough;
  // RAM only: scheduling
  uint8_t failStreak;
  bool uncovered;                      // /points answered 404: NWS does not cover the location
  uint32_t retryAfter;                 // no request for this client before this epoch
  uint32_t lastLocationRequest;
};

struct NwsTempStore {
  NwsClientTemp clients[NWS_TEMP_MAX_CLIENTS];
  uint8_t count;
};

static inline NwsClientTemp *nwsFindClient(NwsTempStore &s, const char *uid) {
  if (!uid) return nullptr;
  for (uint8_t i = 0; i < s.count; ++i) {
    if (strcmp(s.clients[i].uid, uid) == 0) return &s.clients[i];
  }
  return nullptr;
}

static inline const NwsClientTemp *nwsFindClient(const NwsTempStore &s, const char *uid) {
  return nwsFindClient(const_cast<NwsTempStore &>(s), uid);
}

static inline NwsClientTemp *nwsFindOrAddClient(NwsTempStore &s, const char *uid) {
  NwsClientTemp *c = nwsFindClient(s, uid);
  if (c || !nwsValidUid(uid) || s.count >= NWS_TEMP_MAX_CLIENTS) return c;
  c = &s.clients[s.count++];
  memset(c, 0, sizeof(*c));
  snprintf(c->uid, sizeof(c->uid), "%s", uid);
  return c;
}

static inline bool nwsForgetClient(NwsTempStore &s, const char *uid) {
  for (uint8_t i = 0; i < s.count; ++i) {
    if (strcmp(s.clients[i].uid, uid) != 0) continue;
    for (uint8_t j = i; j + 1 < s.count; ++j) memcpy(&s.clients[j], &s.clients[j + 1], sizeof(NwsClientTemp));
    s.count--;
    memset(&s.clients[s.count], 0, sizeof(NwsClientTemp));
    return true;
  }
  return false;
}

static inline uint8_t nwsStationCount(const NwsClientTemp &c) {
  uint8_t n = 0;
  while (n < NWS_STATIONS && c.stations[n][0] != '\0') n++;
  return n;
}

static inline void nwsClearStations(NwsClientTemp &c) {
  memset(c.stations, 0, sizeof(c.stations));
  c.stationsLat = c.stationsLon = 0.0f;
  c.stationsEpoch = 0;
}

static inline void nwsSetStations(NwsClientTemp &c, char ids[][NWS_STATION_ID_MAX], uint8_t n, float lat, float lon,
                                  double now) {
  nwsClearStations(c);
  for (uint8_t i = 0; i < n && i < NWS_STATIONS; ++i) snprintf(c.stations[i], NWS_STATION_ID_MAX, "%s", ids[i]);
  c.stationsLat = lat;
  c.stationsLon = lon;
  c.stationsEpoch = (uint32_t)now;
}

// Stations are known, were found for (about) this location, and are not due for a refresh
static inline bool nwsStationsCurrent(const NwsClientTemp &c, float lat, float lon, double now) {
  if (c.stations[0][0] == '\0' || c.stationsEpoch == 0) return false;
  if (!(fabsf(lat - c.stationsLat) <= NWS_LOCATION_SAME_DEG && fabsf(lon - c.stationsLon) <= NWS_LOCATION_SAME_DEG)) {
    return false;
  }
  const double age = now - (double)c.stationsEpoch;
  return age >= -3600.0 && age < NWS_STATION_REFRESH_SEC;
}

// Inserts in time order; an equal epoch replaces its value. When full, the
// oldest sample makes room, and a sample older than all of them is dropped.
// Returns true when the store changed.
static inline bool nwsAddSample(NwsClientTemp &c, uint32_t epoch, float tempF) {
  if (!((double)epoch >= NWS_MIN_VALID_EPOCH && (double)epoch < NWS_MAX_VALID_EPOCH)) return false;
  if (!((double)tempF >= NWS_TEMP_MIN_F && (double)tempF <= NWS_TEMP_MAX_F)) return false;
  const int16_t tenths = nwsTenths(tempF);
  uint16_t i = c.count;
  while (i > 0 && c.sampleEpoch[i - 1] > epoch) i--;
  if (i > 0 && c.sampleEpoch[i - 1] == epoch) {
    if (c.sampleTenthsF[i - 1] == tenths) return false;
    c.sampleTenthsF[i - 1] = tenths;
    return true;
  }
  if (c.count >= NWS_TEMP_SAMPLES) {
    if (i == 0) return false;  // older than everything kept
    memmove(&c.sampleEpoch[0], &c.sampleEpoch[1], (size_t)(i - 1) * sizeof(c.sampleEpoch[0]));
    memmove(&c.sampleTenthsF[0], &c.sampleTenthsF[1], (size_t)(i - 1) * sizeof(c.sampleTenthsF[0]));
    i--;
  } else {
    memmove(&c.sampleEpoch[i + 1], &c.sampleEpoch[i], (size_t)(c.count - i) * sizeof(c.sampleEpoch[0]));
    memmove(&c.sampleTenthsF[i + 1], &c.sampleTenthsF[i], (size_t)(c.count - i) * sizeof(c.sampleTenthsF[0]));
    c.count++;
  }
  c.sampleEpoch[i] = epoch;
  c.sampleTenthsF[i] = tenths;
  return true;
}

static inline bool nwsSampleNear(const NwsClientTemp &c, double t, double within) {
  for (uint16_t i = 0; i < c.count; ++i) {
    if (fabs((double)c.sampleEpoch[i] - t) <= within) return true;
  }
  return false;
}

static inline void nwsSettle(NwsClientTemp &c, double t) {
  if (t > (double)c.settledThrough && t < NWS_MAX_VALID_EPOCH) c.settledThrough = (uint32_t)t;
}

// A reading at time t should be matched now: observations for it have had time to
// be published, NWS still keeps them, no stored sample covers it, and it is after
// settledThrough.
static inline bool nwsReadingNeedsTemp(const NwsClientTemp &c, double t, double now) {
  if (!(t >= NWS_MIN_VALID_EPOCH && t < NWS_MAX_VALID_EPOCH) || !(now >= NWS_MIN_VALID_EPOCH)) return false;
  if (t <= (double)c.settledThrough) return false;
  const double age = now - t;
  if (!(age >= NWS_OBS_SETTLE_SEC && age <= NWS_OBS_MAX_AGE_SEC)) return false;
  return !nwsSampleNear(c, t, NWS_OBS_MATCH_SEC);
}

// ============================================================================
// Scheduling
// ============================================================================

enum NwsAction : uint8_t {
  NWS_ACT_NONE = 0,
  NWS_ACT_REQUEST_LOCATION,  // ask the client for its location (a Notecard note, not a web request)
  NWS_ACT_FIND_STATIONS,     // /points, then the stations list
  NWS_ACT_FETCH_OBS          // observations around the oldest reading that needs a temperature
};

static inline NwsAction nwsPlan(const NwsClientTemp &c, bool hasLocation, float lat, float lon, bool readingWaiting,
                                double now) {
  if (!readingWaiting || !(now >= NWS_MIN_VALID_EPOCH)) return NWS_ACT_NONE;
  if (now < (double)c.retryAfter) return NWS_ACT_NONE;
  if (!hasLocation) {
    const bool due = c.lastLocationRequest == 0 || now - (double)c.lastLocationRequest >= NWS_LOCATION_REQUEST_SEC;
    return due ? NWS_ACT_REQUEST_LOCATION : NWS_ACT_NONE;
  }
  if (!nwsStationsCurrent(c, lat, lon, now)) return NWS_ACT_FIND_STATIONS;
  return NWS_ACT_FETCH_OBS;
}

static inline double nwsRetryDelay(uint8_t failStreak) {
  double d = NWS_RETRY_BASE_SEC;
  for (uint8_t i = 1; i < failStreak && d < NWS_RETRY_MAX_SEC; ++i) d *= 2.0;
  return d < NWS_RETRY_MAX_SEC ? d : NWS_RETRY_MAX_SEC;
}

static inline void nwsNoteFailure(NwsClientTemp &c, double now) {
  if (c.failStreak < 255) c.failStreak++;
  c.retryAfter = (uint32_t)(now + nwsRetryDelay(c.failStreak));
}

static inline void nwsNoteSuccess(NwsClientTemp &c) {
  c.failStreak = 0;
  c.retryAfter = 0;
}

static inline void nwsNoteUncovered(NwsClientTemp &c, double now) {
  c.uncovered = true;
  nwsClearStations(c);
  c.failStreak = 0;
  c.retryAfter = (uint32_t)(now + NWS_UNCOVERED_RETRY_SEC);
}

// The client reported a new location: find its stations again, at once
static inline void nwsNoteLocationChanged(NwsClientTemp &c) {
  nwsClearStations(c);
  c.uncovered = false;
  c.failStreak = 0;
  c.retryAfter = 0;
}

// The observation at obsEpoch matched reading time t. Returns true when the store
// changed. A sample the store cannot keep (full of newer ones) settles t instead,
// so the reading is not asked about again.
static inline bool nwsNoteMatched(NwsClientTemp &c, double t, uint32_t obsEpoch, float tempF) {
  c.failStreak = 0;
  c.retryAfter = 0;
  const bool changed = nwsAddSample(c, obsEpoch, tempF);
  if (!nwsSampleNear(c, t, NWS_OBS_MATCH_SEC)) nwsSettle(c, t);
  return changed;
}

// The observations came back but none matched reading time t. A young reading
// is asked again later (observations are published late); an older one is left
// without a temperature.
static inline void nwsNoteNoMatch(NwsClientTemp &c, double t, double now) {
  c.failStreak = 0;
  if (now - t < NWS_OBS_EMPTY_RETRY_SEC) {
    c.retryAfter = (uint32_t)(now + NWS_OBS_EMPTY_WAIT_SEC);
  } else {
    c.retryAfter = 0;
    nwsSettle(c, t);
  }
}

// ============================================================================
// Saved file:
//   [{"c":uid,"st":["KDCA","KCGS"],"sl":[lat,lon],"se":epoch,"sb":epoch,"t":[[epoch,tenthsF],...]},...]
// st/sl/se: the stations, the location they were found for and when; sb: settledThrough;
// t: the samples, oldest first.
// ============================================================================

struct NwsOut {
  char *buf;
  size_t cap, len;
  void putf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    char *dst = (buf && len < cap) ? buf + len : nullptr;
    const size_t room = dst ? cap - len : 0;
    const int n = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (n > 0) len += (size_t)n;
  }
};

// Writes the store as JSON into out (NUL-terminated when cap > 0) and returns the
// full length, as snprintf does; call with out = nullptr to measure. Clients with
// nothing to keep (no stations, samples or settled time) are left out.
static inline size_t nwsSerializeStore(const NwsTempStore &s, char *out, size_t cap) {
  NwsOut o = {out, cap, 0};
  if (out && cap > 0) out[0] = '\0';
  o.putf("[");
  bool first = true;
  for (uint8_t i = 0; i < s.count; ++i) {
    const NwsClientTemp &c = s.clients[i];
    const uint8_t ns = nwsStationCount(c);
    if (!nwsValidUid(c.uid) || (ns == 0 && c.count == 0 && c.settledThrough == 0)) continue;
    o.putf("%s{\"c\":\"%s\"", first ? "" : ",", c.uid);
    first = false;
    if (ns > 0) {
      o.putf(",\"st\":[");
      for (uint8_t k = 0; k < ns; ++k) o.putf("%s\"%s\"", k ? "," : "", c.stations[k]);
      o.putf("],\"sl\":[%.4f,%.4f],\"se\":%lu", (double)c.stationsLat, (double)c.stationsLon,
             (unsigned long)c.stationsEpoch);
    }
    if (c.settledThrough != 0) o.putf(",\"sb\":%lu", (unsigned long)c.settledThrough);
    o.putf(",\"t\":[");
    for (uint16_t k = 0; k < c.count; ++k) {
      o.putf("%s[%lu,%d]", k ? "," : "", (unsigned long)c.sampleEpoch[k], (int)c.sampleTenthsF[k]);
    }
    o.putf("]}");
  }
  o.putf("]");
  return o.len;
}

// Replaces the store with the clients in arr. Bad entries and values are
// skipped; samples later than now + 1 h are dropped (now <= 0: no check).
static inline uint8_t nwsLoadStore(NwsTempStore &s, JsonArrayConst arr, double now) {
  memset(&s, 0, sizeof(s));
  for (JsonVariantConst e : arr) {
    const char *uid = e["c"].as<const char *>();
    if (!nwsValidUid(uid) || nwsFindClient(s, uid)) continue;
    NwsClientTemp *c = nwsFindOrAddClient(s, uid);
    if (!c) break;
    char ids[NWS_STATIONS][NWS_STATION_ID_MAX];
    uint8_t n = 0;
    for (JsonVariantConst id : e["st"].as<JsonArrayConst>()) {
      if (n >= NWS_STATIONS) break;
      const char *v = id.as<const char *>();
      if (nwsValidStationId(v)) snprintf(ids[n++], NWS_STATION_ID_MAX, "%s", v);
    }
    const float lat = e["sl"][0] | 0.0f;
    const float lon = e["sl"][1] | 0.0f;
    const double se = e["se"] | 0.0;
    if (n > 0 && se >= NWS_MIN_VALID_EPOCH && se < NWS_MAX_VALID_EPOCH && (lat != 0.0f || lon != 0.0f)) {
      nwsSetStations(*c, ids, n, lat, lon, se);
    }
    const double sb = e["sb"] | 0.0;
    if (sb >= NWS_MIN_VALID_EPOCH && sb < NWS_MAX_VALID_EPOCH && !(now > 0.0 && sb > now + 3600.0)) {
      c->settledThrough = (uint32_t)sb;
    }
    for (JsonVariantConst p : e["t"].as<JsonArrayConst>()) {
      if (!p[0].is<uint32_t>() || !p[1].is<int>()) continue;
      const uint32_t ts = p[0].as<uint32_t>();
      if (now > 0.0 && (double)ts > now + 3600.0) continue;
      const int tenths = p[1].as<int>();
      nwsAddSample(*c, ts, (float)tenths / 10.0f);
    }
  }
  return s.count;
}

// stdio reader for deserializeJson (ArduinoJson custom-reader contract)
struct NwsFileReader {
  FILE *f;
  int read() {
    const int c = fgetc(f);
    return c == EOF ? -1 : c;
  }
  size_t readBytes(char *dst, size_t n) { return fread(dst, 1, n, f); }
};

#endif  // TANKALARM_NWS_TEMPERATURE_H
