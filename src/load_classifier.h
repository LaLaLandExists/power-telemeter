/**
 * load_classifier.h
 * Hand-crafted decision tree for 6-class AC load identification.
 *
 * Runs on ESP32-S3 node, Core 0, within adcTask (METER_AFE builds only).
 *
 * Integration pattern:
 *   1. After each 128-point FFT completes, populate a ClassifierFeatures
 *      struct from the DSP results.
 *   2. Call classifyLoad() to get a raw ClassResult.
 *   3. Call classifierUpdate() to push into the 5-sample smoother.
 *   4. Call packClassByte() and write the result to g_classByte so
 *      buildTelemetryPacket() picks it up for the next uplink.
 *
 * See future/load_classification_algorithm.md for the full design rationale,
 * feature definitions, and tree structure diagram.
 */
#pragma once
#include <stdint.h>
#include <math.h>
#include "tdma_protocol.h"   // CLASS_BYTE_PENDING

// -----------------------------------------------------------------------------
// Load class identifiers
// -----------------------------------------------------------------------------
enum LoadClass : uint8_t {
  LOAD_RESISTIVE  = 0,   // Resistive: incandescent, iron, heater
  LOAD_CAPACITIVE = 1,   // Capacitive: PFC SMPS, UPS on battery, LED with PFC
  LOAD_MOTOR_1PH  = 2,   // Single-phase induction motor: aircon, refrigerator
  LOAD_FAN        = 3,   // Fan motor: ceiling fan, desk fan (deep lagging PF)
  LOAD_SMPS       = 4,   // Switch-mode PSU (no PFC): laptop charger, desktop PSU
  LOAD_LIGHTING   = 5,   // Non-linear lighting: CFL, LED (no PFC), fluorescent
  LOAD_NONE       = 6,   // No load: relay open or irms below minimum gate (50 mA)
  LOAD_COUNT      = 7
};

// -----------------------------------------------------------------------------
// Feature vector -- 10 floats (40 bytes), computed by the DSP pipeline.
// All features are dimensionless ratios or normalised quantities so the
// classifier is independent of absolute load power level.
// -----------------------------------------------------------------------------
struct ClassifierFeatures {
  float pf;        // f0: power factor [0..1]
  float qSign;     // f1: reactive power sign: +1 inductive, -1 capacitive, 0
  float thdI;      // f2: current THD (odd harmonics, h3..h15), dimensionless
  float h3Ratio;   // f3: |I3| / |I1|
  float h5Ratio;   // f4: |I5| / |I1|
  float h7Ratio;   // f5: |I7| / |I1|
  float cf;        // f6: crest factor  Ipeak / Irms
  float oeRatio;   // f7: even harmonic ratio (|I2|+|I4|) / |I1|
  float h53Ratio;  // f8: |I5| / |I3|
  float pRms;      // f9: log2(Irms / 0.05), clamped [0, 10]
};

// -----------------------------------------------------------------------------
// Classification result (single invocation)
// -----------------------------------------------------------------------------
struct ClassResult {
  LoadClass classId;
  uint8_t   confidence;  // 0-7 (maps to 0%-100% in ~14.3% steps)
  bool      transient;   // true when classification may be unstable
};

// -----------------------------------------------------------------------------
// Tunable thresholds -- all defaults derived from Section 2.4 of the
// load_classification_algorithm.md typical feature table.
// Can be loaded from NVS and adjusted per-deployment without recompiling.
// -----------------------------------------------------------------------------
struct ClassifierThresholds {
  float minIrms;           // no-load gate (A)
  float lowThdLimit;       // THD boundary between linear and non-linear loads
  float highPfLimit;       // PF above which load is considered resistive
  float cfSmpsLimit;       // CF above which high-THD load is SMPS
  float h3LightingLimit;   // H3/H1 above which moderate-CF load is lighting
  float pfMotorFanLimit;   // PF boundary between motor (high) and fan (low)
  float qSignCapThresh;    // Qsign threshold for capacitive detection (< this)
};

static const ClassifierThresholds DEFAULT_THRESHOLDS = {
  0.050f,   // minIrms:         50 mA  no-load gate
  0.150f,   // lowThdLimit:     15% THD separates linear from non-linear
  0.950f,   // highPfLimit:     PF > 0.95 => resistive
  2.200f,   // cfSmpsLimit:     CF > 2.2 => SMPS
  0.350f,   // h3LightingLimit: H3/H1 > 0.35 => lighting
  0.720f,   // pfMotorFanLimit: PF > 0.72 => motor, else fan
 -0.500f    // qSignCapThresh:  Qsign < -0.5 => capacitive
};

// -----------------------------------------------------------------------------
// Confidence estimation
// Measures how far the deciding feature is from the threshold that was crossed.
// margin is the "full confidence" distance; dist/margin clamped to [0,1]
// then mapped to [1,7] so we never return 0 (which is reserved for pending).
// -----------------------------------------------------------------------------
static inline uint8_t computeConfidence(float feature, float threshold,
                                        float margin)
{
  float dist = (feature >= threshold)
               ? (feature - threshold)
               : (threshold - feature);
  float ratio = dist / margin;
  if (ratio > 1.0f) ratio = 1.0f;
  return (uint8_t)(1.0f + ratio * 6.0f);
}

// -----------------------------------------------------------------------------
// Main decision tree classifier
//
// Tree strategy: "peel off easy cases first" -- each split isolates one class
// with minimal false positives before harder separations.
//
// Step 1: No-load gate (Irms < 50 mA) -> RESISTIVE (idle/off)
// Step 2: THD split (< 15% = sinusoidal branch, >= 15% = distorted branch)
// Step 3a (sinusoidal, PF > 0.95):  -> RESISTIVE
// Step 3b (sinusoidal, Qsign < 0):  -> CAPACITIVE
// Step 3c (sinusoidal, inductive):  PF > 0.72 -> MOTOR_1PH, else FAN
// Step 4  (distorted, CF > 2.2):    -> SMPS
// Step 5  (distorted, H3 > 0.35):   -> LIGHTING
// Step 5b (residual):               Qsign/PF -> CAPACITIVE/MOTOR_1PH/FAN
// -----------------------------------------------------------------------------
static inline ClassResult classifyLoad(const ClassifierFeatures& fv,
                                       float irms,
                                       const ClassifierThresholds& th)
{
  ClassResult result;
  result.transient = false;

  // Step 1: No-load gate -- irms below minimum means relay is open or nothing connected.
  // LOAD_NONE is handled by the caller (bypasses smoother); returned here for completeness.
  if (irms < th.minIrms) {
    result.classId    = LOAD_NONE;
    result.confidence = 7;
    return result;
  }

  if (fv.thdI < th.lowThdLimit) {
    // ---- LOW-THD BRANCH (approximately sinusoidal current) ----

    // Step 3a: High PF => resistive
    if (fv.pf > th.highPfLimit) {
      result.classId    = LOAD_RESISTIVE;
      result.confidence = computeConfidence(fv.pf, th.highPfLimit, 0.03f);
      return result;
    }

    // Step 3b: Leading current (capacitive)
    if (fv.qSign < th.qSignCapThresh) {
      result.classId    = LOAD_CAPACITIVE;
      result.confidence = computeConfidence(fv.qSign, th.qSignCapThresh, 0.50f);
      return result;
    }

    // Step 3c: Motor vs. Fan (both inductive; separated by PF)
    if (fv.pf > th.pfMotorFanLimit) {
      result.classId    = LOAD_MOTOR_1PH;
      result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit, 0.10f);
    } else {
      result.classId    = LOAD_FAN;
      result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit, 0.15f);
    }
    return result;

  } else {
    // ---- HIGH-THD BRANCH (distorted current waveform) ----

    // Step 4: High crest factor => SMPS (narrow current pulses)
    if (fv.cf > th.cfSmpsLimit) {
      result.classId    = LOAD_SMPS;
      result.confidence = computeConfidence(fv.cf, th.cfSmpsLimit, 0.80f);
      return result;
    }

    // Step 5: High H3 ratio => lighting (CFL / non-PFC LED)
    if (fv.h3Ratio > th.h3LightingLimit) {
      result.classId    = LOAD_LIGHTING;
      result.confidence = computeConfidence(fv.h3Ratio, th.h3LightingLimit, 0.15f);
      return result;
    }

    // Step 5b: Residual -- use Qsign and PF (same logic as low-THD branch)
    if (fv.qSign < th.qSignCapThresh) {
      result.classId    = LOAD_CAPACITIVE;
      result.confidence = computeConfidence(fv.qSign, th.qSignCapThresh, 0.50f);
      return result;
    }
    if (fv.pf > th.pfMotorFanLimit) {
      result.classId    = LOAD_MOTOR_1PH;
      result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit, 0.10f);
    } else {
      result.classId    = LOAD_FAN;
      result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit, 0.15f);
    }
    // High-THD motor/fan is unusual (triac dimmer, VFD); flag as transient
    result.transient = true;
    return result;
  }
}

// -----------------------------------------------------------------------------
// Temporal smoothing -- majority vote over last SMOOTH_WINDOW classifications
// weighted by confidence.  Suppresses transient misclassifications (e.g. motor
// inrush, load switching).  Window = 5 x 2 s = 10 s of history.
// -----------------------------------------------------------------------------
#define SMOOTH_WINDOW 5

struct ClassifierState {
  LoadClass history[SMOOTH_WINDOW];
  uint8_t   confHistory[SMOOTH_WINDOW];
  uint8_t   writeIdx;
  bool      filled;            // true once SMOOTH_WINDOW samples have arrived
  LoadClass smoothedClass;
  uint8_t   smoothedConf;
  bool      smoothedTransient;
};

static inline void classifierStateInit(ClassifierState& st)
{
  for (int i = 0; i < SMOOTH_WINDOW; i++) {
    st.history[i]     = LOAD_RESISTIVE;
    st.confHistory[i] = 0;
  }
  st.writeIdx          = 0;
  st.filled            = false;
  st.smoothedClass     = LOAD_RESISTIVE;
  st.smoothedConf      = 0;
  st.smoothedTransient = false;
}

static inline void classifierUpdate(ClassifierState& st, const ClassResult& raw)
{
  st.history[st.writeIdx]     = raw.classId;
  st.confHistory[st.writeIdx] = raw.confidence;
  st.writeIdx++;
  if (st.writeIdx >= SMOOTH_WINDOW) {
    st.writeIdx = 0;
    st.filled   = true;
  }

  int limit = st.filled ? SMOOTH_WINDOW : (int)st.writeIdx;
  float votes[LOAD_COUNT];
  for (int c = 0; c < LOAD_COUNT; c++) votes[c] = 0.0f;
  for (int i = 0; i < limit; i++) votes[st.history[i]] += (float)st.confHistory[i];

  float bestScore = -1.0f;
  LoadClass bestClass = LOAD_RESISTIVE;
  for (int c = 0; c < LOAD_COUNT; c++) {
    if (votes[c] > bestScore) { bestScore = votes[c]; bestClass = (LoadClass)c; }
  }

  bool unanimous = true;
  for (int i = 0; i < limit; i++) {
    if (st.history[i] != bestClass) { unanimous = false; break; }
  }

  st.smoothedClass     = bestClass;
  st.smoothedTransient = !unanimous;

  float sumConf = 0.0f;
  int   countWinner = 0;
  for (int i = 0; i < limit; i++) {
    if (st.history[i] == bestClass) { sumConf += (float)st.confHistory[i]; countWinner++; }
  }
  st.smoothedConf = (countWinner > 0) ? (uint8_t)(sumConf / (float)countWinner) : 0;
}

// -----------------------------------------------------------------------------
// Telemetry packing -- encode ClassifierState into the single classByte field
// of TelemetryPacket.  Returns CLASS_BYTE_PENDING (0x40) while the smoothing
// window is still filling so the gateway can signal "analyzing" to the UI.
// -----------------------------------------------------------------------------
static inline uint8_t packClassByte(const ClassifierState& st)
{
  if (!st.filled) return CLASS_BYTE_PENDING;
  uint8_t b = 0;
  b |= (st.smoothedClass & 0x07u);
  b |= ((st.smoothedConf & 0x07u) << 3);
  if (st.smoothedTransient) b |= (1u << 6);
  return b;
}
