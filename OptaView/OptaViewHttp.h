/**
 * OptaViewHttp.h
 *
 * The little HTTP that OptaView's web page needs: one request per connection, read with limits
 * on everything, and one answer that ends when the connection closes. No Arduino dependency: a
 * connection is reached through OvConn, so the host tests feed requests as strings and the
 * sketch (OptaView.ino) puts the Opta's Ethernet client behind it.
 *
 * What it deliberately does not do: keep-alive, chunked bodies in either direction, percent
 * decoding, multipart forms, TLS. The page (web/index.html) is written to need none of them.
 *
 * Limits (a request that breaks one is refused, never cut to fit):
 *   request line or one header line   1024 bytes
 *   path                                63 bytes    query   127 bytes
 *   headers                             64
 *   body                              1024 bytes, and only with Content-Length
 *   silence inside a request             2 s        whole request   6 s
 *
 * And on the way out (an answer that breaks one is given up and the connection closed):
 *   the other end takes nothing          5 s        sending one answer, in all   30 s
 *
 * Nothing here waits without a limit. One connection can therefore keep the Opta's single loop
 * for a bounded time only, whatever the other end does: send without end, or read a byte now
 * and then.
 */

#ifndef OPTAVIEW_HTTP_H
#define OPTAVIEW_HTTP_H

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OptaViewText.h"

namespace optaview {

// One TCP connection, as the web server sees it. Neither read() nor write() may wait: all the
// waiting is done in this file, on OvClock and against a limit.
class OvConn {
 public:
  virtual ~OvConn() {}
  // Next received byte, or -1 when none is waiting right now.
  virtual int read() = 0;
  // Hands over as much of data as the connection takes right now and returns how many bytes
  // that was: 0 when it takes none at the moment, -1 when the other end is known to be gone.
  virtual int write(const uint8_t *data, size_t len) = 0;
  // Ends the connection. What was handed over before is still delivered.
  virtual void close() = 0;
};

class OvClock {
 public:
  virtual ~OvClock() {}
  // Monotonic milliseconds; may wrap like millis().
  virtual uint32_t nowMs() = 0;
  // Called while waiting for bytes. Time must advance across calls.
  virtual void idle() = 0;
};

static const size_t OV_HTTP_LINE_MAX = 1024;
static const size_t OV_HTTP_PATH_MAX = 64;    // with the terminating NUL
static const size_t OV_HTTP_QUERY_MAX = 128;  // with the terminating NUL
static const size_t OV_HTTP_BODY_MAX = 1024;
static const size_t OV_HTTP_HOST_MAX = 48;
static const size_t OV_HTTP_COOKIE_MAX = 48;
static const uint8_t OV_HTTP_MAX_HEADERS = 64;
static const uint32_t OV_HTTP_GAP_MS = 2000;
static const uint32_t OV_HTTP_REQUEST_MS = 6000;
static const uint32_t OV_HTTP_SEND_STALL_MS = 5000;  // the other end takes nothing for this long: it is gone
static const uint32_t OV_HTTP_SEND_MS = 30000;       // all the time one answer may spend being sent

enum OvHttpParse : uint8_t {
  OV_HTTP_OK = 0,
  OV_HTTP_TIMEOUT,          // the request stopped arriving: there is nobody to answer
  OV_HTTP_BAD_REQUEST,      // 400
  OV_HTTP_LENGTH_REQUIRED,  // 411: a POST without Content-Length
  OV_HTTP_BODY_TOO_LARGE,   // 413
  OV_HTTP_TARGET_TOO_LONG,  // 414
  OV_HTTP_HEADERS_TOO_BIG,  // 431: a header line over the limit, or too many of them
  OV_HTTP_NOT_IMPLEMENTED   // 501: a transfer coding
};

struct OvHttpRequest {
  char method[8];
  char path[OV_HTTP_PATH_MAX];
  char query[OV_HTTP_QUERY_MAX];      // what follows the '?', not decoded
  char host[OV_HTTP_HOST_MAX];        // the Host header; "" when absent or too long to be ours
  char cookie[OV_HTTP_COOKIE_MAX];    // the value of the cookie that was asked for, or ""
  bool marker;                        // the request carried the marker header
  size_t bodyLength;
  char body[OV_HTTP_BODY_MAX + 1];    // NUL-terminated
};

inline const char *ovHttpReason(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 421: return "Misdirected Request";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

inline int ovHttpStatusFor(OvHttpParse parse) {
  switch (parse) {
    case OV_HTTP_OK: return 200;
    case OV_HTTP_LENGTH_REQUIRED: return 411;
    case OV_HTTP_BODY_TOO_LARGE: return 413;
    case OV_HTTP_TARGET_TOO_LONG: return 414;
    case OV_HTTP_HEADERS_TOO_BIG: return 431;
    case OV_HTTP_NOT_IMPLEMENTED: return 501;
    default: return 400;
  }
}

// True when the two texts are the same. The time it takes depends on `expected` alone, so a
// caller who is guessing a PIN or a session token learns nothing from how long a wrong one took.
inline bool ovSameSecret(const char *expected, const char *given) {
  const size_t n = strlen(expected);
  const size_t m = strlen(given);
  unsigned char diff = (unsigned char)(n != m ? 1 : 0);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char g = i < m ? (unsigned char)given[i] : (unsigned char)0;
    diff = (unsigned char)(diff | ((unsigned char)expected[i] ^ g));
  }
  return diff == 0 && n > 0;
}

// The value of `name` in a query string ("groups=live,status&auto=1"), copied as it stands.
// Returns false when it is absent or does not fit.
inline bool ovQueryValue(const char *query, const char *name, char *out, size_t cap) {
  const size_t nameLen = strlen(name);
  const char *p = query;
  while (*p != '\0') {
    const char *end = strchr(p, '&');
    const size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
    if (len > nameLen && p[nameLen] == '=' && strncmp(p, name, nameLen) == 0) {
      const size_t valueLen = len - nameLen - 1;
      if (valueLen + 1 > cap) {
        return false;
      }
      memcpy(out, p + nameLen + 1, valueLen);
      out[valueLen] = '\0';
      return true;
    }
    if (end == nullptr) {
      break;
    }
    p = end + 1;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Reading a request
// ---------------------------------------------------------------------------------------------

class OvHttpReader {
 public:
  OvHttpReader(OvConn &conn, OvClock &clock) : conn_(conn), clock_(clock), startMs_(0), lastByteMs_(0), first_(-1) {}

  // Reads one request. `first` is a byte that was already taken from the connection (-1 for
  // none). `cookieName` names the cookie to pick out of the Cookie header, `markerName` (lower
  // case) the header whose presence request->marker reports.
  OvHttpParse read(int first, const char *cookieName, const char *markerName, OvHttpRequest *request) {
    memset(request, 0, sizeof(*request));
    startMs_ = clock_.nowMs();
    lastByteMs_ = startMs_;
    first_ = first;

    size_t len = 0;
    LineResult got = readLine(&len);
    if (got == LINE_TIMEOUT) {
      return OV_HTTP_TIMEOUT;
    }
    if (got == LINE_TOO_LONG) {
      return OV_HTTP_TARGET_TOO_LONG;
    }
    if (got == LINE_BAD) {
      return OV_HTTP_BAD_REQUEST;
    }
    const OvHttpParse startLine = parseRequestLine(request);
    if (startLine != OV_HTTP_OK) {
      return startLine;
    }

    bool haveLength = false;
    bool haveHost = false;
    bool coded = false;
    uint32_t contentLength = 0;
    for (uint8_t headers = 0;; ++headers) {
      got = readLine(&len);
      if (got == LINE_TIMEOUT) {
        return OV_HTTP_TIMEOUT;
      }
      if (got == LINE_TOO_LONG) {
        return OV_HTTP_HEADERS_TOO_BIG;
      }
      if (got == LINE_BAD) {
        return OV_HTTP_BAD_REQUEST;
      }
      if (len == 0) {
        break;  // the empty line that ends the headers
      }
      if (headers >= OV_HTTP_MAX_HEADERS) {
        return OV_HTTP_HEADERS_TOO_BIG;
      }
      if (line_[0] == ' ' || line_[0] == '\t') {
        return OV_HTTP_BAD_REQUEST;  // a folded header line: obsolete, and a way to hide one
      }
      char *colon = strchr(line_, ':');
      if (colon == nullptr || colon == line_) {
        return OV_HTTP_BAD_REQUEST;
      }
      *colon = '\0';
      for (char *p = line_; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '\t') {
          return OV_HTTP_BAD_REQUEST;  // "Name : value" is not allowed
        }
        *p = (char)tolower((unsigned char)*p);
      }
      char *value = colon + 1;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      size_t valueLen = strlen(value);
      while (valueLen > 0 && (value[valueLen - 1] == ' ' || value[valueLen - 1] == '\t')) {
        value[--valueLen] = '\0';
      }

      if (strcmp(line_, "content-length") == 0) {
        uint32_t parsed = 0;
        if (!parseLength(value, &parsed) || (haveLength && parsed != contentLength)) {
          return OV_HTTP_BAD_REQUEST;
        }
        haveLength = true;
        contentLength = parsed;
      } else if (strcmp(line_, "transfer-encoding") == 0) {
        coded = true;
      } else if (strcmp(line_, "host") == 0) {
        if (haveHost) {
          return OV_HTTP_BAD_REQUEST;
        }
        haveHost = true;
        if (valueLen < sizeof(request->host)) {
          ovCopy(request->host, sizeof(request->host), value);
        }
      } else if (strcmp(line_, "cookie") == 0) {
        pickCookie(value, cookieName, request);
      } else if (markerName != nullptr && strcmp(line_, markerName) == 0) {
        request->marker = true;
      }
    }

    if (coded) {
      return OV_HTTP_NOT_IMPLEMENTED;
    }
    if (strcmp(request->method, "POST") == 0 && !haveLength) {
      return OV_HTTP_LENGTH_REQUIRED;
    }
    if (contentLength > OV_HTTP_BODY_MAX) {
      return OV_HTTP_BODY_TOO_LARGE;
    }
    for (uint32_t i = 0; i < contentLength; ++i) {
      const int b = next();
      if (b < 0) {
        return OV_HTTP_TIMEOUT;
      }
      request->body[i] = (char)b;
    }
    request->bodyLength = contentLength;
    request->body[contentLength] = '\0';
    return OV_HTTP_OK;
  }

 private:
  enum LineResult : uint8_t { LINE_OK, LINE_TIMEOUT, LINE_TOO_LONG, LINE_BAD };

  OvConn &conn_;
  OvClock &clock_;
  uint32_t startMs_;
  uint32_t lastByteMs_;
  int first_;
  char line_[OV_HTTP_LINE_MAX + 1];

  // The next byte of the request, or -1 when it stopped arriving or has taken too long.
  int next() {
    if (first_ >= 0) {
      const int b = first_;
      first_ = -1;
      return b;
    }
    for (;;) {
      const uint32_t now = clock_.nowMs();
      if ((uint32_t)(now - startMs_) >= OV_HTTP_REQUEST_MS) {
        return -1;  // also when bytes keep coming: a request is not allowed to go on for ever
      }
      const int b = conn_.read();
      if (b >= 0) {
        lastByteMs_ = now;
        return b;
      }
      if ((uint32_t)(now - lastByteMs_) >= OV_HTTP_GAP_MS) {
        return -1;
      }
      clock_.idle();
    }
  }

  // Reads up to the next line feed into line_, without the line ending. It stops at the first
  // byte that does not fit: the caller answers, and ovHttpDrain() takes what is left of the
  // request.
  LineResult readLine(size_t *length) {
    size_t len = 0;
    for (;;) {
      const int b = next();
      if (b < 0) {
        return LINE_TIMEOUT;
      }
      if (b == '\n') {
        break;
      }
      if (len >= OV_HTTP_LINE_MAX) {
        return LINE_TOO_LONG;
      }
      line_[len++] = (char)b;
    }
    if (len > 0 && line_[len - 1] == '\r') {
      --len;
    }
    line_[len] = '\0';
    *length = len;
    for (size_t i = 0; i < len; ++i) {
      const unsigned char c = (unsigned char)line_[i];
      if ((c < 0x20 && c != '\t') || c == 0x7F) {
        return LINE_BAD;  // a control character, a NUL or a lone carriage return
      }
    }
    return LINE_OK;
  }

  // "GET /api/read?groups=live HTTP/1.1"
  OvHttpParse parseRequestLine(OvHttpRequest *request) {
    char *space = strchr(line_, ' ');
    if (space == nullptr || space == line_ || (size_t)(space - line_) >= sizeof(request->method)) {
      return OV_HTTP_BAD_REQUEST;
    }
    *space = '\0';
    for (const char *p = line_; *p != '\0'; ++p) {
      if (*p < 'A' || *p > 'Z') {
        return OV_HTTP_BAD_REQUEST;
      }
    }
    ovCopy(request->method, sizeof(request->method), line_);
    char *target = space + 1;
    char *version = strchr(target, ' ');
    if (version == nullptr) {
      return OV_HTTP_BAD_REQUEST;
    }
    *version++ = '\0';
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
      return OV_HTTP_BAD_REQUEST;
    }
    if (target[0] != '/') {
      return OV_HTTP_BAD_REQUEST;  // an absolute URI, an asterisk, or nothing at all
    }
    for (const char *p = target; *p != '\0'; ++p) {
      const unsigned char c = (unsigned char)*p;
      if (c <= 0x20 || c >= 0x7F) {
        return OV_HTTP_BAD_REQUEST;
      }
    }
    char *query = strchr(target, '?');
    if (query != nullptr) {
      *query++ = '\0';
      if (strlen(query) >= sizeof(request->query)) {
        return OV_HTTP_TARGET_TOO_LONG;
      }
      ovCopy(request->query, sizeof(request->query), query);
    }
    if (strlen(target) >= sizeof(request->path)) {
      return OV_HTTP_TARGET_TOO_LONG;
    }
    ovCopy(request->path, sizeof(request->path), target);
    return OV_HTTP_OK;
  }

  // Digits only: "12", never "+12", "0x0C" or "12, 12".
  static bool parseLength(const char *text, uint32_t *out) {
    if (*text == '\0') {
      return false;
    }
    uint32_t value = 0;
    for (const char *p = text; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9' || value > 100000000UL) {
        return false;
      }
      value = value * 10u + (uint32_t)(*p - '0');
    }
    *out = value;
    return true;
  }

  // Finds "name=value" among the cookies of one Cookie header. The first one that fits counts.
  static void pickCookie(const char *header, const char *name, OvHttpRequest *request) {
    if (name == nullptr || request->cookie[0] != '\0') {
      return;
    }
    const size_t nameLen = strlen(name);
    const char *p = header;
    while (*p != '\0') {
      while (*p == ' ' || *p == ';') {
        ++p;
      }
      const char *end = strchr(p, ';');
      const size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);
      if (len > nameLen && p[nameLen] == '=' && strncmp(p, name, nameLen) == 0) {
        const size_t valueLen = len - nameLen - 1;
        if (valueLen < sizeof(request->cookie)) {
          memcpy(request->cookie, p + nameLen + 1, valueLen);
          request->cookie[valueLen] = '\0';
          return;
        }
      }
      if (end == nullptr) {
        break;
      }
      p = end + 1;
    }
  }
};

// Takes what is still arriving, for a short while, before an early answer is closed. A
// connection that is closed with unread bytes is reset, and a reset can cost the other end the
// answer it was just sent.
inline void ovHttpDrain(OvConn &conn, OvClock &clock, uint32_t quietMs, uint32_t mostMs) {
  const uint32_t start = clock.nowMs();
  uint32_t last = start;
  for (;;) {
    const uint32_t now = clock.nowMs();
    if ((uint32_t)(now - start) >= mostMs) {
      return;  // looked at before every byte: bytes that never stop coming do not get past it
    }
    if (conn.read() >= 0) {
      last = now;
      continue;
    }
    if ((uint32_t)(now - last) >= quietMs) {
      return;
    }
    clock.idle();
  }
}

// ---------------------------------------------------------------------------------------------
// Writing an answer
// ---------------------------------------------------------------------------------------------

// The answer to one request. It is an OvOut, so the console and the JSON writer print straight
// into it. The body is not framed: it ends when the connection closes, which every answer here
// is followed by. A body whose length is known in advance says so in Content-Length.
//
// Sending waits for the other end, and that is limited twice. An end that takes nothing for
// OV_HTTP_SEND_STALL_MS is gone. And an answer may spend OV_HTTP_SEND_MS being sent, added up
// over all its pieces: a reader that takes a byte now and then never stalls, and without this it
// could hold the Opta for as long as it liked. Time in which nothing is being sent, as when a
// command is working, does not count.
class OvHttpResponse : public OvOut {
 public:
  OvHttpResponse()
      : conn_(nullptr), clock_(nullptr), used_(0), failed_(false), begun_(false), sent_(0), sendingMs_(0) {}

  // Starts a new answer on `conn`. `clock` times the sending.
  void attach(OvConn &conn, OvClock &clock) {
    conn_ = &conn;
    clock_ = &clock;
    used_ = 0;
    failed_ = false;
    begun_ = false;
    sent_ = 0;
    sendingMs_ = 0;
  }
  void detach() {
    conn_ = nullptr;
    clock_ = nullptr;
  }
  bool attached() const { return conn_ != nullptr; }

  // The status line and the headers. `extra` is zero or more complete header lines, each ending
  // in CR LF. `length` is the size of the body when it is known, or -1.
  void begin(int status, const char *contentType, const char *extra = nullptr, long length = -1) {
    begun_ = true;
    print("HTTP/1.1 ");
    printU((uint32_t)status);
    printChar(' ');
    print(ovHttpReason(status));
    print("\r\n");
    if (contentType != nullptr) {
      print("Content-Type: ");
      print(contentType);
      print("\r\n");
    }
    if (length >= 0) {
      print("Content-Length: ");
      printU((uint32_t)length);
      print("\r\n");
    }
    print("Cache-Control: no-store\r\n"
          "X-Content-Type-Options: nosniff\r\n"
          "X-Frame-Options: DENY\r\n"
          "Referrer-Policy: no-referrer\r\n"
          "Connection: close\r\n");
    if (extra != nullptr) {
      print(extra);
    }
    print("\r\n");
  }
  bool begun() const { return begun_; }

  void write(const char *text, size_t len) override {
    while (len > 0 && !failed_ && conn_ != nullptr) {
      const size_t room = sizeof(buffer_) - used_;
      const size_t n = len < room ? len : room;
      memcpy(buffer_ + used_, text, n);
      used_ += n;
      text += n;
      len -= n;
      if (used_ == sizeof(buffer_)) {
        flush();
      }
    }
  }

  // Sends what has been collected, waiting for the other end to take it. Returns false once the
  // answer has been given up: the other end is gone or takes nothing, or the answer has had all
  // the time it gets. What had not been sent by then is dropped, and nothing is sent after it.
  bool flush() {
    if (used_ > 0 && !failed_ && conn_ != nullptr) {
      const uint32_t start = clock_->nowMs();
      uint32_t last = start;  // when the other end last took something
      size_t done = 0;
      while (done < used_) {
        const uint32_t now = clock_->nowMs();
        if (sendingMs_ + (uint32_t)(now - start) >= OV_HTTP_SEND_MS) {
          failed_ = true;  // looked at before every piece, so a slow but steady reader ends here
          break;
        }
        const int took = conn_->write(buffer_ + done, used_ - done);
        if (took < 0 || (size_t)took > used_ - done) {
          failed_ = true;
          break;
        }
        if (took > 0) {
          done += (size_t)took;
          sent_ += (uint32_t)took;
          last = now;
          continue;
        }
        if ((uint32_t)(now - last) >= OV_HTTP_SEND_STALL_MS) {
          failed_ = true;
          break;
        }
        clock_->idle();
      }
      sendingMs_ += (uint32_t)(clock_->nowMs() - start);
    }
    used_ = 0;
    return !failed_;
  }

  bool pending() const { return used_ > 0; }
  bool failed() const { return failed_; }
  uint32_t sent() const { return sent_; }

 private:
  OvConn *conn_;
  OvClock *clock_;
  uint8_t buffer_[1024];
  size_t used_;
  bool failed_;
  bool begun_;
  uint32_t sent_;
  uint32_t sendingMs_;  // what this answer has spent in flush() so far
};

// ---------------------------------------------------------------------------------------------
// Writing JSON
// ---------------------------------------------------------------------------------------------

// A value in the shortest form that keeps `decimals` places: 100, 96.667, 13.1
inline size_t ovFormatTrimmed(double value, uint8_t decimals, char *out, size_t cap) {
  size_t n = ovFormatFixed(value, decimals, out, cap);
  if (strchr(out, '.') != nullptr) {
    while (n > 0 && out[n - 1] == '0') {
      out[--n] = '\0';
    }
    if (n > 0 && out[n - 1] == '.') {
      out[--n] = '\0';
    }
  }
  return n;
}

// Writes JSON into an OvOut and keeps track of the commas. Give a key inside an object and
// nullptr inside an array.
class OvJson {
 public:
  explicit OvJson(OvOut &out) : out_(out), depth_(0) { first_[0] = true; }

  void beginObject(const char *key = nullptr) { open(key, '{'); }
  void endObject() { close('}'); }
  void beginArray(const char *key = nullptr) { open(key, '['); }
  void endArray() { close(']'); }

  void str(const char *key, const char *value) {
    lead(key);
    string(value != nullptr ? value : "");
  }
  void u(const char *key, uint32_t value) {
    lead(key);
    out_.printU(value);
  }
  void i(const char *key, int32_t value) {
    lead(key);
    out_.printI(value);
  }
  void flag(const char *key, bool value) {
    lead(key);
    out_.print(value ? "true" : "false");
  }
  void null(const char *key) {
    lead(key);
    out_.print("null");
  }
  // A number with at most `decimals` places and no trailing zeros. JSON has no NaN and no
  // infinity: those become null.
  void num(const char *key, double value, uint8_t decimals) {
    lead(key);
    char text[40];
    ovFormatTrimmed(value, decimals, text, sizeof(text));
    const char c = text[0] == '-' ? text[1] : text[0];
    out_.print(c >= '0' && c <= '9' ? text : "null");
  }

 private:
  OvOut &out_;
  uint8_t depth_;
  bool first_[12];

  void lead(const char *key) {
    if (!first_[depth_]) {
      out_.printChar(',');
    }
    first_[depth_] = false;
    if (key != nullptr) {
      string(key);
      out_.printChar(':');
    }
  }

  void open(const char *key, char bracket) {
    lead(key);
    out_.printChar(bracket);
    if (depth_ + 1 < (uint8_t)(sizeof(first_) / sizeof(first_[0]))) {
      ++depth_;
    }
    first_[depth_] = true;
  }

  void close(char bracket) {
    out_.printChar(bracket);
    if (depth_ > 0) {
      --depth_;
    }
  }

  void string(const char *text) {
    out_.printChar('"');
    const char *run = text;
    const char *p = text;
    for (; *p != '\0'; ++p) {
      const unsigned char c = (unsigned char)*p;
      if (c != '"' && c != '\\' && c >= 0x20) {
        continue;
      }
      if (p > run) {
        out_.write(run, (size_t)(p - run));
      }
      if (c == '"' || c == '\\') {
        out_.printChar('\\');
        out_.printChar((char)c);
      } else {
        out_.print("\\u00");
        out_.printHex(c, 2);
      }
      run = p + 1;
    }
    if (p > run) {
      out_.write(run, (size_t)(p - run));
    }
    out_.printChar('"');
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_HTTP_H
