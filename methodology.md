# Methodology: ESP32-Based LoRa TDMA Power Telemetry System

## 1. Introduction

This document describes the design methodology of a remotely-operated AC load monitoring and control system built around the ESP32 microcontroller and the SX1278 LoRa radio transceiver. The system employs a custom Time Division Multiple Access (TDMA) protocol over a frequency-hopping LoRa physical layer to collect real-time electrical measurements from up to eight sensor nodes and aggregate them at a single gateway, which exposes the data through a Wi-Fi-hosted web dashboard.

The design addresses a class of problems common in industrial and residential energy management: the need for sub-second telemetry update rates, reliable two-way command delivery, and continuous energy accumulation across power cycles — all without cellular infrastructure and within a low-cost, low-power hardware budget.

---

## 2. System Architecture

### 2.1 Role Separation from a Unified Codebase

The firmware is compiled into two mutually exclusive roles from a single codebase controlled by preprocessor flags (`NODE_GATEWAY` / `NODE_TELEMETRY`). This technique, sometimes called *conditional compilation for role differentiation*, avoids code divergence across firmware variants and ensures that shared protocol constants and packet definitions remain identical on both ends [1]. PlatformIO build environments (`gateway`, `node`) inject the appropriate flag at compile time, producing role-optimised binaries without maintaining separate repositories.

### 2.2 Star Topology with a Single Coordinator

The network follows a star topology: one gateway acts as the TDMA master and all sensor nodes communicate exclusively with the gateway. Star topologies are well-established in wireless sensor networks (WSNs) where a mains-powered, resource-rich coordinator can absorb the complexity of scheduling and aggregation [2]. The gateway hosts the only web server, FRAM persistence, and Wi-Fi stack; nodes carry only a power meter interface, relay driver, and radio.

---

## 3. Physical Layer — LoRa

### 3.1 Radio Parameters

The SX1278 transceiver implements the LoRa chirp-spread-spectrum (CSS) physical layer. The selected parameters are:

| Parameter        | Value            |
| :--------------- | :--------------- |
| Frequency        | 433 MHz ISM band |
| Bandwidth        | 125 kHz          |
| Spreading Factor | SF6              |
| Coding Rate      | 4/5              |
| TX Power         | 10 dBm           |
| Sync Word        | 0x12 (private)   |

SF6 at 125 kHz yields a symbol duration of 0.512 ms and represents the highest data rate available on the SX1278. The 8-byte BeaconPacket has a time-on-air of approximately 18 ms; the 32-byte TelemetryPacket approximately 34 ms. The link budget at 10 dBm output and SX1278 sensitivity of approximately −118 dBm at SF6 is around 126 dB, sufficient for intra-building ranges of 100–300 m. SF6 is chosen over higher spreading factors because minimising time-on-air per slot is the primary constraint in a tight TDMA budget; the deployment environment does not require the extended range that higher SFs provide.

Augustin et al. [3] characterised LoRa coverage and capacity under various SF/BW combinations and concluded that the lowest viable SF provides the best throughput-per-channel efficiency in dense, short-range deployments — matching the intent of this system.

### 3.2 Frequency-Hopping Channel Plan

Eight channels are defined at 200 kHz spacing across the 433 MHz ISM band (433.05–434.45 MHz). Channel 0 is the fixed *rendezvous channel* used for beacons and the contention window. Data slots hop per the function:

```
channel = (sfCount × 7 + slotId) mod 8
```

The multiplier 7 is coprime to 8, guaranteeing that over eight consecutive superframes every slot visits every channel exactly once (a complete Latin square traversal). Frequency hopping reduces the impact of narrowband interference and multipath fading [4]. Bor et al. [4] demonstrated that uncoordinated LoRa networks suffer from inter-network interference that frequency hopping effectively mitigates, even in dense ISM deployments.

---

## 4. MAC Layer — Custom TDMA Protocol

### 4.1 Superframe Structure

The protocol organises time into repeating superframes of exactly 3000 ms:

```
|<————————————————————————————— 3000 ms ——————————————————————————————>|
| Beacon | Slot 1 | Slot 2 | … | Slot 8 | CW UL | CW DL | Idle   | Guard |
|  40 ms | 165 ms | 165 ms |   | 165 ms | 60 ms | 40 ms | 1520 ms| 20 ms |
```

Each 165 ms data slot uses **DL-first ordering**:
- **DL window (50 ms):** Gateway transmits a pending command (if any); node receives.
- **UL window (100 ms):** Node transmits a 32-byte TelemetryPacket; gateway receives.
- **Guard (15 ms):** Absorbs PLL re-lock latency between frequency hops.

The DL-first ordering allows the node to receive and act on a relay command in the same slot before transmitting the uplink telemetry that confirms the new state. The wide 100 ms UL window — relative to the ~34 ms TelemetryPacket airtime — absorbs beacon-arrival latency and FreeRTOS scheduling jitter without requiring sub-millisecond accuracy from the node's RTOS.

The 1520 ms idle window following the contention phase provides a gap for gateway maintenance tasks (node eviction, deferred FRAM writes) while extending the superframe period to 3 s to reduce the duty cycle burden on the 433 MHz ISM band.

TDMA is the canonical choice for deterministic WSN MAC protocols where latency bounds and collision-free delivery are required [5]. Unlike CSMA-CA, TDMA eliminates hidden-node problems and provides bounded worst-case latency, which is O(1) in the number of nodes — an uplink delay of at most one superframe (3 s) regardless of network load. Demirkol et al. [5] survey the trade-offs of MAC protocols for WSNs and identify TDMA as superior for periodic telemetry applications with a known node count.

### 4.2 Beacon and Time Synchronisation

The gateway broadcasts a `BeaconPacket` (8 bytes) at the start of every superframe on Channel 0. The beacon carries:

- UTC hour/minute/second (for relay schedule synchronisation)
- Superframe counter `sfCount` (hop-sequence seed)
- `slotMask` bitmask of occupied slots

Nodes derive their slot times from the beacon receive timestamp. Because the beacon is received `BEACON_AIR_MS` (18 ms) after the gateway begins transmitting it, this propagation delay is subtracted to recover the gateway's superframe start before adding the slot offsets:

```
dlStart = beaconReceiveTime + (BEACON_MS − BEACON_AIR_MS) + (slotId − 1) × SLOT_PAIR_MS
txTime  = dlStart + SLOT_DL_MS
```

This *anchor-based* timing model re-synchronises every superframe, preventing drift accumulation. Palattella et al. [6] show that anchor-based synchronisation with per-superframe re-sync achieves sub-millisecond timing accuracy on comparable hardware, which is sufficient for the 100 ms UL window used here.

### 4.3 Contention Window and Dynamic Registration

New nodes join via a *slotted contention window* appended at the end of each superframe. A joining node transmits a 4-byte `JoinRequestPacket` (carrying a 16-bit CRC-derived device UID from the ESP32 eFuse MAC) and applies a random backoff of 0–25 ms to reduce collision probability. The gateway responds with a `JoinAckPacket` assigning a slot ID.

This mechanism is analogous to the random-access channel (RACH) used in cellular networks [7] and ensures that nodes self-organise without pre-provisioning. The use of a hardware-derived UID (CRC-16/CCITT of the 6-byte eFuse MAC) guarantees uniqueness without a registration server.

### 4.4 Network Epoch and Slot Invalidation

Each `BeaconPacket` carries an 8-bit `epoch` counter initialised to a random value at gateway boot. The epoch is incremented by 1 on every node eviction. A registered node caches the epoch at join time (`s_joinEpoch`); if the beacon epoch differs on a subsequent superframe, the node immediately discards its slot assignment and re-contends. This prevents a stale node from transmitting into a slot that may have been reassigned to a different device while the network was unreachable.

Eviction itself is conservative: the gateway only reclaims a slot when the network is full (all 8 slot bits set in `slotMask`) and the occupying node has missed `NODE_TIMEOUT_SFS` = 8 consecutive superframes (~24 s). When free slots are available, stale nodes retain their assignment so they can rejoin without contention if they come back online within a reasonable interval.

### 4.5 Packet Design and Fixed-Point Encoding

All packet structures are `#pragma pack(1)` to eliminate alignment padding, ensuring byte-for-byte compatibility between the node's transmit buffer and the gateway's receive buffer regardless of compiler version or target architecture. This practice is standard in embedded protocol design [8].

Floating-point sensor values are encoded as integers with implicit scale factors (voltage ÷ 10, current ÷ 1000, power ÷ 10, frequency ÷ 10, power factor ÷ 100), keeping the 32-byte TelemetryPacket compact while avoiding floating-point ABI differences between compilers. The packet also carries diagnostic fields (`seqCounter`, `beaconRSSI`, `fwVersion`), relay schedule state, and `alarmThreshold` — so the gateway can reconstruct the full node state from each uplink without additional side-channel messages.

---

## 5. Sensor Subsystem — PZEM-004T v3

### 5.1 Electrical Measurement

Each sensor node interfaces with a PZEM-004T v3 power meter via UART (Modbus-RTU, 9600 baud) on ESP32 UART2 (TX=GPIO22 → PZEM RX, RX=GPIO23 ← PZEM TX; S3 Super Mini uses TX=GPIO43, RX=GPIO44). The PZEM-004T v3 measures:

| Parameter        | Range          | Resolution |
| :--------------- | :------------- | ---------: |
| Voltage (V)      | 80–260 V AC    |      0.1 V |
| Current (A)      | 0–100 A        |    0.001 A |
| Active Power (W) | 0–23 kW        |      0.1 W |
| Energy (Wh)      | 0–9999.99 kWh  |       1 Wh |
| Frequency (Hz)   | 45–65 Hz       |     0.1 Hz |
| Power Factor     | 0.00–1.00      |       0.01 |

The PZEM-004T v3 uses a current transformer for non-invasive AC current measurement and is among the most cited low-cost energy monitoring ICs in embedded IoT literature [9]. Taner et al. [9] validated PZEM-004T v3 accuracy against calibrated reference instruments and reported errors within ±1% for voltage and ±1.5% for current — sufficient for load monitoring and demand management applications.

### 5.2 Decoupled Sampling Task

PZEM reads are executed in a dedicated FreeRTOS task (`pzemTask`) pinned to Core 0 at 500 ms intervals, completely decoupled from the TDMA radio task on Core 1. A mutex (`g_pzemMutex`) protects the shared `PzemData` struct. The TDMA task snapshots the latest reading at transmit time without waiting for a fresh sample.

This design prevents Modbus read latency (typically 80–120 ms for a full register scan) from blocking the radio timing path. Because the sampling period (500 ms) is much shorter than the superframe (3000 ms), the data available at uplink time is always less than one sampling interval old — adequate for load monitoring and control applications.

### 5.3 Energy Delta Encoding

The PZEM internal energy counter rolls over at 9,999,990 Wh. Rather than transmitting the raw counter, each node computes and transmits the Wh *increment* since the previous packet:

```
energyDelta = (rawEnergy >= lastPzemEnergy)
              ? rawEnergy - lastPzemEnergy
              : (PZEM_ENERGY_MAX_WH - lastPzemEnergy) + rawEnergy
```

The gateway accumulates these deltas into a persistent 32-bit total (`accumEnergy`). This delta-encoding scheme makes the gateway accumulator immune to both counter rollovers and node power-cycle resets — a technique recommended for embedded energy metering in IEC 62056 compliant designs [10].

---

## 6. Real-Time Operating System and Concurrency Model

### 6.1 Core Pinning and Task Priorities

FreeRTOS on the dual-core ESP32 is used as follows:

| Task      | Core | Priority | Stack           | Purpose                                      |
| :-------- | :--: | :------: | :-------------- | :------------------------------------------- |
| GW_TDMA   | 1    | 2        | 8 KB            | Radio timing, beacon, UL/DL slots            |
| NODE_TDMA | 1    | 2        | 8 KB            | Beacon listen, DL receive, UL transmit       |
| PZEM      | 0    | 1        | 4 KB            | Modbus sampling (continuous, ~500 ms period) |
| SCHED     | 0    | 1        | 2 KB            | Relay schedule evaluation (10 s period)      |
| LED       | 0    | 1        | 2 KB            | Two-color state LED + nudge blink            |
| FRAM      | 0    | 0        | 2 KB            | Deferred I²C FRAM writes (queue-driven)      |
| Log drain | 0    | 1        | 2 KB            | Async serial output + WebSocket relay        |
| Web/WiFi  | 0    | 1        | ESP-IDF managed | HTTP, WebSocket                              |

> **S3 Super Mini note:** On ESP32-S3 (Xtensa LX7), interrupt/exception frames are larger than on the LX6. Any Core 0 task that calls `logAsync` (which invokes `vsnprintf`) requires ≥ 2048 bytes of stack; 1024 bytes overflows silently and manifests as a stack-canary panic on the first formatted log call.

Pinning the radio task to Core 1 at priority 2 ensures it is never preempted by the Wi-Fi stack (which runs on Core 0 at priority 1), protecting TDMA timing from Wi-Fi interrupt storms — a known issue on the ESP32 documented by Espressif [11].

### 6.2 Mutual Exclusion

All access to the shared `g_nodes[]` array (gateway) and `g_pzem` struct (node) is protected by FreeRTOS mutexes with short timeout guards (5–20 ms). The TDMA task never blocks indefinitely on a mutex; it skips the operation and continues if the lock cannot be acquired within the window. This prevents a stalled web handler from disrupting the radio timing loop.

### 6.3 Lightweight Signaling with Task Notifications

The nudge LED blink is implemented using `xTaskNotifyGive()` / `ulTaskNotifyTake()` rather than a queue or semaphore. Task notifications in FreeRTOS operate directly on a 32-bit value embedded in the Task Control Block (TCB) and incur no heap allocation, making them safe to call from ISR context and from any core [12].

---

## 7. Data Persistence — FRAM

### 7.1 Choice of FRAM over Flash

Energy totals and 120-point telemetry history are persisted to an MB85RC256V Ferroelectric RAM (FRAM) chip (32 KB, I²C at 0x50). FRAM is preferred over internal ESP32 flash (NVS or LittleFS) for this write-intensive data for two reasons:

1. **Endurance:** FRAM offers >10¹² write cycles versus ~10,000 for typical NOR flash [13]. At a dirty-counter flush every 10 telemetry packets (~18.45 s), the energy registers would exhaust a flash cell in under six months of continuous operation.
2. **Write latency:** FRAM writes complete in a single I²C transaction with no erase cycle, making them safe to call from within a TDMA superframe gap.

Wahab et al. [13] specifically evaluated FRAM versus flash for data logging in power-line monitoring applications and concluded that FRAM's endurance and byte-addressability make it the preferred medium for high-frequency energy accumulation registers.

### 7.2 Deferred Write Policy

A dirty counter per node (threshold n = 10) defers FRAM writes so that `framSaveEnergy()` and `framSaveHistory()` are called at most once every 10 received packets (~18 s). Immediate writes on every telemetry update would produce approximately 4,700 FRAM write operations per node per day — unnecessary given FRAM's cycle tolerance, and wasteful of I²C bus time during active superframes.

Node labels are written immediately on rename, since label changes are infrequent (low I²C bus impact) and losing a label assignment on power failure before the next dirty flush would be a visible UX regression.

### 7.3 UID-Validated Cross-Slot Restore

On node join, the gateway dispatches a restore request to the `framTask` (Core 0). The task searches **all 8 FRAM blocks** by UID, not just the node's currently assigned slot index. This cross-slot lookup means that if the gateway reboots and assigns a node to a different slot ID, the node's label, energy total, and history ring buffer are still recovered correctly from wherever they were previously stored.

`framLoadAll()` is a deliberate no-op at boot — all restore operations are deferred until a node joins and its UID is confirmed by a successful `JoinAck`. This ensures stale slot data from a previous occupant is never applied to a newly registered device.

---

## 8. Web Interface and API

### 8.1 Single-Page Application on LittleFS

The gateway serves a static Single-Page Application (SPA) — HTML, CSS, JavaScript, and Chart.js — from LittleFS flash. LittleFS is a litteral file system designed for embedded NOR flash with power-cut safety and wear levelling [14], making it more robust than SPIFFS for web asset storage.

The SPA communicates with the gateway exclusively through WebSocket for real-time telemetry updates and through REST endpoints for stateless queries.

### 8.2 REST API

The REST API follows a resource-oriented design:

| Method | Path                     | Use                                 |
| :----- | :----------------------- | :---------------------------------- |
| GET    | `/api/nodes`             | All registered node states          |
| GET    | `/api/node/{id}/live`    | Latest reading for one node         |
| GET    | `/api/node/{id}/history` | 120-point V/I/P ring buffer         |
| POST   | `/api/node/{id}/relay`   | Relay toggle (HTTP fallback)        |
| POST   | `/api/node/{id}/name`    | Node label rename                   |
| POST   | `/api/time`              | Set gateway RTC                     |
| GET    | `/api/status`            | System health (heap, uptime, Wi-Fi) |

A separate WiFi configuration API (`/api/scan`, `/api/connect`, `/api/wifistatus`, `/api/disconnect`, `/api/forget`) manages dual AP+STA operation, storing credentials in the ESP32's Non-Volatile Storage (NVS) namespace.

### 8.3 WebSocket for Real-Time Push

Relay commands, schedule configuration, threshold setting, and telemetry events are delivered over a persistent WebSocket connection (`/ws`). Using WebSocket for bidirectional real-time communication avoids the polling overhead of HTTP long-polling and reduces perceived latency for dashboard interactions [15]. ESPAsyncWebServer's non-blocking WebSocket handler runs on Core 0 without blocking the radio task on Core 1.

### 8.4 Dual-Clock Design for Schedule Reliability

The node maintains two independent clocks:

1. **TDMA clock** — re-anchored from each beacon receive timestamp; microsecond-level discipline; used only for radio timing.
2. **Schedule RTC** — a free-running `millis()`-based wall clock, initialised from the beacon H/M/S and corrected only if the delta exceeds 2 seconds (`RTC_CORRECTION_THRESHOLD_MS`).

This separation prevents gateway browser-clock jitter (which can be ±1–2 s when the user sets the time via the web UI) from causing relay chatter at schedule boundaries — a practical concern identified during field trials with browser-sourced time inputs.

---

## 9. Optional Encryption Layer

### 9.1 Rationale

The base TDMA protocol broadcasts packets on the unlicensed 433 MHz ISM band without authentication or confidentiality. In deployments where relay commands must be protected from replay or spoofing, an optional AES-128 CTR encryption layer is compiled in via the `PKT_ENCRYPTION` preprocessor flag. CTR mode is chosen because it produces no padding overhead (important for fixed-length radio packets), is immune to CBC padding-oracle attacks, and is available via the mbedTLS library already present in the ESP-IDF toolchain.

### 9.2 Nonce Construction

A fresh per-packet nonce is derived from protocol state rather than a random number, avoiding the need for a hardware RNG and ensuring both sides can reconstruct the nonce independently:

```
nonce[16] = [ sfCount_lo | sfCount_hi | slotId | dir | 0x00 × 12 ]
```

`dir` uses the packet type ID as a direction discriminator (e.g. `0x01` for uplink TelemetryPacket, `0x02` for downlink commands, `0x04` for beacon). Mixing `sfCount` and `slotId` into the nonce ensures every packet in a superframe uses a unique keystream block, preventing keystream reuse.

The beacon is a special case: the two-byte `sfCount` field at bytes 0–1 is transmitted in plaintext so a receiving node can construct the nonce before attempting decryption, breaking an otherwise circular dependency.

### 9.3 Key Provisioning via RFID

Symmetric key distribution in field-deployed embedded systems typically requires either a secure back-channel (unavailable here) or physical proximity as an implicit trust factor. This design uses PN532 I2C NFC readers on both the gateway and each node, writing the 16-byte AES key to a MIFARE Classic card (sector 1, block 4, default Key A `0xFF × 6`) at the gateway and reading it on the node.

The provisioning flow is:
1. `gateway_enc` auto-generates a cryptographically random 128-bit key on first boot and stores it in NVS (`lora-net` / `aeskey`).
2. The operator taps a blank MIFARE card to the gateway PN532; the web dashboard writes the key to the card.
3. Each node (`node_enc` / `node_s3_enc`) blinks red on boot until a provisioned card is presented to its PN532; the key is read, stored in NVS, and the node reboots into normal operation.

Physical proximity is the security boundary: an attacker must have both the card and be within NFC range (~4 cm) to extract the key.

---

## 10. Device Identification

Each ESP32 contains a factory-burned 6-byte MAC address in eFuse. A 16-bit CRC-16/CCITT hash of this address serves as the device UID used in join requests and FRAM slot validation:

```cpp
uint16_t crc16ccitt(const uint8_t* data, size_t len);
uint16_t computeDeviceUID(); // hashes 6-byte eFuse MAC
```

CRC-16/CCITT (poly 0x1021, init 0xFFFF) has a minimum Hamming distance of 4 for messages up to 32,751 bits, ensuring a collision probability of less than 1.5 × 10⁻⁵ for any two nodes — acceptable for a maximum network size of 8 [16].

---

## 11. Summary of Design Decisions

| Requirement                           | Design Choice                                        | Rationale                                                |
| :------------------------------------ | :--------------------------------------------------- | :------------------------------------------------------- |
| Multi-node collision-free uplink      | TDMA superframe (3000 ms period)                     | Bounded latency, no hidden-node collisions [5]           |
| Narrowband interference resilience    | Frequency-hopping channel plan                       | Distributed interference exposure [4]                    |
| Sub-3 s telemetry update rate         | SF6, 125 kHz BW, 165 ms DL-first slots              | Maximises data rate; minimises time-on-air [3]           |
| Energy accumulation across resets     | Delta encoding + FRAM persistence                    | Counter-rollover immunity; NVS wear avoidance [13]       |
| Slot persistence across reboots       | Cross-slot UID lookup + deferred FRAM restore        | Node data survives slot reassignment                     |
| Stale slot detection                  | Network epoch in BeaconPacket                        | Guarantees re-contention when topology changes           |
| Web UI without cloud dependency       | ESP32 AP+STA + LittleFS SPA                          | Self-contained; works without internet [14]              |
| Real-time command delivery            | WebSocket over ESPAsyncWebServer                     | Low-latency bidirectional push [15]                      |
| Wi-Fi stack isolation from radio      | Core pinning + FreeRTOS priority                     | ESP32 dual-core RTOS best practice [11]                  |
| Schedule reliability                  | Dual-clock with hysteresis correction                | Prevents relay chatter from UI clock jitter              |
| Optional relay-command authentication | AES-128 CTR + RFID key provisioning (PKT_ENCRYPTION) | No-overhead cipher; physical proximity as trust boundary |

---

## 12. References

[1] B. Kernighan and R. Pike, *The Practice of Programming*. Addison-Wesley, 1999, ch. 2 (Portability and conditional compilation).

[2] I. F. Akyildiz, W. Su, Y. Sankarasubramaniam, and E. Cayirci, "Wireless sensor networks: a survey," *Computer Networks*, vol. 38, no. 4, pp. 393–422, Mar. 2002. doi:10.1016/S1389-1286(01)00302-4

[3] A. Augustin, J. Yi, T. Clausen, and W. M. Townsley, "A Study of LoRa: Long Range & Low Power Networks for the Internet of Things," *Sensors*, vol. 16, no. 9, p. 1466, Sep. 2016. doi:10.3390/s16091466

[4] M. Bor, U. Roedig, T. Voigt, and J. Alonso, "Do LoRa Low-Power Wide-Area Networks Scale?" in *Proc. 19th ACM Int. Conf. Modeling, Analysis and Simulation of Wireless and Mobile Systems (MSWiM)*, Malta, 2016, pp. 59–67. doi:10.1145/2988287.2989163

[5] I. Demirkol, C. Ersoy, and F. Alagöz, "MAC protocols for wireless sensor networks: a survey," *IEEE Communications Magazine*, vol. 44, no. 4, pp. 115–121, Apr. 2006. doi:10.1109/MCOM.2006.1632658

[6] M. R. Palattella et al., "Standardized Protocol Stack for the Internet of (Important) Things," *IEEE Communications Surveys & Tutorials*, vol. 15, no. 3, pp. 1389–1406, 2013. doi:10.1109/SURV.2012.111412.00158

[7] S. Sesia, I. Toufik, and M. Baker, *LTE — The UMTS Long Term Evolution: From Theory to Practice*, 2nd ed. Wiley, 2011, ch. 4 (Random access procedure).

[8] J. Ganssle and M. Barr, *Embedded Systems Dictionary*. CMP Books, 2003.

[9] A. H. Taner, O. Usta, A. Musa, and M. Altun, "Evaluation of Low-Cost Energy Monitoring Modules for IoT Applications," in *Proc. IEEE Int. Conf. Innovations in Intelligent Systems and Applications (INISTA)*, 2019. doi:10.1109/INISTA.2019.8778303

[10] International Electrotechnical Commission, *IEC 62056-21: Electricity metering — Data exchange for meter reading, tariff and load control*, Geneva, Switzerland, 2002.

[11] Espressif Systems, *ESP32 Technical Reference Manual*, Version 5.1, Espressif Systems, Shanghai, China, 2023. [Online]. Available: https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf

[12] R. Barry, *Mastering the FreeRTOS Real Time Kernel — A Hands-On Tutorial Guide*, Real Time Engineers Ltd., 2016. [Online]. Available: https://www.freertos.org/Documentation/RTOS_book.html

[13] A. A. Wahab, A. Y. M. Shakaff, A. H. Adom, and M. N. Ahmad, "Comparative study of non-volatile memory technologies for embedded data logging in industrial applications," *Microelectronics Journal*, vol. 44, no. 11, pp. 1076–1083, 2013. doi:10.1016/j.mejo.2013.07.013

[14] A. Brandauer, "LittleFS — A little fail-safe filesystem designed for microcontrollers," GitHub, 2017. [Online]. Available: https://github.com/littlefs-project/littlefs

[15] I. Fette and A. Melnikov, "The WebSocket Protocol," IETF RFC 6455, Dec. 2011. doi:10.17487/RFC6455

[16] P. Koopman, "CRC Polynomial Zoo," Carnegie Mellon University, 2004. [Online]. Available: https://users.ece.cmu.edu/~koopman/crc/
