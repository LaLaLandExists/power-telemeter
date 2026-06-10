/**
 * node_tdma_task.cpp
 *
 * Task-based TDMA state machine for ESP32 and non-IRQ STM32 builds.
 * Excluded when TDMA_IRQ_DRIVEN is defined.
 *
 * Shared code (globals, LED, PZEM task, handleDownlink, buildTelemetryPacket,
 * GFSK bulk, schedule RTC) lives in node_tdma_common.cpp.
 *
 * State machine:
 *   BOOT -> LISTEN -> CONTENDING -> REGISTERED -> (repeat via LISTEN)
 *
 * Timing model (DL-first slot ordering):
 *   - beaconReceiveTime = millis() when beacon lands.
 *   - dlStart = beaconReceiveTime + (BEACON_MS - BEACON_AIR_MS) + (slotId-1) x SLOT_PAIR_MS
 *   - txTime  = dlStart + SLOT_DL_MS
 */
#ifndef TDMA_IRQ_DRIVEN

#include "node_tdma_common.h"
#include "log_async.h"
#include "hal/hal_sys.h"
#include "hal/hal_rtos.h"
#include <RadioLib.h>

extern SX1278 radio;
extern uint8_t DIO0_PIN;

// =============================================================================
// RX window helper
// =============================================================================

static int16_t rxWindow(uint8_t* buf, size_t maxLen, uint32_t windowMs) {
  uint32_t deadline = millis() + windowMs;
  while ((int32_t)(deadline - millis()) > 0) {
    bool pktReady = radio.available() ||
                    (digitalRead(DIO0_PIN) && radio.getPacketLength() > 0);
    if (pktReady) {
      int len = radio.getPacketLength();
      if (len > 0 && (size_t)len <= maxLen) {
        int16_t st = radio.readData(buf, (size_t)len);
        if (st == RADIOLIB_ERR_NONE) return (int16_t)len;
        logAsync("[NODE] RX err %d len=%d\n", st, len);
      }
      return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return -1;
}

// =============================================================================
// Transmit telemetry (blocking)
// Builds packet via buildTelemetryPacket(), encrypts, then calls radio.transmit().
// =============================================================================

static void transmitTelemetry(uint16_t sfCount) {
  TelemetryPacket pkt;
  buildTelemetryPacket(pkt);

  uint8_t txBuf[sizeof(TelemetryPacket)];
  memcpy(txBuf, &pkt, sizeof(pkt));
  pktEncrypt(txBuf, sizeof(txBuf), sfCount, g_nodeSlotId, PKT_DIR_UL);

  int16_t st = radio.transmit(txBuf, sizeof(txBuf));
  if (st == RADIOLIB_ERR_NONE) {
    logAsync("[NODE-TX] V=%.1f A=%.3f W=%.1f seq=%d\n",
             g_meter.voltage, g_meter.current, g_meter.power,
             pkt.seqCounter & 0x7F);
  } else {
    logAsync("[NODE-TX] Error %d\n", st);
  }
}

// =============================================================================
// TDMA node state machine task
// =============================================================================

typedef enum { ST_LISTEN, ST_CONTENDING, ST_REGISTERED } NodeState_t;

static void nodeTdmaTask(void* /*params*/) {
  logAsync("[NODE-TDMA] Task started on Core 1\n");

  NodeState_t state             = ST_LISTEN;
  uint32_t    beaconReceiveTime = 0;
  uint16_t    sfCount           = 0;

  radio.setFrequency(LORA_CHANNELS[0]);

  while (true) {

    // -- LISTEN ----------------------------------------------------------------
    if (state == ST_LISTEN || state == ST_CONTENDING || state == ST_REGISTERED) {
      uint8_t buf[64];
      radio.setFrequency(LORA_CHANNELS[0]);

      uint32_t listenTimeout  = (state == ST_LISTEN) ? 10000 : SUPERFRAME_MS + 200;
      uint32_t listenDeadline = millis() + listenTimeout;

      BeaconPacket bcn;
      bool isBeacon = false;

      while (true) {
        int32_t remaining = (int32_t)(listenDeadline - millis());
        if (remaining <= 0) break;

        radio.standby();
        radio.implicitHeader(sizeof(BeaconPacket));
        int16_t rxSt = radio.startReceive();
        if (rxSt != RADIOLIB_ERR_NONE) {
          logAsync("[NODE-TDMA] startReceive failed %d\n", rxSt);
        }

        int16_t len = rxWindow(buf, sizeof(buf), (uint32_t)remaining);
        if (len <= 0) break;

        isBeacon = (len == (int16_t)sizeof(BeaconPacket)) &&
                   pktReceiveBeacon(buf, (size_t)len, bcn) &&
                   (bcn.pktType == PKT_BEACON);

        if (isBeacon) break;

        logAsync("[NODE-TDMA] Unexpected pkt len=%d - still listening\n", len);
      }

      if (isBeacon) {
        beaconReceiveTime = millis();
        sfCount           = bcn.sfCount;
        tdmaSetBeaconRSSI((int8_t)radio.getRSSI());

        rtcConditionalSync(bcn.hour, bcn.minute, bcn.second);
        evaluateSchedule();

        // epoch / slot mask checks handled below via local s_joinEpoch
      } else if (state != ST_LISTEN) {
        logAsync("[NODE-TDMA] Beacon timeout - re-listening\n");
        state = ST_LISTEN;
        if (!g_nodeRegistered) tdmaSetLedMode(LED_MODE_LISTEN);
        continue;
      } else {
        logAsync("[NODE-TDMA] No beacon heard - listening...\n");
        continue;
      }

      // Process beacon fields (epoch / slot mask) via local state
      // s_joinEpoch and s_lastBeaconEpoch are local to this task because
      // they are only ever read/written from this single task function.
      static uint8_t s_joinEpoch       = 0xFF;
      static uint8_t s_lastBeaconEpoch = 0xFF;

      s_lastBeaconEpoch = bcn.epoch;

      if (g_nodeRegistered) {
        if (bcn.epoch != s_joinEpoch) {
          logAsync("[NODE-TDMA] Epoch changed (%d->%d) - re-registering\n",
                   s_joinEpoch, bcn.epoch);
          g_nodeRegistered = false;
          g_nodeSlotId     = 0;
          s_joinEpoch      = 0xFF;
          state = ST_CONTENDING;
          tdmaSetLedMode(LED_MODE_CONTENDING);
        } else if (bcn.slotMask & (1u << (g_nodeSlotId - 1))) {
          state = ST_REGISTERED;
          tdmaSetLedMode(LED_MODE_REGISTERED);
        } else {
          g_nodeRegistered = false;
          g_nodeSlotId     = 0;
          s_joinEpoch      = 0xFF;
          state = ST_CONTENDING;
          tdmaSetLedMode(LED_MODE_CONTENDING);
        }
      } else {
        state = ST_CONTENDING;
        tdmaSetLedMode(LED_MODE_CONTENDING);
      }

      // -- CONTENDING --------------------------------------------------------
      if (state == ST_CONTENDING) {
        uint32_t cwStart = beaconReceiveTime
                           + (BEACON_MS - BEACON_AIR_MS)
                           + (uint32_t)MAX_NODES * SLOT_PAIR_MS;

        waitUntilMs(cwStart);
        vTaskDelay((halRandom() % 25) / portTICK_PERIOD_MS);

        JoinRequestPacket req;
        req.pktType   = PKT_JOIN_REQUEST;
        req.uid_lo    = (uint8_t)(g_nodeUID & 0xFF);
        req.uid_hi    = (uint8_t)(g_nodeUID >> 8);
        req.fwVersion = FW_VERSION;

        radio.setFrequency(LORA_CHANNELS[0]);
        uint8_t joinBuf[sizeof(JoinRequestPacket)];
        memcpy(joinBuf, &req, sizeof(req));
        pktEncrypt(joinBuf, sizeof(joinBuf), sfCount, 0, PKT_DIR_JOIN_RQ);
        int16_t joinTxSt = radio.transmit(joinBuf, sizeof(joinBuf));
        if (joinTxSt != RADIOLIB_ERR_NONE) {
          logAsync("[NODE-JOIN] TX failed %d\n", joinTxSt);
        } else {
          logAsync("[NODE-JOIN] Sent JoinReq UID=0x%04X\n", g_nodeUID);
        }

        radio.implicitHeader(sizeof(JoinAckPacket));
        radio.startReceive();
        uint8_t ackBuf[8];
        uint32_t ackDeadline = cwStart + CONTENTION_UL_MS + CONTENTION_DL_MS;
        int16_t  ackLen      = rxWindow(ackBuf, sizeof(ackBuf),
                                        ackDeadline - millis());

        JoinAckPacket ack;
        if (ackLen == (int16_t)sizeof(JoinAckPacket) &&
            pktReceive(ackBuf, (size_t)ackLen, ack, sfCount, 0, PKT_DIR_JOIN_AK) &&
            ack.pktType == PKT_JOIN_ACK) {
          uint16_t echoed = ((uint16_t)ack.uid_hi << 8) | ack.uid_lo;
          if (echoed == g_nodeUID && ack.slotId >= 1 && ack.slotId <= MAX_NODES) {
            g_nodeSlotId     = ack.slotId;
            g_nodeRegistered = true;
            s_joinEpoch      = s_lastBeaconEpoch;
            logAsync("[NODE-JOIN] Registered! slotId=%d epoch=%d\n",
                     g_nodeSlotId, s_joinEpoch);
            state = ST_LISTEN;
            continue;
          }
        }

        logAsync("[NODE-JOIN] No ACK - retrying next superframe\n");
        state = ST_LISTEN;
        tdmaSetLedMode(LED_MODE_LISTEN);
        continue;
      }
    }

    // -- REGISTERED ------------------------------------------------------------
    if (state == ST_REGISTERED) {
      uint8_t  ch      = hopChannel(sfCount, g_nodeSlotId);
      uint32_t dlStart = beaconReceiveTime
                         + (BEACON_MS - BEACON_AIR_MS)
                         + (uint32_t)(g_nodeSlotId - 1) * SLOT_PAIR_MS;
      uint32_t txTime  = dlStart + SLOT_DL_MS;

      waitUntilMs(dlStart);
      radio.setFrequency(LORA_CHANNELS[ch]);
      delayMicroseconds(PHASE_GUARD_US);
      radio.implicitHeader(MAX_DL_PAYLOAD_LEN);
      radio.startReceive();

      uint8_t dlBuf[16];
      int16_t dlLen = rxWindow(dlBuf, sizeof(dlBuf), SLOT_DL_MS);
      if (dlLen > 0) {
        pktDecrypt(dlBuf, (size_t)dlLen, sfCount, g_nodeSlotId, PKT_DIR_DL);
        handleDownlink(dlBuf, dlLen);
      }

      if (!g_nodeRegistered) {
        state = ST_LISTEN;
        continue;
      }

      waitUntilMs(txTime);
      transmitTelemetry(sfCount);
      waitUntilMs(txTime + SLOT_UL_MS + SLOT_GUARD_MS);

      if (tdmaHasBulkPending()) {
        tdmaRunBulkIdle(sfCount, beaconReceiveTime);
        // task-based: no DIO callback re-attachment needed
      }

      state = ST_LISTEN;
    }
  }
}

// =============================================================================
// Task launcher
// =============================================================================

void nodeTdmaTaskStart() {
  nodeTdmaStartCommonTasks();
  halTaskCreatePinned(nodeTdmaTask, "NODE_TDMA", 8192, nullptr, 2, HAL_CORE_1, nullptr);
}

#endif // TDMA_IRQ_DRIVEN
