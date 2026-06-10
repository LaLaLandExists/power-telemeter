/**
 * meter_pzem.cpp
 * PZEM-004T v3 metering backend.
 * Compiled when neither PZEM_FAKE nor METER_AFE is defined.
 *
 * Reads voltage/current/power/energy/frequency/PF from the hardware meter
 * every 500 ms and writes results into g_meter under g_meterMutex.
 * Also consumes hasPendingThreshold to program the PZEM hardware alarm register.
 */
#if defined(NODE_TELEMETRY) && !defined(PZEM_FAKE) && !defined(METER_AFE)

#include "meter.h"
#include <Arduino.h>
#include <PZEM004Tv30.h>

extern PZEM004Tv30 pzem;  // defined in main.cpp

void meterTaskFn(void* /*params*/) {
  Serial.println("[PZEM] Sampling task started");
  while (true) {
    float v  = pzem.voltage();
    float i  = pzem.current();
    float p  = pzem.power();
    float e  = pzem.energy();
    float f  = pzem.frequency();
    float pf = pzem.pf();
    bool  valid = !isnan(v) && !isnan(i) && !isnan(p) && !isnan(f);

    if (valid) {
      if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_meter.voltage     = v;
        g_meter.current     = i;
        g_meter.power       = p;
        g_meter.energy      = isnan(e) ? g_meter.energy : (uint32_t)(e * 1000.0f + 0.5f);
        g_meter.frequency   = f;
        g_meter.powerFactor = isnan(pf) ? 0.0f : pf;
        g_meter.valid       = true;
        g_meter.readAt      = millis();
        xSemaphoreGive(g_meterMutex);
      }
    } else {
      static uint32_t lastErrLog = 0;
      if (millis() - lastErrLog > 5000) {
        Serial.println("[PZEM] Read error - check wiring/baud");
        lastErrLog = millis();
      }
    }

    bool     doThreshold = false;
    uint16_t threshWatts = 0;
    if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (g_meter.hasPendingThreshold) {
        doThreshold               = true;
        threshWatts               = g_meter.pendingThresholdW;
        g_meter.hasPendingThreshold = false;
      }
      xSemaphoreGive(g_meterMutex);
    }
    if (doThreshold) {
      bool ok = pzem.setPowerAlarm(threshWatts);
      Serial.printf("[PZEM] Alarm threshold -> %d W (%s)\n", threshWatts, ok ? "OK" : "FAIL");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

#endif // NODE_TELEMETRY && !PZEM_FAKE && !METER_AFE
