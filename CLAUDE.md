# Power Telemetry v2 — CLAUDE.md

## Workflow Rules

### Plan before code
When the user asks for a plan, design, analysis, or approach, do NOT read code or make edits until the plan is agreed upon. Produce the plan first — (1) files to change, (2) design tradeoffs, (3) edge cases — then wait for explicit approval before implementing.

### Narrow fix scope
Fix exactly the bug or feature requested. Do not expand into adjacent refactors or related fixes without explicit approval. If related issues are spotted, list them at the end as suggestions, not edits.

### Build verification
After any firmware or filesystem edits, run `pio run -e <env>` to verify the build compiles and report the output. Do not ask for permission to run read-only build or check commands.

---

## Project Overview

ESP32-based LoRa telemetry system for remotely monitoring and controlling AC loads
via PZEM-004T v3 power meters. The firmware compiles into two distinct roles from
a single codebase:

- **Gateway** — frequency-hopping TDMA master; hosts a web dashboard (LittleFS SPA,
  REST + WebSocket), manages up to 8 sensor nodes, persists energy totals and
  telemetry history to FRAM.
- **Sensor Node** — TDMA slave; samples voltage/current/power/energy via PZEM-004T v3,
  controls a relay, and reports to the gateway every superframe (~3 s).

---

## Tech Stack

| Layer       | Technology                                                                             |
| ----------- | -------------------------------------------------------------------------------------- |
| MCU         | ESP32 (esp32dev) / ESP32-S3 Super Mini (node S3 variants)                             |
| Framework   | Arduino (PlatformIO)                                                                   |
| Radio       | SX1278 — RadioLib `^7.6.0`                                                          |
| Power meter | PZEM-004T v3 — PZEM-004T-v30 `^1.1.2`                                               |
| Web server  | ESPAsyncWebServer `^3.6.0` + AsyncTCP `^3.4.10`                                    |
| JSON        | ArduinoJson `^7.2.2`                                                                 |
| Persistence | LittleFS (gateway SPA only), NVS (WiFi creds), MB85RC256V FRAM via FRAM_I2C `^0.8.4` |
| RTOS        | FreeRTOS (built into ESP-IDF Arduino core)                                             |

---

## Build & Flash Commands

Ten PlatformIO environments are defined:

| Environment           | Board         | Role    | Encryption  | PZEM      |
| --------------------- | ------------- | ------- | ----------- | --------- |
| `gateway`             | esp32dev      | Gateway | off         | n/a       |
| `gateway_enc`         | esp32dev      | Gateway | AES-128 CTR | n/a       |
| `node`                | esp32dev      | Node    | off         | hardware  |
| `node_enc`            | esp32dev      | Node    | AES-128 CTR | hardware  |
| `node_nettest`        | esp32dev      | Node    | off         | simulated |
| `node_enc_nettest`    | esp32dev      | Node    | AES-128 CTR | simulated |
| `node_s3`             | S3 Super Mini | Node    | off         | hardware  |
| `node_s3_enc`         | S3 Super Mini | Node    | AES-128 CTR | hardware  |
| `node_s3_nettest`     | S3 Super Mini | Node    | off         | simulated |
| `node_s3_enc_nettest` | S3 Super Mini | Node    | AES-128 CTR | simulated |

```bash
# Build gateway firmware (includes LittleFS, web server, FRAM)
pio run -e gateway

# Build node firmware (PZEM, relay, no web)
pio run -e node

# Upload firmware
pio upload -e gateway
pio upload -e node

# Upload LittleFS filesystem image (gateway only — web SPA in data/)
pio run -e gateway -t uploadfs

# Open serial monitor
pio device monitor -e gateway   # 115200 baud
pio device monitor -e node
```

There are no automated tests. Verification is done via serial monitor output and the
web dashboard at `http://telemeter.local` (mDNS) or `http://192.168.4.1` (AP fallback).

### Optional features (build flags)

- **`-D PKT_ENCRYPTION`** — enables AES-128 CTR packet encryption and PN532 RFID key
  provisioning. The `gateway_enc` / `node_enc` / `node_enc_nettest` / `node_s3_enc` /
  `node_s3_enc_nettest` environments set this flag and add the `adafruit/Adafruit PN532`
  library dependency. Provisioning flow:
  1. Flash `gateway_enc` → key auto-generated on first boot (stored in NVS `lora-net`/`aeskey`).
  2. Web dashboard → "Write Key to Card" → tap MIFARE card to gateway PN532.
  3. Flash `node_enc` → blinks red until card is tapped; stores key; reboots.

- **`-D PZEM_FAKE`** — replaces PZEM hardware reads with simulated data. Used for
  network/protocol testing without AC metering hardware (`node_nettest`,
  `node_enc_nettest`, `node_s3_nettest`, `node_s3_enc_nettest` environments).

- **`-D BOARD_S3_SUPERMINI`** — selects ESP32-S3 Super Mini pin assignments for LoRa,
  PZEM UART, relay, LEDs, and PN532. Set automatically by the `flags:s3_supermini`
  feature group; do not set manually.

---

## Directory Structure

```
power-telemeterv2/
├── platformio.ini              # Ten envs: gateway, gateway_enc, node, node_enc, node_nettest, node_enc_nettest, node_s3, node_s3_enc, node_s3_nettest, node_s3_enc_nettest
├── CLAUDE.md
├── scripts/
│   └── gzip_data.py            # Pre-uploadfs: gzip-compresses data/ assets
├── src/
│   ├── main.cpp                # Unified entry point — NODE_GATEWAY or NODE_TELEMETRY
│   ├── tdma_protocol.h         # Shared: all packet structs, channel plan, timing constants
│   ├── pkt_crypto.h            # Encryption wrapper: packet TX/RX helpers (PKT_ENCRYPTION only)
│   ├── gateway_state.h         # Shared: NodeState, HistoryPoint, PendingCmd; extern declarations
│   ├── gateway_tdma_task.h/.cpp # TDMA engine (beacon, UL/DL slots, contention window)
│   ├── gateway_web.h/.cpp      # REST API + WebSocket + LittleFS static serve
│   ├── gateway_wifi_config.h/.cpp # AP + STA state machine, NVS credential store
│   ├── fram_store.h/.cpp       # MB85RC256V FRAM persistence (energy, history, node labels)
│   ├── node_tdma_task.h/.cpp   # Node state machine, PZEM sampling, relay, schedule
│   ├── log_async.h/.cpp        # Non-blocking ring-buffer serial logger + WebSocket relay
│   ├── crypto.h/.cpp           # AES-128 CTR encryption, NVS key store (PKT_ENCRYPTION only)
│   ├── rfid_provision.h/.cpp   # PN532 I2C MIFARE Classic key provisioning (PKT_ENCRYPTION only)
│   ├── node_fake_pzem.h/.cpp   # Simulated PZEM task for net-test builds (PZEM_FAKE only)
│   └── hal/                    # Platform HAL -- isolates ESP32-exclusive APIs
│       ├── hal_sys.h           # halReboot, halRandom, halFillRandom, halFreeHeap, halGetMac
│       ├── hal_rtos.h          # halTaskCreatePinned, HalCritSec_t spinlock
│       ├── hal_nvs.h           # halNvsOpen/Close/GetStr/PutStr/GetBlob/PutBlob/GetInt/PutInt/...
│       ├── hal_crypto.h        # halAesCtr (AES-128-CTR)
│       └── esp32/              # ESP32 implementations (only files that include esp_*.h, mbedtls, Preferences)
│           ├── hal_sys_esp32.cpp
│           ├── hal_rtos_esp32.cpp
│           ├── hal_nvs_esp32.cpp
│           └── hal_crypto_esp32.cpp
└── data/                       # LittleFS image root (gateway only); .gz variants built by gzip_data.py
    ├── index.html
    ├── app.js
    ├── chart.js
    ├── styles.css
    ├── simulation.js
    └── site.webmanifest
```

---

## Hardware Pinout

### SX1278 LoRa — ESP32-dev (gateway + standard node)

| Signal | GPIO |
| ------ | ---- |
| NSS    | 26   |
| MOSI   | 27   |
| MISO   | 14   |
| SCK    | 13   |
| DIO0   | 2    |
| RST    | 4    |
| DIO1   | N/C (RADIOLIB_NC; GPIO27 occupied by MOSI) |

### SX1278 LoRa — ESP32-S3 Super Mini (node S3 variants only)

| Signal | GPIO |
| ------ | ---- |
| NSS    | 4    |
| MOSI   | 5    |
| MISO   | 6    |
| SCK    | 7    |
| DIO0   | 8    |
| RST    | 9    |
| DIO1   | N/C  |

### Gateway only (ESP32-dev)

| Peripheral          | GPIO | Notes                              |
| ------------------- | ---- | ---------------------------------- |
| FRAM SDA            | 16   | Shared with PN532 in encrypted builds |
| FRAM SCL            | 17   | Shared with PN532 in encrypted builds |
| Button (active LOW) | 34   | Input-only; external pull-up required |

### Node only — ESP32-dev

| Peripheral          | GPIO | Notes                    |
| ------------------- | ---- | ------------------------ |
| PZEM TX (UART2)     | 22   | ESP32 TX → PZEM RX       |
| PZEM RX (UART2)     | 23   | ESP32 RX ← PZEM TX       |
| Relay (active HIGH) | 25   |                          |
| LED green           | 32   |                          |
| LED red             | 33   |                          |
| Button (active LOW) | 34   | Input-only; external pull-up required |

### Node only — ESP32-S3 Super Mini

| Peripheral          | GPIO | Notes                                              |
| ------------------- | ---- | -------------------------------------------------- |
| PZEM TX (UART2)     | 43   | S3 TX (U0TXD via GPIO matrix) → PZEM RX           |
| PZEM RX (UART2)     | 44   | S3 RX (U0RXD via GPIO matrix) ← PZEM TX           |
| Relay (active HIGH) | 3    | JTAG strapping pin; LOW at boot is safe            |
| LED green           | 11   | Re-asserted OUTPUT after SPI.begin() (FSPI clobber) |
| LED red             | 12   | Re-asserted OUTPUT after SPI.begin() (FSPI clobber) |
| Button (active LOW) | 13   |                                                    |

### Encrypted builds only (`PKT_ENCRYPTION`)

PN532 I2C shares the FRAM bus on the gateway and on ESP32-dev nodes. The S3 Super Mini
uses a dedicated I2C bus on THT GPIOs.

| Role              | PN532 SDA | PN532 SCL | Notes                       |
| ----------------- | --------- | --------- | --------------------------- |
| Gateway           | 16        | 17        | Shared with FRAM (0x50); PN532 addr 0x48 |
| Node (ESP32-dev)  | 16        | 17        | Same shared bus as gateway  |
| Node (S3 Super Mini) | 1      | 2         | Dedicated THT bus           |

---

## Analog Frontend (Circuit / SPICE)

The project currently uses PZEM-004T v3 for all AC measurements. A custom discrete
analog frontend using ZMPT101B (voltage) + HWCT004 (current) is planned as a future
extension or replacement for scenarios where higher integration or multi-phase sensing
is needed.

### SPICE source of truth

`circuit/analog_frontend.cir` is the authoritative circuit document. It is written
in standard SPICE netlist format compatible with ngspice and LTspice. Subcircuits
`ZMPT101B`, `HWCT004`, and `ADCInterface` are defined there.

### Pinout inference from SPICE

When reading the SPICE netlist, GPIO assignments are embedded directly in the node
names used in `ADCInterface` instance calls at the top level. Convention:

```
Xadc_v  adc_volt  gpio36  VCC  ADCInterface
```

Here `gpio36` is the SPICE node name AND the GPIO pin. To infer the full pinout from
a SPICE netlist:

1. Find every `ADCInterface` instance (`X...ADCInterface` lines).
2. The second positional argument (after `in`) is the `out` node -- that is the GPIO.
3. Trace any other top-level nodes named `gpioNN` or `gpiNN` to find non-ADC pins.
4. Cross-check against the Hardware Pinout tables above for conflicts.

### SPICE analysis workflow

When given a `.cir` netlist, the analysis steps are:

| Step | What to check |
| ---- | ------------- |
| `.op` / DC bias | Verify bias node (vbias = VCC/2 = 1.65 V) appears at all ADC nodes; flag any node stuck at rail |
| `.tran` waveform | Confirm ADC output stays within 0.1--3.1 V across the full mains cycle; check for clipping at the Schottky clamps |
| `.ac` sweep | Verify LP filter rolls off above ~1.5--2 kHz (fc = 1/(2*pi*R*C)); confirm <1% attenuation at 50/60 Hz fundamental |
| `.meas` values | Report peak/min/swing on each ADC node; flag swing < 100 mV (poor SNR) or > 3.1 V (ADC saturation) |
| Phase match | For power-factor measurement, voltage and current channels must have matched phase shift through their LP filters; use identical Rlpf/Clpf values |

### ADC GPIO candidates

These pins are currently unassigned and are free for the analog frontend:

**ESP32-dev (gateway + standard node):**

| Signal        | GPIO  | ADC channel   | Notes |
| ------------- | ----- | ------------- | ----- |
| Voltage sense | 36    | ADC1_CH0 (VP) | Input-only; no internal pull; lowest noise |
| Current sense | 39    | ADC1_CH3 (VN) | Input-only; no internal pull |
| Spare         | 35    | ADC1_CH7      | Input-only |

**ESP32-S3 Super Mini (node S3 variants):**

| Signal        | GPIO  | ADC channel | Conflict check |
| ------------- | ----- | ----------- | -------------- |
| Voltage sense | 10    | ADC1_CH9    | None (free) |
| Current sense | 14    | ADC2_CH3    | ADC2 shares arbitration with WiFi; prefer ADC1 |
| Spare         | 15    | ADC2_CH4    | Same ADC2 caveat |

- Use `ADC_11db` attenuation for 0--3.3 V range (`analogSetAttenuation(ADC_11db)` in esp-idf).
- ADC1 is preferred over ADC2 on both boards: ADC2 cannot be used while WiFi is active.
- Never open-circuit the secondary of a CT (HWCT004); the PCB must always have the burden resistor populated before the primary conductor is energised.

---

## TDMA Superframe Architecture

```
|<————————————————————————————— 3000 ms ——————————————————————————————>|
| Beacon | Slot 1 | Slot 2 | … | Slot 8 | CW UL | CW DL | Idle   | Guard |
|  40 ms | 165 ms | 165 ms |   | 165 ms | 60 ms | 40 ms | 1520 ms| 20 ms |

Each data slot (165 ms) — DL-first ordering:
  [DL TX 50 ms] [UL RX 100 ms] [Guard 15 ms]
```

**Slot timing anchor:** Node derives slot times from beacon receive timestamp:
```
dlStart = beaconReceiveTime + (BEACON_MS - BEACON_AIR_MS) + (slotId - 1) × SLOT_PAIR_MS
txTime  = dlStart + SLOT_DL_MS
```
`BEACON_AIR_MS = 18` accounts for on-air propagation of the 8-byte beacon at SF6.

**Frequency hopping:** `ch = (sfCount * 7 + slotId) % 8`
(coprime multiplier ensures every slot visits every channel across 8 superframes)

**Channel plan (433 MHz ISM, 200 kHz spacing):**
Ch 0 = 433.050 MHz (beacon/contention), Ch 1–7 = +0.2 MHz per step.

**Node lifecycle:** `LISTEN → CONTENDING → REGISTERED`
Join via contention window (JoinRequest 0xA0 / JoinAck 0xA1).

**Network epoch:** `BeaconPacket.epoch` (uint8_t) is incremented by 1 on every node
eviction. A registered node whose saved join epoch differs from the beacon epoch knows
its slot may have been reassigned and re-contends immediately. Eviction only occurs
when all 8 slots are full (`slotMask == 0xFF`) and a node exceeds `NODE_TIMEOUT_SFS`
(8 consecutive missed superframes ≈ 24 s).

---

## Packet Types

All structs are `#pragma pack(1)` (no alignment padding).

| Packet               | ID   | Direction  | Bytes |
| -------------------- | ---- | ---------- | ----- |
| BeaconPacket         | 0x04 | GW → All  | 8     |
| TelemetryPacket      | 0x01 | Node → GW | 32    |
| RelayCommandPacket   | 0x02 | GW → Node | 3     |
| RelaySchedulePacket  | 0x03 | GW → Node | 7     |
| RelayClearPacket     | 0x05 | GW → Node | 2     |
| ThresholdPacket      | 0x06 | GW → Node | 4     |
| NudgePacket          | 0x07 | GW → Node | 2     |
| JoinRequestPacket    | 0xA0 | Node → GW | 4     |
| JoinAckPacket        | 0xA1 | GW → Node | 4     |

All downlink packets share a 2-byte `DlHeader` prefix (`pktType` + `nodeId`).
All downlink frames are zero-padded to `MAX_DL_PAYLOAD_LEN = 7` bytes so the SF6
implicit-header RX window uses a fixed receive length.

---

## FRAM Persistence (Gateway)

**Chip:** MB85RC256V (32 KB), I²C address 0x50, SDA=16 SCL=17.

**Memory map (v2):**

```
0x0000  Magic (uint32_t = 0xDEADF00D) + version(1) + pad(11) = 16 bytes header
0x0010  Node[0] block (1968 bytes):
          +0   deviceUID   (uint16_t)
          +2   pad         (uint16_t)
          +4   accumEnergy (uint32_t)
          +8   histHead    (int32_t)
          +12  histCount   (int32_t)
          +16  label       (char[30])
          +46  pad2        (uint16_t, aligns history to offset 48)
          +48  history[120] (1920 bytes = 120 × 16 bytes per HistoryPoint)
0x07C0  Node[1] block …  (total 16 + 8×1968 = 15 760 bytes used)
```

**Write policy:**

- `framSaveEnergy()` / `framSaveHistory()`: dirty-counter (n=10), triggered from `accumulateEnergy()` / `addHistory()` in `gateway_tdma_task.cpp`. Actual writes are dispatched to `framTask` (Core 0) via a FreeRTOS queue (`framQueueSave()`) so I²C writes never block the Core 1 TDMA task.
- `framSaveLabel()`: queued immediately on every rename (low frequency, no dirty counter).

On node join, `framQueueRestore()` dispatches a restore request to `framTask`. The task searches **all 8 FRAM slots** by UID to locate the node's data regardless of which slot index it was previously assigned. This cross-slot UID lookup ensures labels, energy, and history survive gateway reboots even when nodes are reassigned to different slot IDs.

`framLoadAll()` is a no-op at boot; all data is restored lazily when a node joins and its UID is confirmed.

On version mismatch, the new version is stamped and restore is skipped — stale data is never applied to wrong memory offsets.

---

## Web API

### REST — Dashboard (`gateway_web.cpp`)

| Method | Path                       | Body / Response                                            |
| ------ | -------------------------- | ---------------------------------------------------------- |
| GET    | `/api/nodes`             | All registered node states                                 |
| GET    | `/api/node/{id}/live`    | Latest reading for one node                                |
| GET    | `/api/node/{id}/history` | 120-point V/I/P history                                    |
| POST   | `/api/node/{id}/relay`   | `state=0\|1` (curl/debug; WS preferred)                   |
| POST   | `/api/node/{id}/name`    | `name=<string>` (curl/debug; WS preferred)               |
| POST   | `/api/time`              | `hour=H&minute=M&second=S` (curl/debug; WS preferred)    |
| GET    | `/api/status`            | Uptime, heap, WiFi (ntpSynced, mode, ssid, RSSI, IP), node count |
| GET    | `/api/features`          | Active feature flags (`encryption`, etc.) for dashboard UI adaptation |
| POST   | `/api/reboot`            | Trigger halReboot() (auth-gated)                           |

**Dashboard authentication** (all public — no auth token required):

| Method | Path                | Body / Response                                       |
| ------ | ------------------- | ----------------------------------------------------- |
| POST   | `/api/login`       | `token=<password>` — validate and issue session token |
| GET    | `/api/logout`      | Invalidate session token                              |
| GET    | `/api/authstatus`  | `{required, authenticated}` — JS polls on load        |
| GET    | `/api/dashsecure`  | Get current password state                            |
| POST   | `/api/dashsecure`  | Set or clear dashboard password (stored in NVS)       |

> When no password is set (`dashpass` key absent or empty), all REST and WS endpoints are open. Once set, a `token` obtained from `/api/login` must be passed in the `Authorization` header or as the first WS message (`{cmd:"auth", token:"..."}`).

> `/api/node/{id}/relay`, `/name`, `/api/time` are kept for curl debugging and as HTTP fallback targets. The dashboard uses WebSocket commands for these operations.

### REST — WiFi Config (`gateway_wifi_config.cpp`)

| Method | Path                   | Body / Response                                             |
| ------ | ---------------------- | ----------------------------------------------------------- |
| GET    | `/api/info`          | AP SSID/IP, STA connection status                           |
| GET    | `/api/scan`          | Async WiFi scan; returns `{scanning, networks[]}`         |
| POST   | `/api/connect`       | `ssid=&password=` — save creds + begin STA              |
| GET    | `/api/wifistatus`    | `{apActive, connecting, connected, ip, ssid, rssi, ntpSynced}` |
| GET    | `/api/disconnect`    | Drop STA, restore AP                                        |
| GET    | `/api/forget`        | Clear NVS STA creds, drop STA, restore AP                   |
| GET    | `/api/staticip`      | Return current static IP config or DHCP state               |
| POST   | `/api/staticip`      | `ip=&gateway=&subnet=&dns=` — set static IP              |
| GET    | `/api/staticip/clear` | Revert to DHCP (removes static IP NVS keys)                |
| GET    | `/api/ap`            | AP SSID and whether a password is set                       |
| POST   | `/api/ap`            | `password=<string>` — set/clear AP password (min 8 chars if non-empty) |

Static IP is optional. When set, stored in NVS (`wifi-cfg`, keys `sip`/`sgw`/`ssn`/`sdns`); absent keys fall back to DHCP.

**NTP:** When STA connects, the gateway automatically calls `configTime()` with `pool.ntp.org`
and the stored timezone offset (`tzoff` key, default UTC+0). The `/api/status` and `/api/wifistatus`
responses include `ntpSynced`. While NTP is active, `set_time` WS commands are echoed back
to the requesting client without overwriting the synced clock. NTP re-anchors every hour.

A DNS catch-all redirects all queries to 192.168.4.1 while the AP is active, acting as
a captive portal for Android, iOS/macOS, Windows, and Firefox OS detection flows.

### REST — Encrypted builds only (`PKT_ENCRYPTION`)

| Method | Path             | Body / Response                                   |
| ------ | ---------------- | ------------------------------------------------- |
| POST   | `/api/provision` | Writes AES-128 key to a tapped MIFARE card (gateway only) |

### WebSocket (`/ws`)

**Client → server commands:** `relay_manual`, `relay_schedule`, `relay_clear`,
`set_threshold`, `nudge`, `rename`, `set_time`, `clear_energy`, `clear_all_energy`, `get_nodes`

**Server → client events:** `telemetry`, `nodes`, `name_changed`, `time_set`,
`relay_ack`, `schedule_ack`, `clear_ack`, `threshold_ack`, `nudge_ack`,
`energy_cleared`, `all_energy_cleared`, `log`

> `log` events carry a single serial log line (`{"type":"log","line":"..."}`) relayed from
> the async log ring buffer. They allow the web dashboard to display a live serial console.

---

## Coding Conventions

### Language & runtime

- C++11, no STL, no exceptions — Arduino + ESP-IDF APIs only.
- Error handling via return `bool` / early `return` guards, never `throw`.
- No heap allocation after `setup()` — all state in statically allocated globals.
- Use ASCII characters only — no Unicode in source code, comments, or string literals. Replacements: em dash `—` → `--`, en dash `–` → `-`, right arrow `→` → `->`, `×` → `x`, `µ` → `us`, `÷` → `/`, `²` → `2`, `≥` → `>=`, `≤` → `<=`, `≈` → `~=`, `±` → `+/-`, ellipsis `…` → `...`.
- Use spaces for tab indents. Indent width is two spaces

### Naming

| Scope               | Style                     | Example                           |
| ------------------- | ------------------------- | --------------------------------- |
| Global shared state | `g_` prefix, snake_case | `g_nodes`, `g_sfCount`        |
| Static module state | `s_` prefix, snake_case | `s_framOk`, `s_state`         |
| Functions           | camelCase                 | `processUplink`, `setChannel` |
| Macros / constants  | UPPER_SNAKE_CASE          | `BEACON_MS`, `MAX_NODES`      |
| Types / structs     | PascalCase                | `NodeState`, `BeaconPacket`   |
| Local variables     | snake_case                | `ns`, `idx`, `deltaWh`      |

### File naming

`<role>_<contract>.cpp/.h` — role prefix is `gateway_` or `node_`; omit prefix for
shared files (e.g., `lora_tdma_protocol.h`, `main.cpp`).

### Comments

- Public API: Doxygen `/** … */` block above declaration in `.h`.
- Section dividers: `// --- Section name -----------------------------------------------`.
- Inline: `//` short note on same or following line.

### Serial logging prefixes

Always use `[PREFIX]` format so output is grep-able.

| Prefix           | Source                    |
| ---------------- | ------------------------- |
| `[LORA]`       | Radio init                |
| `[FRAM]`       | FRAM persistence          |
| `[GW-TDMA]`    | Gateway TDMA task         |
| `[GW-UL]`      | Gateway uplink processing |
| `[GW-BCN]`     | Beacon TX                 |
| `[GW-DL]`      | Downlink TX               |
| `[GW-CW]`      | Contention window         |
| `[WEB]`        | Web server                |
| `[WS]`         | WebSocket events          |
| `[WIFI]`       | WiFi state machine        |
| `[NODE-TDMA]`  | Node TDMA task            |
| `[NODE-TX]`    | Node telemetry TX         |
| `[NODE-DL]`    | Node downlink RX          |
| `[NODE-JOIN]`  | Node join process         |
| `[NODE-RTC]`   | Schedule RTC sync         |
| `[NODE-SCHED]` | Schedule evaluation       |
| `[PZEM]`       | Power meter sampling      |

### Platform HAL layer

All ESP32-exclusive APIs are wrapped behind a thin HAL in `src/hal/`. When adding new
code that needs any of the following, use the HAL function -- never call the raw ESP32
API directly in application code:

| ESP32 API | HAL replacement | Header |
|---|---|---|
| `ESP.restart()` | `halReboot()` | `hal/hal_sys.h` |
| `esp_random()` | `halRandom()` | `hal/hal_sys.h` |
| `esp_fill_random(buf, n)` | `halFillRandom(buf, n)` | `hal/hal_sys.h` |
| `ESP.getFreeHeap()` | `halFreeHeap()` | `hal/hal_sys.h` |
| `esp_efuse_mac_get_default(mac)` | `halGetMac(mac)` | `hal/hal_sys.h` |
| `xTaskCreatePinnedToCore(...)` | `halTaskCreatePinned(fn, name, bytes, param, pri, HAL_CORE_0/1, out)` | `hal/hal_rtos.h` |
| `portMUX_TYPE` + `portENTER/EXIT_CRITICAL(mux*)` | `HalCritSec_t` + `halCritSecInit/Enter/Exit` | `hal/hal_rtos.h` |
| `Preferences` class | `halNvsOpen/Close/GetStr/PutStr/GetInt/PutInt/GetBlob/PutBlob/Remove/Clear` | `hal/hal_nvs.h` |
| `mbedtls_aes_*` | `halAesCtr(buf, len, key16, nonce)` | `hal/hal_crypto.h` |

**Standard FreeRTOS stays unchanged** -- `xSemaphore*`, `xQueue*`, `vTaskDelay`,
`pdMS_TO_TICKS`, `ulTaskNotifyTake`, `xTaskNotifyGive`, `configASSERT`, etc. are
upstream freertos.org spec and are used directly.

To add a new platform: create `src/hal/<platform>/` with implementations of the four
headers and set build flags to select the correct impl files.

### FreeRTOS patterns

- **Mutex protect all `g_nodes[]` access** — both gateway TDMA task (Core 1) and web
  task (Core 0) must hold `g_nodesMutex` before reading or writing.
  ```cpp
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      // ... access g_nodes[] ...
      xSemaphoreGive(g_nodesMutex);
  }
  ```
- **Core pinning:** TDMA radio timing on Core 1 (priority 2); all other tasks on Core 0
  (priority 1). Use `halTaskCreatePinned()` with `HAL_CORE_0` or `HAL_CORE_1`; standard
  `xTaskCreate()` (no pinning) stays as-is.
- **Lightweight signaling:** use `xTaskNotifyGive()` / `ulTaskNotifyTake()` for simple
  one-shot signals (e.g., LED nudge); reserve queues for data.
- Keep ISR-context code minimal; radio interrupts handled via RadioLib callbacks only.

### Packet structs

- All packet structs in `tdma_protocol.h` must be `#pragma pack(1)`.
- Never add pointer members — packets are copied directly into/out of radio buffers.
- Fixed-point encoding: voltage ÷ 10, current ÷ 1000, power ÷ 10, frequency ÷ 10,
  power factor ÷ 100.

### Persistence rules

- **FRAM writes** are deferred via dirty counters (n=10) — do not write on every change.
- **LittleFS** is only for the web SPA (index.html, app.js, etc.); do not add new file types
  without considering flash wear and mount-failure recovery. Node labels are stored in FRAM, not LittleFS.
- **NVS** namespaces in use:
  - `wifi-cfg` — keys `ssid`, `pass` (STA credentials); `sip`, `sgw`, `ssn`, `sdns` (static IP, optional); `tzoff` (int32 seconds east of UTC, default 0); `appass` (AP password, optional); `dashpass` (dashboard login password, optional)
  - `lora-net` — key `aeskey` (16-byte AES key, encrypted builds only)

### Async logging

All `Serial.print` calls go through `log_async.h` (`LOG_*` macros) rather than directly
to `Serial`. A ring buffer (depth 24, 96 bytes per line) is drained by a low-priority
Core 0 task. This keeps TDMA timing on Core 1 free from blocking UART writes. Log lines
are also forwarded to WebSocket clients as `log` events.

### Encryption (PKT_ENCRYPTION builds)

`pkt_crypto.h` wraps all radio TX/RX. When `PKT_ENCRYPTION` is defined:

- Cipher: AES-128 CTR (mbedTLS).
- Nonce per packet: `[sfCount_lo | sfCount_hi | slotId | dir | 0x00×12]`; direction
  discriminators (`PKT_DIR_UL`, `PKT_DIR_DL`, `PKT_DIR_BEACON`, `PKT_DIR_JOIN_RQ`,
  `PKT_DIR_JOIN_AK`) match the relevant packet type IDs.
- BeaconPacket special case: bytes 0–1 (`sfCount`) are sent plaintext so nodes can build
  the nonce before decrypting the rest.
- Key storage: gateway auto-generates a key on first boot; provisioned to node hardware
  via PN532 I2C RFID (MIFARE Classic sector 1, block 4, default Key A 0xFF×6).

## Embedded Constraints

- This is a TDMA/real-time embedded project: avoid blocking I/O (e.g., Serial.print) in hot loops
- Prefer async/non-blocking patterns for logging and I/O
- Always analyze timing impact when modifying code in time-critical paths
