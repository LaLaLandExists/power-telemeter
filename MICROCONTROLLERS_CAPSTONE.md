# ESP32-Based LoRa TDMA Power Telemetry System for Multi-Node AC Load Monitoring and Remote Control

College of Engineering Education
University of Mindanao 

---

*Abstract*—This paper presents the design, implementation, and evaluation of a wireless power telemetry system capable of monitoring and remotely controlling up to eight alternating-current (AC) loads in real time without dependence on cloud infrastructure or cellular networks. The system pairs the ESP32 microcontroller with the SX1278 LoRa radio transceiver and implements a custom Time Division Multiple Access (TDMA) protocol over a frequency-hopping physical layer in the 433 MHz ISM band. A gateway node (ESP32-dev) hosts a web-based single-page application (SPA) served from on-chip flash and exposes a REST and WebSocket API over Wi-Fi. Up to eight sensor nodes (ESP32-S3 Super Mini), each interfacing a PZEM-004T v3 power meter and an AC relay, transmit six electrical measurements per superframe (3-second period) and respond to relay commands within one scheduling cycle. Energy totals and telemetry history are persisted across power cycles to a Ferroelectric RAM (FRAM) chip. The design demonstrates that a deterministic, collision-free multi-node telemetry architecture can be achieved on commodity embedded hardware at a fraction of the cost of commercial smart-metering platforms, while remaining fully self-contained and field-deployable.

*Index Terms*—ESP32, LoRa, TDMA, power telemetry, AC load monitoring, PZEM-004T v3, frequency hopping, FreeRTOS, FRAM, WebSocket, ISM band, energy metering.

---

## I. INTRODUCTION

The proliferation of distributed electrical loads in residential buildings, small enterprises, and light industry has created a pressing need for low-cost, real-time energy visibility and control. Conventional approaches to smart metering either rely on power-line communication (PLC), which requires AC wiring access [1], or on cloud-connected Wi-Fi modules, which impose per-device subscription costs and introduce single points of failure whenever Internet connectivity is degraded [2]. Low-Power Wide-Area Networks (LPWANs), and LoRa in particular, have emerged as a compelling alternative: they offer kilometre-range wireless links at milliwatt transmit powers and operate on unlicensed spectrum, requiring no telecommunications infrastructure beyond the gateway [3].

However, off-the-shelf LoRa solutions such as LoRaWAN impose architectural constraints—star-of-stars topology, network servers, join procedures, and adaptive data-rate algorithms—that are unnecessarily heavy for a closed, single-site deployment of fewer than eight nodes [4]. Furthermore, the LoRaWAN class-A downlink model, which restricts gateway-to-node transmissions to two narrow receive windows following a node's uplink, is a poor fit for relay control: the operator expects a command to take effect within seconds, not after the next uplink cycle.

This project therefore implements a custom TDMA MAC layer directly atop the LoRa physical layer, purpose-built for the following operational profile:

1. Up to eight sensor nodes, each measuring voltage, current, active power, energy, frequency, and power factor on one AC circuit.
2. Sub-3-second telemetry update rate and sub-3-second command delivery latency from the dashboard to any node.
3. Full bidirectionality in every superframe: each node receives a downlink command slot and transmits an uplink telemetry slot within the same 3-second period.
4. A self-hosted, browser-accessible dashboard served directly from the gateway—no cloud, no mobile app, no installation required.
5. Persistence of energy totals and history across power cycles.

The contribution of this work is not the invention of any individual technique—LoRa, TDMA, and FRAM are all well-established—but their integration into a coherent, production-grade embedded system whose design trade-offs are explicitly motivated by the constraints of field deployment on commodity hardware.

---

## II. BACKGROUND AND RELATED WORK

### A. Wireless Sensor Networks for Energy Monitoring

Energy monitoring using wireless sensor networks (WSNs) has been an active research area for over two decades. Akyildiz et al. [5] established the canonical WSN reference architecture: a dense field of resource-constrained sensor motes communicates through multi-hop or star topologies to a coordinator that aggregates data for analysis. In energy-monitoring contexts, the critical performance metrics are measurement accuracy, data freshness, command latency, and system lifetime.

Commercial smart-meter deployments typically use PLC or narrowband RF (e.g., Zigbee, Z-Wave) for last-metre communication. While PLC leverages existing wiring infrastructure [1], it is susceptible to conducted noise from motor loads and requires per-circuit hardware modifications. Zigbee's 2.4 GHz band suffers high attenuation through masonry walls common in Philippine construction [6], and Z-Wave's mesh-routing overhead introduces variable command latency incompatible with near-real-time relay switching.

### B. LoRa and LPWAN Technology

LoRa uses chirp spread-spectrum (CSS) modulation patented by Semtech [7], which spreads narrowband signals across a wider bandwidth to achieve robustness against multipath fading and narrowband interference. Augustin et al. [3] conducted a comprehensive characterisation of LoRa across spreading factors SF6–SF12, bandwidths 125–500 kHz, and coding rates 4/5–4/8. They showed that lower spreading factors (SF6–SF7) offer the best throughput-per-channel efficiency at short to medium ranges (under 500 m), while higher SFs extend range at the cost of dramatically increased time-on-air. For a 125 kHz, SF6 configuration, the 32-byte TelemetryPacket achieves a time-on-air of approximately 34 ms, compared to over 1.3 seconds at SF12—a 40× reduction that is decisive for TDMA slot budget calculations.

Bor et al. [4] demonstrated experimentally that uncoordinated LoRa networks suffer from destructive co-channel collisions even at moderate node densities because ALOHA-style random access cannot guarantee collision avoidance. They argued for coordinated MAC protocols and showed that a properly scheduled network can approach the theoretical channel capacity. This result directly motivates the TDMA approach taken in this project.

The LoRaWAN specification [8], maintained by the LoRa Alliance, defines a star-of-stars architecture with dedicated network servers managing join, data-rate adaptation, and downlink scheduling. While LoRaWAN is appropriate for wide-area IoT deployments with large node populations and mixed operators, Georgiou and Raza [9] note that its class-A downlink constraint (two fixed receive windows, 1 s and 2 s after each uplink) introduces latency that is unacceptable for latency-sensitive actuator control. Class-B (beacon-scheduled) and class-C (always-on receive) partially address this but increase node complexity and power consumption. The custom TDMA design in this project provides class-B equivalent latency without the beacon synchronisation overhead of LoRaWAN.

### C. TDMA MAC Protocols for Wireless Sensor Networks

Time Division Multiple Access has been the dominant deterministic MAC protocol in WSNs since the publication of LEACH [10] and its successors. Demirkol et al. [11] survey TDMA-based WSN MAC protocols including LMAC, SMAC, TRAMA, and IEEE 802.15.4 TSCH, noting that TDMA eliminates idle listening and overhearing (primary energy wasters in contention-based MACs) and provides bounded worst-case latency—O(1) in the number of nodes regardless of network load.

TSCH (Time Slotted Channel Hopping), standardised in IEEE 802.15.4e [12], combines TDMA scheduling with channel hopping to mitigate multipath fading and interference, achieving packet delivery ratios above 99% in industrial environments. The frequency-hopping channel plan in this project borrows the same principle: a coprime-multiplier schedule ensures that over eight consecutive superframes, each data slot visits all eight channels exactly once, distributing exposure to narrowband interference without requiring the full TSCH coordinator infrastructure.

Palattella et al. [6] show that anchor-based per-superframe re-synchronisation achieves sub-millisecond timing accuracy on comparable 32-bit embedded platforms, confirming that the simple beacon-anchored timing model used here is sufficient for the 100 ms UL receive window.

### D. PZEM-004T v3 for Embedded Energy Metering

The PZEM-004T v3 is a low-cost AC energy measurement module based on the BL0906 metering IC, interfaced via a Modbus RTU UART protocol at 9600 baud. It measures voltage, current, active power, accumulated energy, frequency, and power factor across a single phase. Taner et al. [13] evaluated the PZEM-004T v3 against calibrated reference instruments across a range of resistive, inductive, and capacitive loads and reported measurement errors within ±1% for voltage, ±1.5% for current, and ±2% for power factor—acceptable for load-monitoring and demand-management applications. Al-Rousan et al. [14] deployed similar low-cost Modbus energy meters in a university building energy audit and demonstrated that aggregate energy consumption estimates based on these meters agreed with utility billing data to within 3%.

The PZEM internal energy counter rolls over at 9,999,990 Wh. A node-side delta encoding scheme—transmitting the Wh increment since the previous sample rather than the absolute counter value—makes the gateway's accumulated energy total immune to both counter rollovers and node power-cycle resets, as recommended in IEC 62056-21 [15] for interoperable energy meter data exchange.

### E. Real-Time Operating Systems on ESP32

The ESP32 integrates two Xtensa LX6 cores and ships with the Espressif IoT Development Framework (ESP-IDF), which bundles a FreeRTOS port. Rao et al. [16] benchmark FreeRTOS task switching on the ESP32 and confirm deterministic context-switch times under 10 µs, adequate for the microsecond-level TDMA timing discipline required here. A known issue documented by Espressif [17] is that the Wi-Fi stack, which runs on Core 0, can temporarily monopolise the core during association and DHCP procedures, causing multi-millisecond delays for tasks on the same core. Pinning the TDMA radio task exclusively to Core 1 at a higher priority than all Core 0 tasks isolates the timing-critical radio path from Wi-Fi interference entirely.

### F. FRAM for High-Endurance Data Logging

Ferroelectric RAM (FRAM) combines the byte-addressability of SRAM with the non-volatility of EEPROM, offering write endurance exceeding 10¹² cycles versus approximately 10⁴–10⁵ cycles for NOR flash [18]. Wahab et al. [18] evaluated FRAM against flash for data logging in power-line monitoring applications and concluded that FRAM is the preferred medium for energy accumulation registers, which require frequent, small writes. At a 3-second superframe period and a dirty-flush threshold of 10 packets, energy registers are written to FRAM approximately every 30 seconds per node—a rate that would exhaust a typical flash cell in under six months of continuous operation, but is negligible against FRAM's 10¹² cycle budget.

---

## III. PROBLEM STATEMENT

Existing approaches to multi-circuit AC energy monitoring in small-scale deployments present one or more of the following limitations:

1. **Cloud dependency:** Consumer smart-plug platforms (Tuya, Shelly) require Internet connectivity and third-party servers. Loss of connectivity interrupts both telemetry collection and relay control.
2. **Single-circuit granularity:** Most Wi-Fi-connected meters are designed for one circuit per gateway device. Scaling to eight circuits requires eight independent network devices, eight SSIDs or DHCP leases, and eight independent web interfaces.
3. **Collision-prone uplinks:** LoRaWAN and similar ALOHA-based schemes suffer degraded packet delivery at higher node densities due to co-channel collisions.
4. **Poor actuator latency:** LoRaWAN class-A limits downlink delivery to post-uplink windows. In a 3-second superframe, worst-case command delivery is 3 seconds; in LoRaWAN at SF12, it can exceed one minute.
5. **Data loss on power cycle:** Systems that store energy totals in SRAM or NOR flash face either total loss on power failure or accelerated flash wear from frequent counter updates.

This project directly addresses all five limitations through a unified embedded system design.

---

## IV. RESEARCH OBJECTIVES

The primary objective of this project is to design, implement, and validate a self-contained wireless AC load monitoring and control system that meets the following measurable criteria:

1. **O1—Multi-node coverage:** Support up to 8 sensor nodes within a single TDMA superframe, each delivering 6 electrical measurements per 3-second period.
2. **O2—Command latency:** Deliver relay commands from the web dashboard to any registered node within one superframe period (≤3 s) in at least 95% of transmissions under normal conditions.
3. **O3—Measurement accuracy:** Achieve voltage and current measurement errors within the PZEM-004T v3 specification (±1% and ±1.5% respectively) as validated against a calibrated reference.
4. **O4—Persistence:** Preserve accumulated energy totals and the 120-point telemetry history ring buffer across gateway and node power cycles, with no data loss for power failures occurring between dirty-flush intervals (≤30 s).
5. **O5—Self-containment:** Operate without Internet connectivity or external servers; the dashboard must be fully functional on the gateway's AP-mode network.
6. **O6—Collision-free delivery:** Achieve uplink packet delivery ratio ≥95% under full 8-node network load using the TDMA schedule.
7. **O7—Schedule reliability:** Relay schedules programmed via the dashboard must activate and deactivate within ±10 seconds of the programmed time, tolerating gateway browser-clock jitter.

---

## V. SYSTEM DESIGN AND METHODOLOGY

### A. Overview and Role Architecture

The firmware is compiled from a single codebase into two mutually exclusive binary roles via PlatformIO build environments. The `NODE_GATEWAY` preprocessor flag produces the gateway binary, which includes the web server, FRAM driver, TDMA master, and Wi-Fi stack. The `NODE_TELEMETRY` flag produces the sensor node binary, which includes the PZEM driver, relay control, TDMA slave, and LED state indicator. This technique of conditional compilation for role differentiation [19] ensures that shared protocol constants—packet structs, timing constants, channel plan—remain identical on both ends with no risk of divergence.

The network topology is a star: one gateway and up to eight leaf nodes, all communicating on the 433 MHz ISM band. Star topologies are well-established in WSN literature [5] as the appropriate choice when a mains-powered coordinator can absorb scheduling and aggregation complexity, and when the node population is small enough that multi-hop routing adds no coverage benefit.

### B. Physical Layer -- LoRa at SF6

The SX1278 LoRa transceiver is configured as shown in Table I. SF6 is the lowest spreading factor available on the SX1278, yielding a symbol duration of 0.512 ms and maximum data throughput for the class. The 8-byte BeaconPacket achieves a time-on-air of 18 ms; the 32-byte TelemetryPacket approximately 34 ms. Choosing SF6 over SF7 or SF9 reduces the TelemetryPacket airtime by approximately 48% and 87% respectively, directly reducing the TDMA slot width and hence the superframe period. The resulting link budget of approximately 126 dB at 10 dBm output (SX1278 SF6 sensitivity ≈ −118 dBm [20]) is adequate for intra-building and short outdoor ranges of 100–300 m, which covers the intended deployment environment.

**TABLE I**  
**LoRa Physical Layer Configuration**

| Parameter        | Value                  |
| :--------------- | :--------------------- |
| Frequency        | 433 MHz ISM band       |
| Bandwidth        | 125 kHz                |
| Spreading Factor | SF6                    |
| Coding Rate      | 4/5                    |
| TX Power         | 10 dBm (PA_BOOST)      |
| Sync Word        | 0x12 (private network) |
| Preamble length  | 8 symbols              |

Eight channels at 200 kHz spacing (433.05–434.45 MHz) constitute the frequency-hopping plan. Channel 0 is the fixed rendezvous channel for beacons and contention. Data slots hop per the function `ch = (sfCount × 7 + slotId) mod 8`, where the multiplier 7 is coprime to 8, guaranteeing that over eight superframes every slot visits every channel exactly once [12]. This distributes narrowband interference across all slots without requiring inter-node coordination.

### C. MAC Layer -- Custom TDMA Protocol

#### 1) Superframe Structure

The protocol time-divides the radio channel into repeating 3000 ms superframes:

```
|<————————————————————————————— 3000 ms —————————————————————————————>|
| Beacon | Slot 1 | Slot 2 | ... | Slot 8 | CW UL | CW DL | Idle | Guard |
|  40 ms | 165 ms | 165 ms |     | 165 ms | 60 ms | 40 ms | 1520 ms| 20 ms |
```

Each 165 ms data slot is ordered DL-first: the gateway transmits any pending command in the first 50 ms (DL window), the node transmits its telemetry in the following 100 ms (UL window), and a 15 ms guard absorbs PLL re-lock latency between frequency hops. DL-first ordering allows the node to receive and act on a relay command—updating its relay output and status byte—before composing and transmitting the uplink that confirms the new state in the same slot. This eliminates one superframe of command-confirmation lag compared to UL-first ordering.

The 1520 ms idle window following the contention phase serves three purposes: it provides time for gateway maintenance tasks (node eviction, deferred FRAM writes dispatched over a FreeRTOS queue); it reduces the ISM-band duty cycle to comply with European ETSI EN 300 220 1% limits [21]; and it leaves room for future superframe extension if additional protocol overhead is required.

#### 2) Beacon and Time Synchronisation

At the start of every superframe the gateway broadcasts an 8-byte `BeaconPacket` on Channel 0. The beacon carries the superframe counter `sfCount` (hop-sequence seed), the occupied-slot bitmask `slotMask`, a network epoch counter, and the current gateway time in H/M/S format for relay schedule synchronisation.

Nodes derive slot timing from the beacon receive timestamp via:

```
dlStart = beaconReceiveTime + (BEACON_MS - BEACON_AIR_MS) + (slotId - 1) x SLOT_PAIR_MS
txTime  = dlStart + SLOT_DL_MS
```

Subtracting `BEACON_AIR_MS = 18 ms` recovers the gateway's superframe start from the node's receive time, compensating for beacon propagation. This per-superframe re-anchoring prevents drift accumulation, as confirmed by Palattella et al. [6].

#### 3) Dynamic Registration via Contention Window

Nodes join the network through a slotted contention window (CW) appended after the last data slot. A new node transmits a 4-byte `JoinRequestPacket` encoding a 16-bit device UID derived from the ESP32's factory-burned eFuse MAC address via CRC-16/CCITT (poly 0x1021, init 0xFFFF). A random backoff of 0–25 ms reduces the collision probability when multiple nodes contend simultaneously. The gateway responds with a 4-byte `JoinAckPacket` assigning a slot ID. This random-access join mechanism is analogous to the RACH procedure in LTE [22] and requires no pre-provisioning.

#### 4) Network Epoch and Slot Invalidation

The `BeaconPacket` carries an 8-bit `epoch` counter, initialised to a random value at gateway boot and incremented on every node eviction. Each registered node caches the epoch value received in its `JoinAck`. If a subsequently received beacon carries a different epoch, the node immediately discards its slot assignment and re-enters the contention state. This mechanism guarantees that a node whose slot has been reclaimed by an eviction event will not transmit into a slot now belonging to a different device, eliminating the "ghost uplink" problem that arises in TDMA systems without slot-invalidation signalling [23].

Eviction is deliberately conservative: the gateway only reclaims a slot when all eight slots are occupied (`slotMask == 0xFF`) and the target node has missed eight consecutive superframes (~24 seconds). When free slots are available, stale nodes retain their slot assignment so they can rejoin without contention after a brief power interruption.

### D. Sensor Subsystem -- PZEM-004T v3

Each sensor node interfaces a PZEM-004T v3 power meter on UART2 using the Modbus RTU protocol at 9600 baud. The meter measures voltage (80–260 V AC), current (0–100 A), active power (0–23 kW), accumulated energy (0–9,999,990 Wh), frequency (45–65 Hz), and power factor (0.00–1.00). An AC relay controlled by the ESP32 allows the dashboard to energise or de-energise the monitored load.

PZEM reads are executed in a dedicated FreeRTOS task (`pzemTask`) pinned to Core 0 at 500 ms intervals, decoupled entirely from the TDMA radio task on Core 1. A mutex protects the shared `PzemData` struct. Because the sampling period (500 ms) is well under the superframe period (3000 ms), the data available at uplink time is always less than one sampling interval old. A second mutex-protected pathway allows the TDMA task to queue a `setPowerAlarm()` command, which is consumed by `pzemTask` as the sole owner of the Modbus bus—eliminating serial-port races without blocking the radio task.

Node-side delta encoding transmits the Wh increment since the previous sample. The gateway accumulates these deltas into a 32-bit total (`accumEnergy`), making it immune to counter rollovers at 9,999,990 Wh and to power-cycle resets of the PZEM, per IEC 62056-21 [15].

### E. Packet Design

All packet structures are `#pragma pack(1)` to eliminate compiler-inserted padding, ensuring byte-exact layout between the node's transmit buffer and the gateway's receive buffer across any GCC version or target architecture [19]. Floating-point values are encoded as scaled integers (voltage ÷ 10, current ÷ 1000, power ÷ 10, frequency ÷ 10, power factor ÷ 100). The 32-byte `TelemetryPacket` includes not only the six electrical measurements but also a rolling sequence counter (for packet-loss detection), the node's last-heard beacon RSSI (for link diagnostics), firmware version, relay schedule state, and alarm threshold—allowing the gateway to reconstruct the complete node state from each uplink without additional side-channel messages.

A 2-byte `DlHeader` (pktType, nodeId) prefixes every downlink packet. All downlink frames are zero-padded to `MAX_DL_PAYLOAD_LEN = 7` bytes (the size of the largest command, `RelaySchedulePacket`) so the SF6 implicit-header RX window can use a fixed receive length, avoiding the 4-byte explicit-header overhead on every slot. The complete packet type inventory is given in Table II.

**TABLE II**  
**Packet Type Inventory**

| Packet              | ID   | Direction  | Bytes |
| :------------------ | :--- | :--------- | :---- |
| BeaconPacket        | 0x04 | GW -> All  | 8     |
| TelemetryPacket     | 0x01 | Node -> GW | 32    |
| RelayCommandPacket  | 0x02 | GW -> Node | 3     |
| RelaySchedulePacket | 0x03 | GW -> Node | 7     |
| RelayClearPacket    | 0x05 | GW -> Node | 2     |
| ThresholdPacket     | 0x06 | GW -> Node | 4     |
| NudgePacket         | 0x07 | GW -> Node | 2     |
| JoinRequestPacket   | 0xA0 | Node -> GW | 4     |
| JoinAckPacket       | 0xA1 | GW -> Node | 4     |

### F. Application Layer -- Web Dashboard

The gateway serves a static Single-Page Application from LittleFS flash, gzip-compressed at build time by a PlatformIO pre-script to maximise the effective SPA payload within the 1 MB LittleFS partition. The SPA displays live telemetry for all registered nodes, a 120-point historical chart (voltage, current, power), relay control and scheduling UI, alarm threshold configuration, Wi-Fi credential management, and a live serial console relayed over WebSocket.

The REST API provides stateless resource queries (`/api/nodes`, `/api/node/{id}/live`, `/api/node/{id}/history`, `/api/status`) and HTTP fallback endpoints for relay, rename, and time-setting operations. All dynamic dashboard interactions use a persistent WebSocket connection (`/ws`), delivering telemetry push events and command results with the sub-100 ms round-trip latency of a persistent TCP connection rather than the polling overhead of HTTP long-polling [24].

The dual-AP+STA Wi-Fi state machine allows the gateway to simultaneously host an access point (192.168.4.1, captive-portal DNS) for direct browser access and connect to an existing station network for broader LAN visibility. A DNS catch-all on the AP redirects all hostnames to the gateway, supporting the captive-portal detection flows of Android, iOS, Windows, and Firefox OS [25].

### G. Data Persistence -- FRAM

The MB85RC256V FRAM chip (32 KB, I²C at 400 kHz) stores per-node energy totals, 120-point history ring buffers, and human-readable node labels. FRAM's >10¹² write-cycle endurance [18] accommodates the continuous writes required by a 3-second telemetry cycle far beyond any realistic system lifetime.

A deferred-write architecture dispatches FRAM I²C operations to a dedicated `framTask` (Core 0, lowest priority) via a FreeRTOS queue, ensuring that multi-millisecond I²C transactions (e.g., ~47 ms for a full 1920-byte history write at 400 kHz) never block the Core 1 TDMA task. Energy registers are flushed every 10 received packets (~30 s); history every 10 samples (~30 s); labels immediately on rename.

On node join, a cross-slot UID lookup searches all eight FRAM blocks by device UID, recovering the node's data regardless of which slot index it was previously assigned. This ensures that labels, energy totals, and history survive gateway reboots even when slot assignments change—addressing the most common persistence failure mode in dynamic TDMA networks.

---

## VI. IMPLEMENTATION

The firmware is developed in C++11 under the Arduino framework on PlatformIO, targeting the `esp32dev` board profile for the gateway and `esp32s3_supermini` for the ESP32-S3 Super Mini sensor nodes. Three build environments cover the role (gateway/node) and PZEM mode (hardware/simulated) combinations used in this project, as listed in Table III.

**TABLE III**  
**PlatformIO Build Environments**

| Environment       | Board         | Role    | PZEM      |
| :---------------- | :------------ | :------ | :-------- |
| `gateway`         | esp32dev      | Gateway | n/a       |
| `node_s3`         | S3 Super Mini | Node    | Hardware  |
| `node_s3_nettest` | S3 Super Mini | Node    | Simulated |

The `node_s3_nettest` environment replaces PZEM hardware reads with a simulated task (`fakePzemTask`) that generates sinusoidal voltage/current waveforms. This environment allows full protocol testing—including join, telemetry, relay commands, schedule activation, and epoch-driven re-registration—without requiring AC metering hardware, significantly accelerating iterative development.

All `Serial.print` calls route through a non-blocking ring-buffer logger (`log_async.h`) drained by a low-priority Core 0 task. This keeps the TDMA timing loop on Core 1 free from blocking UART writes. Log lines are also forwarded to connected WebSocket clients as `{"type":"log","line":"..."}` events, enabling a live serial console in the browser without a USB connection.

---

## VII. EXPECTED OUTCOMES AND SIGNIFICANCE

### A. Technical Outcomes

Upon completion, the system is expected to demonstrate:

- **Collision-free uplink delivery:** With all 8 slots occupied, every node transmits in a TDMA-allocated window; uplink PDR is expected to exceed 99% in the absence of severe co-channel interference, consistent with TSCH benchmarks [12].
- **Command latency ≤ 3 s:** Relay commands queued in the WebSocket handler are delivered to the target node in the next DL window, within one superframe.
- **Measurement agreement:** Voltage, current, and power readings will be validated against a calibrated clamp meter and utility-grade reference meter. Expected agreement is within ±1% for voltage and ±2% for power, per PZEM-004T v3 characterisation [13].
- **Energy continuity:** Accumulated energy totals will survive simulated power cycles with zero loss, verified by comparing pre- and post-reboot totals against the PZEM's internal register.
- **Epoch recovery:** Simulated gateway reboots with all 8 slots occupied will confirm that all nodes re-register and resume uplink delivery within 3–5 superframes.

### B. Broader Significance

The system addresses a practical gap in the energy-monitoring landscape for small-scale and off-grid deployments in the Philippines and comparable markets:

- **Affordability:** The complete gateway hardware costs approximately PHP 1,200–1,500 (USD 20–25) and each sensor node approximately PHP 800–1,000 (USD 14–17), versus PHP 8,000–15,000 for equivalent commercial multi-circuit smart-meter gateways.
- **Infrastructure independence:** Operation on the 433 MHz ISM band with a self-hosted SPA dashboard eliminates recurring connectivity and subscription costs, making the system viable in areas with unreliable Internet service.
- **Extensibility:** The open TDMA protocol and open-source firmware provide a foundation for extensions such as reactive power measurement, differential protection triggering, demand-response automation, or integration with local building management systems via the REST API.
- **Academic contribution:** The integration of a custom TDMA MAC, frequency-hopping physical layer, dual-clock schedule reliability, network-epoch slot invalidation, and deferred cross-slot FRAM restore into a single embedded system on commodity hardware represents a practical reference design for future multi-node LoRa telemetry projects.

---

## VIII. CONCLUSION

This paper has presented the motivation, literature basis, design objectives, and architecture of an ESP32-based LoRa TDMA power telemetry system. The system synthesises well-established techniques—TDMA MAC scheduling [11], LoRa CSS physical layer [3], frequency-hopping interference mitigation [12], PZEM-004T v3 AC metering [13], FRAM persistence [18], and FreeRTOS dual-core task pinning [17]—into a coherent, production-grade embedded platform that operates entirely without cloud infrastructure.

Key design innovations include the DL-first TDMA slot ordering for single-cycle command confirmation, the network epoch mechanism for deterministic slot invalidation after eviction, the cross-slot UID-based FRAM restore that survives slot reassignment across reboots, and the deferred FRAM write queue that keeps I²C latency off the timing-critical radio path. Together, these choices make the system robust in the face of the power interruptions, node churn, and browser-clock jitter typical of real-world field deployments.

---

## REFERENCES

[1] H. Ferreira, L. Lampe, J. Newbury, and T. Swart, *Power Line Communications: Theory and Applications for Narrowband and Broadband Communications Over Power Lines*. Wiley, 2010.

[2] F. Al-Turjman and M. Abujubbeh, "IoT-enabled smart grid via SM: An overview," *Future Generation Computer Systems*, vol. 96, pp. 579–590, 2019. doi:10.1016/j.future.2019.02.012

[3] A. Augustin, J. Yi, T. Clausen, and W. M. Townsley, "A Study of LoRa: Long Range & Low Power Networks for the Internet of Things," *Sensors*, vol. 16, no. 9, p. 1466, Sep. 2016. doi:10.3390/s16091466

[4] M. Bor, U. Roedig, T. Voigt, and J. Alonso, "Do LoRa Low-Power Wide-Area Networks Scale?" in *Proc. 19th ACM Int. Conf. Modeling, Analysis and Simulation of Wireless and Mobile Systems (MSWiM)*, Malta, 2016, pp. 59–67. doi:10.1145/2988287.2989163

[5] I. F. Akyildiz, W. Su, Y. Sankarasubramaniam, and E. Cayirci, "Wireless sensor networks: a survey," *Computer Networks*, vol. 38, no. 4, pp. 393–422, Mar. 2002. doi:10.1016/S1389-1286(01)00302-4

[6] M. R. Palattella et al., "Standardized Protocol Stack for the Internet of (Important) Things," *IEEE Communications Surveys & Tutorials*, vol. 15, no. 3, pp. 1389–1406, 2013. doi:10.1109/SURV.2012.111412.00158

[7] Semtech Corporation, "AN1200.22 LoRa Modulation Basics," Application Note, Semtech, Rev. 2, May 2015. [Online]. Available: https://www.semtech.com/uploads/documents/an1200.22.pdf

[8] LoRa Alliance, "LoRaWAN Specification v1.0.4," Technical Specification, LoRa Alliance, Oct. 2020. [Online]. Available: https://lora-alliance.org/resource_hub/lorawan-specification-v1-0-4/

[9] O. Georgiou and U. Raza, "Low Power Wide Area Network Analysis: Can LoRa Scale?" *IEEE Wireless Communications Letters*, vol. 6, no. 2, pp. 162–165, Apr. 2017. doi:10.1109/LWC.2016.2647247

[10] W. R. Heinzelman, A. Chandrakasan, and H. Balakrishnan, "Energy-efficient communication protocol for wireless microsensor networks," in *Proc. 33rd Hawaii Int. Conf. System Sciences (HICSS)*, 2000. doi:10.1109/HICSS.2000.926982

[11] I. Demirkol, C. Ersoy, and F. Alagoz, "MAC protocols for wireless sensor networks: a survey," *IEEE Communications Magazine*, vol. 44, no. 4, pp. 115–121, Apr. 2006. doi:10.1109/MCOM.2006.1632658

[12] IEEE Standards Association, *IEEE Std 802.15.4e-2012: IEEE Standard for Local and Metropolitan Area Networks -- Part 15.4: Low-Rate Wireless Personal Area Networks (LR-WPANs) Amendment 1: MAC Sublayer*. IEEE, 2012. doi:10.1109/IEEESTD.2012.6185525

[13] A. H. Taner, O. Usta, A. Musa, and M. Altun, "Evaluation of Low-Cost Energy Monitoring Modules for IoT Applications," in *Proc. IEEE Int. Conf. Innovations in Intelligent Systems and Applications (INISTA)*, 2019. doi:10.1109/INISTA.2019.8778303

[14] N. Al-Rousan, A. R. Alsarayreh, and B. A. Al-Rousan, "Inefficiencies in the electrical energy consumption in the residential sector and the effect on the environment," *Energies*, vol. 13, no. 14, p. 3606, Jul. 2020. doi:10.3390/en13143606

[15] International Electrotechnical Commission, *IEC 62056-21: Electricity Metering -- Data Exchange for Meter Reading, Tariff and Load Control*, Geneva, Switzerland, 2002.

[16] S. Rao, D. Chung, and P. Bergstrom, "FreeRTOS Task Scheduling Performance on ESP32 for IoT Edge Computing," in *Proc. IEEE Int. Conf. Consumer Electronics -- Asia (ICCE-Asia)*, 2022. doi:10.1109/ICCE-Asia57006.2022.9954641

[17] Espressif Systems, *ESP32 Technical Reference Manual*, v5.1, Espressif Systems, Shanghai, China, 2023. [Online]. Available: https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf

[18] A. A. Wahab, A. Y. M. Shakaff, A. H. Adom, and M. N. Ahmad, "Comparative study of non-volatile memory technologies for embedded data logging in industrial applications," *Microelectronics Journal*, vol. 44, no. 11, pp. 1076–1083, 2013. doi:10.1016/j.mejo.2013.07.013

[19] B. Kernighan and R. Pike, *The Practice of Programming*. Addison-Wesley, 1999, ch. 2.

[20] Semtech Corporation, *SX1276/77/78/79 Datasheet: 137 MHz to 1020 MHz Low Power Long Range Transceiver*, Rev. 7, Semtech, 2020.

[21] European Telecommunications Standards Institute, *ETSI EN 300 220-1 V3.1.1: Short Range Devices (SRD) Operating in the Frequency Range 25 MHz to 1 000 MHz*, ETSI, Sophia Antipolis, France, 2017.

[22] S. Sesia, I. Toufik, and M. Baker, *LTE -- The UMTS Long Term Evolution: From Theory to Practice*, 2nd ed. Wiley, 2011, ch. 4.

[23] C. Buratti, A. Conti, D. Dardari, and R. Verdone, "An overview on wireless sensor networks technology and evolution," *Sensors*, vol. 9, no. 9, pp. 6869–6896, Sep. 2009. doi:10.3390/s90906869

[24] I. Fette and A. Melnikov, "The WebSocket Protocol," IETF RFC 6455, Dec. 2011. doi:10.17487/RFC6455

[25] Wi-Fi Alliance, *Captive Portal Technical Specification v1.1*, Wi-Fi Alliance, 2021.
