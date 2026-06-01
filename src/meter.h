/**
 * meter.h
 * Hardware-independent power meter data interface.
 *
 * MeterData is the single shared struct between the metering backend task and
 * the TDMA consumer (buildTelemetryPacket, handleDownlink). Access to g_meter
 * must be protected by g_meterMutex.
 *
 * meterTaskFn is resolved at link time to whichever backend is compiled in:
 *   meter_pzem.cpp  -- PZEM-004T v3 hardware  [!PZEM_FAKE && !METER_AFE]
 *   meter_fake.cpp  -- simulated readings      [PZEM_FAKE]
 *   meter_afe.cpp   -- ADS131M02 + DSP pipeline [METER_AFE]
 *
 * To add a new measurement field:
 *   1. Add it here in MeterData.
 *   2. Populate it in the active meter_*.cpp backend.
 *   3. Add the matching field to TelemetryPacket in tdma_protocol.h.
 *   4. Encode it in buildTelemetryPacket() in node_tdma_common.cpp.
 */
#pragma once
#include <Arduino.h>
#include "hal/hal_rtos.h"

struct MeterData {
  float    voltage;
  float    current;
  float    power;
  uint32_t energy;          // Wh (backend-maintained accumulator)
  float    frequency;
  float    powerFactor;
  uint16_t alarmThreshold;  // W; compared in software by buildTelemetryPacket
  bool     valid;
  uint32_t readAt;          // millis() of last successful sample

  // AFE-only fields -- PZEM and fake backends leave these zero-initialized.
  float    reactivePower;   // VAR; positive = inductive, negative = capacitive
  float    apparentPower;   // VA  (Vrms * Irms)
  float    qSign;           // +1 inductive, -1 capacitive, 0 resistive
  float    crestFactor;     // Ipeak / Irms

  // Pending threshold write: set by handleDownlink, consumed by meterTaskFn.
  // PZEM backend programs the hardware alarm register.
  // AFE backend applies the value to alarmThreshold directly (software only).
  // Both fields protected by g_meterMutex.
  uint16_t pendingThresholdW;
  bool     hasPendingThreshold;
};

extern MeterData          g_meter;
extern SemaphoreHandle_t  g_meterMutex;

/** Task entry point -- resolved at link time to the active backend. */
void meterTaskFn(void* params);
