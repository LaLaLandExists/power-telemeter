# Signal Processing Pipeline: Raw Waveforms to Metering Readings

## ADS131M02 + ESP32-S3 / STM32F411 — Portable DSP Architecture

---

## 1. Pipeline Overview

The signal processing pipeline transforms raw 24-bit ADC codes from the ADS131M02 into calibrated electrical readings and derived features. It operates at three distinct time scales, each triggered by the one above it:

```
Time Scale 0: PER-SAMPLE (4000 Hz — every 250 μs)
  ├── ADC code → physical voltage/current conversion
  ├── Accumulate running sums: V², I², V×I, V_q×I
  ├── Zero-crossing detection on voltage waveform
  └── Track peak |I| for crest factor

Time Scale 1: PER-CYCLE (≈60 Hz — every ≈16.67 ms)
  ├── Finalise: Vrms, Irms, P, Q, S, PF
  ├── Compute frequency from zero-crossing timestamps
  ├── Accumulate energy (Wh increment)
  ├── Update MeterData shared struct (mutex)
  └── Copy samples into FFT staging buffer

Time Scale 2: PERIODIC (0.5 Hz — every 2 seconds)
  ├── Apply Hann window to 128-sample buffer
  ├── Run 128-point FFT on V and I channels
  ├── Extract harmonic magnitudes (H1–H15) and phases
  ├── Compute THD, crest factor, harmonic ratios
  ├── Frequency cross-check via FFT bin interpolation
  └── Update ClassifierFeatures shared struct (mutex)
```

---

## 2. Stage 0 — ADC Code to Physical Units

### 2.1 Raw Code Format

The ADS131M02 outputs 24-bit two's complement codes. The conversion from raw code to differential input voltage at the ADC pins is:

$$V_{\text{ADC}} = \frac{\text{code}}{2^{23}} \times \frac{V_{\text{ref}}}{G} = \frac{\text{code}}{2^{23}} \times \frac{1.2}{G}$$

where $G$ is the PGA gain setting (1, 2, 4, 8, 16, 32, 64, 128) and `code` is a signed 24-bit integer in the range $[-2^{23}, +2^{23} - 1]$.

In practice, we avoid the floating-point division per sample by precomputing a scale factor:

$$k_{\text{ADC}} = \frac{V_{\text{ref}}}{G \times 2^{23}} = \frac{1.2}{G \times 8{,}388{,}608}$$

At the default $G = 2$: $k_{\text{ADC}} = 7.1526 \times 10^{-8}$ V/code.

### 2.2 Sign Extension

The 24-bit code arrives packed into three bytes (MSB first) from the SPI frame. It must be sign-extended to a 32-bit integer before use:

```cpp
// C++11, no STL. Sign-extend 24-bit two's complement to int32_t.
static inline int32_t signExtend24(uint32_t raw)
{
    // raw contains the 24-bit value in bits [23:0].
    // If bit 23 is set, the value is negative — fill bits [31:24] with 1s.
    if (raw & 0x00800000u)
        return (int32_t)(raw | 0xFF000000u);
    else
        return (int32_t)(raw & 0x007FFFFFu);
}
```

### 2.3 Scaling to Physical Units

The voltage and current channels have different transducer transfer functions, established in the analog front-end design.

**Voltage channel** — ZMPT101B with $R_L = 112$ k$\Omega$, $R_s = 330$ $\Omega$, turns ratio 1:1:

$$V_{\text{mains}}[n] = V_{\text{ADC,V}}[n] \times \frac{R_L}{R_s} = V_{\text{ADC,V}}[n] \times \frac{112{,}000}{330} = V_{\text{ADC,V}}[n] \times 339.39$$

Equivalently, the composite scale factor from code to mains voltage:

$$k_V = k_{\text{ADC}} \times \frac{R_L}{R_s}$$

At $G = 1$: $k_V = \frac{1.2}{8{,}388{,}608} \times 339.39 = 4.855 \times 10^{-5}$ V/code.

**Current channel** — CT with turns ratio $n = 1000$, burden $R_b = 10$ $\Omega$:

$$I_{\text{primary}}[n] = V_{\text{ADC,I}}[n] \times \frac{n}{R_b} = V_{\text{ADC,I}}[n] \times \frac{1000}{10} = V_{\text{ADC,I}}[n] \times 100$$

The composite scale factor from code to primary current:

$$k_I = k_{\text{ADC}} \times \frac{n}{R_b}$$

At the default $G = 2$: $k_I = 7.1526 \times 10^{-8} \times 100 = 7.1526 \times 10^{-6}$ A/code.

### 2.4 Calibration Adjustment

The raw scale factors above assume ideal components. After factory or field calibration, a gain correction factor $c_V$ and $c_I$ (both close to 1.0, stored in NVS) are applied:

$$v[n] = \text{code}_V[n] \times k_V \times c_V$$
$$i[n] = \text{code}_I[n] \times k_I \times c_I$$

Offset calibration is handled in hardware by the ADS131M02's OCAL registers (programmed at startup from NVS). The DC block filter (DCBLOCK register, recommended setting 0x06 = $a = 1/128$, corner frequency 5.0 Hz at 4 kSPS) removes any residual DC offset digitally inside the ADC, which is important because the RMS computation assumes a zero-mean signal.

### 2.5 Code Structure — Per-Sample Processing

```cpp
// --------------------------------------------------------------------------
// Calibration constants, loaded from NVS at startup.
// --------------------------------------------------------------------------
struct CalibrationParams {
    float kV;           // code-to-Vmains scale factor (includes c_V)
    float kI;           // code-to-Iprimary scale factor (includes c_I)
    float phaseCompRad; // residual phase error in radians (for software correction)
};

// --------------------------------------------------------------------------
// Running accumulators, reset at each cycle boundary.
// --------------------------------------------------------------------------
struct CycleAccumulator {
    float sumV2;        // Σ v[n]²
    float sumI2;        // Σ i[n]²
    float sumVI;        // Σ v[n]·i[n]        (real power accumulator)
    float sumVqI;       // Σ v_q[n]·i[n]      (reactive power accumulator)
    float peakI;        // max |i[n]| in this cycle
    int32_t nSamples;   // sample count in this cycle
};

// --------------------------------------------------------------------------
// Per-sample processing — called from DRDY ISR or DMA completion callback.
// Runs at 4 kHz. Must complete in < 5 μs.
// --------------------------------------------------------------------------
static void processSample(int32_t codeV, int32_t codeI,
                          const CalibrationParams& cal,
                          CycleAccumulator& acc,
                          float* vBuf, float* iBuf,
                          int32_t& bufIdx,
                          int32_t bufLen)
{
    // --- Scale to physical units ---
    float v = (float)codeV * cal.kV;
    float i = (float)codeI * cal.kI;

    // --- Store into circular buffer for FFT and ZC ---
    vBuf[bufIdx] = v;
    iBuf[bufIdx] = i;
    bufIdx = (bufIdx + 1) % bufLen;  // bufLen = 256 typically

    // --- Accumulate for RMS ---
    acc.sumV2 += v * v;
    acc.sumI2 += i * i;

    // --- Accumulate for real power: P = mean(v·i) ---
    acc.sumVI += v * i;

    // --- Track peak current for crest factor ---
    float absI = (i >= 0.0f) ? i : -i;
    if (absI > acc.peakI)
        acc.peakI = absI;

    acc.nSamples++;
}
```

### 2.6 Reactive Power — 90-Degree Delayed Voltage

The time-domain reactive power formula requires a voltage waveform phase-shifted by exactly 90 degrees:

$$Q = \frac{1}{N} \sum_{n=0}^{N-1} v_q[n] \cdot i[n]$$

where $v_q[n] = v[n - n_{90}]$ is the voltage waveform delayed by a quarter cycle. At 60 Hz and 4 kSPS:

$$n_{90} = \frac{f_s}{4 \times f_{\text{mains}}} = \frac{4000}{4 \times 60} = 16.667 \text{ samples}$$

This is not an integer. Two approaches exist:

**Approach A — Integer delay with correction.** Use $n_{90} = 17$ samples. The phase error from the non-integer delay is:

$$\Delta\phi = 360° \times 60 \times \frac{17 - 16.667}{4000} = 360° \times 60 \times 8.333 \times 10^{-5} = 1.8°$$

This introduces a systematic error in $Q$ of approximately $\sin(1.8°) / \sin(90°) = 3.1\%$. Correctable by a constant factor $\cos(\Delta\phi)$ applied to the final $Q$ result, but it couples accuracy to frequency stability.

**Approach B — Linear interpolation (recommended).** Interpolate between samples at indices $n - 16$ and $n - 17$:

$$v_q[n] = (1 - \alpha) \cdot v[n - 17] + \alpha \cdot v[n - 16]$$

where $\alpha = 17 - n_{90} = 17 - 16.667 = 0.333$. This gives sub-sample accuracy with one multiply-add of overhead. The fractional delay $\alpha$ should be recomputed whenever the measured frequency changes (see Section 4):

$$n_{90}(f) = \frac{f_s}{4f}, \qquad \lfloor n_{90} \rfloor = \text{integer part}, \qquad \alpha = \lceil n_{90} \rceil - n_{90}$$

```cpp
// --------------------------------------------------------------------------
// Compute v_q[n] using linearly-interpolated quarter-cycle delay.
// vBuf is a circular buffer of length bufLen.
// writeIdx is the current write position (one past the latest sample).
// n90Int and n90Frac are precomputed from measured frequency.
// --------------------------------------------------------------------------
static inline float getVq(const float* vBuf, int32_t bufLen,
                          int32_t writeIdx,
                          int32_t n90Int, float n90Frac)
{
    // Index of the sample (n90Int + 1) samples ago
    int32_t idxOlder = (writeIdx - n90Int - 1 + bufLen) % bufLen;
    // Index of the sample n90Int samples ago
    int32_t idxNewer = (writeIdx - n90Int + bufLen) % bufLen;

    // Linear interpolation: fractional position between older and newer
    return (1.0f - n90Frac) * vBuf[idxOlder] + n90Frac * vBuf[idxNewer];
}
```

The reactive power accumulator then becomes:

```cpp
    // Inside processSample(), after storing v into vBuf:
    float vq = getVq(vBuf, bufLen, bufIdx, n90Int, n90Frac);
    acc.sumVqI += vq * i;
```

This adds approximately 0.15 μs per sample on an LX7 core — negligible.

---

## 3. Stage 1 — Per-Cycle Computation

### 3.1 Cycle Boundary Detection

A cycle boundary is detected by a zero-crossing of the voltage waveform from negative to positive. Specifically, when $v[n-1] < 0$ and $v[n] \geq 0$ (rising zero-crossing), the current cycle ends and a new one begins.

The sub-sample zero-crossing timestamp is obtained by linear interpolation between the two samples straddling zero:

$$t_{\text{zc}} = t[n-1] + T_s \times \frac{-v[n-1]}{v[n] - v[n-1]}$$

where $T_s = 1/f_s = 250$ μs. In practice, we track the fractional sample index rather than absolute time:

$$n_{\text{zc}} = (n - 1) + \frac{-v[n-1]}{v[n] - v[n-1]}$$

The interpolation improves frequency resolution dramatically. Without interpolation, the frequency measurement quantisation at 4 kSPS is:

$$\Delta f = \frac{f_{\text{mains}}^2}{f_s} = \frac{60^2}{4000} = 0.9 \text{ Hz}$$

which is unacceptable. With linear interpolation, the resolution is limited by noise rather than sampling rate, and sub-millihertz resolution is achievable with averaging.

### 3.2 Frequency Measurement

The instantaneous frequency is computed from the interval between consecutive rising zero-crossings:

$$f_{\text{inst}} = \frac{f_s}{n_{\text{zc,current}} - n_{\text{zc,previous}}}$$

where $n_{\text{zc}}$ values include the fractional part from interpolation.

To reject noise, the frequency is averaged over 16 consecutive cycles using a simple IIR filter:

$$f_{\text{avg}}[k] = \frac{15}{16} \cdot f_{\text{avg}}[k-1] + \frac{1}{16} \cdot f_{\text{inst}}[k]$$

The multiplications by $15/16$ and $1/16$ can be implemented with a right-shift and subtraction to avoid floating-point division, but on both the LX7 and M4F the FPU makes the float version equally fast.

**Sanity gating.** If $f_{\text{inst}}$ falls outside the range $[45, 75]$ Hz, the measurement is discarded (noise-triggered false zero-crossing). This range covers both 50 Hz and 60 Hz systems with generous margin.

```cpp
struct FrequencyTracker {
    float prevZcFrac;       // fractional sample index of previous rising ZC
    float freqAvg;          // IIR-filtered frequency (Hz)
    float prevV;            // v[n-1] for edge detection
    int32_t sampleCounter;  // monotonic sample count (wraps at 2^31)
    bool valid;             // true after first two ZCs established

    void init()
    {
        prevZcFrac = 0.0f;
        freqAvg = 60.0f;   // initial assumption
        prevV = 0.0f;
        sampleCounter = 0;
        valid = false;
    }

    // Returns true if a rising zero-crossing was detected at this sample.
    // If true, freqAvg is updated.
    bool update(float v)
    {
        bool zcDetected = false;
        if (prevV < 0.0f && v >= 0.0f) {
            // Interpolated fractional crossing point
            float frac = -prevV / (v - prevV);  // in [0, 1)
            float zcSample = (float)(sampleCounter - 1) + frac;

            if (valid) {
                float period = zcSample - prevZcFrac;  // in samples
                if (period > 0.0f) {
                    float fInst = 4000.0f / period;  // fs / period
                    // Sanity gate
                    if (fInst > 45.0f && fInst < 75.0f) {
                        freqAvg = 0.9375f * freqAvg + 0.0625f * fInst;
                    }
                }
            }
            prevZcFrac = zcSample;
            valid = true;
            zcDetected = true;
        }
        prevV = v;
        sampleCounter++;
        return zcDetected;
    }
};
```

### 3.3 RMS Voltage and Current

At each cycle boundary (rising zero-crossing detected), the accumulated sums are finalised:

$$V_{\text{rms}} = \sqrt{\frac{\text{sumV2}}{N}}$$

$$I_{\text{rms}} = \sqrt{\frac{\text{sumI2}}{N}}$$

where $N$ = `acc.nSamples` for this cycle (nominally 66–67 at 60 Hz / 4 kSPS).

The square root is the most expensive operation here. On the ESP32-S3, the hardware FPU computes `sqrtf()` in approximately 14 cycles (58 ns at 240 MHz). On the Cortex-M4F, the VSQRT.F32 instruction takes 14 cycles (140 ns at 100 MHz). Both are negligible.

### 3.4 Real Power, Reactive Power, Apparent Power, Power Factor

**Real (active) power:**

$$P = \frac{\text{sumVI}}{N}$$

This is the fundamental definition — the mean of instantaneous power $p[n] = v[n] \cdot i[n]$ over a complete cycle. It requires no phase information and is inherently correct for any waveform shape, including non-sinusoidal (harmonic-rich) waveforms. This captures total real power including harmonic power.

**Reactive power (time-domain primary method):**

$$Q = \frac{\text{sumVqI}}{N}$$

where the 90-degree delayed voltage $v_q[n]$ was accumulated per-sample using the interpolated delay from Section 2.6. For a purely sinusoidal system, this is exactly $V_{\text{rms}} \cdot I_{\text{rms}} \cdot \sin(\phi)$. For non-sinusoidal waveforms, this gives the Budeanu reactive power at the fundamental — adequate for power factor computation.

**Apparent power:**

$$S = V_{\text{rms}} \times I_{\text{rms}}$$

Note: this is the arithmetic apparent power. For a single-phase system, this is the standard definition.

**Power factor:**

$$\text{PF} = \frac{P}{S} = \frac{P}{V_{\text{rms}} \times I_{\text{rms}}}$$

This is the total (true) power factor, which includes the effects of both displacement and distortion. For a purely sinusoidal system, $\text{PF} = \cos(\phi)$. For distorted waveforms, $\text{PF} < \cos(\phi_1)$ because $I_{\text{rms}}$ includes harmonic current that does not contribute to real power delivery at fundamental frequency.

**Sign of reactive power (for load classification):**

$$\text{Qsign} = \begin{cases} +1 & \text{if } Q > +\epsilon \text{ (inductive, current lags)} \\ -1 & \text{if } Q < -\epsilon \text{ (capacitive, current leads)} \\ 0 & \text{if } |Q| \leq \epsilon \text{ (resistive)} \end{cases}$$

where $\epsilon = 0.02 \times S$ (2% of apparent power deadband to avoid noise-induced sign flipping).

### 3.5 Energy Accumulation

Energy is accumulated as a running sum of per-cycle real power, multiplied by the cycle duration:

$$\Delta E = P \times \frac{N}{f_s} \quad [\text{watt-seconds}]$$

$$E_{\text{total}} \mathrel{+}= \Delta E$$

The energy accumulator uses a `double` (64-bit float) to avoid precision loss over extended operation. At 1 kW continuous load, the energy accumulates at $1000 \times 1/60 = 16.67$ Ws per cycle. After one year:

$$E_{\text{year}} = 1000 \times 3.156 \times 10^7 = 3.156 \times 10^{10} \text{ Ws}$$

A 64-bit `double` has 52 bits of mantissa, giving approximately 15 significant decimal digits. At $3.156 \times 10^{10}$ Ws, the LSB is approximately $3.5 \times 10^{-5}$ Ws = 35 μJ — more than adequate. A 32-bit `float` (23-bit mantissa, 7 decimal digits) would lose precision after only a few hours at high power.

### 3.6 Cycle-End Processing — Complete Code

```cpp
struct MeterData {
    float vrms;         // V
    float irms;         // A
    float realPower;    // W
    float reactivePower;// VAR
    float apparentPower;// VA
    float powerFactor;  // dimensionless, [-1, +1]
    float frequency;    // Hz
    float qSign;        // +1, -1, or 0
    double energyWh;    // cumulative Wh
    float crestFactor;  // Ipeak / Irms
};

// --------------------------------------------------------------------------
// Called when a rising zero-crossing is detected — once per mains cycle.
// --------------------------------------------------------------------------
static void finaliseCycle(CycleAccumulator& acc,
                          FrequencyTracker& freq,
                          MeterData& meter)
{
    if (acc.nSamples < 30)  // reject spurious short "cycles"
        return;

    float invN = 1.0f / (float)acc.nSamples;

    // RMS values
    float vrms = sqrtf(acc.sumV2 * invN);
    float irms = sqrtf(acc.sumI2 * invN);

    // Power quantities
    float P = acc.sumVI * invN;
    float Q = acc.sumVqI * invN;
    float S = vrms * irms;

    // Power factor with sign preservation and divide-by-zero guard
    float pf = 0.0f;
    if (S > 1.0e-6f)
        pf = P / S;

    // Clamp PF to [-1, +1] (numerical noise can push slightly outside)
    if (pf > 1.0f) pf = 1.0f;
    if (pf < -1.0f) pf = -1.0f;

    // Reactive power sign for load classification
    float qSign = 0.0f;
    float qDeadband = 0.02f * S;
    if (Q > qDeadband)
        qSign = +1.0f;
    else if (Q < -qDeadband)
        qSign = -1.0f;

    // Crest factor: Ipeak / Irms
    float cf = 0.0f;
    if (irms > 1.0e-6f)
        cf = acc.peakI / irms;

    // Energy accumulation (Watt-seconds → Wh)
    float cycleDuration = (float)acc.nSamples / 4000.0f;  // seconds
    double deltaWh = (double)P * (double)cycleDuration / 3600.0;

    // --- Write to shared struct (under mutex in RTOS) ---
    meter.vrms = vrms;
    meter.irms = irms;
    meter.realPower = P;
    meter.reactivePower = Q;
    meter.apparentPower = S;
    meter.powerFactor = pf;
    meter.frequency = freq.freqAvg;
    meter.qSign = qSign;
    meter.energyWh += deltaWh;
    meter.crestFactor = cf;

    // --- Reset accumulators for next cycle ---
    acc.sumV2 = 0.0f;
    acc.sumI2 = 0.0f;
    acc.sumVI = 0.0f;
    acc.sumVqI = 0.0f;
    acc.peakI = 0.0f;
    acc.nSamples = 0;
}
```

### 3.7 Updating the Quarter-Cycle Delay from Measured Frequency

Whenever the frequency estimate changes materially (more than 0.1 Hz from the value used to compute the current delay parameters), the reactive power delay parameters must be recalculated:

```cpp
static void updateReactiveDelay(float measuredFreq,
                                int32_t& n90Int, float& n90Frac)
{
    // n90 = fs / (4 * f)
    float n90 = 4000.0f / (4.0f * measuredFreq);  // e.g., 16.667 at 60 Hz
    n90Int = (int32_t)n90;                         // floor: 16
    n90Frac = n90 - (float)n90Int;                 // fractional part: 0.667

    // n90Int is the "older" delay, n90Int+1 does not exist as a variable
    // because getVq() computes indices (writeIdx - n90Int - 1) and
    // (writeIdx - n90Int) and interpolates with weight n90Frac.
}
```

This is called once at startup with $f = 60$ Hz and then periodically (every ~16 cycles) when the measured frequency drifts.

---

## 4. Stage 2 — FFT-Based Harmonic Analysis

### 4.1 Triggering and Buffer Management

The FFT runs every 2 seconds (approximately 120 mains cycles at 60 Hz). The analysis uses the most recent 128 samples from each channel's circular buffer. These 128 samples span:

$$T_{\text{window}} = \frac{128}{4000} = 32 \text{ ms} = 1.92 \text{ cycles at 60 Hz}$$

The non-integer cycle count causes spectral leakage. A Hann window reduces sidelobe levels from $-13$ dB (rectangular) to $-31.5$ dB, at the cost of 6 dB main-lobe widening. Since the harmonic spacing (60 Hz) exceeds the widened main-lobe width ($\approx 2 \times 31.25 = 62.5$ Hz), adjacent harmonics remain resolvable.

### 4.2 FFT Frequency Resolution and Bin Mapping

With $N = 128$ and $f_s = 4000$ SPS:

$$\Delta f = \frac{f_s}{N} = 31.25 \text{ Hz}$$

The fundamental at 60 Hz does not fall exactly on a bin centre. Bin $k = 2$ corresponds to $62.5$ Hz — an offset of $2.5$ Hz from the true fundamental. This mismatch is the primary source of spectral leakage and is why the Hann window is essential.

The harmonic-to-bin mapping:

| Harmonic $h$ | Frequency (Hz) | Exact bin $f_h / \Delta f$ | Nearest integer bin $k_h$ | Bin centre error (Hz) |
|---|---|---|---|---|
| 1 | 60 | 1.920 | 2 | +2.5 |
| 2 | 120 | 3.840 | 4 | +5.0 |
| 3 | 180 | 5.760 | 6 | +7.5 |
| 4 | 240 | 7.680 | 8 | +10.0 |
| 5 | 300 | 9.600 | 10 | +12.5 |
| 7 | 420 | 13.440 | 13 | −7.5 |
| 9 | 540 | 17.280 | 17 | −7.5 |
| 11 | 660 | 21.120 | 21 | −2.5 |
| 13 | 780 | 24.960 | 25 | +2.5 |
| 15 | 900 | 28.800 | 29 | +7.5 |

The bin-centre errors are bounded within $\pm \Delta f / 2 = \pm 15.625$ Hz. With a Hann window, the magnitude estimation error from a $\pm 15.6$ Hz offset is at most $-3.9$ dB ($\approx 36\%$). Since all features used by the classifier are ratios of harmonic magnitudes (e.g., $|I_3|/|I_1|$), the absolute error partially cancels because all harmonics experience similar (though not identical) leakage. The filter compensation from Section 4.6 handles the frequency-dependent part.

### 4.3 Windowing

The Hann window coefficients for $N = 128$ can be precomputed once at startup and stored in a 512-byte lookup table (128 floats):

$$w[n] = 0.5 - 0.5 \cos\left(\frac{2\pi n}{N}\right), \quad n = 0, 1, \ldots, N-1$$

The coherent gain of the Hann window is 0.5 (compared to 1.0 for rectangular), so magnitudes must be scaled by $2/N$ after the FFT to get the single-sided amplitude spectrum, then divided by the coherent gain $= 0.5$, giving an overall normalisation factor of $4/N = 4/128 = 0.03125$.

Correction: the standard normalisation for a windowed single-sided spectrum is:

$$|X_h| = \frac{2}{N \cdot S_1} \cdot |X_{\text{raw}}[k_h]|$$

where $S_1 = \frac{1}{N}\sum_{n=0}^{N-1} w[n] = 0.5$ for Hann. So the factor is $2 / (128 \times 0.5) = 1/32 = 0.03125$.

### 4.4 FFT Implementation — Platform Abstraction

```cpp
// --------------------------------------------------------------------------
// Platform-agnostic FFT interface.  The actual implementation is provided
// by esp-dsp (dsps_fft2r_fc32) on ESP32-S3 or CMSIS-DSP (arm_cfft_f32)
// on STM32F411.  This wrapper normalises the calling convention.
// --------------------------------------------------------------------------

// Complex pair stored as interleaved float: [Re0, Im0, Re1, Im1, ...]
// Total buffer size: N * 2 floats = 128 * 2 = 256 floats = 1024 bytes.

// Forward declaration — implemented per platform
void platformFFT(float* complexBuf, int32_t n);

// --------------------------------------------------------------------------
// Precomputed Hann window (128 floats, computed once at init).
// --------------------------------------------------------------------------
static float hannWindow[128];

void initHannWindow()
{
    for (int32_t n = 0; n < 128; n++) {
        // Avoid <cmath> — use platform math.h
        float phase = 2.0f * 3.14159265358979f * (float)n / 128.0f;
        hannWindow[n] = 0.5f - 0.5f * cosf(phase);
    }
}

// --------------------------------------------------------------------------
// Prepare the FFT input buffer from the circular sample buffer.
// Copies the most recent 128 samples, applies the Hann window,
// and packs into complex format (imaginary parts = 0).
// --------------------------------------------------------------------------
void prepareFFTInput(const float* circBuf, int32_t bufLen,
                     int32_t writeIdx,
                     float* complexBuf)
{
    for (int32_t n = 0; n < 128; n++) {
        int32_t srcIdx = (writeIdx - 128 + n + bufLen) % bufLen;
        complexBuf[2 * n]     = circBuf[srcIdx] * hannWindow[n]; // Real
        complexBuf[2 * n + 1] = 0.0f;                            // Imag
    }
}
```

### 4.5 Harmonic Magnitude and Phase Extraction

After the FFT, each bin $k$ contains a complex number $X[k] = \text{Re}[k] + j \cdot \text{Im}[k]$. The magnitude and phase at harmonic $h$:

$$|X_h| = \frac{1}{32} \sqrt{\text{Re}[k_h]^2 + \text{Im}[k_h]^2}$$

$$\phi_h = \text{atan2}(\text{Im}[k_h], \text{Re}[k_h])$$

The bin indices $k_h$ for the harmonics of interest are precomputed constants (from the table in Section 4.2).

```cpp
// Harmonic bin indices for 60 Hz fundamental at fs = 4000, N = 128
static const int32_t kHarmonicBins[] = {
    2,   // H1 (fundamental, 60 Hz)
    4,   // H2 (120 Hz)
    6,   // H3 (180 Hz)
    8,   // H4 (240 Hz)
    10,  // H5 (300 Hz)
    13,  // H7 (420 Hz)
    17,  // H9 (540 Hz)
    21,  // H11 (660 Hz)
    25,  // H13 (780 Hz)
    29   // H15 (900 Hz)
};
static const int32_t kNumHarmonics = 10;

struct HarmonicResult {
    float magV[10];    // voltage harmonic magnitudes (V rms, primary-referred)
    float magI[10];    // current harmonic magnitudes (A rms, primary-referred)
    float phaseV[10];  // voltage harmonic phases (radians)
    float phaseI[10];  // current harmonic phases (radians)
};

void extractHarmonics(const float* complexBuf,
                      float normFactor,   // = 1.0f / 32.0f
                      float* magOut,
                      float* phaseOut)
{
    for (int32_t h = 0; h < kNumHarmonics; h++) {
        int32_t k = kHarmonicBins[h];
        float re = complexBuf[2 * k];
        float im = complexBuf[2 * k + 1];
        magOut[h] = normFactor * sqrtf(re * re + im * im);
        phaseOut[h] = atan2f(im, re);
    }
}
```

### 4.6 Anti-Aliasing Filter Compensation

The external RC anti-aliasing filter ($R_{\text{filt}} = 100$ $\Omega$, $C_{\text{diff}} = 680$ nF, $f_c = 2341$ Hz) attenuates higher harmonics. Since both channels use identical components, the amplitude attenuation is the same on both channels at any given frequency, but the correction is still needed for absolute magnitude accuracy (which affects THD computation).

The correction factor for harmonic $h$:

$$G_{\text{comp}}(h) = \sqrt{1 + \left(\frac{h \times 60}{2341}\right)^2}$$

This is precomputed once:

| Harmonic | $G_{\text{comp}}$ | Magnitude correction |
|---|---|---|
| H1 (60 Hz) | 1.00033 | +0.033% |
| H3 (180 Hz) | 1.00296 | +0.30% |
| H5 (300 Hz) | 1.00821 | +0.82% |
| H7 (420 Hz) | 1.01606 | +1.6% |
| H9 (540 Hz) | 1.02637 | +2.6% |
| H11 (660 Hz) | 1.03913 | +3.9% |
| H13 (780 Hz) | 1.05427 | +5.4% |
| H15 (900 Hz) | 1.07168 | +7.2% |

At H15, the correction is 7.2% — significant enough to matter for accurate THD computation.

```cpp
static float filterCompensation[10];  // one per harmonic

void initFilterCompensation(float fc)
{
    static const float harmonicFreqs[] = {
        60, 120, 180, 240, 300, 420, 540, 660, 780, 900
    };
    for (int32_t h = 0; h < kNumHarmonics; h++) {
        float ratio = harmonicFreqs[h] / fc;
        filterCompensation[h] = sqrtf(1.0f + ratio * ratio);
    }
}
```

After extraction, apply: `magOut[h] *= filterCompensation[h];`

### 4.7 FFT-Based Frequency Cross-Check

The fundamental frequency can be estimated from the FFT by finding the peak bin near the expected fundamental and applying quadratic (parabolic) interpolation for sub-bin resolution.

Given the three bins around the peak — $|X[k-1]|$, $|X[k]|$, $|X[k+1]|$ where $k$ is the peak bin — the fractional bin offset is:

$$\delta = \frac{|X[k-1]| - |X[k+1]|}{2 \cdot (|X[k-1]| - 2|X[k]| + |X[k+1]|)}$$

The corrected frequency estimate:

$$f_{\text{FFT}} = (k + \delta) \times \Delta f = (k + \delta) \times 31.25 \text{ Hz}$$

This gives approximately $\pm 1$ Hz accuracy — not as precise as the zero-crossing method (which achieves sub-0.01 Hz with averaging), but serves as an independent sanity check. If the FFT and ZC frequency estimates disagree by more than 2 Hz, something is wrong (noise, waveform distortion causing false zero-crossings, or a frequency transient).

```cpp
float estimateFreqFromFFT(const float* complexBuf)
{
    // Search for peak magnitude in bins 1–3 (45–95 Hz range)
    float maxMag = 0.0f;
    int32_t peakBin = 2;  // default to bin 2 (62.5 Hz)
    for (int32_t k = 1; k <= 3; k++) {
        float re = complexBuf[2 * k];
        float im = complexBuf[2 * k + 1];
        float mag = re * re + im * im;  // squared magnitude (skip sqrt)
        if (mag > maxMag) {
            maxMag = mag;
            peakBin = k;
        }
    }

    // Parabolic interpolation (only if peak is not at boundary)
    if (peakBin < 1 || peakBin > 62) return (float)peakBin * 31.25f;

    float re_m = complexBuf[2 * (peakBin - 1)];
    float im_m = complexBuf[2 * (peakBin - 1) + 1];
    float mag_m = sqrtf(re_m * re_m + im_m * im_m);

    float re_0 = complexBuf[2 * peakBin];
    float im_0 = complexBuf[2 * peakBin + 1];
    float mag_0 = sqrtf(re_0 * re_0 + im_0 * im_0);

    float re_p = complexBuf[2 * (peakBin + 1)];
    float im_p = complexBuf[2 * (peakBin + 1) + 1];
    float mag_p = sqrtf(re_p * re_p + im_p * im_p);

    float denom = 2.0f * (mag_m - 2.0f * mag_0 + mag_p);
    float delta = 0.0f;
    if (denom < -1.0e-10f || denom > 1.0e-10f)
        delta = (mag_m - mag_p) / denom;

    return ((float)peakBin + delta) * 31.25f;
}
```

### 4.8 FFT-Based Reactive Power Cross-Check

The reactive power can be independently estimated from the FFT phase data at the fundamental:

$$Q_{\text{FFT}} = V_1 \times I_1 \times \sin(\phi_{V,1} - \phi_{I,1})$$

where $V_1$ and $I_1$ are the fundamental magnitudes from the voltage and current FFTs, and $\phi_{V,1}$, $\phi_{I,1}$ are the corresponding phases. This should agree with the time-domain $Q$ to within approximately 5%. A larger discrepancy indicates either a measurement error or that harmonic power is contributing significantly to the time-domain reactive power calculation — in which case the FFT value (fundamental-only) is the more physically meaningful one for displacement power factor.

The displacement power factor (DPF) is:

$$\text{DPF} = \cos(\phi_{V,1} - \phi_{I,1})$$

This is distinct from the total PF computed in Section 3.4. The ratio $\text{PF} / \text{DPF}$ equals the distortion factor, which is another way to quantify THD's impact on power delivery.

---

## 5. Stage 2 (continued) — Derived Features for Load Classification

The following quantities are computed from the FFT results and the per-cycle measurements. They form the feature vector consumed by the load classifier (documented separately). This section specifies only the computation — not the classification logic.

### 5.1 Total Harmonic Distortion of Current (THD_I)

$$\text{THD}_I = \frac{\sqrt{\sum_{h=3,5,7,9,11,13,15} |I_h|^2}}{|I_1|}$$

Only odd harmonics are summed because even harmonics are negligible in symmetric AC waveforms. The denominator is the fundamental current magnitude from the FFT.

```cpp
float computeThdI(const float* magI)
{
    // magI[0] = H1 (fundamental)
    // magI[2] = H3, magI[4] = H5, magI[5] = H7, magI[6] = H9,
    // magI[7] = H11, magI[8] = H13, magI[9] = H15
    // (magI[1] = H2, magI[3] = H4 are even — excluded from THD sum)

    float sumSq = 0.0f;
    // Indices of odd harmonics in magI array: 2(H3), 4(H5), 5(H7),
    // 6(H9), 7(H11), 8(H13), 9(H15)
    static const int32_t oddIdx[] = {2, 4, 5, 6, 7, 8, 9};
    for (int32_t j = 0; j < 7; j++) {
        float m = magI[oddIdx[j]];
        sumSq += m * m;
    }

    float i1 = magI[0];
    if (i1 < 1.0e-9f) return 0.0f;  // avoid division by zero
    return sqrtf(sumSq) / i1;
}
```

### 5.2 Individual Harmonic Ratios

These are simple divisions used directly as classifier features:

$$f_{\text{H3}} = \frac{|I_3|}{|I_1|}, \quad f_{\text{H5}} = \frac{|I_5|}{|I_1|}, \quad f_{\text{H7}} = \frac{|I_7|}{|I_1|}$$

$$f_{\text{H53}} = \frac{|I_5|}{|I_3|}, \quad f_{\text{OE}} = \frac{|I_2| + |I_4|}{|I_1|}$$

The even harmonic ratio $f_{\text{OE}}$ detects half-wave asymmetry (diagnostic for certain lighting loads and half-wave rectifiers).

### 5.3 Crest Factor

The crest factor was already tracked per-cycle in the `CycleAccumulator` (Section 2.5). For the classifier, we use the most recent per-cycle value:

$$\text{CF} = \frac{I_{\text{peak}}}{I_{\text{rms}}}$$

For a pure sinusoid, $\text{CF} = \sqrt{2} \approx 1.414$. Values significantly above this indicate a peaky waveform (SMPS loads typically show $\text{CF} = 2.0$–$3.5$).

### 5.4 Log-Scaled Current Magnitude

$$f_{\text{Prms}} = \log_2\left(\frac{I_{\text{rms}}}{0.05}\right), \quad \text{clamped to } [0, 10]$$

This feature separates no-load (phone charger standby) from full-load conditions. The logarithmic scaling compresses the 5000:1 dynamic range into a compact $[0, 10]$ interval. The base of 0.05 A corresponds to the no-load gate threshold.

```cpp
float computeLogCurrent(float irms)
{
    if (irms < 0.05f) return 0.0f;
    // log2f(x) = log(x) / log(2) = logf(x) * 1.442695f
    float val = logf(irms / 0.05f) * 1.442695f;
    if (val > 10.0f) val = 10.0f;
    return val;
}
```

### 5.5 Complete Feature Vector Assembly

```cpp
struct ClassifierFeatures {
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

void assembleFeatures(const MeterData& meter,
                      const HarmonicResult& harm,
                      ClassifierFeatures& feat)
{
    float i1 = harm.magI[0];  // fundamental current magnitude
    float guard = (i1 > 1.0e-9f) ? i1 : 1.0e-9f;

    feat.pf      = (meter.powerFactor >= 0.0f) ? meter.powerFactor
                                                : -meter.powerFactor;
    feat.qSign   = meter.qSign;
    feat.thdI    = computeThdI(harm.magI);

    feat.h3Ratio = harm.magI[2] / guard;    // H3
    feat.h5Ratio = harm.magI[4] / guard;    // H5
    feat.h7Ratio = harm.magI[5] / guard;    // H7

    feat.cf      = meter.crestFactor;

    feat.oeRatio = (harm.magI[1] + harm.magI[3]) / guard;  // (H2+H4)/H1

    float i3 = harm.magI[2];
    float i3guard = (i3 > 1.0e-9f) ? i3 : 1.0e-9f;
    feat.h53Ratio = harm.magI[4] / i3guard;  // H5/H3

    feat.pRms    = computeLogCurrent(meter.irms);
}
```

---

## 6. Numerical Considerations

### 6.1 Floating-Point Accumulator Drift

The per-sample accumulators `sumV2`, `sumI2`, `sumVI`, `sumVqI` are reset every cycle (~67 samples). The accumulated values at nominal load:

$$\text{sumV2} \approx 67 \times (220)^2 = 67 \times 48{,}400 \approx 3.24 \times 10^6$$

A 32-bit `float` has precision to $\sim 2^{-23} \approx 1.2 \times 10^{-7}$ relative. At $3.24 \times 10^6$, the LSB is $\approx 0.39$. Each individual $v^2$ sample contributes $\sim 48{,}400$, so the accumulator error per sample is $0.39 / 48{,}400 = 8 \times 10^{-6}$ — well below the ADC's noise floor. No Kahan summation or double-precision needed for the per-cycle accumulators.

### 6.2 ADC Code to Float Conversion

The 24-bit code range is $[-2^{23}, +2^{23}-1] = [-8{,}388{,}608, +8{,}388{,}607]$. A 32-bit `float` can represent all integers up to $2^{24} = 16{,}777{,}216$ exactly, so the `(float)code` cast is lossless for all possible ADC output codes. No precision is lost in the conversion step.

### 6.3 Trigonometric Precision for Reactive Power

The `atan2f()` function (used for phase extraction) has a typical error of $\pm 1$ ULP ($\approx 6 \times 10^{-8}$ radians). At 60 Hz, the phase angle between V and I for a PF = 0.5 load is $\phi = 60°$. The power factor error from a $6 \times 10^{-8}$ radian phase error is:

$$\Delta \text{PF} = \sin(\phi) \times \Delta\phi = \sin(60°) \times 6 \times 10^{-8} = 5.2 \times 10^{-8}$$

Negligible — the transducer phase errors ($\sim 0.5°$) dominate by six orders of magnitude.

---

## 7. Data Flow Summary

```
 ADS131M02                    ESP32-S3 / STM32F411
┌──────────┐                 ┌─────────────────────────────────────────┐
│  CH0: V  │──DRDY──────────▶│  DRDY ISR                              │
│  CH1: I  │──SPI────────────▶│    └── DMA SPI read (80-bit frame)    │
│          │                 │         └── signExtend24() × 2         │
└──────────┘                 │              │                          │
                             │    processSample()          ◄── 4 kHz  │
                             │    ├── Scale: code → v[n], i[n]        │
                             │    ├── Store into circular buffers      │
                             │    ├── Accumulate: V², I², V×I, Vq×I  │
                             │    ├── Track peak |i[n]|               │
                             │    └── Zero-crossing check             │
                             │         │                               │
                             │    ┌────▼── Rising ZC detected? ──┐    │
                             │    │ YES                     NO   │    │
                             │    │                          │   │    │
                             │    │  finaliseCycle()         │   │    │
                             │    │  ├── Vrms, Irms          │   │    │
                             │    │  ├── P, Q, S, PF         │   │    │
                             │    │  ├── Frequency update    │   │    │
                             │    │  ├── Energy += ΔE        │   │    │
                             │    │  ├── Crest factor        │   │    │
                             │    │  └── → MeterData{}       │   │    │
                             │    │                          │   │    │
                             │    └────────── continue ──────┘   │    │
                             │                                        │
                             │    ┌── Every 2 seconds ──────────┐     │
                             │    │  runHarmonicAnalysis()       │     │
                             │    │  ├── Copy 128 samples        │     │
                             │    │  ├── Apply Hann window       │     │
                             │    │  ├── FFT (V), FFT (I)        │     │
                             │    │  ├── Extract H1–H15 mag+phase│     │
                             │    │  ├── Filter compensation     │     │
                             │    │  ├── THD, harmonic ratios    │     │
                             │    │  ├── Freq cross-check        │     │
                             │    │  ├── Q cross-check (FFT)     │     │
                             │    │  └── → ClassifierFeatures{}  │     │
                             │    └──────────────────────────────┘     │
                             │                                        │
                             │    MeterData{} ──────┐                 │
                             │    ClassifierFeatures{} ──┤            │
                             │                       ▼                │
                             │              TDMA Telemetry Packet     │
                             └─────────────────────────────────────────┘
```

---

## 8. Computational Budget Summary

| Operation | Rate | ESP32-S3 (LX7 240 MHz) | STM32F411 (M4F 100 MHz) |
|---|---|---|---|
| SPI frame read (DMA) | 4000/s | 3.2 μs | 5 μs |
| `processSample()` | 4000/s | ~0.3 μs | ~0.5 μs |
| `finaliseCycle()` | 60/s | ~10 μs | ~20 μs |
| `updateReactiveDelay()` | ~4/s | ~0.2 μs | ~0.4 μs |
| FFT × 2 channels (128-pt) | 0.5/s | ~30 μs | ~134 μs |
| Harmonic extraction + features | 0.5/s | ~8 μs | ~20 μs |
| **Total DSP CPU load** | — | **~1.5%** | **~2.3%** |

Both platforms have over 97% idle capacity remaining for TDMA radio, relay control, and housekeeping.

---

## 9. Output Summary — What Each Downstream Consumer Receives

### 9.1 MeterData (updated every cycle, ~60 Hz)

| Field | Unit | Resolution | Accuracy target |
|---|---|---|---|
| `vrms` | V | 2.6 mV | ≤ 0.5% |
| `irms` | A | 0.35 mA (at G=2) | ≤ 0.5% |
| `realPower` | W | ~0.1 W at 220V/1A | ≤ 0.5% |
| `reactivePower` | VAR | ~0.1 VAR | ≤ 1% |
| `apparentPower` | VA | derived from Vrms×Irms | ≤ 0.5% |
| `powerFactor` | — | 0.001 | ≤ 0.005 |
| `frequency` | Hz | ~0.01 Hz (after averaging) | ≤ 0.05 Hz |
| `qSign` | — | discrete: −1, 0, +1 | — |
| `energyWh` | Wh | 35 μJ (double precision) | cumulative |
| `crestFactor` | — | ~0.01 | ≤ 2% |

### 9.2 ClassifierFeatures (updated every 2 seconds)

| Feature | Symbol | Range | Notes |
|---|---|---|---|
| Power factor | $f_\text{PF}$ | [0, 1] | Magnitude of PF from MeterData |
| Q sign | $f_\text{Qsign}$ | {−1, 0, +1} | Reactive power polarity |
| THD of current | $f_\text{THD}$ | [0, ~2.0] | Odd harmonics only |
| H3 ratio | $f_\text{H3}$ | [0, ~1.0] | |I3|/|I1| |
| H5 ratio | $f_\text{H5}$ | [0, ~0.8] | |I5|/|I1| |
| H7 ratio | $f_\text{H7}$ | [0, ~0.5] | |I7|/|I1| |
| Crest factor | $f_\text{CF}$ | [1.0, ~4.0] | From per-cycle peak tracking |
| Even harmonic ratio | $f_\text{OE}$ | [0, ~0.15] | (|I2|+|I4|)/|I1| |
| H5/H3 ratio | $f_\text{H53}$ | [0, ~1.0] | Shape discriminator |
| Log current | $f_\text{Prms}$ | [0, 10] | log₂(Irms/0.05) |

---

## 10. Platform Portability Notes

### 10.1 Abstraction Points

The following functions must be implemented per-platform:

| Function | ESP32-S3 (esp-dsp) | STM32F411 (CMSIS-DSP) |
|---|---|---|
| `platformFFT()` | `dsps_fft2r_fc32()` | `arm_cfft_f32()` |
| `sqrtf()` | Hardware FPU | `VSQRT.F32` instruction |
| `atan2f()` | Newlib math | ARM math library |
| `cosf()`, `sinf()` | Newlib math | ARM math library |
| `logf()` | Newlib math | ARM math library |
| Mutex lock/unlock | `xSemaphoreTake/Give` | `osMutexAcquire/Release` |
| DMA SPI setup | ESP-IDF SPI master | HAL SPI + DMA |
| NVS read/write | `nvs_get/set_blob` | Flash sector R/W |

### 10.2 Memory Layout (Identical on Both Platforms)

| Buffer | Size (bytes) | Notes |
|---|---|---|
| Voltage circular buffer (256 floats) | 1024 | 3.8 cycles at 60 Hz |
| Current circular buffer (256 floats) | 1024 | Mirror of voltage buffer |
| FFT complex buffer (256 floats) | 1024 | Reused for V then I |
| Hann window table (128 floats) | 512 | Precomputed at init |
| Filter compensation (10 floats) | 40 | Precomputed at init |
| Harmonic bin indices (10 int32) | 40 | Compile-time constant |
| `MeterData` struct | 44 | Shared output |
| `ClassifierFeatures` struct | 40 | Shared output |
| `CycleAccumulator` struct | 24 | Working state |
| `FrequencyTracker` struct | 20 | Working state |
| **Total DSP RAM** | **~3.8 KB** | Well within both platforms |

### 10.3 Compile-Time Configuration

```cpp
// --------------------------------------------------------------------------
// System configuration — adjust for 50 Hz systems by changing MAINS_FREQ_NOM.
// --------------------------------------------------------------------------
static const float FS              = 4000.0f;     // ADC sample rate (Hz)
static const float MAINS_FREQ_NOM  = 60.0f;       // nominal mains frequency
static const int32_t FFT_N         = 128;          // FFT length
static const int32_t CIRC_BUF_LEN  = 256;          // circular buffer length
static const float VREF            = 1.2f;         // ADS131M02 internal ref
static const float FILTER_FC       = 2341.0f;      // anti-aliasing filter cutoff

// Voltage channel: ZMPT101B
static const float R_LIMIT         = 112000.0f;    // current-limiting resistor
static const float R_SAMPLE        = 330.0f;        // sampling resistor

// Current channel: CT
static const float CT_TURNS        = 1000.0f;      // CT turns ratio
static const float R_BURDEN        = 10.0f;         // burden resistor
```
