/**
 * gateway_state.h
 * NodeState struct, shared gateway state, and command queue type.
 * Included by both gateway_tdma_task.cpp and gateway_web.cpp.
 */
#pragma once
#include "tdma_protocol.h"
#include "auto_rule.h"

// -----------------------------------------------------------------------------
// History circular buffer — per node, 120 points, ~3 s apart
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
// Pending downlink command — up to MAX_DL_PAYLOAD_LEN bytes, queued per node
// -----------------------------------------------------------------------------
struct PendingCmd {
  bool    active;
  uint8_t data[9];   // MAX_DL_PAYLOAD_LEN = 9 (DlRulePacket)
  uint8_t len;
};

// -----------------------------------------------------------------------------
// Multi-superframe rule delivery state machine
//
// Sending a full RuleSet requires K+2 superframes:
//   step 0        — send CLEAR (ruleIndex=0xFE)
//   step 1..K     — send rule[step-1]
//   step K+1      — send COMMIT (ruleIndex=0xFF)
//
// The gateway TDMA task advances step on each successful DL TX.
// After commit is sent, pending is set to wait for echo confirmation via
// the node's ruleCount field in the next TelemetryPacket.
// -----------------------------------------------------------------------------
struct RuleDelivery {
  bool     active;
  uint8_t  step;           // current step (0 = CLEAR, 1..N = rule, N+1 = COMMIT)
  uint8_t  ruleCount;      // total rules to deliver (K)
  uint8_t  expectedCount;  // expected ruleCount echo after commit
  // Pre-encoded DlRulePacket payloads: index 0 = CLEAR, 1..K = rules, K+1 = COMMIT
  uint8_t  pkts[10][9];    // max 10 steps (1 clear + 8 rules + 1 commit), 9 bytes each
};

// -----------------------------------------------------------------------------
// NodeState — full state for one registered node
// Indices: nodes[0] = slot 1, nodes[7] = slot 8
// -----------------------------------------------------------------------------
struct NodeState {
  // Registration
  bool     active;            // Slot is occupied
  uint8_t  slotId;            // 1–8
  uint16_t deviceUID;         // CRC-16 of MAC (for re-registration)
  char     label[30];         // Human-readable name, e.g., "Electric Fan of Death"

  // Latest telemetry (raw values)
  TelemetryPacket latest;
  bool     hasData;
  int16_t  rssi;              // dBm, RSSI of last received UL packet
  uint32_t lastSeen;          // millis() at last successful UL receive
  uint16_t missedSfs;         // consecutive superframes with no UL from this node

  // Decoded status fields (decoded from statusByte for fast access)
  uint8_t  relayState;          // 0=OFF, 1=ON
  uint8_t  ruleEngineEnabled;   // 1 = engine active (enabled + has rules)
  uint8_t  relaySource;         // 0=manual,1=protection,2=schedule,3=default
  uint8_t  alarmState;          // 0=OK, 1=ALARM
  uint8_t  protectionLatched;   // 1 = at least one protection rule latched
  uint8_t  hasAutoRule;         // 1 = rules stored on node (engine may be off)
  uint8_t  ruleCount;           // active rule count echoed from node

  // AutoRule storage (gateway caches these for FRAM and dashboard GET)
  AutoRule rules[AUTORULE_MAX];
  uint8_t  rulesStored;         // number of valid entries in rules[]

  // Gateway-side energy accumulation.
  // pkt.energy carries Wh increments (node-side delta encoding), so the
  // gateway simply adds each packet's value directly to accumEnergy.
  uint32_t accumEnergy;       // Accumulated Wh since last clear

  // Pending command tracking
  bool        pending;        // True while awaiting confirmation from node
  uint32_t    pendingSentAt;  // millis() when command was sent
  PendingCmd  queuedCmd;      // Command to send in next DL slot
  RuleDelivery ruleDelivery;  // Multi-superframe rule delivery state machine

  // History buffer
  HistoryPoint history[HISTORY_MAX_POINTS];
  int histHead;               // Write index
  int histCount;              // Number of valid entries

  // Diagnostics
  uint8_t  seqLast;           // Last seqCounter received (for packet loss calc)
  uint8_t  beaconRSSI;        // Last beacon RSSI as reported by node
  uint8_t  fwVersion;
};

// -----------------------------------------------------------------------------
// Shared gateway state — extern declarations
// Definitions live in gateway_tdma_task.cpp
// -----------------------------------------------------------------------------
extern NodeState         g_nodes[MAX_NODES];
extern SemaphoreHandle_t g_nodesMutex;
extern uint16_t          g_sfCount;
extern uint8_t           g_slotMask;
extern uint8_t           g_networkEpoch;  // incremented on every node eviction; sent in BeaconPacket.epoch

// Gateway clock (no NTP — set via /api/time or WebSocket set_time)
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
