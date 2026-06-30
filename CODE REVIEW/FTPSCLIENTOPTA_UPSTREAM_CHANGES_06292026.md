# FTPSclientOPTA Upstream Changes Required — 06/29/2026

**Target repo:** `dorkmo/FTPSclientOPTA`
**Required new version:** `0.3.0`
**Reason:** TankAlarm server v2.0.70+ adds a "Discover SHA-256 Fingerprint" button on the FTP settings page. The button calls `FtpsClient::discoverFingerprint(...)`, a new public method that does not exist in 0.2.4. Without these upstream changes the SenaxTankAlarm GitHub Actions release build will fail when arduino-cli compiles the sketch (the symbol is unresolved).

This document captures the exact diff that was applied locally so the same change can be committed upstream.

---

## TL;DR

Three files change in `dorkmo/FTPSclientOPTA`:

| File | Change |
|---|---|
| `library.properties` | bump `version=0.2.4` → `version=0.3.0` |
| `src/FtpsClient.h` | add public method `discoverFingerprint(...)` |
| `src/FtpsClient.cpp` | add implementation that issues TCP + AUTH TLS + TLS handshake (cert validation disabled) and extracts the peer fingerprint, then closes |

No changes are required in `IFtpsTransport.h` or `MbedSecureSocketFtpsTransport.{h,cpp}` — the transport already exposes `getPeerCertFingerprint()`, this change just wires it through `FtpsClient`.

After publishing, push a Git tag `v0.3.0` so the Arduino Library Manager / consumers can pin to it.

---

## Rationale

`MbedSecureSocketFtpsTransport::getPeerCertFingerprint(out, outLen)` already exists in the library but is not reachable through the public `FtpsClient` API. The only way to currently capture a server's fingerprint via `FtpsClient` is to attempt a full `connect()` against the very fingerprint you're trying to discover — a chicken-and-egg situation. `discoverFingerprint()` solves this by running only the steps needed to read the cert:

1. TCP connect to host:port
2. Read 220 banner
3. Send `AUTH TLS` (fall back to `AUTH SSL`)
4. Run the TLS handshake **with `validateServerCert = false`** and **`pinnedFingerprint = nullptr`** (accept any cert)
5. Read the peer cert fingerprint via `transport.getPeerCertFingerprint()`
6. Close the socket (no `QUIT`, since we never authenticated)

Cert validation is intentionally skipped because the operator's whole reason for calling this is that they do not yet have a known-good fingerprint to validate against. The returned value is presented to the operator for out-of-band verification (e.g. comparing with the server admin) before being pasted into the settings field for future verified connections.

---

## File 1 — `library.properties`

```diff
 name=FTPSclientOPTA
-version=0.2.4
+version=0.3.0
 author=dorkmo
 maintainer=dorkmo <dorkmo@gmail.com>
 sentence=Explicit FTPS client library for Arduino Opta.
 paragraph=Provides secure FTP file transfers over Explicit TLS using Mbed networking on Arduino Opta devices.
 category=Communication
 url=https://github.com/dorkmo/FTPSclientOPTA
 architectures=mbed_opta
 depends=Ethernet
```

A minor bump (`0.2.4` → `0.3.0`) is appropriate because this is a *new public method*; the change is fully additive (no behavior change for existing callers).

---

## File 2 — `src/FtpsClient.h`

Insert immediately **after** the existing `void quit();` declaration and **before** `bool reconnect(...)`. Around line 84 in the current header.

```cpp
  /// Send QUIT and close all sockets.
  void quit();

  /// Probe an FTPS server just far enough (TCP + banner + AUTH TLS +
  /// control-channel TLS handshake) to capture the peer certificate's
  /// SHA-256 fingerprint, then close the connection. Cert validation is
  /// intentionally skipped so this works before the operator has trusted
  /// anything. Use the returned 64-hex-char string to populate the
  /// `fingerprint` field of a subsequent verified FtpsServerConfig.
  ///
  /// Requires begin() to have been called first. `tlsServerName` is
  /// optional — if null/empty, falls back to `host`. On success, writes
  /// a NUL-terminated lowercase hex string to `fingerprintOut` (no colons).
  bool discoverFingerprint(const char *host,
                           uint16_t port,
                           const char *tlsServerName,
                           char *fingerprintOut,
                           size_t fingerprintOutSize,
                           char *error,
                           size_t errorSize);
```

No other changes to `FtpsClient.h`.

---

## File 3 — `src/FtpsClient.cpp`

Insert the new method definition immediately **after** the existing `void FtpsClient::quit() { ... }` body (around line 1805) and **before** `void FtpsClient::setReconnectBetweenStores(bool enabled)`.

```cpp
bool FtpsClient::discoverFingerprint(const char *host,
                                     uint16_t port,
                                     const char *tlsServerName,
                                     char *fingerprintOut,
                                     size_t fingerprintOutSize,
                                     char *error,
                                     size_t errorSize) {
  clearError(error, errorSize);
  if (fingerprintOut != nullptr && fingerprintOutSize > 0) {
    fingerprintOut[0] = '\0';
  }

  if (_transport == nullptr) {
    return failWith(
        _lastError,
        FtpsError::NetworkNotInitialized,
        error,
        errorSize,
        "FTPS transport not initialized. Call begin() first.");
  }
  if (!hasValue(host) || port == 0) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        "host and port are required for discovery.");
  }
  if (fingerprintOut == nullptr || fingerprintOutSize < 65) {
    return failWith(
        _lastError,
        FtpsError::CertValidationFailed,
        error,
        errorSize,
        "fingerprint output buffer too small (need >= 65 bytes).");
  }

  // Make sure no stale connection is lingering.
  _transport->closeAll();
  _connected = false;

  FtpEndpoint endpoint;
  endpoint.host = host;
  endpoint.port = port;

  FtpTlsConfig tls;
  tls.securityMode = FtpTlsSecurityMode::ExplicitTls;
  tls.serverName = hasValue(tlsServerName) ? tlsServerName : host;
  tls.pinnedFingerprint = nullptr;       // we are discovering it
  tls.rootCaPem = nullptr;
  tls.validateServerCert = false;        // accept any cert so we can read it

  tracePhase("discover:tcp-open");
  if (!_transport->connectControl(endpoint, tls, error, errorSize)) {
    return failWith(
        _lastError,
        FtpsError::ConnectionFailed,
        error,
        errorSize,
        hasValue(error) ? error : "TCP control connection failed.");
  }

  char reply[FTPS_REPLY_BUFFER_SIZE] = {};
  tracePhase("discover:banner");
  int code = ftpReadResponse(*_transport, reply, sizeof(reply));
  if (code != 220) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::BannerReadFailed,
        error,
        errorSize,
        hasValue(reply) ? reply : "Failed to read FTP banner.");
  }

  tracePhase("discover:auth-tls");
  code = ftpSendCommand(*_transport, "AUTH TLS", reply, sizeof(reply));
  if (code != 234) {
    code = ftpSendCommand(*_transport, "AUTH SSL", reply, sizeof(reply));
    if (code != 234) {
      _transport->closeAll();
      return failWith(
          _lastError,
          FtpsError::AuthTlsRejected,
          error,
          errorSize,
          hasValue(reply) ? reply : "AUTH TLS was rejected by the server.");
    }
  }

  tracePhase("discover:tls-handshake");
  if (!_transport->upgradeControlToTls(tls, error, errorSize)) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::ControlTlsHandshakeFailed,
        error,
        errorSize,
        hasValue(error) ? error : "Control-channel TLS handshake failed.");
  }

  tracePhase("discover:extract-fingerprint");
  if (!_transport->getPeerCertFingerprint(fingerprintOut, fingerprintOutSize)) {
    _transport->closeAll();
    return failWith(
        _lastError,
        FtpsError::CertValidationFailed,
        error,
        errorSize,
        "Could not extract peer certificate fingerprint.");
  }

  tracePhase("discover:done");
  _transport->closeAll();
  _lastError = FtpsError::None;
  return true;
}
```

The implementation reuses the same anonymous-namespace helpers (`clearError`, `hasValue`, `failWith`, `ftpReadResponse`, `ftpSendCommand`, `FTPS_REPLY_BUFFER_SIZE`) that `FtpsClient::connect()` uses, so no new private helpers are needed.

---

## What stays untouched

- `IFtpsTransport.h` — `getPeerCertFingerprint(out, len)` already declared with a default `return false` body, and `MbedSecureSocketFtpsTransport` already overrides it. No change.
- `FtpsTrust.{h,cpp}` — `FtpsTrustMode::Fingerprint` already supported. Discovery deliberately bypasses the trust system by setting `validateServerCert = false`.
- `FtpsTypes.h` — `FtpsServerConfig::validateServerCert` is already wired through. No change.
- `MbedSecureSocketFtpsTransport.{h,cpp}` — already computes the SHA-256 of the peer cert internally and exposes it through `getPeerCertFingerprint`. No change.

---

## Manual verification before publishing

1. Open `examples/FtpsSpikeTest/FtpsSpikeTest.ino` (or any other example) and confirm it still compiles. The new method is additive so existing code is unaffected.
2. Compile a sketch that calls `discoverFingerprint` against a known FTPS server (e.g. vsftpd configured with a self-signed cert) and confirm the returned hex matches what `openssl s_client -starttls ftp -connect host:21` reports. Example consumer code:

   ```cpp
   FtpsClient ftps;
   char err[160] = {};
   if (!ftps.begin(Ethernet.getNetwork(), err, sizeof(err))) {
     Serial.print(F("begin: ")); Serial.println(err); return;
   }
   char fp[80] = {};
   if (ftps.discoverFingerprint("192.0.2.50", 21, nullptr,
                                fp, sizeof(fp), err, sizeof(err))) {
     Serial.print(F("Fingerprint: ")); Serial.println(fp);
   } else {
     Serial.print(F("discover failed: ")); Serial.println(err);
   }
   ```

3. Confirm that an immediate second call (or a follow-up `connect()` with the discovered fingerprint pinned) still works — discovery must leave the transport in a clean state so the next call can re-open a fresh socket.

---

## Suggested release steps

1. Apply the three changes above to the upstream `dorkmo/FTPSclientOPTA` repo on a feature branch.
2. Commit message suggestion:
   ```
   Add FtpsClient::discoverFingerprint() for one-shot fingerprint pinning

   Lets callers run TCP + AUTH TLS + control-channel TLS handshake with
   cert validation disabled solely to capture the peer cert SHA-256
   fingerprint, then close. Used by SenaxTankAlarm v2.0.70+ to power
   a "Discover Fingerprint" button on the FTP settings page.

   Reuses MbedSecureSocketFtpsTransport::getPeerCertFingerprint() which
   already exists in the transport layer.
   ```
3. Open and merge a PR to `master`.
4. Tag the merge commit `v0.3.0` and push the tag.
5. Update the Arduino Library Manager index if applicable (or just rely on `actions/checkout@v5 repository: dorkmo/FTPSclientOPTA` resolving to `master` — current SenaxTankAlarm CI does that, so once `master` has the change the next SenaxTankAlarm build will pick it up automatically).

---

## How to verify SenaxTankAlarm consumer-side

After the upstream release, the next push to `SenaxTankAlarm` master will rebuild against the new library on a clean runner. The release artifact build (`auto-tag-on-version-bump.yml` → `release-firmware-112025.yml`) should:

- Resolve `dorkmo/FTPSclientOPTA` HEAD
- Find `discoverFingerprint` in `FtpsClient.h`
- Compile the server sketch (currently ~954 KB)
- Publish `TankAlarm-112025-Server-BluesOpta.ino.bin` as a release asset for the tag

If the library has *not* been published yet, the build will fail with an undefined-reference error pointing to `FtpsClient::discoverFingerprint`.
