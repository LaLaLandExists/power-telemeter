/**
 * meter_afe.cpp
 * ADS131M02 analog frontend metering backend (-D METER_AFE).
 *
 * Three-stage DSP pipeline driven by halAdsReadSample() at ~4 kSPS:
 *
 *   Stage 0 -- per sample (4 kHz):
 *     processSample() scales codes to physical units, accumulates running sums
 *     (V^2, I^2, V*I, Vq*I), tracks peak |I|, and detects rising zero-crossings.
 *
 *   Stage 1 -- per cycle (~60 Hz, on rising ZC):
 *     finaliseCycle() computes Vrms, Irms, P, Q, S, PF, frequency, crest factor,
 *     and energy; writes results to g_meter under g_meterMutex.
 *
 *   Stage 2 -- every 2 seconds:
 *     runHarmonicAnalysis() windows 128 samples, runs the FFT twice (V, I),
 *     extracts H1-H15 magnitudes, computes THD and harmonic ratios, and writes
 *     g_features under g_featuresMutex.
 *
 * Reactive power uses a linearly-interpolated quarter-cycle voltage delay
 * (Approach B) to avoid the ~3% systematic error from integer-only delays.
 *
 * Energy is accumulated as a double for long-term precision and truncated to
 * uint32_t Wh when writing to g_meter.energy for telemetry packet compatibility.
 *
 * All three stages run sequentially within the single meterTaskFn() context so
 * the circular buffers (s_vBuf, s_iBuf) and accumulators are never accessed
 * concurrently -- no spinlock needed for them. g_meter and g_features are
 * shared with the TDMA consumer and are protected by their respective mutexes.
 *
 * Calibration gains (kV, kI) are loaded from NVS namespace "afe-cal" (keys
 * "kv", "ki", "phcorr") and fall back to nominal component values if absent.
 */
#if defined(NODE_TELEMETRY) && defined(METER_AFE)

#include "meter.h"
#include "classifier_features.h"
#include "load_classifier.h"
#include "hal/hal_ads131.h"
#include "hal/hal_fft.h"
#include "hal/hal_nvs.h"
#include "hal/hal_rtos.h"
#include <Arduino.h>
#include <math.h>

// =============================================================================
// Compile-time configuration
// =============================================================================

static const float    FS            = 4000.0f;
static const int32_t  FFT_N        = 128;
static const int32_t  CIRC_BUF_LEN = 256;
static const float    VREF         = 1.2f;
static const float    FILTER_FC    = 2341.0f;   // AA filter cutoff (Hz)
static const float    R_LIMIT      = 112000.0f; // ZMPT101B current-limiting R (ohm)
static const float    R_SAMPLE     = 330.0f;    // ZMPT101B sampling R (ohm)
static const float    CT_TURNS     = 1000.0f;   // CT turns ratio
static const float    R_BURDEN     = 10.0f;     // CT burden R (ohm)
static const float    PI_F         = 3.14159265358979f;

// =============================================================================
// Internal types
// =============================================================================

struct CalibrationParams {
  float kV;           // code -> Vmains scale factor (includes field calibration)
  float kI;           // code -> Iprimary scale factor (includes field calibration)
  float phaseCompRad; // residual phase compensation (radians)
};

struct CycleAccumulator {
  float   sumV2;
  float   sumI2;
  float   sumVI;    // real power:     sum(v[n] * i[n])
  float   sumVqI;   // reactive power: sum(vq[n] * i[n])
  float   peakI;    // max |i[n]| in this cycle for crest factor
  int32_t nSamples;
};

struct FrequencyTracker {
  float   prevZcFrac;    // fractional sample index of previous rising ZC
  float   freqAvg;       // IIR-filtered frequency estimate (Hz)
  float   prevV;         // v[n-1] for rising-edge detection
  int32_t sampleCounter; // monotonic per-session sample count
  bool    valid;         // true once first pair of ZCs has been observed

  void init() {
    prevZcFrac    = 0.0f;
    freqAvg       = 60.0f;
    prevV         = 0.0f;
    sampleCounter = 0;
    valid         = false;
  }

  // Returns true when a rising ZC is detected; updates freqAvg via IIR.
  bool update(float v) {
    bool zc = false;
    if (prevV < 0.0f && v >= 0.0f) {
      float frac   = -prevV / (v - prevV);
      float zcSamp = (float)(sampleCounter - 1) + frac;
      if (valid) {
        float period = zcSamp - prevZcFrac;
        if (period > 0.0f) {
          float fInst = FS / period;
          if (fInst > 45.0f && fInst < 75.0f) {
            freqAvg = 0.9375f * freqAvg + 0.0625f * fInst; // IIR alpha=1/16
          }
        }
      }
      prevZcFrac = zcSamp;
      valid      = true;
      zc         = true;
    }
    prevV = v;
    sampleCounter++;
    return zc;
  }
};

struct HarmonicResult {
  float magV[10];
  float magI[10];
  float phaseV[10];
  float phaseI[10];
};

// =============================================================================
// Harmonic bin indices for 60 Hz / fs=4000 / N=128
// H1=60Hz(2) H2(4) H3(6) H4(8) H5(10) H7(13) H9(17) H11(21) H13(25) H15(29)
// =============================================================================

static const int32_t kHarmonicBins[10]  = { 2, 4, 6, 8, 10, 13, 17, 21, 25, 29 };
static const float   kHarmonicFreqs[10] = { 60, 120, 180, 240, 300, 420, 540, 660, 780, 900 };

// =============================================================================
// Module state (all static -- no heap allocation after init)
// =============================================================================

static float s_vBuf[CIRC_BUF_LEN];
static float s_iBuf[CIRC_BUF_LEN];
static int32_t s_bufIdx = 0;

static CycleAccumulator s_acc;
static CycleAccumulator s_accSnap;  // snapped at ZC boundary; consumed by finaliseCycle
static bool             s_zcPending = false;

static FrequencyTracker  s_freq;
static CalibrationParams s_cal;

static int32_t s_n90Int   = 16;     // floor(FS / (4 * 60))
static float   s_n90Frac  = 0.667f; // fractional part of n90
static float   s_lastFreqForDelay = 60.0f;

static float  s_hannWindow[FFT_N];
static float  s_filterComp[10];
static float  s_fftBuf[FFT_N * 2]; // complex interleaved [Re0, Im0, Re1, Im1, ...]

static double s_energyAccWh = 0.0; // double for long-run precision (see design doc sec 3.5)

static ClassifierState s_clfState;

// =============================================================================
// Shared globals
// g_meter and g_meterMutex are defined in node_tdma_common.cpp and created
// by nodeTdmaStartCommonTasks() before this task runs.
// g_features and g_featuresMutex are AFE-only; defined and created here.
// =============================================================================

ClassifierFeatures g_features      = {};
SemaphoreHandle_t  g_featuresMutex = nullptr;

// =============================================================================
// Platform pin selection
// =============================================================================

static HalAdsPins selectPins() {
#if defined(BOARD_S3_SUPERMINI)
  // SCK=16 MOSI=17 MISO=18 CS=15 DRDY=14 RST=21  (PCB TBD)
  return { 16, 17, 18, 15, 14, 21 };
#elif defined(BOARD_STM32_F411)
  // PB13=SCK PB15=MOSI PB14=MISO PB12=CS PA0=DRDY PA1=RST  (PCB TBD)
  return { PB13, PB15, PB14, PB12, PA0, PA1 };
#else
  #error "METER_AFE requires BOARD_S3_SUPERMINI or BOARD_STM32_F411"
#endif
}

// =============================================================================
// Stage 0: per-sample processing
// =============================================================================

// Quarter-cycle voltage delay via linear interpolation (Approach B).
// Avoids the ~3% systematic reactive power error from integer-only delay.
static inline float getVq(int32_t writeIdx) {
  int32_t idxOlder = (writeIdx - s_n90Int - 1 + CIRC_BUF_LEN) % CIRC_BUF_LEN;
  int32_t idxNewer = (writeIdx - s_n90Int     + CIRC_BUF_LEN) % CIRC_BUF_LEN;
  return (1.0f - s_n90Frac) * s_vBuf[idxOlder] + s_n90Frac * s_vBuf[idxNewer];
}

// Recompute quarter-cycle delay params when measured frequency drifts > 0.1 Hz.
static void updateReactiveDelay(float freq) {
  float n90     = FS / (4.0f * freq);
  s_n90Int      = (int32_t)n90;
  s_n90Frac     = n90 - (float)s_n90Int;
  s_lastFreqForDelay = freq;
}

static void processSample(int32_t codeV, int32_t codeI) {
  float v = (float)codeV * s_cal.kV;
  float i = (float)codeI * s_cal.kI;

  s_vBuf[s_bufIdx] = v;
  s_iBuf[s_bufIdx] = i;
  s_bufIdx = (s_bufIdx + 1) % CIRC_BUF_LEN;

  s_acc.sumV2  += v * v;
  s_acc.sumI2  += i * i;
  s_acc.sumVI  += v * i;
  s_acc.sumVqI += getVq(s_bufIdx) * i;

  float absI = (i >= 0.0f) ? i : -i;
  if (absI > s_acc.peakI) s_acc.peakI = absI;
  s_acc.nSamples++;

  if (s_freq.update(v)) {
    s_accSnap  = s_acc;
    s_acc      = CycleAccumulator{};
    s_zcPending = true;

    if ((s_freq.freqAvg - s_lastFreqForDelay) >  0.1f ||
        (s_freq.freqAvg - s_lastFreqForDelay) < -0.1f) {
      updateReactiveDelay(s_freq.freqAvg);
    }
  }
}

// =============================================================================
// Stage 1: per-cycle finalisation
// =============================================================================

static void finaliseCycle() {
  const CycleAccumulator& a = s_accSnap;
  if (a.nSamples < 30) return; // reject spurious sub-half-cycle events

  float invN = 1.0f / (float)a.nSamples;
  float vrms = sqrtf(a.sumV2 * invN);
  float irms = sqrtf(a.sumI2 * invN);
  float P    = a.sumVI  * invN;
  float Q    = a.sumVqI * invN;
  float S    = vrms * irms;

  float pf = 0.0f;
  if (S > 1.0e-6f) {
    pf = P / S;
    if (pf >  1.0f) pf =  1.0f;
    if (pf < -1.0f) pf = -1.0f;
  }

  float qSign     = 0.0f;
  float qDeadband = 0.02f * S;
  if      (Q >  qDeadband) qSign =  1.0f;
  else if (Q < -qDeadband) qSign = -1.0f;

  float cf = (irms > 1.0e-6f) ? (a.peakI / irms) : 0.0f;

  float  cycleDur = (float)a.nSamples / FS;
  s_energyAccWh  += (double)P * (double)cycleDur / 3600.0;

  bool     doThreshold = false;
  uint16_t threshWatts = 0;

  if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    g_meter.voltage       = vrms;
    g_meter.current       = irms;
    g_meter.power         = (P > 0.0f) ? P : 0.0f;
    g_meter.reactivePower = Q;
    g_meter.apparentPower = S;
    g_meter.powerFactor   = pf;
    g_meter.qSign         = qSign;
    g_meter.crestFactor   = cf;
    g_meter.frequency     = s_freq.freqAvg;
    g_meter.energy        = (uint32_t)(s_energyAccWh);
    g_meter.valid         = true;
    g_meter.readAt        = millis();

    if (g_meter.hasPendingThreshold) {
      doThreshold                 = true;
      threshWatts                 = g_meter.pendingThresholdW;
      g_meter.alarmThreshold      = threshWatts;
      g_meter.hasPendingThreshold = false;
    }
    xSemaphoreGive(g_meterMutex);
  }

  if (doThreshold) {
    Serial.printf("[METER-AFE] Alarm threshold -> %d W (software)\n", threshWatts);
  }
}

// =============================================================================
// Stage 2: FFT-based harmonic analysis
// =============================================================================

static void initHannWindow() {
  for (int32_t n = 0; n < FFT_N; n++) {
    float phase     = 2.0f * PI_F * (float)n / (float)FFT_N;
    s_hannWindow[n] = 0.5f - 0.5f * cosf(phase);
  }
}

static void initFilterCompensation() {
  for (int32_t h = 0; h < 10; h++) {
    float ratio    = kHarmonicFreqs[h] / FILTER_FC;
    s_filterComp[h] = sqrtf(1.0f + ratio * ratio);
  }
}

// Copy most-recent FFT_N samples, apply Hann window, pack as complex (Im=0).
static void prepareFFTInput(const float* circBuf) {
  for (int32_t n = 0; n < FFT_N; n++) {
    int32_t src        = (s_bufIdx - FFT_N + n + CIRC_BUF_LEN) % CIRC_BUF_LEN;
    s_fftBuf[2 * n]    = circBuf[src] * s_hannWindow[n];
    s_fftBuf[2 * n + 1] = 0.0f;
  }
}

// Extract magnitude and phase at each harmonic bin.
// Normalisation: 2/(N*S1) = 2/(128*0.5) = 1/32 (Hann coherent gain S1=0.5).
static void extractHarmonics(float* magOut, float* phaseOut) {
  static const float NORM = 1.0f / 32.0f;
  for (int32_t h = 0; h < 10; h++) {
    int32_t k  = kHarmonicBins[h];
    float   re = s_fftBuf[2 * k];
    float   im = s_fftBuf[2 * k + 1];
    magOut[h]   = NORM * sqrtf(re * re + im * im) * s_filterComp[h];
    phaseOut[h] = atan2f(im, re);
  }
}

static float computeThdI(const float* magI) {
  // Odd harmonics only: H3(idx2), H5(4), H7(5), H9(6), H11(7), H13(8), H15(9)
  static const int32_t oddIdx[] = { 2, 4, 5, 6, 7, 8, 9 };
  float sumSq = 0.0f;
  for (int32_t j = 0; j < 7; j++) { float m = magI[oddIdx[j]]; sumSq += m * m; }
  float i1 = magI[0];
  if (i1 < 1.0e-9f) return 0.0f;
  return sqrtf(sumSq) / i1;
}

static float computeLogCurrent(float irms) {
  if (irms < 0.05f) return 0.0f;
  float val = logf(irms / 0.05f) * 1.442695f; // log2(x) = log(x)/log(2)
  return (val > 10.0f) ? 10.0f : val;
}

static float estimateFreqFromFFT() {
  float    maxMag = 0.0f;
  int32_t  peak   = 2;
  for (int32_t k = 1; k <= 3; k++) {
    float re  = s_fftBuf[2 * k], im = s_fftBuf[2 * k + 1];
    float mag = re * re + im * im;
    if (mag > maxMag) { maxMag = mag; peak = k; }
  }

  float df = FS / (float)FFT_N;
  if (peak < 1 || peak > (FFT_N / 2 - 1)) return (float)peak * df;

  int32_t m = peak - 1, p = peak + 1;
  float mM  = sqrtf(s_fftBuf[2*m]*s_fftBuf[2*m] + s_fftBuf[2*m+1]*s_fftBuf[2*m+1]);
  float m0  = sqrtf(s_fftBuf[2*peak]*s_fftBuf[2*peak] + s_fftBuf[2*peak+1]*s_fftBuf[2*peak+1]);
  float mP  = sqrtf(s_fftBuf[2*p]*s_fftBuf[2*p] + s_fftBuf[2*p+1]*s_fftBuf[2*p+1]);
  float den = 2.0f * (mM - 2.0f * m0 + mP);
  float delta = (den < -1.0e-10f || den > 1.0e-10f) ? (mM - mP) / den : 0.0f;
  return ((float)peak + delta) * df;
}

static void runHarmonicAnalysis() {
  HarmonicResult harm;

  // Voltage FFT (s_vBuf and s_fftBuf accessed only from this task -- no lock needed)
  prepareFFTInput(s_vBuf);
  halFft(s_fftBuf, FFT_N);
  extractHarmonics(harm.magV, harm.phaseV);

  // Current FFT (reuse s_fftBuf)
  prepareFFTInput(s_iBuf);
  halFft(s_fftBuf, FFT_N);
  extractHarmonics(harm.magI, harm.phaseI);

  float fftFreq = estimateFreqFromFFT();

  float irms_snap = 0.0f, pf_snap = 0.0f, qSign_snap = 0.0f, cf_snap = 0.0f;
  if (xSemaphoreTake(g_meterMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    irms_snap  = g_meter.current;
    pf_snap    = g_meter.powerFactor;
    qSign_snap = g_meter.qSign;
    cf_snap    = g_meter.crestFactor;
    xSemaphoreGive(g_meterMutex);
  }

  float i1     = harm.magI[0];
  float guard  = (i1 > 1.0e-9f) ? i1 : 1.0e-9f;
  float i3     = harm.magI[2];
  float i3grd  = (i3 > 1.0e-9f) ? i3 : 1.0e-9f;

  ClassifierFeatures feat;
  feat.pf      = (pf_snap >= 0.0f) ? pf_snap : -pf_snap;
  feat.qSign   = qSign_snap;
  feat.thdI    = computeThdI(harm.magI);
  feat.h3Ratio = harm.magI[2] / guard;
  feat.h5Ratio = harm.magI[4] / guard;
  feat.h7Ratio = harm.magI[5] / guard;
  feat.cf      = cf_snap;
  feat.oeRatio = (harm.magI[1] + harm.magI[3]) / guard;
  feat.h53Ratio = harm.magI[4] / i3grd;
  feat.pRms    = computeLogCurrent(irms_snap);

  if (xSemaphoreTake(g_featuresMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    g_features = feat;
    xSemaphoreGive(g_featuresMutex);
  }

  // Classify -- uses local feat and irms_snap; no mutex needed.
  // g_classByte is uint8_t; single-byte write is atomic on ESP32/STM32.
  ClassResult raw = classifyLoad(feat, irms_snap, DEFAULT_THRESHOLDS);
  if (raw.classId == LOAD_NONE) {
    // Mechanical relay guarantees open circuit; state is unambiguous.
    // Bypass the smoother so the dashboard reflects off-state immediately.
    // The ring buffer is NOT updated, preserving the prior class for relay-on restart.
    g_classByte = CLASS_BYTE_NONE;
  } else {
    classifierUpdate(s_clfState, raw);
    g_classByte = packClassByte(s_clfState);
  }

  // Sanity: ZC and FFT frequency estimates should agree to within 2 Hz
  float diff = fftFreq - s_freq.freqAvg;
  if (diff < 0.0f) diff = -diff;
  if (diff > 2.0f) {
    Serial.printf("[METER-AFE] Freq discrepancy: ZC=%.2f FFT=%.2f Hz\n",
                  s_freq.freqAvg, fftFreq);
  }
}

// =============================================================================
// Calibration load from NVS
// =============================================================================

static void loadCalibration() {
  // Nominal values from component spec (G=1 for voltage, G=2 for current)
  float kV_nom = (VREF / (1.0f * 8388608.0f)) * (R_LIMIT / R_SAMPLE);
  float kI_nom = (VREF / (2.0f * 8388608.0f)) * (CT_TURNS / R_BURDEN);

  s_cal.kV           = kV_nom;
  s_cal.kI           = kI_nom;
  s_cal.phaseCompRad = 0.0f;

  HalNvs_t h = halNvsOpen("afe-cal", true);
  if (h) {
    float tmp;
    if (halNvsGetBlob(h, "kv",     &tmp, sizeof(tmp)) == sizeof(tmp)) s_cal.kV = tmp;
    if (halNvsGetBlob(h, "ki",     &tmp, sizeof(tmp)) == sizeof(tmp)) s_cal.kI = tmp;
    if (halNvsGetBlob(h, "phcorr", &tmp, sizeof(tmp)) == sizeof(tmp))
      s_cal.phaseCompRad = tmp;
    halNvsClose(h);
  }
  Serial.printf("[METER-AFE] kV=%.4e kI=%.4e phCorr=%.4f\n",
                s_cal.kV, s_cal.kI, s_cal.phaseCompRad);
}

// =============================================================================
// meterTaskFn -- entry point (resolved at link time, declared in meter.h)
// g_meterMutex is created by nodeTdmaStartCommonTasks() before this task runs.
// =============================================================================

void meterTaskFn(void* /*params*/) {
  Serial.println("[METER-AFE] DSP pipeline task started");

  loadCalibration();
  initHannWindow();
  initFilterCompensation();
  updateReactiveDelay(60.0f);

  s_freq.init();
  s_acc      = CycleAccumulator{};
  s_zcPending = false;
  classifierStateInit(s_clfState);

  // g_featuresMutex is AFE-only; create it here before first FFT cycle
  g_featuresMutex = xSemaphoreCreateMutex();
  configASSERT(g_featuresMutex);

  halAdsInit(selectPins());
  halAdsStart();

  uint32_t lastFftMs = millis();

  while (true) {
    int32_t codeV = 0, codeI = 0;
    halAdsReadSample(&codeV, &codeI);
    processSample(codeV, codeI);

    if (s_zcPending) {
      s_zcPending = false;
      finaliseCycle();
    }

    if ((millis() - lastFftMs) >= 2000) {
      runHarmonicAnalysis();
      lastFftMs = millis();
    }
  }
}

#endif // NODE_TELEMETRY && METER_AFE
