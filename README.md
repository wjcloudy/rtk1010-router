# rtk1010-router

![build](https://github.com/wjcloudy/rtk1010-router/actions/workflows/build.yml/badge.svg)

A WiFi web router for the **RTK1010 board** (ESP32-S2 + LOCOSYS RTK-1010 GNSS). It bridges
serial between the onboard GNSS, an optional **Ebyte E220-900T22D LoRa** radio, **USB-CDC**,
and a raw **TCP** socket — and serves a full single-page web app to control routing, configure
both radios, watch live signal/position charts, and update firmware over the air.

The whole thing is a single Arduino/PlatformIO project: firmware in [`src/`](src/), a no-build-step
preact + chart.js UI in [`data/`](data/) served from SPIFFS.

---

## Features

- **4 routing modes** (web-selectable, saved to NVS):
  - **GPS** — USB/TCP ⇄ RTK-1010 (transparent passthrough)
  - **LoRa** — USB/TCP ⇄ LoRa radio
  - **Rover** — LoRa RTCM → GPS (corrections in), GPS → host (output only)
  - **Hybrid** — RTCM → GPS *and* full-duplex GPS ⇄ host
  GPS and LoRa are always decoded in every mode, so the dashboard stays live regardless.
- **Dashboard** with chart.js: per-satellite **C/N0 signal bars** (filled = used in fix,
  outline = tracked), **position scatter** (offset from the running average, auto-scaling,
  last 500 fixes, reset), fix-quality / HDOP / satellites / throughput / RSSI history.
- **Fix status**: quality (GPS / DGPS / RTK Float / RTK Fixed) + dimension (2D/3D) + HDOP,
  plus a live-measured NMEA **output rate**.
- **RTK-1010 config**: read/parse version, fix interval (PAIR050), output rate (FIXRATE),
  RTK rover/base, DGPS/RTCM source (PAIR400/401), port baud (PAIR864, with auto module
  reboot + ESP UART re-sync), save-to-flash (handles the multi-Hz power-cycle requirement),
  and restore-to-defaults. Free-form command box + live reply log.
- **E220 LoRa config**: address, channel/frequency, air rate, UART baud, parity, power,
  packet size, WOR cycle, LBT, channel/packet RSSI, fixed/transparent mode, encryption key,
  and the mode pin (M0+M1 tied) + AUX — read/write over a config-mode switch.
- **Live consoles** for the GPS (NMEA), LoRa (RTCM, hex) and host-in streams.
- **OTA** app + filesystem updates over WiFi, browser firmware upload, device diagnostics
  (uptime, free/min heap), and a raw **TCP NMEA/RTCM server on `:10110`** (no DTR needed).
- Responsive UI: the nav rail collapses to icons on narrow screens; installable PWA.

---

## Hardware & pinout

ESP32-S2 (single core, native USB-OTG, **2 hardware UARTs**). All pins below are power-on
defaults and are **runtime-configurable from the web UI** (saved to NVS).

| Function | UART | Default pins | Notes |
|---|---|---|---|
| RTK-1010 GNSS | UART1 | TX `GPIO1` / RX `GPIO2` | onboard, via R3/R4 |
| GNSS enable | — | `GPIO4` (active high) | also used to reboot the module |
| LoRa (E220) data | UART0 | TX `GPIO33` / RX `GPIO35` | J5 "Conn_GPS" header (no soldering) |
| E220 mode pin | — | M0+M1 (tied) `GPIO38` / AUX `GPIO37` | M0 and M1 share one GPIO; AUX `-1` disables |

USB-CDC (`Serial`) is the host data channel — there is **no serial console**; status lives
on the web page.

### WiFi

- **STA**: joins your network; hostname `rtk1010` → reach the UI at `http://rtk1010.local`.
- **AP**: SSID `RTK1010` / pass `gps12345` → `http://192.168.4.1`.

Set credentials on the **System** tab (saved to NVS, reboots to apply).

---

## Quick start

```bash
pio run                                  # build firmware
```

### First flash (USB, once)

The custom partition table can only be written over USB. Put the S2 into ROM download mode
(BOOT0 jumper, or the on-page `/download` trigger once running), then:

```bash
pio run -t upload        # app + partition table
pio run -t uploadfs      # web UI (SPIFFS)
```

> The S2's app USB-CDC port is often held open by an editor's serial monitor — if esptool
> reports *"a device attached to the system is not functioning"* (errno 13 = busy), close the
> monitor or just use OTA below.

### After that — OTA over WiFi (no COM port)

```bash
pio run -e rtk1010_web_ota -t upload     # app firmware
pio run -e rtk1010_web_ota -t uploadfs   # web UI
```

Targets `rtk1010.local`. On the board's own AP, set `upload_port = 192.168.4.1` in
[`platformio.ini`](platformio.ini). The web page's **System → Firmware** card also accepts a
browser `.bin` upload.

---

## HTTP API

| Endpoint | Purpose |
|---|---|
| `GET /status` | JSON: mode, WiFi, fix/fixmode/sats/hdop/lat/lon, output rate, per-constellation counts, byte counters, uptime/heap, E220 pins |
| `GET /sats` | per-satellite list `{v:svid, c:C/N0, s:constellation, u:used}` |
| `GET /rtcmstats` | RTCM message-type histogram |
| `GET /mode?set=gps\|lora\|rover\|hybrid` | set routing mode |
| `GET /ports?gpsbaud=&lorabaud=&gtx=&grx=&ltx=&lrx=` | ESP UART baud + pins |
| `GET /gpscmd?cmd=` | send a LOCOSYS NMEA command (auto `$`/checksum) |
| `GET /gpsbaud?baud=` | change the **module** port baud (PAIR864 + save + reboot + ESP re-sync) |
| `GET /gpssave` | save GPS config to flash (power-cycles GNSS for the multi-Hz case) |
| `GET /gpsreset` | restore factory defaults (PAIR514) + auto baud re-sync |
| `GET /loracfg[?write=1&...]` | read / write E220 registers |
| `GET /console?src=gps\|lora\|host\|resp&since=N` | raw stream tap (X-Next cursor) |
| `POST /wifi` · `GET /reboot` · `GET /download` · `POST /update` | system |

---

## RTK-1010 (LOCOSYS) notes

Grounded in the RTK1010 software command list:

- **Version**: query `$PLSC,VER` → `$PLSR,VER,<model>,<fw>,<lib>,<n>` (e.g. `RTK35X,V1.1X0314AS`).
- **Fix interval** vs **output rate** are *separate*: `$PAIR050,<ms>` (100–1000 ms,
  query `$PAIR051`) sets the position-fix interval; `$PLSC,FIXRATE,<1|5|10>` sets the NMEA
  output rate (which the firmware also measures live from the GGA cadence).
- **DGPS/RTCM source**: `$PAIR400,<0 off|1 RTCM|2 SBAS|3 SLAS>`, query `$PAIR401`.
- **Port baud** (`$PAIR864,0,0,<baud>`): supports 115200/230400/460800/921600/3000000 and
  **only takes effect after the module reboots** — `/gpsbaud` issues it, saves, power-cycles
  the module via the enable pin, then re-syncs the ESP UART.
- **Save** (`$PAIR513`) **silently fails at multi-Hz** unless the GNSS is powered off first.
  On this module the power commands are **inverted from their names**: `$PAIR003` = power
  **off**, `$PAIR002` = power **on**. `/gpssave` and `/gpsreset` handle the off → save/restore
  → on sequence (and `/gpsreset` re-detects the baud afterwards).

## E220 LoRa notes

Register layout verified against
[xreef/EByte_LoRa_E220_Series_Library](https://github.com/xreef/EByte_LoRa_E220_Series_Library):
`SPED = baud(5-7)|parity(3-4)|air(0-2)`, `OPTION = subpacket(6-7)|RSSInoise(5)|power(0-1)`,
`TRANSMISSION_MODE = packetRSSI(7)|fixed(6)|LBT(4)|WORcycle(0-2)`, `CHAN` → `850.125 + CHAN` MHz.
Reads/writes briefly enter config mode (M0=1, M1=1, UART 9600 8N1) then restore transparent mode.

---

## Gotchas

- **USB-CDC needs DTR.** The ESP32-S2 Arduino `USBCDC` only delivers TX while the host asserts
  DTR. Apps that open the port with DTR off receive nothing. Use the **TCP server on `:10110`**
  (no DTR gate) for those, or enable DTR in your GPS app.
- **Partition table changes need a USB flash** — OTA can't rewrite it. After the one-time USB
  flash, everything is OTA.
- Charts/UI are vendored (preact, htm, chart.js as gzipped UMD) with **no build step** — edit
  `data/app.js` and `pio run -t uploadfs`.

---

## Repository layout

```
platformio.ini            PlatformIO project (envs: rtk1010_web [USB], rtk1010_web_ota [WiFi])
src/main.cpp              firmware: router task, decoders, web server, OTA, endpoints
data/                     web UI served from SPIFFS
  index.html  app.js  style.css   preact/htm/chart.js (gzipped UMD)  favicon.svg  manifest.json  sw.js
.github/workflows/build.yml   CI: build firmware + filesystem image, attach to tagged releases
```

## License

[MIT](LICENSE)
