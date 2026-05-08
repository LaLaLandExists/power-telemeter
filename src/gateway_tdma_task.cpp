/**
 * gateway_tdma_task.cpp
 *
 * Core TDMA engine for the gateway.
 *
 * Superframe structure (~1840 ms total):
 *   [Beacon 40 ms] [Slot-1..8 each 210 ms] [Contention UL 60 ms + DL 40 ms] [Guard 20 ms]
 *
 * Each slot pair:
 *   [UL RX window 80 ms] [DL TX window 80 ms] [Guard 50 ms]
 *   Channel = hopChannel(sfCount, slotId)
 *   Phase guard (500 µs) applied between pairs where channel changes.
 *
 * Timing discipline:
 *   All waits are anchored to sfStart (millis() at beacon TX).
 *   waitUntilMs() is used everywhere to prevent accumulated drift.
 *
 * Thread safety:
 *   g_nodesMutex protects all g_nodes[] writes.
 *   The web task (Core 0) must also hold the mutex before reading g_nodes[].
 */

#include "gateway_tdma_task.h"
#include "gateway_web.h"          // webBroadcastTelemetry()
#include "fram_store.h"
#include "log_async.h"
#include <RadioLib.h>

// Dirty counters for deferred FRAM writes (n=10 policy)
#define FRAM_WRITE_N 10
static uint8_t s_energyDirty[MAX_NODES] = {};
static uint8_t s_histDirty[MAX_NODES]   = {};

// --- Radio instance (SPI pins defined in main.cpp) -----------------------
extern SX1278 radio;

// --- Shared state definitions (extern in gateway_state.h) -------------------
NodeState         g_nodes[MAX_NODES]  = {};
SemaphoreHandle_t g_nodesMutex        = nullptr;
uint16_t          g_sfCount           = 0;
uint8_t           g_slotMask          = 0;
uint8_t           g_networkEpoch      = 0;
uint8_t           g_gwHour            = 0;
uint8_t           g_gwMinute          = 0;
uint8_t           g_gwSecond          = 0;
uint32_t          g_gwTimeAt          = 0;
bool              g_timeSet           = false;

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

/** Change frequency and wait for PLL to re-lock (~500 µs). */
static void setChannel(uint8_t ch) {
  radio.setFrequency(LORA_CHANNELS[ch]);
  delayMicroseconds(PHASE_GUARD_US);
}

/**
 * Wait for a LoRa packet within a time window.
 * Radio must already be in startReceive() mode before calling.
 *
 * @param buf        Output buffer
 * @param maxLen     Buffer size
 * @param windowMs   How long to listen (ms)
 * @return           Bytes received, or -1 on timeout/error
 */
static int16_t rxWindow(uint8_t* buf, size_t maxLen, uint32_t windowMs) {
  uint32_t deadline = millis() + windowMs;
  while ((int32_t)(deadline - millis()) > 0) {
    // radio.available() relies on the DIO0 ISR; also poll the pin directly as a
    // fallback for marginal breadboard contacts that pass level-read but miss edges.
    bool pktReady = radio.available() ||
                    (digitalRead(14) && radio.getPacketLength() > 0); // 14 = LORA_PIN_DIO0
    if (pktReady) {
      int len = radio.getPacketLength();
      if (len > 0 && (size_t)len <= maxLen) {
        int16_t st = radio.readData(buf, (size_t)len);
        if (st == RADIOLIB_ERR_NONE) return (int16_t)len;
      }
      return -1;  // CRC error or oversized - caller restarts receive
    }
    vTaskDelay(pdMS_TO_TICKS(1));  // block so lower-priority tasks (loop/WiFi) can run
  }
  return -1;  // Timeout
}

// -----------------------------------------------------------------------------
// Energy accumulation
// -----------------------------------------------------------------------------
static void accumulateEnergy(NodeState* ns, uint32_t deltaWh) {
  // pkt.energy is now a Wh increment computed on the node, not a raw counter.
  // Rollover and power-cycle resets are handled node-side; the gateway just adds.
  ns->accumEnergy += deltaWh;

  uint8_t idx = (uint8_t)(ns - g_nodes);
  if (++s_energyDirty[idx] >= FRAM_WRITE_N) {
    s_energyDirty[idx] = 0;
    framQueueSave(idx, true, false);
  }
}

// -----------------------------------------------------------------------------
// History buffer
// -----------------------------------------------------------------------------
static void addHistory(NodeState* ns, const TelemetryPacket& pkt) {
  HistoryPoint& hp = ns->history[ns->histHead];
  hp.t = millis();
  hp.v = pkt.voltage     / 10.0f;
  hp.i = pkt.current     / 1000.0f;
  hp.p = pkt.power       / 10.0f;

  ns->histHead = (ns->histHead + 1) % HISTORY_MAX_POINTS;
  if (ns->histCount < HISTORY_MAX_POINTS) ns->histCount++;

  uint8_t idx = (uint8_t)(ns - g_nodes);
  if (++s_histDirty[idx] >= FRAM_WRITE_N) {
    s_histDirty[idx] = 0;
    framQueueSave(idx, false, true);
  }
}

// -----------------------------------------------------------------------------
// Process a received TelemetryPacket
// -----------------------------------------------------------------------------
static void processUplink(uint8_t slotIdx, const TelemetryPacket& pkt, int16_t rssi) {
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

  NodeState* ns = &g_nodes[slotIdx];

  // Ghost-node detection: DL-first design handles eviction via the NoopPacket
  // UID check.  An unexpected UL UID is a secondary indicator (the ghost has
  // already received a DL with the wrong UID and should be re-contending).
  // Log the event but do not mutate node state; the DL path is authoritative.
  if (pkt.uid != ns->deviceUID) {
    logAsync("[GW-UL] Slot%d UID mismatch (rx=0x%04X exp=0x%04X) - ignoring UL\n",
             slotIdx + 1, pkt.uid, ns->deviceUID);
    xSemaphoreGive(g_nodesMutex);
    return;
  }

  memcpy(&ns->latest, &pkt, sizeof(TelemetryPacket));
  ns->hasData    = true;
  ns->rssi       = rssi;
  ns->lastSeen   = millis();
  ns->missedSfs  = 0;

  // Decode status byte
  ns->relayState = decodeRelayState(pkt.statusByte);
  ns->relayMode  = decodeRelayMode (pkt.statusByte);
  ns->schedState = decodeSchedState(pkt.statusByte);
  ns->alarmState = decodeAlarmState(pkt.statusByte);
  ns->fwVersion  = pkt.fwVersion;
  ns->beaconRSSI = pkt.beaconRSSI;
  ns->seqLast    = pkt.seqCounter;

  accumulateEnergy(ns, pkt.energy);
  addHistory(ns, pkt);

  // Resolve pending flag: check if node state matches what was commanded
  if (ns->pending) {
    bool confirmed = false;
    if (ns->queuedCmd.len >= 3 && ns->queuedCmd.data[0] == PKT_RELAY_MANUAL) {
      confirmed = (ns->relayState == ns->queuedCmd.data[2]);
    } else if (ns->queuedCmd.len >= 7 && ns->queuedCmd.data[0] == PKT_RELAY_SCHEDULE) {
      confirmed = (ns->relayMode == 1 && ns->schedState > 0);
    } else if (ns->queuedCmd.len >= 2 && ns->queuedCmd.data[0] == PKT_RELAY_CLEAR) {
      confirmed = (ns->relayMode == 0 && ns->schedState == 0);
    } else {
      // Threshold, nudge - no confirmation from telemetry, rely on timeout
      if ((millis() - ns->pendingSentAt) >= PENDING_TIMEOUT_MS) {
        ns->pending = false;
      }
    }
    if (confirmed) ns->pending = false;
  }
  // Also clear pending on timeout regardless of command type
  if (ns->pending && (millis() - ns->pendingSentAt) >= PENDING_TIMEOUT_MS) {
    ns->pending = false;
  }

  xSemaphoreGive(g_nodesMutex);

  // Push to WebSocket clients on Core 0 (non-blocking, safe from Core 1)
  webBroadcastTelemetry(slotIdx);

  logAsync("[GW-UL] Slot%d V=%.1f A=%.3f W=%.1f Wh=%lu RSSI=%d\n",
           pkt.nodeId,
           pkt.voltage / 10.0f,
           pkt.current / 1000.0f,
           pkt.power   / 10.0f,
           (unsigned long)ns->accumEnergy,
           rssi);
}

// -----------------------------------------------------------------------------
// Build beacon and transmit on Ch 0
// -----------------------------------------------------------------------------
static void sendBeacon() {
  BeaconPacket beacon;
  beacon.pktType  = PKT_BEACON;
  beacon.epoch    = g_networkEpoch;
  beacon.sfCount  = g_sfCount;
  beacon.slotMask = g_slotMask;
  getGwHMS(beacon.hour, beacon.minute, beacon.second);

  // Quiesce radio before TX: contention window leaves it in startReceive() with no
  // cleanup. If the SX1278 is mid-receive when transmit() fires, its internal state
  // machine can reject config register writes, producing RADIOLIB_ERR_SPI_WRITE_FAILED.
  // standby() here settles the mode; PHASE_GUARD_US in setChannel() covers the transition.
  radio.standby();
  setChannel(0);
  uint8_t beaconBuf[sizeof(BeaconPacket)];
  memcpy(beaconBuf, &beacon, sizeof(beacon));
  // sfCount (bytes 0-1) stays plaintext; encrypt the rest with nonce (sfCount, 0, BEACON).
  pktEncrypt(beaconBuf + 2, sizeof(beaconBuf) - 2, beacon.sfCount, 0, PKT_DIR_BEACON);
  int16_t st = radio.transmit(beaconBuf, sizeof(beaconBuf));
  if (st != RADIOLIB_ERR_NONE) {
    logAsync("[GW-BCN] TX error %d\n", st);
  } else if (g_sfCount % 10 == 0) {
    logAsync("[GW-BCN] sf=%d mask=0x%02X\n", g_sfCount, g_slotMask);
  }
}

// -----------------------------------------------------------------------------
// Downlink TX — always called for every occupied slot.
//
// If a command is queued the packet is built as:
//   [pktType | nodeId | uid_lo | uid_hi | payload...]
// (PendingCmd stores the old-format {pktType, nodeId, payload...}; the UID is
// injected here at bytes 2-3 from g_nodes[slotIdx].deviceUID.)
//
// If no command is queued a NoopPacket (DlHeader only, PKT_NOOP) is sent.
// The UID in every DL lets a ghost node self-evict: it compares the received
// uid against g_nodeUID and re-contends on mismatch without gateway intervention.
//
// Must be called with g_nodesMutex held.
// -----------------------------------------------------------------------------
static void sendDownlink(uint8_t slotIdx) {
  NodeState* ns = &g_nodes[slotIdx];

  uint8_t txBuf[12];
  memset(txBuf, 0, MAX_DL_PAYLOAD_LEN);
  uint8_t txLen;
  bool    hasCmd = ns->queuedCmd.active && ns->queuedCmd.len >= 2;

  if (hasCmd) {
    // Reshape from old-format {pktType, nodeId, payload...}
    //                  to new {pktType, nodeId, uid_lo, uid_hi, payload...}
    uint8_t payloadLen = ns->queuedCmd.len - 2;
    txBuf[0] = ns->queuedCmd.data[0];
    txBuf[1] = ns->slotId;
    txBuf[2] = (uint8_t)(ns->deviceUID & 0xFF);
    txBuf[3] = (uint8_t)(ns->deviceUID >> 8);
    if (payloadLen > 0) {
      memcpy(txBuf + 4, ns->queuedCmd.data + 2, payloadLen);
    }
    txLen = 4 + payloadLen;
  } else {
    txBuf[0] = PKT_NOOP;
    txBuf[1] = ns->slotId;
    txBuf[2] = (uint8_t)(ns->deviceUID & 0xFF);
    txBuf[3] = (uint8_t)(ns->deviceUID >> 8);
    txLen = 4;
  }

  pktEncrypt(txBuf, MAX_DL_PAYLOAD_LEN, g_sfCount, (uint8_t)(slotIdx + 1), PKT_DIR_DL);
  int16_t st = radio.transmit(txBuf, MAX_DL_PAYLOAD_LEN);
  bool ok = (st == RADIOLIB_ERR_NONE);

  if (ok && hasCmd) {
    ns->pendingSentAt    = millis();
    ns->queuedCmd.active = false;
    logAsync("[GW-DL] Slot%d type=0x%02X len=%d\n", slotIdx + 1, txBuf[0], txLen);
  } else if (!ok) {
    logAsync("[GW-DL] TX error %d slot%d\n", st, slotIdx + 1);
  }
}

// -----------------------------------------------------------------------------
// Evict nodes that have missed NODE_TIMEOUT_SFS consecutive superframes.
// Called once per superframe from the tail end of gatewayTdmaTask(), after all
// radio windows are closed and before the next sfStart.
// missedSfs is incremented in the slot loop only when no valid UL arrived for
// that slot; processUplink() resets it to 0 on a successful UL.  Active nodes
// (UL every SF) always have missedSfs == 0 entering this function.
// -----------------------------------------------------------------------------
static void evictStaleNodes() {
  bool evicted = false;

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

  for (uint8_t i = 0; i < MAX_NODES; i++) {
    NodeState* ns = &g_nodes[i];
    if (!ns->active) continue;
    if (ns->missedSfs < NODE_TIMEOUT_SFS) continue;

    logAsync("[GW-TDMA] Slot %d UID=0x%04X evicted (%d missed superframes)\n",
             ns->slotId, ns->deviceUID, ns->missedSfs);

    framQueueSave(i, true, true);   // flush any dirty energy/history before clearing
    ns->active    = false;
    ns->hasData   = false;
    ns->missedSfs = 0;
    g_slotMask &= ~(1u << i);
    evicted = true;
  }

  // No epoch bump here: ghost nodes that return after slot reassignment are
  // handled by the DL-first UID check (NoopPacket carries the owner UID every
  // superframe).  Epoch changes only on gateway boot (random seed, reboot case).
  (void)evicted;

  xSemaphoreGive(g_nodesMutex);
}

// -----------------------------------------------------------------------------
// Contention window - register new nodes
// -----------------------------------------------------------------------------
static void handleContentionWindow(uint32_t sfStart) {
  uint32_t cwStart = sfStart + BEACON_MS + (uint32_t)MAX_NODES * SLOT_PAIR_MS;

  // Contention UL: listen for join requests on Ch 0
  setChannel(0);
  radio.implicitHeader(sizeof(JoinRequestPacket));
  radio.startReceive();

  uint8_t buf[16];
  waitUntilMs(cwStart);

  uint32_t cwEnd = cwStart + CONTENTION_UL_MS;
  int16_t len = rxWindow(buf, sizeof(buf), (uint32_t)max(0L, (long)(cwEnd - millis())));

  uint32_t dlStart = cwStart + CONTENTION_UL_MS;
  waitUntilMs(dlStart);

  JoinRequestPacket req;
  if (len == (int16_t)sizeof(JoinRequestPacket) &&
      pktReceive(buf, (size_t)len, req, g_sfCount, 0, PKT_DIR_JOIN_RQ) &&
      req.pktType == PKT_JOIN_REQUEST) {
    uint16_t uid = ((uint16_t)req.uid_hi << 8) | req.uid_lo;

    logAsync("[GW-CW] JoinReq UID=0x%04X fw=%d\n", uid, req.fwVersion);

    // Find existing slot for this UID, or allocate a new one
    uint8_t targetSlot = 0;
    if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      // Check if already registered (node reboot case — slot still active)
      for (uint8_t i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].active && g_nodes[i].deviceUID == uid) {
          targetSlot            = g_nodes[i].slotId;
          g_nodes[i].lastSeen   = millis();
          g_nodes[i].missedSfs  = 0;   // reset so re-joining node gets a full window
          break;
        }
      }
      // Allocate new slot
      if (targetSlot == 0) {
        for (uint8_t i = 0; i < MAX_NODES; i++) {
          if (!g_nodes[i].active) {
            g_nodes[i].active    = true;
            g_nodes[i].slotId    = i + 1;
            g_nodes[i].deviceUID = uid;
            g_nodes[i].lastSeen  = millis();
            g_nodes[i].missedSfs = 0;
            // Preserve any label restored from FRAM; only set default if none.
            if (g_nodes[i].label[0] == '\0') {
              snprintf(g_nodes[i].label, sizeof(g_nodes[i].label),
                       "Node %d", i + 1);
            }
            g_slotMask |= (1u << i);
            targetSlot  = i + 1;
            logAsync("[GW-CW] Assigned slot %d to UID=0x%04X epoch=%d\n",
                     targetSlot, uid, g_networkEpoch);
            framQueueRestore(i);
            break;
          }
        }
      }
      xSemaphoreGive(g_nodesMutex);
    }

    if (targetSlot > 0) {
      // Send ACK on Ch 0 during contention DL window
      JoinAckPacket ack;
      ack.pktType = PKT_JOIN_ACK;
      ack.uid_lo  = req.uid_lo;
      ack.uid_hi  = req.uid_hi;
      ack.slotId  = targetSlot;
      uint8_t ackBuf[sizeof(JoinAckPacket)];
      memcpy(ackBuf, &ack, sizeof(ack));
      pktEncrypt(ackBuf, sizeof(ackBuf), g_sfCount, 0, PKT_DIR_JOIN_AK);
      radio.transmit(ackBuf, sizeof(ackBuf));
      logAsync("[GW-CW] JoinACK slot=%d\n", targetSlot);
    }
  }

  waitUntilMs(dlStart + CONTENTION_DL_MS);
}

// -----------------------------------------------------------------------------
// Main TDMA task
// -----------------------------------------------------------------------------
static void gatewayTdmaTask(void* /*params*/) {
  logAsync("[GW-TDMA] Task started on Core 1\n");

  while (true) {
    uint32_t sfStart = millis();
    g_sfCount++;

    // -- Zone 1: Beacon --------------------------------------------------
    sendBeacon();
    waitUntilMs(sfStart + BEACON_MS);

    // -- Zone 2: Slot pairs ----------------------------------------------
    for (uint8_t s = 0; s < MAX_NODES; s++) {
      uint32_t slotBase = sfStart + BEACON_MS + (uint32_t)s * SLOT_PAIR_MS;

      waitUntilMs(slotBase);

      // Skip unoccupied slots - advance time but don't touch radio
      if (!(g_slotMask & (1u << s))) {
        waitUntilMs(slotBase + SLOT_PAIR_MS);
        continue;
      }

      // Frequency hop — same channel for both DL and UL of the pair.
      uint8_t ch = hopChannel(g_sfCount, s + 1);
      setChannel(ch);

      // -- DL transmit window (first) --------------------------------
      // Always send: NoopPacket when idle, or the queued command.
      // The UID embedded in every DL lets a ghost node self-evict on mismatch.
      radio.standby();
      vTaskDelay(pdMS_TO_TICKS(5));
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sendDownlink(s);
        xSemaphoreGive(g_nodesMutex);
      }

      waitUntilMs(slotBase + SLOT_DL_MS);

      // -- UL receive window (second) --------------------------------
      radio.implicitHeader(sizeof(TelemetryPacket));
      radio.startReceive();
      uint8_t ulBuf[64];
      int16_t ulLen = rxWindow(ulBuf, sizeof(ulBuf), SLOT_UL_MS);

      // getRSSI() must be called before any further SPI activity (e.g. crypto).
      int16_t rssi = radio.getRSSI();
      TelemetryPacket ulPkt;
      bool gotUl = (ulLen == (int16_t)sizeof(TelemetryPacket) &&
                    pktReceive(ulBuf, (size_t)ulLen, ulPkt, g_sfCount,
                               (uint8_t)(s + 1), PKT_DIR_UL) &&
                    ulPkt.pktType == PKT_TELEMETRY &&
                    ulPkt.nodeId  == (uint8_t)(s + 1));
      if (gotUl) {
        processUplink(s, ulPkt, rssi);
      } else {
        // Count this as a missed superframe for this slot.
        // processUplink() resets missedSfs on a successful UL; we only
        // increment here, never in evictStaleNodes(), so active nodes are immune.
        if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          if (g_nodes[s].active) g_nodes[s].missedSfs++;
          xSemaphoreGive(g_nodesMutex);
        }
      }

      waitUntilMs(slotBase + SLOT_DL_MS + SLOT_UL_MS);

      // -- Intra-pair guard ------------------------------------------
      waitUntilMs(slotBase + SLOT_PAIR_MS);
    }

    // -- Zone 3: Contention window ----------------------------------------
    handleContentionWindow(sfStart);

    // -- Zone 4: End guard + eviction ------------------------------------
    uint32_t sfEnd = sfStart + SUPERFRAME_MS;
    waitUntilMs(sfEnd);
    evictStaleNodes();  // tail-end: no radio windows open, next beacon resyncs
  }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void gatewayTdmaTaskStart() {
  g_nodesMutex   = xSemaphoreCreateMutex();
  configASSERT(g_nodesMutex);
  g_networkEpoch = (uint8_t)(esp_random() & 0xFF);

  xTaskCreatePinnedToCore(
    gatewayTdmaTask,
    "GW_TDMA",
    8192,        // Stack in bytes (6k for tx/rx + JSON + Serial)
    nullptr,
    2,           // Priority 2 (higher than WiFi stack at 1)
    nullptr,
    1            // Core 1
  );
}

bool tdmaQueueCommand(uint8_t slotIdx, const uint8_t* data, uint8_t len) {
  if (slotIdx >= MAX_NODES || len == 0 || len > 8) return false;

  bool ok = false;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    NodeState* ns = &g_nodes[slotIdx];
    if (ns->active) {
      memcpy(ns->queuedCmd.data, data, len);
      ns->queuedCmd.len    = len;
      ns->queuedCmd.active = true;
      // Mark pending immediately so UL broadcasts before DL transmits still
      // show the locked state on the dashboard. sendDownlink resets pendingSentAt
      // to actual TX time so the 15 s timeout is anchored correctly.
      ns->pending       = true;
      ns->pendingSentAt = millis();
      ok = true;
    }
    xSemaphoreGive(g_nodesMutex);
  }
  return ok;
}

uint8_t tdmaFindSlotByNodeId(uint8_t nodeId) {
  for (uint8_t i = 0; i < MAX_NODES; i++) {
    if (g_nodes[i].active && g_nodes[i].slotId == nodeId) return i;
  }
  return 0xFF;
}
