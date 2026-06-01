/**
 * node_irq_tdma.cpp
 *
 * Interrupt-driven TDMA state machine for STM32 single-core targets.
 * Only compiled when TDMA_IRQ_DRIVEN is defined (node_stm32_*_irq environments).
 *
 * Shared code (globals, LED, PZEM task, handleDownlink, buildTelemetryPacket,
 * GFSK bulk, schedule RTC) lives in node_tdma_common.cpp.
 *
 * Event sources (all post TdmaEvt_t into s_tdmaEvtQueue from ISR context):
 *   DIO0 ISR  -> EVT_DIO0        (RxDone or TxDone depending on s_awaitTxDone)
 *   DIO1 ISR  -> EVT_DIO1        (RxTimeout)
 *   TIM5 ISR  -> EVT_SLOT_TIMER  (slot boundary or listen timeout via halTimerOnce)
 *
 * Uplink TX uses radio.startTransmit() (non-blocking). TxDone is signalled
 * by DIO0 firing EVT_DIO0 with s_awaitTxDone == true.
 */
#ifdef TDMA_IRQ_DRIVEN

#include "node_tdma_common.h"
#include "log_async.h"
#include "hal/hal_sys.h"
#include "hal/hal_rtos.h"
#include "hal/hal_timer.h"
#include <RadioLib.h>

extern SX1278 radio;

// =============================================================================
// Event queue
// =============================================================================

typedef enum : uint8_t {
  EVT_DIO0,        // RxDone (s_awaitTxDone==false) or TxDone (s_awaitTxDone==true)
  EVT_DIO1,        // RxTimeout
  EVT_SLOT_TIMER,  // hardware timer fired
} TdmaEvtType_t;

typedef struct { TdmaEvtType_t type; } TdmaEvt_t;

static QueueHandle_t s_tdmaEvtQueue = nullptr;

// =============================================================================
// Task-local timing state
// =============================================================================

static uint32_t s_beaconReceiveTime = 0;
static uint16_t s_sfCount           = 0;
static uint8_t  s_joinEpoch         = 0xFF;
static uint8_t  s_lastBeaconEpoch   = 0xFF;

// TX completion flag: disambiguates DIO0 RxDone vs TxDone
static volatile bool s_awaitTxDone = false;

// Pending TX buffer (filled before startTransmit)
static uint8_t s_txBuf[sizeof(TelemetryPacket)];

// =============================================================================
// ISR callbacks -- post event only; no RadioLib, no blocking
// =============================================================================

static void onDio0();
static void onDio1();

static void onDio0() {
  TdmaEvt_t evt = { EVT_DIO0 };
  BaseType_t wake = pdFALSE;
  xQueueSendFromISR(s_tdmaEvtQueue, &evt, &wake);
  portYIELD_FROM_ISR(wake);
}

static void onDio1() {
  TdmaEvt_t evt = { EVT_DIO1 };
  BaseType_t wake = pdFALSE;
  xQueueSendFromISR(s_tdmaEvtQueue, &evt, &wake);
  portYIELD_FROM_ISR(wake);
}

static void onSlotTimer() {
  TdmaEvt_t evt = { EVT_SLOT_TIMER };
  BaseType_t wake = pdFALSE;
  xQueueSendFromISR(s_tdmaEvtQueue, &evt, &wake);
  portYIELD_FROM_ISR(wake);
}

// =============================================================================
// State machine
// =============================================================================

typedef enum {
  ST_LISTEN,
  ST_AWAIT_DL,
  ST_DL_OPEN,
  ST_UL_TX,
  ST_CONTENDING,
  ST_JOIN_WAIT,
} TdmaIrqState_t;

// ---------------------------------------------------------------------------
// Enter listen state: arm TIM5 timeout, open Ch0 RX
// ---------------------------------------------------------------------------

static void enterListen(TdmaIrqState_t* state) {
  *state = ST_LISTEN;
  radio.setFrequency(LORA_CHANNELS[0]);
  radio.standby();
  radio.implicitHeader(sizeof(BeaconPacket));
  radio.startReceive();

  uint32_t listenTimeoutUs = g_nodeRegistered
    ? (uint32_t)(SUPERFRAME_MS + 200) * 1000u
    : 10000u * 1000u;
  halTimerOnce(listenTimeoutUs, onSlotTimer);
}

// ---------------------------------------------------------------------------
// EVT_DIO0 in ST_LISTEN: beacon received
// ---------------------------------------------------------------------------

static void onBeaconReceived(TdmaIrqState_t* state) {
  halTimerCancel();

  uint8_t buf[sizeof(BeaconPacket) + 4];
  int len = radio.getPacketLength();
  if (len != (int)sizeof(BeaconPacket)) { enterListen(state); return; }
  if (radio.readData(buf, (size_t)len) != RADIOLIB_ERR_NONE) { enterListen(state); return; }

  BeaconPacket bcn;
  if (!pktReceiveBeacon(buf, (size_t)len, bcn) || bcn.pktType != PKT_BEACON) {
    logAsync("[NODE-TDMA] Unexpected pkt len=%d - still listening\n", len);
    radio.standby();
    radio.implicitHeader(sizeof(BeaconPacket));
    radio.startReceive();
    return;
  }

  s_beaconReceiveTime = millis();
  s_sfCount           = bcn.sfCount;
  s_lastBeaconEpoch   = bcn.epoch;
  tdmaSetBeaconRSSI((int8_t)radio.getRSSI());

  rtcConditionalSync(bcn.hour, bcn.minute, bcn.second);
  evaluateSchedule();

  if (g_nodeRegistered) {
    if (bcn.epoch != s_joinEpoch) {
      logAsync("[NODE-TDMA] Epoch changed (%d->%d) - re-registering\n",
               s_joinEpoch, bcn.epoch);
      g_nodeRegistered = false;
      g_nodeSlotId     = 0;
      s_joinEpoch      = 0xFF;
    } else if (!(bcn.slotMask & (1u << (g_nodeSlotId - 1)))) {
      g_nodeRegistered = false;
      g_nodeSlotId     = 0;
      s_joinEpoch      = 0xFF;
    }
  }

  if (!g_nodeRegistered) {
    *state = ST_CONTENDING;
    tdmaSetLedMode(LED_MODE_CONTENDING);
    uint32_t cwStart = s_beaconReceiveTime
                       + (BEACON_MS - BEACON_AIR_MS)
                       + (uint32_t)MAX_NODES * SLOT_PAIR_MS;
    int32_t delayMs = (int32_t)(cwStart - millis());
    halTimerOnce((uint32_t)(delayMs < 1 ? 1 : delayMs) * 1000u, onSlotTimer);
    return;
  }

  *state = ST_AWAIT_DL;
  tdmaSetLedMode(LED_MODE_REGISTERED);
  uint32_t dlStart = s_beaconReceiveTime
                     + (BEACON_MS - BEACON_AIR_MS)
                     + (uint32_t)(g_nodeSlotId - 1) * SLOT_PAIR_MS;
  int32_t delayMs = (int32_t)(dlStart - millis());
  halTimerOnce((uint32_t)(delayMs < 1 ? 1 : delayMs) * 1000u, onSlotTimer);
}

// ---------------------------------------------------------------------------
// EVT_SLOT_TIMER in ST_AWAIT_DL: open DL RX window
// ---------------------------------------------------------------------------

static void openDlWindow(TdmaIrqState_t* state) {
  *state = ST_DL_OPEN;
  uint8_t ch = hopChannel(s_sfCount, g_nodeSlotId);
  radio.standby();
  radio.setFrequency(LORA_CHANNELS[ch]);
  delayMicroseconds(PHASE_GUARD_US);
  radio.implicitHeader(MAX_DL_PAYLOAD_LEN);
  radio.startReceive();
  halTimerOnce((uint32_t)SLOT_DL_MS * 1000u, onSlotTimer);
}

// ---------------------------------------------------------------------------
// Close DL window and issue non-blocking UL startTransmit
// ---------------------------------------------------------------------------

static void startUlTx(TdmaIrqState_t* state) {
  halTimerCancel();
  *state = ST_UL_TX;

  TelemetryPacket pkt;
  buildTelemetryPacket(pkt);
  memcpy(s_txBuf, &pkt, sizeof(pkt));
  pktEncrypt(s_txBuf, sizeof(s_txBuf), s_sfCount, g_nodeSlotId, PKT_DIR_UL);

  logAsync("[NODE-TX] V=%.1f A=%.3f W=%.1f seq=%d\n",
           g_meter.voltage, g_meter.current, g_meter.power,
           pkt.seqCounter & 0x7F);

  s_awaitTxDone = true;
  int16_t st = radio.startTransmit(s_txBuf, sizeof(s_txBuf));
  if (st != RADIOLIB_ERR_NONE) {
    logAsync("[NODE-TX] startTransmit err %d\n", st);
    s_awaitTxDone = false;
    enterListen(state);
  }
}

// ---------------------------------------------------------------------------
// EVT_DIO0 in ST_UL_TX: TxDone
// ---------------------------------------------------------------------------

static void onTxDone(TdmaIrqState_t* state) {
  s_awaitTxDone = false;
  radio.finishTransmit();

  if (tdmaHasBulkPending()) {
    tdmaRunBulkIdle(s_sfCount, s_beaconReceiveTime);
    // beginFSK inside tdmaRunBulkIdle may clear RadioLib DIO handlers; re-attach.
    radio.setDio0Action(onDio0, RISING);
    radio.setDio1Action(onDio1, RISING);
  }

  enterListen(state);
}

// ---------------------------------------------------------------------------
// ST_CONTENDING: contention window timer fired -> send JoinRequest
// ---------------------------------------------------------------------------

static void sendJoinRequest(TdmaIrqState_t* state) {
  vTaskDelay(pdMS_TO_TICKS(halRandom() % 25));

  JoinRequestPacket req;
  req.pktType   = PKT_JOIN_REQUEST;
  req.uid_lo    = (uint8_t)(g_nodeUID & 0xFF);
  req.uid_hi    = (uint8_t)(g_nodeUID >> 8);
  req.fwVersion = FW_VERSION;

  radio.setFrequency(LORA_CHANNELS[0]);
  uint8_t joinBuf[sizeof(JoinRequestPacket)];
  memcpy(joinBuf, &req, sizeof(req));
  pktEncrypt(joinBuf, sizeof(joinBuf), s_sfCount, 0, PKT_DIR_JOIN_RQ);

  // JoinRequest is 4 bytes (~16 ms airtime); blocking transmit is acceptable
  // in the contention window and avoids an extra state for TxDone.
  int16_t st = radio.transmit(joinBuf, sizeof(joinBuf));
  if (st != RADIOLIB_ERR_NONE) {
    logAsync("[NODE-JOIN] TX failed %d\n", st);
    enterListen(state);
    return;
  }
  logAsync("[NODE-JOIN] Sent JoinReq UID=0x%04X\n", g_nodeUID);

  *state = ST_JOIN_WAIT;
  radio.implicitHeader(sizeof(JoinAckPacket));
  radio.startReceive();
  halTimerOnce((uint32_t)CONTENTION_DL_MS * 1000u, onSlotTimer);
}

// ---------------------------------------------------------------------------
// ST_JOIN_WAIT: JoinAck received
// ---------------------------------------------------------------------------

static void onJoinAck(TdmaIrqState_t* state) {
  halTimerCancel();

  uint8_t ackBuf[8];
  int len = radio.getPacketLength();
  if (len != (int)sizeof(JoinAckPacket)) { enterListen(state); return; }
  if (radio.readData(ackBuf, (size_t)len) != RADIOLIB_ERR_NONE) { enterListen(state); return; }

  JoinAckPacket ack;
  if (!pktReceive(ackBuf, (size_t)len, ack, s_sfCount, 0, PKT_DIR_JOIN_AK) ||
      ack.pktType != PKT_JOIN_ACK) {
    enterListen(state); return;
  }
  uint16_t echoed = ((uint16_t)ack.uid_hi << 8) | ack.uid_lo;
  if (echoed == g_nodeUID && ack.slotId >= 1 && ack.slotId <= MAX_NODES) {
    g_nodeSlotId     = ack.slotId;
    g_nodeRegistered = true;
    s_joinEpoch      = s_lastBeaconEpoch;
    logAsync("[NODE-JOIN] Registered! slotId=%d epoch=%d\n",
             g_nodeSlotId, s_joinEpoch);
  } else {
    logAsync("[NODE-JOIN] UID mismatch or invalid slotId\n");
  }
  enterListen(state);
}

// =============================================================================
// TDMA processor task (event-driven)
// =============================================================================

static void nodeTdmaIrqTask(void* /*params*/) {
  logAsync("[NODE-TDMA] IRQ-driven task started\n");

  TdmaIrqState_t state = ST_LISTEN;
  enterListen(&state);

  TdmaEvt_t evt;
  while (true) {
    if (xQueueReceive(s_tdmaEvtQueue, &evt, portMAX_DELAY) != pdTRUE) continue;

    switch (state) {

    case ST_LISTEN:
      if (evt.type == EVT_DIO0 && !s_awaitTxDone) {
        onBeaconReceived(&state);
      } else if (evt.type == EVT_SLOT_TIMER) {
        if (g_nodeRegistered) {
          logAsync("[NODE-TDMA] Beacon timeout - re-listening\n");
          g_nodeRegistered = false;
          g_nodeSlotId     = 0;
          s_joinEpoch      = 0xFF;
        } else {
          logAsync("[NODE-TDMA] No beacon heard - listening...\n");
        }
        tdmaSetLedMode(LED_MODE_LISTEN);
        enterListen(&state);
      }
      break;

    case ST_AWAIT_DL:
      if (evt.type == EVT_SLOT_TIMER) openDlWindow(&state);
      break;

    case ST_DL_OPEN:
      if (evt.type == EVT_DIO0 && !s_awaitTxDone) {
        halTimerCancel();
        uint8_t dlBuf[16];
        int len = radio.getPacketLength();
        if (len > 0 && (size_t)len <= sizeof(dlBuf)) {
          if (radio.readData(dlBuf, (size_t)len) == RADIOLIB_ERR_NONE) {
            pktDecrypt(dlBuf, (size_t)len, s_sfCount, g_nodeSlotId, PKT_DIR_DL);
            handleDownlink(dlBuf, (int16_t)len);
          }
        }
        g_nodeRegistered ? startUlTx(&state) : enterListen(&state);
      } else if (evt.type == EVT_DIO1 || evt.type == EVT_SLOT_TIMER) {
        startUlTx(&state);
      }
      break;

    case ST_UL_TX:
      if (evt.type == EVT_DIO0 && s_awaitTxDone) onTxDone(&state);
      break;

    case ST_CONTENDING:
      if (evt.type == EVT_SLOT_TIMER) sendJoinRequest(&state);
      break;

    case ST_JOIN_WAIT:
      if (evt.type == EVT_DIO0 && !s_awaitTxDone) {
        onJoinAck(&state);
      } else if (evt.type == EVT_DIO1 || evt.type == EVT_SLOT_TIMER) {
        logAsync("[NODE-JOIN] No ACK - retrying next superframe\n");
        tdmaSetLedMode(LED_MODE_LISTEN);
        enterListen(&state);
      }
      break;
    }
  }
}

// =============================================================================
// Task launcher
// =============================================================================

void nodeTdmaTaskStart() {
  s_tdmaEvtQueue = xQueueCreate(4, sizeof(TdmaEvt_t));
  configASSERT(s_tdmaEvtQueue);

  nodeTdmaStartCommonTasks();

  radio.setDio0Action(onDio0, RISING);
  radio.setDio1Action(onDio1, RISING);

  halTaskCreatePinned(nodeTdmaIrqTask, "NODE_TDMA_IRQ", 8192, nullptr, 2, HAL_CORE_1, nullptr);
}

#endif // TDMA_IRQ_DRIVEN
