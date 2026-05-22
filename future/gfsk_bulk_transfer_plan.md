# GFSK Bulk Transfer Mode — Protocol Extension Plan

## On-Demand High-Throughput Data Transfer via LoRa/GFSK Hybrid Superframe

---

## 1. Motivation

The existing TDMA protocol delivers deterministic, collision-free telemetry at 32 bytes per node per 3-second superframe using LoRa SF6 modulation. The downlink window supports at most 7 bytes per superframe (zero-padded for SF6 implicit header). This is optimal for periodic sensor readings and short control commands (relay toggle, schedule set, nudge), but it is insufficient for any operation that needs to move more than a few bytes between the gateway and a node.

Several planned and anticipated subsystems require payloads in the range of 28 bytes to 4 kB — calibration coefficient blocks, configuration parameter sets, classifier models, diagnostic dumps, and training-data uploads. Delivering even a modest 500-byte payload through the existing 7-byte DL window would require 72 superframes (216 seconds) of fragmented commands, which is impractical and error-prone.

This document specifies a general-purpose bulk data transport that operates within the existing superframe structure. The transport layer is application-agnostic: it delivers an opaque byte buffer of a declared length between the gateway and a specific node, reliably, using a GFSK burst during the idle window. What those bytes represent — a classifier model, a calibration table, a diagnostic snapshot — is the caller's concern, not the transport's.

### 1.1 Transport Contract

The bulk transfer subsystem provides a single service primitive:

```
BulkTransfer(nodeId, direction, typeTag, data[], length) → {success, data[]}
```

Where:

- `nodeId` identifies the target node (1–8).
- `direction` is DL (gateway → node) or UL (node → gateway).
- `typeTag` is an opaque 8-bit application identifier. The transport layer stores and forwards it but never interprets it. The application layers on each side use it to dispatch the received payload to the correct handler.
- `data[]` is the payload buffer (max 4096 bytes).
- `length` is the payload size in bytes.
- The return value indicates success/failure and, for UL transfers, provides the received data.

The transport guarantees: either all bytes are delivered intact (verified by per-fragment CRC-16 and end-to-end CRC-32), or the transfer fails explicitly with no partial delivery visible to the application layer.

---

## 2. Design Principles

1. **Zero disruption to regular telemetry.** All 8 data slots, the contention window, and the beacon operate identically regardless of whether a bulk transfer is active. Non-participating nodes never observe or interact with the bulk phase.

2. **Single modulation transition per superframe.** The radio switches from LoRa to GFSK exactly once (at the start of the bulk phase) and back exactly once (at the end). No interleaved mode switching within the data-slot region.

3. **On-demand activation.** The GFSK phase only occurs when a bulk transfer has been granted and acknowledged. In normal operation (no pending bulk), the idle window is entirely untouched — the superframe behaves exactly as today.

4. **Beacon format preservation.** Bulk transfer signalling uses a new DL command packet type within the existing slot structure. The 8-byte `BeaconPacket` is not modified.

5. **Single node per superframe.** Each bulk session targets one node. The gateway serialises multi-node operations across consecutive superframes, completing all 8 nodes in 24 seconds.

6. **Application-layer independence.** The transport moves bytes. It does not know or care what those bytes represent. Application-specific dispatch (writing a model to flash, storing calibration coefficients to NVS, etc.) is handled by a callback registered above the transport layer.

---

## 3. Transport Capacity

### 3.1 Size Envelope

The transport supports payloads from 1 byte to 4096 bytes per session. This range is driven by the idle-window airtime budget at the chosen GFSK rate, not by any specific application's needs:

| Payload Size | Fragments (200 B each) | Airtime (100 kbps, no retries) |
|---|---|---|
| 28 B | 1 | ~22 ms |
| 200 B | 1 | ~22 ms |
| 500 B | 3 | ~66 ms |
| 2048 B | 11 | ~242 ms |
| 4096 B | 21 | ~462 ms |

All sizes fit comfortably within the ~1476 ms of usable bulk window, leaving ample margin for retransmissions and gateway maintenance.

### 3.2 Example Consumers (Out of Scope for This Document)

The following are anticipated application-layer users of the bulk transport. Their specifications are maintained in separate documents. They are listed here solely to validate the size envelope:

- Classifier model deployment (~500 B DL) — see `load_classification_algorithm.md`
- Classifier threshold tuning (~28 B DL)
- ADC calibration coefficients (~24 B DL)
- Compound configuration blocks (50–200 B DL)
- Training-data collection batches (2–4 kB UL) — see `load_classification_algorithm.md`
- Diagnostic snapshots (1–2 kB UL)

---

## 4. Physical Layer — GFSK Configuration

### 4.1 Rate Selection: 100 kbps

The GFSK bit rate is chosen to balance throughput against link margin within the target deployment environment (≤150 m² household, ≤20 m through 1–2 concrete walls).

**Indoor path loss at 433 MHz, 20 m, through 2 walls:**

$$PL = 26.7 + 10 \times 2.5 \times \log_{10}(20) + 8 + 6 \approx 73 \text{ dB}$$

Received signal at 10 dBm TX: approximately −63 dBm.

| GFSK Rate | SX1278 Sensitivity (Band 2, 0.1% BER) | Margin | 4 kB Airtime |
|---|---|---|---|
| 38.4 kbps | −108 dBm | 45 dB | ~853 ms |
| **100 kbps** | **~−104 dBm** | **41 dB** | **~328 ms** |
| 250 kbps | −95 dBm | 32 dB | ~131 ms |

100 kbps provides 41 dB of margin — ample for body shadowing (3–5 dB), transient fading, and the occasional cross-room deployment — while completing a worst-case 4 kB transfer in 328 ms, leaving over 1100 ms of the idle window for gateway maintenance.

### 4.2 GFSK Radio Parameters

| Parameter | Value | Rationale |
|---|---|---|
| Bit rate | 100 kbps | See Section 4.1 |
| Frequency deviation ($F_{DEV}$) | 50 kHz | Modulation index $\beta = 2 \times 50 / 100 = 1.0$; within demodulator optimum $0.5 \leq \beta \leq 10$ |
| RX bandwidth (single-side) | 125 kHz | $\geq F_{DEV} + BR/2 = 100$ kHz; next available SX1278 RxBw setting |
| Gaussian filter BT | 0.5 | Reduces spectral splatter; standard GFSK shaping |
| Preamble | 4 bytes (0xAA pattern) | Sufficient for bit synchroniser lock at 100 kbps |
| Sync word | 3 bytes (0xB5, 0x4A, 0x7E) | Distinct from LoRa sync (0x12); low autocorrelation |
| Packet format | Variable length, with length byte | Accommodates fragments of different sizes |
| CRC | CRC-16 (CCITT) | Hardware CRC in SX1278 FSK packet engine |
| Encoding | NRZ (no Manchester/whitening) | Maximum throughput; link margin is sufficient |

### 4.3 Mode Switching Mechanics

The SX1278's `LongRangeMode` bit (RegOpMode bit 7) can only be changed in Sleep mode. The switching sequence:

**LoRa → GFSK:**

```
1. radio.sleep()                         // RegOpMode = 0x00; immediate
2. SPI write RegOpMode = 0x01            // LongRangeMode=0, Mode=Standby
3. Crystal oscillator wakeup             // TS_OSC = 250 µs
4. Configure FSK registers               // ~15 SPI writes: BitRate, Fdev, RxBw,
                                          // SyncConfig, PacketConfig1/2, PreambleLen,
                                          // PayloadLength, FifoThreshold
5. Set frequency                         // TS_HOP ≤ 50 µs for 200 kHz step
6. Enter TX or RX                        // TS_FS = 60 µs (PLL lock)
```

**Total: ~0.5 ms.** Conservative budget: 1 ms.

**GFSK → LoRa:**

Same sequence in reverse, restoring LoRa register state. If RadioLib's `begin()` is used instead of cached register writes, the cost increases to ~2 ms due to full re-initialisation. Total budget: 2 ms.

**Round trip:** ~3 ms maximum. Against a 1520 ms idle window, this is 0.2% overhead.

---

## 5. MAC Layer — Bulk Transfer Protocol

### 5.1 New Packet Types

#### 5.1.1 BulkGrantPacket (LoRa DL, existing slot)

Delivered via the target node's regular LoRa DL window using the existing implicit-header mechanism. This is the only bulk-related packet that travels over the LoRa channel.

```cpp
#define PKT_BULK_GRANT   0x08   // GW → Node, 6 bytes

#pragma pack(push, 1)
struct BulkGrantPacket : DlHeader {  // pktType (0x08) + nodeId = 2 B
    uint8_t  bulkDir;       // 0 = DL (GW → Node), 1 = UL (Node → GW)
    uint8_t  bulkType;      // Application-defined type tag (opaque to transport)
    uint16_t totalLen;      // Total payload bytes, LE, max 4096
};
#pragma pack(pop)
static_assert(sizeof(BulkGrantPacket) == 6, "must fit MAX_DL_PAYLOAD_LEN=7");
```

Size: 6 bytes, zero-padded to `MAX_DL_PAYLOAD_LEN = 7`. Fits the existing DL window without modification.

The `bulkType` field is carried end-to-end but never interpreted by the transport. The application layer on each side uses it to route the received payload to the correct handler. Values are assigned by the application layer; the transport reserves no values.

#### 5.1.2 BulkFragment (GFSK, bulk phase)

Used during the GFSK bulk phase for data transport in both directions.

```cpp
#define PKT_BULK_FRAG    0xB0   // Data fragment

#pragma pack(push, 1)
struct BulkFragHeader {
    uint8_t  pktType;       // 0xB0
    uint8_t  seqNum;        // Fragment sequence number (0–255)
    uint8_t  fragCount;     // Total fragment count in this transfer
    uint8_t  fragLen;       // Payload bytes in this fragment (≤ BULK_FRAG_PAYLOAD)
};
#pragma pack(pop)

#define BULK_FRAG_PAYLOAD    200   // Bytes of application data per fragment
#define BULK_FRAG_TOTAL      (sizeof(BulkFragHeader) + BULK_FRAG_PAYLOAD)  // 204
```

A 200-byte fragment payload was chosen because 4 (header) + 200 (payload) + 2 (CRC) = 206 bytes fits comfortably within the SX1278's 256-byte FSK packet-mode limit, leaving room for preamble and sync word overhead without requiring on-the-fly FIFO refill.

#### 5.1.3 BulkAck (GFSK, bulk phase)

Sent by the receiver to acknowledge or reject each fragment.

```cpp
#define PKT_BULK_ACK     0xB1   // Fragment ACK
#define PKT_BULK_NACK    0xB2   // Fragment NACK (request retransmit)
#define PKT_BULK_ABORT   0xB3   // Abort session (unrecoverable error)

#pragma pack(push, 1)
struct BulkAckPacket {
    uint8_t  pktType;       // 0xB1 or 0xB2 or 0xB3
    uint8_t  ackSeq;        // Sequence number being acknowledged
};
#pragma pack(pop)
```

### 5.2 Superframe Structure with Active Bulk Phase

When a bulk transfer is active, the idle window is partitioned:

```
|<——————————————————————— 3000 ms ————————————————————————>|
| Beacon | S1..S8 | CW UL | CW DL | Maint | GFSK Bulk | Restore | Guard |
|  40 ms | 1320ms | 60 ms | 40 ms | 20 ms | ≤1380 ms  |  ~2 ms  | 20 ms |
|<——————— LoRa SF6 (unchanged) ——————>|<GFSK>|<——LoRa——>|
                                             ^           ^
                                         single       single
                                      LoRa→GFSK     GFSK→LoRa
                                       transition    transition
```

When no bulk transfer is pending, the entire idle window (1520 ms) remains in LoRa mode, identical to the current protocol.

#### 5.2.1 Timing Constants

```cpp
// Bulk transfer timing (milliseconds)
#define BULK_MAINT_MS          20    // Gateway maintenance before mode switch
#define BULK_SWITCH_MS          2    // Conservative LoRa→GFSK switch budget
#define BULK_RESTORE_MS         2    // GFSK→LoRa restoration budget
#define BULK_MAX_WINDOW_MS   (IDLE_MS - BULK_MAINT_MS - BULK_SWITCH_MS \
                              - BULK_RESTORE_MS - END_GUARD_MS)
                              // 1520 - 20 - 2 - 2 - 20 = 1476 ms

// Fragment timing
#define BULK_FRAG_AIR_MS       17    // 206 bytes at 100 kbps ≈ 16.5 ms
#define BULK_ACK_AIR_MS         1    // 2 bytes at 100 kbps ≈ 0.16 ms
#define BULK_TURNAROUND_MS      2    // TX/RX switch + processing
#define BULK_FRAG_CYCLE_MS   (BULK_FRAG_AIR_MS + BULK_ACK_AIR_MS \
                              + BULK_TURNAROUND_MS * 2)
                              // 17 + 1 + 4 = 22 ms per fragment round-trip
#define BULK_ACK_TIMEOUT_MS    50    // Receiver ACK deadline
#define BULK_MAX_RETRIES        3    // Per-fragment retry limit
#define BULK_GRANT_RETRIES      3    // Grant-level retry (across superframes)
#define BULK_MAX_PAYLOAD     4096    // Hard ceiling on payload size
```

---

## 6. Protocol State Machine

### 6.1 Gateway Side

```
                           ┌──────────────┐
                           │  IDLE        │  No bulk pending
                           │  (normal SF) │◄───────────────────────────┐
                           └──────┬───────┘                           │
                                  │                                   │
                    API call: bulkEnqueue(                             │
                      nodeId, dir, typeTag,                            │
                      data, len)                                      │
                                  │                                   │
                                  ▼                                   │
                           ┌──────────────┐                           │
                           │  GRANT_QUEUED│  BulkGrantPacket          │
                           │              │  loaded into target       │
                           │              │  node's DL queue          │
                           └──────┬───────┘                           │
                                  │                                   │
                    Target node's DL window:                          │
                    BulkGrantPacket transmitted                        │
                                  │                                   │
                                  ▼                                   │
                           ┌──────────────┐                           │
                           │  GRANT_SENT  │  Awaiting BULK_READY      │
                           │              │  flag in node's UL        │
                           └──────┬───────┘                           │
                                  │                                   │
                         ┌────────┴────────┐                          │
                    UL has                UL missing                   │
                  BULK_READY              or no flag                   │
                         │                    │                       │
                         ▼                    ▼                       │
                  ┌──────────────┐     ┌──────────────┐              │
                  │  BULK_ACTIVE │     │  RETRY       │──(retry next │
                  │              │     │              │    SF)────────┘
                  │ Switch GFSK  │     └──────────────┘
                  │ Run fragments│               │ (max retries)
                  │ Switch LoRa  │               ▼
                  └──────┬───────┘        ┌──────────────┐
                         │                │  FAILED      │
                    Transfer              │  Report to   │
                    result                │  caller      │
                         │                └──────┬───────┘
                         ▼                       │
                  ┌──────────────┐               │
                  │  COMPLETE    │               │
                  │  Deliver buf │               │
                  │  to caller   │               │
                  └──────┬───────┘               │
                         │                       │
                         └───────────────────────┘
                                    │
                                    ▼
                              Back to IDLE
```

### 6.2 Node Side

```
                           ┌──────────────┐
                           │  NORMAL      │  Regular TDMA operation
                           │              │◄──────────────────────────┐
                           └──────┬───────┘                          │
                                  │                                  │
                    DL window: BulkGrantPacket                       │
                    received and validated                            │
                                  │                                  │
                                  ▼                                  │
                           ┌──────────────┐                          │
                           │  GRANT_RCVD  │  Set BULK_READY flag     │
                           │              │  in next TelemetryPacket │
                           │              │  Prepare RX/TX buffer    │
                           └──────┬───────┘                          │
                                  │                                  │
                    After contention window:                         │
                    wait for bulk phase offset                       │
                                  │                                  │
                                  ▼                                  │
                           ┌──────────────┐                          │
                           │  BULK_ACTIVE │                          │
                           │              │                          │
                           │ Switch GFSK  │                          │
                           │ RX/TX frags  │                          │
                           │ Switch LoRa  │                          │
                           └──────┬───────┘                          │
                                  │                                  │
                         ┌────────┴────────┐                         │
                    Transfer            Timeout /                    │
                    complete            abort received               │
                         │                    │                      │
                         ▼                    │                      │
                  ┌──────────────┐            │                      │
                  │  DELIVER     │            │                      │
                  │  Invoke app  │            │                      │
                  │  callback    │            │                      │
                  │  with (type, │            │                      │
                  │   buf, len)  │            │                      │
                  └──────┬───────┘            │                      │
                         │                    │                      │
                         └────────────────────┘                      │
                                    │                                │
                                    └────────────────────────────────┘
                                         Resume beacon listen
```

### 6.3 Application-Layer Interface

The transport delivers received data to the application layer via a registered callback. The transport does not know what the callback does with the data:

```cpp
// Callback type: invoked by the transport after a successful DL bulk receive.
// typeTag: the bulkType value from the BulkGrantPacket (opaque to transport).
// data:    pointer to the reassembled payload buffer.
// len:     payload length in bytes.
// Return:  true if the application accepted the payload, false to report error.
typedef bool (*BulkRxCallback)(uint8_t typeTag, const uint8_t* data, uint16_t len);

// Registered once during node initialisation:
void bulkRegisterRxCallback(BulkRxCallback cb);
```

On the gateway side, the equivalent interface is:

```cpp
// Enqueue a DL bulk transfer. Returns false if a session is already active.
bool bulkEnqueue(uint8_t slotId, uint8_t typeTag,
                 const uint8_t* data, uint16_t len);

// Enqueue a UL bulk request (gateway wants data from node).
// The node's application layer must have data ready for the given typeTag.
bool bulkRequestUpload(uint8_t slotId, uint8_t typeTag, uint16_t expectedLen);

// Check session status (polled by web server or called from WebSocket handler).
BulkSessionState bulkGetStatus();
```

The web API calls `bulkEnqueue()` or `bulkRequestUpload()`. The TDMA task drives the state machine. The web server reads completion status via `bulkGetStatus()` and pushes it to the dashboard via WebSocket. None of these functions know what the payload contains.

### 6.4 Superframe Timeline (Concrete Example)

**Scenario:** Gateway pushes a 500-byte opaque payload to Node 3 (Slot 3), typeTag=0x01.

```
t=0 ms       Gateway TX beacon (LoRa, Ch 0)
             All nodes receive beacon, synchronise

t=40 ms      Data slots begin

t=370 ms     Slot 3 DL window opens
             Gateway TX: BulkGrantPacket {
               pktType=0x08, nodeId=3,
               bulkDir=0 (DL), bulkType=0x01,
               totalLen=500
             }
             (zero-padded to 7 bytes, encrypted, LoRa SF6)

t=420 ms     Slot 3 UL window opens
             Node 3 TX: TelemetryPacket with BULK_READY flag set
             (statusFlags bit 7 = 1)

t=1460 ms    All data slots + contention window complete

t=1460 ms    Gateway runs maintenance (eviction, FRAM drain)
             Gateway checks: did Slot 3 UL have BULK_READY? → YES

t=1480 ms    ═══ PHASE TRANSITION: LoRa → GFSK ═══
             Both gateway and Node 3 execute:
               radio.sleep()
               configure GFSK 100 kbps / Fdev=50kHz
               radio.setFrequency(433.050 MHz)

t=1482 ms    GFSK bulk transfer begins
             Gateway TX fragment 0: seq=0, fragLen=200, payload[0..199]
t=1499 ms    Node 3 RX complete, CRC OK
             Node 3 TX ACK: {0xB1, ackSeq=0}
t=1503 ms    Gateway RX ACK

             Gateway TX fragment 1: seq=1, fragLen=200, payload[200..399]
t=1520 ms    Node 3 RX complete → ACK
t=1524 ms    Gateway RX ACK

             Gateway TX fragment 2: seq=2, fragLen=100, payload[400..499]
             (last fragment, fragLen=100)
t=1541 ms    Node 3 RX complete → ACK
t=1545 ms    Gateway RX ACK, transfer complete

t=1545 ms    ═══ PHASE TRANSITION: GFSK → LoRa ═══
             Both sides restore LoRa SF6 configuration

t=1548 ms    Back in LoRa mode
             Node 3: invokes BulkRxCallback(0x01, buf, 500)
                      resumes beacon listen
             Gateway: sets session state = COMPLETE
                      notifies web dashboard via WebSocket

             ──── remaining ~1432 ms of idle window: unused ────

t=2980 ms    END_GUARD begins
t=3000 ms    Next beacon (normal superframe)
```

---

## 7. Encryption

### 7.1 Nonce Construction for Bulk Fragments

The existing AES-128 CTR nonce scheme uses `sfCount`, `slotId`, and `direction` to guarantee unique keystream blocks. Bulk fragments extend this with the fragment sequence number:

```
Standard TDMA nonce (16 bytes):
  [ sfCount_lo | sfCount_hi | slotId | dir | 0x00 × 12 ]

Bulk fragment nonce (16 bytes):
  [ sfCount_lo | sfCount_hi | slotId | dir | seqNum | 0x00 × 11 ]
```

New direction constants:

```cpp
#define PKT_DIR_BULK_DL  0x03   // Gateway → Node bulk fragment
#define PKT_DIR_BULK_UL  0x04   // Node → Gateway bulk fragment
```

The `seqNum` in byte 4 ensures each fragment within a transfer uses a unique keystream block. The `sfCount` ensures that a retried transfer in a different superframe produces different keystreams even for the same sequence number. ACK packets (2 bytes) are not encrypted — they carry no sensitive data, and their brevity provides no useful ciphertext for an attacker.

### 7.2 Integrity Verification

Each GFSK fragment carries a hardware CRC-16 computed by the SX1278 packet engine. After decryption, the application layer verifies the `BulkFragHeader` fields (pktType, seqNum range, fragLen ≤ `BULK_FRAG_PAYLOAD`). If the decrypted header is malformed, the fragment is NACKed.

For the complete transfer, the sender appends a CRC-32 over the entire original payload as the last 4 bytes of the last fragment's data region. The receiver computes CRC-32 over the reassembled payload (excluding the trailing 4 bytes) and compares. This detects reassembly errors that per-fragment CRC would miss (e.g., a duplicated fragment replacing a valid one). The `totalLen` in the `BulkGrantPacket` refers to the application payload size; the transport adds the 4-byte end-to-end CRC internally, so the actual bytes transmitted are `totalLen + 4` (split across fragments).

---

## 8. Fragment Protocol

### 8.1 Stop-and-Wait

The fragment protocol uses simple stop-and-wait: the sender transmits one fragment, waits for an ACK or NACK, then proceeds. This is chosen over sliding-window for three reasons:

1. The round-trip time is sub-millisecond at indoor range. The wasted idle time per fragment (waiting for ACK) is negligible compared to the on-air time.

2. The maximum fragment count is 21 (for 4 kB + CRC-32). Sliding-window overhead (sequence-space management, out-of-order reassembly, selective retransmission) is not justified for 21 fragments.

3. Stop-and-wait is trivial to implement in C++11 without dynamic memory allocation — a single `for` loop with a timeout.

### 8.2 Sender Logic (Pseudocode)

```cpp
bool runBulkSend(const uint8_t* data, uint16_t totalLen,
                 uint16_t sfCount, uint8_t slotId, uint8_t dir,
                 uint32_t bulkPhaseStart)
{
    // Append end-to-end CRC-32 to a working copy
    uint16_t wireLen = totalLen + 4;
    // (Caller provides buffer with 4 bytes of headroom beyond totalLen)
    uint32_t crc = crc32(data, totalLen);
    memcpy((uint8_t*)data + totalLen, &crc, 4);

    uint8_t fragCount = (wireLen + BULK_FRAG_PAYLOAD - 1) / BULK_FRAG_PAYLOAD;
    uint8_t txBuf[BULK_FRAG_TOTAL];
    uint8_t rxBuf[4];

    for (uint8_t seq = 0; seq < fragCount; seq++) {
        // Overrun protection
        uint32_t elapsed = millis() - bulkPhaseStart;
        if (elapsed + BULK_FRAG_CYCLE_MS + BULK_RESTORE_MS > BULK_MAX_WINDOW_MS) {
            return false;  // Not enough time — abort cleanly
        }

        uint16_t offset = (uint16_t)seq * BULK_FRAG_PAYLOAD;
        uint8_t  len    = (wireLen - offset > BULK_FRAG_PAYLOAD)
                          ? BULK_FRAG_PAYLOAD
                          : (uint8_t)(wireLen - offset);

        // Build fragment
        BulkFragHeader* hdr = (BulkFragHeader*)txBuf;
        hdr->pktType   = PKT_BULK_FRAG;
        hdr->seqNum    = seq;
        hdr->fragCount = fragCount;
        hdr->fragLen   = len;
        memcpy(txBuf + sizeof(BulkFragHeader), data + offset, len);

        // Encrypt (header + payload)
        uint8_t totalFragLen = sizeof(BulkFragHeader) + len;
        pktEncryptBulk(txBuf, totalFragLen, sfCount, slotId, dir, seq);

        // Transmit with retry loop
        bool acked = false;
        for (uint8_t retry = 0; retry < BULK_MAX_RETRIES; retry++) {
            radio.transmit(txBuf, totalFragLen);

            radio.startReceive();
            int16_t rxLen = rxWindowMs(rxBuf, sizeof(rxBuf), BULK_ACK_TIMEOUT_MS);
            if (rxLen == sizeof(BulkAckPacket)) {
                BulkAckPacket* ack = (BulkAckPacket*)rxBuf;
                if (ack->pktType == PKT_BULK_ACK && ack->ackSeq == seq) {
                    acked = true;
                    break;
                }
                if (ack->pktType == PKT_BULK_ABORT) {
                    return false;
                }
            }
        }
        if (!acked) return false;
    }
    return true;
}
```

### 8.3 Receiver Logic (Pseudocode)

```cpp
bool runBulkReceive(uint8_t* buf, uint16_t expectedLen,
                    uint16_t sfCount, uint8_t slotId, uint8_t dir)
{
    uint16_t wireLen = expectedLen + 4;  // includes trailing CRC-32
    uint8_t expectedFrags = (wireLen + BULK_FRAG_PAYLOAD - 1) / BULK_FRAG_PAYLOAD;
    uint8_t rxBuf[BULK_FRAG_TOTAL];
    BulkAckPacket ack;
    uint8_t nextSeq = 0;
    uint8_t timeoutCount = 0;

    while (nextSeq < expectedFrags) {
        radio.startReceive();
        int16_t rxLen = rxWindowMs(rxBuf, sizeof(rxBuf), BULK_ACK_TIMEOUT_MS * 2);

        if (rxLen < (int16_t)sizeof(BulkFragHeader)) {
            if (++timeoutCount > BULK_MAX_RETRIES) return false;
            continue;
        }

        pktDecryptBulk(rxBuf, (uint16_t)rxLen, sfCount, slotId, dir, nextSeq);

        BulkFragHeader* hdr = (BulkFragHeader*)rxBuf;
        if (hdr->pktType != PKT_BULK_FRAG || hdr->seqNum != nextSeq) {
            ack.pktType = PKT_BULK_NACK;
            ack.ackSeq  = nextSeq;
            radio.transmit((uint8_t*)&ack, sizeof(ack));
            continue;
        }

        uint16_t offset = (uint16_t)nextSeq * BULK_FRAG_PAYLOAD;
        memcpy(buf + offset, rxBuf + sizeof(BulkFragHeader), hdr->fragLen);

        ack.pktType = PKT_BULK_ACK;
        ack.ackSeq  = nextSeq;
        radio.transmit((uint8_t*)&ack, sizeof(ack));

        nextSeq++;
        timeoutCount = 0;
    }

    // Verify end-to-end CRC-32
    uint32_t rxCrc, calcCrc;
    memcpy(&rxCrc, buf + expectedLen, 4);
    calcCrc = crc32(buf, expectedLen);
    return (rxCrc == calcCrc);
}
```

---

## 9. Integration with Existing Firmware

### 9.1 Gateway — `gateway_tdma_task.cpp`

The bulk logic inserts into the existing idle-window phase (Zone 4) of the TDMA loop:

```cpp
// -- Zone 4: Idle window + end guard ----------------------------------------

// Step 1: Maintenance (always runs, ~20 ms)
evictStaleNodes();
// ... FRAM queue drain ...

// Step 2: Bulk transfer (conditional)
if (g_bulkSession.state == BULK_ACTIVE && g_bulkSession.acked) {
    uint32_t bulkStart = millis();

    // Single phase transition: LoRa → GFSK
    switchToGfsk();

    bool ok;
    if (g_bulkSession.dir == BULK_DIR_DL) {
        ok = runBulkSend(g_bulkSession.data, g_bulkSession.totalLen,
                         g_sfCount, g_bulkSession.slotId,
                         PKT_DIR_BULK_DL, bulkStart);
    } else {
        ok = runBulkReceive(g_bulkSession.rxBuf, g_bulkSession.totalLen,
                            g_sfCount, g_bulkSession.slotId,
                            PKT_DIR_BULK_UL);
    }

    // Single phase transition: GFSK → LoRa
    switchToLora();

    g_bulkSession.state = ok ? BULK_COMPLETE : BULK_FAILED;
    if (!ok) g_bulkSession.retryCount++;

    // Notify web layer (transport-level status only: success / fail / bytes)
    broadcastBulkStatus(g_bulkSession.slotId, ok,
                        g_bulkSession.totalLen);

    // If UL completed, the received data sits in g_bulkSession.rxBuf.
    // The web server reads it on the next /api/bulk/result GET.
    // The transport does not interpret the data.
}

// Step 3: Wait for superframe end
waitUntilMs(sfStart + SUPERFRAME_MS - END_GUARD_MS);
```

### 9.2 Node — `node_tdma_task.cpp`

The node's bulk participation inserts after the contention window, replacing the idle-sleep phase when a bulk transfer is active:

```cpp
// After contention window handling:

if (s_bulkGranted && s_bulkReady) {
    uint32_t bulkStart = sfStart + SUPERFRAME_MS - IDLE_MS + BULK_MAINT_MS;
    waitUntilMs(bulkStart);

    switchToGfsk();

    bool ok;
    if (s_bulkDir == BULK_DIR_DL) {
        ok = runBulkReceive(s_bulkBuf, s_bulkTotalLen,
                            s_sfCount, s_slotId, PKT_DIR_BULK_DL);
    } else {
        ok = runBulkSend(s_bulkBuf, s_bulkTotalLen,
                         s_sfCount, s_slotId, PKT_DIR_BULK_UL,
                         millis());
    }

    switchToLora();

    // Deliver to application layer via callback (transport does not interpret)
    if (ok && s_bulkDir == BULK_DIR_DL && s_bulkRxCallback) {
        s_bulkRxCallback(s_bulkType, s_bulkBuf, s_bulkTotalLen);
    }

    s_bulkGranted = false;
    s_bulkReady   = false;
}

// Resume LoRa beacon listen
radio.implicitHeader(sizeof(BeaconPacket));
radio.startReceive();
```

### 9.3 Telemetry Packet — BULK_READY Flag

The node signals readiness by setting bit 7 of the existing `statusFlags` byte in `TelemetryPacket`:

```cpp
#define STATUS_BULK_READY  0x80   // Bit 7: node is ready for bulk phase

// In node's UL composition:
if (s_bulkGranted) {
    telPkt.statusFlags |= STATUS_BULK_READY;
}
```

The gateway checks this flag when processing the UL in `processUplink()`:

```cpp
if (ulPkt.statusFlags & STATUS_BULK_READY) {
    if (g_bulkSession.state == BULK_GRANT_SENT &&
        g_bulkSession.slotIdx == slotIdx) {
        g_bulkSession.acked = true;
    }
}
```

### 9.4 Web API Endpoint

A generic REST endpoint triggers bulk transfers. The web server passes the payload through without interpreting it:

```
POST /api/bulk
Content-Type: application/json

{
  "slotId": 3,
  "direction": "dl",
  "typeTag": 1,
  "data": "<base64-encoded payload>"
}

Response: 200 OK
{
  "status": "queued",
  "totalLen": 500,
  "estimatedMs": 66
}
```

For UL requests:

```
POST /api/bulk
{
  "slotId": 3,
  "direction": "ul",
  "typeTag": 16,
  "expectedLen": 4000
}
```

Completion is pushed via WebSocket:

```json
{
  "event": "bulk_complete",
  "slotId": 3,
  "direction": "dl",
  "typeTag": 1,
  "success": true,
  "totalLen": 500
}
```

For UL completions, the received data is available via:

```
GET /api/bulk/result?slotId=3

Response: 200 OK
Content-Type: application/octet-stream
<raw payload bytes>
```

The web API is a thin pass-through. It does not validate the payload contents or interpret `typeTag`. Application-specific dashboards (classifier management, calibration UI, etc.) are built on top of this generic API.

### 9.5 Multi-Node Broadcast

For operations that push the same payload to multiple nodes (e.g., deploying a configuration block network-wide), the gateway maintains a broadcast queue:

```cpp
struct BulkBroadcast {
    uint8_t  pendingSlots;     // Bitmask: which nodes still need the payload
    uint8_t  typeTag;          // Application-defined type tag (opaque)
    uint8_t  bulkDir;          // Always DL for broadcast
    uint16_t totalLen;
    uint8_t  data[BULK_MAX_PAYLOAD]; // Payload (shared across all targets)
    uint8_t  retries[MAX_NODES];     // Per-node retry counter
};
```

Each superframe, the gateway picks the next unserved node from `pendingSlots`, sends the `BulkGrantPacket` in that node's DL window, and runs the bulk phase. At 3 seconds per node, all 8 nodes are served in 24 seconds. Failed nodes are retried on the next pass, up to `BULK_GRANT_RETRIES` attempts per node.

The web API for broadcast:

```
POST /api/bulk/broadcast
{
  "typeTag": 1,
  "data": "<base64-encoded payload>"
}
```

Progress is reported via WebSocket: `{"event": "bulk_broadcast_progress", "completed": 5, "total": 8}`.

---

## 10. Memory Requirements

### 10.1 Gateway

| Buffer | Size | Notes |
|---|---|---|
| `g_bulkSession.data` (DL payload) | 4096 B | Allocated in `.bss`; opaque byte buffer |
| `g_bulkSession.rxBuf` (UL receive) | 4096 B | Allocated in `.bss`; opaque byte buffer |
| Fragment TX buffer | 204 B | Stack-allocated in `runBulkSend` |
| Fragment RX buffer | 204 B | Stack-allocated in `runBulkReceive` |
| `BulkBroadcast` struct | ~4108 B | Only allocated if broadcast feature is used |
| **Total additional RAM** | **~8.4 kB** (without broadcast) | Well within ESP32's 520 kB SRAM |

### 10.2 Node

| Buffer | Size | Notes |
|---|---|---|
| `s_bulkBuf` (DL receive / UL transmit) | 4096 B | Allocated in `.bss`; opaque byte buffer |
| Fragment TX/RX buffer | 204 B | Stack-allocated |
| **Total additional RAM** | **~4.3 kB** | Well within ESP32-S3's 512 kB SRAM |

---

## 11. Non-Participating Node Isolation

Non-participating nodes are provably unaffected by the bulk phase:

1. **Temporal isolation.** The GFSK burst occurs exclusively within the idle window, after all 8 data slots and the contention window have completed. No node's UL/DL timing is displaced or shortened.

2. **Modulation isolation.** Non-participating nodes are in LoRa RX mode (`radio.startReceive()` with implicit header), waiting for the next beacon. The LoRa demodulator and FSK demodulator are separate signal paths in the SX1278. GFSK transmissions do not produce a LoRa preamble detection. The LoRa correlator rejects GFSK energy as wideband noise.

3. **Timing isolation.** Non-participating nodes' beacon timeout (~2040 ms from the last beacon) expires before the next beacon arrives at t=3000 ms. The `startReceive()` in LoRa continuous-RX mode simply waits, seeing no valid preamble from GFSK traffic, until the actual LoRa beacon arrives. No false trigger occurs.

4. **Frequency isolation.** The bulk phase operates on a single channel (Ch 0 or a dedicated bulk channel). Non-participating nodes are tuned to Ch 0 for beacon reception. Even if the GFSK and LoRa frequencies overlap, the modulation mismatch provides complete rejection.

---

## 12. Error Handling and Edge Cases

### 12.1 Grant Not Acknowledged

If the target node misses the `BulkGrantPacket` (e.g., its DL window reception failed), the `BULK_READY` flag will not be set in the UL telemetry. The gateway detects this and does not initiate the mode switch. The grant is retried in the next superframe, up to `BULK_GRANT_RETRIES` attempts. If all fail, the session is marked as failed and the web API returns an error.

### 12.2 Transfer Timeout

If a fragment fails after `BULK_MAX_RETRIES` (3) retransmissions, the sender aborts the session. Both sides switch back to LoRa and resume normal operation. The gateway can re-queue the transfer for a future superframe.

### 12.3 Superframe Overrun Protection

The bulk sender monitors elapsed time against `BULK_MAX_WINDOW_MS`. If the remaining time is insufficient for another fragment cycle, the sender aborts cleanly and switches back to LoRa before `END_GUARD_MS`. This prevents the GFSK phase from bleeding into the next beacon. See the overrun check in the sender pseudocode (Section 8.2).

### 12.4 Node Power Cycle During Transfer

If the target node loses power during the GFSK phase, the gateway's ACK timeout fires for the current fragment. After `BULK_MAX_RETRIES` timeouts, the gateway aborts, switches back to LoRa, and marks the session as failed. The node reboots, re-contends via the contention window, and the gateway can re-queue the bulk transfer once the node is registered again.

### 12.5 Gateway Reboot During Transfer

The bulk session state is volatile (RAM only). A gateway reboot during the GFSK phase causes the node's ACK timeout to fire repeatedly. The node eventually gives up, switches back to LoRa, and resumes normal beacon listen. On the next beacon, normal TDMA operation resumes. The caller can re-trigger the bulk transfer.

### 12.6 Concurrent DL Command Conflict

The `BulkGrantPacket` occupies the DL window for one superframe. If a relay command or other DL packet is also pending for the same node, the gateway must prioritise. Relay commands (immediate control) take priority over bulk grants (deferred setup). The bulk grant is delayed by one superframe. This is implemented by checking `queuedCmd.active` before loading the bulk grant into the DL queue.

### 12.7 End-to-End CRC Failure

If all fragments are received and ACKed but the end-to-end CRC-32 fails, `runBulkReceive()` returns `false`. The transport reports failure; the application callback is not invoked. The gateway can re-queue the entire transfer. This case indicates a systematic issue (e.g., encryption key mismatch, buffer corruption) rather than a transient RF problem, since per-fragment CRCs passed.

---

## 13. Implementation Checklist

### Phase 1: Core Transport Infrastructure

- [ ] Define new packet types in `tdma_protocol.h`: `PKT_BULK_GRANT` (0x08), `BulkFragHeader`, `BulkAckPacket`, direction constants (`PKT_DIR_BULK_DL`, `PKT_DIR_BULK_UL`)
- [ ] Implement `switchToGfsk()` and `switchToLora()` helper functions using RadioLib (or direct SPI register caching for lower latency)
- [ ] Implement `runBulkSend()` stop-and-wait fragment sender with overrun protection
- [ ] Implement `runBulkReceive()` stop-and-wait fragment receiver with end-to-end CRC-32 verification
- [ ] Implement `crc32()` utility (software CRC-32, no dependency on hardware accelerator)
- [ ] Extend `pkt_crypto.h` with `pktEncryptBulk()` / `pktDecryptBulk()` using the extended nonce format (byte 4 = seqNum)
- [ ] Add `STATUS_BULK_READY` flag definition (bit 7 of `statusFlags`)

### Phase 2: Gateway Integration

- [ ] Add `BulkSession` state struct and state machine to `gateway_tdma_task.cpp`
- [ ] Insert bulk phase logic into Zone 4 (idle window) of the TDMA loop
- [ ] Add `BulkGrantPacket` queuing logic (priority below relay commands, mutually exclusive with `queuedCmd`)
- [ ] Implement `bulkEnqueue()`, `bulkRequestUpload()`, `bulkGetStatus()` API functions
- [ ] Add `POST /api/bulk` and `GET /api/bulk/result` REST endpoints to `web_server.cpp`
- [ ] Add WebSocket push for bulk transfer completion events
- [ ] Implement `BulkBroadcast` serialiser for multi-node delivery via `POST /api/bulk/broadcast`

### Phase 3: Node Integration

- [ ] Add `BulkGrantPacket` handler in node DL processing (store dir, typeTag, totalLen; set `s_bulkGranted`)
- [ ] Add `BULK_READY` flag setting in telemetry composition when `s_bulkGranted` is true
- [ ] Insert bulk phase logic after contention window in `node_tdma_task.cpp`
- [ ] Implement `bulkRegisterRxCallback()` and callback invocation after successful DL receive
- [ ] For UL: implement `bulkSetUplinkData(typeTag, data, len)` to stage data before the bulk phase
- [ ] Allocate `s_bulkBuf[BULK_MAX_PAYLOAD + 4]` in `.bss` (extra 4 bytes for CRC-32 headroom)

### Phase 4: Transport Validation

These tests validate the transport layer in isolation, without any application-specific payloads.

- [ ] **Echo test (DL + UL round trip):** Gateway sends a known byte pattern (e.g., 500 B of `0xAA 0x55` repeating) to a node via DL bulk. Node's test callback stores the payload unchanged. Gateway immediately requests a UL bulk of the same data. Compare received bytes against the original. Validates: fragment assembly, CRC-32 integrity, encryption/decryption round trip, GFSK mode switching, timing.
- [ ] **Boundary size tests:** Repeat the echo test at payload sizes 1 B, 200 B (single fragment, full), 201 B (two fragments, second is 1 B), 4096 B (maximum), and 4097 B (must be rejected by `bulkEnqueue()`).
- [ ] **Pattern integrity test:** Send 4096 bytes of sequential byte values (0x00, 0x01, ..., 0xFF, 0x00, ...) via DL. Node callback verifies every byte matches the expected sequence. Validates byte-level integrity across all fragment boundaries.
- [ ] **Forced retransmission test:** Insert a deliberate CRC error on fragment N (by corrupting the SPI buffer before TX, or using an RF attenuator to drop one fragment). Verify: the NACK/retry mechanism recovers, the final payload is intact, and the transfer completes within `BULK_MAX_WINDOW_MS`.
- [ ] **Grant rejection test:** Send `BulkGrantPacket` but prevent the node from setting `BULK_READY` (e.g., by temporarily disabling the grant handler). Verify: gateway does not switch to GFSK, the idle window is undisturbed, the grant is retried next superframe.
- [ ] **Overrun protection test:** Request a transfer large enough that simulated retries push the total time near `BULK_MAX_WINDOW_MS`. Verify: the sender aborts cleanly before `END_GUARD_MS`, the next beacon is transmitted on time, and regular telemetry resumes in the following superframe.
- [ ] **Concurrent command test:** Queue both a relay command and a `BulkGrantPacket` for the same node in the same superframe. Verify: the relay command is delivered (priority), the bulk grant is deferred to the next superframe.

### Phase 5: System-Level Regression

- [ ] **Telemetry PDR regression:** With bulk transfers active (one per superframe, cycling through all 8 nodes), measure uplink packet delivery ratio across 1000 superframes. PDR must remain ≥99% — identical to the baseline without bulk transfers.
- [ ] **Timing regression (logic analyser):** Capture DIO pin transitions across 100 superframes with bulk active. Verify: GFSK phase never starts before the contention window ends, GFSK phase never extends past `SUPERFRAME_MS - END_GUARD_MS`, LoRa beacon TX occurs within ±1 ms of the expected time.
- [ ] **Multi-node broadcast test:** Trigger a broadcast of a 500 B payload to all 8 nodes. Verify: all 8 receive the payload intact, delivery completes within 8 superframes (24 s) with no retries, or within 16 superframes (48 s) with worst-case retries.
- [ ] **Encryption validation:** Run the echo test with `PKT_ENCRYPTION` enabled. Verify: payloads are correctly encrypted and decrypted, no keystream reuse across fragments (compare ciphertext of identical plaintext fragments — must differ due to seqNum in nonce).
- [ ] **Power-cycle resilience:** Trigger a node power cycle mid-transfer. Verify: gateway detects failure within `BULK_MAX_RETRIES × BULK_ACK_TIMEOUT_MS`, switches back to LoRa, next beacon is on time, node re-joins via contention and can receive a new bulk grant.

---

## 14. Future Extensions

These are explicitly deferred and do not affect the current design:

**Application-layer consumers.** The load classification subsystem (NN model deployment, training-data upload, threshold tuning), ADC calibration, and rich automation commands are all planned consumers of the bulk transport. Each will define its own `typeTag` values and register appropriate `BulkRxCallback` handlers. Their specifications are maintained in separate documents and depend on this transport being available, not the reverse.

**Multi-node per superframe.** If a future use case requires simultaneous bulk delivery (none identified currently), the idle window can be subdivided into time-sliced GFSK sub-windows. The `BulkGrantPacket` would carry a `turnOrder` field. The complexity is not justified for the current payload sizes and operation frequencies.

**Adaptive GFSK rate.** The gateway could measure RSSI during regular LoRa telemetry and select the GFSK rate dynamically (38.4 kbps for weak nodes, 250 kbps for strong ones). This requires a rate field in the `BulkGrantPacket` and pre-agreed register configurations for each rate tier.

**Streaming mode.** For continuous high-rate data (e.g., real-time waveform capture), the fragment protocol could be replaced with a streaming mode that sacrifices per-fragment acknowledgement for throughput. This would require a different error recovery strategy (FEC codes rather than ARQ).

**OTA firmware update.** The bulk channel could eventually carry firmware images (64–256 kB), requiring a multi-superframe transfer manager with checkpoint/resume. This is architecturally compatible with the fragment protocol but needs persistent session state in FRAM to survive power cycles.
