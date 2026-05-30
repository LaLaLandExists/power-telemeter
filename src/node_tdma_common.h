/**
 * node_tdma_common.h
 * Shared types, globals, and utilities for node TDMA firmware.
 *
 * Included by both mode-specific headers:
 *   node_tdma_task.h  -- task-based (ESP32 and STM32 non-IRQ builds)
 *   node_irq_tdma.h   -- interrupt-driven (STM32 IRQ builds)
 *
 * Add new PZEM fields to PzemData here; both TDMA implementations pick them
 * up automatically.  Extend TelemetryPacket in tdma_protocol.h and update
 * buildTelemetryPacket() in node_tdma_common.cpp.
 */
#pragma once
#include <Arduino.h>
#include "hal/hal_rtos.h"
#include "tdma_protocol.h"

// =============================================================================
// PZEM data -- single definition shared by all node builds
// =============================================================================

struct PzemData {
  float    voltage;
  float    current;
  float    power;
  uint32_t energy;         // Wh (raw PZEM register)
  float    frequency;
  float    powerFactor;
  uint16_t alarmThreshold; // last value written to PZEM alarm register
  bool     valid;
  uint32_t readAt;         // millis() of last successful read

  // Pending threshold write: set by TDMA task, consumed by pzemTask.
  // Both fields protected by g_pzemMutex.
  uint16_t pendingThresholdW;
  bool     hasPendingThreshold;
};

// =============================================================================
// Globals (defined in node_tdma_common.cpp)
// =============================================================================

extern PzemData          g_pzem;
extern SemaphoreHandle_t g_pzemMutex;

extern bool     g_nodeRegistered;
extern uint8_t  g_nodeSlotId;
extern uint16_t g_nodeUID;

extern uint8_t  g_relayState;   // 0=OFF, 1=ON
extern uint8_t  g_relayMode;    // 0=MANUAL, 1=SCHEDULED
extern uint8_t  g_schedState;   // 0=NONE, 1=WAITING, 2=ACTIVE
extern uint8_t  g_schedSH, g_schedSM, g_schedEH, g_schedEM;

extern uint32_t g_rtcBaseSec;
extern uint32_t g_rtcBaseMs;
extern bool     g_rtcSet;

// =============================================================================
// LED mode (used by both state machine and LED task)
// =============================================================================

typedef enum {
  LED_MODE_LISTEN,
  LED_MODE_CONTENDING,
  LED_MODE_REGISTERED
} LedTdmaMode_t;

// =============================================================================
// Inline helpers
// =============================================================================

inline uint32_t rtcGetSec() {
  if (!g_rtcSet) return 0;
  return (g_rtcBaseSec + (millis() - g_rtcBaseMs) / 1000UL) % 86400UL;
}

// =============================================================================
// Shared API (implemented in node_tdma_common.cpp)
// =============================================================================

/** Set relay GPIO and update g_relayState. Thread-safe. */
void setRelay(uint8_t state);

/** Conditionally sync schedule RTC from beacon H:M:S. */
void rtcConditionalSync(uint8_t h, uint8_t m, uint8_t s);

/** Evaluate relay schedule against current RTC time; call setRelay if needed. */
void evaluateSchedule();

/**
 * Process a received downlink buffer.
 * Writes to global relay/schedule/threshold state and sets the dlAck flag
 * used by the next uplink packet.
 */
void handleDownlink(const uint8_t* buf, int16_t len);

/**
 * Fill a TelemetryPacket from current g_pzem readings and node state.
 * Does NOT encrypt or transmit -- caller handles those steps so that
 * task-based (blocking transmit) and IRQ-driven (startTransmit) can differ.
 */
void buildTelemetryPacket(TelemetryPacket& pkt);

/** Trigger the LED nudge pattern (rapid red blink). ISR/task safe. */
void triggerNudge();

/** Update LED mode from the TDMA state machine. */
void tdmaSetLedMode(LedTdmaMode_t mode);

/** Record beacon RSSI for inclusion in the next telemetry packet. */
void tdmaSetBeaconRSSI(int8_t rssi);

/** True when a BulkGrant has been received and the node is ready. */
bool tdmaHasBulkPending();

/**
 * Run GFSK bulk exchange inside the Idle window, then restore LoRa.
 * waitUntilMs() is called internally for the window start.
 * After returning, IRQ builds must re-attach setDio0Action/setDio1Action.
 */
void tdmaRunBulkIdle(uint16_t sfCount, uint32_t beaconReceiveTime);

/** Clear bulk grant state after the Idle window. */
void tdmaClearBulkGrant();

/**
 * Spawn PZEM, schedule-evaluator, and LED tasks.
 * Call from nodeTdmaTaskStart() before launching the TDMA task itself.
 * Initialises g_pzemMutex and g_nodeUID.
 */
void nodeTdmaStartCommonTasks();

// =============================================================================
// Entry point -- implemented in the mode-specific .cpp (task or IRQ)
// =============================================================================

/**
 * Launch the node TDMA subsystem.
 * Calls nodeTdmaStartCommonTasks() then adds the mode-specific TDMA task.
 */
void nodeTdmaTaskStart();
