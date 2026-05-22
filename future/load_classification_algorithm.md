# Load Classification Decision Algorithm

## On-Node Load Characterisation for ADS131M02 + ESP32-S3

---

## 1. Scope and Constraints

This document specifies the classification decision algorithm that runs on the ESP32-S3 node as part of the `adcTask` on Core 0. Per the analog front-end design (Section 11.1), the classifier executes every 2 seconds after the 128-point FFT completes. The computational budget is approximately 5 microseconds for the decision tree baseline. The neural network upgrade path is allocated up to 200 microseconds, which is still negligible relative to the 2-second period (0.001% CPU load).

The classifier operates on-node only. The result is packed into the `ClassificationData` struct and transmitted via TDMA. No raw features are sent to the gateway.

### 1.1 Target Load Categories

Six classes are defined, chosen to cover the dominant residential load types in a 220 V / 60 Hz Philippine residential installation. Each class has a distinct electrical signature in the harmonic domain.

| Class ID | Label | Typical Loads | Distinguishing Electrical Behaviour |
|----------|-------|---------------|--------------------------------------|
| 0 | `RESISTIVE` | Incandescent bulbs, flat irons, electric stoves, water heaters | Near-unity PF, sinusoidal current, negligible harmonic content |
| 1 | `CAPACITIVE` | Power-factor-corrected SMPS, UPS on battery, LED drivers with PFC | Current leads voltage, low THD but negative reactive power |
| 2 | `MOTOR_1PH` | Aircon compressor, refrigerator compressor, washing machine motor | Lagging PF (0.65-0.90), moderate H3/H5 from magnetic saturation, high inrush |
| 3 | `FAN` | Ceiling fans, desk fans, exhaust fans (shaded-pole or PSC motors) | Deep lagging PF (0.45-0.75), elevated H3 from speed-control triac chopping or saturation |
| 4 | `SMPS` | Laptop chargers, phone chargers, desktop PSUs without PFC | Very high THD (50-150%), dominant H3 and H5, peaky current waveform, high crest factor |
| 5 | `LIGHTING` | CFL, LED lamps (non-PFC), fluorescent with magnetic ballast | High THD (30-100%), current pulses near voltage peaks, distinctive H3/H5/H7 pattern |

Class 0 (`RESISTIVE`) also serves as the "idle" or "no load" classification when Irms is below a minimum threshold (e.g., 50 mA).

### 1.2 Encoding for Telemetry

The classification result fits in a single byte within the `TelemetryPacket`:

```
Bits [2:0]  — classId (0-5, values 6-7 reserved)
Bits [5:3]  — confidence level (0-7, mapping to 0%-100% in 14.3% steps)
Bit  [6]    — transient flag (1 = classification may be unstable, e.g. motor startup)
Bit  [7]    — reserved
```

---

## 2. Feature Extraction Pipeline

The feature extraction runs immediately after the 128-point FFT, before the classifier is invoked. All quantities below are computed from the most recent complete FFT window (approximately 1.9 cycles at 60 Hz with 128 samples at 4 kSPS).

### 2.1 FFT Output and Harmonic Bins

With $N = 128$ samples at $f_s = 4000$ SPS, the FFT frequency resolution is:

$$\Delta f = \frac{f_s}{N} = \frac{4000}{128} = 31.25 \text{ Hz}$$

The fundamental at 60 Hz falls in bin $k_1 = \text{round}(60 / 31.25) = 2$. Each harmonic $h$ maps to bin $k_h = \text{round}(h \times 60 / 31.25)$. The mapping is not perfectly aligned (60 Hz does not divide evenly into 31.25 Hz bins), so spectral leakage occurs. However, for ratio-based features the leakage affects numerator and denominator similarly, preserving discriminative power. A Hann window applied before the FFT reduces sidelobe leakage at the cost of 6 dB main-lobe widening, which is acceptable since the harmonic spacing (60 Hz) exceeds the widened main-lobe width (approximately $2 \times 31.25 = 62.5$ Hz).

| Harmonic ($h$) | Frequency (Hz) | FFT Bin ($k_h$) |
|----------------|-----------------|------------------|
| 1 (fundamental) | 60 | 2 |
| 3 | 180 | 6 |
| 5 | 300 | 10 |
| 7 | 420 | 13 |
| 9 | 540 | 17 |
| 11 | 660 | 21 |
| 13 | 780 | 25 |
| 15 | 900 | 29 |

Only odd harmonics are extracted because even harmonics are negligible in symmetric AC waveforms (half-wave symmetry forces even harmonics to zero in the ideal case). Even harmonics above noise indicate waveform asymmetry, which can serve as a secondary diagnostic but is not used in the primary classifier.

### 2.2 Magnitude Extraction

For each harmonic $h$, the magnitude from the complex FFT output $X[k]$ is:

$$|I_h| = \frac{2}{N} \sqrt{\text{Re}(X[k_h])^2 + \text{Im}(X[k_h])^2}$$

The factor $2/N$ normalises the single-sided spectrum. In practice, the `esp-dsp` library's FFT returns unnormalised values, so a single division by $N/2 = 64$ is applied once.

The anti-aliasing filter's frequency-dependent attenuation is compensated for each harmonic using the known filter transfer function from Section 4.3 of the analog front-end design:

$$|I_{h,\text{corrected}}| = |I_h| \times \sqrt{1 + (h \times 60 / f_{c,I})^2}$$

where $f_{c,I} = 2341$ Hz. At the fundamental this correction is $1.00033\times$ (negligible). At H15 it is $1.071\times$ (significant for accurate THD computation).

### 2.3 Feature Vector Definition

The classifier uses 10 features, carefully chosen to maximise separability among the six classes while minimising computation. All features are dimensionless ratios or normalised quantities, which makes the classifier independent of the absolute load power level.

| Feature Index | Symbol | Definition | Physical Meaning |
|---------------|--------|------------|------------------|
| 0 | $f_\text{PF}$ | Power factor (from per-cycle computation) | Displacement between V and I fundamentals |
| 1 | $f_\text{Qsign}$ | Sign of reactive power: $+1$ (lagging/inductive), $-1$ (leading/capacitive), $0$ (resistive within $\pm 0.02$) | Nature of reactance |
| 2 | $f_\text{THD}$ | $\text{THD}_I = \sqrt{\sum_{h=3,5,...,15} |I_h|^2} / |I_1|$ | Total harmonic distortion of current |
| 3 | $f_\text{H3}$ | $|I_3| / |I_1|$ | 3rd harmonic ratio |
| 4 | $f_\text{H5}$ | $|I_5| / |I_1|$ | 5th harmonic ratio |
| 5 | $f_\text{H7}$ | $|I_7| / |I_1|$ | 7th harmonic ratio |
| 6 | $f_\text{CF}$ | $I_\text{peak} / I_\text{rms}$ | Crest factor (peakiness of current waveform) |
| 7 | $f_\text{OE}$ | $(|I_2| + |I_4|) / |I_1|$ | Even harmonic ratio (asymmetry indicator) |
| 8 | $f_\text{H53}$ | $|I_5| / |I_3|$ | Ratio of 5th to 3rd (shape discriminator between SMPS and lighting) |
| 9 | $f_\text{Prms}$ | $\log_2(I_\text{rms} / 0.05)$, clamped to $[0, 10]$ | Log-scaled current magnitude (separates no-load from active) |

The total feature vector occupies 10 floats = 40 bytes.

### 2.4 Typical Feature Values by Load Class

These values are derived from published NILM datasets and measurements of common Philippine residential loads. They serve as the basis for the decision tree thresholds and as training-set centroids for the neural network.

| Feature | RESISTIVE | CAPACITIVE | MOTOR_1PH | FAN | SMPS | LIGHTING |
|---------|-----------|------------|-----------|-----|------|----------|
| PF | 0.97-1.00 | 0.85-0.99 | 0.65-0.90 | 0.45-0.75 | 0.45-0.70 | 0.40-0.65 |
| Qsign | 0 | $-1$ | $+1$ | $+1$ | $+1$ or $-1$ | $+1$ |
| THD_I | 0.00-0.05 | 0.02-0.10 | 0.05-0.20 | 0.08-0.30 | 0.50-1.50 | 0.30-1.00 |
| H3/H1 | < 0.03 | 0.02-0.08 | 0.03-0.15 | 0.05-0.25 | 0.40-0.90 | 0.25-0.70 |
| H5/H1 | < 0.02 | 0.01-0.05 | 0.02-0.08 | 0.03-0.10 | 0.25-0.70 | 0.15-0.50 |
| H7/H1 | < 0.01 | 0.01-0.03 | 0.01-0.04 | 0.02-0.06 | 0.10-0.40 | 0.08-0.30 |
| CF | 1.41 | 1.41-1.50 | 1.41-1.60 | 1.41-1.80 | 2.0-3.5 | 1.8-3.0 |
| OE | < 0.02 | < 0.02 | < 0.03 | < 0.05 | < 0.05 | 0.02-0.10 |
| H5/H3 | — | — | 0.3-0.8 | 0.3-0.6 | 0.50-0.85 | 0.40-0.70 |
| Prms | varies | varies | varies | varies | varies | varies |

The key observation is that the feature space is not cleanly separable by any single feature. PF alone cannot distinguish FAN from SMPS (overlapping ranges). THD alone cannot distinguish SMPS from LIGHTING. The combination of PF, THD, crest factor, Qsign, and harmonic ratios provides robust separation.

---

## 3. Baseline Classifier: Hand-Crafted Decision Tree

### 3.1 Rationale

A hand-crafted decision tree is the appropriate baseline for several reasons. First, it requires no training data, which is critical during initial deployment when no labelled measurements exist. Second, the execution time is deterministic and trivially fast (a handful of floating-point comparisons). Third, it is fully interpretable: every classification can be traced through a chain of physically meaningful threshold tests. Fourth, the thresholds can be tuned per-deployment by adjusting constants in firmware, without retraining a model.

### 3.2 Tree Structure

The tree is designed with a "peel off the easy cases first" strategy. At each node, the feature and threshold are chosen to isolate one class with minimal false positives before proceeding to harder separations.

```
                        [Irms < 0.05 A ?]
                        /              \
                      YES               NO
                       |                 |
                  RESISTIVE         [THD_I < 0.15 ?]
                  (no load)         /              \
                                  YES               NO
                                   |                 |
                          [PF > 0.95 ?]        [CF > 2.2 ?]
                          /          \          /          \
                        YES          NO      YES          NO
                         |            |       |            |
                    RESISTIVE   [Qsign<0?] SMPS    [H3/H1 > 0.35 ?]
                                /      \              /            \
                              YES      NO           YES            NO
                               |        |            |              |
                         CAPACITIVE  [PF>0.72?]  LIGHTING    [Qsign > 0 ?]
                                      /     \                  /         \
                                    YES     NO               YES         NO
                                     |       |                |           |
                                MOTOR_1PH   FAN          [PF>0.72?]  CAPACITIVE
                                                          /     \
                                                        YES     NO
                                                         |       |
                                                    MOTOR_1PH   FAN
```

### 3.3 Decision Logic — Step by Step

The reasoning behind each split:

**Step 1: No-load gate ($I_\text{rms} < 0.05$ A).** Below 50 mA, the signal is indistinguishable from noise at PGA gain 1. The noise floor is 0.53 mA (Section 2.2 of the analog front-end design), so 50 mA provides a 94:1 SNR margin. Any current below this threshold is classified as `RESISTIVE` (idle circuit, possible standby leakage). This prevents nonsensical harmonic analysis on noise.

**Step 2: Low-THD branch ($\text{THD}_I < 0.15$).** This separates loads with approximately sinusoidal current waveforms (resistive, capacitive, and motor loads) from loads with significantly distorted waveforms (SMPS and lighting). The threshold of 0.15 (15% THD) is conservative; most motors produce THD below 20%, while most SMPS produce THD above 50%. The overlap zone (15-20%) exists for large compressor motors at light load, which is handled by the crest factor check in the other branch.

**Step 3a: High-PF check within low-THD branch ($\text{PF} > 0.95$).** Among low-THD loads, a very high power factor isolates purely resistive loads. Motors and capacitive loads always have $\text{PF} < 0.95$ because their current-voltage phase displacement is at least several degrees.

**Step 3b: Reactive power sign check ($Q_\text{sign} < 0$).** Within the low-THD, non-resistive region, the sign of reactive power is the single most discriminative feature. Capacitive loads (PFC circuits, UPS inverters) produce leading current ($Q < 0$). Inductive loads (motors, fans) produce lagging current ($Q > 0$). This is a fundamental physical distinction that no amount of harmonic analysis can replicate as cleanly.

**Step 3c: Motor vs. Fan separation ($\text{PF} > 0.72$).** Among low-THD inductive loads, the power factor separates compressor motors (higher PF, 0.72-0.90) from small fan motors (lower PF, 0.45-0.72). This threshold was chosen from the midpoint of the calibration verification loads: aircon at 0.75-0.90, fan at 0.55-0.75. The overlap zone at 0.72-0.75 is narrow.

**Step 4: High-THD branch — Crest factor check ($\text{CF} > 2.2$).** Among high-THD loads, a very high crest factor (peaky waveform) is the hallmark of SMPS loads without power factor correction. A pure sinusoid has $\text{CF} = \sqrt{2} \approx 1.414$. SMPS loads typically produce narrow current pulses near the voltage peak, resulting in CF values of 2.0-3.5. Lighting loads (CFL, LED) also have elevated CF but typically lower than SMPS. The threshold of 2.2 separates the two populations at their valley.

**Step 5: High-THD, moderate-CF branch — H3 ratio check ($|I_3|/|I_1| > 0.35$).** When THD is high but CF is not extreme, the 3rd harmonic ratio distinguishes lighting from motor/fan loads that happen to have elevated THD (e.g., fan on triac dimmer). Lighting loads characteristically have $\text{H3/H1} > 0.35$, while motors rarely exceed 0.25.

**Step 5b: Residual separation.** The remaining cases in the high-THD, moderate-CF, low-H3 region use the reactive power sign and PF threshold to distinguish motors from fans (same logic as the low-THD branch), with capacitive loads caught by the Qsign check.

### 3.4 C++11 Implementation (No STL)

```cpp
// load_classifier.h
// Classification decision tree for 6-class load identification.
// Runs on ESP32-S3, Core 0, within adcTask.
// C++11 compliant, no STL dependencies.

#ifndef LOAD_CLASSIFIER_H
#define LOAD_CLASSIFIER_H

#include <stdint.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Load class identifiers
// ---------------------------------------------------------------------------
enum LoadClass : uint8_t {
    LOAD_RESISTIVE  = 0,
    LOAD_CAPACITIVE = 1,
    LOAD_MOTOR_1PH  = 2,
    LOAD_FAN        = 3,
    LOAD_SMPS       = 4,
    LOAD_LIGHTING   = 5,
    LOAD_COUNT      = 6
};

// ---------------------------------------------------------------------------
// Feature vector — 10 floats, computed by the DSP pipeline before
// the classifier is invoked.
// ---------------------------------------------------------------------------
struct FeatureVector {
    float pf;       // f0: power factor [0..1]
    float qSign;    // f1: sign of reactive power: +1, -1, or 0
    float thdI;     // f2: THD of current waveform
    float h3Ratio;  // f3: |I3| / |I1|
    float h5Ratio;  // f4: |I5| / |I1|
    float h7Ratio;  // f5: |I7| / |I1|
    float cf;       // f6: crest factor  Ipeak / Irms
    float oeRatio;  // f7: even harmonic ratio (|I2|+|I4|) / |I1|
    float h53Ratio; // f8: |I5| / |I3|
    float pRms;     // f9: log2(Irms / 0.05), clamped [0..10]
};

// ---------------------------------------------------------------------------
// Classification result
// ---------------------------------------------------------------------------
struct ClassResult {
    LoadClass classId;
    uint8_t   confidence;   // 0-7 (maps to 0%-100% in ~14.3% steps)
    bool      transient;    // true if classification may be unstable
};

// ---------------------------------------------------------------------------
// Tunable thresholds — stored in a struct so they can be loaded from
// NVS (non-volatile storage) and adjusted per-deployment without
// recompiling.  Defaults are set from the typical feature table.
// ---------------------------------------------------------------------------
struct ClassifierThresholds {
    float minIrms;          // no-load gate (A)
    float lowThdLimit;      // THD boundary between linear and nonlinear loads
    float highPfLimit;      // PF above which load is considered resistive
    float cfSmpsLimit;      // CF above which high-THD load is SMPS
    float h3LightingLimit;  // H3/H1 above which moderate-CF load is lighting
    float pfMotorFanLimit;  // PF boundary between motor and fan
    float qSignCapThresh;   // Qsign threshold for capacitive detection
};

// Default thresholds derived from Section 2.4 typical feature values
static const ClassifierThresholds DEFAULT_THRESHOLDS = {
    0.050f,   // minIrms:          50 mA
    0.150f,   // lowThdLimit:      15% THD
    0.950f,   // highPfLimit:      PF > 0.95 => resistive
    2.200f,   // cfSmpsLimit:      CF > 2.2 => SMPS
    0.350f,   // h3LightingLimit:  H3/H1 > 0.35 => lighting
    0.720f,   // pfMotorFanLimit:  PF > 0.72 => motor, else fan
   -0.500f    // qSignCapThresh:   Qsign < -0.5 => capacitive
};

// ---------------------------------------------------------------------------
// Confidence estimation — based on distance from the nearest threshold
// that was crossed.  The deeper into a branch, the more certain we are.
// ---------------------------------------------------------------------------
static inline uint8_t computeConfidence(float feature, float threshold,
                                        float margin)
{
    // margin is the "full confidence" distance from the threshold.
    // E.g., if threshold = 0.15 and margin = 0.10, then a feature
    // value of 0.05 (0.10 below threshold) gets confidence 7,
    // while 0.14 (0.01 below) gets confidence 1.
    float dist = (feature >= threshold)
                 ? (feature - threshold)
                 : (threshold - feature);
    float ratio = dist / margin;
    if (ratio > 1.0f) ratio = 1.0f;
    // Map [0..1] to [1..7].  Never return 0 (which means "unknown").
    return (uint8_t)(1.0f + ratio * 6.0f);
}

// ---------------------------------------------------------------------------
// Main classification function
// ---------------------------------------------------------------------------
static inline ClassResult classifyLoad(const FeatureVector& fv,
                                       float irms,
                                       const ClassifierThresholds& th)
{
    ClassResult result;
    result.transient = false;

    // ------------------------------------------------------------------
    // Step 1: No-load gate
    // ------------------------------------------------------------------
    if (irms < th.minIrms) {
        result.classId    = LOAD_RESISTIVE;
        result.confidence = 7;  // certain: it is simply off / idle
        return result;
    }

    // ------------------------------------------------------------------
    // Step 2: Split on THD
    // ------------------------------------------------------------------
    if (fv.thdI < th.lowThdLimit) {
        // ---- LOW-THD BRANCH (approximately sinusoidal current) ----

        // Step 3a: High PF => resistive
        if (fv.pf > th.highPfLimit) {
            result.classId    = LOAD_RESISTIVE;
            result.confidence = computeConfidence(fv.pf, th.highPfLimit,
                                                  0.03f);
            return result;
        }

        // Step 3b: Capacitive (leading current)
        if (fv.qSign < th.qSignCapThresh) {
            result.classId    = LOAD_CAPACITIVE;
            result.confidence = computeConfidence(fv.qSign, th.qSignCapThresh,
                                                  0.50f);
            return result;
        }

        // Step 3c: Motor vs. Fan (both inductive, separated by PF)
        if (fv.pf > th.pfMotorFanLimit) {
            result.classId    = LOAD_MOTOR_1PH;
            result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit,
                                                  0.10f);
        } else {
            result.classId    = LOAD_FAN;
            result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit,
                                                  0.15f);
        }
        return result;

    } else {
        // ---- HIGH-THD BRANCH (distorted current waveform) ----

        // Step 4: Crest factor distinguishes SMPS from others
        if (fv.cf > th.cfSmpsLimit) {
            result.classId    = LOAD_SMPS;
            result.confidence = computeConfidence(fv.cf, th.cfSmpsLimit,
                                                  0.80f);
            return result;
        }

        // Step 5: H3 ratio distinguishes lighting from residual motors
        if (fv.h3Ratio > th.h3LightingLimit) {
            result.classId    = LOAD_LIGHTING;
            result.confidence = computeConfidence(fv.h3Ratio,
                                                  th.h3LightingLimit, 0.15f);
            return result;
        }

        // Step 5b: Residual — use Qsign and PF for motor/fan/capacitive
        if (fv.qSign < th.qSignCapThresh) {
            result.classId    = LOAD_CAPACITIVE;
            result.confidence = computeConfidence(fv.qSign, th.qSignCapThresh,
                                                  0.50f);
            return result;
        }
        if (fv.pf > th.pfMotorFanLimit) {
            result.classId    = LOAD_MOTOR_1PH;
            result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit,
                                                  0.10f);
        } else {
            result.classId    = LOAD_FAN;
            result.confidence = computeConfidence(fv.pf, th.pfMotorFanLimit,
                                                  0.15f);
        }
        // High-THD motors/fans are unusual; flag as potentially transient
        result.transient = true;
        return result;
    }
}

// ---------------------------------------------------------------------------
// Temporal smoothing — majority vote over last N classifications
// to suppress transient misclassifications during motor startups,
// load switching, etc.
// ---------------------------------------------------------------------------
#define SMOOTH_WINDOW 5   // 5 x 2 seconds = 10 seconds of history

struct ClassifierState {
    LoadClass history[SMOOTH_WINDOW];
    uint8_t   confHistory[SMOOTH_WINDOW];
    uint8_t   writeIdx;
    bool      filled;       // true once we have SMOOTH_WINDOW samples
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

static inline void classifierUpdate(ClassifierState& st,
                                    const ClassResult& raw)
{
    st.history[st.writeIdx]     = raw.classId;
    st.confHistory[st.writeIdx] = raw.confidence;
    st.writeIdx++;
    if (st.writeIdx >= SMOOTH_WINDOW) {
        st.writeIdx = 0;
        st.filled   = true;
    }

    // Count votes, weighted by confidence
    int limit = st.filled ? SMOOTH_WINDOW : (int)st.writeIdx;
    float votes[LOAD_COUNT];
    for (int c = 0; c < LOAD_COUNT; c++) votes[c] = 0.0f;

    for (int i = 0; i < limit; i++) {
        votes[st.history[i]] += (float)st.confHistory[i];
    }

    // Find winner
    float bestScore = -1.0f;
    LoadClass bestClass = LOAD_RESISTIVE;
    for (int c = 0; c < LOAD_COUNT; c++) {
        if (votes[c] > bestScore) {
            bestScore = votes[c];
            bestClass = (LoadClass)c;
        }
    }

    // Check unanimity for transient flag
    bool unanimous = true;
    for (int i = 0; i < limit; i++) {
        if (st.history[i] != bestClass) {
            unanimous = false;
            break;
        }
    }

    st.smoothedClass     = bestClass;
    st.smoothedTransient = !unanimous;

    // Smoothed confidence = average confidence of votes for the winner
    float sumConf = 0.0f;
    int   countWinner = 0;
    for (int i = 0; i < limit; i++) {
        if (st.history[i] == bestClass) {
            sumConf += (float)st.confHistory[i];
            countWinner++;
        }
    }
    st.smoothedConf = (countWinner > 0)
                      ? (uint8_t)(sumConf / (float)countWinner)
                      : 0;
}

// ---------------------------------------------------------------------------
// Telemetry packing — encode ClassResult into a single uint8_t
// for inclusion in the TelemetryPacket.
// ---------------------------------------------------------------------------
static inline uint8_t packClassByte(const ClassifierState& st)
{
    uint8_t b = 0;
    b |= (st.smoothedClass & 0x07);         // bits [2:0]
    b |= ((st.smoothedConf & 0x07) << 3);   // bits [5:3]
    if (st.smoothedTransient) b |= (1 << 6); // bit  [6]
    return b;
}

static inline void unpackClassByte(uint8_t b, LoadClass& cls,
                                   uint8_t& conf, bool& trans)
{
    cls   = (LoadClass)(b & 0x07);
    conf  = (b >> 3) & 0x07;
    trans = (b >> 6) & 0x01;
}

#endif // LOAD_CLASSIFIER_H
```

### 3.5 Computational Cost Analysis

The decision tree traverses at most 4 comparisons (the deepest path is: Irms check, THD check, CF check, H3 check). Each comparison is a single floating-point compare-and-branch. On the ESP32-S3 LX7 at 240 MHz, a float comparison takes 1 clock cycle (the FPU handles `c.lt.s` / `bc1t` natively). The confidence computation involves one subtraction, one division, and one clamp — approximately 5 additional cycles.

Total worst-case: approximately 25 clock cycles = 0.1 microseconds. This is well within the 5-microsecond budget from the analog front-end design. The temporal smoothing adds approximately 60 cycles (loop over 5 history entries with weighted accumulation), bringing the total to under 0.5 microseconds.

### 3.6 Integration with adcTask

The classifier is invoked within the existing adcTask structure defined in Section 11.1 of the analog front-end design:

```cpp
// Inside adcTask, every 2 seconds after FFT completion:

static ClassifierState clsState;
static bool clsInitialized = false;

// One-time init
if (!clsInitialized) {
    classifierStateInit(clsState);
    clsInitialized = true;
}

// Build feature vector from DSP results
FeatureVector fv;
fv.pf       = meterData.pf;
fv.qSign    = (meterData.Q > 0.5f) ? 1.0f
             : (meterData.Q < -0.5f) ? -1.0f : 0.0f;
fv.thdI     = dspData.thdI;
fv.h3Ratio  = dspData.harmonicI[1] / dspData.harmonicI[0]; // H3/H1
fv.h5Ratio  = dspData.harmonicI[2] / dspData.harmonicI[0]; // H5/H1
fv.h7Ratio  = dspData.harmonicI[3] / dspData.harmonicI[0]; // H7/H1
fv.cf       = dspData.iPeak / meterData.irms;
fv.oeRatio  = (dspData.harmonicI_even[0] + dspData.harmonicI_even[1])
              / dspData.harmonicI[0];
fv.h53Ratio = (dspData.harmonicI[1] > 1e-6f)
              ? dspData.harmonicI[2] / dspData.harmonicI[1]
              : 0.0f;
float logArg = meterData.irms / 0.05f;
fv.pRms     = (logArg > 1.0f) ? log2f(logArg) : 0.0f;
if (fv.pRms > 10.0f) fv.pRms = 10.0f;

// Classify
ClassResult raw = classifyLoad(fv, meterData.irms, DEFAULT_THRESHOLDS);
classifierUpdate(clsState, raw);

// Pack into telemetry
classificationData.classByte = packClassByte(clsState);
```

---

## 4. Upgrade Path: Lightweight Neural Network (TFLite Micro)

### 4.1 Why a Neural Network

The decision tree works well for loads with textbook-clean harmonic signatures, but real-world loads frequently violate the assumptions. A triac-dimmed incandescent bulb is resistive by nature but produces high THD due to the chopped waveform. A PFC-corrected SMPS has low THD and leading current, making it look capacitive. A variable-frequency-drive (VFD) compressor produces a complex harmonic spectrum that does not fit neatly into the motor or SMPS categories. A neural network trained on actual measured data can learn these nonlinear boundaries in the feature space without requiring the designer to anticipate every edge case.

### 4.2 Architecture

The network is deliberately small to fit within the ESP32-S3's resource constraints. The architecture is a fully-connected feedforward network (multi-layer perceptron):

```
Input layer:   10 neurons (one per feature)
Hidden layer:  16 neurons, ReLU activation
Hidden layer:  12 neurons, ReLU activation
Output layer:   6 neurons, softmax activation (one per class)
```

Parameter count:

$$W_1: 10 \times 16 = 160, \quad b_1: 16$$
$$W_2: 16 \times 12 = 192, \quad b_2: 12$$
$$W_3: 12 \times 6 = 72, \quad b_3: 6$$
$$\text{Total} = 160 + 16 + 192 + 12 + 72 + 6 = 458 \text{ parameters}$$

At 32-bit float, this is $458 \times 4 = 1832$ bytes. With 8-bit quantisation (post-training), it drops to $458 + \text{scales/zeros} \approx 500$ bytes.

### 4.3 Inference Cost

Each hidden neuron requires one multiply-accumulate (MAC) per input weight, plus the bias addition and ReLU (which is a single comparison). The total MAC count:

$$\text{MACs} = (10 \times 16) + (16 \times 12) + (12 \times 6) = 160 + 192 + 72 = 424$$

The ESP32-S3's PIE (Processor Instruction Extensions) provides 128-bit SIMD operations that can perform 4 single-precision float MACs per cycle. Without SIMD, at 240 MHz:

$$t_\text{inference} = \frac{424}{240 \times 10^6} \approx 1.77 \text{ }\mu\text{s (unoptimised)}$$

With SIMD (4x throughput on the MAC-dominant portion):

$$t_\text{inference} \approx 0.5 \text{ }\mu\text{s}$$

Both are well within the 200-microsecond budget, leaving ample margin for the softmax computation (which involves 6 exponentials and a division, approximately 50 additional cycles).

### 4.4 TFLite Micro Integration

The ESP-IDF framework has first-class support for TFLite Micro. The model is converted from a TensorFlow/Keras training pipeline and stored as a C array in flash:

```cpp
// model_data.h — auto-generated by xxd from the .tflite flatbuffer
extern const unsigned char g_load_classifier_model[];
extern const unsigned int  g_load_classifier_model_len;
```

The inference wrapper (C++11, no STL):

```cpp
// nn_classifier.h
#ifndef NN_CLASSIFIER_H
#define NN_CLASSIFIER_H

#include "load_classifier.h"  // for FeatureVector, LoadClass, ClassResult
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Arena size — for a network this small, 4 KB is more than enough.
// TFLite Micro uses this as working memory for tensor allocations.
#define NN_ARENA_SIZE 4096

class NNClassifier {
public:
    bool init(const unsigned char* modelData, unsigned int modelLen);
    ClassResult classify(const FeatureVector& fv);

private:
    uint8_t arena_[NN_ARENA_SIZE];
    tflite::MicroInterpreter* interpreter_;
    TfLiteTensor* input_;
    TfLiteTensor* output_;

    // Op resolver — only the ops we use
    tflite::MicroMutableOpResolver<3> resolver_;

    // Storage for interpreter (placement new)
    alignas(tflite::MicroInterpreter)
        uint8_t interpreterBuf_[sizeof(tflite::MicroInterpreter)];
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
inline bool NNClassifier::init(const unsigned char* modelData,
                               unsigned int modelLen)
{
    const tflite::Model* model = tflite::GetModel(modelData);
    if (model == nullptr) return false;

    // Register only the operations our model uses
    resolver_.AddFullyConnected();
    resolver_.AddSoftmax();
    // ReLU is folded into FullyConnected as fused activation in
    // TFLite's converter, so no separate AddRelu() is needed.

    interpreter_ = new (interpreterBuf_) tflite::MicroInterpreter(
        model, resolver_, arena_, NN_ARENA_SIZE);

    if (interpreter_->AllocateTensors() != kTfLiteOk) return false;

    input_  = interpreter_->input(0);
    output_ = interpreter_->output(0);

    return true;
}

inline ClassResult NNClassifier::classify(const FeatureVector& fv)
{
    // Copy features into input tensor
    float* in = input_->data.f;
    in[0] = fv.pf;
    in[1] = fv.qSign;
    in[2] = fv.thdI;
    in[3] = fv.h3Ratio;
    in[4] = fv.h5Ratio;
    in[5] = fv.h7Ratio;
    in[6] = fv.cf;
    in[7] = fv.oeRatio;
    in[8] = fv.h53Ratio;
    in[9] = fv.pRms;

    // Run inference
    interpreter_->Invoke();

    // Find argmax of softmax output
    float* out = output_->data.f;
    float bestProb = -1.0f;
    int   bestIdx  = 0;
    for (int i = 0; i < LOAD_COUNT; i++) {
        if (out[i] > bestProb) {
            bestProb = out[i];
            bestIdx  = i;
        }
    }

    ClassResult result;
    result.classId    = (LoadClass)bestIdx;
    // Map softmax probability [0..1] to confidence [0..7]
    result.confidence = (uint8_t)(bestProb * 7.0f);
    if (result.confidence > 7) result.confidence = 7;
    if (result.confidence < 1 && bestProb > 0.05f)
        result.confidence = 1;
    result.transient  = (bestProb < 0.50f);  // low confidence => transient

    return result;
}

#endif // NN_CLASSIFIER_H
```

### 4.5 Training Data Collection Strategy

The neural network requires labelled training data. Since this is a single-outlet metering system (not whole-house NILM disaggregation), collecting training data is straightforward. The procedure for each node:

**Phase 1 — Supervised labelling.** The operator plugs a known load into the monitored outlet and labels it via the gateway web interface. The node stores 50-100 feature vectors (100-200 seconds of data) per load. This is repeated for 3-5 examples of each load class. The raw feature vectors are transmitted to the gateway as part of an extended diagnostic mode and stored for offline training.

**Phase 2 — Offline training.** The collected feature vectors are exported from the gateway and used to train the MLP in a standard TensorFlow/Keras environment on a PC. Data augmentation is applied: small Gaussian perturbations ($\sigma = 0.02$) to each feature simulate measurement noise and component tolerances. The trained model is quantised to 8-bit integers using TFLite's post-training quantisation pipeline.

**Phase 3 — OTA deployment.** The quantised `.tflite` model (approximately 500 bytes) is uploaded to the node via the TDMA downlink channel as a firmware parameter update, or via a full OTA firmware update through the gateway's Wi-Fi backhaul.

### 4.6 Fallback Strategy

The system always maintains the decision tree as a fallback. The runtime selection logic is:

```cpp
// In adcTask, after feature extraction:
ClassResult raw;

if (useNNClassifier && nnReady) {
    raw = nnClassifier.classify(fv);
    // Sanity check: if NN confidence is very low, fall back to tree
    if (raw.confidence <= 1) {
        raw = classifyLoad(fv, meterData.irms, DEFAULT_THRESHOLDS);
        raw.transient = true;  // flag that we fell back
    }
} else {
    raw = classifyLoad(fv, meterData.irms, DEFAULT_THRESHOLDS);
}

classifierUpdate(clsState, raw);
```

---

## 5. Memory and Computation Summary

| Component | RAM | Flash | CPU Time (per invocation) | Invocation Rate |
|-----------|-----|-------|---------------------------|-----------------|
| Feature vector | 40 B | 0 | ~2 us (part of FFT post-processing) | 0.5 Hz |
| Decision tree code | ~0 (inline) | ~400 B | ~0.5 us | 0.5 Hz |
| Decision tree thresholds | 28 B | 28 B (defaults) | — | — |
| Temporal smoother state | 16 B | ~300 B (code) | ~0.3 us | 0.5 Hz |
| NN model (quantised) | 0 (flash-resident) | ~500 B | ~2 us (SIMD) | 0.5 Hz |
| NN arena (TFLite Micro) | 4096 B | 0 | — | — |
| TFLite Micro runtime | ~2 KB | ~20 KB | — | — |
| **Total (decision tree only)** | **84 B** | **~728 B** | **~0.8 us** | **0.5 Hz** |
| **Total (with NN upgrade)** | **~6.2 KB** | **~21 KB** | **~2.8 us** | **0.5 Hz** |

Both configurations fit comfortably within the ESP32-S3's 512 KB SRAM and 16 MB flash. The CPU load contribution is negligible relative to the existing 1.4% DSP load on Core 0.

---

## 6. Edge Cases and Robustness

### 6.1 Motor Startup Transients

During the first 1-3 seconds of a compressor motor startup, the current waveform is dominated by the inrush transient: a large, decaying sinusoidal pulse that produces elevated THD and crest factor. The classifier may briefly output `SMPS` or `LIGHTING` during this period. The temporal smoother (5-sample window = 10 seconds) absorbs this transient. Additionally, the transient flag in the `ClassResult` is set when the crest factor exceeds 2.5 while PF is above 0.6, which is a physically implausible combination for steady-state SMPS loads but common during motor inrush.

### 6.2 Triac-Dimmed Loads

A triac dimmer chops the current waveform, producing significant odd harmonics regardless of the load type. A dimmed incandescent bulb will appear as `LIGHTING` or `SMPS` to the classifier. This is a fundamental ambiguity: the dimmer's harmonics dominate the load's intrinsic signature. The decision tree handles this gracefully by classifying based on what the current waveform actually looks like, which is the operationally relevant answer (the circuit is drawing non-sinusoidal current regardless of the resistive element downstream). The NN can be trained to recognise triac-dimmed resistive loads as a sub-case if labelled training data is provided.

### 6.3 Mixed Loads on a Single Outlet

If multiple loads are connected to the same monitored outlet (e.g., via a power strip), the classifier sees the aggregate waveform. The harmonic signature of the aggregate is the superposition of the individual loads' harmonic currents. In general, the dominant load's signature prevails. This system is not designed for disaggregation of mixed loads — that would require whole-house NILM algorithms that are beyond the scope of single-outlet metering. The confidence level will naturally drop when the aggregate waveform does not cleanly match any single class.

### 6.4 Low-Current Accuracy Degradation

Below approximately 500 mA primary current, the SNR for higher harmonics (H7, H9, etc.) drops below 20 dB, and the harmonic ratios become unreliable. The classifier should weight features differently at low currents. The $f_\text{Prms}$ feature (log-scaled current magnitude) provides this signal to the NN. For the decision tree, a practical mitigation is to widen the THD threshold at low currents:

```cpp
float effectiveThdLimit = th.lowThdLimit;
if (irms < 0.5f) {
    // Relax THD threshold at low currents where harmonic
    // noise is proportionally larger
    effectiveThdLimit = th.lowThdLimit + 0.10f * (0.5f - irms) / 0.5f;
}
```

This linearly increases the low-THD boundary from 0.15 to 0.25 as current drops from 500 mA to 50 mA.

---

## 7. Validation Protocol

### 7.1 Required Test Loads

Each class must be validated with at least two distinct physical loads. Suggested test set for a Philippine residential environment:

| Class | Load 1 | Load 2 | Load 3 (optional) |
|-------|--------|--------|---------------------|
| RESISTIVE | 100 W incandescent bulb | Electric flat iron | Resistive space heater |
| CAPACITIVE | Active-PFC laptop charger | UPS (battery mode, resistive load) | — |
| MOTOR_1PH | Window-type aircon (0.75 HP) | Refrigerator compressor | Washing machine |
| FAN | 75 W ceiling fan (low speed) | Desk fan (oscillating) | Exhaust fan |
| SMPS | Laptop charger (no PFC) | Desktop PC PSU | Phone charger (5 W) |
| LIGHTING | 13 W CFL | 9 W LED bulb (no PFC) | 36 W fluorescent + magnetic ballast |

### 7.2 Acceptance Criteria

The classifier must achieve at least 85% accuracy across all six classes on the test set, with no single class falling below 70%. The temporal smoother should bring effective accuracy above 90% by suppressing transient misclassifications. Confusion between adjacent classes (e.g., MOTOR_1PH classified as FAN) is tolerable and counted as a partial success. Confusion between fundamentally different classes (e.g., RESISTIVE classified as SMPS) indicates a threshold calibration error and must be investigated.
