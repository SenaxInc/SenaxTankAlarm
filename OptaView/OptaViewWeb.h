/**
 * OptaViewWeb.h
 *
 * OptaView's web page: the server on the Opta that hosts it, and the small API behind it.
 *
 * No Arduino dependency. Connections come in through OvWebHost, so the host tests drive this
 * code with requests written as strings, against the simulated SunSaver, and a small program in
 * tests/host/optaview serves the real page from a PC for a browser.
 *
 * How it changes the controller: through a console
 * ------------------------------------------------
 * The page does not have write calls of its own. It has a console (the same OvConsole class as
 * the USB port, a second instance) and sends it command lines: POST /api/console. 'set', 'coil',
 * 'reset' and the rest answer over HTTP exactly as they do over USB: they show the old and the
 * new value, the pair checks and the warnings, and then ask. The page shows that text and offers
 * the buttons; a click on the confirming one sends "yes". So every rule about writing is the
 * console's, is there once, and holds for both front ends.
 *
 * Two things are added for HTTP, where an answer can arrive late, twice or from another tab:
 *   - A question has an id. A "yes" counts only together with the id of the question that is
 *     open. Any other line drops an open question before it runs.
 *   - A command that runs long is stopped when the browser goes away or after one minute.
 *
 * Reading has calls of its own (GET /api/read and so on) that return JSON, because a page wants
 * values, not columns of text.
 *
 * Who may use it
 * --------------
 * A PIN of 4 to 8 digits. It can be built in (OptaViewConfig.h) or set on the USB console
 * ('web pin'); if there is none, the first visitor chooses it. It is kept in memory only and is
 * gone at power-off. One browser is logged in at a time, with a session cookie, and wrong PINs
 * are answered more and more slowly. Requests that change something must carry the X-OptaView
 * header, which a page from another site cannot add, and every request must name the Opta by
 * its IP address.
 *
 * What this is not: there is no TLS. The PIN and the session cookie cross the network in the
 * clear, so this belongs on a network you trust, not on the internet.
 *
 * One thing at a time
 * -------------------
 * The Opta runs one loop. While a request is being answered the USB console waits, and the
 * other way round. The page therefore sends its requests one after another.
 */

#ifndef OPTAVIEW_WEB_H
#define OPTAVIEW_WEB_H

#include <ArduinoJson.h>

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OptaViewConsole.h"
#include "OptaViewDevice.h"
#include "OptaViewHttp.h"
#include "OptaViewModbus.h"
#include "OptaViewNet.h"
#include "OptaViewProfile.h"
#include "OptaViewText.h"

namespace optaview {

static const char OV_WEB_COOKIE[] = "optaview_session";
static const char OV_WEB_MARKER[] = "x-optaview";  // lower case: header names are compared that way
static const uint8_t OV_WEB_SLOTS = 2;             // connections that may wait for their first byte
static const uint32_t OV_WEB_FIRST_BYTE_MS = 3000;
static const uint32_t OV_WEB_SESSION_IDLE_MS = 30UL * 60UL * 1000UL;
static const uint32_t OV_WEB_COMMAND_MS = 60000;   // the longest a request may keep the Opta busy
static const uint32_t OV_WEB_HEARTBEAT_MS = 1000;
static const size_t OV_WEB_PIN_MIN = 4;
static const size_t OV_WEB_PIN_MAX = 8;
// In the text of a console answer: a byte that only shows that the Opta is still at work (the
// page drops it), and the byte after which the JSON that ends the answer follows.
static const char OV_WEB_HEARTBEAT = 0x1F;
static const char OV_WEB_TRAILER = 0x1E;

// True for a PIN: 4 to 8 digits. It is constexpr so that a PIN that is built into the sketch is
// checked when the sketch is compiled.
constexpr bool ovIsPin(const char *pin) {
  if (pin == nullptr) {
    return false;
  }
  size_t len = 0;
  for (; pin[len] != '\0'; ++len) {
    if (len >= OV_WEB_PIN_MAX || pin[len] < '0' || pin[len] > '9') {
      return false;
    }
  }
  return len >= OV_WEB_PIN_MIN;
}

// What the web server needs from the board.
class OvWebHost {
 public:
  virtual ~OvWebHost() {}
  // A connection that has just come in, or nullptr. The server calls its close() when done.
  virtual OvConn *accept() = 0;
  // Bytes that nobody can predict: session tokens are made of them.
  virtual void random(uint8_t *out, size_t len) = 0;
  // "192.168.1.57", or false while the network is down. For the USB console's 'web'.
  virtual bool address(char *out, size_t cap) = 0;
};

// The page, as tools/embed_web.py builds it into the sketch.
struct OvWebPage {
  const char *html;
  size_t length;
  const char *headers;  // its Content-Security-Policy, as a header line
};

class OvWeb : public OvConsoleExtra {
 public:
  OvWeb(OvDevice &device, OvWebHost &host, OvClock &clock, const OvWebPage &page)
      : device_(device),
        host_(host),
        clock_(clock),
        page_(page),
        link_(*this),
        console_(link_, link_, device),
        sessionOpen_(false),
        sessionUsedMs_(0),
        failures_(0),
        blocked_(false),
        blockedAtMs_(0),
        blockedForMs_(0),
        running_(false),
        cut_(false),
        heartbeat_(false),
        commandStartMs_(0),
        lastSendMs_(0),
        served_(0) {
    pin_[0] = '\0';
    token_[0] = '\0';
    question_[0] = '\0';
    for (uint8_t i = 0; i < OV_WEB_SLOTS; ++i) {
      slots_[i].conn = nullptr;
      slots_[i].sinceMs = 0;
    }
  }

  // Sets the PIN. Whoever is logged in has to log in again. Returns false, and changes nothing,
  // when it is not 4 to 8 digits.
  bool setPin(const char *pin) {
    if (!ovIsPin(pin)) {
      return false;
    }
    ovCopy(pin_, sizeof(pin_), pin);
    endSession();
    failures_ = 0;
    blocked_ = false;
    return true;
  }

  // No PIN: the next visitor chooses one.
  void clearPin() {
    pin_[0] = '\0';
    endSession();
    failures_ = 0;
    blocked_ = false;
  }

  bool pinSet() const { return pin_[0] != '\0'; }
  bool sessionOpen() const { return sessionOpen_; }
  uint32_t served() const { return served_; }

  // Call from loop(). Answers at most one request and returns.
  void poll() {
    const uint32_t now = clock_.nowMs();
    if (sessionOpen_ && (uint32_t)(now - sessionUsedMs_) >= OV_WEB_SESSION_IDLE_MS) {
      endSession();
    }
    if (blocked_ && (uint32_t)(now - blockedAtMs_) >= blockedForMs_) {
      blocked_ = false;
    }
    for (uint8_t i = 0; i < OV_WEB_SLOTS; ++i) {
      Slot &slot = slots_[i];
      if (slot.conn == nullptr) {
        slot.conn = host_.accept();
        slot.sinceMs = now;
        continue;
      }
      const int first = slot.conn->read();
      if (first >= 0) {
        OvConn *conn = slot.conn;
        slot.conn = nullptr;
        serve(*conn, first);
        return;
      }
      // A browser opens connections in advance and may never use them. They must not hold the
      // loop up, and they must not keep a slot for long.
      if ((uint32_t)(now - slot.sinceMs) >= OV_WEB_FIRST_BYTE_MS) {
        slot.conn->close();
        slot.conn = nullptr;
      }
    }
  }

  // --- OvConsoleExtra: 'web' on the USB console ---

  bool command(int argc, char **argv, OvOut &out) override {
    if (!ovEqualsIgnoreCase(argv[0], "web")) {
      return false;
    }
    if (argc == 1) {
      status(out);
      out.print("          ");
      out.printU(served_);
      out.println(served_ == 1 ? " request answered since power-up" : " requests answered since power-up");
      return true;
    }
    if (argc == 2 && ovEqualsIgnoreCase(argv[1], "logout")) {
      if (sessionOpen_) {
        endSession();
        out.println("The browser's session is ended. It has to log in again.");
      } else {
        out.println("Nobody is logged in.");
      }
      return true;
    }
    if (argc == 3 && ovEqualsIgnoreCase(argv[1], "pin")) {
      if (ovEqualsIgnoreCase(argv[2], "clear")) {
        clearPin();
        out.println("There is no PIN now. The next visitor of the web page chooses one.");
        return true;
      }
      if (!setPin(argv[2])) {
        out.println("A PIN is 4 to 8 digits.");
        return true;
      }
      out.println("The PIN is set. Whoever was logged in has to log in again.");
      out.println("It lasts until power-off; OptaViewConfig.h sets the PIN that OptaView starts with.");
      return true;
    }
    out.println("Usage: web   |   web pin <4 to 8 digits>   |   web pin clear   |   web logout");
    return true;
  }

  void help(OvOut &out) override {
    out.println("Web page");
    out.println("  web                     where the page is and who is logged in");
    out.println("  web pin <digits>        set its PIN (4 to 8 digits); 'web pin clear' lets the next visitor choose");
    out.println("  web logout              end the browser's session");
  }

  // "Web page  http://192.168.1.57/, PIN set, nobody logged in"
  void status(OvOut &out) override {
    char address[24];
    out.print("Web page  ");
    if (host_.address(address, sizeof(address))) {
      out.print("http://");
      out.print(address);
      out.print("/");
    } else {
      out.print("not reachable while the network is down");
    }
    out.print(pin_[0] != '\0' ? ", PIN set" : ", no PIN yet: the first visitor chooses it");
    if (sessionOpen_) {
      out.print(", a browser is logged in (last used ");
      out.printU((uint32_t)((uint32_t)(clock_.nowMs() - sessionUsedMs_) / 1000UL));
      out.print(" s ago)");
    } else if (pin_[0] != '\0') {
      out.print(", nobody logged in");
    }
    out.println();
    const uint32_t wait = waitSeconds();
    if (wait > 0) {
      out.print("          Logins are refused for another ");
      out.printU(wait);
      out.print(" s after ");
      out.printU(failures_);
      out.println(failures_ == 1 ? " wrong PIN. 'web pin' sets a new one and ends the wait."
                                 : " wrong PINs. 'web pin' sets a new one and ends the wait.");
    }
  }

 private:
  // The web console's output and its "has a key been pressed": both lead to the answer that is
  // being written.
  class ConsoleLink : public OvOut, public OvConsoleHost, public OvAbortCheck {
   public:
    explicit ConsoleLink(OvWeb &web) : web_(web) {}
    void write(const char *text, size_t len) override { web_.response_.write(text, len); }
    bool inputPending() override { return web_.shouldStop(); }
    void discardInput() override {}
    bool hasKeys() override { return false; }
    bool aborted() override { return web_.shouldStop(); }

   private:
    OvWeb &web_;
  };

  struct Slot {
    OvConn *conn;
    uint32_t sinceMs;
  };

  // While a request works on the bus: the trace of the USB console keeps out of it, and the
  // limits on how long it may take are armed. Console answers are text, into which a heartbeat
  // byte can go; JSON answers are not.
  class BusWork {
   public:
    BusWork(OvWeb &web, bool console)
        : web_(web), saved_(web.device_.master().trace), lateBefore_(web.device_.master().stats.lateArrivals) {
      web.device_.master().trace = (console && web.console_.traceOn()) ? &web.console_ : nullptr;
      web.running_ = true;
      web.cut_ = false;
      web.heartbeat_ = console;
      web.commandStartMs_ = web.clock_.nowMs();
      web.lastSendMs_ = web.commandStartMs_;
      if (!console) {
        web.device_.beginBatch();
      }
    }
    ~BusWork() {
      web_.device_.master().trace = saved_;
      web_.running_ = false;
    }
    // Something arrived for a request that had been given up (see ModbusMaster::settleBus).
    bool late() const { return web_.device_.master().stats.lateArrivals != lateBefore_; }

   private:
    OvWeb &web_;
    MbTraceSink *saved_;
    uint32_t lateBefore_;
  };

  OvDevice &device_;
  OvWebHost &host_;
  OvClock &clock_;
  OvWebPage page_;
  ConsoleLink link_;
  OvConsole console_;
  OvHttpRequest request_;
  OvHttpResponse response_;
  Slot slots_[OV_WEB_SLOTS];

  char pin_[OV_WEB_PIN_MAX + 1];
  char token_[33];          // the session cookie's value, 32 hex digits
  bool sessionOpen_;
  uint32_t sessionUsedMs_;  // when the person last did something (the page's own refreshing does not count)
  uint8_t failures_;        // wrong PINs in a row
  bool blocked_;            // logins are refused until blockedForMs_ after blockedAtMs_
  uint32_t blockedAtMs_;
  uint32_t blockedForMs_;

  char question_[9];        // the id of the question the web console has open, or ""
  bool running_;            // a request is working on the bus
  bool cut_;                // ...and was stopped: the browser went away, or it took too long
  bool heartbeat_;
  uint32_t commandStartMs_;
  uint32_t lastSendMs_;
  uint32_t served_;

  // -------------------------------------------------------------------------------------------
  // Small helpers
  // -------------------------------------------------------------------------------------------

  // A Host header that names an IP address: "192.168.1.57" or "192.168.1.57:80". A page on
  // another site can make a browser send requests here under a name of its own (DNS rebinding),
  // but never under a bare address.
  static bool hostIsAddress(const char *host) {
    char address[20];
    const char *colon = strchr(host, ':');
    const size_t len = colon != nullptr ? (size_t)(colon - host) : strlen(host);
    if (len == 0 || len >= sizeof(address)) {
      return false;
    }
    memcpy(address, host, len);
    address[len] = '\0';
    OvIp4 ip;
    if (!ovParseIp4(address, &ip)) {
      return false;
    }
    if (colon != nullptr) {
      const size_t digits = strlen(colon + 1);
      if (digits < 1 || digits > 5) {
        return false;
      }
      for (const char *p = colon + 1; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
          return false;
        }
      }
    }
    return true;
  }

  void makeHex(char *out, size_t bytes) {
    uint8_t raw[16];
    if (bytes > sizeof(raw)) {
      bytes = sizeof(raw);
    }
    memset(raw, 0, sizeof(raw));
    host_.random(raw, bytes);
    for (size_t i = 0; i < bytes; ++i) {
      ovFormatHex(raw[i], 2, out + 2 * i, 3);
    }
    out[2 * bytes] = '\0';
    for (char *p = out; *p != '\0'; ++p) {
      *p = (char)tolower((unsigned char)*p);
    }
  }

  bool sessionValid() const { return sessionOpen_ && ovSameSecret(token_, request_.cookie); }

  void endSession() {
    sessionOpen_ = false;
    token_[0] = '\0';
    question_[0] = '\0';
    console_.newVisitor();
  }

  void openSession() {
    endSession();
    makeHex(token_, 16);
    sessionOpen_ = true;
    sessionUsedMs_ = clock_.nowMs();
  }

  // Seconds until a PIN is looked at again, or 0.
  uint32_t waitSeconds() {
    if (!blocked_) {
      return 0;
    }
    const uint32_t elapsed = (uint32_t)(clock_.nowMs() - blockedAtMs_);
    if (elapsed >= blockedForMs_) {
      blocked_ = false;
      return 0;
    }
    return (blockedForMs_ - elapsed + 999UL) / 1000UL;
  }

  // A wrong PIN. The next one is not looked at for a while, and for longer each time: a
  // four-digit PIN has only ten thousand possibilities.
  void noteFailure() {
    static const uint16_t kWaitSeconds[] = {1, 2, 4, 8, 30, 60, 120, 240, 480, 900};
    if (failures_ < 250) {
      ++failures_;
    }
    const size_t last = sizeof(kWaitSeconds) / sizeof(kWaitSeconds[0]) - 1;
    const size_t index = (size_t)(failures_ - 1) < last ? (size_t)(failures_ - 1) : last;
    blocked_ = true;
    blockedAtMs_ = clock_.nowMs();
    blockedForMs_ = (uint32_t)kWaitSeconds[index] * 1000UL;
  }

  // True when the request that is being answered should stop working: the browser is gone or
  // the time is up. Also sends what has been printed so far, so that the page shows progress.
  bool shouldStop() {
    if (!running_) {
      return false;
    }
    if (cut_) {
      return true;
    }
    const uint32_t now = clock_.nowMs();
    if (response_.pending()) {
      if (response_.flush()) {
        lastSendMs_ = now;
      }
    } else if (heartbeat_ && (uint32_t)(now - lastSendMs_) >= OV_WEB_HEARTBEAT_MS) {
      // Nothing has been printed for a while. A connection that the browser has closed is only
      // noticed when something is sent, so send a byte that means nothing.
      response_.printChar(OV_WEB_HEARTBEAT);
      response_.flush();
      lastSendMs_ = now;
    }
    if (response_.failed() || (uint32_t)(now - commandStartMs_) >= OV_WEB_COMMAND_MS) {
      cut_ = true;
    }
    return cut_;
  }

  // -------------------------------------------------------------------------------------------
  // One request
  // -------------------------------------------------------------------------------------------

  // Kept out of serve() so that the reader's line buffer is off the stack again before the
  // request is carried out.
#if defined(__GNUC__)
  __attribute__((noinline))
#endif
  OvHttpParse readRequest(OvConn &conn, int first) {
    OvHttpReader reader(conn, clock_);
    return reader.read(first, OV_WEB_COOKIE, OV_WEB_MARKER, &request_);
  }

  void serve(OvConn &conn, int first) {
    cut_ = false;
    const OvHttpParse parse = readRequest(conn, first);
    response_.attach(&conn);
    if (parse == OV_HTTP_OK) {
      ++served_;
      route();
      response_.flush();
    } else if (parse != OV_HTTP_TIMEOUT) {
      ++served_;
      const int status = ovHttpStatusFor(parse);
      plain(status, parse == OV_HTTP_BODY_TOO_LARGE || parse == OV_HTTP_TARGET_TOO_LONG ||
                            parse == OV_HTTP_HEADERS_TOO_BIG
                        ? "That request is larger than OptaView accepts."
                        : "OptaView could not read that request.");
      response_.flush();
      ovHttpDrain(conn, clock_, 50, 500);
    }
    response_.detach();
    conn.close();
  }

  static const char *apiHeaders() { return "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"; }

  void plain(int status, const char *text, const char *extra = nullptr) {
    char headers[200];
    OvText all(headers, sizeof(headers));
    all.add(apiHeaders());
    all.add(extra);
    response_.begin(status, "text/plain; charset=utf-8", headers);
    response_.println(text);
  }

  void beginJson(int status, const char *extra = nullptr) {
    char headers[260];
    OvText all(headers, sizeof(headers));
    all.add(apiHeaders());
    all.add(extra);
    response_.begin(status, "application/json", headers);
  }

  // {"error":"wrong-pin","message":"...","wait":4}
  void jsonError(int status, const char *code, const char *message, uint32_t wait = 0) {
    beginJson(status);
    OvJson json(response_);
    json.beginObject();
    json.str("error", code);
    json.str("message", message);
    if (wait > 0) {
      json.u("wait", wait);
    }
    json.endObject();
  }

  void route() {
    const OvHttpRequest &r = request_;
    const bool get = strcmp(r.method, "GET") == 0;
    const bool post = strcmp(r.method, "POST") == 0;
    if (!hostIsAddress(r.host)) {
      plain(421, "Open OptaView by its IP address, for example http://192.168.1.16/");
      return;
    }
    if (strcmp(r.path, "/") == 0 || strcmp(r.path, "/index.html") == 0) {
      if (!get) {
        plain(405, "Only GET works here.", "Allow: GET\r\n");
        return;
      }
      response_.begin(200, "text/html; charset=utf-8", page_.headers, (long)page_.length);
      response_.write(page_.html, page_.length);
      return;
    }
    if (strcmp(r.path, "/favicon.ico") == 0) {
      response_.begin(204, nullptr, apiHeaders(), 0);
      return;
    }
    if (strncmp(r.path, "/api/", 5) != 0) {
      plain(404, "There is nothing at that address.");
      return;
    }

    struct Route {
      const char *name;
      bool post;
      bool session;
      void (OvWeb::*handler)();
    };
    static const Route kRoutes[] = {
        {"session", false, false, &OvWeb::getSession},   {"pin", true, false, &OvWeb::postPin},
        {"login", true, false, &OvWeb::postLogin},       {"logout", true, false, &OvWeb::postLogout},
        {"state", false, true, &OvWeb::getState},        {"profile", false, true, &OvWeb::getProfile},
        {"read", false, true, &OvWeb::getRead},          {"coils", false, true, &OvWeb::getCoils},
        {"snapshot", false, true, &OvWeb::getSnapshot},  {"console", true, true, &OvWeb::postConsole}};
    const Route *route = nullptr;
    for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); ++i) {
      if (strcmp(r.path + 5, kRoutes[i].name) == 0) {
        route = &kRoutes[i];
        break;
      }
    }
    if (route == nullptr) {
      jsonError(404, "not-found", "There is no such call.");
      return;
    }
    if (route->post ? !post : !get) {
      beginJson(405, route->post ? "Allow: POST\r\n" : "Allow: GET\r\n");
      OvJson json(response_);
      json.beginObject();
      json.str("error", "method");
      json.str("message", route->post ? "This call needs POST." : "This call needs GET.");
      json.endObject();
      return;
    }
    if (route->post && !r.marker) {
      // Only the page itself sets this header. A form or a script on another site cannot.
      jsonError(403, "marker", "This request did not come from the OptaView page.");
      return;
    }
    if (route->session) {
      if (!sessionValid()) {
        jsonError(401, "login", "Log in first.");
        return;
      }
      // The page refreshes its readings by itself and marks those requests. Only what a person
      // does keeps the session alive.
      char automatic[4];
      if (!(ovQueryValue(r.query, "auto", automatic, sizeof(automatic)) && strcmp(automatic, "1") == 0)) {
        sessionUsedMs_ = clock_.nowMs();
      }
    }
    (this->*(route->handler))();
  }

  // -------------------------------------------------------------------------------------------
  // Logging in
  // -------------------------------------------------------------------------------------------

  void getSession() {
    beginJson(200);
    OvJson json(response_);
    json.beginObject();
    json.str("version", OV_VERSION);
    json.str("profile", device_.profile().title());
    json.flag("pinSet", pin_[0] != '\0');
    json.flag("loggedIn", sessionValid());
    json.u("wait", waitSeconds());
    json.endObject();
  }

  // The text of a JSON string. nullptr when the value is not a string, and when it holds a NUL,
  // which would end it early for everything that reads it as a C string.
  static const char *jsonText(JsonVariantConst value) {
    const JsonString text = value.as<JsonString>();
    if (text.isNull() || strlen(text.c_str()) != text.size()) {
      return nullptr;
    }
    return text.c_str();
  }

  // The "pin" of a JSON body, when it has the form of a PIN.
  bool pinFromBody(char *out, size_t cap) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char *)request_.body, request_.bodyLength)) {
      return false;
    }
    const char *pin = jsonText(doc["pin"]);
    if (!ovIsPin(pin)) {
      return false;
    }
    ovCopy(out, cap, pin);
    return true;
  }

  void answerLoggedIn() {
    char cookie[120];
    OvText text(cookie, sizeof(cookie));
    text.add("Set-Cookie: ");
    text.add(OV_WEB_COOKIE);
    text.add("=");
    text.add(token_);
    text.add("; Path=/; HttpOnly; SameSite=Strict\r\n");
    beginJson(200, cookie);
    OvJson json(response_);
    json.beginObject();
    json.flag("ok", true);
    json.endObject();
  }

  // The first visitor chooses the PIN. Once there is one, this call is closed.
  void postPin() {
    char pin[OV_WEB_PIN_MAX + 1];
    if (pin_[0] != '\0') {
      jsonError(409, "pin-set", "A PIN has been chosen already. Log in with it.");
      return;
    }
    if (!pinFromBody(pin, sizeof(pin))) {
      jsonError(400, "pin-format", "A PIN is 4 to 8 digits.");
      return;
    }
    ovCopy(pin_, sizeof(pin_), pin);
    failures_ = 0;
    blocked_ = false;
    openSession();
    answerLoggedIn();
  }

  void postLogin() {
    char pin[OV_WEB_PIN_MAX + 1];
    if (pin_[0] == '\0') {
      jsonError(409, "no-pin", "No PIN has been chosen yet.");
      return;
    }
    const uint32_t wait = waitSeconds();
    if (wait > 0) {
      // Not even looked at: a guess made during the wait must not count as a try.
      jsonError(429, "wait", "Too many wrong PINs. Wait, then try again.", wait);
      return;
    }
    if (!pinFromBody(pin, sizeof(pin)) || !ovSameSecret(pin_, pin)) {
      noteFailure();
      jsonError(401, "wrong-pin", "That is not the PIN.", waitSeconds());
      return;
    }
    failures_ = 0;
    blocked_ = false;
    openSession();
    answerLoggedIn();
  }

  void postLogout() {
    if (sessionValid()) {
      endSession();
    }
    char cookie[120];
    OvText text(cookie, sizeof(cookie));
    text.add("Set-Cookie: ");
    text.add(OV_WEB_COOKIE);
    text.add("=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n");
    beginJson(200, cookie);
    OvJson json(response_);
    json.beginObject();
    json.flag("ok", true);
    json.endObject();
  }

  // -------------------------------------------------------------------------------------------
  // Reading
  // -------------------------------------------------------------------------------------------

  static const char *typeName(uint8_t type) {
    switch (type) {
      case OV_U16: return "u16";
      case OV_I16: return "i16";
      case OV_U8: return "u8";
      case OV_I8: return "i8";
      case OV_U32: return "u32";
      case OV_ENUM: return "enum";
      case OV_BITS: return "bits";
      default: return "?";
    }
  }

  void writeScale(OvJson &json, const char *key, const OvScale *scale, bool confirmed, double found) {
    if (scale == nullptr) {
      return;
    }
    json.beginObject(key);
    json.str("name", scale->name);
    json.num("num", scale->num, 4);
    json.num("den", scale->den, 4);
    json.flag("confirmed", confirmed);
    json.num("found", found, 4);  // what 'scalecheck' found on this controller, or 0
    json.endObject();
  }

  // What the page shows around everything else: the link, the voltage scales, a restart that
  // is due.
  void writeState(OvJson &json) {
    OvProfile &profile = device_.profile();
    ModbusMaster &master = device_.master();
    const OvScaleCheck &check = profile.scaleCheck();
    json.str("version", OV_VERSION);
    json.beginObject("link");
    json.u("slave", device_.link.slave);
    json.u("baud", device_.link.baud);
    json.str("format", mbFormatName(device_.link.format));
    json.u("readFunction", device_.link.readFunction);
    json.u("timeoutMs", master.responseTimeoutMs);
    json.str("blocks", ovBlockModeName(device_.blockMode));
    json.flag("blocksInUse", device_.blocksInUse());
    json.endObject();
    json.beginObject("scales");
    writeScale(json, "settings", check.settingsScale, device_.settingsScaleConfirmed(), device_.settingsScaleFound());
    writeScale(json, "live", check.liveScale, device_.liveScaleConfirmed(), device_.liveScaleFound());
    json.endObject();
    json.flag("resetNeeded", device_.resetNeeded());
    json.beginObject("counters");
    json.u("sent", master.stats.sent);
    json.u("ok", master.stats.ok);
    json.u("timeouts", master.stats.timeouts);
    json.u("crcErrors", master.stats.crcErrors);
    json.u("exceptions", master.stats.exceptions);
    json.u("otherErrors", master.stats.otherErrors);
    json.u("lateArrivals", master.stats.lateArrivals);
    json.endObject();
  }

  void getState() {
    beginJson(200);
    OvJson json(response_);
    json.beginObject();
    writeState(json);
    json.endObject();
  }

  static void writeList(OvJson &json, const char *key, OvProfile &profile, size_t count,
                        const char *(OvProfile::*item)(size_t) const) {
    json.beginArray(key);
    for (size_t i = 0; i < count; ++i) {
      json.str(nullptr, (profile.*item)(i));
    }
    json.endArray();
  }

  // The register map, for the page to build its tables from. Nothing is read from the bus.
  void getProfile() {
    OvProfile &profile = device_.profile();
    const OvModbusDefaults &modbus = profile.modbus();
    beginJson(200);
    OvJson json(response_);
    json.beginObject();
    json.str("id", profile.id());
    json.str("title", profile.title());
    json.str("version", profile.version());
    json.str("source", profile.source());
    json.str("verifiedBy", profile.verifiedBy());
    json.str("resetCoil", modbus.resetCoil != nullptr ? modbus.resetCoil->name : "");

    json.beginArray("groups");
    for (size_t i = 0; i < profile.groupCount(); ++i) {
      const OvGroup &group = profile.group(i);
      bool live = false;
      for (size_t k = 0; k < profile.liveGroupCount(); ++k) {
        live = live || ovEqualsIgnoreCase(profile.liveGroup(k), group.name);
      }
      json.beginObject();
      json.str("name", group.name);
      json.str("title", group.title);
      json.flag("live", live);
      json.endObject();
    }
    json.endArray();

    json.beginArray("registers");
    for (size_t i = 0; i < profile.registerCount(); ++i) {
      const OvRegister &reg = profile.reg(i);
      json.beginObject();
      json.str("name", reg.name);
      json.u("addr", reg.addr);
      json.str("label", reg.label);
      json.str("group", reg.group);
      json.str("type", typeName(reg.type));
      json.str("units", reg.units);
      json.u("decimals", reg.decimals);
      json.flag("writable", reg.writable);
      json.flag("bench", reg.bench);
      if (reg.scale != nullptr) {
        json.str("scale", reg.scale->name);
      }
      if (reg.hasMin) {
        json.num("min", reg.minValue, 6);
      }
      if (reg.hasMax) {
        json.num("max", reg.maxValue, 6);
      }
      if (reg.zeroOk) {
        json.flag("zeroOk", true);
      }
      if (reg.note != nullptr && reg.note[0] != '\0') {
        json.str("note", reg.note);
      }
      json.endObject();
      if (response_.failed()) {
        return;  // nobody is listening any more
      }
    }
    json.endArray();

    json.beginArray("coils");
    for (size_t i = 0; i < profile.coilCount(); ++i) {
      const OvCoil &coil = profile.coil(i);
      json.beginObject();
      json.str("name", coil.name);
      json.u("addr", coil.addr);
      json.str("label", coil.label);
      json.flag("trigger", coil.trigger);
      json.flag("reset", &coil == modbus.resetCoil);
      json.endObject();
    }
    json.endArray();

    json.beginArray("checks");
    for (size_t i = 0; i < profile.checkCount(); ++i) {
      const OvCheck &check = profile.check(i);
      json.beginObject();
      json.str("below", check.below->name);
      json.str("above", check.above->name);
      json.str("message", check.message);
      json.endObject();
    }
    json.endArray();

    writeList(json, "notes", profile, profile.noteCount(), &OvProfile::note);
    writeList(json, "writeNotes", profile, profile.writeNoteCount(), &OvProfile::writeNote);
    writeList(json, "wiring", profile, profile.wiringCount(), &OvProfile::wiring);
    json.endObject();
  }

  // One register that was just read: its raw words and what they mean, or why there are none.
  void writeReading(OvJson &json, const OvRegister &reg, const uint16_t *words, const OvWordStatus &status) {
    json.beginObject();
    json.str("name", reg.name);
    if (status.status == MB_OK) {
      char text[OV_VALUE_TEXT];
      ovFormatValue(reg, words, text, sizeof(text));
      json.beginArray("raw");
      for (uint8_t w = 0; w < reg.words; ++w) {
        json.u(nullptr, words[w]);
      }
      json.endArray();
      json.str("text", text);
      if (ovIsNumeric(reg) && !ovIsSentinel(reg, words)) {
        json.num("value", ovValue(reg, words), 6);
      }
    } else {
      char text[64];
      ovDescribeStatus(status, text, sizeof(text));
      json.str("error", text);
    }
    json.endObject();
  }

  // GET /api/read?groups=live,status
  void getRead() {
    OvProfile &profile = device_.profile();
    char groups[OV_HTTP_QUERY_MAX];
    if (!ovQueryValue(request_.query, "groups", groups, sizeof(groups)) || groups[0] == '\0') {
      jsonError(400, "groups", "Name the groups to read, for example /api/read?groups=live,status");
      return;
    }
    // Every name is checked before anything is read: a request is carried out whole or not at all.
    const OvGroup *wanted[16];
    size_t count = 0;
    for (char *name = groups; name != nullptr;) {
      char *comma = strchr(name, ',');
      if (comma != nullptr) {
        *comma = '\0';
      }
      const OvGroup *group = profile.findGroup(name);
      if (group == nullptr || count >= sizeof(wanted) / sizeof(wanted[0])) {
        jsonError(404, "group", "There is no such group of registers.");
        return;
      }
      wanted[count++] = group;
      name = comma != nullptr ? comma + 1 : nullptr;
    }

    BusWork work(*this, false);
    beginJson(200);
    OvJson json(response_);
    json.beginObject();
    json.beginArray("groups");
    uint16_t words[OV_MAX_SPAN_WORDS];
    OvWordStatus status[OV_MAX_SPAN_WORDS];
    for (size_t g = 0; g < count; ++g) {
      json.beginObject();
      json.str("name", wanted[g]->name);
      json.beginArray("registers");
      size_t cursor = 0;
      OvSpan span;
      while (!shouldStop() && device_.nextSpan(wanted[g]->name, &cursor, &span)) {
        device_.readSpan(span.addr, span.words, words, status);
        uint16_t offset = 0;
        for (size_t k = 0; k < span.count; ++k) {
          const OvRegister &reg = profile.reg(span.first + k);
          writeReading(json, reg, words + offset, OvDevice::combine(status + offset, reg.words));
          offset = (uint16_t)(offset + reg.words);
        }
      }
      json.endArray();
      json.endObject();
    }
    json.endArray();
    json.flag("linkDown", device_.linkDown());
    json.flag("late", work.late());
    json.flag("stopped", cut_);
    json.beginObject("state");
    writeState(json);
    json.endObject();
    json.endObject();
  }

  void getCoils() {
    OvProfile &profile = device_.profile();
    BusWork work(*this, false);
    beginJson(200);
    OvJson json(response_);
    json.beginObject();
    json.beginArray("coils");
    bool down = false;
    for (size_t i = 0; i < profile.coilCount(); ++i) {
      const OvCoil &coil = profile.coil(i);
      json.beginObject();
      json.str("name", coil.name);
      if (coil.trigger) {
        json.flag("trigger", true);  // an action: there is no state to read
      } else if (down || shouldStop()) {
        json.str("error", "not read");
      } else {
        bool on = false;
        const MbResult result = device_.readCoil(coil, &on);
        if (result.status == MB_OK) {
          json.flag("on", on);
        } else {
          char text[64];
          ovDescribeResult(result, text, sizeof(text));
          json.str("error", text);
          down = down || mbIsLinkFailure(result.status);  // one silence is enough: the rest is not asked
        }
      }
      json.endObject();
    }
    json.endArray();
    json.flag("linkDown", down);
    json.flag("late", work.late());
    json.endObject();
  }

  // The same JSON as the console's 'snapshot', as a file to keep.
  void getSnapshot() {
    BusWork work(*this, false);
    char headers[200];
    OvText all(headers, sizeof(headers));
    all.add(apiHeaders());
    all.add("Content-Disposition: attachment; filename=\"optaview-snapshot.json\"\r\n");
    response_.begin(200, "application/json", headers);
    ovPrintSnapshot(device_, response_, &link_);
  }

  // -------------------------------------------------------------------------------------------
  // The console
  // -------------------------------------------------------------------------------------------

  // A line the console may be given: printable ASCII, not empty, and short enough to arrive whole.
  static bool validLine(const char *line) {
    if (line == nullptr) {
      return false;
    }
    const size_t len = strlen(line);
    if (len == 0 || len >= OV_LINE_MAX) {
      return false;
    }
    bool visible = false;
    for (size_t i = 0; i < len; ++i) {
      const unsigned char c = (unsigned char)line[i];
      if (c < 0x20 || c > 0x7E) {
        return false;
      }
      visible = visible || c != ' ';
    }
    return visible;
  }

  static bool isYes(const char *line) {
    while (*line == ' ') {
      ++line;
    }
    size_t len = strlen(line);
    while (len > 0 && line[len - 1] == ' ') {
      --len;
    }
    return len == 3 && tolower((unsigned char)line[0]) == 'y' && tolower((unsigned char)line[1]) == 'e' &&
           tolower((unsigned char)line[2]) == 's';
  }

  // POST /api/console {"line": "set EV_lvd 13.10"}, and later {"line": "yes", "answer": "<id>"}.
  // The answer is the console's text, then OV_WEB_TRAILER and one JSON object.
  void postConsole() {
    JsonDocument doc;
    if (deserializeJson(doc, (const char *)request_.body, request_.bodyLength)) {
      jsonError(400, "body", "The request is not JSON.");
      return;
    }
    const char *line = jsonText(doc["line"]);
    const char *answer = jsonText(doc["answer"]);
    const bool answers = !doc["answer"].isNull();  // an "answer" of any kind is never a new command
    if (!validLine(line) ||
        (answers && (answer == nullptr || answer[0] == '\0' || strlen(answer) >= sizeof(question_)))) {
      jsonError(400, "line", "A command is one line of plain text, at most 159 characters.");
      return;
    }

    bool asked = false;
    {
      BusWork work(*this, true);
      char headers[160];
      OvText all(headers, sizeof(headers));
      all.add(apiHeaders());
      response_.begin(200, "text/plain; charset=utf-8", headers);
      if (answers) {
        // An answer counts only for the question it names. A click that arrives twice, or late,
        // or from a tab that still shows an older question, changes nothing.
        if (!console_.awaitingConfirmation() || question_[0] == '\0' || strcmp(answer, question_) != 0) {
          response_.println("That question is no longer open. Nothing was sent.");
        } else {
          question_[0] = '\0';
          console_.handleLine(isYes(line) ? "yes" : "no");
          asked = console_.awaitingConfirmation();
        }
      } else {
        // Anything else is a new command. A question that was open is dropped first: a "yes"
        // typed as a command must find nothing to agree to.
        console_.cancelPending();
        question_[0] = '\0';
        console_.handleLine(line);
        asked = console_.awaitingConfirmation();
      }
      if (asked) {
        makeHex(question_, 4);  // a new question, a new id
      }
    }

    response_.printChar(OV_WEB_TRAILER);
    OvJson json(response_);
    json.beginObject();
    json.flag("awaiting", asked);
    if (asked) {
      json.str("question", question_);
    }
    json.flag("cut", cut_);
    json.beginObject("state");
    writeState(json);
    json.endObject();
    json.endObject();
  }
};

}  // namespace optaview

#endif  // OPTAVIEW_WEB_H
