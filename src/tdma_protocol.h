/**
 * tdma_protocol.h
 * Shared header for gateway AND node firmware.
 *
 * Protocol v1.0 -- SF6 / 125 kHz / 433 MHz / 8-node star topology
 */
#pragma once
#include <Arduino.h>
// hal_rtos.h is included here so waitUntilMs() can use vTaskDelay / pdMS_TO_TICKS
// on all platforms.  On ESP32 these arrive transitively via <Arduino.h>; on STM32
// they do not, so the explicit include is required.
#include "hal/hal_rtos.h"
#include "hal/hal_sys.h"  // halGetMac()
#include "pkt_crypto.h"   // pktEncrypt / pktDecrypt / pktReceive (no-ops when !PKT_ENCRYPTION)

// -----------------------------------------------------------------------------
// Network constants
// -----------------------------------------------------------------------------
#define FW_VERSION          1
#define MAX_NODES           8
#define N_CHANNELS          8
#define LORA_SYNC_WORD      0x12    // Private network (not LoRaWAN 0x34)
#define BROADCAST_ADDR      0xFF

// -----------------------------------------------------------------------------
// Radio parameters  (SX1278 via RadioLib)
// -----------------------------------------------------------------------------
#define LORA_FREQUENCY      433.05f  // Ch 0 start; setFrequency() called per hop
#define LORA_BANDWIDTH      125.0f   // kHz
#define LORA_SF             6
#define LORA_CR             5        // RadioLib value: 5 = 4/5 coding rate
#define LORA_TX_POWER       10       // dBm, PA_BOOST pin -- 10 mW ERP limit (NTC 433 MHz ISM)
#define LORA_PREAMBLE_LEN   8

// -----------------------------------------------------------------------------
// Channel plan -- 433 MHz ISM, 200 kHz spacing
// Ch 0 is the fixed rendezvous channel (beacon + contention).
// -----------------------------------------------------------------------------
static const float LORA_CHANNELS[N_CHANNELS] = {
  433.050f,   // Ch 0 -- Beacon + contention (FIXED)
  433.250f,   // Ch 1
  433.450f,   // Ch 2
  433.650f,   // Ch 3
  433.850f,   // Ch 4
  434.050f,   // Ch 5
  434.250f,   // Ch 6
  434.450f,   // Ch 7
};

// -----------------------------------------------------------------------------
// TDMA timing constants (milliseconds)
//
// Superframe = Beacon + N x SlotPair + CWin_UL + CWin_DL + Idle + EndGuard
//            = 40 + 8 x 165 + 60 + 40 + 1520 + 20 = 3000 ms
//
// SlotPair breakdown -- DL-first ordering:
//   DL window  : 50 ms  (GW TX -> node RX; ~11 ms airtime, 4.5 x margin)
//   UL window  : 100 ms (node TX -> GW RX; kept wide to absorb beacon-latency jitter)
//   Guard      : 15 ms  (SX1278 mode-switch <1 ms; 15 ms covers FreeRTOS tick budget)
//
// UL window is intentionally NOT reduced: the node's TX start lags the gateway's
// RX open by beacon-arrival latency (~2-5 ms) + radio setup overhead.  The 20 ms
// pad over TelemetryPacket airtime (~34 ms) has been observed as necessary.
//
// The 360 ms IDLE_MS recovered from tighter slots is placed after the contention
// window.  The gateway uses it for maintenance (eviction, FRAM writes); the node
// re-enters ST_LISTEN and waits for the next beacon with a 2040 ms timeout.
// -----------------------------------------------------------------------------
#define BEACON_MS               40
#define SLOT_UL_MS              100
#define SLOT_DL_MS              50
#define SLOT_GUARD_MS           15
#define SLOT_PAIR_MS            (SLOT_UL_MS + SLOT_DL_MS + SLOT_GUARD_MS)  // 165 ms
#define PHASE_GUARD_US          500     // us - PLL re-lock between hops
#define CONTENTION_UL_MS        60
#define CONTENTION_DL_MS        40
#define IDLE_MS                 1520    // post-contention idle window; includes 360 ms recovered from slot guard reductions + 1160 ms intentional SF extension
#define END_GUARD_MS            20     // RF-quiet buffer before next beacon TX

// -- GFSK bulk transfer timing (ms) -----------------------------------------
// All constants anchor to the Idle window that follows Zone 3 (CW).
#define BULK_MAINT_MS           20      // Maintenance before mode switch
#define BULK_SWITCH_MS           2      // LoRa -> GFSK mode switch budget
#define BULK_RESTORE_MS          2      // GFSK -> LoRa restore budget
#define BULK_MAX_WINDOW_MS      (IDLE_MS - BULK_MAINT_MS - BULK_SWITCH_MS \
                                 - BULK_RESTORE_MS - END_GUARD_MS)  // 1476 ms
#define BULK_FRAG_PAYLOAD       200     // Usable payload bytes per fragment
#define BULK_FRAG_HEADER_LEN      4     // sizeof(BulkFragHeader)
#define BULK_FRAG_TOTAL         (BULK_FRAG_HEADER_LEN + BULK_FRAG_PAYLOAD)  // 204 bytes
#define BULK_FRAG_AIR_MS         17     // On-air time of a 204-byte GFSK frame at 100 kbps
#define BULK_ACK_AIR_MS           1     // On-air time of a 2-byte ACK
#define BULK_TURNAROUND_MS        2     // TX->RX or RX->TX turnaround
#define BULK_FRAG_CYCLE_MS      (BULK_FRAG_AIR_MS + BULK_ACK_AIR_MS \
                                 + BULK_TURNAROUND_MS * 2)          // 22 ms per fragment
#define BULK_ACK_TIMEOUT_MS      50     // Wait for ACK/NACK before retry
#define BULK_MAX_RETRIES          3     // Per-fragment retry limit
#define BULK_GRANT_RETRIES        3     // Grant-level retry across superframes
#define BULK_MAX_PAYLOAD       4096     // Maximum total payload size (bytes)

// -- GFSK radio parameters (SX1278, 100 kbps, 433 MHz band) -----------------
#define GFSK_FREQUENCY          433.05f   // MHz (same as LoRa Ch 0)
#define GFSK_BITRATE            100.0f    // kbps
#define GFSK_DEVIATION           50.0f    // kHz (modulation index 1.0)
#define GFSK_RX_BANDWIDTH       125.0f    // kHz (>= Fdev + BR/2)
#define GFSK_TX_POWER             10      // dBm
#define GFSK_PREAMBLE_LEN          4      // bytes (bit-synchroniser lock at 100 kbps)
static const uint8_t GFSK_SYNC_WORD[3] = {0xB5, 0x4A, 0x7E};

// Total superframe duration -- adjust IDLE_MS to change the period; verify the
// computed value matches the dashboard's SUPERFRAME_MS constant in app.js.
#define SUPERFRAME_MS \
  (BEACON_MS + MAX_NODES * SLOT_PAIR_MS + \
   CONTENTION_UL_MS + CONTENTION_DL_MS + IDLE_MS + END_GUARD_MS)  // 3000 ms

// On-air duration of an 8-byte BeaconPacket at SF6 / 125 kHz / CR 4:5,
// 8-symbol preamble, implicit header, CRC enabled.
// Ts = 2^6/125000 = 0.512 ms; ToA = (12.25 + 23) x 0.512 = 18.048 ms -> 18 ms.
// The node receives the beacon BEACON_AIR_MS after the gateway begins transmitting it,
// so it must subtract this value when anchoring its slot times to sfStart.
#define BEACON_AIR_MS           18

// -----------------------------------------------------------------------------
// Application timeouts
// -----------------------------------------------------------------------------
#define NODE_TIMEOUT_SFS        8       // Evict after 8 consecutive missed superframes (~24 s at 3000 ms/SF)
#define PENDING_TIMEOUT_MS      15000   // Pending command clears after 15 s
#define RTC_CORRECTION_THRESHOLD_MS 2000  // Only correct schedule RTC if drift > 2 s

// -----------------------------------------------------------------------------
// Packet type IDs
// -----------------------------------------------------------------------------
#define PKT_TELEMETRY           0x01    // Node -> GW,   32 bytes
#define PKT_RELAY_MANUAL        0x02    // GW   -> Node,  3 bytes (DlHeader + relayState)
#define PKT_RELAY_SCHEDULE      0x03    // GW   -> Node,  7 bytes (DlHeader + 5)
#define PKT_BEACON              0x04    // GW   -> All,   8 bytes
#define PKT_RELAY_CLEAR         0x05    // GW   -> Node,  2 bytes (DlHeader only)
#define PKT_THRESHOLD           0x06    // GW   -> Node,  4 bytes (DlHeader + thresh_lo/hi)
#define PKT_NUDGE               0x07    // GW   -> Node,  2 bytes (DlHeader only)
// -- Bulk transfer packet types (GFSK transport; Idle window only) -----------
#define PKT_BULK_GRANT          0x08    // GW   -> Node,  6 bytes (in existing DL slot)
#define PKT_BULK_FRAG           0xB0    // GFSK: BulkFragHeader(4B) + up to 200B payload
#define PKT_BULK_ACK            0xB1    // 2 bytes; NOT encrypted
#define PKT_BULK_NACK           0xB2    // 2 bytes; NOT encrypted
#define PKT_BULK_ABORT          0xB3    // 2 bytes; NOT encrypted
// Bit 7 of TelemetryPacket.statusByte (reserved range; does not touch bits 4:0)
#define STATUS_BULK_READY       0x80
#define PKT_JOIN_REQUEST        0xA0    // Node -> GW,    4 bytes
#define PKT_JOIN_ACK            0xA1    // GW   -> Node,  4 bytes

// -----------------------------------------------------------------------------
// Packet structures -- all packed to avoid alignment padding
// -----------------------------------------------------------------------------
#pragma pack(push, 1)

/**
 * BeaconPacket (8 bytes) -- broadcast on Ch 0 at superframe start.
 * Provides: time sync, superframe counter (hop seed), occupied slot bitmask,
 * and a network epoch for stale-registration detection.
 *
 * On-wire layout (PKT_ENCRYPTION builds):
 *   bytes 0-1  sfCount  -- plaintext; node reads this before decryption to build the nonce
 *   bytes 2-7  <rest>   -- AES-128 CTR encrypted with nonce (sfCount, slotId=0, PKT_DIR_BEACON)
 * In plain builds the full packet is sent unencrypted (same struct layout).
 *
 * epoch: initialised to halRandom() on gateway boot; incremented by 1 on every
 * node eviction (only possible when all 8 slots are full).  A node whose stored
 * s_joinEpoch differs from the beacon epoch knows its slot assignment may have
 * been invalidated and re-contends to obtain a fresh assignment.
 */
struct BeaconPacket {
  uint16_t sfCount;    // bytes 0-1: plaintext on wire - superframe counter, hop seed
  uint8_t  pktType;    // byte 2:  0x04 (encrypted in PKT_ENCRYPTION builds)
  uint8_t  epoch;      // byte 3:  network epoch - incremented every node eviction
  uint8_t  hour;       // byte 4:  0-23
  uint8_t  minute;     // byte 5:  0-59
  uint8_t  second;     // byte 6:  0-59
  uint8_t  slotMask;   // byte 7:  bit N = slot (N+1) occupied
};
static_assert(sizeof(BeaconPacket) == 8, "BeaconPacket must be 8 bytes");

/**
 * TelemetryPacket (30 bytes) -- node uplink every superframe.
 * Status byte packs relayState, relayMode, schedState, alarmState.
 *
 * statusByte layout:
 *   bit 0   : relayState  (0=OFF, 1=ON)
 *   bit 1   : relayMode   (0=MANUAL, 1=SCHEDULED)
 *   bits 3:2: schedState  (00=NONE, 01=WAITING, 10=ACTIVE)
 *   bit 4   : alarmState  (0=OK, 1=ALARM)
 *   bits 7:5: reserved
 */
struct TelemetryPacket {
  uint8_t  pktType;          // 0x01
  uint8_t  nodeId;           // 1-8 (assigned slot ID)
  uint16_t uid;              // CRC-16 of node MAC -- gateway verifies slot ownership
  uint16_t voltage;          // /10  -> volts     (2204 = 220.4 V)
  uint32_t current;          // /1000 -> amps     (2345 = 2.345 A)
  uint32_t power;            // /10  -> watts     (5163 = 516.3 W)
  uint32_t energy;           // Wh increment since last packet (node-side delta; handles rollover)
  uint16_t frequency;        // /10  -> Hz        (600  = 60.0 Hz)
  uint16_t powerFactor;      // /100 -> 0.00-1.00 (98   = 0.98)
  uint8_t  statusByte;       // packed bitfield (see above)
  uint8_t  schedSH;          // Schedule start hour
  uint8_t  schedSM;          // Schedule start minute
  uint8_t  schedEH;          // Schedule end hour
  uint8_t  schedEM;          // Schedule end minute
  uint16_t alarmThreshold;   // watts
  // seqCounter wire layout:
  //   bit  7   : dlAck -- set by node when a DL packet was decoded this superframe (one-shot, cleared after TX)
  //   bits 6:0 : rolling counter 0-127, increments every UL, for packet-loss detection
  uint8_t  seqCounter;
  uint8_t  beaconRSSI;       // int8 cast to uint8 -- RSSI of last beacon (dBm)
  uint8_t  fwVersion;        // Firmware version
};
static_assert(sizeof(TelemetryPacket) == 32, "TelemetryPacket must be 32 bytes");

/**
 * DlHeader (2 bytes) -- base for every gateway->node downlink packet.
 *
 * Slot ownership is validated by the epoch field in BeaconPacket: if
 * bcn.epoch != s_joinEpoch the node re-contends before the next data slot,
 * so a stale node can never occupy a slot belonging to another device.
 * The UID fields previously in this header are no longer needed.
 *
 * IMPORTANT: All DL command types MUST be absolute (set-to-value), never
 * relative (toggle/increment). The gateway retransmits unacknowledged commands
 * up to MAX_DL_RETRIES times; a relative command retried would double its effect.
 */
struct DlHeader {
  uint8_t pktType;
  uint8_t nodeId;
};
static_assert(sizeof(DlHeader) == 2, "DlHeader must be 2 bytes");

/** RelayCommandPacket (3 bytes) -- immediate relay toggle */
struct RelayCommandPacket : DlHeader {
  uint8_t relayState;   // 0=OFF, 1=ON
};
static_assert(sizeof(RelayCommandPacket) == 3, "RelayCommandPacket must be 3 bytes");

/** RelaySchedulePacket (7 bytes) -- daily recurring window */
struct RelaySchedulePacket : DlHeader {
  uint8_t onState;    // Relay state INSIDE the window (1=ON is the normal case)
  uint8_t startH;
  uint8_t startM;
  uint8_t endH;
  uint8_t endM;
};
static_assert(sizeof(RelaySchedulePacket) == 7, "RelaySchedulePacket must be 7 bytes");

/** RelayClearPacket (2 bytes) -- cancel active schedule, revert to manual */
struct RelayClearPacket : DlHeader {};
static_assert(sizeof(RelayClearPacket) == 2, "RelayClearPacket must be 2 bytes");

/** ThresholdPacket (4 bytes) -- set PZEM over-power alarm */
struct ThresholdPacket : DlHeader {
  uint8_t thresh_lo;   // watts, little-endian
  uint8_t thresh_hi;
};
static_assert(sizeof(ThresholdPacket) == 4, "ThresholdPacket must be 4 bytes");

/** NudgePacket (2 bytes) -- blink LED for physical identification */
struct NudgePacket : DlHeader {};
static_assert(sizeof(NudgePacket) == 2, "NudgePacket must be 2 bytes");

/**
 * BulkGrantPacket (6 bytes) -- delivered in target node's regular DL slot.
 * Informs the node that the gateway will initiate a GFSK bulk transfer during
 * the next Idle window.  The node responds with STATUS_BULK_READY set in its
 * next TelemetryPacket.statusByte.
 */
struct BulkGrantPacket : DlHeader {  // pktType=0x08, nodeId = 2 bytes
  uint8_t  bulkDir;       // 0=DL (GW->Node), 1=UL (Node->GW)
  uint8_t  bulkType;      // application type tag (0xF0 = echo/loopback)
  uint8_t  totalLen_lo;   // payload byte count, little-endian
  uint8_t  totalLen_hi;
};
static_assert(sizeof(BulkGrantPacket) == 6, "BulkGrantPacket must fit MAX_DL_PAYLOAD_LEN=7");

/**
 * BulkFragHeader (4 bytes) -- GFSK data fragment header.
 * Followed by up to BULK_FRAG_PAYLOAD bytes of payload data.
 * Hardware CRC-16 is appended by the SX1278 FSK packet engine.
 */
struct BulkFragHeader {
  uint8_t  pktType;       // PKT_BULK_FRAG (0xB0)
  uint8_t  seqNum;        // 0-based fragment sequence number (wraps at 255)
  uint8_t  payloadLen;    // actual payload bytes in this fragment (1-200)
  uint8_t  flags;         // bit 0 = LAST_FRAG; bits 7:1 reserved
};
static_assert(sizeof(BulkFragHeader) == 4, "BulkFragHeader must be 4 bytes");

/**
 * BulkAckPacket (2 bytes) -- NOT encrypted.
 * Used for PKT_BULK_ACK, PKT_BULK_NACK, and PKT_BULK_ABORT.
 */
struct BulkAckPacket {
  uint8_t  seqNum;        // echo of fragment seqNum being acknowledged
  uint8_t  pktType;       // PKT_BULK_ACK / PKT_BULK_NACK / PKT_BULK_ABORT
};
static_assert(sizeof(BulkAckPacket) == 2, "BulkAckPacket must be 2 bytes");

/** JoinRequestPacket (4 bytes) -- contention uplink from new node */
struct JoinRequestPacket {
  uint8_t  pktType;    // 0xA0
  uint8_t  uid_lo;
  uint8_t  uid_hi;
  uint8_t  fwVersion;
};

/** JoinAckPacket (4 bytes) -- contention downlink from gateway */
struct JoinAckPacket {
  uint8_t pktType;    // 0xA1
  uint8_t uid_lo;
  uint8_t uid_hi;
  uint8_t slotId;     // 1-8
};

#pragma pack(pop)

// Largest DL command (RelaySchedulePacket = 7 bytes).
// SF6 implicit header requires a fixed receive length; all DL frames are
// zero-padded to this size so every slot DL window uses one implicitHeader() call.
#define MAX_DL_PAYLOAD_LEN  sizeof(RelaySchedulePacket)

// -----------------------------------------------------------------------------
// Status byte helpers
// -----------------------------------------------------------------------------
inline uint8_t encodeStatus(uint8_t relayState, uint8_t relayMode,
                              uint8_t schedState, uint8_t alarmState) {
  return ((alarmState & 0x01) << 4) |
          ((schedState & 0x03) << 2) |
          ((relayMode  & 0x01) << 1) |
          (relayState & 0x01);
}
inline uint8_t decodeRelayState(uint8_t s)  { return  s        & 0x01; }
inline uint8_t decodeRelayMode (uint8_t s)  { return (s >> 1)  & 0x01; }
inline uint8_t decodeSchedState(uint8_t s)  { return (s >> 2)  & 0x03; }
inline uint8_t decodeAlarmState(uint8_t s)  { return (s >> 4)  & 0x01; }

// -----------------------------------------------------------------------------
// Hop sequence
//
// channel = (sfCount x 7 + slotId) % N_CHANNELS
// Multiplier 7 is coprime to 8 -> over 8 superframes each slot visits all channels.
// Beacon and contention always use Ch 0, regardless.
// -----------------------------------------------------------------------------
inline uint8_t hopChannel(uint16_t sfCount, uint8_t slotId) {
  return (uint8_t)(((uint32_t)sfCount * 7u + slotId) % N_CHANNELS);
}

// -----------------------------------------------------------------------------
// CRC-16/CCITT (initial value 0xFFFF, polynomial 0x1021)
// Used to derive a 2-byte deviceUID from the 6-byte ESP32 MAC address.
// -----------------------------------------------------------------------------
inline uint16_t crc16ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int j = 0; j < 8; j++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// CRC-32 (IEEE 802.3, polynomial 0xEDB88320)
// Used for end-to-end bulk payload integrity verification.
inline uint32_t crc32_calc(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
  }
  return ~crc;
}

/** Compute 2-byte device UID from factory MAC. Called once at boot. */
inline uint16_t computeDeviceUID() {
  uint8_t mac[6] = {0};
  halGetMac(mac);
  return crc16ccitt(mac, 6);
}

// -----------------------------------------------------------------------------
// Utility: spin-wait until millis() reaches a target
// Safe for 32-bit millis() rollover as long as gap < 2^31 ms.
// -----------------------------------------------------------------------------
inline void waitUntilMs(uint32_t target) {
  // Block (yields to lower-priority tasks) until within 2 ms of target,
  // then spin for the final stretch to preserve sub-millisecond precision.
  while ((int32_t)(target - millis()) > 2) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  while ((int32_t)(target - millis()) > 0) {
    taskYIELD();
  }
}
