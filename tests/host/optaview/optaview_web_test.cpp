// Host tests for OptaView's web side: the HTTP reader and writer, the network address policy and
// the web server with its API. Requests are written here as strings and answered by the code
// that runs on the Opta, on top of the simulated SunSaver of the other suite.
//
// Build and run with `make test` in this folder (see the Makefile).

#include "test_support.h"

#include "OptaViewHttp.h"
#include "OptaViewNet.h"
#include "OptaViewWeb.h"
#include "OptaViewWebPage.h"

#include <deque>
#include <functional>
#include <memory>

// ============================================================================
// What stands in for the board
// ============================================================================

// A clock that only moves when the code under test waits.
struct ManualClock : OvClock {
  uint64_t now = 0;
  uint32_t nowMs() override { return (uint32_t)(0xFFFFF000ULL + now); }  // four seconds before it wraps
  void idle() override {
    if (++now > 24ULL * 3600ULL * 1000ULL) {
      throw std::runtime_error("a day has passed on the test clock and it has not returned");
    }
  }
};

// The simulator's clock, for everything that also talks to the simulated controller.
struct SimClock : OvClock {
  SimSunSaver &sim;
  explicit SimClock(SimSunSaver &s) : sim(s) {}
  uint32_t nowMs() override { return sim.nowMs(); }
  void idle() override { sim.idle(); }
};

static const uint64_t kNever = std::numeric_limits<uint64_t>::max();

// One TCP connection as the server sees it: bytes that arrive when the test says so, and a
// record of everything that was sent back.
struct FakeConn : OvConn {
  struct Piece {
    uint64_t at;
    std::string data;
  };
  std::function<uint64_t()> clock;  // elapsed milliseconds
  std::deque<Piece> arriving;
  size_t offset = 0;
  std::string sent;
  std::vector<uint64_t> sentAt;      // when each write that took something was made
  std::vector<size_t> sentSizes;     // ...and how many bytes it took
  uint64_t goneAt = kNever;          // from then on the other end is gone, and a write says so
  size_t failAfterBytes = std::numeric_limits<size_t>::max();
  // How the other end takes what is sent to it. As it stands: all of it, at once.
  size_t takeAtMost = std::numeric_limits<size_t>::max();  // bytes a write takes
  uint64_t takeEveryMs = 0;          // after taking something it takes nothing for this long: a slow reader
  uint64_t deafFrom = kNever;        // from then on it takes nothing, without being gone
  uint64_t deafUntil = 0;            // ...and neither does it before this
  // A sender that never pauses: once everything that was fed has been read, read() returns this
  // byte for ever (-1: it does not). `tick` is called after every thousand of them, which is how
  // time passes in a test where nobody waits.
  int endless = -1;
  uint64_t endlessReads = 0;
  std::function<void()> tick;
  bool failed = false;
  unsigned writes = 0;
  uint64_t refusedAt = kNever;       // the moment of the last write that took nothing
  unsigned refusedThen = 0;          // ...and how many writes that moment has seen
  unsigned writesAfterFailure = 0;
  unsigned writesAfterClose = 0;
  unsigned readsAfterClose = 0;
  unsigned closes = 0;

  // `data` arrives `afterMs` from now. Feed pieces in the order in which they arrive.
  void feed(const std::string &data, uint64_t afterMs = 0) {
    Piece piece;
    piece.at = clock() + afterMs;
    piece.data = data;
    arriving.push_back(piece);
  }

  int read() override {
    if (closes > 0) {
      ++readsAfterClose;
      return -1;
    }
    while (!arriving.empty()) {
      Piece &piece = arriving.front();
      if (piece.at > clock()) {
        return -1;
      }
      if (offset < piece.data.size()) {
        return (unsigned char)piece.data[offset++];
      }
      arriving.pop_front();
      offset = 0;
    }
    if (endless >= 0) {
      if (++endlessReads % 1000 == 0 && tick) {
        tick();
      }
      return endless;
    }
    return -1;
  }

  int write(const uint8_t *data, size_t len) override {
    ++writes;
    if (closes > 0) {
      ++writesAfterClose;
      return -1;
    }
    if (failed) {
      ++writesAfterFailure;
      return -1;
    }
    const uint64_t now = clock();
    if (now >= goneAt || sent.size() + len > failAfterBytes) {
      failed = true;
      return -1;
    }
    if (now >= deafFrom || now < deafUntil || (!sentAt.empty() && now - sentAt.back() < takeEveryMs)) {
      // Whoever sends has to let time pass before it tries again. A sender that does not would
      // try for ever, and the test with it.
      if (now != refusedAt) {
        refusedAt = now;
        refusedThen = 0;
      }
      if (++refusedThen > 100000) {
        throw std::runtime_error("the sender tried 100000 times in one millisecond: it waits without its clock moving");
      }
      return 0;
    }
    const size_t taken = len < takeAtMost ? len : takeAtMost;
    sent.append((const char *)data, taken);
    sentAt.push_back(now);
    sentSizes.push_back(taken);
    return (int)taken;
  }

  void close() override { ++closes; }

  // Bytes that had arrived but were never read.
  size_t unread() const {
    size_t count = 0;
    for (size_t i = 0; i < arriving.size(); ++i) {
      count += arriving[i].data.size() - (i == 0 ? offset : 0);
    }
    return count;
  }
};

// ============================================================================
// Reading an answer the way a browser would
// ============================================================================
struct Reply {
  bool answered = false;  // a complete header block came back
  int status = 0;
  std::string head;       // the status line and the headers, without the empty line
  std::string body;
  // For answers of /api/console: the console's text without the heartbeat bytes, and how many
  // of those there were. The JSON after the trailer byte is in `json`.
  std::string text;
  unsigned heartbeats = 0;
  bool isJson = false;    // `json` holds the body (or the console answer's trailer)
  JsonDocument json;

  // The value of the first header with this name (compared without regard to case), or "".
  std::string header(const char *name) const {
    std::string found;
    eachHeader(name, &found);
    return found;
  }
  size_t headerCount(const char *name) const { return eachHeader(name, nullptr); }

 private:
  size_t eachHeader(const char *name, std::string *first) const {
    size_t count = 0;
    size_t at = head.find("\r\n");
    while (at != std::string::npos) {
      const size_t start = at + 2;
      const size_t end = head.find("\r\n", start);
      const std::string line = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
      const size_t colon = line.find(':');
      if (colon != std::string::npos && ovEqualsIgnoreCase(line.substr(0, colon).c_str(), name)) {
        if (count == 0 && first != nullptr) {
          size_t value = colon + 1;
          while (value < line.size() && line[value] == ' ') {
            ++value;
          }
          *first = line.substr(value);
        }
        ++count;
      }
      at = end;
    }
    return count;
  }
};

static Reply parseReply(const std::string &raw) {
  Reply reply;
  const size_t split = raw.find("\r\n\r\n");
  if (split == std::string::npos || raw.compare(0, 9, "HTTP/1.1 ") != 0) {
    return reply;
  }
  reply.answered = true;
  reply.head = raw.substr(0, split);
  reply.body = raw.substr(split + 4);
  reply.status = atoi(raw.c_str() + 9);
  const std::string type = reply.header("Content-Type");
  if (type == "application/json") {
    reply.isJson = !deserializeJson(reply.json, reply.body);
  } else if (reply.body.find(OV_WEB_TRAILER) != std::string::npos) {
    const size_t trailer = reply.body.rfind(OV_WEB_TRAILER);
    for (size_t i = 0; i < trailer; ++i) {
      if (reply.body[i] == OV_WEB_HEARTBEAT) {
        ++reply.heartbeats;
      } else {
        reply.text += reply.body[i];
      }
    }
    reply.isJson = !deserializeJson(reply.json, reply.body.substr(trailer + 1));
  } else {
    reply.text = reply.body;
  }
  return reply;
}

// What every answer has to say about itself, whatever it is an answer to.
static void checkCommonHeaders(const Reply &reply, const char *file, int line) {
  ++gChecks;
  std::string problem;
  if (!reply.answered) {
    problem = "no answer";
  } else if (reply.header("Cache-Control") != "no-store") {
    problem = "Cache-Control";
  } else if (reply.header("X-Content-Type-Options") != "nosniff") {
    problem = "X-Content-Type-Options";
  } else if (reply.header("X-Frame-Options") != "DENY") {
    problem = "X-Frame-Options";
  } else if (reply.header("Referrer-Policy") != "no-referrer") {
    problem = "Referrer-Policy";
  } else if (reply.header("Connection") != "close") {
    problem = "Connection";
  } else if (reply.headerCount("Content-Security-Policy") != 1) {
    problem = "Content-Security-Policy";
  } else if (reply.status != 204 && reply.headerCount("Content-Type") != 1) {
    problem = "Content-Type";
  }
  if (!problem.empty()) {
    reportFailure(file, line, "the answer carries the headers every answer has", problem + "\n" + excerpt(reply.head));
  }
}
#define CHECK_HEADERS(reply) checkCommonHeaders((reply), __FILE__, __LINE__)

static std::string jsonQuote(const std::string &text) {
  std::string out = "\"";
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      out += fmtStr("\\u%04x", (unsigned)c);
    } else {
      out += (char)c;
    }
  }
  return out + "\"";
}

// ============================================================================
// The HTTP reader
// ============================================================================
struct HttpRig {
  ManualClock clock;
  FakeConn conn;
  OvHttpRequest request;
  HttpRig() {
    conn.clock = [this] { return clock.now; };
    memset(&request, 0xAA, sizeof(request));  // what a request leaves behind must not show through
  }
  OvHttpParse read(const std::string &raw, int first = -1) {
    if (!raw.empty()) {
      conn.feed(raw);
    }
    OvHttpReader reader(conn, clock);
    return reader.read(first, "optaview_session", "x-optaview", &request);
  }
};

static OvHttpParse parseOnly(const std::string &raw) {
  HttpRig rig;
  return rig.read(raw);
}

static void testHttpReader() {
  {
    HttpRig h;
    CHECK_EQ(h.read("GET /api/read?groups=live,status&auto=1 HTTP/1.1\r\n"
                    "Host: 192.168.1.57\r\n"
                    "User-Agent: test\r\n"
                    "Cookie: a=b; optaview_session=0123abcd; c=d\r\n"
                    "X-OptaView: 1\r\n"
                    "\r\n"),
             OV_HTTP_OK);
    CHECK(strcmp(h.request.method, "GET") == 0);
    CHECK(strcmp(h.request.path, "/api/read") == 0);
    CHECK(strcmp(h.request.query, "groups=live,status&auto=1") == 0);
    CHECK(strcmp(h.request.host, "192.168.1.57") == 0);
    CHECK(strcmp(h.request.cookie, "0123abcd") == 0);
    CHECK(h.request.marker);
    CHECK_EQ(h.request.bodyLength, 0);
    CHECK_EQ(h.request.body[0], 0);
    CHECK_EQ(h.clock.now, 0);  // everything was there: no waiting
    CHECK_EQ(h.conn.unread(), 0);
  }
  {
    // A POST with a body, header names in any case, values with blanks around them.
    HttpRig h;
    CHECK_EQ(h.read("POST /api/console HTTP/1.1\r\n"
                    "HOST:192.168.1.57:80  \r\n"
                    "content-LENGTH:   16\t\r\n"
                    "x-optaview:\r\n"
                    "\r\n"
                    "{\"line\":\"live\"}\nextra"),
             OV_HTTP_OK);
    CHECK(strcmp(h.request.method, "POST") == 0);
    CHECK(strcmp(h.request.path, "/api/console") == 0);
    CHECK_EQ(h.request.query[0], 0);
    CHECK(strcmp(h.request.host, "192.168.1.57:80") == 0);
    CHECK_EQ(h.request.cookie[0], 0);
    CHECK(h.request.marker);
    CHECK_EQ(h.request.bodyLength, 16);
    CHECK(std::string(h.request.body) == "{\"line\":\"live\"}\n");
    CHECK_EQ(h.conn.unread(), 5);  // what follows the body is not the request's
  }
  {
    // Line feeds alone end lines as well, HTTP/1.0 is read, and without the headers that are
    // looked for the fields stay empty.
    HttpRig h;
    CHECK_EQ(h.read("GET / HTTP/1.0\n\n"), OV_HTTP_OK);
    CHECK(strcmp(h.request.path, "/") == 0);
    CHECK_EQ(h.request.host[0], 0);
    CHECK_EQ(h.request.cookie[0], 0);
    CHECK(!h.request.marker);
  }
  {
    // A byte that the server had already taken to see whether the connection is alive.
    HttpRig h;
    CHECK_EQ(h.read("ET /x HTTP/1.1\r\n\r\n", 'G'), OV_HTTP_OK);
    CHECK(strcmp(h.request.method, "GET") == 0);
    CHECK(strcmp(h.request.path, "/x") == 0);
  }
  {
    // A body may hold anything, and a GET may have one.
    HttpRig h;
    CHECK_EQ(h.read(std::string("GET /x HTTP/1.1\r\nContent-Length: 5\r\n\r\na\0b\xFF\n", 43)), OV_HTTP_OK);
    CHECK_EQ(h.request.bodyLength, 5);
    CHECK(memcmp(h.request.body, "a\0b\xFF\n", 5) == 0);
    CHECK_EQ(h.request.body[5], 0);
  }

  // --- The request line ---
  CHECK_EQ(parseOnly("\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET /\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/2.0\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / http/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1 \r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET  / HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("get / HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly(" GET / HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("G3T / HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("OPTIONS / HTTP/1.1\r\n\r\n"), OV_HTTP_OK);  // seven letters fit; the server answers 405
  CHECK_EQ(parseOnly("PROPFIND / HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET http://192.168.1.57/ HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("OPTIONS * HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET x HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET /a\tb HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET /caf\xC3\xA9 HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET /a\x7F HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly(std::string("GET /a\0b HTTP/1.1\r\n\r\n", 21)), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET /a\rb HTTP/1.1\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  {
    // The path holds 63 characters and the query 127. One more is refused, not cut.
    HttpRig h;
    const std::string path = "/" + std::string(62, 'p');
    const std::string query(127, 'q');
    CHECK_EQ(h.read("GET " + path + "?" + query + " HTTP/1.1\r\n\r\n"), OV_HTTP_OK);
    CHECK(path == h.request.path);
    CHECK(query == h.request.query);
    CHECK_EQ(parseOnly("GET " + path + "p?" + query + " HTTP/1.1\r\n\r\n"), OV_HTTP_TARGET_TOO_LONG);
    CHECK_EQ(parseOnly("GET " + path + "?" + query + "q HTTP/1.1\r\n\r\n"), OV_HTTP_TARGET_TOO_LONG);
    // A second question mark belongs to the query.
    HttpRig two;
    CHECK_EQ(two.read("GET /a?b?c HTTP/1.1\r\n\r\n"), OV_HTTP_OK);
    CHECK(strcmp(two.request.path, "/a") == 0);
    CHECK(strcmp(two.request.query, "b?c") == 0);
  }
  {
    // A request line that does not fit in the line buffer: it stops at the first byte too many.
    HttpRig h;
    CHECK_EQ(h.read("GET /" + std::string(3000, 'a') + " HTTP/1.1\r\n\r\n"), OV_HTTP_TARGET_TOO_LONG);
    CHECK(h.conn.unread() > 1900);
    CHECK_EQ(h.clock.now, 0);
  }

  // --- Headers ---
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nNoColon\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\n: empty name\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nHost : 192.168.1.57\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nA: 1\r\n folded\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nA: 1\r\n\tfolded\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nA: be\x01ll\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly(std::string("GET / HTTP/1.1\r\nA: nu\0l\r\n\r\n", 28)), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nA: lone\rcr\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nA: tab\tis fine\r\nB: caf\xC3\xA9 too\r\n\r\n"), OV_HTTP_OK);
  CHECK_EQ(parseOnly("GET / HTTP/1.1\r\nHost: 192.168.1.57\r\nHost: 192.168.1.57\r\n\r\n"), OV_HTTP_BAD_REQUEST);
  {
    // A Host that is too long to be an address is not kept at all.
    HttpRig h;
    CHECK_EQ(h.read("GET / HTTP/1.1\r\nHost: " + std::string(47, 'h') + "\r\n\r\n"), OV_HTTP_OK);
    CHECK_EQ(strlen(h.request.host), 47);
    HttpRig longer;
    CHECK_EQ(longer.read("GET / HTTP/1.1\r\nHost: " + std::string(48, 'h') + "\r\n\r\n"), OV_HTTP_OK);
    CHECK_EQ(longer.request.host[0], 0);
  }
  {
    // A header line may be 1024 bytes long with its carriage return.
    const std::string fits = "X-Long: " + std::string(1024 - 8 - 1, 'x');
    CHECK_EQ(fits.size(), 1023);
    CHECK_EQ(parseOnly("GET / HTTP/1.1\r\n" + fits + "\r\n\r\n"), OV_HTTP_OK);
    CHECK_EQ(parseOnly("GET / HTTP/1.1\r\n" + fits + "x\r\n\r\n"), OV_HTTP_HEADERS_TOO_BIG);
    // Sixty-four headers are read, the sixty-fifth is one too many.
    std::string many;
    for (int i = 0; i < 64; ++i) {
      many += fmtStr("X-H%d: %d\r\n", i, i);
    }
    CHECK_EQ(parseOnly("GET / HTTP/1.1\r\n" + many + "\r\n"), OV_HTTP_OK);
    CHECK_EQ(parseOnly("GET / HTTP/1.1\r\n" + many + "X-One-More: 1\r\n\r\n"), OV_HTTP_HEADERS_TOO_BIG);
  }

  // --- The cookie ---
  {
    struct Case {
      const char *header;
      const char *expected;
    };
    const Case cases[] = {
        {"optaview_session=abc", "abc"},
        {"optaview_session=abc; other=1", "abc"},
        {"other=1; optaview_session=abc", "abc"},
        {"other=1;optaview_session=abc;more=2", "abc"},
        {"xoptaview_session=bad; optaview_session=good", "good"},
        {"optaview_session_old=bad; optaview_session=good", "good"},
        {"optaview_session=first; optaview_session=second", "first"},
        {"OPTAVIEW_SESSION=abc", ""},  // cookie names are case sensitive
        {"optaview_session", ""},
        {"optaview_session=", ""},
        {"other=optaview_session=abc", ""},
        {"", ""},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      HttpRig h;
      CHECK_EQ(h.read(std::string("GET / HTTP/1.1\r\nCookie: ") + cases[i].header + "\r\n\r\n"), OV_HTTP_OK);
      CHECK_MSG(strcmp(h.request.cookie, cases[i].expected) == 0, "Cookie: %s gave '%s'", cases[i].header,
                h.request.cookie);
    }
    // A value that does not fit is not a session token: it is skipped, never cut.
    HttpRig h;
    CHECK_EQ(h.read("GET / HTTP/1.1\r\nCookie: optaview_session=" + std::string(47, 'a') + "\r\n\r\n"), OV_HTTP_OK);
    CHECK_EQ(strlen(h.request.cookie), 47);
    HttpRig longer;
    CHECK_EQ(longer.read("GET / HTTP/1.1\r\nCookie: optaview_session=" + std::string(48, 'a') + "\r\n\r\n"), OV_HTTP_OK);
    CHECK_EQ(longer.request.cookie[0], 0);
    // Two Cookie headers: the first one that has the cookie counts.
    HttpRig twice;
    CHECK_EQ(twice.read("GET / HTTP/1.1\r\nCookie: a=1\r\nCookie: optaview_session=two\r\n\r\n"), OV_HTTP_OK);
    CHECK(strcmp(twice.request.cookie, "two") == 0);
  }

  // --- The body ---
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\n\r\n"), OV_HTTP_LENGTH_REQUIRED);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n"), OV_HTTP_OK);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: \r\n\r\n"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: +2\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: -2\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 0x2\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 2, 2\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 2 2\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 4294967298\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}"), OV_HTTP_OK);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}"), OV_HTTP_BAD_REQUEST);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 0002\r\n\r\n{}"), OV_HTTP_OK);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n"), OV_HTTP_NOT_IMPLEMENTED);
  CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n{}"),
           OV_HTTP_NOT_IMPLEMENTED);
  CHECK_EQ(parseOnly("GET /x HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n"), OV_HTTP_NOT_IMPLEMENTED);
  {
    HttpRig h;
    const std::string body(1024, 'b');
    CHECK_EQ(h.read("POST /x HTTP/1.1\r\nContent-Length: 1024\r\n\r\n" + body), OV_HTTP_OK);
    CHECK_EQ(h.request.bodyLength, 1024);
    CHECK(body == h.request.body);
    // One byte more is refused before any of it is read.
    HttpRig big;
    CHECK_EQ(big.read("POST /x HTTP/1.1\r\nContent-Length: 1025\r\n\r\n" + body + "b"), OV_HTTP_BODY_TOO_LARGE);
    CHECK_EQ(big.conn.unread(), 1025);
    CHECK_EQ(parseOnly("POST /x HTTP/1.1\r\nContent-Length: 100000000\r\n\r\n"), OV_HTTP_BODY_TOO_LARGE);
  }

  // --- Time ---
  {
    // Nothing at all after the first byte: two seconds, then the connection is given up.
    HttpRig h;
    CHECK_EQ(h.read("", 'G'), OV_HTTP_TIMEOUT);
    CHECK_EQ(h.clock.now, 2000);
  }
  {
    // The headers never end.
    HttpRig h;
    CHECK_EQ(h.read("GET / HTTP/1.1\r\nHost: 192.168.1.57\r\n"), OV_HTTP_TIMEOUT);
    CHECK_EQ(h.clock.now, 2000);
  }
  {
    // The body is shorter than it was said to be.
    HttpRig h;
    CHECK_EQ(h.read("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\n{}"), OV_HTTP_TIMEOUT);
    CHECK_EQ(h.clock.now, 2000);
  }
  {
    // A request that arrives in pieces, each within two seconds of the one before, is read.
    HttpRig h;
    h.conn.feed("POST /x HT");
    h.conn.feed("TP/1.1\r\nContent-Le", 1900);
    h.conn.feed("ngth: 4\r\n\r\nab", 3800);
    h.conn.feed("cd", 5700);
    CHECK_EQ(h.read(""), OV_HTTP_OK);
    CHECK(std::string(h.request.body) == "abcd");
    CHECK_EQ(h.clock.now, 5700);
  }
  {
    // Six seconds is all a request gets, however steadily it trickles in.
    HttpRig h;
    h.conn.feed("GET /");
    for (int i = 1; i <= 20; ++i) {
      h.conn.feed("a", (uint64_t)i * 1000);
    }
    CHECK_EQ(h.read(""), OV_HTTP_TIMEOUT);
    CHECK_EQ(h.clock.now, 6000);
  }
  {
    // The limit holds when the bytes never stop coming, too: nobody waits, so the test clock
    // only moves because the connection's own clock is asked for the time.
    struct Flood : OvConn {
      ManualClock *clock;
      uint64_t reads = 0;
      int read() override {
        if (++reads % 1000 == 0) {
          ++clock->now;  // a thousand bytes a millisecond
        }
        return reads % 900 == 0 ? '\n' : 'a';
      }
      int write(const uint8_t *, size_t len) override { return (int)len; }
      void close() override {}
    };
    ManualClock clock;
    Flood flood;
    flood.clock = &clock;
    OvHttpRequest request;
    OvHttpReader reader(flood, clock);
    // Lines of 899 bytes without a colon: the first one is a bad request line.
    CHECK_EQ(reader.read(-1, "c", "m", &request), OV_HTTP_BAD_REQUEST);
    // A request line that is fine, then header lines without end.
    struct Headers : OvConn {
      ManualClock *clock;
      uint64_t reads = 0;
      std::string start = "GET / HTTP/1.1\r\n";
      int read() override {
        if (reads < start.size()) {
          return (unsigned char)start[reads++];
        }
        ++reads;
        if (reads % 1000 == 0) {
          ++clock->now;
        }
        const uint64_t at = reads % 6;
        return at == 0 ? 'a' : at == 1 ? ':' : at == 2 ? 'b' : at == 3 ? '\r' : at == 4 ? '\n' : 'x';
      }
      int write(const uint8_t *, size_t len) override { return (int)len; }
      void close() override {}
    };
    ManualClock clock2;
    Headers headers;
    headers.clock = &clock2;
    OvHttpReader reader2(headers, clock2);
    // It ends with "too many headers" long before the clock would have to end it.
    CHECK_EQ(reader2.read(-1, "c", "m", &request), OV_HTTP_HEADERS_TOO_BIG);
    CHECK(headers.reads < 1000);
    // A body that is sent byte by byte with pauses just under the limit runs into the six seconds.
    HttpRig h;
    h.conn.feed("POST /x HTTP/1.1\r\nContent-Length: 1000\r\n\r\n");
    for (int i = 1; i <= 10; ++i) {
      h.conn.feed("z", (uint64_t)i * 1900);
    }
    CHECK_EQ(h.read(""), OV_HTTP_TIMEOUT);
    CHECK_EQ(h.clock.now, 6000);
  }

  // --- Draining ---
  {
    ManualClock clock;
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.feed(std::string(100, 'x'));
    conn.feed(std::string(100, 'y'), 30);
    conn.feed(std::string(100, 'z'), 200);  // too late: by then it has been quiet for 50 ms
    ovHttpDrain(conn, clock, 50, 500);
    CHECK_EQ(conn.unread(), 100);
    CHECK_EQ(clock.now, 80);
    // However much keeps coming, it ends.
    FakeConn busy;
    busy.clock = [&clock] { return clock.now; };
    for (int i = 0; i < 100; ++i) {
      busy.feed("x", (uint64_t)i * 20);
    }
    const uint64_t start = clock.now;
    ovHttpDrain(busy, clock, 50, 500);
    CHECK_EQ(clock.now - start, 500);
    // Also when it comes without a single pause, so that there is never a moment at which
    // nothing is waiting to be read. Nobody waits then, and the test clock only moves because
    // the connection moves it. A drain that looked at the time only between bursts would read
    // this one for ever; the connection gives up after a minute of it and says so.
    struct Stream : OvConn {
      ManualClock *clock;
      uint64_t reads = 0;
      int read() override {
        if (++reads % 1000 == 0 && ++clock->now > 60000) {  // a thousand bytes a millisecond
          throw std::runtime_error("the drain went on reading for a minute");
        }
        return 'x';
      }
      int write(const uint8_t *, size_t len) override { return (int)len; }
      void close() override {}
    };
    ManualClock streamClock;
    Stream stream;
    stream.clock = &streamClock;
    try {
      ovHttpDrain(stream, streamClock, 50, 500);
    } catch (const std::runtime_error &stuck) {
      ++gChecks;
      reportFailure(__FILE__, __LINE__, "a stream without pauses is let go after the longest time", stuck.what());
    }
    CHECK_EQ(streamClock.now, 500);
    CHECK(stream.reads >= 500000 && stream.reads < 501000);
    // The same across the wrap of the 32-bit clock, which this test clock reaches after 4.096 s.
    streamClock.now = 3800;
    stream.reads = 0;
    try {
      ovHttpDrain(stream, streamClock, 50, 500);
    } catch (const std::runtime_error &stuck) {
      ++gChecks;
      reportFailure(__FILE__, __LINE__, "a stream without pauses is let go across the clock's wrap", stuck.what());
    }
    CHECK_EQ(streamClock.now, 4300);
    // A longest time of nothing reads nothing.
    stream.reads = 0;
    ovHttpDrain(stream, streamClock, 50, 0);
    CHECK_EQ(stream.reads, 0);
  }
}

static void testHttpHelpers() {
  // ovSameSecret
  CHECK(ovSameSecret("1234", "1234"));
  CHECK(!ovSameSecret("1234", "1235"));
  CHECK(!ovSameSecret("1234", "123"));
  CHECK(!ovSameSecret("1234", "12345"));
  CHECK(!ovSameSecret("1234", ""));
  CHECK(!ovSameSecret("", ""));       // no secret is set: nothing matches it
  CHECK(!ovSameSecret("", "1234"));
  CHECK(!ovSameSecret("abcd", "ABCD"));
  CHECK(ovSameSecret("0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcdef"));
  CHECK(!ovSameSecret("0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcdee"));

  // ovQueryValue
  char value[8];
  CHECK(ovQueryValue("groups=live&auto=1", "auto", value, sizeof(value)) && strcmp(value, "1") == 0);
  CHECK(ovQueryValue("groups=live&auto=1", "groups", value, sizeof(value)) && strcmp(value, "live") == 0);
  CHECK(ovQueryValue("a=&b=2", "a", value, sizeof(value)) && value[0] == '\0');
  CHECK(ovQueryValue("a=1&a=2", "a", value, sizeof(value)) && strcmp(value, "1") == 0);
  CHECK(ovQueryValue("x=a=b", "x", value, sizeof(value)) && strcmp(value, "a=b") == 0);
  CHECK(!ovQueryValue("groups=live&auto=1", "aut", value, sizeof(value)));
  CHECK(!ovQueryValue("groups=live&xauto=1", "auto", value, sizeof(value)));
  CHECK(!ovQueryValue("groups=live&auto", "auto", value, sizeof(value)));
  CHECK(!ovQueryValue("", "auto", value, sizeof(value)));
  CHECK(!ovQueryValue("&&", "auto", value, sizeof(value)));
  CHECK(ovQueryValue("&&auto=1", "auto", value, sizeof(value)));
  CHECK(ovQueryValue("a=1234567", "a", value, sizeof(value)) && strcmp(value, "1234567") == 0);
  strcpy(value, "kept");
  CHECK(!ovQueryValue("a=12345678", "a", value, sizeof(value)));  // does not fit: refused, not cut
  CHECK(strcmp(value, "kept") == 0);

  // Status codes
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_OK), 200);
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_BAD_REQUEST), 400);
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_LENGTH_REQUIRED), 411);
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_BODY_TOO_LARGE), 413);
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_TARGET_TOO_LONG), 414);
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_HEADERS_TOO_BIG), 431);
  CHECK_EQ(ovHttpStatusFor(OV_HTTP_NOT_IMPLEMENTED), 501);
  const int known[] = {200, 204, 400, 401, 403, 404, 405, 409, 411, 413, 414, 421, 429, 431, 501, 503};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
    CHECK_MSG(strcmp(ovHttpReason(known[i]), "Error") != 0, "no reason phrase for %d", known[i]);
  }
  CHECK(strcmp(ovHttpReason(418), "Error") == 0);

  // ovFormatTrimmed
  char text[40];
  CHECK(ovFormatTrimmed(100.0, 4, text, sizeof(text)) == 3 && strcmp(text, "100") == 0);
  CHECK(ovFormatTrimmed(96.667, 4, text, sizeof(text)) == 6 && strcmp(text, "96.667") == 0);
  CHECK(ovFormatTrimmed(13.1, 2, text, sizeof(text)) == 4 && strcmp(text, "13.1") == 0);
  CHECK(ovFormatTrimmed(0.0, 3, text, sizeof(text)) == 1 && strcmp(text, "0") == 0);
  CHECK(ovFormatTrimmed(-0.5, 3, text, sizeof(text)) == 4 && strcmp(text, "-0.5") == 0);
  CHECK(ovFormatTrimmed(1200.0, 0, text, sizeof(text)) == 4 && strcmp(text, "1200") == 0);  // no point: zeros stay
  CHECK(ovFormatTrimmed(0.000001, 6, text, sizeof(text)) == 8 && strcmp(text, "0.000001") == 0);
}

// ============================================================================
// The answer writer and the JSON writer
// ============================================================================
// The two limits on sending, as the README and the header of OptaViewHttp.h state them.
static_assert(OV_HTTP_SEND_STALL_MS == 5000 && OV_HTTP_SEND_MS == 30000, "the limits on sending an answer");

static void testHttpResponse() {
  ManualClock clock;
  {
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    OvHttpResponse response;
    CHECK(!response.attached());
    response.print("into nothing");  // not attached: dropped
    CHECK(!response.pending());
    response.attach(conn, clock);
    CHECK(response.attached());
    CHECK(!response.begun());
    response.begin(200, "application/json", "X-Extra: 1\r\n", 2);
    CHECK(response.begun());
    response.print("{}");
    CHECK(response.pending());
    CHECK(conn.sent.empty());  // collected, not sent byte by byte
    CHECK(response.flush());
    CHECK(!response.pending());
    CHECK(conn.sent ==
          "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: 2\r\n"
          "Cache-Control: no-store\r\n"
          "X-Content-Type-Options: nosniff\r\n"
          "X-Frame-Options: DENY\r\n"
          "Referrer-Policy: no-referrer\r\n"
          "Connection: close\r\n"
          "X-Extra: 1\r\n"
          "\r\n"
          "{}");
    CHECK_EQ(response.sent(), conn.sent.size());
    CHECK_EQ(conn.writes, 1);
    CHECK(response.flush());  // nothing to send is not a failure
    CHECK_EQ(conn.writes, 1);

    // Without a type, a length or extra headers.
    FakeConn bare;
    bare.clock = conn.clock;
    response.attach(bare, clock);
    CHECK(!response.begun());
    CHECK_EQ(response.sent(), 0);
    response.begin(404, nullptr);
    response.flush();
    CHECK(bare.sent ==
          "HTTP/1.1 404 Not Found\r\n"
          "Cache-Control: no-store\r\n"
          "X-Content-Type-Options: nosniff\r\n"
          "X-Frame-Options: DENY\r\n"
          "Referrer-Policy: no-referrer\r\n"
          "Connection: close\r\n"
          "\r\n");
    response.detach();
    CHECK(!response.attached());
    response.print("after");
    response.flush();
    CHECK_EQ(bare.sent.size(), response.sent());
  }
  {
    // A long answer goes out in pieces of the buffer's size, in order and complete.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    OvHttpResponse response;
    response.attach(conn, clock);
    std::string big;
    for (int i = 0; i < 5000; ++i) {
      big += (char)('a' + i % 26);
    }
    response.write(big.data(), 100);
    response.write(big.data() + 100, 4900);
    CHECK_EQ(conn.writes, 4);
    CHECK_EQ(conn.sent.size(), 4096);
    CHECK(response.pending());
    CHECK(response.flush());
    CHECK(conn.sent == big);
    CHECK_EQ(response.sent(), 5000);
    // Exactly one buffer: sent when it is full, with nothing left over.
    FakeConn exact;
    exact.clock = conn.clock;
    response.attach(exact, clock);
    response.write(big.data(), 1024);
    CHECK_EQ(exact.writes, 1);
    CHECK(!response.pending());
  }
  {
    // Once a write has failed the other end is gone: nothing more is sent, and it says so.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.failAfterBytes = 1500;
    OvHttpResponse response;
    response.attach(conn, clock);
    const std::string big(5000, 'x');
    response.write(big.data(), big.size());
    CHECK(response.failed());
    CHECK(!response.flush());
    CHECK_EQ(conn.sent.size(), 1024);
    CHECK_EQ(response.sent(), 1024);
    response.print("more");
    CHECK(!response.pending());
    CHECK(!response.flush());
    CHECK_EQ(conn.writes, 2);
    CHECK_EQ(conn.writesAfterFailure, 0);
    // A new answer on a new connection starts clean.
    FakeConn next;
    next.clock = conn.clock;
    response.attach(next, clock);
    CHECK(!response.failed());
    response.print("ok");
    CHECK(response.flush());
    CHECK(next.sent == "ok");
  }

  // --- The other end does not take everything at once ---
  std::string big;
  for (int i = 0; i < 5000; ++i) {
    big += (char)('A' + i % 26);
  }
  {
    // It takes a few bytes at a time: everything arrives, once and in order, and no time passes.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.takeAtMost = 100;
    OvHttpResponse response;
    response.attach(conn, clock);
    const uint64_t start = clock.now;
    response.write(big.data(), big.size());
    CHECK(response.flush());
    CHECK(conn.sent == big);
    CHECK_EQ(response.sent(), 5000);
    CHECK_EQ(conn.writes, 54);  // four full buffers in eleven pieces each, and the rest in ten
    CHECK_EQ(clock.now - start, 0);
  }
  {
    // It takes nothing for a while and then carries on: the answer waits for it, and is whole.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.deafUntil = clock.now + OV_HTTP_SEND_STALL_MS - 1;
    OvHttpResponse response;
    response.attach(conn, clock);
    const uint64_t start = clock.now;
    response.write(big.data(), big.size());
    CHECK(response.flush());
    CHECK(conn.sent == big);
    CHECK_EQ(clock.now - start, OV_HTTP_SEND_STALL_MS - 1);
  }
  {
    // It takes nothing for five seconds: it is gone. The time counts from the last byte it took,
    // nothing is tried after that, and what was left is dropped.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.takeAtMost = 300;
    conn.takeEveryMs = 1000;
    conn.deafFrom = clock.now + 2500;  // it takes 300 bytes at 0, 1 and 2 s, and then no more
    OvHttpResponse response;
    response.attach(conn, clock);
    const uint64_t start = clock.now;
    response.write(big.data(), big.size());
    CHECK(response.failed());
    CHECK_EQ(clock.now - start, 2000 + OV_HTTP_SEND_STALL_MS);
    CHECK(conn.sent == big.substr(0, 900));
    CHECK_EQ(response.sent(), 900);
    const unsigned writes = conn.writes;
    CHECK(!response.flush());
    response.print("more");
    CHECK(!response.pending());
    CHECK(!response.flush());
    CHECK_EQ(conn.writes, writes);
    CHECK_EQ(clock.now - start, 2000 + OV_HTTP_SEND_STALL_MS);
  }
  {
    // It never stalls, but it takes a byte a second. Thirty seconds is all an answer gets.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.takeAtMost = 1;
    conn.takeEveryMs = 1000;
    OvHttpResponse response;
    response.attach(conn, clock);
    const uint64_t start = clock.now;
    response.write(big.data(), big.size());
    CHECK(response.failed());
    CHECK_EQ(clock.now - start, OV_HTTP_SEND_MS);
    CHECK(conn.sent == big.substr(0, 30));  // at 0 s, 1 s ... 29 s
    CHECK_EQ(response.sent(), 30);
    const unsigned writes = conn.writes;
    CHECK(!response.flush());
    response.print("more");
    CHECK(!response.flush());
    CHECK_EQ(conn.writes, writes);
    CHECK_EQ(clock.now - start, OV_HTTP_SEND_MS);
  }
  {
    // The thirty seconds are those spent sending, added up over the pieces of one answer. Time
    // in which nothing is waiting to go out, as while a command works, is not part of them.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.takeAtMost = 100;
    conn.takeEveryMs = 1000;
    OvHttpResponse response;
    response.attach(conn, clock);
    const uint64_t start = clock.now;
    response.write(big.data(), 1000);
    CHECK(response.flush());                  // 100 bytes at 0 s, 1 s ... 9 s
    CHECK_EQ(clock.now - start, 9000);
    clock.now += 100000;                      // a long command works, and nothing is sent
    response.write(big.data() + 1000, 1000);
    CHECK(response.flush());                  // nine seconds more
    CHECK_EQ(clock.now - start, 118000);
    clock.now += 100000;
    response.write(big.data() + 2000, 1000);
    CHECK(response.flush());                  // and nine more, which makes 27 s of sending
    CHECK_EQ(clock.now - start, 227000);
    CHECK(conn.sent == big.substr(0, 3000));
    clock.now += 100000;
    response.write(big.data() + 3000, 1000);  // three seconds are left: 100 bytes at 0 s, 1 s and 2 s
    CHECK(!response.flush());
    CHECK(response.failed());
    CHECK_EQ(clock.now - start, 330000);
    CHECK(conn.sent == big.substr(0, 3300));
    CHECK_EQ(response.sent(), 3300);
    // The next answer has thirty seconds of its own.
    FakeConn next;
    next.clock = conn.clock;
    next.takeAtMost = 100;
    next.takeEveryMs = 1000;
    response.attach(next, clock);
    const uint64_t again = clock.now;
    response.write(big.data(), 1000);
    CHECK(response.flush());
    CHECK(!response.failed());
    CHECK_EQ(clock.now - again, 9000);
    CHECK(next.sent == big.substr(0, 1000));
  }
  {
    // When the other end is known to be gone, nothing waits for it.
    FakeConn conn;
    conn.clock = [&clock] { return clock.now; };
    conn.takeAtMost = 10;
    conn.goneAt = clock.now;
    OvHttpResponse response;
    response.attach(conn, clock);
    const uint64_t start = clock.now;
    response.print("hello");
    CHECK(!response.flush());
    CHECK_EQ(clock.now - start, 0);
    CHECK(conn.sent.empty());
    // Neither is one that claims to have taken more than it was given.
    struct Liar : OvConn {
      unsigned writes = 0;
      int read() override { return -1; }
      int write(const uint8_t *, size_t len) override {
        ++writes;
        return (int)len + 1;
      }
      void close() override {}
    };
    Liar liar;
    response.attach(liar, clock);
    response.print("hello");
    CHECK(!response.flush());
    CHECK(response.failed());
    CHECK_EQ(response.sent(), 0);
    CHECK_EQ(liar.writes, 1);
  }
}

static void testJsonWriter() {
  {
    StringOut out;
    OvJson json(out);
    json.beginObject();
    json.str("s", "text");
    json.u("u", 4294967295UL);
    json.i("i", -2147483647 - 1);
    json.flag("t", true);
    json.flag("f", false);
    json.null("n");
    json.num("d", 13.10, 2);
    json.beginArray("a");
    json.u(nullptr, 1);
    json.str(nullptr, "two");
    json.beginObject();
    json.str("k", "v");
    json.endObject();
    json.beginArray();
    json.endArray();
    json.endArray();
    json.beginObject("o");
    json.endObject();
    json.str("last", nullptr);
    json.endObject();
    CHECK_MSG(out.text ==
                  "{\"s\":\"text\",\"u\":4294967295,\"i\":-2147483648,\"t\":true,\"f\":false,\"n\":null,\"d\":13.1,"
                  "\"a\":[1,\"two\",{\"k\":\"v\"},[]],\"o\":{},\"last\":\"\"}",
              "%s", out.text.c_str());
    JsonDocument doc;
    CHECK(!deserializeJson(doc, out.text));
    CHECK_EQ(doc["a"][2]["k"].as<std::string>() == "v", 1);
  }
  {
    // Everything a text can hold comes out as valid JSON and reads back the same.
    std::string all;
    for (int c = 1; c < 256; ++c) {
      all += (char)c;
    }
    StringOut out;
    OvJson json(out);
    json.beginArray();
    json.str(nullptr, all.c_str());
    json.str(nullptr, "quote \" backslash \\ slash / tab \t newline \n");
    json.endArray();
    for (size_t i = 0; i < out.text.size(); ++i) {
      CHECK_MSG((unsigned char)out.text[i] >= 0x20, "a raw control character at %u", (unsigned)i);
    }
    CHECK(has(out.text, "\\u0001"));
    CHECK(has(out.text, "\\u001F"));
    CHECK(has(out.text, "quote \\\" backslash \\\\ slash / tab \\u0009 newline \\u000A"));
    JsonDocument doc;
    CHECK(!deserializeJson(doc, out.text));
    // ArduinoJson reads "\u0080" and up as UTF-8, so compare the part that is plain ASCII.
    CHECK(doc[0].as<std::string>().substr(0, 127) == all.substr(0, 127));
    CHECK(doc[1].as<std::string>() == "quote \" backslash \\ slash / tab \t newline \n");
  }
  {
    // Numbers: no trailing zeros, and nothing that JSON cannot say.
    StringOut out;
    OvJson json(out);
    json.beginArray();
    json.num(nullptr, 100.0, 4);
    json.num(nullptr, 96.667, 4);
    json.num(nullptr, -0.25, 6);
    json.num(nullptr, 0.0, 6);
    json.num(nullptr, std::numeric_limits<double>::quiet_NaN(), 2);
    json.num(nullptr, std::numeric_limits<double>::infinity(), 2);
    json.num(nullptr, -std::numeric_limits<double>::infinity(), 2);
    json.num(nullptr, 1e300, 2);
    json.endArray();
    JsonDocument doc;
    CHECK_MSG(!deserializeJson(doc, out.text), "%s", out.text.c_str());
    CHECK(has(out.text, "[100,96.667,-0.25,0,"));
    CHECK(doc[4].isNull());
    CHECK(doc[5].isNull());
    CHECK(doc[6].isNull());
    CHECK(doc[7].isNull() || doc[7].is<double>());
  }
  {
    // Deeper than it keeps count of: it must not write outside its bookkeeping.
    StringOut out;
    OvJson json(out);
    for (int i = 0; i < 40; ++i) {
      json.beginArray();
    }
    for (int i = 0; i < 40; ++i) {
      json.endArray();
    }
    CHECK_EQ(countOf(out.text, "["), 40);
    CHECK_EQ(countOf(out.text, "]"), 40);
  }
}

// ============================================================================
// Network addresses
// ============================================================================
static bool parsesTo(const char *text, int a, int b, int c, int d) {
  OvIp4 ip = ovIp4(9, 9, 9, 9);
  return ovParseIp4(text, &ip) && ip.b[0] == a && ip.b[1] == b && ip.b[2] == c && ip.b[3] == d;
}

static void testIpHelpers() {
  CHECK(parsesTo("192.168.1.16", 192, 168, 1, 16));
  CHECK(parsesTo("0.0.0.0", 0, 0, 0, 0));
  CHECK(parsesTo("255.255.255.255", 255, 255, 255, 255));
  CHECK(parsesTo("10.0.0.1", 10, 0, 0, 1));
  const char *bad[] = {"",         "1.2.3",     "1.2.3.4.5", "1.2.3.",     ".1.2.3",       "1..2.3",   "256.1.1.1",
                       "1.2.3.256", "1.2.3.4 ",  " 1.2.3.4",  "1.2.3.-4",   "+1.2.3.4",     "1.2.3.4a", "a.b.c.d",
                       "01.2.3.4", "1.2.3.04",  "1.2.3.0x4", "1.2.3.1000", "192.168.1.16/24", "1,2,3,4", "::1",
                       "1.2.3.4.", "0000.0.0.0", "1.2.3.4\n"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    OvIp4 ip;
    CHECK_MSG(!ovParseIp4(bad[i], &ip), "'%s' was read as an address", bad[i]);
  }
  OvIp4 none;
  CHECK(!ovParseIp4(nullptr, &none));

  char text[20];
  CHECK(ovFormatIp4(ovIp4(192, 168, 1, 16), text, sizeof(text)) == 12 && strcmp(text, "192.168.1.16") == 0);
  CHECK(ovFormatIp4(ovIp4(255, 255, 255, 255), text, sizeof(text)) == 15 && strcmp(text, "255.255.255.255") == 0);
  CHECK(ovFormatIp4(ovIp4(0, 0, 0, 0), text, sizeof(text)) == 7 && strcmp(text, "0.0.0.0") == 0);
  CHECK(ovIp4Equal(ovIp4(1, 2, 3, 4), ovIp4(1, 2, 3, 4)));
  CHECK(!ovIp4Equal(ovIp4(1, 2, 3, 4), ovIp4(1, 2, 3, 5)));
  CHECK(ovIp4IsZero(ovIp4(0, 0, 0, 0)));
  CHECK(!ovIp4IsZero(ovIp4(0, 0, 1, 0)));
  CHECK_EQ(ovIp4Bits(ovIp4(192, 168, 1, 16)), 0xC0A80110UL);

  CHECK(ovIsNetmask(ovIp4(255, 255, 255, 0)));
  CHECK(ovIsNetmask(ovIp4(255, 0, 0, 0)));
  CHECK(ovIsNetmask(ovIp4(255, 255, 255, 252)));
  CHECK(ovIsNetmask(ovIp4(255, 255, 254, 0)));
  CHECK(!ovIsNetmask(ovIp4(255, 255, 255, 254)));  // /31 and /32 leave no room for a device
  CHECK(!ovIsNetmask(ovIp4(255, 255, 255, 255)));
  CHECK(!ovIsNetmask(ovIp4(254, 0, 0, 0)));        // wider than /8
  CHECK(!ovIsNetmask(ovIp4(0, 0, 0, 0)));
  CHECK(!ovIsNetmask(ovIp4(255, 0, 255, 0)));
  CHECK(!ovIsNetmask(ovIp4(255, 255, 255, 1)));
  CHECK(!ovIsNetmask(ovIp4(192, 168, 1, 1)));

  const OvIp4 mask24 = ovIp4(255, 255, 255, 0);
  CHECK(ovIsHostAddress(ovIp4(192, 168, 1, 16), mask24));
  CHECK(ovIsHostAddress(ovIp4(192, 168, 1, 1), mask24));
  CHECK(ovIsHostAddress(ovIp4(192, 168, 1, 254), mask24));
  CHECK(!ovIsHostAddress(ovIp4(192, 168, 1, 0), mask24));    // the network
  CHECK(!ovIsHostAddress(ovIp4(192, 168, 1, 255), mask24));  // its broadcast
  CHECK(!ovIsHostAddress(ovIp4(0, 1, 2, 3), mask24));
  CHECK(!ovIsHostAddress(ovIp4(127, 0, 0, 1), mask24));
  CHECK(!ovIsHostAddress(ovIp4(224, 0, 0, 5), mask24));
  CHECK(!ovIsHostAddress(ovIp4(255, 255, 255, 255), mask24));
  CHECK(ovIsHostAddress(ovIp4(10, 0, 1, 0), ovIp4(255, 255, 0, 0)));  // .0 is fine in a /16
  CHECK(ovIsHostAddress(ovIp4(10, 0, 0, 255), ovIp4(255, 255, 0, 0)));
  CHECK(ovSameSubnet(ovIp4(192, 168, 1, 16), ovIp4(192, 168, 1, 1), mask24));
  CHECK(!ovSameSubnet(ovIp4(192, 168, 1, 16), ovIp4(192, 168, 2, 1), mask24));

  OvNetConfig config;
  config.mode = OV_NET_AUTO;
  config.ip = ovIp4(192, 168, 1, 16);
  config.mask = mask24;
  config.gateway = ovIp4(192, 168, 1, 1);
  CHECK(ovNetConfigProblem(config) == nullptr);
  config.gateway = ovIp4(0, 0, 0, 0);
  CHECK(ovNetConfigProblem(config) == nullptr);  // no gateway is allowed
  config.mode = 3;
  CHECK_HAS(ovNetConfigProblem(config), "auto, dhcp or static");
  config.mode = OV_NET_STATIC;
  config.mask = ovIp4(255, 0, 255, 0);
  CHECK_HAS(ovNetConfigProblem(config), "not a subnet mask");
  config.mask = mask24;
  config.ip = ovIp4(192, 168, 1, 255);
  CHECK_HAS(ovNetConfigProblem(config), "cannot be given to a device");
  config.ip = ovIp4(192, 168, 1, 16);
  config.gateway = ovIp4(192, 168, 2, 1);
  CHECK_HAS(ovNetConfigProblem(config), "gateway");
  config.gateway = ovIp4(192, 168, 1, 16);
  CHECK_HAS(ovNetConfigProblem(config), "gateway");
  config.gateway = ovIp4(192, 168, 1, 255);
  CHECK_HAS(ovNetConfigProblem(config), "gateway");
  // The settings of a mode that is not in use still have to make sense: they are the fallback.
  config.mode = OV_NET_DHCP;
  CHECK_HAS(ovNetConfigProblem(config), "gateway");

  // The gateway that is assumed when none is named.
  CHECK(ovIp4Equal(ovUsualGateway(ovIp4(192, 168, 1, 16), mask24), ovIp4(192, 168, 1, 1)));
  CHECK(ovIp4IsZero(ovUsualGateway(ovIp4(192, 168, 1, 1), mask24)));  // the Opta has that address itself
  CHECK(ovIp4Equal(ovUsualGateway(ovIp4(10, 20, 30, 40), ovIp4(255, 255, 0, 0)), ovIp4(10, 20, 0, 1)));
  CHECK(ovIp4Equal(ovUsualGateway(ovIp4(10, 20, 30, 42), ovIp4(255, 255, 255, 252)), ovIp4(10, 20, 30, 41)));
  CHECK(ovIp4IsZero(ovUsualGateway(ovIp4(10, 20, 30, 41), ovIp4(255, 255, 255, 252))));
  CHECK(ovIp4Equal(ovUsualGateway(ovIp4(172, 16, 9, 200), ovIp4(255, 255, 254, 0)), ovIp4(172, 16, 8, 1)));

  // Words in any mix of cases, and the three modes by name.
  CHECK(ovIsWord("auto", "auto") && ovIsWord("AUTO", "auto") && ovIsWord("aUtO", "auto"));
  CHECK(!ovIsWord("aut", "auto") && !ovIsWord("autos", "auto") && !ovIsWord("", "auto"));
  CHECK(!ovIsWord(nullptr, "auto") && !ovIsWord(" auto", "auto") && !ovIsWord("auto ", "auto"));
  CHECK(!ovIsWord("auto", "aut") && !ovIsWord("`uto", "auto") && !ovIsWord("{uto", "auto"));
  CHECK(ovIsWord("", "") && !ovIsWord("a", ""));
  uint8_t mode = 9;
  CHECK(ovNetModeFromName("dhcp", &mode) && mode == OV_NET_DHCP);
  CHECK(ovNetModeFromName("STATIC", &mode) && mode == OV_NET_STATIC);
  CHECK(ovNetModeFromName("Auto", &mode) && mode == OV_NET_AUTO);
  mode = 9;
  CHECK(!ovNetModeFromName("fixed", &mode) && !ovNetModeFromName("", &mode) && !ovNetModeFromName(nullptr, &mode));
  CHECK(!ovNetModeFromName("static ", &mode) && !ovNetModeFromName("dhcpd", &mode) && !ovNetModeFromName("0", &mode));
  CHECK_EQ(mode, 9);
  CHECK(ovIsNetModeName("auto") && ovIsNetModeName("Dhcp") && ovIsNetModeName("static") && !ovIsNetModeName("manual"));

  // The settings as OptaViewConfig.h spells them.
  const OvNetConfig untouched = {7, ovIp4(1, 2, 3, 4), ovIp4(5, 6, 7, 8), ovIp4(9, 10, 11, 12)};
  const auto same = [](const OvNetConfig &a, const OvNetConfig &b) {
    return a.mode == b.mode && ovIp4Equal(a.ip, b.ip) && ovIp4Equal(a.mask, b.mask) && ovIp4Equal(a.gateway, b.gateway);
  };
  OvNetConfig got = untouched;
  CHECK(ovNetConfigFromText("static", "10.20.30.40", "255.255.0.0", "", &got) == nullptr);
  CHECK(same(got, OvNetConfig{OV_NET_STATIC, ovIp4(10, 20, 30, 40), ovIp4(255, 255, 0, 0), ovIp4(0, 0, 0, 0)}));
  got = untouched;
  CHECK(ovNetConfigFromText("AUTO", "10.20.30.40", "255.255.0.0", nullptr, &got) == nullptr);
  CHECK(same(got, OvNetConfig{OV_NET_AUTO, ovIp4(10, 20, 30, 40), ovIp4(255, 255, 0, 0), ovIp4(10, 20, 0, 1)}));
  got = untouched;
  CHECK(ovNetConfigFromText("dhcp", "192.168.5.20", "255.255.255.0", "192.168.5.254", &got) == nullptr);
  CHECK(same(got, OvNetConfig{OV_NET_DHCP, ovIp4(192, 168, 5, 20), ovIp4(255, 255, 255, 0), ovIp4(192, 168, 5, 254)}));
  // An Opta at the first address of its network has no gateway unless one is named.
  got = untouched;
  CHECK(ovNetConfigFromText("static", "192.168.5.1", "255.255.255.0", nullptr, &got) == nullptr);
  CHECK(same(got, OvNetConfig{OV_NET_STATIC, ovIp4(192, 168, 5, 1), ovIp4(255, 255, 255, 0), ovIp4(0, 0, 0, 0)}));
  const struct {
    const char *mode;
    const char *ip;
    const char *mask;
    const char *gateway;
    const char *problem;
  } refused[] = {
      {"fixed", "192.168.1.16", "255.255.255.0", "", "auto, dhcp or static"},
      {"", "192.168.1.16", "255.255.255.0", "", "auto, dhcp or static"},
      {nullptr, "192.168.1.16", "255.255.255.0", "", "auto, dhcp or static"},
      {"static", "192.168.1.300", "255.255.255.0", "", "four numbers"},
      {"static", "192.168.1", "255.255.255.0", "", "four numbers"},
      {"static", nullptr, "255.255.255.0", "", "four numbers"},
      {"static", "192.168.1.16", "24", "", "four numbers"},
      {"static", "192.168.1.16", nullptr, "", "four numbers"},
      {"static", "192.168.1.16", "255.255.255.0", "none", "four numbers"},
      {"static", "192.168.1.16", "255.255.255.0", " ", "four numbers"},
      {"static", "192.168.1.16", "255.255.255.255", "", "not a subnet mask"},
      {"static", "192.168.1.16", "255.0.255.0", nullptr, "not a subnet mask"},
      {"static", "192.168.1.255", "255.255.255.0", "", "cannot be given to a device"},
      {"static", "192.168.1.0", "255.255.255.0", nullptr, "cannot be given to a device"},
      {"static", "192.168.5.20", "255.255.255.0", "192.168.1.1", "gateway"},
      {"auto", "192.168.5.20", "255.255.255.0", "192.168.5.20", "gateway"},
      {"dhcp", "192.168.5.20", "255.255.255.0", "0.0.0.1", "gateway"},
  };
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
    got = untouched;
    const char *problem =
        ovNetConfigFromText(refused[i].mode, refused[i].ip, refused[i].mask, refused[i].gateway, &got);
    CHECK_MSG(problem != nullptr && strstr(problem, refused[i].problem) != nullptr, "case %u: %s", (unsigned)i,
              problem == nullptr ? "(accepted)" : problem);
    CHECK_MSG(same(got, untouched), "case %u changed the configuration it refused", (unsigned)i);
    const char *again = ovNetConfigTextProblem(refused[i].mode, refused[i].ip, refused[i].mask, refused[i].gateway);
    CHECK_MSG(again == problem, "case %u: the check without a configuration says something else", (unsigned)i);
  }
  CHECK(ovNetConfigTextProblem("auto", "192.168.1.16", "255.255.255.0", nullptr) == nullptr);
}

// What the sketch checks while it is compiled has to work while it is compiled.
static_assert(ovNetConfigTextProblem("auto", "192.168.1.16", "255.255.255.0", nullptr) == nullptr,
              "the built-in settings");
static_assert(ovNetConfigTextProblem("Static", "10.20.30.40", "255.255.0.0", "") == nullptr, "no gateway");
static_assert(ovNetConfigTextProblem("dhcp", "192.168.5.20", "255.255.255.0", "192.168.5.1") == nullptr, "a gateway");
static_assert(ovNetConfigTextProblem("static", "192.168.5.20", "255.255.255.0", "192.168.1.1") != nullptr,
              "a gateway on another network");
static_assert(ovNetConfigTextProblem("static", "192.168.1.300", "255.255.255.0", "") != nullptr, "not an address");
static_assert(ovNetConfigTextProblem("static", "192.168.1.16", "255.255.255.255", "") != nullptr, "not a mask");
static_assert(ovNetConfigTextProblem("fixed", "192.168.1.16", "255.255.255.0", "") != nullptr, "not a mode");
static_assert(ovIsNetModeName("auto") && ovIsNetModeName("DHCP") && ovIsNetModeName("static"), "the modes");
static_assert(!ovIsNetModeName("fixed") && !ovIsNetModeName("") && !ovIsNetModeName(nullptr), "not modes");
static_assert(ovIp4Bits(ovUsualGateway(ovIp4(192, 168, 1, 16), ovIp4(255, 255, 255, 0))) == 0xC0A80101UL,
              "the usual gateway");
static_assert(ovIsPin("2468") && ovIsPin("0000") && ovIsPin("13572468"), "PINs");
static_assert(!ovIsPin("123") && !ovIsPin("123456789") && !ovIsPin("12a4") && !ovIsPin("") && !ovIsPin(nullptr),
              "not PINs");
static_assert(!ovIsPin(" 2468") && !ovIsPin("2468 ") && !ovIsPin("-2468") && !ovIsPin("24.68"), "not PINs either");

// The Opta's Ethernet port and what is at the other end of the cable, as far as the address
// policy can tell. It follows what the Arduino core and mbed OS do (see OptaViewNet.h),
// including the DHCP attempt that mbed starts late on its own.
struct FakeEthernet : OvNetDriver {
  ManualClock &clock;
  // The world outside.
  bool cable = false;
  bool dhcpServer = false;
  uint32_t dhcpDelayMs = 1500;
  OvIp4 lease = ovIp4(192, 168, 1, 57);
  bool lateDhcp = true;   // mbed: a DHCP start that never saw a link is made up for later
  bool stuckOnLease = false;  // the lease wins over a fixed address every time (worse than mbed)
  std::string mac = "A8:61:0A:50:12:34";
  // The port.
  enum Mode { OFF, DHCP, STATIC } mode = OFF;
  OvIp4 fixed = ovIp4(0, 0, 0, 0);
  OvIp4 fixedMask = ovIp4(0, 0, 0, 0);
  OvIp4 fixedGateway = ovIp4(0, 0, 0, 0);
  uint64_t linkSince = kNever;  // when the link came up in this mode
  bool owed = false;            // a DHCP start is still owed (see lateDhcp)
  bool overlay = false;         // ...and is now running on top of the fixed address
  std::string log;
  unsigned startsWithoutStop = 0;
  unsigned stops = 0;

  explicit FakeEthernet(ManualClock &c) : clock(c) {}

  void plug(bool in) {
    track();
    cable = in;
    track();
  }

  void startDhcp() override {
    if (mode != OFF) {
      ++startsWithoutStop;
    }
    log += "dhcp;";
    mode = DHCP;
    linkSince = kNever;
    track();
  }
  void startStatic(const OvIp4 &ip, const OvIp4 &mask, const OvIp4 &gateway) override {
    if (mode != OFF) {
      ++startsWithoutStop;
    }
    char a[20];
    char m[20];
    char g[20];
    ovFormatIp4(ip, a, sizeof(a));
    ovFormatIp4(mask, m, sizeof(m));
    ovFormatIp4(gateway, g, sizeof(g));
    log += std::string("static ") + a + "/" + m + " gw " + g + ";";
    mode = STATIC;
    fixed = ip;
    fixedMask = mask;
    fixedGateway = gateway;
    linkSince = kNever;
    track();
  }
  void stop() override {
    track();
    ++stops;
    log += "stop;";
    if (mode == DHCP && linkSince == kNever && lateDhcp) {
      owed = true;  // it never got to send anything
    }
    mode = OFF;
    overlay = false;
    linkSince = kNever;
  }
  bool up() override {
    track();
    return !ovIp4IsZero(address());
  }
  OvIp4 address() override {
    track();
    if (mode == OFF || linkSince == kNever) {
      return ovIp4(0, 0, 0, 0);
    }
    const uint64_t linked = clock.now - linkSince;
    const bool leased = dhcpServer && linked >= dhcpDelayMs;
    if (mode == DHCP) {
      return leased ? lease : ovIp4(0, 0, 0, 0);
    }
    if ((overlay || stuckOnLease) && leased) {
      return lease;
    }
    return linked >= 50 ? fixed : ovIp4(0, 0, 0, 0);
  }
  void hardwareAddress(char *out, size_t cap) override { ovCopy(out, cap, mac.c_str()); }

 private:
  // Notices the link coming up or going away.
  void track() {
    if (mode == OFF) {
      return;
    }
    if (cable && linkSince == kNever) {
      linkSince = clock.now;
      if (mode == STATIC && owed) {
        owed = false;
        overlay = true;
      }
      if (mode == DHCP) {
        owed = false;
      }
    } else if (!cable && linkSince != kNever) {
      linkSince = kNever;
    }
  }
};

struct NetRig {
  ManualClock clock;
  FakeEthernet port;
  OvNet net;
  StringOut out;
  unsigned changes = 0;
  NetRig() : port(clock), net(port, clock) {}

  OvNetConfig config(uint8_t mode) const {
    OvNetConfig c;
    c.mode = mode;
    c.ip = ovIp4(192, 168, 1, 16);
    c.mask = ovIp4(255, 255, 255, 0);
    c.gateway = ovIp4(192, 168, 1, 1);
    return c;
  }
  // Lets `ms` pass, with loop() running once a millisecond.
  void run(uint64_t ms) {
    for (uint64_t i = 0; i < ms; ++i) {
      net.poll();
      if (net.takeChange()) {
        ++changes;
      }
      ++clock.now;
    }
    net.poll();
    if (net.takeChange()) {
      ++changes;
    }
  }
  std::string status() {
    out.text.clear();
    net.status(out);
    return out.text;
  }
  std::string address() {
    char text[20];
    ovFormatIp4(net.address(), text, sizeof(text));
    return text;
  }
  std::string command(const std::string &line) {
    std::vector<std::string> words;
    size_t at = 0;
    while (at < line.size()) {
      const size_t end = line.find(' ', at);
      words.push_back(line.substr(at, end == std::string::npos ? std::string::npos : end - at));
      at = end == std::string::npos ? line.size() : end + 1;
    }
    std::vector<char *> argv;
    for (size_t i = 0; i < words.size(); ++i) {
      argv.push_back(&words[i][0]);
    }
    out.text.clear();
    const bool taken = net.command((int)argv.size(), argv.data(), out);
    return taken ? out.text : std::string("(not a net command)");
  }
};

static const char kStaticStart[] = "static 192.168.1.16/255.255.255.0 gw 192.168.1.1;";

static void testNetPolicy() {
  {
    // Before begin(), and with settings that make no sense, nothing is started.
    NetRig n;
    CHECK(n.status() == "Network   not started\r\n");
    n.run(100);
    CHECK(n.port.log.empty());
    CHECK(!n.net.up());
    OvNetConfig broken = n.config(OV_NET_STATIC);
    broken.mask = ovIp4(255, 0, 255, 0);
    CHECK(!n.net.begin(broken));
    n.run(100);
    CHECK(n.port.log.empty());
    CHECK(n.status() == "Network   not started\r\n");
    CHECK_EQ(n.changes, 0);
  }
  {
    // Auto, on a network with a DHCP server: the usual case.
    NetRig n;
    n.port.cable = true;
    n.port.dhcpServer = true;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    CHECK(n.port.log == "dhcp;");
    CHECK(!n.net.up());
    n.run(1000);
    CHECK(n.status() ==
          "Network   waiting for an address from DHCP, 1 s so far (after 20 s the fixed address 192.168.1.16 is used)\r\n");
    CHECK_EQ(n.changes, 0);
    n.run(1000);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.57");
    CHECK(n.status() == "Network   address 192.168.1.57 from DHCP\r\n");
    CHECK_EQ(n.changes, 1);
    n.run(120000);
    CHECK(n.port.log == "dhcp;");  // and it is left alone
    CHECK_EQ(n.changes, 1);

    // The cable comes out: the address is gone, and DHCP is waited for, for as long as it takes.
    // It answered on this network, so the fixed address is not the better guess any more.
    n.port.plug(false);
    n.run(10);
    CHECK(!n.net.up());
    CHECK(n.address() == "0.0.0.0");
    CHECK_EQ(n.changes, 2);
    CHECK(n.status() ==
          "Network   no link now, or no answer from DHCP (the address was 192.168.1.57). Is the cable in?\r\n");
    n.run(300000);
    CHECK(n.port.log == "dhcp;");
    n.port.plug(true);
    n.run(2000);
    CHECK(n.net.up());
    CHECK(n.status() == "Network   address 192.168.1.57 from DHCP\r\n");
    CHECK_EQ(n.changes, 3);

    // A new lease with another address is noticed within a second or so.
    n.port.lease = ovIp4(192, 168, 1, 99);
    n.run(1100);
    CHECK(n.address() == "192.168.1.99");
    CHECK_EQ(n.changes, 4);
    CHECK(n.port.log == "dhcp;");
    CHECK_EQ(n.port.startsWithoutStop, 0);
  }
  {
    // Auto, cable in, but nothing hands out addresses (a laptop on the other end): after twenty
    // seconds the fixed address is used.
    NetRig n;
    n.port.cable = true;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    n.run(19999);
    CHECK(n.port.log == "dhcp;");
    CHECK(has(n.status(), "waiting for an address from DHCP, 19 s so far"));
    n.run(1);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart);
    CHECK(!n.net.up());
    n.run(100);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.16");
    CHECK(n.status() == "Network   address 192.168.1.16, the fixed one: DHCP did not answer\r\n");
    CHECK_EQ(n.changes, 1);
    // The cable out and in again changes nothing about that.
    n.port.plug(false);
    n.run(5000);
    CHECK(!n.net.up());
    CHECK(n.status() == "Network   fixed address 192.168.1.16 (DHCP did not answer), but no link. Is the cable in?\r\n");
    n.port.plug(true);
    n.run(5000);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.16");
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart);
    CHECK_EQ(n.port.startsWithoutStop, 0);
  }
  {
    // Auto, switched on with no cable in. The first DHCP try was made on no network at all, so
    // when the cable goes in later, DHCP gets one more try on the network it leads to.
    NetRig n;
    n.port.dhcpServer = true;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    n.run(60000);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart);
    CHECK(!n.net.up());
    CHECK(n.status() == "Network   fixed address 192.168.1.16 (DHCP did not answer), but no link. Is the cable in?\r\n");
    n.port.plug(true);
    n.run(100);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;dhcp;");
    CHECK(!n.net.up());  // the fixed address was never announced as the one in use
    CHECK_EQ(n.changes, 0);
    n.run(2000);
    CHECK(n.net.up());
    CHECK(n.status() == "Network   address 192.168.1.57 from DHCP\r\n");
    CHECK_EQ(n.changes, 1);
    CHECK_EQ(n.port.startsWithoutStop, 0);
  }
  {
    // The same, but the network the cable leads to has no DHCP either: the fixed address after
    // another twenty seconds, and that was the one extra try.
    NetRig n;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    n.run(60000);
    n.port.plug(true);
    n.run(100);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;dhcp;");
    n.run(19000);
    CHECK(!n.net.up());
    n.run(1100);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;dhcp;stop;" + kStaticStart);
    CHECK(n.net.up());
    CHECK(n.status() == "Network   address 192.168.1.16, the fixed one: DHCP did not answer\r\n");
    n.port.plug(false);
    n.run(10000);
    n.port.plug(true);
    n.run(10000);
    CHECK(n.net.up());
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;dhcp;stop;" + kStaticStart);
    CHECK_EQ(n.port.startsWithoutStop, 0);
  }
  {
    // The cable goes in a moment after the switch to the fixed address, so no extra DHCP try is
    // made. mbed then starts DHCP by itself, on top of the fixed address, and takes the lease.
    // The policy sees that the address in use is not the one it set and goes with DHCP, which
    // is what auto mode wanted from the start.
    NetRig n;
    n.port.dhcpServer = true;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    n.run(21000);
    n.port.plug(true);
    n.run(200);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.16");
    CHECK_EQ(n.changes, 1);
    n.run(3000);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;dhcp;");
    n.run(2000);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.57");
    CHECK(n.status() == "Network   address 192.168.1.57 from DHCP\r\n");
    // From here on it is a DHCP network: no falling back when the cable comes out.
    n.port.plug(false);
    n.run(60000);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;dhcp;");
    CHECK(has(n.status(), "no link now, or no answer from DHCP (the address was 192.168.1.57)"));
    CHECK_EQ(n.port.startsWithoutStop, 0);
  }
  {
    // Without that habit of mbed's (lateDhcp off) the same course of events simply stays fixed.
    NetRig n;
    n.port.dhcpServer = true;
    n.port.lateDhcp = false;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    n.run(21000);
    n.port.plug(true);
    n.run(60000);
    CHECK(n.address() == "192.168.1.16");
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart);
  }
  {
    // DHCP only: it waits for ever and says for how long.
    NetRig n;
    CHECK(n.net.begin(n.config(OV_NET_DHCP)));
    n.run(600000);
    CHECK(n.port.log == "dhcp;");
    CHECK(n.status() == "Network   waiting for an address from DHCP, 600 s so far\r\n");
    n.port.dhcpServer = true;
    n.port.plug(true);
    n.run(2000);
    CHECK(n.status() == "Network   address 192.168.1.57 from DHCP\r\n");
  }
  {
    // Fixed only.
    NetRig n;
    n.port.dhcpServer = true;  // and it is not asked
    CHECK(n.net.begin(n.config(OV_NET_STATIC)));
    CHECK(n.port.log == kStaticStart);
    CHECK(n.status() == "Network   fixed address 192.168.1.16, but no link. Is the cable in?\r\n");
    n.run(30000);
    n.port.plug(true);
    n.run(100);
    CHECK(n.net.up());
    CHECK(n.status() == "Network   address 192.168.1.16, fixed\r\n");
    n.run(60000);
    CHECK(n.port.log == kStaticStart);
    CHECK_EQ(n.changes, 1);
  }
  {
    // Fixed, chosen after a DHCP start that never saw a link: mbed's late DHCP takes the lease.
    // A fixed address that was asked for by name is set again, and then it holds.
    NetRig n;
    n.port.dhcpServer = true;
    CHECK(n.net.begin(n.config(OV_NET_DHCP)));
    n.run(5000);
    CHECK(has(n.command("net static 192.168.1.16"), "fixed address 192.168.1.16, but no link"));
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart);
    n.port.plug(true);
    n.run(1000);
    CHECK(n.address() == "192.168.1.16");
    n.run(2000);
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;" + kStaticStart);
    n.run(60000);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.16");
    CHECK(n.status() == "Network   address 192.168.1.16, fixed\r\n");
    CHECK(n.port.log == std::string("dhcp;stop;") + kStaticStart + "stop;" + kStaticStart);
    CHECK_EQ(n.port.startsWithoutStop, 0);
  }
  {
    // If the port keeps coming back with another address, that is said, and it is not fought
    // over for ever: two more tries, then the address that is really in use is the one shown.
    NetRig n;
    n.port.dhcpServer = true;
    n.port.stuckOnLease = true;
    n.port.cable = true;
    CHECK(n.net.begin(n.config(OV_NET_STATIC)));
    n.run(60000);
    CHECK_EQ(n.port.stops, 2);
    CHECK(n.port.log == std::string(kStaticStart) + "stop;" + kStaticStart + "stop;" + kStaticStart);
    CHECK(n.net.up());
    CHECK(n.address() == "192.168.1.57");
    CHECK(n.status() == "Network   address 192.168.1.57, which is not the fixed address that was asked for\r\n");
    // Should it come right after all, the complaint goes.
    n.port.stuckOnLease = false;
    n.run(1100);
    CHECK(n.status() == "Network   address 192.168.1.16, fixed\r\n");
    CHECK_EQ(n.port.stops, 2);
  }
  {
    // The 'net' command.
    NetRig n;
    n.port.cable = true;
    n.port.dhcpServer = true;
    CHECK(n.net.begin(n.config(OV_NET_AUTO)));
    n.run(2000);
    CHECK(n.command("web") == "(not a net command)");
    CHECK(n.command("net") ==
          "Network   address 192.168.1.57 from DHCP\r\n"
          "          mode auto: DHCP first, then the fixed address 192.168.1.16 when nothing answers within 20 s\r\n"
          "          hardware address A8:61:0A:50:12:34\r\n");
    CHECK(n.command("NET") == n.command("net"));
    n.port.mac.clear();
    CHECK(!has(n.command("net"), "hardware address"));
    const char kUsage[] = "Usage: net   |   net auto   |   net dhcp   |   net static <address> [mask] [gateway]\r\n";
    CHECK(n.command("net up") == kUsage);
    CHECK(n.command("net auto now") == kUsage);
    CHECK(n.command("net static") == kUsage);
    CHECK(n.command("net static 1.2.3.4 255.255.255.0 1.2.3.1 extra") == kUsage);
    const char kForm[] = "An address is four numbers from 0 to 255, like 192.168.1.16.\r\n";
    CHECK(n.command("net static 192.168.1") == kForm);
    CHECK(n.command("net static 192.168.1.300") == kForm);
    CHECK(n.command("net static 192.168.1.20 255.255.255") == kForm);
    CHECK(n.command("net static 192.168.1.20 255.255.255.0 gateway") == kForm);
    CHECK(has(n.command("net static 192.168.1.20 255.0.255.0"), "not a subnet mask"));
    CHECK(has(n.command("net static 192.168.1.255"), "cannot be given to a device"));
    CHECK(has(n.command("net static 192.168.1.20 255.255.255.0 192.168.2.1"), "The gateway has to be another address"));
    // None of that changed anything.
    CHECK(n.port.log == "dhcp;");
    CHECK(n.net.up());
    CHECK_EQ(n.net.config().mode, OV_NET_AUTO);
    CHECK_EQ(n.changes, 1);

    // A new mode starts from the top, and says what holds from now on.
    const std::string fixed = n.command("net static 10.20.30.40");
    CHECK(fixed ==
          "Network   fixed address 10.20.30.40, but no link. Is the cable in?\r\n"
          "          A browser that was connected has to connect again. This lasts until power-off;\r\n"
          "          OptaViewConfig.h sets what OptaView starts with.\r\n");
    CHECK(n.port.log == "dhcp;stop;static 10.20.30.40/255.255.255.0 gw 10.20.30.1;");
    CHECK(!n.net.up());
    n.run(100);
    CHECK_EQ(n.changes, 3);  // down, and up again at the new address
    CHECK(n.address() == "10.20.30.40");
    CHECK(n.command("net") ==
          "Network   address 10.20.30.40, fixed\r\n"
          "          mode static: 10.20.30.40, mask 255.255.255.0, gateway 10.20.30.1\r\n");
    // The first address of a network has no gateway to guess.
    n.command("net static 10.20.30.1");
    CHECK(has(n.port.log, "static 10.20.30.1/255.255.255.0 gw 0.0.0.0;"));
    CHECK(has(n.command("net"), "mode static: 10.20.30.1, mask 255.255.255.0, no gateway"));
    n.command("net static 172.16.5.9 255.255.0.0");
    CHECK(has(n.port.log, "static 172.16.5.9/255.255.0.0 gw 172.16.0.1;"));
    n.command("net static 172.16.5.9 255.255.0.0 172.16.200.200");
    CHECK(has(n.port.log, "static 172.16.5.9/255.255.0.0 gw 172.16.200.200;"));
    // Auto again: the last fixed address is the fallback now.
    n.port.dhcpServer = false;
    const std::string automatic = n.command("net auto");
    CHECK(has(automatic, "waiting for an address from DHCP, 0 s so far (after 20 s the fixed address 172.16.5.9 is used)"));
    n.run(21000);
    CHECK(n.status() == "Network   address 172.16.5.9, the fixed one: DHCP did not answer\r\n");
    CHECK(has(n.command("net dhcp"), "waiting for an address from DHCP, 0 s so far\r\n"));
    CHECK(has(n.command("net"), "mode dhcp: DHCP only"));
    CHECK_EQ(n.port.startsWithoutStop, 0);

    StringOut help;
    n.net.help(help);
    CHECK(has(help.text, "Network\r\n"));
    CHECK(has(help.text, "  net static <address> [mask] [gateway]\r\n"));
  }
  {
    // On the USB console, with the other commands.
    Rig rig;
    SimClock clock(rig.sim);
    ManualClock portClock;
    FakeEthernet port(portClock);
    OvNet net(port, clock);
    OvNetConfig config;
    config.mode = OV_NET_STATIC;
    config.ip = ovIp4(192, 168, 1, 16);
    config.mask = ovIp4(255, 255, 255, 0);
    config.gateway = ovIp4(0, 0, 0, 0);
    CHECK(net.begin(config));
    CHECK(rig.console.addExtra(&net));
    CHECK_HAS(rig.run("net"), "mode static: 192.168.1.16, mask 255.255.255.0, no gateway");
    CHECK_HAS(rig.run("status"), "Network   fixed address 192.168.1.16, but no link. Is the cable in?");
    CHECK_HAS(rig.run("help"), "  net auto | net dhcp     DHCP, with or without the fixed address as a fallback");
    CHECK_HAS(rig.run("net   dhcp"), "waiting for an address from DHCP");
    CHECK_EQ(rig.sim.requests.size(), 0);  // none of it went to the controller
  }
}

// ============================================================================
// The web server on the simulated controller
// ============================================================================
struct FakeWebHost : OvWebHost {
  std::deque<FakeConn *> incoming;
  uint32_t state = 0x2545F491u;
  unsigned randomCalls = 0;
  bool networkUp = true;
  OvConn *accept() override {
    if (incoming.empty()) {
      return nullptr;
    }
    FakeConn *conn = incoming.front();
    incoming.pop_front();
    return conn;
  }
  void random(uint8_t *out, size_t len) override {
    ++randomCalls;
    for (size_t i = 0; i < len; ++i) {
      state = state * 1664525u + 1013904223u;
      out[i] = (uint8_t)(state >> 24);
    }
  }
  bool address(char *out, size_t cap) override {
    if (!networkUp) {
      return false;
    }
    ovCopy(out, cap, "192.168.1.57");
    return true;
  }
};

static const char kPageHeaders[] = "Content-Security-Policy: default-src 'none'; script-src 'sha256-test'\r\n";

struct Ask {
  std::string method = "GET";
  std::string target = "/";
  std::string body;
  std::string host = "192.168.1.57";
  bool marker = true;   // the page sets it on every POST
  bool cookie = true;   // send the session cookie, when there is one
  std::string headers;  // more header lines, each with its CR LF
};

struct WebRig {
  Rig rig;
  SimClock clock;
  FakeWebHost host;
  std::string pageText;
  const char *pageHeaders;
  OvWeb web;
  std::string cookie;  // what the browser holds
  std::vector<std::unique_ptr<FakeConn> > conns;
  FakeConn *last = nullptr;

  // With a short page of the test's own, or with the page that is built into the firmware. And
  // with the built-in register map, or with one of the test's own (its text has to outlive this).
  enum Page { TEST_PAGE, EMBEDDED_PAGE };
  explicit WebRig(Page which = TEST_PAGE, const char *profileJson = OV_BUILTIN_PROFILE)
      : rig(100.0, profileJson),
        clock(rig.sim),
        pageText(which == EMBEDDED_PAGE ? std::string(OV_WEB_PAGE, OV_WEB_PAGE_LENGTH) : makePage()),
        pageHeaders(which == EMBEDDED_PAGE ? OV_WEB_PAGE_HEADERS : kPageHeaders),
        web(rig.device, host, clock, page()) {}

  static std::string makePage() {
    std::string text = "<!doctype html>\n<title>OptaView test page</title>\n";
    for (int i = 0; i < 80; ++i) {
      text += fmtStr("<p>line %d of a page that is longer than one buffer</p>\n", i);
    }
    return text;
  }
  OvWebPage page() const {
    OvWebPage p;
    p.html = pageText.c_str();
    p.length = pageText.size();
    p.headers = pageHeaders;
    return p;
  }

  uint64_t now() const { return rig.sim.elapsedMs(); }

  // A browser connects. Nothing has been sent on it yet.
  FakeConn *connect() {
    conns.emplace_back(new FakeConn());
    FakeConn *conn = conns.back().get();
    conn->clock = [this] { return rig.sim.elapsedMs(); };
    host.incoming.push_back(conn);
    return conn;
  }

  // Lets loop() run, a turn a millisecond, until the server has closed `conn`.
  void serve(FakeConn *conn, uint64_t mostMs = 300000) {
    const uint64_t start = now();
    try {
      while (conn->closes == 0 && now() - start < mostMs) {
        web.poll();
        if (conn->closes == 0) {
          rig.sim.advance(1);
        }
      }
    } catch (const std::runtime_error &stuck) {
      ++gChecks;
      reportFailure(__FILE__, __LINE__, "the request is answered", stuck.what());
    }
  }

  // Lets loop() run for `ms`.
  void idle(uint64_t ms) {
    if (ms > 24ULL * 3600ULL * 1000ULL) {
      // No test waits a day. A time like this comes from a subtraction that went below zero,
      // because a wait the test counted on is not there. Saying so beats waiting it out.
      ++gChecks;
      reportFailure(__FILE__, __LINE__, "the test waits for a time that makes sense",
                    fmtStr("it would wait %llu ms", (unsigned long long)ms));
      return;
    }
    for (uint64_t i = 0; i < ms; ++i) {
      web.poll();
      rig.sim.advance(1);
    }
  }

  std::string build(const Ask &ask) const {
    std::string raw = ask.method + " " + ask.target + " HTTP/1.1\r\n";
    if (!ask.host.empty()) {
      raw += "Host: " + ask.host + "\r\n";
    }
    raw += "User-Agent: optaview-test\r\nAccept: */*\r\n";
    if (ask.cookie && !cookie.empty()) {
      raw += "Cookie: theme=dark; " + std::string(OV_WEB_COOKIE) + "=" + cookie + "\r\n";
    }
    if (ask.method == "POST") {
      if (ask.marker) {
        raw += "X-OptaView: 1\r\n";
      }
      raw += "Content-Type: application/json\r\nContent-Length: " + std::to_string(ask.body.size()) + "\r\n";
    }
    return raw + ask.headers + "\r\n" + ask.body;
  }

  // One request on a connection of its own, answered and closed.
  Reply exchange(const std::string &raw) {
    FakeConn *conn = connect();
    conn->feed(raw);
    serve(conn);
    last = conn;
    CHECK_EQ(conn->closes, 1);
    CHECK_EQ(conn->writesAfterClose, 0);
    CHECK_EQ(conn->writesAfterFailure, 0);
    Reply reply = parseReply(conn->sent);
    // A browser keeps what Set-Cookie gives it, and drops a cookie that is set to nothing.
    const std::string set = reply.header("Set-Cookie");
    const std::string name = std::string(OV_WEB_COOKIE) + "=";
    if (set.compare(0, name.size(), name) == 0) {
      const size_t end = set.find(';');
      cookie = set.substr(name.size(), end == std::string::npos ? std::string::npos : end - name.size());
    }
    return reply;
  }
  Reply ask(const Ask &what) { return exchange(build(what)); }
  Reply get(const std::string &target) {
    Ask what;
    what.target = target;
    return ask(what);
  }
  Reply post(const std::string &target, const std::string &body) {
    Ask what;
    what.method = "POST";
    what.target = target;
    what.body = body;
    return ask(what);
  }
  // A line for the web console; with `answer`, the reply to the question of that id.
  Reply console(const std::string &line, const std::string &answer = std::string()) {
    std::string body = "{\"line\":" + jsonQuote(line);
    if (!answer.empty()) {
      body += ",\"answer\":" + jsonQuote(answer);
    }
    Reply reply = post("/api/console", body + "}");
    gTranscript += reply.text;
    return reply;
  }
  // The PIN is set on the USB side and a browser logs in with it.
  void login(const char *pin = "2468") {
    CHECK(web.setPin(pin));
    const Reply reply = post("/api/login", std::string("{\"pin\":\"") + pin + "\"}");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(cookie.size(), 32);
  }
};

// A register map of sixteen registers that are read as one block, with `pad` more characters
// spread over their names: the text of the block in an answer grows by a byte with each.
static std::string wideProfile(size_t pad) {
  std::string json = "{\"schema\":1,\"profile\":\"wide\",\"title\":\"Wide\",\"version\":\"1\","
                     "\"groups\":[{\"name\":\"g\",\"title\":\"G\"}],\"registers\":[";
  std::string first;
  for (size_t i = 0; i < 16; ++i) {
    const std::string name = fmtStr("r%02u", (unsigned)i) + std::string(pad / 16 + (i < pad % 16 ? 1 : 0), 'x');
    if (i == 0) {
      first = name;
    }
    json += fmtStr("%s{\"addr\":%u,\"name\":\"%s\",\"group\":\"g\"}", i == 0 ? "" : ",", (unsigned)(0xE000 + i),
                   name.c_str());
  }
  return json + "],\"coils\":[{\"addr\":\"0xFF\",\"name\":\"reset\",\"trigger\":true}],"
                "\"modbus\":{\"slaveId\":1,\"baud\":9600,\"format\":\"8N2\",\"readFunction\":4,\"maxBlock\":16,"
                "\"probeRegister\":\"" + first + "\",\"resetCoil\":\"reset\"},\"liveGroups\":[\"g\"]}";
}

// A JSON value as text when it is a string, and something that matches nothing when it is not.
static std::string str(JsonVariantConst value) {
  const char *text = value.as<const char *>();
  return text != nullptr ? std::string(text) : std::string("(not a string)");
}

static bool isLowerHex(const std::string &text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
      return false;
    }
  }
  return !text.empty();
}

static void checkError(const Reply &reply, int status, const char *code, const char *file, int line) {
  ++gChecks;
  if (reply.status != status || !reply.isJson || str(reply.json["error"]) != code ||
      str(reply.json["message"]).size() < 10 || reply.header("Content-Type") != "application/json") {
    reportFailure(file, line, fmtStr("the answer is %d with the error '%s'", status, code).c_str(),
                  fmtStr("got %d:\n", reply.status) + excerpt(reply.body));
  }
}
#define CHECK_ERROR(reply, status, code) checkError((reply), (status), (code), __FILE__, __LINE__)

// ============================================================================
// The page that is built into the firmware (OptaViewWebPage.h, made by tools/embed_web.py)
// ============================================================================
static void testEmbeddedPage() {
  const std::string page(OV_WEB_PAGE, OV_WEB_PAGE_LENGTH);
  CHECK_EQ(OV_WEB_PAGE_LENGTH, strlen(OV_WEB_PAGE));  // nothing in it ends the string early
  CHECK_EQ(sizeof(OV_WEB_PAGE), OV_WEB_PAGE_LENGTH + 1);
  CHECK(page.size() > 20000 && page.size() < 150000);
  CHECK(page.compare(0, 16, "<!DOCTYPE html>\n") == 0);
  CHECK(page.size() > 8 && page.compare(page.size() - 8, 8, "</html>\n") == 0);
  size_t odd = 0;
  for (size_t i = 0; i < page.size(); ++i) {
    odd += (page[i] == '\n' || (page[i] >= 0x20 && page[i] <= 0x7E)) ? 0 : 1;
  }
  CHECK_EQ(odd, 0);  // plain ASCII with LF line ends: what the hashes of the policy were made of
  // One style sheet and one script, which is all that the policy lets a browser use.
  CHECK_EQ(countOf(page, "<style>"), 1);
  CHECK_EQ(countOf(page, "</style>"), 1);
  CHECK_EQ(countOf(page, "<script>"), 1);
  CHECK_EQ(countOf(page, "</script>"), 1);
  CHECK_EQ(countOf(page, "<script"), 1);
  CHECK_EQ(countOf(page, "<style"), 1);
  CHECK_HAS(page, "<title>OptaView</title>");
  CHECK_HASNT(page, "http://");
  CHECK_HASNT(page, "https://");

  const std::string headers(OV_WEB_PAGE_HEADERS);
  const std::string name = "Content-Security-Policy: ";
  CHECK(headers.compare(0, name.size(), name) == 0);
  CHECK_EQ(countOf(headers, "\r\n"), 1);  // one header line, and it is complete
  CHECK_EQ(countOf(headers, "\r"), 1);
  CHECK_EQ(countOf(headers, "\n"), 1);
  CHECK(headers.size() > 2 && headers.compare(headers.size() - 2, 2, "\r\n") == 0);
  const std::string policy = headers.substr(name.size(), headers.size() - name.size() - 2);
  const char *parts[] = {"default-src 'none'", "connect-src 'self'", "img-src data:",     "form-action 'none'",
                         "base-uri 'none'",    "frame-ancestors 'none'", "script-src 'sha256-", "style-src 'sha256-"};
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
    CHECK_MSG(countOf(policy, parts[i]) == 1, "the policy has '%s' once", parts[i]);
  }
  CHECK_HASNT(policy, "unsafe");
  CHECK_HASNT(policy, "*");
  // A SHA-256 hash in base64 is 43 characters and one '='.
  CHECK_EQ(countOf(policy, "'sha256-"), 2);
  for (size_t at = policy.find("'sha256-"); at != std::string::npos; at = policy.find("'sha256-", at + 1)) {
    const size_t end = policy.find('\'', at + 1);
    CHECK(end != std::string::npos && end - at == 8 + 44 && policy[end - 1] == '=');
  }

  // The server sends it whole, with its length and its policy.
  WebRig w(WebRig::EMBEDDED_PAGE);
  const Reply got = w.get("/");
  CHECK_EQ(got.status, 200);
  CHECK_HEADERS(got);
  CHECK(got.body == page);
  CHECK(got.header("Content-Length") == std::to_string(OV_WEB_PAGE_LENGTH));
  CHECK(got.header("Content-Type") == "text/html; charset=utf-8");
  CHECK(got.header("Content-Security-Policy") == policy);
  CHECK_EQ(got.headerCount("Content-Security-Policy"), 1);
  // The API's answers carry no page policy of their own to get wrong.
  CHECK_EQ(w.get("/api/session").status, 200);
}

// ============================================================================
// The page, the routes and what is refused before anything is looked at
// ============================================================================
static void testWebRouting() {
  WebRig w;
  {
    const Reply page = w.get("/");
    CHECK_EQ(page.status, 200);
    CHECK_HEADERS(page);
    CHECK(page.body == w.pageText);
    CHECK(page.header("Content-Type") == "text/html; charset=utf-8");
    CHECK(page.header("Content-Length") == std::to_string(w.pageText.size()));
    CHECK(page.header("Content-Security-Policy") == "default-src 'none'; script-src 'sha256-test'");
    CHECK_EQ(page.headerCount("Set-Cookie"), 0);
    CHECK(w.last->writes > 2);  // longer than one buffer, and still whole
    CHECK(w.get("/index.html").body == w.pageText);
    CHECK(w.get("/?from=a-bookmark").body == w.pageText);
    CHECK_EQ(w.web.served(), 3);
  }
  {
    Ask post;
    post.method = "POST";
    post.body = "{}";
    const Reply posted = w.ask(post);
    CHECK_EQ(posted.status, 405);
    CHECK(posted.header("Allow") == "GET");
    CHECK_HEADERS(posted);
    Ask head;
    head.method = "HEAD";
    CHECK_EQ(w.ask(head).status, 405);
    Ask options;
    options.method = "OPTIONS";
    options.target = "/api/console";
    const Reply preflight = w.ask(options);  // what a browser sends first for a page of another site
    CHECK_ERROR(preflight, 405, "method");
    CHECK_EQ(preflight.headerCount("Access-Control-Allow-Origin"), 0);
  }
  {
    const Reply icon = w.get("/favicon.ico");
    CHECK_EQ(icon.status, 204);
    CHECK(icon.body.empty());
    CHECK(icon.header("Content-Length") == "0");
    CHECK_HEADERS(icon);
  }
  {
    const char *nowhere[] = {"/nothing", "/index.htm", "/index.html/", "/api", "/API/state", "//", "/api../x", "/.env"};
    for (size_t i = 0; i < sizeof(nowhere) / sizeof(nowhere[0]); ++i) {
      const Reply reply = w.get(nowhere[i]);
      CHECK_MSG(reply.status == 404 && reply.header("Content-Type") == "text/plain; charset=utf-8", "%s gave %d",
                nowhere[i], reply.status);
      CHECK_HEADERS(reply);
    }
    const char *noCall[] = {"/api/", "/api/nothing", "/api/state/", "/api/State", "/api/session.json", "/api//state"};
    for (size_t i = 0; i < sizeof(noCall) / sizeof(noCall[0]); ++i) {
      const Reply reply = w.get(noCall[i]);
      CHECK_ERROR(reply, 404, "not-found");
      CHECK_HEADERS(reply);
    }
  }
  {
    // The wrong method for a call, whoever asks.
    const char *posts[] = {"/api/pin", "/api/login", "/api/logout", "/api/console"};
    for (size_t i = 0; i < sizeof(posts) / sizeof(posts[0]); ++i) {
      const Reply reply = w.get(posts[i]);
      CHECK_ERROR(reply, 405, "method");
      CHECK(reply.header("Allow") == "POST");
    }
    const char *gets[] = {"/api/session", "/api/state", "/api/profile", "/api/read", "/api/coils", "/api/snapshot"};
    for (size_t i = 0; i < sizeof(gets) / sizeof(gets[0]); ++i) {
      const Reply reply = w.post(gets[i], "{}");
      CHECK_ERROR(reply, 405, "method");
      CHECK(reply.header("Allow") == "GET");
      Ask del;
      del.method = "DELETE";
      del.target = gets[i];
      CHECK_ERROR(w.ask(del), 405, "method");
    }
  }
  {
    // A POST without the page's own header is not from the page.
    const char *posts[] = {"/api/pin", "/api/login", "/api/logout", "/api/console"};
    for (size_t i = 0; i < sizeof(posts) / sizeof(posts[0]); ++i) {
      Ask plain;
      plain.method = "POST";
      plain.target = posts[i];
      plain.body = "{\"pin\":\"1234\",\"line\":\"live\"}";
      plain.marker = false;
      CHECK_ERROR(w.ask(plain), 403, "marker");
    }
    CHECK(!w.web.pinSet());
  }
  {
    // The Opta is asked for by its address, or not at all.
    const char *refused[] = {"",                 "optaview.local",   "localhost",         "evil.example",
                             "192.168.1.57.evil.example", "[::1]",    "192.168.1.57:",     "192.168.1.57:http",
                             "192.168.1.57:123456", "192.168.1.57:80:80", "192.168.1",   "192.168.1.057",
                             "1.2.3.4.5",        "http://192.168.1.57", "192.168.1.57/", "user@192.168.1.57"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
      Ask named;
      named.host = refused[i];
      const Reply page = w.ask(named);
      CHECK_MSG(page.status == 421, "Host '%s' got %d for the page", refused[i], page.status);
      CHECK_HAS(page.body, "Open OptaView by its IP address");
      CHECK_HASNT(page.body, "OptaView test page");
      CHECK_HEADERS(page);
      named.target = "/api/session";
      CHECK_EQ(w.ask(named).status, 421);
      named.method = "POST";
      named.target = "/api/pin";
      named.body = "{\"pin\":\"1234\"}";
      CHECK_EQ(w.ask(named).status, 421);
    }
    CHECK(!w.web.pinSet());
    const char *accepted[] = {"192.168.1.57", "192.168.1.57:80", "192.168.1.57:8080", "10.0.0.1", "192.168.1.16:65535"};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
      Ask named;
      named.host = accepted[i];
      named.target = "/api/session";
      CHECK_MSG(w.ask(named).status == 200, "Host '%s' was refused", accepted[i]);
    }
  }
  {
    // Requests that cannot be read get a short answer and are closed. What is still arriving
    // is taken first, so that the answer is not lost to a reset.
    const Reply garbage = w.exchange(std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11) + "abc\n\n");
    CHECK_EQ(garbage.status, 400);
    CHECK_HAS(garbage.body, "OptaView could not read that request.");
    CHECK_HEADERS(garbage);
    const Reply longUrl = w.exchange("GET /" + std::string(5000, 'a') + " HTTP/1.1\r\nHost: 192.168.1.57\r\n\r\n");
    CHECK_EQ(longUrl.status, 414);
    CHECK_HAS(longUrl.body, "That request is larger than OptaView accepts.");
    CHECK_EQ(w.last->unread(), 0);
    const Reply longHeader =
        w.exchange("GET / HTTP/1.1\r\nHost: 192.168.1.57\r\nCookie: " + std::string(3000, 'c') + "\r\n\r\n");
    CHECK_EQ(longHeader.status, 431);
    CHECK_EQ(w.last->unread(), 0);
    Ask big;
    big.method = "POST";
    big.target = "/api/console";
    big.body = "{\"line\":\"" + std::string(1100, 'x') + "\"}";
    const Reply tooBig = w.ask(big);
    CHECK_EQ(tooBig.status, 413);
    CHECK_EQ(w.last->unread(), 0);
    CHECK_EQ(w.exchange("POST /api/login HTTP/1.1\r\nHost: 192.168.1.57\r\nX-OptaView: 1\r\n\r\n").status, 411);
    CHECK_EQ(w.exchange("POST /api/login HTTP/1.1\r\nHost: 192.168.1.57\r\nX-OptaView: 1\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n10\r\n{\"pin\":\"123456\"}\r\n0\r\n\r\n")
                 .status,
             501);
    // A sender that never stops is answered all the same, within the half second that taking
    // the rest is given.
    FakeConn *flood = w.connect();
    flood->feed("GET /" + std::string(2000, 'a'));
    for (int i = 1; i <= 200; ++i) {
      flood->feed(std::string(50, 'a'), (uint64_t)i * 10);
    }
    const uint64_t start = w.now();
    w.serve(flood);
    CHECK_EQ(parseReply(flood->sent).status, 414);
    CHECK(w.now() - start <= 510);
    CHECK_EQ(flood->closes, 1);
    // So is one that sends without a single pause, so that there is never a moment in which
    // nothing is waiting to be read: it gets its answer, and the half second holds.
    FakeConn *stream = w.connect();
    stream->feed("GET /" + std::string(2000, 'a'));
    stream->endless = 'a';
    unsigned ticks = 0;
    stream->tick = [&w, &ticks] {
      w.rig.sim.advance(1);  // a thousand bytes a millisecond
      if (++ticks > 60000) {
        throw std::runtime_error("the stream was read for a minute");
      }
    };
    const uint64_t began = w.now();
    w.serve(stream);
    CHECK_EQ(parseReply(stream->sent).status, 414);
    CHECK(w.now() - began >= 500 && w.now() - began <= 510);
    CHECK_EQ(stream->closes, 1);
    CHECK_EQ(stream->readsAfterClose, 0);
    CHECK_EQ(w.get("/api/session").status, 200);
    // None of this reached anything.
    CHECK(!w.web.pinSet());
    CHECK_EQ(w.rig.sim.requests.size(), 0);
    CHECK_EQ(w.get("/api/session").status, 200);
  }
}

// ============================================================================
// Connections that say nothing, or stop halfway
// ============================================================================
static void testWebConnections() {
  {
    // A browser opens connections ahead of need. One that stays silent is closed after three
    // seconds, without an answer, and does not hold up a request on another connection.
    WebRig w;
    FakeConn *silent = w.connect();
    w.idle(100);
    const uint64_t before = w.now();
    CHECK_EQ(w.get("/api/session").status, 200);
    CHECK(w.now() - before < 10);
    CHECK_EQ(silent->closes, 0);
    w.idle(2880);
    CHECK_EQ(silent->closes, 0);
    w.idle(30);
    CHECK_EQ(silent->closes, 1);
    CHECK(silent->sent.empty());
    CHECK_EQ(w.web.served(), 1);
  }
  {
    // A silent connection that is used after all, just before it would be closed.
    WebRig w;
    FakeConn *late = w.connect();
    late->feed(w.build(Ask()), 2900);
    w.serve(late);
    CHECK_EQ(parseReply(late->sent).status, 200);
  }
  {
    // Two silent connections fill both places. A third with a request waits until one of them
    // is given up, and is answered then.
    WebRig w;
    FakeConn *a = w.connect();
    FakeConn *b = w.connect();
    w.idle(10);
    FakeConn *real = w.connect();
    Ask session;
    session.target = "/api/session";
    real->feed(w.build(session));
    const uint64_t start = w.now();
    w.serve(real);
    CHECK_EQ(parseReply(real->sent).status, 200);
    CHECK(w.now() - start <= 3010);
    CHECK_EQ(a->closes, 1);
    w.idle(10);
    CHECK_EQ(b->closes, 1);
    CHECK(a->sent.empty() && b->sent.empty());
  }
  {
    // Half a request, then silence: given up after two seconds without an answer, because
    // there is nobody to read one. The next request is not affected.
    WebRig w;
    FakeConn *half = w.connect();
    half->feed("GET /api/session HTTP/1.1\r\nHost: 192.168");
    const uint64_t start = w.now();
    w.serve(half);
    CHECK_EQ(half->closes, 1);
    CHECK(half->sent.empty());
    CHECK(w.now() - start >= 2000 && w.now() - start <= 2010);
    CHECK_EQ(w.web.served(), 0);
    CHECK_EQ(w.get("/api/session").status, 200);
  }
  {
    // Two requests at once are answered one after the other, both whole.
    WebRig w;
    w.login();
    FakeConn *first = w.connect();
    FakeConn *second = w.connect();
    Ask live;
    live.target = "/api/read?groups=live,status";
    first->feed(w.build(live));
    Ask state;
    state.target = "/api/state";
    second->feed(w.build(state));
    w.serve(first);
    w.serve(second);
    const Reply one = parseReply(first->sent);
    const Reply two = parseReply(second->sent);
    CHECK(one.isJson && one.json["groups"].size() == 2);
    CHECK(two.isJson && two.json["link"]["slave"] == 1);
    CHECK_EQ(first->closes, 1);
    CHECK_EQ(second->closes, 1);
  }
  {
    // The browser goes away while the page is being sent: the sending stops, the connection is
    // closed once, and the server carries on.
    WebRig w;
    FakeConn *gone = w.connect();
    gone->feed(w.build(Ask()));
    gone->failAfterBytes = 1500;
    w.serve(gone);
    CHECK_EQ(gone->closes, 1);
    CHECK(gone->failed);
    CHECK_EQ(gone->writesAfterFailure, 0);
    CHECK_EQ(gone->writesAfterClose, 0);
    CHECK(gone->sent.size() <= 1500);
    CHECK(w.get("/").body == w.pageText);
  }
  {
    // A browser that reads slowly gets the page all the same, as long as it gets through it in
    // thirty seconds. Meanwhile another browser waits its turn.
    WebRig w;
    FakeConn *slow = w.connect();
    slow->feed(w.build(Ask()));
    slow->takeAtMost = 200;
    slow->takeEveryMs = 1000;
    FakeConn *waiting = w.connect();
    Ask session;
    session.target = "/api/session";
    waiting->feed(w.build(session), 500);
    const uint64_t start = w.now();
    w.serve(slow);
    const Reply page = parseReply(slow->sent);
    CHECK_EQ(page.status, 200);
    CHECK(page.body == w.pageText);
    CHECK(w.now() - start > 20000 && w.now() - start < OV_HTTP_SEND_MS);
    CHECK(waiting->sent.empty());
    w.serve(waiting);
    CHECK_EQ(parseReply(waiting->sent).status, 200);
  }
  {
    // One that takes a few bytes a second never stalls, and would hold the Opta for as long as
    // it liked. Thirty seconds of sending is all an answer gets: then the connection is closed,
    // whatever is left, and the next browser is served.
    WebRig w;
    FakeConn *crawl = w.connect();
    crawl->feed(w.build(Ask()));
    crawl->takeAtMost = 10;
    crawl->takeEveryMs = 1000;
    const uint64_t start = w.now();
    w.serve(crawl);
    CHECK_EQ(crawl->closes, 1);
    CHECK(w.now() - start >= OV_HTTP_SEND_MS && w.now() - start <= OV_HTTP_SEND_MS + 10);
    CHECK_EQ(crawl->sent.size(), 300);  // ten bytes at 0 s, 1 s ... 29 s
    CHECK(w.pageText.size() > 3000);
    CHECK_EQ(crawl->writesAfterClose, 0);
    CHECK_EQ(w.web.served(), 1);
    CHECK(w.get("/").body == w.pageText);
    // The same for an answer of the API, before anybody has logged in.
    FakeConn *api = w.connect();
    Ask profile;
    profile.target = "/api/session";
    api->feed(w.build(profile));
    api->takeAtMost = 1;
    api->takeEveryMs = 1000;
    const uint64_t again = w.now();
    w.serve(api);
    CHECK_EQ(api->closes, 1);
    CHECK(w.now() - again >= OV_HTTP_SEND_MS && w.now() - again <= OV_HTTP_SEND_MS + 10);
    CHECK_EQ(api->sent.size(), 30);
  }
  {
    // One that stops taking the answer altogether, without closing the connection: after five
    // seconds without a byte taken it counts as gone.
    WebRig w;
    FakeConn *stuck = w.connect();
    stuck->feed(w.build(Ask()));
    stuck->takeAtMost = 500;
    stuck->takeEveryMs = 1000;
    stuck->deafFrom = w.now() + 500;  // it takes the first 500 bytes and then nothing
    const uint64_t start = w.now();
    w.serve(stuck);
    CHECK_EQ(stuck->closes, 1);
    CHECK_EQ(stuck->sent.size(), 500);
    CHECK(w.now() - start >= OV_HTTP_SEND_STALL_MS && w.now() - start <= OV_HTTP_SEND_STALL_MS + 10);
    CHECK_EQ(stuck->writesAfterClose, 0);
    CHECK_EQ(w.get("/api/session").status, 200);
    // A refusal that is not taken either, from a sender that goes on sending: five seconds for
    // the answer, half a second for what keeps arriving, and the connection is closed.
    FakeConn *both = w.connect();
    both->feed("GET /" + std::string(2000, 'a'));
    both->deafFrom = 0;
    both->endless = 'a';
    unsigned ticks = 0;
    both->tick = [&w, &ticks] {
      w.rig.sim.advance(1);
      if (++ticks > 60000) {
        throw std::runtime_error("the stream was read for a minute");
      }
    };
    const uint64_t began = w.now();
    w.serve(both);
    CHECK_EQ(both->closes, 1);
    CHECK(both->sent.empty());
    CHECK(w.now() - began >= OV_HTTP_SEND_STALL_MS + 500 && w.now() - began <= OV_HTTP_SEND_STALL_MS + 520);
    CHECK_EQ(w.get("/api/session").status, 200);
  }
}

// ============================================================================
// The PIN, logging in, sessions
// ============================================================================
static void testWebLogin() {
  {
    WebRig w;
    // Nobody has been here yet.
    const Reply fresh = w.get("/api/session");
    CHECK_EQ(fresh.status, 200);
    CHECK_HEADERS(fresh);
    CHECK(str(fresh.json["version"]) == OV_VERSION);
    CHECK(str(fresh.json["profile"]) == "Morningstar SunSaver MPPT (SS-MPPT-15L)");
    CHECK(fresh.json["pinSet"] == false);
    CHECK(fresh.json["loggedIn"] == false);
    CHECK_EQ(fresh.json["wait"].as<int>(), 0);
    CHECK_EQ(fresh.headerCount("Set-Cookie"), 0);
    // Everything about the controller needs a session.
    const char *closed[] = {"/api/state", "/api/profile", "/api/read?groups=live", "/api/coils", "/api/snapshot"};
    for (size_t i = 0; i < sizeof(closed) / sizeof(closed[0]); ++i) {
      CHECK_ERROR(w.get(closed[i]), 401, "login");
    }
    CHECK_ERROR(w.post("/api/console", "{\"line\":\"live\"}"), 401, "login");
    CHECK_EQ(w.rig.sim.requests.size(), 0);
    // There is no PIN to log in with yet.
    CHECK_ERROR(w.post("/api/login", "{\"pin\":\"1234\"}"), 409, "no-pin");

    // The first visitor chooses it. It has to be 4 to 8 digits, as text.
    const char *badPins[] = {"{\"pin\":\"123\"}",    "{\"pin\":\"123456789\"}", "{\"pin\":\"12a4\"}",  "{\"pin\":\"12 4\"}",
                             "{\"pin\":1234}",       "{\"pin\":null}",          "{\"pin\":\"\"}",      "{\"PIN\":\"1234\"}",
                             "{}",                   "[\"1234\"]",              "\"1234\"",            "1234",
                             "",                     "{\"pin\":\"1234\"",       "pin=1234",            "{\"pin\":[\"1234\"]}",
                             "{\"pin\":\"\\u0031234\\u0000\"}"};
    for (size_t i = 0; i < sizeof(badPins) / sizeof(badPins[0]); ++i) {
      const Reply reply = w.post("/api/pin", badPins[i]);
      CHECK_MSG(reply.status == 400 && str(reply.json["error"]) == "pin-format", "%s gave %d", badPins[i], reply.status);
    }
    CHECK(!w.web.pinSet());
    CHECK(w.cookie.empty());
    const Reply chosen = w.post("/api/pin", "{\"pin\":\"4321\"}");
    CHECK_EQ(chosen.status, 200);
    CHECK_HEADERS(chosen);
    CHECK(chosen.json["ok"] == true);
    CHECK(w.web.pinSet());
    CHECK(w.web.sessionOpen());
    // The cookie: 32 hex digits, not readable by scripts, not sent along from other sites.
    CHECK_EQ(chosen.headerCount("Set-Cookie"), 1);
    CHECK(chosen.header("Set-Cookie") == "optaview_session=" + w.cookie + "; Path=/; HttpOnly; SameSite=Strict");
    CHECK_EQ(w.cookie.size(), 32);
    CHECK(isLowerHex(w.cookie));
    // Choosing made the chooser the one who is logged in.
    const Reply in = w.get("/api/session");
    CHECK(in.json["pinSet"] == true);
    CHECK(in.json["loggedIn"] == true);
    CHECK_EQ(w.get("/api/state").status, 200);
    // Once there is a PIN, this door is shut: for others, and for the one who is logged in.
    CHECK_ERROR(w.post("/api/pin", "{\"pin\":\"9999\"}"), 409, "pin-set");
    Ask other;
    other.method = "POST";
    other.target = "/api/pin";
    other.body = "{\"pin\":\"9999\"}";
    other.cookie = false;
    CHECK_ERROR(w.ask(other), 409, "pin-set");
    CHECK_EQ(w.get("/api/state").status, 200);

    // Logging out.
    const std::string old = w.cookie;
    const Reply out = w.post("/api/logout", "");
    CHECK_EQ(out.status, 200);
    CHECK(out.header("Set-Cookie") == "optaview_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    CHECK(w.cookie.empty());
    CHECK(!w.web.sessionOpen());
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    w.cookie = old;  // a copy of the old cookie is worth nothing
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    CHECK(w.get("/api/session").json["loggedIn"] == false);
    w.cookie.clear();
    // Logging out when nobody is logged in is not an error.
    CHECK_EQ(w.post("/api/logout", "{}").status, 200);

    // Logging in with the PIN that was chosen.
    const Reply back = w.post("/api/login", "{\"pin\":\"4321\"}");
    CHECK_EQ(back.status, 200);
    CHECK_EQ(w.cookie.size(), 32);
    CHECK(w.cookie != old);  // a new session, a new token
    CHECK_EQ(w.get("/api/state").status, 200);
  }
  {
    // One browser at a time: a second login ends the first session.
    WebRig w;
    w.login("1357");
    const std::string first = w.cookie;
    w.cookie.clear();
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"1357\"}").status, 200);
    const std::string second = w.cookie;
    CHECK(first != second);
    CHECK_EQ(w.get("/api/state").status, 200);
    w.cookie = first;
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    // A logout from the browser that is not logged in does not end the other's session.
    CHECK_EQ(w.post("/api/logout", "").status, 200);
    w.cookie = second;
    CHECK_EQ(w.get("/api/state").status, 200);
    // Cookies that are nearly right.
    const char *nearly[] = {"", "0", "00000000000000000000000000000000"};
    for (size_t i = 0; i < sizeof(nearly) / sizeof(nearly[0]); ++i) {
      w.cookie = nearly[i];
      CHECK_ERROR(w.get("/api/state"), 401, "login");
    }
    w.cookie = second.substr(0, 31);
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    w.cookie = second + "0";
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    w.cookie = second;
    w.cookie[31] = w.cookie[31] == '0' ? '1' : '0';
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    w.cookie = second;
    for (size_t i = 0; i < w.cookie.size(); ++i) {
      w.cookie[i] = (char)toupper((unsigned char)w.cookie[i]);
    }
    if (w.cookie != second) {
      CHECK_ERROR(w.get("/api/state"), 401, "login");
    }
    w.cookie = second;
    CHECK_EQ(w.get("/api/state").status, 200);
  }
  {
    // Wrong PINs are answered more and more slowly, and during the wait a PIN is not even
    // looked at: the right one is refused as well, and trying does not lengthen the wait.
    WebRig w;
    CHECK(w.web.setPin("2468"));
    const unsigned waits[] = {1, 2, 4, 8, 30, 60, 120, 240, 480, 900, 900, 900};
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); ++i) {
      const uint64_t asked = w.now();
      const Reply wrong = w.post("/api/login", i % 2 == 0 ? "{\"pin\":\"0000\"}" : "not even json");
      CHECK_ERROR(wrong, 401, "wrong-pin");
      CHECK_MSG(wrong.json["wait"].as<unsigned>() == waits[i], "after %u wrong PINs the wait is %u s", (unsigned)i + 1,
                wrong.json["wait"].as<unsigned>());
      CHECK_EQ(wrong.headerCount("Set-Cookie"), 0);
      const Reply early = w.post("/api/login", "{\"pin\":\"2468\"}");
      CHECK_ERROR(early, 429, "wait");
      CHECK(early.json["wait"].as<unsigned>() >= 1 && early.json["wait"].as<unsigned>() <= waits[i]);
      CHECK(w.cookie.empty());
      CHECK_EQ(w.get("/api/session").json["wait"].as<unsigned>(), early.json["wait"].as<unsigned>());
      // Up to the last moment.
      w.idle(asked + waits[i] * 1000ULL - 20 - w.now());
      CHECK_ERROR(w.post("/api/login", "{\"pin\":\"2468\"}"), 429, "wait");
      w.idle(asked + waits[i] * 1000ULL + 5 - w.now());
      CHECK_EQ(w.get("/api/session").json["wait"].as<unsigned>(), 0);
    }
    // After all that, the right PIN still opens it, and the count starts again.
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
    CHECK_EQ(w.cookie.size(), 32);
    w.post("/api/logout", "");
    const Reply wrong = w.post("/api/login", "{\"pin\":\"0000\"}");
    CHECK_EQ(wrong.json["wait"].as<unsigned>(), 1);
  }
  {
    // A wrong PIN does not log out whoever is logged in, and tells nothing about the PIN: the
    // same answer for one digit off, another length, or no PIN at all.
    WebRig w;
    w.login("2468");
    const std::string session = w.cookie;
    const char *guesses[] = {"{\"pin\":\"2469\"}", "{\"pin\":\"246\"}", "{\"pin\":\"24680\"}", "{}", "{\"pin\":\"abcd\"}"};
    for (size_t i = 0; i < sizeof(guesses) / sizeof(guesses[0]); ++i) {
      const Reply wrong = w.post("/api/login", guesses[i]);
      CHECK_ERROR(wrong, 401, "wrong-pin");
      CHECK(str(wrong.json["message"]) == "That is not the PIN.");
      w.idle(wrong.json["wait"].as<unsigned>() * 1000ULL + 5);
    }
    CHECK(w.cookie == session);
    CHECK_EQ(w.get("/api/state").status, 200);
  }
  {
    // A new PIN from the USB side ends the session and the wait.
    WebRig w;
    w.login("2468");
    for (int i = 0; i < 5; ++i) {
      const Reply wrong = w.post("/api/login", "{\"pin\":\"0000\"}");
      w.idle(wrong.json["wait"].as<unsigned>() * 1000ULL - 500);
    }
    CHECK(w.get("/api/session").json["wait"].as<unsigned>() > 0);
    CHECK(!w.web.setPin("12"));
    CHECK(!w.web.setPin("123456789"));
    CHECK(!w.web.setPin("12e4"));
    CHECK(!w.web.setPin(""));
    CHECK(!w.web.setPin(nullptr));
    CHECK(!w.web.setPin("2468\n") && !w.web.setPin(" 2468") && !w.web.setPin("+2468") && !w.web.setPin("0x2468"));
    const char *pins[] = {"0000", "2468", "12345", "123456", "1234567", "12345678"};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
      CHECK_MSG(ovIsPin(pins[i]), "'%s' is a PIN", pins[i]);
    }
    const char *notPins[] = {"", "1", "12", "123", "123456789", "12e4", "abcd", "1234 ", "12 34", "\t1234", "1234\r"};
    for (size_t i = 0; i < sizeof(notPins) / sizeof(notPins[0]); ++i) {
      CHECK_MSG(!ovIsPin(notPins[i]), "'%s' was taken as a PIN", notPins[i]);
    }
    CHECK(!ovIsPin(nullptr));
    CHECK(!w.web.setPin(nullptr));
    CHECK(w.web.sessionOpen());  // none of those changed anything
    CHECK(w.web.setPin("13572468"));
    CHECK(!w.web.sessionOpen());
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    CHECK_EQ(w.get("/api/session").json["wait"].as<unsigned>(), 0);
    CHECK_ERROR(w.post("/api/login", "{\"pin\":\"2468\"}"), 401, "wrong-pin");
    w.idle(1100);
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"13572468\"}").status, 200);
    // No PIN at all again: the next visitor chooses.
    w.web.clearPin();
    CHECK(!w.web.pinSet());
    CHECK(!w.web.sessionOpen());
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    CHECK(w.get("/api/session").json["pinSet"] == false);
    w.cookie.clear();
    CHECK_EQ(w.post("/api/pin", "{\"pin\":\"00000000\"}").status, 200);
    CHECK_EQ(w.get("/api/state").status, 200);
  }
  {
    // A session ends after half an hour in which the person did nothing. What the page does by
    // itself (its refreshing, marked auto=1) does not count as doing something.
    WebRig w;
    w.login();
    w.idle(29 * 60 * 1000);
    CHECK_EQ(w.get("/api/state").status, 200);  // used: the half hour starts again
    w.idle(29 * 60 * 1000);
    CHECK_EQ(w.get("/api/read?groups=live").status, 200);
    w.idle(30 * 60 * 1000 - 100);
    CHECK(w.web.sessionOpen());
    w.idle(200);
    CHECK(!w.web.sessionOpen());
    CHECK_ERROR(w.get("/api/state"), 401, "login");
    CHECK(w.get("/api/session").json["loggedIn"] == false);

    w.cookie.clear();
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
    const uint64_t loggedIn = w.now();
    unsigned answered = 0;
    while (w.now() - loggedIn < 31 * 60 * 1000) {
      w.idle(5000);
      const Reply refresh = w.get("/api/read?groups=live&auto=1");
      if (refresh.status == 200) {
        ++answered;
      } else {
        CHECK_ERROR(refresh, 401, "login");
        break;
      }
    }
    CHECK(w.now() - loggedIn >= 30 * 60 * 1000);
    CHECK(w.now() - loggedIn < 30 * 60 * 1000 + 6000);
    CHECK(answered > 350);
    // Asking who is logged in is not doing something either.
    w.cookie.clear();
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
    for (int i = 0; i < 31; ++i) {
      w.idle(60 * 1000);
      w.get("/api/session");
    }
    CHECK_ERROR(w.get("/api/state"), 401, "login");
  }
  {
    // Tokens come from the board's random numbers, a fresh one for every login.
    WebRig w;
    CHECK(w.web.setPin("2468"));
    std::vector<std::string> seen;
    for (int i = 0; i < 20; ++i) {
      w.cookie.clear();
      CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
      for (size_t k = 0; k < seen.size(); ++k) {
        CHECK(seen[k] != w.cookie);
      }
      seen.push_back(w.cookie);
    }
    CHECK_EQ(w.host.randomCalls, 20);
  }
}

// ============================================================================
// Reading: state, profile, registers, coils, snapshot
// ============================================================================
static JsonVariantConst findByName(JsonArrayConst list, const char *name) {
  for (JsonVariantConst item : list) {
    if (str(item["name"]) == name) {
      return item;
    }
  }
  return JsonVariantConst();
}

static void testWebReading() {
  {
    WebRig w;
    w.login();
    // The state: what the page shows around everything else. It asks the controller nothing.
    const Reply state = w.get("/api/state");
    CHECK_EQ(state.status, 200);
    CHECK_HEADERS(state);
    CHECK(state.isJson);
    CHECK(str(state.json["version"]) == OV_VERSION);
    CHECK_EQ(state.json["link"]["slave"].as<int>(), 1);
    CHECK_EQ(state.json["link"]["baud"].as<long>(), 9600);
    CHECK(str(state.json["link"]["format"]) == "8N2");
    CHECK_EQ(state.json["link"]["readFunction"].as<int>(), 4);
    CHECK_EQ(state.json["link"]["timeoutMs"].as<int>(), 1000);
    CHECK(str(state.json["link"]["blocks"]) == "auto");
    CHECK(state.json["link"]["blocksInUse"] == true);
    CHECK(str(state.json["scales"]["settings"]["name"]) == "vset");
    // (ArduinoJson, which reads the answers here, keeps short decimals as float: hence 1e-5.)
    CHECK(near(state.json["scales"]["settings"]["num"].as<double>(), 100.0, 1e-5));
    CHECK(near(state.json["scales"]["settings"]["den"].as<double>(), 32768.0, 1e-5));
    CHECK(state.json["scales"]["settings"]["confirmed"] == false);
    CHECK(near(state.json["scales"]["settings"]["found"].as<double>(), 0.0, 1e-5));
    CHECK(str(state.json["scales"]["live"]["name"]) == "vlive");
    CHECK(state.json["scales"]["live"]["confirmed"] == false);
    CHECK(state.json["resetNeeded"] == false);
    CHECK(state.json["resetMaybe"] == false);
    CHECK_EQ(state.json["counters"]["sent"].as<long>(), 0);
    CHECK_EQ(w.rig.sim.requests.size(), 0);

    // It follows what happens on the USB console: the link, the scales, a restart that is due.
    w.rig.run("scalecheck 13.68");
    w.rig.run("timeout 800");
    w.rig.run("blocks off");
    const Reply checked = w.get("/api/state");
    CHECK(checked.json["scales"]["settings"]["confirmed"] == true);
    CHECK(near(checked.json["scales"]["settings"]["found"].as<double>(), 100.0, 1e-5));
    CHECK(checked.json["scales"]["live"]["confirmed"] == true);
    CHECK_EQ(checked.json["link"]["timeoutMs"].as<int>(), 800);
    CHECK(str(checked.json["link"]["blocks"]) == "off");
    CHECK(checked.json["link"]["blocksInUse"] == false);
    CHECK_EQ(checked.json["counters"]["sent"].as<unsigned long>(), w.rig.master.stats.sent);
    CHECK_EQ(checked.json["counters"]["ok"].as<unsigned long>(), w.rig.master.stats.ok);
    w.rig.run("scale vset 96.667");
    const Reply rescaled = w.get("/api/state");
    CHECK(rescaled.json["scales"]["settings"]["confirmed"] == false);
    CHECK(near(rescaled.json["scales"]["settings"]["num"].as<double>(), 96.667, 1e-5));
    CHECK(near(rescaled.json["scales"]["settings"]["found"].as<double>(), 100.0, 1e-5));
    w.rig.run("scale vset 100");
    w.rig.run("set EV_lvd 12.00");
    w.rig.run("yes");
    CHECK(w.get("/api/state").json["resetNeeded"] == true);
    w.rig.run("slave 2");
    const Reply moved = w.get("/api/state");
    CHECK_EQ(moved.json["link"]["slave"].as<int>(), 2);
    CHECK(moved.json["scales"]["settings"]["confirmed"] == false);  // it may be another controller now
    CHECK(near(moved.json["scales"]["settings"]["found"].as<double>(), 0.0, 1e-5));
  }
  {
    // The register map, for the page to build its tables from.
    WebRig w;
    w.login();
    const Reply reply = w.get("/api/profile");
    CHECK_EQ(reply.status, 200);
    CHECK_HEADERS(reply);
    CHECK(reply.isJson);
    CHECK(reply.body.size() > 15000);
    CHECK_EQ(w.rig.sim.requests.size(), 0);
    OvProfile &profile = w.rig.profile;
    JsonObjectConst json = reply.json.as<JsonObjectConst>();
    CHECK(str(json["id"]) == profile.id());
    CHECK(str(json["title"]) == profile.title());
    CHECK(str(json["version"]) == profile.version());
    CHECK(str(json["source"]) == profile.source());
    CHECK(str(json["verifiedBy"]) == profile.verifiedBy());
    CHECK(str(json["resetCoil"]) == "reset");
    CHECK_EQ(json["groups"].size(), profile.groupCount());
    size_t liveGroups = 0;
    for (size_t i = 0; i < profile.groupCount(); ++i) {
      CHECK(str(json["groups"][i]["name"]) == profile.group(i).name);
      CHECK(str(json["groups"][i]["title"]) == profile.group(i).title);
      const bool live = json["groups"][i]["live"] == true;
      liveGroups += live ? 1 : 0;
      CHECK_EQ(live, strcmp(profile.group(i).name, "live") == 0 || strcmp(profile.group(i).name, "status") == 0);
    }
    CHECK_EQ(liveGroups, 2);
    CHECK_EQ(json["registers"].size(), profile.registerCount());
    size_t writable = 0;
    for (size_t i = 0; i < profile.registerCount(); ++i) {
      const OvRegister &reg = profile.reg(i);
      JsonVariantConst item = json["registers"][i];
      CHECK(str(item["name"]) == reg.name);
      CHECK_EQ(item["addr"].as<long>(), reg.addr);
      CHECK(str(item["label"]) == reg.label);
      CHECK(str(item["group"]) == reg.group);
      CHECK(str(item["units"]) == reg.units);
      CHECK_EQ(item["decimals"].as<int>(), reg.decimals);
      CHECK_EQ(item["writable"] == true, reg.writable);
      CHECK_EQ(item["bench"] == true, reg.bench);
      CHECK_EQ(item["scale"].isNull(), reg.scale == nullptr);
      CHECK_EQ(item["min"].isNull(), !reg.hasMin);
      CHECK_EQ(item["max"].isNull(), !reg.hasMax);
      if (reg.hasMin) {
        CHECK(near(item["min"].as<double>(), reg.minValue, 1e-6));
      }
      if (reg.hasMax) {
        CHECK(near(item["max"].as<double>(), reg.maxValue, 1e-6));
      }
      CHECK_EQ(item["zeroOk"] == true, reg.zeroOk);
      writable += reg.writable ? 1 : 0;
    }
    CHECK_EQ(writable, 36);
    JsonVariantConst lvd = findByName(json["registers"], "EV_lvd");
    CHECK(str(lvd["type"]) == "u16");
    CHECK(str(lvd["scale"]) == "vset");
    CHECK(str(lvd["units"]) == "V");
    CHECK(lvd["writable"] == true);
    CHECK(str(findByName(json["registers"], "charge_state")["type"]) == "enum");
    CHECK(str(findByName(json["registers"], "alarm")["type"]) == "bits");
    CHECK(str(findByName(json["registers"], "T_hs")["type"]) == "i8");
    CHECK_EQ(json["coils"].size(), profile.coilCount());
    size_t resets = 0;
    for (size_t i = 0; i < profile.coilCount(); ++i) {
      CHECK(str(json["coils"][i]["name"]) == profile.coil(i).name);
      CHECK_EQ(json["coils"][i]["addr"].as<long>(), profile.coil(i).addr);
      CHECK(str(json["coils"][i]["label"]) == profile.coil(i).label);
      CHECK_EQ(json["coils"][i]["trigger"] == true, profile.coil(i).trigger);
      resets += json["coils"][i]["reset"] == true ? 1 : 0;
    }
    CHECK_EQ(resets, 1);
    CHECK(findByName(json["coils"], "reset")["reset"] == true);
    CHECK_EQ(json["checks"].size(), profile.checkCount());
    for (size_t i = 0; i < profile.checkCount(); ++i) {
      CHECK(str(json["checks"][i]["below"]) == profile.check(i).below->name);
      CHECK(str(json["checks"][i]["above"]) == profile.check(i).above->name);
      CHECK(str(json["checks"][i]["message"]) == profile.check(i).message);
    }
    CHECK_EQ(json["notes"].size(), profile.noteCount());
    CHECK_EQ(json["writeNotes"].size(), profile.writeNoteCount());
    CHECK_EQ(json["writeNotes"].size(), 3);
    CHECK(str(json["writeNotes"][0]) == profile.writeNote(0));
    CHECK_EQ(json["wiring"].size(), profile.wiringCount());
    CHECK(str(json["wiring"][12]) == profile.wiring(12));

    // A browser that leaves in the middle of it.
    FakeConn *gone = w.connect();
    Ask ask;
    ask.target = "/api/profile";
    gone->feed(w.build(ask));
    gone->failAfterBytes = 5000;
    w.serve(gone);
    CHECK_EQ(gone->closes, 1);
    CHECK_EQ(gone->writesAfterFailure, 0);
    CHECK(gone->sent.size() <= 5000);
    CHECK_EQ(w.get("/api/profile").body.size(), reply.body.size());
  }
  {
    // Registers, by group.
    WebRig w;
    w.login();
    const Reply live = w.get("/api/read?groups=live");
    CHECK_EQ(live.status, 200);
    CHECK_HEADERS(live);
    CHECK(live.isJson);
    CHECK_EQ(live.json["groups"].size(), 1);
    CHECK(str(live.json["groups"][0]["name"]) == "live");
    JsonArrayConst meters = live.json["groups"][0]["registers"];
    CHECK_EQ(meters.size(), 9);
    CHECK(str(meters[0]["name"]) == "adc_vb_f");
    CHECK_EQ(meters[0]["raw"].size(), 1);
    CHECK_EQ(meters[0]["raw"][0].as<long>(), w.rig.sim.regs[0x0008]);
    CHECK(str(meters[0]["text"]) == "13.68 V");
    CHECK(near(meters[0]["value"].as<double>(), w.rig.sim.regs[0x0008] * 100.0 / 32768.0, 1e-5));
    CHECK(meters[0]["error"].isNull());
    // A value that stands for "there is none" is text only.
    JsonVariantConst rts = findByName(meters, "T_rts");
    CHECK(str(rts["text"]) == "not connected");
    CHECK(rts["value"].isNull());
    CHECK_EQ(rts["raw"][0].as<long>(), 0x80);
    CHECK(live.json["linkDown"] == false);
    CHECK(live.json["late"] == false);
    CHECK(live.json["stopped"] == false);
    CHECK_EQ(live.json["state"]["counters"]["sent"].as<long>(), 1);  // one block read for the nine of them
    CHECK_EQ(w.rig.sim.requests.size(), 1);

    // Several groups come in the order asked for; names and lists are not numbers.
    const Reply two = w.get("/api/read?groups=status,live");
    CHECK_EQ(two.json["groups"].size(), 2);
    CHECK(str(two.json["groups"][0]["name"]) == "status");
    CHECK(str(two.json["groups"][1]["name"]) == "live");
    JsonArrayConst status = two.json["groups"][0]["registers"];
    JsonVariantConst chargeState = findByName(status, "charge_state");
    CHECK(str(chargeState["text"]) == "FLOAT (7)");
    CHECK(chargeState["value"].isNull());
    CHECK_EQ(chargeState["raw"][0].as<long>(), 7);
    JsonVariantConst alarm = findByName(status, "alarm");
    CHECK_EQ(alarm["raw"].size(), 2);  // a 32-bit register: both words
    CHECK(str(alarm["text"]) == "none");
    CHECK(alarm["value"].isNull());
    // Every group holds exactly the registers the profile puts in it.
    OvProfile &profile = w.rig.profile;
    for (size_t g = 0; g < profile.groupCount(); ++g) {
      const Reply group = w.get(std::string("/api/read?groups=") + profile.group(g).name);
      size_t expected = 0;
      for (size_t i = 0; i < profile.registerCount(); ++i) {
        expected += strcmp(profile.reg(i).group, profile.group(g).name) == 0 ? 1 : 0;
      }
      CHECK_MSG(group.json["groups"][0]["registers"].size() == expected, "group %s", profile.group(g).name);
      for (JsonVariantConst item : group.json["groups"][0]["registers"].as<JsonArrayConst>()) {
        const OvRegister *reg = profile.findRegister(str(item["name"]).c_str());
        CHECK(reg != nullptr);
        if (reg != nullptr) {
          CHECK_MSG(item["raw"].size() == reg->words, "%s", reg->name);
          CHECK_MSG(item["value"].isNull() || ovIsNumeric(*reg), "%s", reg->name);
        }
      }
    }
    // A setting reads back what the controller holds.
    const Reply load = w.get("/api/read?groups=load");
    JsonVariantConst lvd = findByName(load.json["groups"][0]["registers"], "EV_lvd");
    CHECK(str(lvd["text"]) == "11.50 V");
    CHECK(near(lvd["value"].as<double>(), 11.50, 0.005));
    CHECK_EQ(lvd["raw"][0].as<long>(), w.rig.sim.regs[0xE022]);

    // What is asked for has to make sense as a whole, or nothing is read.
    const size_t before = w.rig.sim.requests.size();
    CHECK_ERROR(w.get("/api/read"), 400, "groups");
    CHECK_ERROR(w.get("/api/read?groups="), 400, "groups");
    CHECK_ERROR(w.get("/api/read?group=live"), 400, "groups");
    CHECK_ERROR(w.get("/api/read?groups=nothing"), 404, "group");
    CHECK_ERROR(w.get("/api/read?groups=live,nothing"), 404, "group");
    CHECK_ERROR(w.get("/api/read?groups=live,"), 404, "group");
    CHECK_ERROR(w.get("/api/read?groups=,live"), 404, "group");
    CHECK_ERROR(w.get("/api/read?groups=live%2Cstatus"), 404, "group");  // nothing is decoded
    CHECK_ERROR(w.get("/api/read?groups=l,l,l,l,l,l,l,l,l,l,l,l,l,l,l,l,l"), 404, "group");
    CHECK_EQ(w.rig.sim.requests.size(), before);
    // Sixteen groups are the most; the same one twice is read twice.
    const Reply sixteen = w.get("/api/read?groups=led,led,led,led,led,led,led,led,led,led,led,led,led,led,led,led");
    CHECK_EQ(sixteen.json["groups"].size(), 16);
    CHECK_ERROR(w.get("/api/read?groups=led,led,led,led,led,led,led,led,led,led,led,led,led,led,led,led,led"), 404,
                "group");
  }
  {
    // The controller does not answer: said once per register, and not waited for 94 times.
    WebRig w;
    w.login();
    w.rig.sim.powered = false;
    const uint64_t start = w.now();
    const Reply dead = w.get("/api/read?groups=live,status,load");
    CHECK_EQ(dead.status, 200);
    CHECK(dead.isJson);
    CHECK(dead.json["linkDown"] == true);
    CHECK(dead.json["stopped"] == false);
    CHECK(w.now() - start < 9000);
    size_t noReply = 0;
    size_t notRead = 0;
    size_t total = 0;
    for (JsonVariantConst group : dead.json["groups"].as<JsonArrayConst>()) {
      for (JsonVariantConst item : group["registers"].as<JsonArrayConst>()) {
        ++total;
        CHECK(item["raw"].isNull());
        CHECK(item["text"].isNull());
        noReply += str(item["error"]) == "no reply" ? 1 : 0;
        notRead += str(item["error"]) == "not read" ? 1 : 0;
      }
    }
    CHECK_EQ(total, 9 + 10 + 6);
    CHECK(noReply >= 1);
    CHECK_EQ(noReply + notRead, total);
    CHECK(dead.json["state"]["counters"]["timeouts"].as<long>() >= 3);
    // Back again: the next request simply works.
    w.rig.sim.powered = true;
    const Reply back = w.get("/api/read?groups=live");
    CHECK(back.json["linkDown"] == false);
    CHECK(str(back.json["groups"][0]["registers"][0]["text"]) == "13.68 V");
  }
  {
    // A register the controller refuses is an error for that register, not for the request.
    WebRig w;
    w.login();
    w.rig.sim.regs.erase(0x0010);  // T_rts is not there on this one
    const Reply live = w.get("/api/read?groups=live");
    JsonArrayConst meters = live.json["groups"][0]["registers"];
    CHECK(str(findByName(meters, "T_rts")["error"]) == "exception 2 (illegal data address)");
    CHECK(str(findByName(meters, "adc_vb_f")["text"]) == "13.68 V");
    CHECK(live.json["linkDown"] == false);
    // A reply that came too late for its request is told, so that the page can say so.
    w.rig.sim.responseDelayMs = 1100;
    w.rig.run("blocks off");
    const Reply slow = w.get("/api/read?groups=led");
    CHECK(slow.isJson);
    CHECK(slow.json["late"] == true);
    CHECK(slow.json["state"]["counters"]["lateArrivals"].as<long>() >= 1);
    // The last of those replies is still on its way when the next request is made: that one
    // says so too. After it the bus is quiet again.
    w.rig.sim.responseDelayMs = 20;
    w.get("/api/read?groups=led");
    CHECK(w.get("/api/read?groups=led").json["late"] == false);
  }
  {
    // The browser leaves during a read that takes long: it ends with the run of registers that
    // is being read, not with the last one asked for.
    WebRig w;
    w.login();
    w.rig.sim.responseDelayMs = 900;
    const char kMany[] = "/api/read?groups=live,status,counters,mppt,daily,charge1,charge2,charge,load,led";
    size_t before = w.rig.sim.requests.size();
    CHECK_EQ(w.get(kMany).json["groups"].size(), 10);
    const size_t whole = w.rig.sim.requests.size() - before;
    CHECK(whole >= 10);
    FakeConn *gone = w.connect();
    Ask ask;
    ask.target = kMany;
    gone->feed(w.build(ask));
    gone->goneAt = w.now() + 3000;
    before = w.rig.sim.requests.size();
    const uint64_t start = w.now();
    w.serve(gone);
    CHECK_EQ(gone->closes, 1);
    CHECK(w.now() - start < 5000);
    CHECK(w.rig.sim.requests.size() - before <= 5);
    CHECK_EQ(gone->writesAfterFailure, 0);
    w.rig.sim.responseDelayMs = 20;
    const Reply after = w.get("/api/read?groups=live");
    CHECK(after.json["stopped"] == false);
    CHECK_EQ(after.json["groups"][0]["registers"].size(), 9);
  }
  {
    // Coils.
    WebRig w;
    w.login();
    w.rig.sim.coils[0x0001] = true;
    const Reply reply = w.get("/api/coils");
    CHECK_EQ(reply.status, 200);
    CHECK_HEADERS(reply);
    JsonArrayConst coils = reply.json["coils"];
    CHECK_EQ(coils.size(), w.rig.profile.coilCount());
    CHECK(findByName(coils, "equalize")["on"] == false);
    CHECK(findByName(coils, "load_disconnect")["on"] == true);
    CHECK(findByName(coils, "charge_disconnect")["on"] == false);
    CHECK(findByName(coils, "reset")["trigger"] == true);
    CHECK(findByName(coils, "reset")["on"].isNull());  // an action has no state
    CHECK(reply.json["linkDown"] == false);
    CHECK_EQ(w.rig.sent(0x01), 3);  // the three that have a state, and none of the actions
    CHECK_EQ(w.rig.sent(0x05), 0);
    CHECK(w.rig.sim.triggered.empty());
    // No answer: the first silence is enough.
    w.rig.sim.powered = false;
    const uint64_t start = w.now();
    const Reply dead = w.get("/api/coils");
    CHECK(dead.json["linkDown"] == true);
    CHECK(str(dead.json["coils"][0]["error"]) == "no reply");
    CHECK(str(dead.json["coils"][1]["error"]) == "not read");
    CHECK(str(dead.json["coils"][2]["error"]) == "not read");
    CHECK(dead.json["coils"][3]["trigger"] == true);
    CHECK(w.now() - start < 3500);  // one coil's worth of waiting, not three
  }
  {
    // The snapshot is the console's, as a file to keep.
    WebRig w;
    w.login();
    const Reply snap = w.get("/api/snapshot");
    CHECK_EQ(snap.status, 200);
    CHECK_HEADERS(snap);
    CHECK(snap.header("Content-Type") == "application/json");
    CHECK(snap.header("Content-Disposition") == "attachment; filename=\"optaview-snapshot.json\"");
    CHECK(snap.isJson);
    CHECK(str(snap.json["optaview"]) == OV_VERSION);
    CHECK(str(snap.json["profile"]) == "sunsaver-mppt");
    CHECK_EQ(snap.json["registers"].size(), w.rig.profile.registerCount());
    CHECK_EQ(snap.json["missing"].as<long>(), 0);
    const std::string usb = w.rig.run("snapshot");
    const size_t open = usb.find("{\r\n");
    const size_t close = usb.rfind("}\r\n");
    CHECK(open != std::string::npos && close != std::string::npos);
    CHECK(snap.body == usb.substr(open, close + 3 - open));
    // With the controller off it is still a whole document, and says what is missing.
    w.rig.sim.powered = false;
    const Reply dead = w.get("/api/snapshot");
    CHECK(dead.isJson);
    CHECK_EQ(dead.json["missing"].as<long>(), (long)w.rig.profile.registerCount());
  }
}

// ============================================================================
// The console over HTTP
// ============================================================================
static bool plainConsoleText(const std::string &text) {
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (c == '\n') {
      if (i == 0 || text[i - 1] != '\r') {
        return false;
      }
    } else if (c != '\r' && (c < 0x20 || c > 0x7E)) {
      return false;
    }
  }
  return true;
}

static void testWebConsole() {
  WebRig w;
  w.login();
  {
    // A command that reads: the console's text, word for word as on USB, and the state after it.
    const Reply live = w.console("live");
    CHECK_EQ(live.status, 200);
    CHECK_HEADERS(live);
    CHECK(live.header("Content-Type") == "text/plain; charset=utf-8");
    CHECK(live.isJson);
    CHECK(live.json["awaiting"] == false);
    CHECK(live.json["question"].isNull());
    CHECK(live.json["cut"] == false);
    CHECK_EQ(live.json["state"]["counters"]["sent"].as<unsigned long>(), w.rig.master.stats.sent);
    CHECK_EQ(live.heartbeats, 0);
    CHECK_HAS(live.text, "adc_vb_f                13.68 V");
    CHECK(live.text == w.rig.run("live"));
    CHECK(plainConsoleText(live.text));
    // The body ends with the JSON: nothing after it.
    CHECK(live.body[live.body.size() - 1] == '}');
    CHECK_EQ(countOf(live.body, "\x1E"), 1);
  }
  const std::string kNotOpen = "That question is no longer open. Nothing was sent.\r\n";
  {
    // A setting: asked, then confirmed with the id of the question.
    const Reply ask = w.console("set EV_lvd 12.00");
    CHECK_HAS(ask.text, "  now  11.50 V");
    CHECK_HAS(ask.text, "  new  12.00 V");
    CHECK_HAS(ask.text, "Before you write:");
    CHECK_HAS(ask.text, "Type yes to write it. Anything else cancels.");
    CHECK(ask.json["awaiting"] == true);
    const std::string id = str(ask.json["question"]);
    CHECK_EQ(id.size(), 8);
    CHECK(isLowerHex(id));
    CHECK_EQ(w.rig.sent(0x06), 0);
    const Reply yes = w.console("yes", id);
    CHECK_HAS(yes.text, "Written and read back: EV_lvd = 12.00 V");
    CHECK(yes.json["awaiting"] == false);
    CHECK(yes.json["state"]["resetNeeded"] == true);
    CHECK_EQ(w.rig.sent(0x06), 1);
    CHECK_EQ(w.rig.sim.regs[0xE022], w.rig.sim.setVolts(12.00));
    // The same click once more (a double click, a request that was sent twice): nothing.
    const Reply again = w.console("yes", id);
    CHECK(again.text == kNotOpen);
    CHECK(again.json["awaiting"] == false);
    CHECK_EQ(w.rig.sent(0x06), 1);
  }
  {
    // An answer with another id (a tab that still shows an older question) changes nothing, and
    // the question that is open stays open for the tab that asked it.
    const Reply ask = w.console("set EV_lvd 12.10");
    const std::string id = str(ask.json["question"]);
    CHECK(w.console("yes", "00000000").text == kNotOpen);
    CHECK(w.console("yes", id.substr(0, 7)).text == kNotOpen);
    CHECK(w.console("yes", "0").text == kNotOpen);
    CHECK_EQ(w.rig.sent(0x06), 1);
    CHECK_HAS(w.console("yes", id).text, "Written and read back: EV_lvd = 12.10 V");
    CHECK_EQ(w.rig.sent(0x06), 2);
  }
  {
    // A "yes" that comes as a command finds nothing to agree to: every new command drops the
    // open question first. So does any other command, without a word about it.
    const Reply ask = w.console("set EV_lvd 12.20");
    const std::string id = str(ask.json["question"]);
    const Reply typed = w.console("yes");
    CHECK_HAS(typed.text, "Nothing is waiting for a yes or a no.");
    CHECK(typed.json["awaiting"] == false);
    CHECK(w.console("yes", id).text == kNotOpen);
    const Reply second = w.console("set EV_lvd 12.20");
    const std::string id2 = str(second.json["question"]);
    CHECK(id2 != id);  // every question has an id of its own
    const Reply live = w.console("live");
    CHECK_HASNT(live.text, "Cancelled");
    CHECK_HAS(live.text, "adc_vb_f");
    CHECK(w.console("yes", id2).text == kNotOpen);
    // A question that replaces a question: only the newer one can be answered.
    const std::string older = str(w.console("set EV_lvd 12.20").json["question"]);
    const std::string newer = str(w.console("set EV_lvr 13.00").json["question"]);
    CHECK(w.console("yes", older).text == kNotOpen);
    CHECK_HAS(w.console("yes", newer).text, "Written and read back: EV_lvr = 13.00 V");
    CHECK_EQ(w.rig.sent(0x06), 3);
    CHECK_EQ(w.rig.sim.regs[0xE022], w.rig.sim.setVolts(12.10));
  }
  {
    // Only "yes" is a yes.
    std::string id = str(w.console("set EV_lvd 12.30").json["question"]);
    CHECK(w.console("no", id).text == "Cancelled. Nothing was sent.\r\n");
    CHECK(w.console("yes", id).text == kNotOpen);
    id = str(w.console("set EV_lvd 12.30").json["question"]);
    CHECK(w.console("yes please", id).text == "Cancelled. Nothing was sent.\r\n");
    id = str(w.console("set EV_lvd 12.30").json["question"]);
    CHECK(w.console("y", id).text == "Cancelled. Nothing was sent.\r\n");
    CHECK_EQ(w.rig.sent(0x06), 3);
    id = str(w.console("set EV_lvd 12.30").json["question"]);
    CHECK_HAS(w.console("  YES ", id).text, "Written and read back: EV_lvd = 12.30 V");
    CHECK_EQ(w.rig.sent(0x06), 4);
  }
  {
    // The USB console changed something between the question and the click: nothing is sent.
    const std::string id = str(w.console("set EV_lvd 12.40").json["question"]);
    w.rig.run("set EV_lvr 13.10");
    w.rig.run("yes");
    const Reply late = w.console("yes", id);
    CHECK_HAS(late.text, "Not sent: the controller or the link was changed from elsewhere after this was asked.");
    CHECK_EQ(w.rig.sim.regs[0xE022], w.rig.sim.setVolts(12.30));
    // And the other way round.
    w.rig.run("set EV_lvd 12.40");
    const std::string web = str(w.console("set EV_lvr 13.20").json["question"]);
    CHECK_HAS(w.console("yes", web).text, "Written and read back: EV_lvr = 13.20 V");
    CHECK_HAS(w.rig.run("yes"), "Not sent: the controller or the link was changed from elsewhere");
    CHECK_EQ(w.rig.sim.regs[0xE022], w.rig.sim.setVolts(12.30));
    // A question on each side at once is no trouble as long as only one of them is carried out.
    CHECK(!w.rig.console.awaitingConfirmation());
  }
  {
    // What the console refuses on USB it refuses here: limits, pair checks, unknown names.
    CHECK_HAS(w.console("set EV_lvd 99").text, "That is above what this profile allows for EV_lvd");
    CHECK_HAS(w.console("set nothing 1").text, "nothing");
    CHECK_HAS(w.console("raw 1 6 0xE022 0x1000").text, "Only functions 1 to 4 are open here.");
    const Reply unknown = w.console("frobnicate");
    CHECK_HAS(unknown.text, "Unknown command 'frobnicate'");
    CHECK(unknown.json["awaiting"] == false);
    // The commands of the USB side's extras are not here: the PIN and the network address are
    // not for a browser to change.
    CHECK(w.rig.console.addExtra(&w.web));
    CHECK_HAS(w.console("web pin 0000").text, "Unknown command 'web'");
    CHECK_HAS(w.console("net static 10.0.0.1").text, "Unknown command 'net'");
    CHECK_HAS(w.console("help").text, "  set <name> <value>");
    CHECK_HASNT(w.console("help").text, "web pin");
    CHECK(w.web.sessionOpen());
  }
  {
    // A write that gets no answer, and a read-back that gets none either: the page is told that
    // a restart may be due, not that the write failed. The controller did store the value here.
    WebRig lost;
    lost.login();
    const Reply ask = lost.console("set EV_lvr 13.25");
    lost.rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    lost.rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 2);
    const Reply yes = lost.console("yes", str(ask.json["question"]));
    CHECK_HAS(yes.text, "The controller did not acknowledge the write (no reply).\r\n"
                        "Reading it back failed too (no reply): whether it was written is not known.\r\n");
    CHECK_HASNT(yes.text, "The write failed");
    CHECK(yes.json["state"]["resetMaybe"] == true);
    CHECK(yes.json["state"]["resetNeeded"] == false);
    CHECK(lost.get("/api/state").json["resetMaybe"] == true);
    // Reading other registers settles nothing. Reading the settings, as the page does when the
    // dialog closes, shows the new value: now the restart is known to be due.
    const Reply live = lost.get("/api/read?groups=live");
    CHECK(live.json["state"]["resetMaybe"] == true);
    const Reply load = lost.get("/api/read?groups=load");
    CHECK(load.json["state"]["resetNeeded"] == true);
    CHECK(load.json["state"]["resetMaybe"] == false);
    const Reply reset = lost.console("reset");
    const Reply done = lost.console("yes", str(reset.json["question"]));
    CHECK(done.json["state"]["resetNeeded"] == false);
    CHECK(done.json["state"]["resetMaybe"] == false);

    // The other way round: the write never reached the controller. The same read shows the old
    // value, and nothing is due.
    WebRig silent;
    silent.login();
    const Reply again = silent.console("set EV_lvr 13.25");
    silent.rig.sim.failNext(SimSunSaver::FAULT_NO_REPLY, 3);
    const Reply unknown = silent.console("yes", str(again.json["question"]));
    CHECK(unknown.json["state"]["resetMaybe"] == true);
    const Reply settled = silent.get("/api/read?groups=load");
    CHECK(settled.json["state"]["resetMaybe"] == false);
    CHECK(settled.json["state"]["resetNeeded"] == false);
    // A switch whose acknowledgement is lost is read back, and the page is told what it reads.
    const Reply coil = silent.console("coil load_disconnect on");
    silent.rig.sim.failNext(SimSunSaver::FAULT_LOST_REPLY);
    const Reply switched = silent.console("yes", str(coil.json["question"]));
    CHECK_HAS(switched.text, "Sent, but the controller did not acknowledge it (no reply).\r\n"
                             "load_disconnect now reads on: it was switched.\r\n");
    CHECK(switched.json["state"]["resetMaybe"] == false);
  }
  {
    // A coil, and the restart.
    std::string id = str(w.console("coil load_disconnect on").json["question"]);
    CHECK_EQ(id.size(), 8);
    CHECK(!w.rig.sim.coils[0x0001]);
    CHECK_HAS(w.console("yes", id).text, "Sent and acknowledged.");
    CHECK(w.rig.sim.coils[0x0001]);
    const Reply ask = w.console("reset");
    CHECK_HAS(ask.text, "Type yes to restart it. Anything else cancels.");
    CHECK_EQ(w.rig.sim.resets, 0);
    const Reply done = w.console("yes", str(ask.json["question"]));
    CHECK_EQ(w.rig.sim.resets, 1);
    CHECK_HAS(done.text, "Restarting the controller.\r\n");
    CHECK_HASNT(done.text, "Press a key");  // there are no keys on a web page
    CHECK_HAS(done.text, "The controller answers again after");
    CHECK(done.json["state"]["resetNeeded"] == false);
    CHECK(done.json["cut"] == false);
    CHECK(done.heartbeats >= 1);  // it took more than a second, and said so meanwhile
    CHECK(plainConsoleText(done.text));
  }
  {
    // Commands that are stopped with a key on USB.
    const Reply watch = w.console("watch");
    CHECK(watch.text == "'watch' runs until a key is pressed, and there are no keys here. 'live' reads once.\r\n");
    CHECK(w.console("WATCH 2").text == watch.text);
    // ...and 'help' does not offer it here, while the USB console's still does.
    const std::string help = w.console("help").text;
    CHECK_HAS(help, "\r\n  live  ");
    CHECK_HASNT(help, "watch");
    CHECK_HASNT(help, "key is pressed");
    CHECK_HAS(w.rig.run("help"), "\r\n  watch [seconds]  ");
    const Reply sniff = w.console("sniff 300");
    CHECK_HAS(sniff.text, "Listening for 300 ms without sending.\r\n");
    CHECK_HAS(sniff.text, "Heard 0 bytes.");
    const Reply scan = w.console("scan 1 3");
    CHECK_HAS(scan.text, "Asking slaves 1 to 3 at 9600 8N2.\r\n");
    CHECK_HASNT(scan.text, "Press a key");
    const Reply probe = w.console("probe");
    CHECK_HASNT(probe.text, "Press a key");
    CHECK_HAS(probe.text, "(0x0008).\r\n");
  }
  {
    // What is not a command line is refused before the console sees it.
    const size_t sent = w.rig.sim.requests.size();
    const std::string id = str(w.console("set EV_lvd 12.50").json["question"]);
    const char *bad[] = {"",
                         "live",
                         "{\"line\":\"live\"",
                         "{}",
                         "[]",
                         "{\"line\":null}",
                         "{\"line\":5}",
                         "{\"line\":[\"live\"]}",
                         "{\"line\":\"\"}",
                         "{\"line\":\"   \"}",
                         "{\"line\":\"live\\ndump\"}",
                         "{\"line\":\"live\\tstatus\"}",
                         "{\"line\":\"live\\u0000dump\"}",
                         "{\"line\":\"caf\xC3\xA9\"}",
                         "{\"line\":\"live\\u007f\"}",
                         "{\"line\":\"yes\",\"answer\":\"\"}",
                         "{\"line\":\"yes\",\"answer\":\"123456789\"}",
                         "{\"line\":\"yes\",\"answer\":12345678}",
                         "{\"line\":\"yes\",\"answer\":[\"x\"]}",
                         "{\"line\":\"yes\",\"answer\":true}",
                         "{\"Line\":\"live\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      const Reply reply = w.post("/api/console", bad[i]);
      CHECK_MSG(reply.status == 400 && (str(reply.json["error"]) == "line" || str(reply.json["error"]) == "body"),
                "%s gave %d", bad[i], reply.status);
    }
    CHECK_ERROR(w.post("/api/console", "not json"), 400, "body");
    CHECK_ERROR(w.post("/api/console", "{\"line\":\"" + std::string(160, 'x') + "\"}"), 400, "line");
    CHECK_HAS(w.console(std::string(159, 'x')).text, "Unknown command");
    // None of the refused ones touched the question that was open; the last one, a command, did.
    CHECK_EQ(w.rig.sim.requests.size(), sent + 2);
    CHECK(w.console("yes", id).text == kNotOpen);
    CHECK_EQ(w.rig.sent(0x06), 6);
    // "answer": null is the same as no answer: a command.
    CHECK_HAS(w.post("/api/console", "{\"line\":\"yes\",\"answer\":null}").body, "Nothing is waiting for a yes or a no.");
  }
  {
    // Whoever logs in next finds a console that nobody has used: no open question, no expert
    // mode, no trace.
    const std::string id = str(w.console("set EV_lvd 12.60").json["question"]);
    w.console("expert on");
    const std::string expert = str(w.console("set EV_lvd 12.60").json["question"]);
    CHECK_HAS(w.console("status").text, "expert on");
    w.post("/api/logout", "");
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
    CHECK(w.console("yes", expert).text == kNotOpen);
    CHECK(w.console("yes", id).text == kNotOpen);
    CHECK_HAS(w.console("status").text, "expert off, trace off");
    CHECK_HAS(w.console("set EV_lvd 12.60").text, "Before you write:");  // and the notes once more
    CHECK_EQ(w.rig.sent(0x06), 6);
    // The same when the session is ended from the USB side, or runs out.
    const std::string open = str(w.console("set EV_lvd 12.60").json["question"]);
    w.rig.run("web logout");
    CHECK_ERROR(w.post("/api/console", "{\"line\":\"yes\",\"answer\":\"" + open + "\"}"), 401, "login");
    w.cookie.clear();
    CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
    CHECK(w.console("yes", open).text == kNotOpen);
    CHECK_EQ(w.rig.sent(0x06), 6);
  }
  CHECK(plainConsoleText(gTranscript));
}

static void testWebTrace() {
  // The trace of each console shows that console's own traffic, and never gets into JSON.
  WebRig w;
  w.login();
  CHECK_HAS(w.rig.run("trace on"), "Trace is on");
  w.rig.out.text.clear();
  const Reply read = w.get("/api/read?groups=live");
  CHECK(read.isJson);
  CHECK(w.rig.out.text.empty());  // nothing appeared on the USB side
  const Reply quiet = w.console("get adc_vb_f");
  CHECK_HASNT(quiet.text, "> 01 04");
  CHECK(w.rig.out.text.empty());
  CHECK(w.rig.master.trace == &w.rig.console);  // and it is still on for USB
  CHECK_HAS(w.rig.run("get adc_vb_f"), "> 01 04");

  CHECK_HAS(w.console("trace on").text, "Trace is on");
  CHECK(w.rig.master.trace == &w.rig.console);  // between requests the USB console's is in place
  w.rig.out.text.clear();
  const Reply traced = w.console("get adc_vb_f");
  CHECK_HAS(traced.text, "> 01 04 00 08 00 01");
  CHECK_HAS(traced.text, "< 01 04 02");
  CHECK(w.rig.out.text.empty());
  CHECK(w.get("/api/read?groups=live").isJson);
  CHECK(w.get("/api/coils").isJson);
  CHECK(w.get("/api/snapshot").isJson);
  CHECK_HAS(w.rig.run("trace off"), "Trace is off");
  CHECK(w.rig.master.trace == nullptr);
  CHECK_HAS(w.console("get adc_vb_f").text, "> 01 04");  // the web console's is still on
  CHECK(w.rig.master.trace == nullptr);
  CHECK_HASNT(w.rig.run("get adc_vb_f"), "> 01 04");
  CHECK_HAS(w.console("trace off").text, "Trace is off");
  CHECK_HASNT(w.console("get adc_vb_f").text, "> 01 04");
  // Switched on in the browser and never off: the next visitor does not inherit it.
  w.console("trace on");
  w.post("/api/logout", "");
  CHECK_EQ(w.post("/api/login", "{\"pin\":\"2468\"}").status, 200);
  CHECK_HASNT(w.console("get adc_vb_f").text, "> 01 04");
  CHECK(w.rig.master.trace == nullptr);
}

// ============================================================================
// Commands that take long, and browsers that do not wait for them
// ============================================================================
static void testWebLongCommands() {
  {
    // While a command works and has nothing to print, a byte goes out every second (or after
    // the request to the controller that is being waited for, which can make it two). It means
    // nothing to the page; it is there so that a browser that has gone is noticed.
    WebRig w;
    w.login();
    w.rig.sim.resetDownMs = 6000;
    const Reply ask = w.console("reset");
    const uint64_t start = w.now();
    const Reply done = w.console("yes", str(ask.json["question"]));
    CHECK_HAS(done.text, "The controller answers again after");
    CHECK(done.heartbeats >= 4 && done.heartbeats <= 8);
    CHECK(done.json["cut"] == false);
    CHECK(w.now() - start >= 6000);
    CHECK(plainConsoleText(done.text));
    // Never much more than two seconds without something being sent.
    uint64_t longest = 0;
    for (size_t i = 1; i < w.last->sentAt.size(); ++i) {
      const uint64_t gap = w.last->sentAt[i] - w.last->sentAt[i - 1];
      longest = gap > longest ? gap : longest;
    }
    CHECK_MSG(longest <= 2100, "%llu ms without a byte", (unsigned long long)longest);
  }
  {
    // An answer in JSON takes no such byte: one inside it would break it. While the server
    // reads for a JSON answer there is always text waiting to go out, and a byte that means
    // nothing is only ever added when there is none. The one exception is the moment at which
    // the text of a block has just filled the buffer to its last byte and gone out by itself.
    // The test makes that moment happen: sixteen registers in one block, their names made
    // longer until the block's text is exactly one buffer, on a controller that takes two
    // seconds to reply.
    size_t pad = 0;
    for (int round = 0; round < 2; ++round) {
      const std::string profile = wideProfile(pad);
      WebRig w(WebRig::TEST_PAGE, profile.c_str());
      w.login();
      CHECK_EQ(w.console("timeout 3000").status, 200);
      w.rig.sim.responseDelayMs = 2000;
      const uint64_t start = w.now();
      const Reply read = w.get("/api/read?groups=g");
      CHECK(w.now() - start >= 2000);
      std::string clean;
      for (size_t i = 0; i < w.last->sent.size(); ++i) {
        if (w.last->sent[i] != OV_WEB_HEARTBEAT) {
          clean += w.last->sent[i];
        }
      }
      const std::string opens = "\"registers\":[";
      const size_t from = clean.find(opens);
      const size_t to = clean.find("]}],\"linkDown\"");
      CHECK(from != std::string::npos && to != std::string::npos && to > from);
      if (from == std::string::npos || to == std::string::npos || to <= from) {
        break;
      }
      const size_t block = to - (from + opens.size());
      if (round == 0) {
        CHECK(block < 1024);
        pad = block < 1024 ? 1024 - block : 0;
        continue;
      }
      CHECK_EQ(block, 1024);
      CHECK_EQ(w.last->sentSizes.size(), 3);  // the head, the block in one full buffer, the rest
      CHECK_EQ(w.last->sentSizes.size() > 1 ? w.last->sentSizes[1] : 0, 1024);
      CHECK(w.last->sent.find(OV_WEB_HEARTBEAT) == std::string::npos);
      CHECK(read.isJson);
      CHECK_EQ(read.json["groups"][0]["registers"].size(), 16);
      CHECK(read.json["stopped"] == false);
    }
  }
  {
    // The browser goes away during the wait: the command is stopped within about a second,
    // and the Opta is free for the next request. The restart itself had been sent before.
    WebRig w;
    w.login();
    w.rig.sim.resetDownMs = 15000;
    const Reply ask = w.console("reset");
    FakeConn *gone = w.connect();
    Ask yes;
    yes.method = "POST";
    yes.target = "/api/console";
    yes.body = "{\"line\":\"yes\",\"answer\":\"" + str(ask.json["question"]) + "\"}";
    gone->feed(w.build(yes));
    gone->goneAt = w.now() + 3000;
    const uint64_t start = w.now();
    w.serve(gone);
    CHECK(w.now() - start < 4500);
    CHECK_EQ(gone->closes, 1);
    CHECK_EQ(gone->writesAfterFailure, 0);
    CHECK_EQ(w.rig.sim.resets, 1);
    // What comes next is answered in full: the stop belonged to that one request.
    w.idle(15000);
    const Reply after = w.console("live");
    CHECK(after.json["cut"] == false);
    CHECK_HAS(after.text, "adc_vb_f                13.68 V");
    CHECK_HAS(after.text, "led_state");
    const Reply read = w.get("/api/read?groups=live,status");
    CHECK(read.json["stopped"] == false);
    CHECK_EQ(read.json["groups"][1]["registers"].size(), 10);
  }
  {
    // The browser stops taking the answer during the wait, without closing the connection (a
    // cable was pulled). Nothing tells the Opta that it is gone, so the command is stopped when
    // a byte for it has waited five seconds.
    WebRig w;
    w.login();
    w.rig.sim.resetDownMs = 40000;
    const Reply ask = w.console("reset");
    FakeConn *deaf = w.connect();
    Ask yes;
    yes.method = "POST";
    yes.target = "/api/console";
    yes.body = "{\"line\":\"yes\",\"answer\":\"" + str(ask.json["question"]) + "\"}";
    deaf->feed(w.build(yes));
    deaf->deafFrom = w.now() + 3000;
    const uint64_t start = w.now();
    w.serve(deaf);
    CHECK(w.now() - start >= 3000 + OV_HTTP_SEND_STALL_MS && w.now() - start < 4600 + OV_HTTP_SEND_STALL_MS);
    CHECK_EQ(deaf->closes, 1);
    CHECK_EQ(deaf->writesAfterClose, 0);
    CHECK_EQ(w.rig.sim.resets, 1);
    CHECK(deaf->sent.find(OV_WEB_TRAILER) == std::string::npos);  // the page sees an answer that broke off
    w.idle(40000);
    const Reply after = w.console("live");
    CHECK(after.json["cut"] == false);
    CHECK_HAS(after.text, "led_state");
  }
  {
    // One minute is the most a command may work.
    WebRig w;
    w.login();
    const uint64_t start = w.now();
    const Reply sniff = w.console("sniff 600000");
    CHECK(sniff.json["cut"] == true);
    CHECK_HAS(sniff.text, "Stopped.");
    CHECK(w.now() - start >= 60000 && w.now() - start < 61500);
    CHECK(sniff.heartbeats >= 55 && sniff.heartbeats <= 61);
    const Reply after = w.console("live");
    CHECK(after.json["cut"] == false);
    CHECK_HAS(after.text, "led_state");
    // A scan of every address would take longer than that as well.
    w.rig.sim.powered = false;
    const uint64_t scanStart = w.now();
    const Reply scan = w.console("scan 1 247");
    CHECK(scan.json["cut"] == true);
    CHECK(w.now() - scanStart >= 60000 && w.now() - scanStart < 62000);
    CHECK(plainConsoleText(scan.text));
  }
  {
    // While a long request is being answered, another one waits its turn and is answered then.
    WebRig w;
    w.login();
    w.rig.sim.resetDownMs = 8000;
    const Reply ask = w.console("reset");
    FakeConn *slow = w.connect();
    Ask yes;
    yes.method = "POST";
    yes.target = "/api/console";
    yes.body = "{\"line\":\"yes\",\"answer\":\"" + str(ask.json["question"]) + "\"}";
    slow->feed(w.build(yes));
    FakeConn *waiting = w.connect();
    Ask state;
    state.target = "/api/state";
    waiting->feed(w.build(state), 500);
    w.serve(slow);
    CHECK_EQ(waiting->closes, 0);
    CHECK(waiting->sent.empty());
    w.serve(waiting);
    const Reply answer = parseReply(waiting->sent);
    CHECK_EQ(answer.status, 200);
    CHECK(answer.json["resetNeeded"] == false);
  }
}

// ============================================================================
// 'web' on the USB console
// ============================================================================
static void testWebUsbCommands() {
  WebRig w;
  CHECK(w.rig.console.addExtra(&w.web));
  CHECK(w.rig.run("web") ==
        "Web page  http://192.168.1.57/, no PIN yet: the first visitor chooses it\r\n"
        "          0 requests answered since power-up\r\n");
  w.host.networkUp = false;
  CHECK_HAS(w.rig.run("web"), "Web page  not reachable while the network is down, no PIN yet: the first visitor chooses it\r\n");
  w.host.networkUp = true;
  const char kUsage[] = "Usage: web   |   web pin <4 to 8 digits>   |   web pin clear   |   web logout\r\n";
  CHECK(w.rig.run("web pin") == kUsage);
  CHECK(w.rig.run("web login") == kUsage);
  CHECK(w.rig.run("web pin 1234 5678") == kUsage);
  CHECK(w.rig.run("web logout now") == kUsage);
  CHECK(w.rig.run("web pin 12") == "A PIN is 4 to 8 digits.\r\n");
  CHECK(w.rig.run("web pin 123456789") == "A PIN is 4 to 8 digits.\r\n");
  CHECK(w.rig.run("web pin 12a4") == "A PIN is 4 to 8 digits.\r\n");
  CHECK(!w.web.pinSet());
  CHECK(w.rig.run("web logout") == "Nobody is logged in.\r\n");
  CHECK(w.rig.run("web pin 1357") ==
        "The PIN is set. Whoever was logged in has to log in again.\r\n"
        "It lasts until power-off; OptaViewConfig.h sets the PIN that OptaView starts with.\r\n");
  CHECK(w.web.pinSet());
  CHECK_HAS(w.rig.run("status"), "Web page  http://192.168.1.57/, PIN set, nobody logged in\r\n");
  CHECK_EQ(w.post("/api/login", "{\"pin\":\"1357\"}").status, 200);
  w.idle(4200);
  CHECK_HAS(w.rig.run("web"), "Web page  http://192.168.1.57/, PIN set, a browser is logged in (last used 4 s ago)\r\n");
  CHECK_HAS(w.rig.run("web"), "          1 request answered since power-up\r\n");
  CHECK(w.rig.run("WEB LOGOUT") == "The browser's session is ended. It has to log in again.\r\n");
  CHECK_ERROR(w.get("/api/state"), 401, "login");
  CHECK_HAS(w.rig.run("web"), "          2 requests answered since power-up\r\n");
  // Wrong PINs show up here, with the way out.
  w.cookie.clear();
  w.post("/api/login", "{\"pin\":\"0000\"}");
  w.idle(1100);
  w.post("/api/login", "{\"pin\":\"0000\"}");
  CHECK_HAS(w.rig.run("web"),
            "          Logins are refused for another 2 s after 2 wrong PINs. 'web pin' sets a new one and ends the wait.\r\n");
  w.idle(2100);
  CHECK_HASNT(w.rig.run("web"), "Logins are refused");
  CHECK(w.rig.run("web pin clear") == "There is no PIN now. The next visitor of the web page chooses one.\r\n");
  CHECK(!w.web.pinSet());
  const std::string help = w.rig.run("help");
  CHECK_HAS(help, "Web page\r\n");
  CHECK_HAS(help, "  web logout              end the browser's session\r\n");
  CHECK_EQ(w.rig.sim.requests.size(), 0);
  // A PIN typed at the USB console is in the console's transcript; that is the USB port's own
  // business. What matters is that the browser side never sees one.
  CHECK(w.web.setPin("97531246"));
  CHECK_EQ(w.post("/api/login", "{\"pin\":\"97531246\"}").status, 200);
  const char *everything[] = {"/api/session", "/api/state", "/api/profile", "/api/read?groups=live", "/api/coils",
                              "/api/snapshot", "/"};
  for (size_t i = 0; i < sizeof(everything) / sizeof(everything[0]); ++i) {
    CHECK_HASNT(w.get(everything[i]).body, "97531246");
  }
  CHECK_HASNT(w.console("status").body, "97531246");
}

int main() {
  startTestTimeLimit();
  testHttpReader();
  testHttpHelpers();
  testHttpResponse();
  testJsonWriter();
  testIpHelpers();
  testNetPolicy();
  testEmbeddedPage();
  testWebRouting();
  testWebConnections();
  testWebLogin();
  testWebReading();
  testWebConsole();
  testWebTrace();
  testWebLongCommands();
  testWebUsbCommands();

  if (gAllocLive != 0) {
    fprintf(stderr, "optaview web: %ld profile allocations were never freed\n", gAllocLive);
    return 1;
  }
  if (gFailures != 0) {
    fprintf(stderr, "\noptaview web: %ld of %ld checks FAILED\n", gFailures, gChecks);
    return 1;
  }
  printf("optaview web: all %ld checks passed\n", gChecks);
  return 0;
}
