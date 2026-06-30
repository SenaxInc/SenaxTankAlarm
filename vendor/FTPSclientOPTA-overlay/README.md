# FTPSclientOPTA Local Overlay (temporary bridge)

This directory contains a **minimal overlay** of files from `dorkmo/FTPSclientOPTA` that include the `FtpsClient::discoverFingerprint(...)` method added on **2026-06-29** for the TankAlarm server's "Discover Fingerprint" button.

The companion upstream-change document is at [`../../CODE REVIEW/FTPSCLIENTOPTA_UPSTREAM_CHANGES_06292026.md`](../../CODE%20REVIEW/FTPSCLIENTOPTA_UPSTREAM_CHANGES_06292026.md). Once that diff is committed to the public `dorkmo/FTPSclientOPTA` repo and released as `v0.3.0`, **delete this entire `vendor/FTPSclientOPTA-overlay/` directory** and remove the "Apply local FTPSclientOPTA overlay" step from `.github/workflows/arduino-ci-112025.yml` and `.github/workflows/release-firmware-112025.yml`.

## How it works

The Arduino CI workflows do:

1. Check out `dorkmo/FTPSclientOPTA` into `arduino-opta-ftps/`.
2. **(this overlay)** Copy every file in `vendor/FTPSclientOPTA-overlay/` over the matching path inside `arduino-opta-ftps/`, replacing the upstream copy with our patched version.
3. Copy `arduino-opta-ftps/src` and `library.properties` into the libraries root the way they did before.
4. Compile.

If the upstream `dorkmo/FTPSclientOPTA` ever moves ahead of these overlaid files (e.g. picks up unrelated bug fixes), the overlay will silently mask those upstream changes — that's another reason to remove the overlay as soon as upstream catches up.

## Files

| Overlay path | Replaces upstream file |
|---|---|
| `src/FtpsClient.h` | `src/FtpsClient.h` |
| `src/FtpsClient.cpp` | `src/FtpsClient.cpp` |
| `library.properties` | `library.properties` |

The overlay tracks `FTPSclientOPTA 0.3.0` (pre-release). The `library.properties` `version` field reflects that.
