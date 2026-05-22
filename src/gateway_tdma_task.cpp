/**
 * gateway_tdma_task.cpp
 *
 * Core TDMA engine for the gateway.
 *
 * Superframe structure (~3000 ms total):
 *   [Beacon 40 ms] [Slot-1..8 each 165 ms] [Contention UL 60 ms + DL 40 ms] [Idle 1520 ms] [Guard 20 ms]
 *
 * Each slot pair (DL-first):
 *   [DL TX 50 ms] [UL RX 100 ms] [Guard 15 ms]
 *   Channel = hopChannel(sfCount, slotId)
 *   Phase guard (500 us) applied between pairs where channel changes.
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
BulkSession       g_bulkSession       = {};
static uint8_t    s_bulkBuf[BULK_MAX_PAYLOAD];

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

/** Change frequency and wait for PLL to re-lock (~500 us). */
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
                    (digitalRead(2) && radio.getPacketLength() > 0); // 2 = LORA_PIN_DIO0
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

// -- GFSK mode switch helpers -------------------------------------------------

static bool switchToGfsk() {
  radio.standby();
  int16_t st = radio.beginFSK(GFSK_FREQUENCY, GFSK_BITRATE, GFSK_DEVIATION,
                               GFSK_RX_BANDWIDTH, GFSK_TX_POWER, GFSK_PREAMBLE_LEN, false);
  if (st != RADIOLIB_ERR_NONE) {
    logAsync("[GW-BULK] GFSK init err %d\n", st);
    return false;
  }
  radio.setSyncWord(const_cast<uint8_t*>(GFSK_SYNC_WORD), sizeof(GFSK_SYNC_WORD));
  radio.setCRC(2);
  return true;
}

static void switchToLora() {
  radio.standby();
  int16_t st = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SF, LORA_CR,
                            LORA_SYNC_WORD, LORA_TX_POWER, LORA_PREAMBLE_LEN);
  if (st != RADIOLIB_ERR_NONE) logAsync("[GW-BULK] LoRa restore err %d\n", st);
  radio.setCRC(true);
  radio.setFrequency(LORA_CHANNELS[0]);
}

// FSK receive window -- no digitalRead(DIO0) fallback: DIO0 fires PayloadReady
// in FSK mode, so radio.available() is reliable.  The LoRa-specific GPIO
// fallback in rxWindow() would give false positives in FSK mode.
static int16_t rxWindowFsk(uint8_t* buf, size_t maxLen, uint32_t windowMs) {
  uint32_t deadline = millis() + windowMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (radio.available()) {
      int len = radio.getPacketLength();
      if (len > 0 && (size_t)len <= maxLen) {
        int16_t st = radio.readData(buf, (size_t)len);
        if (st == RADIOLIB_ERR_NONE) return (int16_t)len;
      }
      return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return -1;
}

// GFSK stop-and-wait sender (DL: gateway -> node).
// Called from Zone 4 with radio already in GFSK mode.
// g_bulkSession must be BULK_ACTIVE on entry.
static void runBulkSend() {
  BulkSession* bs = &g_bulkSession;
  uint16_t offset = 0;
  uint8_t  seqNum = 0;
  uint16_t total  = bs->totalLen;

  bs->fragsSent  = 0;
  bs->fragsAcked = 0;

  uint8_t  fragBuf[BULK_FRAG_TOTAL];
  uint8_t  ackBuf[sizeof(BulkAckPacket)];
  uint32_t deadline = millis() + BULK_MAX_WINDOW_MS;

  while (offset < total) {
    if ((int32_t)(deadline - millis()) <= 0) {
      logAsync("[GW-BULK] TX window expired offset=%u\n", offset);
      bs->state = BULK_FAILED;
      return;
    }

    uint16_t chunkLen = total - offset;
    if (chunkLen > BULK_FRAG_PAYLOAD) chunkLen = BULK_FRAG_PAYLOAD;
    bool isLast = (offset + chunkLen >= total);

    BulkFragHeader* hdr = reinterpret_cast<BulkFragHeader*>(fragBuf);
    hdr->pktType    = PKT_BULK_FRAG;
    hdr->seqNum     = seqNum;
    hdr->payloadLen = (uint8_t)chunkLen;
    hdr->flags      = isLast ? 0x01 : 0x00;
    memcpy(fragBuf + BULK_FRAG_HEADER_LEN, bs->buf + offset, chunkLen);

    // Encode seqNum into high byte of synthetic sfCount for per-fragment nonce uniqueness.
    uint16_t synth_sf = (uint16_t)(((uint16_t)seqNum << 8) | (g_sfCount & 0xFF));
    uint8_t  nodeId   = g_nodes[bs->slotIdx].slotId;
    pktEncrypt(fragBuf + BULK_FRAG_HEADER_LEN, chunkLen, synth_sf, nodeId, PKT_DIR_BULK_DL);

    bool acked = false;
    for (uint8_t retry = 0; retry <= BULK_MAX_RETRIES && !acked; retry++) {
      bs->fragsSent++;
      int16_t st = radio.transmit(fragBuf, (size_t)(BULK_FRAG_HEADER_LEN + chunkLen));
      if (st != RADIOLIB_ERR_NONE) {
        logAsync("[GW-BULK] TX err %d seq=%d\n", st, seqNum);
        continue;
      }
      radio.startReceive();
      int16_t rxLen = rxWindowFsk(ackBuf, sizeof(ackBuf), BULK_ACK_TIMEOUT_MS);
      if (rxLen == (int16_t)sizeof(BulkAckPacket)) {
        const BulkAckPacket* ack = reinterpret_cast<const BulkAckPacket*>(ackBuf);
        if (ack->seqNum == seqNum && ack->pktType == PKT_BULK_ACK) {
          acked = true;
          bs->fragsAcked++;
        }
      }
    }

    if (!acked) {
      logAsync("[GW-BULK] no ACK after retries seq=%d\n", seqNum);
      bs->state = BULK_FAILED;
      return;
    }

    offset += chunkLen;
    seqNum++;
  }

  bs->pdrTotal      = bs->fragsSent;
  bs->pdrAcked      = bs->fragsAcked;
  bs->crc32Received = bs->crc32Expected;  // DL: ACK protocol guarantees delivery
  bs->state = BULK_COMPLETE;
  logAsync("[GW-BULK] DL done: %u B, %u/%u frags\n",
           total, bs->fragsAcked, bs->fragsSent);
}

// GFSK stop-and-wait receiver (UL: node -> gateway).
// Called from Zone 4 with radio already in GFSK mode.
// g_bulkSession must be BULK_ACTIVE on entry.
static void runBulkReceive() {
  BulkSession* bs = &g_bulkSession;
  uint16_t bufOff = 0;
  uint8_t  expSeq = 0;

  bs->bufFilled  = 0;
  bs->fragsAcked = 0;
  bs->fragsSent  = 0;

  uint8_t  fragBuf[BULK_FRAG_TOTAL];
  uint8_t  ackBuf[sizeof(BulkAckPacket)];
  uint32_t deadline = millis() + BULK_MAX_WINDOW_MS;

  radio.startReceive();

  while (true) {
    if ((int32_t)(deadline - millis()) <= 0) {
      logAsync("[GW-BULK] RX window expired seq=%d\n", expSeq);
      bs->state = BULK_FAILED;
      return;
    }

    int16_t rxLen = rxWindowFsk(fragBuf, sizeof(fragBuf), BULK_ACK_TIMEOUT_MS * 2);
    if (rxLen < (int16_t)(BULK_FRAG_HEADER_LEN + 1)) {
      // Timeout or truncated -- restart receive and retry within deadline.
      radio.startReceive();
      continue;
    }

    const BulkFragHeader* hdr = reinterpret_cast<const BulkFragHeader*>(fragBuf);
    uint8_t* payload = fragBuf + BULK_FRAG_HEADER_LEN;

    if (hdr->pktType != PKT_BULK_FRAG || hdr->seqNum != expSeq ||
        hdr->payloadLen == 0 || hdr->payloadLen > BULK_FRAG_PAYLOAD) {
      BulkAckPacket nack;
      nack.seqNum  = expSeq;
      nack.pktType = PKT_BULK_NACK;
      memcpy(ackBuf, &nack, sizeof(nack));
      radio.transmit(ackBuf, sizeof(ackBuf));
      radio.startReceive();
      continue;
    }

    uint16_t synth_sf = (uint16_t)(((uint16_t)hdr->seqNum << 8) | (g_sfCount & 0xFF));
    uint8_t  nodeId   = g_nodes[bs->slotIdx].slotId;
    pktDecrypt(payload, hdr->payloadLen, synth_sf, nodeId, PKT_DIR_BULK_UL);

    if (bufOff + hdr->payloadLen <= BULK_MAX_PAYLOAD) {
      memcpy(bs->buf + bufOff, payload, hdr->payloadLen);
      bufOff += hdr->payloadLen;
    }
    bs->fragsAcked++;

    BulkAckPacket ack;
    ack.seqNum  = hdr->seqNum;
    ack.pktType = PKT_BULK_ACK;
    memcpy(ackBuf, &ack, sizeof(ack));
    radio.transmit(ackBuf, sizeof(ackBuf));

    if (hdr->flags & 0x01) break;  // LAST_FRAG
    expSeq++;
    radio.startReceive();
  }

  bs->pdrTotal      = bs->fragsSent;
  bs->pdrAcked      = bs->fragsAcked;
  bs->bufFilled     = bufOff;
  bs->crc32Received = crc32_calc(bs->buf, bufOff);
  bs->state         = BULK_COMPLETE;
  logAsync("[GW-BULK] UL done: %u B, %u frags, CRC=0x%08X\n",
           bufOff, bs->fragsAcked, bs->crc32Received);
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

  // Epoch-driven registration: a registered node whose s_joinEpoch matches the
  // current epoch holds a valid slot assignment, so UL UID should always agree.
  // Log a mismatch as a diagnostic but do not mutate state; the node will see the
  // epoch change in the next beacon and re-contend on its own.
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
  // seqCounter[7] = dlAck (node decoded a DL this superframe); [6:0] = rolling counter
  bool dlAck     = (pkt.seqCounter & 0x80) != 0;
  ns->seqLast    = pkt.seqCounter & 0x7F;

  accumulateEnergy(ns, pkt.energy);
  addHistory(ns, pkt);

  // Confirm pending if the node explicitly ACKed the DL this superframe.
  // Belt-and-suspenders: also confirm relay/schedule/clear via state match in case
  // the node is running old firmware without the dlAck bit.
#define MAX_DL_RETRIES 3
  if (ns->pending) {
    bool confirmed = dlAck;
    if (!confirmed) {
      if (ns->queuedCmd.len >= 3 && ns->queuedCmd.data[0] == PKT_RELAY_MANUAL)
        confirmed = (ns->relayState == ns->queuedCmd.data[2]);
      else if (ns->queuedCmd.len >= 7 && ns->queuedCmd.data[0] == PKT_RELAY_SCHEDULE)
        confirmed = (ns->relayMode == 1 && ns->schedState > 0);
      else if (ns->queuedCmd.len >= 2 && ns->queuedCmd.data[0] == PKT_RELAY_CLEAR)
        confirmed = (ns->relayMode == 0 && ns->schedState == 0);
    }
    if (confirmed) {
      ns->pending      = false;
      ns->pendingRetry = 0;
    } else if (!ns->queuedCmd.active && ns->pendingRetry < MAX_DL_RETRIES) {
      // DL not confirmed -- re-arm the same command bytes for retransmission.
      ns->queuedCmd.active = true;
      ns->pendingRetry++;
      logAsync("[GW-DL] Retry %d slot%d type=0x%02X\n",
               ns->pendingRetry, slotIdx + 1, ns->queuedCmd.data[0]);
    }
  }
  // Clear pending on timeout regardless of command type or retry state
  if (ns->pending && (millis() - ns->pendingSentAt) >= PENDING_TIMEOUT_MS) {
    ns->pending      = false;
    ns->pendingRetry = 0;
  }

  // STATUS_BULK_READY (bit 7) -- node acknowledged the bulk grant
  if ((pkt.statusByte & STATUS_BULK_READY) != 0) {
    if (g_bulkSession.state == BULK_GRANT_SENT && g_bulkSession.slotIdx == slotIdx) {
      g_bulkSession.acked = true;
      g_bulkSession.state = BULK_ACTIVE;
    }
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
// Downlink TX -- called for every occupied slot; skips TX when nothing is
// queued.  Priority 1: relay/threshold/nudge commands (queuedCmd).
// Priority 2: bulk grant (bulkGrantPending), sent only when queuedCmd is idle.
// The caller's waitUntilMs(slotBase + SLOT_DL_MS) preserves UL timing regardless.
//
// Must be called with g_nodesMutex held.
// -----------------------------------------------------------------------------
static void sendDownlink(uint8_t slotIdx) {
  NodeState* ns = &g_nodes[slotIdx];

  uint8_t txBuf[MAX_DL_PAYLOAD_LEN];
  memset(txBuf, 0, MAX_DL_PAYLOAD_LEN);

  if (ns->queuedCmd.active && ns->queuedCmd.len >= 2) {
    // Priority 1: relay / threshold / nudge command
    memcpy(txBuf, ns->queuedCmd.data, ns->queuedCmd.len);
    uint8_t pktType = ns->queuedCmd.data[0];
    pktEncrypt(txBuf, MAX_DL_PAYLOAD_LEN, g_sfCount, (uint8_t)(slotIdx + 1), PKT_DIR_DL);
    int16_t st = radio.transmit(txBuf, MAX_DL_PAYLOAD_LEN);
    if (st == RADIOLIB_ERR_NONE) {
      ns->pendingSentAt    = millis();
      ns->queuedCmd.active = false;
      logAsync("[GW-DL] Slot%d type=0x%02X\n", slotIdx + 1, pktType);
    } else {
      logAsync("[GW-DL] TX error %d slot%d\n", st, slotIdx + 1);
    }
  } else if (ns->bulkGrantPending) {
    // Priority 2: bulk grant (6 bytes, zero-padded to MAX_DL_PAYLOAD_LEN)
    memcpy(txBuf, ns->bulkGrantBuf, sizeof(BulkGrantPacket));
    pktEncrypt(txBuf, MAX_DL_PAYLOAD_LEN, g_sfCount, (uint8_t)(slotIdx + 1), PKT_DIR_DL);
    int16_t st = radio.transmit(txBuf, MAX_DL_PAYLOAD_LEN);
    if (st == RADIOLIB_ERR_NONE) {
      ns->bulkGrantPending    = false;
      g_bulkSession.state     = BULK_GRANT_SENT;
      g_bulkSession.grantSentAt = millis();
      logAsync("[GW-DL] Slot%d BulkGrant dir=%d len=%u\n",
               slotIdx + 1, g_bulkSession.dir, g_bulkSession.totalLen);
    } else {
      logAsync("[GW-DL] BulkGrant TX err %d slot%d\n", st, slotIdx + 1);
    }
  }
}

// -----------------------------------------------------------------------------
// Evict stale nodes -- runs only when all slots are occupied (g_slotMask == 0xFF).
// When free slots exist there is no eviction pressure; stale nodes keep their
// slot so they can rejoin without re-contending if they come back.
// When the network is full, any node that has exceeded NODE_TIMEOUT_SFS
// consecutive missed superframes is evicted to open a slot for the next join.
// Active nodes (UL every SF) always have missedSfs == 0.
// -----------------------------------------------------------------------------
static void evictStaleNodes() {
  if (g_slotMask != 0xFF) return;  // free slots exist -- nothing to reclaim

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

  bool anyEvicted = false;
  for (uint8_t i = 0; i < MAX_NODES; i++) {
    NodeState* ns = &g_nodes[i];
    if (!ns->active) continue;
    if (ns->missedSfs < NODE_TIMEOUT_SFS) continue;

    logAsync("[GW-TDMA] Slot %d UID=0x%04X evicted (%d missed SFs)\n",
             ns->slotId, ns->deviceUID, ns->missedSfs);

    framQueueSave(i, true, true);
    ns->active    = false;
    ns->hasData   = false;
    ns->missedSfs = 0;
    ns->label[0]  = '\0';  // clear so next joiner doesn't inherit a stale label
    g_slotMask   &= ~(1u << i);
    anyEvicted = true;
  }
  if (anyEvicted) g_networkEpoch++;  // one increment signals all re-contention needed

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
      // Check if already registered (node reboot case -- slot still active)
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

      // Frequency hop -- same channel for both DL and UL of the pair.
      uint8_t ch = hopChannel(g_sfCount, s + 1);
      setChannel(ch);

      // -- DL transmit window (first) --------------------------------
      // Transmit only when a command is queued; idle slots skip TX entirely.
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

    // -- Zone 4: Idle window + end guard ---------------------------------
    uint32_t idleStart = millis();
    evictStaleNodes();

    bool doBulk = (g_bulkSession.state == BULK_ACTIVE && g_bulkSession.acked);
    if (doBulk) {
      waitUntilMs(idleStart + BULK_MAINT_MS);
      if (switchToGfsk()) {
        vTaskDelay(pdMS_TO_TICKS(BULK_SWITCH_MS));
        if (g_bulkSession.dir == 0) {
          runBulkSend();
          // Echo (typeTag 0xF0): node bounces DL data back as UL in the same
          // GFSK window.  Restore crc32Expected so the CRC comparison is valid.
          if (g_bulkSession.state == BULK_COMPLETE &&
              g_bulkSession.typeTag == 0xF0) {
            uint32_t savedCrc = g_bulkSession.crc32Expected;
            radio.startReceive();
            runBulkReceive();
            g_bulkSession.crc32Expected = savedCrc;
          }
        } else {
          runBulkReceive();
        }
        vTaskDelay(pdMS_TO_TICKS(BULK_RESTORE_MS));
      } else {
        g_bulkSession.state = BULK_FAILED;
      }
      switchToLora();
      webBroadcastBulkEvent();
    } else if (g_bulkSession.state == BULK_GRANT_SENT) {
      // Grant retry: re-arm if node did not respond within 2 superframes
      if ((millis() - g_bulkSession.grantSentAt) > (uint32_t)SUPERFRAME_MS * 2) {
        if (g_bulkSession.grantRetries < BULK_GRANT_RETRIES) {
          g_bulkSession.grantRetries++;
          if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_nodes[g_bulkSession.slotIdx].bulkGrantPending = true;
            g_bulkSession.state = BULK_GRANT_QUEUED;
            xSemaphoreGive(g_nodesMutex);
          }
        } else {
          g_bulkSession.state = BULK_FAILED;
          webBroadcastBulkEvent();
        }
      }
    }

    if (g_sfCount % 10 == 0) {
      logAsync("[GW-TDMA] idle ~%ld ms\n",
               (long)(sfStart + SUPERFRAME_MS - END_GUARD_MS) - (long)millis());
    }
    waitUntilMs(sfStart + SUPERFRAME_MS);
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
      ns->pendingRetry  = 0;
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

bool bulkEnqueue(uint8_t slotIdx, uint8_t dir, uint8_t typeTag,
                 const uint8_t* payload, uint16_t len) {
  if (slotIdx >= MAX_NODES) return false;
  if (len == 0 || len > BULK_MAX_PAYLOAD) return false;
  // Reject if a session is in progress (not idle / terminal)
  BulkSessionState_t cur = g_bulkSession.state;
  if (cur != BULK_IDLE && cur != BULK_COMPLETE && cur != BULK_FAILED) return false;

  bool ok = false;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    NodeState* ns = &g_nodes[slotIdx];
    if (ns->active) {
      // Pre-format the grant packet into NodeState for sendDownlink()
      BulkGrantPacket* grant = reinterpret_cast<BulkGrantPacket*>(ns->bulkGrantBuf);
      grant->pktType     = PKT_BULK_GRANT;
      grant->nodeId      = ns->slotId;
      grant->bulkDir     = dir;
      grant->bulkType    = typeTag;
      grant->totalLen_lo = (uint8_t)(len & 0xFF);
      grant->totalLen_hi = (uint8_t)(len >> 8);
      ns->bulkGrantPending = true;

      // Initialise session state
      g_bulkSession.state        = BULK_GRANT_QUEUED;
      g_bulkSession.slotIdx      = slotIdx;
      g_bulkSession.dir          = dir;
      g_bulkSession.typeTag      = typeTag;
      g_bulkSession.totalLen     = len;
      g_bulkSession.buf          = s_bulkBuf;
      g_bulkSession.bufFilled    = 0;
      g_bulkSession.acked        = false;
      g_bulkSession.grantRetries = 0;
      g_bulkSession.grantSentAt  = 0;
      g_bulkSession.fragsSent    = 0;
      g_bulkSession.fragsAcked   = 0;
      g_bulkSession.crc32Received = 0;
      g_bulkSession.testStartMs  = millis();
      g_bulkSession.pdrTotal     = 0;
      g_bulkSession.pdrAcked     = 0;

      if (dir == 0 && payload != nullptr) {
        memcpy(s_bulkBuf, payload, len);
        g_bulkSession.crc32Expected = crc32_calc(s_bulkBuf, len);
      } else {
        g_bulkSession.crc32Expected = 0;
      }

      ok = true;
    }
    xSemaphoreGive(g_nodesMutex);
  }
  return ok;
}

void bulkAbort() {
  // Write FAILED first so Core 1 sees it immediately on next iteration
  g_bulkSession.state = BULK_FAILED;
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (g_bulkSession.slotIdx < MAX_NODES) {
      g_nodes[g_bulkSession.slotIdx].bulkGrantPending = false;
    }
    xSemaphoreGive(g_nodesMutex);
  }
}
