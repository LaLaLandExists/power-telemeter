/**
 * gateway_state.h
 * NodeState struct, shared gateway state, and command queue type.
 * Included by both gateway_tdma_task.cpp and gateway_web.cpp.
 */
#pragma once
#include "tdma_protocol.h"

// -----------------------------------------------------------------------------
// History circular buffer -- per node, 120 points, ~3 s apart
// Only v/i/p stored (no energy, PF, frequency) per API spec.
// -----------------------------------------------------------------------------
#define HISTORY_MAX_POINTS  120

struct HistoryPoint {
  uint32_t t;   // millis() at gateway when packet received
  float    v;   // voltage  (V)
  float    i;   // current  (A)
  float    p;   // power    (W)
};

// -----------------------------------------------------------------------------
// Bulk transfer session state (gateway side)
// Driven exclusively by the TDMA task on Core 1; read by the web task on Core 0
// only after state reaches BULK_COMPLETE or BULK_FAILED (terminal, no race).
// -----------------------------------------------------------------------------
typedef enum {
  BULK_IDLE = 0,
  BULK_GRANT_QUEUED,   // bulkEnqueue() called; grant loads into next DL slot
  BULK_GRANT_SENT,     // grant transmitted; waiting for STATUS_BULK_READY in UL
  BULK_ACTIVE,         // node ACK'd; next idle window will run GFSK transfer
  BULK_COMPLETE,       // transfer completed successfully
  BULK_FAILED          // transfer aborted or timed out
} BulkSessionState_t;

// Transport test scenario IDs (runtime, set by gateway_web.cpp)
#define BULK_TEST_NONE          0x00
#define BULK_TEST_ECHO          0x01
#define BULK_TEST_BOUNDARY      0x02
#define BULK_TEST_PATTERN       0x03
#define BULK_TEST_PDR           0x04
#define BULK_TEST_BROADCAST500  0x05

struct BulkSession {
  BulkSessionState_t state;
  uint8_t   slotIdx;          // 0-based target slot index
  uint8_t   dir;              // 0=DL (GW->Node), 1=UL (Node->GW)
  uint8_t   typeTag;          // application type (0xF0 = echo/loopback)
  uint16_t  totalLen;         // declared payload size in bytes
  uint8_t*  buf;              // points to static s_bulkBuf in gateway_tdma_task.cpp
  uint16_t  bufFilled;        // bytes assembled so far (UL direction)
  bool      acked;            // node set STATUS_BULK_READY in UL
  uint8_t   grantRetries;     // number of BulkGrantPacket retransmissions
  uint32_t  grantSentAt;      // millis() when grant was last transmitted
  uint16_t  fragsSent;        // total fragment TX attempts
  uint16_t  fragsAcked;       // successfully ACKed fragments
  uint32_t  crc32Expected;    // pre-computed from DL payload (DL direction)
  uint32_t  crc32Received;    // computed from assembled UL payload (UL direction)
  uint32_t  testStartMs;      // millis() when the session was enqueued
  uint8_t   testScenario;     // BULK_TEST_* set by gateway_web.cpp
  uint16_t  pdrTotal;         // fragments attempted (PDR test scenario)
  uint16_t  pdrAcked;         // fragments successfully ACKed (PDR scenario)
};

// -----------------------------------------------------------------------------
// Pending downlink command -- up to 8 bytes, queued per node
// -----------------------------------------------------------------------------
struct PendingCmd {
  bool    active;
  uint8_t data[8];
  uint8_t len;
};

// -----------------------------------------------------------------------------
// NodeState -- full state for one registered node
// Indices: nodes[0] = slot 1, nodes[7] = slot 8
// -----------------------------------------------------------------------------
struct NodeState {
  // Registration
  bool     active;            // Slot is occupied
  uint8_t  slotId;            // 1-8
  uint16_t deviceUID;         // CRC-16 of MAC (for re-registration)
  char     label[30];         // Human-readable name, e.g., "Electric Fan of Death"

  // Latest telemetry (raw values)
  TelemetryPacket latest;
  bool     hasData;
  int16_t  rssi;              // dBm, RSSI of last received UL packet
  uint32_t lastSeen;          // millis() at last successful UL receive
  uint16_t missedSfs;         // consecutive superframes with no UL from this node

  // Decoded status fields (decoded from statusByte for fast access)
  uint8_t  relayState;        // 0=OFF, 1=ON
  uint8_t  relayMode;         // 0=MANUAL, 1=SCHEDULED
  uint8_t  schedState;        // 0=NONE, 1=WAITING, 2=ACTIVE
  uint8_t  alarmState;        // 0=OK, 1=ALARM

  // Gateway-side energy accumulation.
  // pkt.energy carries Wh increments (node-side delta encoding), so the
  // gateway simply adds each packet's value directly to accumEnergy.
  uint32_t accumEnergy;       // Accumulated Wh since last clear

  // Pending command tracking
  bool     pending;           // True while awaiting confirmation from node
  uint32_t pendingSentAt;     // millis() when command was sent
  uint8_t  pendingRetry;      // Number of retransmissions attempted so far
  PendingCmd queuedCmd;       // Command to send in next DL slot

  // Bulk grant pending -- decoupled from queuedCmd so relay priority is natural.
  // sendDownlink() checks queuedCmd.active FIRST; bulk grant only fires when idle.
  bool    bulkGrantPending;
  uint8_t bulkGrantBuf[sizeof(BulkGrantPacket)];  // pre-formatted 6-byte grant

  // History buffer
  HistoryPoint history[HISTORY_MAX_POINTS];
  int histHead;               // Write index
  int histCount;              // Number of valid entries

  // Diagnostics
  uint8_t  seqLast;           // Last seqCounter received (for packet loss calc)
  uint8_t  beaconRSSI;        // Last beacon RSSI as reported by node
  uint8_t  fwVersion;

  // Load classifier result (raw classByte from TelemetryPacket).
  // CLASS_BYTE_UNSUPPORTED (0xFF) = PZEM/no-AFE node.
  // CLASS_BYTE_PENDING     (0x40) = AFE node, smoothing window filling.
  // All other values: packed classId[2:0] + confidence[5:3] + transient[6].
  uint8_t  classByte = CLASS_BYTE_UNSUPPORTED;

#ifdef KNN_INFERENCE
  // K-NN appliance inference state (gateway-only; not transmitted over radio)
  uint8_t  knnMachineState;   // KNN_ST_UNIDENTIFIED / SETTLING / IDENTIFIED
  uint8_t  knnLabelIdx;       // 0xFF = unidentified
  uint8_t  knnStateIdx;       // matched operating-state index within profile
  uint8_t  knnZeroCnt;        // consecutive near-zero W samples (swap detection)
  uint8_t  knnSettleCnt;      // stable samples since last significant ΔW
  uint8_t  knnVotePos;        // circular write index into knnVoteBuf
  uint8_t  knnVoteBuf[5];     // last 5 label-index results for majority vote
  float    knnDistSq;         // nearest normalized distance^2 from last search
  float    knnPrevW;          // W from the previous telemetry sample
#endif
};

// -----------------------------------------------------------------------------
// Shared gateway state -- extern declarations
// Definitions live in gateway_tdma_task.cpp
// -----------------------------------------------------------------------------
extern NodeState         g_nodes[MAX_NODES];
extern SemaphoreHandle_t g_nodesMutex;
extern uint16_t          g_sfCount;
extern uint8_t           g_slotMask;
extern uint8_t           g_networkEpoch;  // incremented on every node eviction; sent in BeaconPacket.epoch
extern BulkSession       g_bulkSession;   // driven by TDMA task (Core 1); read by web task (Core 0)

// Gateway clock -- NTP-synced in STA mode; falls back to set_time WS / /api/time in AP mode
extern uint8_t           g_gwHour;
extern uint8_t           g_gwMinute;
extern uint8_t           g_gwSecond;
extern uint32_t          g_gwTimeAt;   // millis() when clock was last set
extern bool              g_timeSet;

// -----------------------------------------------------------------------------
// Shared clock helpers
// -----------------------------------------------------------------------------

/** Set gateway clock from H/M/S and capture millis() baseline. */
inline void setGatewayTime(uint8_t h, uint8_t m, uint8_t s) {
  g_gwHour   = h;
  g_gwMinute = m;
  g_gwSecond = s;
  g_gwTimeAt = millis();
  g_timeSet  = true;
}

/**
 * Get current gateway time as seconds-since-midnight.
 * Derived from base + elapsed millis(). Rolls over at 86400 s.
 */
inline uint32_t getGwTimeSec() {
  if (!g_timeSet) return 0;
  uint32_t elapsed = (millis() - g_gwTimeAt) / 1000UL;
  uint32_t base    = (uint32_t)g_gwHour * 3600u
                    + (uint32_t)g_gwMinute * 60u
                    + g_gwSecond;
  return (base + elapsed) % 86400UL;
}

/** Populate h/m/s from current gateway time. */
inline void getGwHMS(uint8_t &h, uint8_t &m, uint8_t &s) {
  uint32_t t = getGwTimeSec();
  h = (uint8_t)(t / 3600);
  m = (uint8_t)((t % 3600) / 60);
  s = (uint8_t)(t % 60);
}

/** Format current time as "HH:MM:SS" into a 9-char buffer. */
inline void gwTimeString(char* out_buf) {
  uint8_t h, m, s;
  getGwHMS(h, m, s);
  snprintf(out_buf, 9, "%02d:%02d:%02d", h, m, s);
}
