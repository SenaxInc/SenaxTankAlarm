# TankAlarm 112025 - Bill of Materials

**Version:** 1.6.14  
**Platform:** Arduino Opta + Blues Wireless  
**Last Updated:** April 18, 2026

---

## Core Components

### Client System (Remote Tank Monitoring)

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
1 | Arduino Opta Lite | AFX00003 | [Arduino USA Store](https://store-usa.arduino.cc/products/opta-lite) | Industrial controller (STM32H747XI) - $151
1 | Blues Wireless for Opta | N/A | [Blues Store](https://shop.blues.com/collections/accessories/products/wireless-for-opta) | Cellular Notecard carrier - $170
1 | Blues Notecard (North America) | NOTE-NBGL | [Blues Store](https://shop.blues.com/collections/notecard/products/notecard-cellular) | LTE-M/NB-IoT cellular - $45-49
1 | Arduino Opta Ext A0602 (Optional) | AFX00007 | [Arduino USA Store](https://store-usa.arduino.cc/products/opta-ext-a0602) | 4-20mA analog expansion - $229
1 | 24V DC Power Supply | N/A | Various | DIN rail mount recommended - $30-50
1 | Solar Charge Controller (Optional) | SunKeeper-6 | [Solar Electric](https://www.solar-electric.com/mosupamoco6a.html) | For remote solar installations - ~$60
1 | Battery (Optional) | Optima D31T | [Walmart](https://www.walmart.com/ip/OPTIMA-YellowTop-Dual-Purpose-Battery-Group-d31t/579876980) | For backup/solar power - ~$250

### Server System (Office/Headquarters)

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
1 | Arduino Opta Lite | AFX00003 | [Arduino USA Store](https://store-usa.arduino.cc/products/opta-lite) | Industrial controller - $151
1 | Blues Wireless for Opta | N/A | [Blues Store](https://shop.blues.com/collections/accessories/products/wireless-for-opta) | Cellular Notecard carrier - $170
1 | Blues Notecard (North America) | NOTE-NBGL | [Blues Store](https://shop.blues.com/collections/notecard/products/notecard-cellular) | LTE-M/NB-IoT cellular - $45-49
1 | Ethernet Cable | Cat5e/Cat6 | Various | RJ45 to local network - $5-10
1 | 24V DC Power Supply | N/A | Various | DIN rail mount recommended - $30-50

### Viewer System (Optional - Remote Display)

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
1 | Arduino Opta Lite | AFX00003 | [Arduino USA Store](https://store-usa.arduino.cc/products/opta-lite) | Industrial controller - $151
1 | Blues Wireless for Opta | N/A | [Blues Store](https://shop.blues.com/collections/accessories/products/wireless-for-opta) | Cellular Notecard carrier - $170
1 | Blues Notecard (North America) | NOTE-NBGL | [Blues Store](https://shop.blues.com/collections/notecard/products/notecard-cellular) | LTE-M/NB-IoT cellular - $45-49
1 | Ethernet Cable | Cat5e/Cat6 | Various | RJ45 to local network - $5-10
1 | 24V DC Power Supply | N/A | Various | DIN rail mount recommended - $30-50

---

## Sensors & Accessories

### Tank Level Sensors

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
1+ | Pressure Transmitter 4-20mA (0-5 PSI) | 626-06-CB-P1-E5-S1 | [Dwyer](http://www.dwyer-inst.com/configurator/index.cfm?Group_ID=98) | Bottom-mounted pressure sensor
1+ | Float Switch (N.C.) | 3BY75 | [Grainger](https://www.grainger.com/product/DAYTON-Float-Switch-3BY75) | High/low level detection
N/A | Analog Voltage Sensor (0-10V) | Various | N/A | General purpose analog sensors

### M12 Connectors (for field wiring)

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
2+ | M12 Connector | 21033112400 | [Mouser](https://www.mouser.com/ProductDetail/617-21033112400) | Panel mount connector
2+ | M12 Plug Male | 21032211405 | [Mouser](https://www.mouser.com/ProductDetail/617-21032211405) | Field cable connector
2+ | M12 Plug Female | 21032212405 | [Mouser](https://www.mouser.com/ProductDetail/617-21032212405) | Field cable connector

### Relay & Control

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
1 | 2-Channel 5V Relay Module (Optional) | N/A | [RobotShop](https://www.robotshop.com/en/2-channel-5v-relay-module-elf.html) | For valve control (if needed)
1 | Terminal Block | GTB-406 | [Home Depot](https://www.homedepot.com/p/Gardner-Bender-22-10-AWG-6-Circuit-Terminal-Block-1-Pack-GTB-406/202522482) | For field wiring

### Wiring & Cables

Qty | Component | Part Number | Link | Notes
--- | --------- | ----------- | ---- | -----
1 | Jumper Wire Kit (M-M) | MIKROE-513 | [Mouser](https://www.mouser.com/ProductDetail/932-MIKROE-513) | Prototyping connections
1 | Jumper Wire Kit (M-F) | MIKROE-512 | [Mouser](https://www.mouser.com/ProductDetail/932-MIKROE-512) | Prototyping connections

---

## Enclosures & Mounting

### Recommended Enclosures

Qty | Component | Link | Notes
--- | --------- | ---- | -----
1 | DIN Rail Mount Enclosure | Various | NEMA 4X rated for outdoor use
1 | DIN Rail (35mm) | Various | For mounting Opta controllers
1 | Cable Glands | Various | IP67 rated for outdoor installations

---

## Optional Components

### Solar Power System (Remote Sites)

Qty | Component | Link | Notes
--- | --------- | ---- | -----
1 | Solar Panel (50-100W) | Various | 12V nominal output
1 | Solar Charge Controller | [Solar Electric](https://www.solar-electric.com/mosupamoco6a.html) | SunKeeper-6 or equivalent
1 | Deep Cycle Battery | [Walmart](https://www.walmart.com/ip/OPTIMA-YellowTop-Dual-Purpose-Battery-Group-d31t/579876980) | Optima D31T or equivalent
1 | Solar Panel Mounting Hardware | Various | Ground or pole mount

### External Antenna System (Optional — Recommended for Metal Enclosures)

The Blues Wireless for Opta includes **two SMA connectors** with **two LTE cellular antennas**. These are:

| Connector | Purpose | Required? |
|-----------|---------|----------|
| **MAIN** | Primary LTE cellular antenna (transmit + receive) | Yes — required for cellular operation |
| **DIV** | Diversity receive antenna (improves LTE Cat-1 downlink) | Optional — recommended for best signal |

> **Important:** Neither antenna is for Bluetooth or WiFi. Both are LTE cellular antennas. The Opta WiFi model has its own onboard Bluetooth radio that is separate from the Blues expansion module. GPS is integrated into the Notecard module itself and is not exposed via an external SMA connector on the Wireless for Opta.

The included stub antennas work well in open environments, but for installations inside **metal NEMA enclosures** or in **areas with weak cellular signal**, mounting external antennas on a pole significantly improves reliability. The Wireless for Opta uses a **wideband LTE Cat-1 Notecard** (Quectel EG91-NAX for North America) covering:

- **LTE Bands:** B2, B4, B5, B12, B13, B25, B26
- **WCDMA Bands:** B2, B4, B5
- **Frequency Range:** 698 MHz – 2700 MHz

External antennas should cover the full **698–2700 MHz** range for North American LTE.

#### Recommended Pole-Mount Omnidirectional LTE Antennas

Qty | Component | Frequency | Gain | Connector | Link | Notes
--- | --------- | --------- | ---- | --------- | ---- | -----
1-2 | Taoglas Barracuda OMB.698.B13N2 | 698–2700 MHz | 3–5 dBi | N-Female | [Taoglas](https://www.taoglas.com/product/barracuda-omni-698mhz-2-7ghz/) | Fiberglass radome, outdoor rated, pole mount ~$40-60
1-2 | Laird Connectivity MA950 | 698–2700 MHz | 2–3 dBi | N-Female | [Laird](https://www.lairdconnect.com/antennas/multiband-antennas) | Compact multiband omni, pole mount ~$25-45
1-2 | Panorama LPMM / LGMM Series | 698–2700 MHz | 2–5 dBi | N-Female | [Panorama](https://www.panorama-antennas.com/) | Wideband omni, IP67, pole mount ~$30-60
1-2 | Wilson Electronics 304421 | 700–2700 MHz | 3–5 dBi | N-Female | [Wilson](https://www.weboost.com/antennas) | Outdoor omni, pole mount ~$20-40
1-2 | PCTEL MLPV Series | 698–3100 MHz | 2–3 dBi | N-Female | [PCTEL](https://www.pctel.com/antenna-products/) | Permanent outdoor mount ~$35-55

> **Note:** You need **two** external antennas if you want full MAIN + DIV diversity. At minimum, one external antenna on the MAIN connector is required; the DIV antenna can be omitted (with reduced receive performance) or left as the included stub inside the enclosure.

#### SMA-to-N Adapter Chain

The Wireless for Opta has **SMA female** connectors. Most outdoor pole-mount antennas use **N-type** connectors. The recommended adapter chain from the Wireless for Opta SMA port to an external N-type antenna is:

```
[Wireless for Opta SMA-F] → SMA-M Pigtail Cable → SMA-F Bulkhead (enclosure wall)
    → SMA-M to N-M Adapter → N-M to N-M Coax Cable (LMR-400) → [Antenna N-F]
```

Qty | Component | Link | Notes
--- | --------- | ---- | -----
2 | SMA Male to SMA Female Bulkhead Adapter | [Mouser](https://www.mouser.com/) / [Digi-Key](https://www.digikey.com/) | Panel-mount pass-through for enclosure wall, IP67 rated ~$5-8 each
2 | SMA Male to SMA Male Pigtail Cable (6-12") | [Mouser](https://www.mouser.com/) / [Digi-Key](https://www.digikey.com/) | RG316 or LMR-195, connects Opta SMA to bulkhead ~$5-10 each
2 | SMA Female to N Male Adapter | [Mouser](https://www.mouser.com/) / [Digi-Key](https://www.digikey.com/) | Direct adapter, outdoor side of bulkhead ~$5-8 each
2 | N Male to N Male Coax Cable (LMR-400) | [Mouser](https://www.mouser.com/) / [Digi-Key](https://www.digikey.com/) | Low-loss cable, length as needed for pole run (3-25 ft) ~$15-40 each

> **Cable Selection:** Use **LMR-400** (or equivalent like Times Microwave LMR-400, CommScope CNT-400) for runs over 3 feet. LMR-400 has ~1.5 dB loss per 10 feet at 2 GHz vs. ~5 dB/10ft for RG-58. For short runs under 3 feet, LMR-195 or RG-316 is acceptable. Minimize total cable length to preserve signal quality.

> **Weatherproofing:** Apply self-amalgamating tape or weatherproof boots on all outdoor N-type connections to prevent moisture ingress.

---

## Blues Wireless Service

Item | Link | Notes
---- | ---- | -----
Blues Notehub Account | [notehub.io](https://notehub.io) | Free tier available; 500MB/month included
Cellular Data Plan | [Blues Wireless](https://blues.io/pricing/) | Pay-as-you-go or prepaid options

---

## Cost Estimates (USA Pricing, January 2026)

### Basic Client System
- Arduino Opta Lite: $151
- Blues Wireless for Opta: $170
- Blues Notecard Cellular: $45-49
- Power Supply: $30-50
- **Subtotal: ~$396-420**

### Basic Server System
- Arduino Opta Lite: $151
- Blues Wireless for Opta: $170
- Blues Notecard Cellular: $45-49
- Power Supply: $30-50
- Ethernet Cable: $5-10
- **Subtotal: ~$401-430**

### Sensors (per tank)
- Pressure Transmitter (4-20mA): $150-250
- Float Switch: $50-100
- M12 Connectors/Cables: $50
- **Subtotal: ~$250-400 per tank**

### Analog Expansion (optional per client)
- Arduino Opta Ext A0602: $229

### Optional Solar System (per remote site)
- Solar Panel: $80-150
- Charge Controller: $60
- Battery: $250
- Mounting Hardware: $50
- **Subtotal: ~$440-510**

### Optional External Antenna System (per system in metal enclosure)
- Outdoor LTE Omni Antenna (x2): $40-120
- SMA Bulkhead Adapters (x2): $10-16
- SMA Pigtail Cables (x2): $10-20
- SMA-to-N Adapters (x2): $10-16
- N-Male Coax Cable LMR-400 (x2): $30-80
- Weatherproofing tape/boots: $5-10
- **Subtotal: ~$105-262** (for 2-antenna system)
- **Subtotal: ~$55-135** (for 1-antenna MAIN-only system)

**Note:** Prices shown are from USA vendors and do not include shipping or sales tax. International pricing may vary. Check vendor websites for current pricing and availability.

---

## Compatibility Notes

### Arduino Opta Models
- **Opta Lite** (AFX00003) - Recommended for most installations
- **Opta WiFi** (AFX00002) - Includes built-in RS485 and WiFi/BLE
- **Opta RS485** (AFX00001) - Includes built-in RS485 (no WiFi/BLE)

### Blues Notecard Models
- **NOTE-NBGL-500** - North America (LTE-M/NB-IoT)
- **NOTE-WBEX-500** - Global (LTE-M/NB-IoT)
- **NOTE-CELL-500** - Global (2G/3G fallback)

### Blues Wireless for Opta Antenna Details
- The Wireless for Opta includes **2 SMA connectors** and **2 LTE stub antennas** (MAIN + DIV)
- **MAIN** antenna: Required for cellular operation (transmit + receive)
- **DIV** antenna: Optional diversity receive antenna — improves LTE Cat-1 downlink signal
- **Neither antenna is Bluetooth or WiFi** — both are LTE cellular only
- **GPS** is integrated into the Notecard module (u.FL on the M.2 card); not exposed as external SMA on the Wireless for Opta
- The Notecard inside is a **wideband LTE Cat-1** module (NOTE-WBNA / Quectel EG91-NAX for North America)
- For installations in metal enclosures or weak-signal areas, replace included stub antennas with external pole-mount antennas via SMA→N adapter chain (see Optional Components section)

### Sensor Compatibility
The system supports multiple sensor types:
- **4-20mA Current Loop** - Requires Arduino Opta Ext A0602
- **0-10V Analog** - Requires Arduino Opta Ext A0602
- **Digital On/Off** - Direct connection to Opta digital inputs
- **Pulse/Frequency** - Direct connection to Opta digital inputs

---

## Where to Buy

### Official Distributors (USA)
- **Arduino USA Store:** [store-usa.arduino.cc](https://store-usa.arduino.cc) - Arduino Opta products
- **Blues Wireless Shop:** [shop.blues.com](https://shop.blues.com) - Notecards and accessories
- **Mouser Electronics:** [mouser.com](https://mouser.com) - Electronic components, connectors
- **Digi-Key:** [digikey.com](https://digikey.com) - Electronic components
- **Newark:** [newark.com](https://newark.com) - Industrial electronics

### Industrial Suppliers (USA)
- **Grainger:** [grainger.com](https://grainger.com) - Sensors, switches, enclosures, power supplies
- **Automation Direct:** [automationdirect.com](https://automationdirect.com) - Industrial automation components
- **McMaster-Carr:** [mcmaster.com](https://mcmaster.com) - Mounting hardware, enclosures

---

**For detailed installation and wiring instructions, see:**
- Client: [TankAlarm-112025-Client-BluesOpta/README.md](TankAlarm-112025-Client-BluesOpta/README.md)
- Server: [TankAlarm-112025-Server-BluesOpta/README.md](TankAlarm-112025-Server-BluesOpta/README.md)
