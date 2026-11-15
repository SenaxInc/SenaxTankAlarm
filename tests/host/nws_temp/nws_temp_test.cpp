// Host tests for TankAlarm-112025-Server-BluesOpta/NwsTemperature.h (outdoor
// temperature for the VIN Voltage History chart).
//
//   make test ARDUINOJSON_DIR=<ArduinoJson v7.4.3>/src
//
// The HTTP reader is driven by a byte source that serves canned responses in
// steps of any size and can fail or close at any offset. The JSON fixtures copy
// the shape of real api.weather.gov responses (captured 2026-11-15), with the
// fields the filters must skip.

#include "NwsTemperature.h"

#include <stdarg.h>
#include <stdlib.h>

#include <string>
#include <vector>

// ============================================================================
// Minimal test framework
// ============================================================================
static long gChecks = 0;
static long gFailures = 0;

static std::string fmtStr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string fmtStr(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list again;
  va_copy(again, ap);
  const int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  std::string out(n > 0 ? (size_t)n : 0, '\0');
  if (n > 0) vsnprintf(&out[0], (size_t)n + 1, fmt, again);
  va_end(again);
  return out;
}

static void reportFailure(const char *file, int line, const char *expr, const std::string &detail) {
  ++gFailures;
  if (gFailures <= 200) {
    fprintf(stderr, "%s:%d: CHECK failed: %s%s%s\n", file, line, expr, detail.empty() ? "" : "  -- ",
            detail.c_str());
  }
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++gChecks;                                                                 \
    if (!(cond)) reportFailure(__FILE__, __LINE__, #cond, std::string());      \
  } while (0)

#define CHECK_MSG(cond, ...)                                                   \
  do {                                                                         \
    ++gChecks;                                                                 \
    if (!(cond)) reportFailure(__FILE__, __LINE__, #cond, fmtStr(__VA_ARGS__)); \
  } while (0)

static bool near(double a, double b, double eps) { return fabs(a - b) <= eps; }

// ============================================================================
// Byte source
// ============================================================================
struct Source {
  std::string data;
  size_t pos = 0;
  size_t step = 256;       // bytes per call (at most)
  long failAt = -1;        // return -1 once this offset is reached
  long closeAt = -1;       // return 0 once this offset is reached
  long calls = 0;
};

static int sourceRead(void *ctx, uint8_t *buf, size_t len) {
  Source *s = static_cast<Source *>(ctx);
  s->calls++;
  if (s->failAt >= 0 && s->pos >= (size_t)s->failAt) return -1;
  if (s->closeAt >= 0 && s->pos >= (size_t)s->closeAt) return 0;
  if (s->pos >= s->data.size()) return 0;
  size_t n = s->data.size() - s->pos;
  if (n > len) n = len;
  if (n > s->step) n = s->step;
  if (s->failAt >= 0 && s->pos + n > (size_t)s->failAt) n = (size_t)s->failAt - s->pos;
  if (s->closeAt >= 0 && s->pos + n > (size_t)s->closeAt) n = (size_t)s->closeAt - s->pos;
  if (n == 0) return s->failAt >= 0 ? -1 : 0;
  memcpy(buf, s->data.data() + s->pos, n);
  s->pos += n;
  return (int)n;
}

static std::string chunk(const std::string &body, size_t size, bool extensions) {
  std::string out;
  for (size_t i = 0; i < body.size(); i += size) {
    const std::string part = body.substr(i, size);
    out += fmtStr("%zx%s\r\n", part.size(), extensions ? ";name=value" : "");
    out += part;
    out += "\r\n";
  }
  out += "0\r\nX-Trailer: 1\r\n\r\n";
  return out;
}

static std::string readAllBody(NwsHttpReader &r) {
  std::string out;
  for (;;) {
    const int c = r.read();
    if (c < 0) break;
    out.push_back((char)c);
  }
  return out;
}

// ============================================================================
// Fixtures (shape of real api.weather.gov responses)
// ============================================================================
static const char *kPoints =
    "{\"@context\":[\"https://geojson.org/geojson-ld/geojson-context.jsonld\",{\"@version\":\"1.1\"}],"
    "\"id\":\"https://api.weather.gov/points/38.8894,-77.0352\",\"type\":\"Feature\","
    "\"geometry\":{\"type\":\"Point\",\"coordinates\":[-77.0352,38.8894]},"
    "\"properties\":{\"@id\":\"https://api.weather.gov/points/38.8894,-77.0352\",\"cwa\":\"LWX\","
    "\"forecastOffice\":\"https://api.weather.gov/offices/LWX\",\"gridId\":\"LWX\",\"gridX\":97,\"gridY\":71,"
    "\"forecast\":\"https://api.weather.gov/gridpoints/LWX/97,71/forecast\","
    "\"observationStations\":\"https://api.weather.gov/gridpoints/LWX/97,71/stations\","
    "\"relativeLocation\":{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[-77.01,38.9]},"
    "\"properties\":{\"city\":\"Washington\",\"state\":\"DC\",\"distance\":{\"unitCode\":\"wmoUnit:m\",\"value\":2300},"
    "\"bearing\":{\"unitCode\":\"wmoUnit:degree_(angle)\",\"value\":238}}},\"timeZone\":\"America/New_York\"}}";

static const char *kStations =
    "{\"@context\":[],\"type\":\"FeatureCollection\",\"features\":["
    "{\"id\":\"https://api.weather.gov/stations/KDCA\",\"type\":\"Feature\","
    "\"geometry\":{\"type\":\"Point\",\"coordinates\":[-77.03,38.85]},"
    "\"properties\":{\"@id\":\"https://api.weather.gov/stations/KDCA\",\"elevation\":{\"unitCode\":\"wmoUnit:m\",\"value\":4.0},"
    "\"stationIdentifier\":\"KDCA\",\"name\":\"Washington/Reagan National Airport, DC\",\"timeZone\":\"America/New_York\"}},"
    "{\"id\":\"https://api.weather.gov/stations/KCGS\",\"type\":\"Feature\","
    "\"geometry\":{\"type\":\"Point\",\"coordinates\":[-76.92,38.98]},"
    "\"properties\":{\"stationIdentifier\":\"KCGS\",\"name\":\"College Park Airport\"}}],"
    "\"observationStations\":[\"https://api.weather.gov/stations/KDCA\",\"https://api.weather.gov/stations/KCGS\"],"
    "\"pagination\":{\"next\":\"https://api.weather.gov/stations?id=KCGS&limit=2&cursor=x\"}}";

static std::string observation(const char *ts, const char *value, const char *unit, const char *qc) {
  return fmtStr(
      "{\"id\":\"https://api.weather.gov/stations/KDCA/observations/%s\",\"type\":\"Feature\","
      "\"geometry\":{\"type\":\"Point\",\"coordinates\":[-77.03,38.85]},"
      "\"properties\":{\"@id\":\"x\",\"@type\":\"wx:ObservationStation\",\"elevation\":{\"unitCode\":\"wmoUnit:m\",\"value\":4},"
      "\"station\":\"https://api.weather.gov/stations/KDCA\",\"stationId\":\"KDCA\",\"timestamp\":\"%s\","
      "\"rawMessage\":\"KDCA 131852Z 36010G20KT 10SM BKN048 10/M01 A2968 RMK AO2 SLP052 T01001006\","
      "\"textDescription\":\"Mostly Cloudy\",\"presentWeather\":[],"
      "\"temperature\":{\"unitCode\":\"%s\",\"value\":%s,\"qualityControl\":\"%s\"},"
      "\"dewpoint\":{\"unitCode\":\"wmoUnit:degC\",\"value\":-0.6,\"qualityControl\":\"V\"},"
      "\"cloudLayers\":[{\"base\":{\"unitCode\":\"wmoUnit:m\",\"value\":1460},\"amount\":\"BKN\"}]}}",
      ts, ts, unit, value, qc);
}

static std::string observations(const std::vector<std::string> &features) {
  std::string out = "{\"@context\":[\"https://geojson.org/geojson-ld/geojson-context.jsonld\"],"
                    "\"type\":\"FeatureCollection\",\"features\":[";
  for (size_t i = 0; i < features.size(); ++i) {
    if (i) out += ",";
    out += features[i];
  }
  out += "]}";
  return out;
}

static double iso(const char *s) {
  double e = 0.0;
  if (!nwsParseIso8601(s, &e)) {
    fprintf(stderr, "fixture time does not parse: %s\n", s);
    exit(2);
  }
  return e;
}

// ============================================================================
// Dates
// ============================================================================
static void testIso8601() {
  double e = 0.0;
  CHECK(nwsParseIso8601("2026-11-13T18:52:00+00:00", &e) && e == 1794595920.0);
  CHECK(nwsParseIso8601("2026-11-13T18:52:00Z", &e) && e == 1794595920.0);
  CHECK(nwsParseIso8601("2026-11-13T18:52:00.250Z", &e) && e == 1794595920.0);
  CHECK(nwsParseIso8601("2026-11-13T13:52:00-05:00", &e) && e == 1794595920.0);
  CHECK(nwsParseIso8601("2026-11-14T00:22:00+0530", &e) && e == 1794595920.0);
  CHECK(nwsParseIso8601("2024-02-29T00:00:00Z", &e) && e == 1709164800.0);
  CHECK(nwsParseIso8601("2016-12-31T23:59:60Z", &e) && e == 1483228799.0);  // leap second stays in its minute

  const char *bad[] = {"",
                       "2026-11-13",
                       "2026-11-13T18:52:00",          // no zone
                       "2026-11-13 18:52:00Z",
                       "2026-02-30T00:00:00Z",         // no such day
                       "2025-02-29T00:00:00Z",
                       "2026-13-01T00:00:00Z",
                       "2026-11-13T24:00:00Z",
                       "2026-11-13T18:60:00Z",
                       "2026-11-13T18:52:00+24:00",
                       "2026-11-13T18:52:00+00:00x",
                       "2026-11-13T18:52:00.Z",
                       "1999-12-31T23:59:59Z",
                       "2100-01-01T00:00:00Z",
                       "2026-1x-13T18:52:00Z"};
  for (const char *s : bad) CHECK_MSG(!nwsParseIso8601(s, &e), "accepted %s", s);
  CHECK(!nwsParseIso8601(nullptr, &e));

  char out[24];
  CHECK(nwsFormatIso8601(1794595920.0, out, sizeof(out)) && strcmp(out, "2026-11-13T18:52:00Z") == 0);
  CHECK(nwsFormatIso8601(1794595920.9, out, sizeof(out)) && strcmp(out, "2026-11-13T18:52:00Z") == 0);
  CHECK(nwsFormatIso8601(0.0, out, sizeof(out)) && strcmp(out, "1970-01-01T00:00:00Z") == 0);
  CHECK(!nwsFormatIso8601(-1.0, out, sizeof(out)));
  CHECK(!nwsFormatIso8601(NAN, out, sizeof(out)));
  CHECK(!nwsFormatIso8601(1794595920.0, out, 20));

  // Round trip across years, month ends and leap days
  for (double t = 946684800.0; t < 4102444800.0; t += 86400.0 * 13.0 + 3607.0) {
    CHECK_MSG(nwsFormatIso8601(t, out, sizeof(out)) && nwsParseIso8601(out, &e) && e == floor(t), "%.0f -> %s", t, out);
  }
}

// ============================================================================
// Values and paths
// ============================================================================
static void testValues() {
  float f = 0.0f;
  CHECK(nwsTemperatureF(10.0, "wmoUnit:degC", &f) && near(f, 50.0, 1e-4));
  CHECK(nwsTemperatureF(-40.0, "wmoUnit:degC", &f) && near(f, -40.0, 1e-4));
  CHECK(nwsTemperatureF(72.5, "wmoUnit:degF", &f) && near(f, 72.5, 1e-4));
  CHECK(nwsTemperatureF(273.15, "wmoUnit:K", &f) && near(f, 32.0, 1e-3));
  CHECK(nwsTemperatureF(21.0, "degC", &f) && near(f, 69.8, 1e-4));
  CHECK(!nwsTemperatureF(10.0, "wmoUnit:m", &f));
  CHECK(!nwsTemperatureF(10.0, nullptr, &f));
  CHECK(!nwsTemperatureF(NAN, "wmoUnit:degC", &f));
  CHECK(!nwsTemperatureF(70.0, "wmoUnit:degC", &f));    // 158 F: implausible
  CHECK(!nwsTemperatureF(-70.0, "wmoUnit:degC", &f));   // -94 F

  CHECK(nwsQualityUsable("V") && nwsQualityUsable("Z") && nwsQualityUsable("Q") && nwsQualityUsable("C"));
  CHECK(nwsQualityUsable(nullptr) && nwsQualityUsable(""));
  CHECK(!nwsQualityUsable("X") && !nwsQualityUsable("B"));

  CHECK(nwsTenths(45.25f) == 453 || nwsTenths(45.25f) == 452);
  CHECK(nwsTenths(-12.34f) == -123);
  CHECK(nwsTenths(98.6f) == 986);

  CHECK(nwsValidStationId("KDCA") && nwsValidStationId("W43") && nwsValidStationId("KMDWCN12345"));
  CHECK(!nwsValidStationId("KD") && !nwsValidStationId("KDCA/1") && !nwsValidStationId("KMDWCN123456"));
  CHECK(!nwsValidStationId(nullptr) && !nwsValidStationId("K DC"));

  CHECK(nwsValidUid("dev:864475044012345") && nwsValidUid("a.b_c-d"));
  CHECK(!nwsValidUid("") && !nwsValidUid(nullptr) && !nwsValidUid("dev:\"x") && !nwsValidUid("dev x"));
  CHECK(!nwsValidUid(std::string(NWS_UID_MAX, 'a').c_str()) && nwsValidUid(std::string(NWS_UID_MAX - 1, 'a').c_str()));
}

static void testPaths() {
  char p[NWS_PATH_MAX];
  CHECK(nwsPointsPath(p, sizeof(p), 38.8894f, -77.0352f) && strcmp(p, "/points/38.8894,-77.0352") == 0);
  CHECK(nwsPointsPath(p, sizeof(p), 38.88943f, -77.0f) && strcmp(p, "/points/38.8894,-77.0000") == 0);
  CHECK(!nwsPointsPath(p, sizeof(p), 0.0f, 0.0f));
  CHECK(!nwsPointsPath(p, sizeof(p), 91.0f, 0.5f) && !nwsPointsPath(p, sizeof(p), 10.0f, -181.0f));
  CHECK(!nwsPointsPath(p, sizeof(p), NAN, 10.0f));
  CHECK(!nwsPointsPath(p, 10, 38.8894f, -77.0352f));

  CHECK(nwsStationsPathFromUrl("https://api.weather.gov/gridpoints/LWX/97,71/stations", p, sizeof(p)) &&
        strcmp(p, "/gridpoints/LWX/97,71/stations?limit=2") == 0);
  CHECK(!nwsStationsPathFromUrl("http://api.weather.gov/gridpoints/LWX/97,71/stations", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://api.weather.gov.example/gridpoints/x", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://example.com/https://api.weather.gov/x", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://api.weather.gov/gridpoints/../x", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://api.weather.gov/gridpoints/LWX/97,71/stations?x=1", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://api.weather.gov/a b", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://api.weather.gov", p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl(nullptr, p, sizeof(p)));
  CHECK(!nwsStationsPathFromUrl("https://api.weather.gov/gridpoints/LWX/97,71/stations", p, 20));

  const double t = iso("2026-11-13T18:52:00Z");
  CHECK(nwsObservationsPath(p, sizeof(p), "KDCA", t - 3600.0, t + 3600.0) &&
        strcmp(p, "/stations/KDCA/observations?start=2026-11-13T17:52:00Z&end=2026-11-13T19:52:00Z") == 0);
  CHECK(!nwsObservationsPath(p, sizeof(p), "K/DCA", t, t + 1.0));
  CHECK(!nwsObservationsPath(p, sizeof(p), "KDCA", t + 1.0, t));
  CHECK(!nwsObservationsPath(p, 40, "KDCA", t, t + 1.0));

  char req[512];
  const size_t n = nwsBuildGet(req, sizeof(req), "/points/1.0000,2.0000", "TankAlarm-Server/2.2.17");
  CHECK(n == strlen(req));
  CHECK(strncmp(req, "GET /points/1.0000,2.0000 HTTP/1.1\r\nHost: api.weather.gov\r\n", 59) == 0);
  CHECK(strstr(req, "\r\nUser-Agent: TankAlarm-Server/2.2.17\r\n") != nullptr);
  CHECK(strstr(req, "\r\nAccept-Encoding: identity\r\n") != nullptr);
  CHECK(strstr(req, "\r\nConnection: close\r\n\r\n") != nullptr);
  CHECK(nwsBuildGet(req, sizeof(req), "/x", "") == 0);
  CHECK(nwsBuildGet(req, sizeof(req), "x", "ua") == 0);
  CHECK(nwsBuildGet(req, 40, "/points/1.0000,2.0000", "ua") == 0);
}

// ============================================================================
// HTTP reader
// ============================================================================
static void testHttpFraming() {
  const std::string body = "{\"hello\":\"world\",\"n\":[1,2,3]}";
  const size_t steps[] = {1, 2, 3, 7, 64, 256};

  for (size_t step : steps) {
    // Content-Length (extra bytes after the body are not served)
    {
      Source s;
      s.step = step;
      s.data = "HTTP/1.1 200 OK\r\nContent-Type: application/geo+json\r\ncontent-length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body + "GARBAGE";
      NwsHttpReader r;
      r.begin(sourceRead, &s, 1024);
      CHECK(r.readHead());
      CHECK(r.status == 200 && r.hasLength && !r.chunked && r.length == body.size());
      CHECK_MSG(readAllBody(r) == body, "step %zu", step);
      CHECK(r.bodyDone && !r.bodyError);
    }
    // Chunked, with extensions and trailers, odd chunk sizes
    for (size_t csize : {1u, 5u, 16u, 1000u}) {
      Source s;
      s.step = step;
      s.data = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n" +
               chunk(body, csize, csize == 5);
      NwsHttpReader r;
      r.begin(sourceRead, &s, 1024);
      CHECK(r.readHead());
      CHECK(r.chunked && !r.hasLength);  // chunked wins over Content-Length
      CHECK_MSG(readAllBody(r) == body, "step %zu chunk %zu", step, csize);
      CHECK(r.bodyDone && !r.bodyError);
    }
    // Close-delimited
    {
      Source s;
      s.step = step;
      s.data = "HTTP/1.0 200 OK\r\nServer: x\r\n\r\n" + body;
      NwsHttpReader r;
      r.begin(sourceRead, &s, 1024);
      CHECK(r.readHead() && !r.hasLength && !r.chunked);
      CHECK(readAllBody(r) == body);
      CHECK(r.bodyDone && !r.bodyError);
    }
  }

  // Bare LF line endings, 100 Continue, long header lines, no reason phrase
  {
    Source s;
    s.data = "HTTP/1.1 100 Continue\n\nHTTP/1.1 404\nX-Long: " + std::string(1000, 'a') + "\nContent-Length: 2\n\nhi";
    NwsHttpReader r;
    r.begin(sourceRead, &s, 1024);
    CHECK(r.readHead() && r.status == 404 && r.length == 2);
    CHECK(readAllBody(r) == "hi");
  }
  // Content-Length: 0
  {
    Source s;
    s.data = "HTTP/1.1 301 Moved Permanently\r\nLocation: https://api.weather.gov/x\r\nContent-Length: 0\r\n\r\n";
    NwsHttpReader r;
    r.begin(sourceRead, &s, 1024);
    CHECK(r.readHead() && r.status == 301);
    CHECK(r.read() == -1 && r.bodyDone && !r.bodyError);
  }
  // Bad heads
  const char *badHeads[] = {"", "HTTP/1.1 200 OK\r\n", "HTTP/2 200\r\n\r\n", "HTTP/1.1 2x0 OK\r\n\r\n",
                            "SSH-2.0-OpenSSH\r\n\r\n", "HTTP/1.1 2000 OK\r\n\r\n"};
  for (const char *h : badHeads) {
    Source s;
    s.data = h;
    NwsHttpReader r;
    r.begin(sourceRead, &s, 1024);
    CHECK_MSG(!r.readHead(), "accepted head %s", h);
  }
  // Encodings the reader cannot decode end the body with an error
  const char *encoded[] = {"Content-Encoding: gzip\r\n", "Transfer-Encoding: gzip, chunked\r\n"};
  for (const char *h : encoded) {
    Source s;
    s.data = std::string("HTTP/1.1 200 OK\r\n") + h + "Content-Length: 4\r\n\r\nabcd";
    NwsHttpReader r;
    r.begin(sourceRead, &s, 1024);
    CHECK(r.readHead());
    CHECK(r.read() == -1 && r.bodyError);
  }
  {
    Source s;
    s.data = "HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 2\r\n\r\nok";
    NwsHttpReader r;
    r.begin(sourceRead, &s, 1024);
    CHECK(r.readHead() && readAllBody(r) == "ok" && !r.bodyError);
  }
  // Content-Length overflow
  {
    Source s;
    s.data = "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\nx";
    NwsHttpReader r;
    r.begin(sourceRead, &s, 1024);
    CHECK(r.readHead());
    CHECK(r.read() == -1 && r.bodyError);
  }
}

static void testHttpFailures() {
  const std::string body(300, 'b');
  const std::string lengthResp = "HTTP/1.1 200 OK\r\nContent-Length: 300\r\n\r\n" + body;
  const std::string chunkedResp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk(body, 64, false);
  const size_t headLen = lengthResp.size() - body.size();
  const size_t cHeadLen = chunkedResp.find("\r\n\r\n") + 4;

  // A body cut short (peer close or error) is an error, never a complete body
  for (long cut = (long)headLen; cut < (long)lengthResp.size(); cut += 17) {
    for (int mode = 0; mode < 2; ++mode) {
      Source s;
      s.data = lengthResp;
      s.step = 9;
      if (mode == 0) s.closeAt = cut; else s.failAt = cut;
      NwsHttpReader r;
      r.begin(sourceRead, &s, 4096);
      CHECK(r.readHead());
      const std::string got = readAllBody(r);
      CHECK_MSG(r.bodyError && got.size() < body.size(), "cut %ld mode %d", cut, mode);
    }
  }
  // (the last-chunk line "0\r\n" ends 16 bytes before the end; trailers after it are never read)
  for (long cut = (long)cHeadLen; cut < (long)chunkedResp.size() - 16; cut += 3) {
    Source s;
    s.data = chunkedResp;
    s.closeAt = cut;
    NwsHttpReader r;
    r.begin(sourceRead, &s, 4096);
    CHECK(r.readHead());
    readAllBody(r);
    CHECK_MSG(r.bodyError && !r.bodyDone, "chunked cut %ld", cut);
  }
  // A close-delimited body ends cleanly on close, but not on an error
  {
    Source s;
    s.data = "HTTP/1.1 200 OK\r\n\r\n" + body;
    s.failAt = (long)s.data.size() - 10;
    NwsHttpReader r;
    r.begin(sourceRead, &s, 4096);
    CHECK(r.readHead());
    readAllBody(r);
    CHECK(r.bodyError);
  }
  // Head cut anywhere
  for (long cut = 0; cut < (long)headLen; ++cut) {
    Source s;
    s.data = lengthResp;
    s.closeAt = cut;
    NwsHttpReader r;
    r.begin(sourceRead, &s, 4096);
    CHECK_MSG(!r.readHead(), "head cut %ld", cut);
  }
  // Body over the limit
  {
    Source s;
    s.data = chunkedResp;
    NwsHttpReader r;
    r.begin(sourceRead, &s, 100);
    CHECK(r.readHead());
    CHECK(readAllBody(r).size() == 100 && r.bodyError);
  }
  // Malformed chunk framing
  const char *badChunks[] = {"zz\r\nab\r\n0\r\n\r\n", "2\r\nabX\r\n0\r\n\r\n", "\r\n", "FFFFFFFFF\r\n"};
  for (const char *c : badChunks) {
    Source s;
    s.data = std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") + c;
    NwsHttpReader r;
    r.begin(sourceRead, &s, 4096);
    CHECK(r.readHead());
    readAllBody(r);
    CHECK_MSG(r.bodyError, "chunk %s", c);
  }
  // A source that misbehaves (claims more bytes than asked) is an error
  {
    struct Liar {
      static int read(void *, uint8_t *, size_t len) { return (int)len + 1; }
    };
    NwsHttpReader r;
    r.begin(Liar::read, nullptr, 4096);
    CHECK(!r.readHead() && r.ioError);
  }
}

// ============================================================================
// NWS responses through the reader and the filters
// ============================================================================
template <typename Fill>
static DeserializationError parseResponse(const std::string &resp, size_t step, Fill fillFilter, JsonDocument &doc) {
  Source s;
  s.data = resp;
  s.step = step;
  NwsHttpReader r;
  r.begin(sourceRead, &s, NWS_HTTP_BODY_MAX);
  if (!r.readHead()) return DeserializationError::InvalidInput;
  JsonDocument filter;
  fillFilter(filter);
  return deserializeJson(doc, r, DeserializationOption::Filter(filter));
}

static void testResponses() {
  // /points
  {
    JsonDocument doc;
    const std::string resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk(kPoints, 700, false);
    CHECK(parseResponse(resp, 13, nwsPointsFilter, doc) == DeserializationError::Ok);
    CHECK(!doc["properties"]["gridId"].is<const char *>());  // filtered out
    char path[NWS_PATH_MAX];
    CHECK(nwsParsePoints(doc.as<JsonVariantConst>(), path, sizeof(path)));
    CHECK(strcmp(path, "/gridpoints/LWX/97,71/stations?limit=2") == 0);
    JsonDocument empty;
    deserializeJson(empty, "{\"properties\":{}}");
    CHECK(!nwsParsePoints(empty.as<JsonVariantConst>(), path, sizeof(path)));
  }
  // stations
  {
    JsonDocument doc;
    const std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(strlen(kStations)) + "\r\n\r\n" + kStations;
    CHECK(parseResponse(resp, 5, nwsStationsFilter, doc) == DeserializationError::Ok);
    char ids[NWS_STATIONS][NWS_STATION_ID_MAX];
    CHECK(nwsParseStations(doc.as<JsonVariantConst>(), ids, NWS_STATIONS) == 2);
    CHECK(strcmp(ids[0], "KDCA") == 0 && strcmp(ids[1], "KCGS") == 0);
    CHECK(nwsParseStations(doc.as<JsonVariantConst>(), ids, 1) == 1 && strcmp(ids[0], "KDCA") == 0);

    JsonDocument odd;
    deserializeJson(odd, "{\"features\":[{\"properties\":{\"stationIdentifier\":\"bad id\"}},"
                         "{\"properties\":{}},{\"properties\":{\"stationIdentifier\":\"KXYZ\"}},"
                         "{\"properties\":{\"stationIdentifier\":\"KXYZ\"}},{\"properties\":{\"stationIdentifier\":\"K12\"}}]}");
    CHECK(nwsParseStations(odd.as<JsonVariantConst>(), ids, NWS_STATIONS) == 2);
    CHECK(strcmp(ids[0], "KXYZ") == 0 && strcmp(ids[1], "K12") == 0);
    JsonDocument none;
    deserializeJson(none, "{\"features\":[]}");
    CHECK(nwsParseStations(none.as<JsonVariantConst>(), ids, NWS_STATIONS) == 0);
  }
  // observations: nearest usable one within the window, newest first as NWS lists them
  {
    const std::string body = observations({
        observation("2026-11-13T19:52:00+00:00", "8.3", "wmoUnit:degC", "V"),   // 60 min after: in range
        observation("2026-11-13T19:10:00+00:00", "9", "wmoUnit:degC", "X"),     // 18 min after, rejected by QC
        observation("2026-11-13T18:55:00+00:00", "null", "wmoUnit:degC", "Z"),  // 3 min after, no value
        observation("2026-11-13T18:47:00+00:00", "10.6", "wmoUnit:degC", "V"),  // 5 min before: nearest usable
        observation("2026-11-13T18:30:00+00:00", "11", "wmoUnit:degC", "V"),
        observation("2026-11-13T17:51:59+00:00", "12", "wmoUnit:degC", "V"),    // 1 s outside the window
    });
    const double target = iso("2026-11-13T18:52:00Z");
    for (size_t step : {1u, 31u, 256u}) {
      JsonDocument doc;
      const std::string resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk(body, 4096, false);
      CHECK(parseResponse(resp, step, nwsObservationsFilter, doc) == DeserializationError::Ok);
      CHECK(!doc["features"][0]["properties"]["rawMessage"].is<const char *>());
      CHECK_MSG(measureJson(doc) < 1024, "filtered observations are %zu bytes", measureJson(doc));
      float f = 0.0f;
      double at = 0.0;
      CHECK(nwsNearestObservation(doc.as<JsonVariantConst>(), target, NWS_OBS_MATCH_SEC, &f, &at));
      CHECK(near(f, 51.08, 1e-3) && at == iso("2026-11-13T18:47:00Z"));
      // A tighter window finds nothing usable
      CHECK(!nwsNearestObservation(doc.as<JsonVariantConst>(), target, 120.0, &f, &at));
      // A target near the edge picks the in-range observation
      CHECK(nwsNearestObservation(doc.as<JsonVariantConst>(), iso("2026-11-13T20:30:00Z"), NWS_OBS_MATCH_SEC, &f, &at));
      CHECK(at == iso("2026-11-13T19:52:00Z") && near(f, 46.94, 1e-3));
      CHECK(!nwsNearestObservation(doc.as<JsonVariantConst>(), iso("2026-11-13T16:51:00Z"), NWS_OBS_MATCH_SEC, &f, &at));
    }
    // Ties go to the earlier observation; Fahrenheit passes through
    JsonDocument tie;
    deserializeJson(tie, observations({observation("2026-11-13T19:02:00Z", "40", "wmoUnit:degF", "V"),
                                       observation("2026-11-13T18:42:00Z", "41", "wmoUnit:degF", "V")}));
    float f = 0.0f;
    double at = 0.0;
    CHECK(nwsNearestObservation(tie.as<JsonVariantConst>(), target, NWS_OBS_MATCH_SEC, &f, &at));
    CHECK(near(f, 41.0, 1e-4) && at == iso("2026-11-13T18:42:00Z"));
    JsonDocument empty;
    deserializeJson(empty, "{\"features\":[]}");
    CHECK(!nwsNearestObservation(empty.as<JsonVariantConst>(), target, NWS_OBS_MATCH_SEC, &f, &at));
    // A body cut mid-JSON fails the parse
    JsonDocument cut;
    Source s;
    s.data = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    s.closeAt = (long)(s.data.size() - 40);
    NwsHttpReader r;
    r.begin(sourceRead, &s, NWS_HTTP_BODY_MAX);
    CHECK(r.readHead());
    JsonDocument filter;
    nwsObservationsFilter(filter);
    CHECK(deserializeJson(cut, r, DeserializationOption::Filter(filter)) != DeserializationError::Ok);
  }
}

// ============================================================================
// Store and scheduling
// ============================================================================
static NwsTempStore gStore;  // ~13 KB: keep it off the stack

static void testStore() {
  memset(&gStore, 0, sizeof(gStore));
  CHECK(nwsFindOrAddClient(gStore, "bad uid") == nullptr);
  NwsClientTemp *a = nwsFindOrAddClient(gStore, "dev:1");
  CHECK(a && gStore.count == 1 && nwsFindOrAddClient(gStore, "dev:1") == a);
  const double base = 1763000000.0;

  // Sorted insert, same-epoch replace, bounds
  CHECK(nwsAddSample(*a, (uint32_t)base + 200, 50.0f));
  CHECK(nwsAddSample(*a, (uint32_t)base + 100, 49.0f));
  CHECK(nwsAddSample(*a, (uint32_t)base + 300, 51.0f));
  CHECK(!nwsAddSample(*a, (uint32_t)base + 300, 51.0f));
  CHECK(nwsAddSample(*a, (uint32_t)base + 300, 52.0f));
  CHECK(a->count == 3 && a->sampleEpoch[0] == (uint32_t)base + 100 && a->sampleEpoch[2] == (uint32_t)base + 300);
  CHECK(a->sampleTenthsF[2] == 520);
  CHECK(!nwsAddSample(*a, 1000u, 50.0f));
  CHECK(!nwsAddSample(*a, (uint32_t)base, 200.0f));
  CHECK(!nwsAddSample(*a, (uint32_t)base, NAN));
  CHECK(a->count == 3);

  // Full: the oldest makes room; a sample older than all is dropped; order holds
  NwsClientTemp *b = nwsFindOrAddClient(gStore, "dev:2");
  for (uint32_t i = 0; i < NWS_TEMP_SAMPLES; ++i) CHECK(nwsAddSample(*b, (uint32_t)base + 1000u * (i + 1), (float)i));
  CHECK(b->count == NWS_TEMP_SAMPLES);
  CHECK(!nwsAddSample(*b, (uint32_t)base + 500, 1.0f));
  CHECK(nwsAddSample(*b, (uint32_t)base + 1000u * 50 + 500, 77.0f));  // middle
  CHECK(b->count == NWS_TEMP_SAMPLES && b->sampleEpoch[0] == (uint32_t)base + 2000);
  CHECK(nwsAddSample(*b, (uint32_t)base + 1000u * 500, 88.0f));        // newest
  CHECK(b->sampleEpoch[NWS_TEMP_SAMPLES - 1] == (uint32_t)base + 500000 && b->sampleEpoch[0] == (uint32_t)base + 3000);
  bool sorted = true;
  for (uint16_t i = 1; i < b->count; ++i) sorted = sorted && b->sampleEpoch[i - 1] < b->sampleEpoch[i];
  CHECK(sorted);

  // Coverage
  CHECK(nwsSampleNear(*a, base + 100 + NWS_OBS_MATCH_SEC, NWS_OBS_MATCH_SEC));
  CHECK(!nwsSampleNear(*a, base + 300 + NWS_OBS_MATCH_SEC + 1, NWS_OBS_MATCH_SEC));

  // Which readings need a temperature
  const double now = base + 200000.0;
  CHECK(!nwsReadingNeedsTemp(*a, now - NWS_OBS_SETTLE_SEC + 1, now));     // too fresh
  CHECK(nwsReadingNeedsTemp(*a, now - NWS_OBS_SETTLE_SEC, now));
  CHECK(nwsReadingNeedsTemp(*a, now - NWS_OBS_MAX_AGE_SEC, now));
  CHECK(!nwsReadingNeedsTemp(*a, now - NWS_OBS_MAX_AGE_SEC - 1, now));    // NWS no longer has it
  CHECK(!nwsReadingNeedsTemp(*a, base + 300.0 + NWS_OBS_MATCH_SEC, now));  // covered by a sample
  CHECK(nwsReadingNeedsTemp(*a, base + 301.0 + NWS_OBS_MATCH_SEC, now));
  CHECK(!nwsReadingNeedsTemp(*a, 0.0, now) && !nwsReadingNeedsTemp(*a, now - 5000.0, 0.0));
  // Settled readings are never asked about again; settling only moves forward
  nwsSettle(*a, now - 5000.0);
  CHECK(!nwsReadingNeedsTemp(*a, now - 5000.0, now) && !nwsReadingNeedsTemp(*a, now - 9000.0, now));
  CHECK(nwsReadingNeedsTemp(*a, now - 4999.0, now));
  nwsSettle(*a, now - 9000.0);
  CHECK(a->settledThrough == (uint32_t)(now - 5000.0));

  // A match is always covered afterwards, even at the edge of the window
  NwsClientTemp m;
  memset(&m, 0, sizeof(m));
  const double t = now - 20000.0;
  CHECK(nwsNoteMatched(m, t, (uint32_t)(t - NWS_OBS_MATCH_SEC), 40.0f));
  CHECK(!nwsReadingNeedsTemp(m, t, now) && m.settledThrough == 0);
  // A match the full store cannot keep settles the reading, so it is not fetched forever
  for (uint32_t i = 0; i < NWS_TEMP_SAMPLES; ++i) nwsAddSample(m, (uint32_t)(now - 10000.0) + i * 60u, 50.0f);
  const double old = now - 400000.0;
  CHECK(nwsReadingNeedsTemp(m, old, now));
  CHECK(!nwsNoteMatched(m, old, (uint32_t)old, 41.0f));
  CHECK(!nwsReadingNeedsTemp(m, old, now) && m.settledThrough == (uint32_t)old);

  // Forget compacts the table
  NwsClientTemp *c = nwsFindOrAddClient(gStore, "dev:3");
  CHECK(c && gStore.count == 3);
  CHECK(nwsForgetClient(gStore, "dev:2") && gStore.count == 2);
  CHECK(nwsFindClient(gStore, "dev:2") == nullptr && nwsFindClient(gStore, "dev:3") == &gStore.clients[1]);
  CHECK(!nwsForgetClient(gStore, "dev:9"));
  for (int i = gStore.count; i < NWS_TEMP_MAX_CLIENTS; ++i) CHECK(nwsFindOrAddClient(gStore, fmtStr("dev:x%d", i).c_str()));
  CHECK(nwsFindOrAddClient(gStore, "dev:overflow") == nullptr);
}

static void testScheduling() {
  NwsClientTemp c;
  memset(&c, 0, sizeof(c));
  const double now = 1763000000.0;
  const float lat = 38.8894f, lon = -77.0352f;

  CHECK(nwsPlan(c, true, lat, lon, false, now) == NWS_ACT_NONE);
  CHECK(nwsPlan(c, true, lat, lon, true, 0.0) == NWS_ACT_NONE);
  CHECK(nwsPlan(c, false, 0, 0, true, now) == NWS_ACT_REQUEST_LOCATION);
  c.lastLocationRequest = (uint32_t)now;
  CHECK(nwsPlan(c, false, 0, 0, true, now + NWS_LOCATION_REQUEST_SEC - 1) == NWS_ACT_NONE);
  CHECK(nwsPlan(c, false, 0, 0, true, now + NWS_LOCATION_REQUEST_SEC) == NWS_ACT_REQUEST_LOCATION);
  CHECK(nwsPlan(c, true, lat, lon, true, now) == NWS_ACT_FIND_STATIONS);

  char ids[NWS_STATIONS][NWS_STATION_ID_MAX] = {"KDCA", "KCGS"};
  nwsSetStations(c, ids, 2, lat, lon, now);
  CHECK(nwsStationCount(c) == 2 && strcmp(c.stations[1], "KCGS") == 0);
  CHECK(nwsPlan(c, true, lat, lon, true, now) == NWS_ACT_FETCH_OBS);
  CHECK(nwsPlan(c, true, lat + 0.004f, lon, true, now) == NWS_ACT_FETCH_OBS);   // same place
  CHECK(nwsPlan(c, true, lat + 0.02f, lon, true, now) == NWS_ACT_FIND_STATIONS);  // moved
  CHECK(nwsPlan(c, true, lat, lon, true, now + NWS_STATION_REFRESH_SEC) == NWS_ACT_FIND_STATIONS);
  CHECK(nwsPlan(c, true, lat, lon, true, now - 7200.0) == NWS_ACT_FIND_STATIONS);  // clock went back

  // Failures back off, doubling to the cap; success clears it
  CHECK(nwsRetryDelay(1) == NWS_RETRY_BASE_SEC && nwsRetryDelay(2) == 2 * NWS_RETRY_BASE_SEC);
  CHECK(nwsRetryDelay(200) == NWS_RETRY_MAX_SEC);
  nwsNoteFailure(c, now);
  CHECK(c.failStreak == 1 && c.retryAfter == (uint32_t)(now + NWS_RETRY_BASE_SEC));
  CHECK(nwsPlan(c, true, lat, lon, true, now + 1) == NWS_ACT_NONE);
  CHECK(nwsPlan(c, true, lat, lon, true, now + NWS_RETRY_BASE_SEC) == NWS_ACT_FETCH_OBS);
  for (int i = 0; i < 300; ++i) nwsNoteFailure(c, now);
  CHECK(c.failStreak == 255 && c.retryAfter == (uint32_t)(now + NWS_RETRY_MAX_SEC));
  nwsNoteSuccess(c);
  CHECK(c.failStreak == 0 && c.retryAfter == 0);

  // No match: a young reading is asked again later; an old one is settled
  const double young = now - 3600.0, old = now - NWS_OBS_EMPTY_RETRY_SEC;
  nwsNoteNoMatch(c, young, now);
  CHECK(c.retryAfter == (uint32_t)(now + NWS_OBS_EMPTY_WAIT_SEC) && c.settledThrough == 0);
  CHECK(nwsReadingNeedsTemp(c, young, now));
  nwsNoteNoMatch(c, old, now);
  CHECK(c.retryAfter == 0 && c.settledThrough == (uint32_t)old && !nwsReadingNeedsTemp(c, old, now));

  // Uncovered: weekly; a new location clears it at once
  nwsNoteUncovered(c, now);
  CHECK(c.uncovered && nwsStationCount(c) == 0 && c.retryAfter == (uint32_t)(now + NWS_UNCOVERED_RETRY_SEC));
  CHECK(nwsPlan(c, true, lat, lon, true, now + 86400.0) == NWS_ACT_NONE);
  nwsNoteLocationChanged(c);
  CHECK(!c.uncovered && c.retryAfter == 0 && nwsPlan(c, true, lat, lon, true, now) == NWS_ACT_FIND_STATIONS);
}

static void testSaveLoad() {
  memset(&gStore, 0, sizeof(gStore));
  const double now = 1763000000.0;
  NwsClientTemp *a = nwsFindOrAddClient(gStore, "dev:864475044012345");
  char ids[NWS_STATIONS][NWS_STATION_ID_MAX] = {"KDCA", "KCGS"};
  nwsSetStations(*a, ids, 2, 38.8894f, -77.0352f, now - 86400.0);
  nwsAddSample(*a, (uint32_t)now - 7200, 45.3f);
  nwsAddSample(*a, (uint32_t)now - 3600, -3.7f);
  a->failStreak = 4;  // RAM only: not saved
  nwsSettle(*a, now - 9000.0);
  NwsClientTemp *b = nwsFindOrAddClient(gStore, "dev:2");
  nwsAddSample(*b, (uint32_t)now - 100, 70.0f);
  nwsFindOrAddClient(gStore, "dev:empty");  // nothing to save

  const size_t need = nwsSerializeStore(gStore, nullptr, 0);
  std::vector<char> buf(need + 1);
  CHECK(nwsSerializeStore(gStore, buf.data(), buf.size()) == need && strlen(buf.data()) == need);
  CHECK(strstr(buf.data(), "\"dev:empty\"") == nullptr);
  CHECK(strstr(buf.data(), "\"st\":[\"KDCA\",\"KCGS\"],\"sl\":[38.8894,-77.0352],\"se\":1762913600,\"sb\":1762991000,") != nullptr);
  CHECK(strstr(buf.data(), "\"t\":[[1762992800,453],[1762996400,-37]]") != nullptr);
  CHECK(strstr(buf.data(), "{\"c\":\"dev:2\",\"t\":[[1762999900,700]]}") != nullptr);

  // A too-small buffer still reports the full length and stays terminated
  char small[16];
  CHECK(nwsSerializeStore(gStore, small, sizeof(small)) == need && strlen(small) == sizeof(small) - 1);

  JsonDocument doc;
  CHECK(deserializeJson(doc, buf.data()) == DeserializationError::Ok);
  static NwsTempStore loaded;
  CHECK(nwsLoadStore(loaded, doc.as<JsonArrayConst>(), now) == 2);
  const NwsClientTemp *la = nwsFindClient(loaded, "dev:864475044012345");
  CHECK(la && la->count == 2 && la->sampleTenthsF[0] == 453 && la->sampleTenthsF[1] == -37);
  CHECK(la && nwsStationCount(*la) == 2 && la->stationsEpoch == (uint32_t)(now - 86400.0));
  CHECK(la && nwsStationsCurrent(*la, 38.8894f, -77.0352f, now));
  CHECK(la && la->failStreak == 0 && la->settledThrough == (uint32_t)(now - 9000.0));
  const size_t again = nwsSerializeStore(loaded, nullptr, 0);
  std::vector<char> buf2(again + 1);
  nwsSerializeStore(loaded, buf2.data(), buf2.size());
  CHECK(std::string(buf.data()) == std::string(buf2.data()));

  // Bad entries are skipped, good ones kept
  JsonDocument messy;
  deserializeJson(messy,
                  "[{\"c\":\"bad uid\",\"t\":[[1763000000,1]]},"
                  "{\"c\":\"dev:a\",\"st\":[\"KDCA\",\"no/pe\",\"KCGS\"],\"sl\":[38.9,-77.0],\"se\":1762000000,\"sb\":1762990000,"
                  "\"t\":[[1763000000,453],[\"x\",1],[1000,2],[1762999000,\"y\"],[1763090000,9],[1762990000,-1500],[1762980000,100]]},"
                  "{\"c\":\"dev:a\",\"t\":[[1763000000,1]]},"
                  "{\"c\":\"dev:b\",\"st\":[\"KDCA\"],\"se\":1762000000,\"sb\":1763090000,\"t\":[]},"
                  "{\"t\":[]},5]");
  CHECK(nwsLoadStore(loaded, messy.as<JsonArrayConst>(), now) == 2);
  const NwsClientTemp *ma = nwsFindClient(loaded, "dev:a");
  CHECK(ma && ma->count == 2 && ma->sampleEpoch[0] == 1762980000u && ma->sampleEpoch[1] == 1763000000u);
  CHECK(ma && nwsStationCount(*ma) == 2 && strcmp(ma->stations[0], "KDCA") == 0 && strcmp(ma->stations[1], "KCGS") == 0);
  const NwsClientTemp *mb = nwsFindClient(loaded, "dev:b");
  CHECK(mb && nwsStationCount(*mb) == 0);  // no location saved with it
  CHECK(ma && ma->settledThrough == 1762990000u);
  CHECK(mb && mb->settledThrough == 0);    // later than now + 1 h
  JsonDocument notArray;
  deserializeJson(notArray, "{\"c\":\"dev:a\"}");
  CHECK(nwsLoadStore(loaded, notArray.as<JsonArrayConst>(), now) == 0);

  // Full store round-trips
  memset(&gStore, 0, sizeof(gStore));
  for (int i = 0; i < NWS_TEMP_MAX_CLIENTS; ++i) {
    NwsClientTemp *c = nwsFindOrAddClient(gStore, fmtStr("dev:86447504401%04d", i).c_str());
    nwsSetStations(*c, ids, 2, 40.0f + i * 0.1f, -100.0f, now);
    for (int k = 0; k < NWS_TEMP_SAMPLES; ++k) nwsAddSample(*c, (uint32_t)(now - 86400.0 * k), -20.0f + k);
  }
  const size_t fullLen = nwsSerializeStore(gStore, nullptr, 0);
  CHECK_MSG(fullLen < 49152, "full store is %zu bytes", fullLen);
  std::vector<char> full(fullLen + 1);
  nwsSerializeStore(gStore, full.data(), full.size());
  JsonDocument fullDoc;
  CHECK(deserializeJson(fullDoc, full.data()) == DeserializationError::Ok);
  CHECK(nwsLoadStore(loaded, fullDoc.as<JsonArrayConst>(), now) == NWS_TEMP_MAX_CLIENTS);
  CHECK(loaded.clients[NWS_TEMP_MAX_CLIENTS - 1].count == NWS_TEMP_SAMPLES);
  printf("full store: %zu bytes saved, %zu bytes in RAM\n", fullLen, sizeof(NwsTempStore));
}

static void testFileReader() {
  const char *path = "nws_temp_reader_test.json";  // tmpfile() needs write access to C:\ on some Windows CRTs
  FILE *f = fopen(path, "wb");
  CHECK(f != nullptr);
  if (!f) return;
  fputs("[{\"c\":\"dev:1\",\"t\":[[1763000000,453]]}]", f);
  fclose(f);
  f = fopen(path, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  NwsFileReader r = {f};
  JsonDocument doc;
  CHECK(deserializeJson(doc, r) == DeserializationError::Ok);
  static NwsTempStore s;
  CHECK(nwsLoadStore(s, doc.as<JsonArrayConst>(), 0.0) == 1 && s.clients[0].sampleTenthsF[0] == 453);
  fclose(f);
  remove(path);
}

int main() {
  testIso8601();
  testValues();
  testPaths();
  testHttpFraming();
  testHttpFailures();
  testResponses();
  testStore();
  testScheduling();
  testSaveLoad();
  testFileReader();
  printf("nws_temp: %ld checks, %ld failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
