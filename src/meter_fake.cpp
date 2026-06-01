/**
 * meter_fake.cpp
 * Simulated metering backend for network-test builds (-D PZEM_FAKE).
 * No real meter hardware required. Produces a refrigerator-like load profile
 * matching simulation.js Node 1 so the LoRa TDMA path can be exercised
 * without AC metering hardware attached.
 */
#if defined(NODE_TELEMETRY) && defined(PZEM_FAKE)

#include "meter.h"
#include <Arduino.h>
#include <math.h>
#include <stdlib.h>

// --- Load profile constants --------------------------------------------------
#define FAKE_NOMINAL_W     120.0f
#define FAKE_IDLE_W          5.0f
#define FAKE_CYCLE_MS     600000UL  // 10-minute ON/OFF cycle
#define FAKE_ON_FRAC         0.60f
#define FAKE_VOLT_NOM      220.0f
#define FAKE_FREQ_NOM       60.0f
#define FAKE_PF_NOM          0.85f
#define FAKE_PF_NOISE        0.04f

// Uniform random noise in [-amp, +amp], matching simulation.js noise(amp)
static inline float noise(float amp) {
  return ((float)(rand() % 1001) / 500.0f - 1.0f) * amp;
}

void meterTaskFn(void* /*params*/) {
  Serial.println("[PZEM-FAKE] Task started -- net-test mode, no hardware required");

  float    accumEnergyWhF = 0.0f;  // float carry so sub-Wh values accumulate
  uint32_t lastMs         = millis();

  while (true) {
    uint32_t now   = millis();
    uint32_t dt_ms = now - lastMs;
    lastMs = now;

    uint32_t phase_ms  = now % FAKE_CYCLE_MS;
    bool     inOnPhase = phase_ms < (uint32_t)(FAKE_CYCLE_MS * FAKE_ON_FRAC);

    float v, i, p, pf, f;

    // Relay state read outside mutex -- g_relayState is written by setRelay()
    // which is called only from the TDMA task or schedule task; reading it
    // without the mutex here is acceptable (single-byte, atomic on ARM/Xtensa).
    extern uint8_t g_relayState;

    if (g_relayState == 0) {
      // Relay OFF: line voltage present, no current
      v  = FAKE_VOLT_NOM + noise(0.6f);
      i  = 0.0f;
      p  = 0.0f;
      pf = 0.0f;
      f  = FAKE_FREQ_NOM + 0.06f * sinf((float)now / 18000.0f) + noise(0.04f);
    } else if (inOnPhase) {
      // ON phase: sinusoidal power ripple + noise, voltage sag under load
      float t = (float)now / 1000.0f;
      p  = FAKE_NOMINAL_W
         + FAKE_NOMINAL_W * 0.12f * sinf(t * 0.8f)
         + noise(FAKE_NOMINAL_W * 0.05f);
      if (p < 0.0f) p = 0.0f;
      pf = fminf(1.0f, fmaxf(0.5f, FAKE_PF_NOM + noise(FAKE_PF_NOISE)));
      float i_est = p / (FAKE_VOLT_NOM * pf + 0.001f);
      v  = fmaxf(195.0f, FAKE_VOLT_NOM - i_est * 0.3f + noise(0.4f));
      i  = p / (v * pf + 0.001f);
      f  = FAKE_FREQ_NOM + 0.06f * sinf((float)now / 18000.0f) + noise(0.04f);
    } else {
      // Idle phase: standby draw, near-nominal voltage
      p  = FAKE_IDLE_W + noise(FAKE_IDLE_W * 0.1f + 0.5f);
      if (p < 0.0f) p = 0.0f;
      pf = fminf(1.0f, fmaxf(0.5f, FAKE_PF_NOM + noise(FAKE_PF_NOISE)));
      v  = FAKE_VOLT_NOM + noise(0.6f);
      i  = p / (v * pf + 0.001f);
      f  = FAKE_FREQ_NOM + 0.06f * sinf((float)now / 18000.0f) + noise(0.04f);
    }

    accumEnergyWhF += p * (float)dt_ms / 3600000.0f;

    bool     doThreshold = false;
    uint16_t threshWatts = 0;

    if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      g_meter.voltage     = v;
      g_meter.current     = i;
      g_meter.power       = p;
      g_meter.energy      = (uint32_t)accumEnergyWhF;
      g_meter.frequency   = f;
      g_meter.powerFactor = pf;
      g_meter.valid       = true;
      g_meter.readAt      = now;

      if (g_meter.hasPendingThreshold) {
        doThreshold                 = true;
        threshWatts                 = g_meter.pendingThresholdW;
        g_meter.alarmThreshold      = threshWatts;
        g_meter.hasPendingThreshold = false;
      }
      xSemaphoreGive(g_meterMutex);
    }

    if (doThreshold) {
      Serial.printf("[PZEM-FAKE] Alarm threshold -> %d W\n", threshWatts);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

#endif // NODE_TELEMETRY && PZEM_FAKE
