# TankAlarm Config Files Guide

*Keep your credentials out of Git and your devices talking to each other.*

---

## Introduction

Every TankAlarm device — **Server**, **Client**, and **Viewer** — needs a **Product UID** to communicate through Blues Notehub. The Config.h file pattern gives you a clean, secure way to set that value at compile time without ever committing secrets to version control.

This guide walks you through creating and using Config.h files for all three device types. If you've ever cloned the repo and wondered why your device won't connect, start here.

---

## What You'll Learn

- What Config.h files are and how the firmware finds them
- How to create one for each device type (Server, Client, Viewer)
- How the `__has_include` preprocessor pattern works under the hood
- Alternative ways to set the Product UID without a Config.h file

---

## Time Required

**5 minutes** per device

---

## Prerequisites

| Requirement | Details |
|---|---|
| Arduino IDE | 2.0+ installed |
| TankAlarm repository | Cloned or downloaded |
| Notehub account | [notehub.io](https://notehub.io) — free tier works |
| Product UID | From your Notehub project (format: `com.company.product:project`) |

> 💡 **Where do I find my Product UID?** Log in to [notehub.io](https://notehub.io), open your project, and look at the top of the page — it's displayed as `com.your-org.your-project:label`. You can also find it under **Settings → General**.

---

## How It Works

Each firmware sketch uses the C/C++ `__has_include` preprocessor directive to check for a local Config.h file at compile time. Here's the pattern (shown for the Client):

```cpp
#if __has_include("ClientConfig.h")
  #include "ClientConfig.h"
  #ifndef DEFAULT_PRODUCT_UID
    #define DEFAULT_PRODUCT_UID ""
  #endif
#else
  #define DEFAULT_PRODUCT_UID ""
#endif
```

**What this does:**

1. **If `ClientConfig.h` exists** → include it and use the `DEFAULT_PRODUCT_UID` you defined
2. **If it doesn't exist** → fall back to an empty string (`""`)
3. **If the file exists but doesn't define the macro** → also fall back to empty

This means the firmware **always compiles** — with or without your Config.h file. No build errors, no manual editing of `.ino` files.

> 📂 **Security by default**: All Config.h files are listed in `.gitignore`. Only the `.example` templates are committed. Your credentials never end up in version control.

---

## The Three Config Files

| Device | Config File | Example Template | Define Name |
|---|---|---|---|
| **Server** | `ServerConfig.h` | `ServerConfig.h.example` | `DEFAULT_SERVER_PRODUCT_UID` |
| **Client** | `ClientConfig.h` | `ClientConfig.h.example` | `DEFAULT_PRODUCT_UID` |
| **Viewer** | `ViewerConfig.h` | `ViewerConfig.h.example` | `DEFAULT_VIEWER_PRODUCT_UID` |

> ⚠️ **Critical**: The Product UID must be **identical** across your server and all client devices. If they don't match, Notehub can't route messages between them and your devices will not communicate.

---

## Step-by-Step Setup

### Step 1: Server Config

The server is typically set up first since it provides the dashboard where you'll verify everything.

**1.1** Navigate to your server sketch folder:
```
TankAlarm-112025-Server-BluesOpta/
```

**1.2** Copy the example template:

| OS | Command |
|---|---|
| **Windows** | `copy ServerConfig.h.example ServerConfig.h` |
| **macOS / Linux** | `cp ServerConfig.h.example ServerConfig.h` |

Or simply duplicate the file in your file manager and rename it.

**1.3** Open `ServerConfig.h` in any text editor and replace the placeholder:

```cpp
#define DEFAULT_SERVER_PRODUCT_UID "com.your-company.your-product:your-project"
```

Change it to your actual Product UID:

```cpp
#define DEFAULT_SERVER_PRODUCT_UID "com.senax.tankalarm:production"
```

**1.4** Save the file.

> 💡 **Alternative**: You can skip this step entirely and set the Product UID at runtime through the server's web dashboard: **Server Settings** → **Blues Notehub** → **Product UID**.

**✓ Checkpoint**: Your server folder should now contain both `ServerConfig.h.example` (template) and `ServerConfig.h` (your actual config).

---

### Step 2: Client Config

Every client device needs the **same** Product UID as the server.

**2.1** Navigate to your client sketch folder:
```
TankAlarm-112025-Client-BluesOpta/
```

**2.2** Copy the example template:

| OS | Command |
|---|---|
| **Windows** | `copy ClientConfig.h.example ClientConfig.h` |
| **macOS / Linux** | `cp ClientConfig.h.example ClientConfig.h` |

**2.3** Open `ClientConfig.h` and set the Product UID — **must match the server**:

```cpp
#define DEFAULT_PRODUCT_UID "com.senax.tankalarm:production"
```

**2.4** Save the file.

> 💡 **Alternative**: Flash the client without a Config.h file. Then use the server's **Config Generator** page to push the Product UID at runtime — it auto-fills from the server settings so you can't accidentally mismatch.

> ⚠️ **Warning**: If you flash a client with an empty Product UID and no server-pushed config, you'll see this warning in the serial monitor:
> ```
> WARNING: No Product UID configured! Create ClientConfig.h or push config from server Config Generator.
> ```

**✓ Checkpoint**: Compile the client sketch — no errors about missing files or undefined macros.

---

### Step 3: Viewer Config (Optional)

The Viewer is a read-only kiosk display. Only set this up if you're deploying a Viewer device.

**3.1** Navigate to your viewer sketch folder:
```
TankAlarm-112025-Viewer-BluesOpta/
```

**3.2** Copy the example template:

| OS | Command |
|---|---|
| **Windows** | `copy ViewerConfig.h.example ViewerConfig.h` |
| **macOS / Linux** | `cp ViewerConfig.h.example ViewerConfig.h` |

**3.3** Open `ViewerConfig.h` and set the Product UID:

```cpp
#define DEFAULT_VIEWER_PRODUCT_UID "com.senax.tankalarm:production"
```

**3.4** Save the file.

**✓ Checkpoint**: Viewer sketch compiles without errors.

---

## Quick Reference

### Do I Need a Config.h File?

| Scenario | Config.h Needed? | Alternative |
|---|---|---|
| Fresh deployment, want it working on first boot | **Yes** | — |
| Server already running, pushing config to clients | **No** | Use Config Generator |
| Production fleet, credentials in CI/CD | **Yes** | Build-time injection via GitHub secret |
| Just testing locally | **No** | Set via web dashboard |

---

## CI/CD: Build-Time Injection via GitHub Secret (Optional)

If you use the **GitHub Actions firmware build workflow** (`.github/workflows/arduino-ci-112025.yml`), you can inject the Product UID at build time using a GitHub repository secret. This is **optional** — you only need this if you want CI-built `.bin` files to have the Product UID baked in. If you build locally with your own Config.h files, or configure devices at runtime through the web dashboard, you can skip this section entirely.

### Step 1: Add the Secret

1. Go to your GitHub repository on github.com
2. Click **Settings** → **Secrets and variables** → **Actions**
3. Click **New repository secret**
4. Set the name to: `BLUES_PRODUCT_UID`
5. Set the value to your Product UID (e.g. `com.your-company.your-product:your-project`)
6. Click **Add secret**

### Step 2: That's It

The build workflow already has a step that generates all three Config.h files from this secret before compiling:

- `ClientConfig.h` → `DEFAULT_PRODUCT_UID`
- `ServerConfig.h` → `DEFAULT_SERVER_PRODUCT_UID`
- `ViewerConfig.h` → `DEFAULT_VIEWER_PRODUCT_UID`

The generated files are never committed — they only exist during the build.

### What Happens Without the Secret?

If you don't add the `BLUES_PRODUCT_UID` secret, the workflow still runs and compiles successfully. The Config.h files are generated with an empty Product UID, which means:

- Firmware `.bin` files will work but devices start with no Product UID configured
- You'll need to set the Product UID at runtime (server web dashboard, or push config to clients)

This is the same behavior as building locally without a Config.h file.

---

### What If I Don't Create One?

The firmware compiles and runs fine — the Product UID defaults to an empty string. You'll need to set it another way:

- **Server**: Web dashboard → Server Settings → Blues Notehub → Product UID
- **Client**: Server pushes config via Config Generator (requires server to be configured first)
- **Viewer**: Configuration JSON pushed from server

### What If I Mess Up the Product UID?

Symptoms of a Product UID mismatch:
- Client sends telemetry but server never receives it
- Server dashboard shows "No clients configured yet" forever
- Notehub shows events going to the wrong project

**Fix**: Check the Product UID on each device (serial monitor at boot) and ensure they all match exactly.

---

## File Structure Overview

After setup, your project tree should look like this:

```
TankAlarm-112025-Server-BluesOpta/
├── TankAlarm-112025-Server-BluesOpta.ino
├── ServerConfig.h.example      ← template (committed to Git)
├── ServerConfig.h              ← YOUR config (gitignored)
└── ...

TankAlarm-112025-Client-BluesOpta/
├── TankAlarm-112025-Client-BluesOpta.ino
├── ClientConfig.h.example      ← template (committed to Git)
├── ClientConfig.h              ← YOUR config (gitignored)
└── ...

TankAlarm-112025-Viewer-BluesOpta/
├── TankAlarm-112025-Viewer-BluesOpta.ino
├── ViewerConfig.h.example      ← template (committed to Git)
├── ViewerConfig.h              ← YOUR config (gitignored)
└── ...
```

The `.gitignore` entries that protect your configs:

```gitignore
**/ServerConfig.h
**/ClientConfig.h
**/ViewerConfig.h
```

---

## Troubleshooting

### "WARNING: No Product UID configured!"

**Cause**: No Config.h file found and no runtime config has been pushed.

**Fix**: Either create the appropriate Config.h file (see steps above) or push the config from the server's Config Generator page.

### Firmware Won't Compile After Creating Config.h

**Check**:
1. File is in the **same folder** as the `.ino` file (not a subfolder)
2. File name is exact: `ServerConfig.h`, `ClientConfig.h`, or `ViewerConfig.h`
3. File uses proper C syntax — check for missing quotes or semicolons
4. The `#define` name matches what the firmware expects (see table above)

### Client and Server Can't Communicate

1. Verify Product UIDs match **exactly** (case-sensitive, including the `:project` suffix)
2. Check serial monitor on both devices at boot — both print their Product UID
3. In Notehub, verify both devices are in the **same project**
4. Ensure the Route Relay is configured between fleets

### Config.h Accidentally Committed to Git

If you accidentally committed a Config.h file:

```bash
git rm --cached TankAlarm-112025-Server-BluesOpta/ServerConfig.h
git commit -m "Remove ServerConfig.h from tracking"
```

The `.gitignore` will prevent it from being tracked again.

---

## Further Reading

- [Quick Start Guide](QUICK_START_GUIDE.md) — Full setup walkthrough
- [Server Installation Guide](SERVER_INSTALLATION_GUIDE.md) — Detailed server setup
- [Client Installation Guide](CLIENT_INSTALLATION_GUIDE.md) — Detailed client setup
- [Fleet Setup Guide](FLEET_SETUP_GUIDE.md) — Notehub fleet architecture
- [Troubleshooting Guide](TROUBLESHOOTING_GUIDE.md) — Common issues and fixes

---

*TankAlarm v1.1.3 — Config Files Guide*
