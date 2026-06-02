/**
 * node_tdma_common.cpp
 *
 * Shared implementation for all node TDMA builds (task-based and IRQ-driven).
 * Compiled unconditionally for every NODE_TELEMETRY environment.
 *
 * What lives here:
 *   - All global variable definitions
 *   - Relay, RTC, schedule evaluation
 *   - LED task + nudge
 *   - Downlink command handler
 *   - Telemetry packet builder (no encryption, no TX)
 *   - GFSK bulk transfer (send + receive) + LoRa restore
 *   - Schedule evaluator task
 *   - nodeTdmaStartCommonTasks()
 *
 * To extend meter data or telemetry:
 *   1. Add fields to MeterData in meter.h
 *   2. Populate the field in the active metering backend (meter_*.cpp)
 *   3. Update buildTelemetryPacket() below (and TelemetryPacket in tdma_protocol.h)
 */
#ifdef NODE_TELEMETRY

#include "node_tdma_common.h"
#include "log_async.h"
#include "hal/hal_sys.h"
#include "hal/hal_rtos.h"
#include <RadioLib.h>

// --- Hardware instances (defined in main.cpp) --------------------------------
extern SX1278 radio;

// --- GPIO pins (defined in main.cpp, NODE_TELEMETRY build) -------------------
extern uint8_t RELAY_PIN;
extern uint8_t LED_GREEN_PIN;
extern uint8_t LED_RED_PIN;

// =============================================================================
// Global variable definitions
// =============================================================================

MeterData         g_meter      = {};
SemaphoreHandle_t g_meterMutex = nullptr;

bool     g_nodeRegistered = false;
uint8_t  g_nodeSlotId     = 0;
uint16_t g_nodeUID        = 0;

// Classifier result byte.  Written by meterTaskFn (METER_AFE builds) via packClassByte()
// at the end of each 2-second FFT cycle.  PZEM and fake builds never write here,
// so CLASS_BYTE_UNSUPPORTED is always transmitted by those builds.
uint8_t  g_classByte      = CLASS_BYTE_UNSUPPORTED;

uint8_t  g_relayState = 0;
uint8_t  g_relayMode  = 0;
uint8_t  g_schedState = 0;
uint8_t  g_schedSH    = 0;
uint8_t  g_schedSM    = 0;
uint8_t  g_schedEH    = 8;
uint8_t  g_schedEM    = 0;

uint32_t g_rtcBaseSec = 0;
uint32_t g_rtcBaseMs  = 0;
bool     g_rtcSet     = false;

// =============================================================================
// Module-private state shared between common functions
// =============================================================================

static uint8_t  s_seqCounter        = 0;
static bool     s_dlAck             = false;
static int8_t   s_beaconRSSI        = -128;

// GFSK bulk transfer
static bool     s_bulkGrantReceived = false;
static uint8_t  s_bulkDir           = 0;     // 0=DL (receive), 1=UL (send)
static uint8_t  s_bulkTypeTag       = 0;
static uint16_t s_bulkTotalLen      = 0;
static bool     s_bulkReady         = false;
static uint8_t  s_nodeBulkBuf[BULK_MAX_PAYLOAD];

// PZEM energy delta tracking
static uint32_t s_lastPzemEnergy    = 0;
static bool     s_pzemEnergyBaseSet = false;

// LED
static volatile LedTdmaMode_t s_tdmaLedMode = LED_MODE_LISTEN;
static TaskHandle_t           s_ledTaskHandle = nullptr;

// =============================================================================
// Relay
// =============================================================================

void setRelay(uint8_t state) {
  g_relayState = state & 1;
#ifndef PZEM_FAKE  // nettest builds have no real relay hardware
  digitalWrite(RELAY_PIN, g_relayState ? HIGH : LOW);
#endif
}

// =============================================================================
// Schedule RTC helpers
// =============================================================================

static void rtcSet(uint8_t h, uint8_t m, uint8_t s) {
  g_rtcBaseSec = (uint32_t)h * 3600u + (uint32_t)m * 60u + s;
  g_rtcBaseMs  = millis();
  g_rtcSet     = true;
}

void rtcConditionalSync(uint8_t h, uint8_t m, uint8_t s) {
  uint32_t beaconSec = (uint32_t)h * 3600u + m * 60u + s;
  if (!g_rtcSet) { rtcSet(h, m, s); return; }
  uint32_t localSec = rtcGetSec();
  int32_t  delta_ms = (int32_t)(localSec - beaconSec) * 1000;
  if (delta_ms >  43200000L) delta_ms -= 86400000L;
  if (delta_ms < -43200000L) delta_ms += 86400000L;
  if (abs(delta_ms) > RTC_CORRECTION_THRESHOLD_MS) {
    rtcSet(h, m, s);
    logAsync("[NODE-RTC] Corrected %+d ms\n", delta_ms);
  }
}

void evaluateSchedule() {
  if (g_relayMode != 1 || g_schedState == 0) return;
  uint16_t nowMins   = (uint16_t)(rtcGetSec() / 60);
  uint16_t startMins = (uint16_t)g_schedSH * 60 + g_schedSM;
  uint16_t endMins   = (uint16_t)g_schedEH * 60 + g_schedEM;
  bool inside;
  if (startMins <= endMins) {
    inside = (nowMins >= startMins && nowMins < endMins);
  } else {
    inside = (nowMins >= startMins || nowMins < endMins);
  }
  uint8_t newState = inside ? 2 : 1;
  if (newState != g_schedState) {
    g_schedState = newState;
    logAsync("[NODE-SCHED] %s (now=%02d:%02d)\n",
             inside ? "ACTIVE" : "WAITING", nowMins / 60, nowMins % 60);
  }
  setRelay(inside ? 1 : 0);
}

// =============================================================================
// LED task
// =============================================================================

static void setLed(bool red, bool green) {
  digitalWrite(LED_RED_PIN,   red   ? LOW : HIGH);
  digitalWrite(LED_GREEN_PIN, green ? LOW : HIGH);
}

void triggerNudge() {
  if (s_ledTaskHandle) xTaskNotifyGive(s_ledTaskHandle);
}

void tdmaSetLedMode(LedTdmaMode_t mode) {
  s_tdmaLedMode = mode;
}

static void ledTask(void* /*params*/) {
  while (true) {
    bool nudged = (ulTaskNotifyTake(pdTRUE, 0) > 0);
    if (!nudged) {
      switch (s_tdmaLedMode) {
      case LED_MODE_LISTEN:
        setLed(true, false);
        nudged = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) > 0);
        setLed(false, false);
        if (!nudged) nudged = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) > 0);
        break;
      case LED_MODE_CONTENDING:
        setLed(false, true);
        nudged = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(125)) > 0);
        setLed(false, false);
        if (!nudged) nudged = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(125)) > 0);
        break;
      case LED_MODE_REGISTERED:
        setLed(g_relayState, true);
        nudged = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) > 0);
        setLed(false, false);
        if (!nudged) nudged = (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1800)) > 0);
        break;
      }
    }
    if (nudged) {
      logAsync("[NODE] Nudge!\n");
      for (int i = 0; i < 30; i++) {
        setLed(i & 1, false);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      setLed(false, false);
      xTaskNotifyStateClear(nullptr);
    }
  }
}

// =============================================================================
// Downlink command handler
// =============================================================================

void handleDownlink(const uint8_t* buf, int16_t len) {
  if (len < 2) return;
  uint8_t type   = buf[0];
  uint8_t nodeId = buf[1];
  if (nodeId != g_nodeSlotId && nodeId != BROADCAST_ADDR) return;

  switch (type) {
  case PKT_RELAY_MANUAL:
    if (len >= 3) {
      g_relayMode  = 0;
      g_schedState = 0;
      setRelay(buf[2]);
      logAsync("[NODE-DL] Relay manual -> %s\n", buf[2] ? "ON" : "OFF");
    }
    break;
  case PKT_RELAY_SCHEDULE:
    if (len >= 7) {
      g_relayMode  = 1;
      g_schedSH    = buf[3];
      g_schedSM    = buf[4];
      g_schedEH    = buf[5];
      g_schedEM    = buf[6];
      g_schedState = 1;
      evaluateSchedule();
      logAsync("[NODE-DL] Schedule set %02d:%02d - %02d:%02d\n",
               g_schedSH, g_schedSM, g_schedEH, g_schedEM);
    }
    break;
  case PKT_RELAY_CLEAR:
    g_relayMode  = 0;
    g_schedState = 0;
    logAsync("[NODE-DL] Schedule cleared\n");
    break;
  case PKT_THRESHOLD:
    if (len >= 4) {
      uint16_t watts = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
      if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_meter.alarmThreshold      = watts;
        g_meter.pendingThresholdW   = watts;
        g_meter.hasPendingThreshold = true;
        xSemaphoreGive(g_meterMutex);
      }
      logAsync("[NODE-DL] Alarm threshold queued -> %d W\n", watts);
    }
    break;
  case PKT_NUDGE:
    triggerNudge();
    break;
  case PKT_BULK_GRANT:
    if (len >= (int16_t)sizeof(BulkGrantPacket)) {
      const BulkGrantPacket* g = reinterpret_cast<const BulkGrantPacket*>(buf);
      s_bulkDir           = g->bulkDir;
      s_bulkTypeTag       = g->bulkType;
      s_bulkTotalLen      = (uint16_t)g->totalLen_lo | ((uint16_t)g->totalLen_hi << 8);
      s_bulkGrantReceived = true;
      s_bulkReady         = true;
      logAsync("[NODE-DL] BulkGrant dir=%d type=0x%02X len=%u\n",
               s_bulkDir, s_bulkTypeTag, s_bulkTotalLen);
    }
    return; // BulkGrant does not set dlAck
  default:
    logAsync("[NODE-DL] Unknown pkt type 0x%02X\n", type);
    return;
  }
  s_dlAck = true;
}

// =============================================================================
// Telemetry packet builder
//
// Fills all fields from g_meter and node state.
// Does NOT encrypt or transmit; caller handles those steps so that task-based
// and IRQ-driven builds can use blocking vs. non-blocking TX respectively.
// =============================================================================

#define PZEM_ENERGY_MAX_WH 9999990UL

void buildTelemetryPacket(TelemetryPacket& pkt) {
  pkt = {};

  if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    pkt.voltage     = (uint16_t)(g_meter.voltage     * 10.0f);
    pkt.current     = (uint32_t)(g_meter.current     * 1000.0f);
    pkt.power       = (uint32_t)(g_meter.power       * 10.0f);
    pkt.frequency   = (uint16_t)(g_meter.frequency   * 10.0f);
    pkt.powerFactor = (uint16_t)(g_meter.powerFactor * 100.0f);
    pkt.alarmThreshold = g_meter.alarmThreshold;

    uint32_t rawEnergy = g_meter.energy;
    uint32_t energyDelta;
    if (!s_pzemEnergyBaseSet) {
      s_lastPzemEnergy    = rawEnergy;
      s_pzemEnergyBaseSet = true;
      energyDelta = 0;
    } else if (rawEnergy >= s_lastPzemEnergy) {
      energyDelta = rawEnergy - s_lastPzemEnergy;
    } else {
      energyDelta = (PZEM_ENERGY_MAX_WH - s_lastPzemEnergy) + rawEnergy;
    }
    s_lastPzemEnergy = rawEnergy;
    pkt.energy = energyDelta;
    xSemaphoreGive(g_meterMutex);
  }

  uint8_t alarmState = (pkt.alarmThreshold > 0 &&
                        (pkt.power / 10) >= pkt.alarmThreshold) ? 1 : 0;

  pkt.pktType    = PKT_TELEMETRY;
  pkt.nodeId     = g_nodeSlotId;
  pkt.uid        = g_nodeUID;
  pkt.statusByte = encodeStatus(g_relayState, g_relayMode, g_schedState, alarmState);
  if (s_bulkReady) pkt.statusByte |= STATUS_BULK_READY;
  pkt.schedSH    = g_schedSH;
  pkt.schedSM    = g_schedSM;
  pkt.schedEH    = g_schedEH;
  pkt.schedEM    = g_schedEM;
  pkt.seqCounter = (s_dlAck ? 0x80u : 0x00u) | (++s_seqCounter & 0x7Fu);
  s_dlAck        = false;
  pkt.beaconRSSI = (uint8_t)(int8_t)s_beaconRSSI;
  pkt.fwVersion  = FW_VERSION;
  pkt.classByte  = g_classByte;
}

// =============================================================================
// State machine accessors (called by mode-specific state machines)
// =============================================================================

void tdmaSetBeaconRSSI(int8_t rssi)  { s_beaconRSSI = rssi; }
bool tdmaHasBulkPending()            { return s_bulkGrantReceived && s_bulkReady; }
void tdmaClearBulkGrant()            { s_bulkGrantReceived = false; s_bulkReady = false; }

// =============================================================================
// GFSK bulk transfer helpers
// =============================================================================

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

static void nodeRunBulkSend(uint16_t sfCount) {
  uint16_t offset = 0;
  uint8_t  seqNum = 0;
  uint8_t  fragBuf[BULK_FRAG_TOTAL];
  uint8_t  ackBuf[sizeof(BulkAckPacket)];
  uint32_t deadline = millis() + BULK_MAX_WINDOW_MS;

  while (offset < s_bulkTotalLen) {
    if ((int32_t)(deadline - millis()) <= 0) {
      logAsync("[NODE-BULK] TX window expired offset=%u\n", offset);
      return;
    }
    uint16_t chunkLen = s_bulkTotalLen - offset;
    if (chunkLen > BULK_FRAG_PAYLOAD) chunkLen = BULK_FRAG_PAYLOAD;
    bool isLast = (offset + chunkLen >= s_bulkTotalLen);

    BulkFragHeader* hdr = reinterpret_cast<BulkFragHeader*>(fragBuf);
    hdr->pktType    = PKT_BULK_FRAG;
    hdr->seqNum     = seqNum;
    hdr->payloadLen = (uint8_t)chunkLen;
    hdr->flags      = isLast ? 0x01 : 0x00;
    memcpy(fragBuf + BULK_FRAG_HEADER_LEN, s_nodeBulkBuf + offset, chunkLen);

    uint16_t synth_sf = (uint16_t)(((uint16_t)seqNum << 8) | (sfCount & 0xFF));
    pktEncrypt(fragBuf + BULK_FRAG_HEADER_LEN, chunkLen, synth_sf, g_nodeSlotId, PKT_DIR_BULK_UL);

    bool acked = false;
    for (uint8_t retry = 0; retry <= BULK_MAX_RETRIES && !acked; retry++) {
      if (radio.transmit(fragBuf, (size_t)(BULK_FRAG_HEADER_LEN + chunkLen)) != RADIOLIB_ERR_NONE) continue;
      radio.startReceive();
      int16_t rxLen = rxWindowFsk(ackBuf, sizeof(ackBuf), BULK_ACK_TIMEOUT_MS);
      if (rxLen == (int16_t)sizeof(BulkAckPacket)) {
        const BulkAckPacket* ack = reinterpret_cast<const BulkAckPacket*>(ackBuf);
        if (ack->seqNum == seqNum && ack->pktType == PKT_BULK_ACK) acked = true;
      }
    }
    if (!acked) { logAsync("[NODE-BULK] no ACK seq=%d\n", seqNum); return; }
    offset += chunkLen;
    seqNum++;
  }
  logAsync("[NODE-BULK] UL done: %u B\n", s_bulkTotalLen);
}

static void nodeRunBulkReceive(uint16_t sfCount) {
  uint16_t bufOff = 0;
  uint8_t  expSeq = 0;
  uint8_t  fragBuf[BULK_FRAG_TOTAL];
  uint8_t  ackBuf[sizeof(BulkAckPacket)];
  uint32_t deadline = millis() + BULK_MAX_WINDOW_MS;
  radio.startReceive();

  while (true) {
    if ((int32_t)(deadline - millis()) <= 0) {
      logAsync("[NODE-BULK] RX window expired seq=%d\n", expSeq); return;
    }
    int16_t rxLen = rxWindowFsk(fragBuf, sizeof(fragBuf), BULK_ACK_TIMEOUT_MS * 2);
    if (rxLen < (int16_t)(BULK_FRAG_HEADER_LEN + 1)) { radio.startReceive(); continue; }

    const BulkFragHeader* hdr = reinterpret_cast<const BulkFragHeader*>(fragBuf);
    uint8_t* payload = fragBuf + BULK_FRAG_HEADER_LEN;

    if (hdr->pktType != PKT_BULK_FRAG || hdr->seqNum != expSeq ||
        hdr->payloadLen == 0 || hdr->payloadLen > BULK_FRAG_PAYLOAD) {
      BulkAckPacket nack; nack.seqNum = expSeq; nack.pktType = PKT_BULK_NACK;
      memcpy(ackBuf, &nack, sizeof(nack));
      radio.transmit(ackBuf, sizeof(ackBuf));
      radio.startReceive(); continue;
    }

    uint16_t synth_sf = (uint16_t)(((uint16_t)hdr->seqNum << 8) | (sfCount & 0xFF));
    pktDecrypt(payload, hdr->payloadLen, synth_sf, g_nodeSlotId, PKT_DIR_BULK_DL);

    if (bufOff + hdr->payloadLen <= BULK_MAX_PAYLOAD) {
      memcpy(s_nodeBulkBuf + bufOff, payload, hdr->payloadLen);
      bufOff += hdr->payloadLen;
    }

    BulkAckPacket ack; ack.seqNum = hdr->seqNum; ack.pktType = PKT_BULK_ACK;
    memcpy(ackBuf, &ack, sizeof(ack));
    radio.transmit(ackBuf, sizeof(ackBuf));

    if (hdr->flags & 0x01) {
      if (s_bulkTypeTag == 0xF0 && bufOff > 0) {
        s_bulkTotalLen = bufOff;
        nodeRunBulkSend(sfCount);
      }
      logAsync("[NODE-BULK] DL done: %u B\n", bufOff);
      return;
    }
    expSeq++;
    radio.startReceive();
  }
}

void tdmaRunBulkIdle(uint16_t sfCount, uint32_t beaconReceiveTime) {
  uint32_t bulkStart = beaconReceiveTime
                       + (BEACON_MS - BEACON_AIR_MS)
                       + (uint32_t)MAX_NODES * SLOT_PAIR_MS
                       + CONTENTION_UL_MS + CONTENTION_DL_MS
                       + BULK_MAINT_MS + BULK_SWITCH_MS;
  waitUntilMs(bulkStart);

  radio.standby();
  int16_t fst = radio.beginFSK(GFSK_FREQUENCY, GFSK_BITRATE, GFSK_DEVIATION,
                                GFSK_RX_BANDWIDTH, GFSK_TX_POWER,
                                GFSK_PREAMBLE_LEN, false);
  if (fst == RADIOLIB_ERR_NONE) {
    radio.setSyncWord(const_cast<uint8_t*>(GFSK_SYNC_WORD), sizeof(GFSK_SYNC_WORD));
    radio.setCRC(2);
    if (s_bulkDir == 0) nodeRunBulkReceive(sfCount);
    else                nodeRunBulkSend(sfCount);
  } else {
    logAsync("[NODE-BULK] GFSK init err %d\n", fst);
  }

  // Restore LoRa; caller is responsible for re-attaching DIO callbacks if
  // using the IRQ-driven mode (beginFSK may clear RadioLib's DIO handlers).
  radio.standby();
  int16_t lrst = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SF, LORA_CR,
                              LORA_SYNC_WORD, LORA_TX_POWER, LORA_PREAMBLE_LEN);
  if (lrst != RADIOLIB_ERR_NONE) logAsync("[NODE-BULK] LoRa restore err %d\n", lrst);
  radio.setCRC(true);
  radio.setFrequency(LORA_CHANNELS[0]);

  tdmaClearBulkGrant();
}

// =============================================================================
// Schedule evaluator task (runs every 10 s, independent of TDMA cycle)
// =============================================================================

static void schedTask(void* /*params*/) {
  while (true) { evaluateSchedule(); vTaskDelay(pdMS_TO_TICKS(10000)); }
}

// =============================================================================
// Common task launcher
// =============================================================================

void nodeTdmaStartCommonTasks() {
  g_meterMutex = xSemaphoreCreateMutex();
  configASSERT(g_meterMutex);

  g_nodeUID = computeDeviceUID();
  Serial.printf("[NODE] Device UID = 0x%04X\n", g_nodeUID);

  halTaskCreatePinned(meterTaskFn, "METER", 4096, nullptr, 1, HAL_CORE_0, nullptr);
  halTaskCreatePinned(schedTask,   "SCHED", 2048, nullptr, 1, HAL_CORE_0, nullptr);
  halTaskCreatePinned(ledTask,     "LED",   2048, nullptr, 1, HAL_CORE_0, &s_ledTaskHandle);
}

#endif // NODE_TELEMETRY
