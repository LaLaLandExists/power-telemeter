# Analog Front-End Design: ADS131M02 + ESP32-S3

## Power Metering and Load Characterisation Acquisition System

---

## 1. System Parameters and Design Targets

### 1.1 Mains Electrical Parameters

| Parameter | Symbol | Value | Unit |
|-----------|--------|-------|------|
| Nominal RMS voltage | $V_{\text{nom}}$ | 220 | V |
| Peak voltage | $V_{\text{pk}}$ | $220\sqrt{2} \approx 311$ | V |
| Mains frequency | $f_{\text{mains}}$ | 60 | Hz |
| Maximum load current (target) | $I_{\text{max}}$ | 30 | A |
| CT rated current | $I_{\text{CT,rated}}$ | 100 | A |
| Peak current at target load | $I_{\text{pk}}$ | $30\sqrt{2} \approx 42.4$ | A |

### 1.2 ADC Parameters (ADS131M02)

| Parameter | Symbol | Value | Unit |
|-----------|--------|-------|------|
| Resolution | $N$ | 24 | bits |
| Internal reference voltage | $V_{\text{ref}}$ | 1.2 | V |
| Full-scale differential input (gain = 1) | $V_{\text{FSR}}$ | $\pm 1.2$ | V |
| Analog supply | $\text{AVDD}$ | 2.7 – 3.6 | V |
| Digital supply | $\text{DVDD}$ | 1.8 or 3.3 | V |
| Maximum data rate (HR mode) | $f_{\text{data}}$ | 32k (up to 64k) | SPS |
| PGA gain settings | $G$ | 1, 2, 4, 8, 16, 32, 64, 128 | — |
| Input-referred noise (8 kSPS, $G=1$) | $e_{n}$ | 7.56 | $\mu$V rms |
| Input impedance ($G \leq 4$) | $Z_{\text{in}}$ | 300 | k$\Omega$ |
| Differential input range | — | $\pm V_{\text{ref}} / G$ | V |
| CLKIN frequency | $f_{\text{CLKIN}}$ | 8.192 | MHz |

### 1.3 Current Transformer Parameters (PZEM-004T Bundled CT)

| Parameter | Symbol | Value | Unit |
|-----------|--------|-------|------|
| Rated primary current | $I_{p,\text{rated}}$ | 100 | A |
| Turns ratio | $n$ | 1 : 1000 | — |
| Rated secondary current | $I_{s,\text{rated}}$ | $100 / 1000 = 100$ | mA |
| Type | — | Solid-core toroidal | — |
| Accuracy class | — | Unknown (requires characterisation) | — |
| Phase angle error | $\phi_{\text{CT}}$ | Unknown (requires characterisation) | — |
| Isolation voltage | $V_{\text{iso,CT}}$ | Unknown (estimated $\geq 1$ kV for toroidal) | V |
| Mounting | — | Wire pass-through (toroidal) | — |
| Notes | — | Salvaged from PZEM-004T module; operating at 30% of rated capacity for improved linearity | — |

### 1.4 Voltage Transformer Parameters (ZMPT101B)

| Parameter | Symbol | Value | Unit |
|-----------|--------|-------|------|
| Turns ratio | $n_V$ | 1000 : 1000 (1:1) | — |
| Rated operating current | $I_{\text{op}}$ | 2 | mA |
| Maximum operating current | $I_{\text{op,max}}$ | 10 | mA |
| Phase angle error ($I = 2$ mA, $R_s = 100 \Omega$) | $\phi_{\text{ZMPT}}$ | $\leq 20'$ ($0.333^\circ$) | arcmin |
| Linearity (20%–120% rated) | — | $\leq 0.2$% | — |
| Permissible error ($I = 2$ mA, $R_s = 100 \Omega$) | — | $-0.3$% to $+0.2$% | — |
| Isolation voltage | $V_{\text{iso}}$ | 4000 | V AC |
| Linear input voltage range | — | 0 – 1000 | V |
| Operating temperature | — | $-40$ to $+70$ | $^\circ$C |

### 1.5 Design Targets

| Requirement | Target |
|-------------|--------|
| Metering accuracy (V, I, P) | $\leq 0.5$% (match or improve PZEM-004T) |
| Harmonic analysis bandwidth | Up to 15th harmonic (900 Hz at 60 Hz fundamental) |
| Sampling rate per channel | 4 – 8 kSPS (simultaneously sampled) |
| Samples per mains cycle | 67 – 133 |
| Phase skew between V and I | 0 (simultaneous sampling) |
| Dynamic range for current | 5000:1 (10 mA to 30 A target, CT rated to 100 A) |
| Galvanic isolation | Full isolation — both V and I channels |

---

## 2. Current Channel Design

### 2.1 Split Burden Resistor Selection

The CT secondary current is proportional to the primary current through the turns ratio $n$. For a primary current $I_p$, the secondary current is:

$$I_s = \frac{I_p}{n}$$

At the target maximum load current of 30 A:

$$I_{s,\text{target}} = \frac{I_{\text{max}}}{n} = \frac{30}{1000} = 30 \text{ mA (rms)}$$

The corresponding peak secondary current:

$$I_{s,\text{pk}} = I_{s,\text{target}} \cdot \sqrt{2} = 30 \times 1.414 = 42.43 \text{ mA}$$

At the CT's full rated current of 100 A, the peak secondary current would be:

$$I_{s,\text{rated,pk}} = \frac{100 \times \sqrt{2}}{1000} = 141.4 \text{ mA}$$

The burden resistor $R_b$ converts this current to a voltage across the ADC differential input. The ADS131M02 at PGA gain $G = 1$ accepts a full-scale differential input of $\pm V_{\text{ref}} = \pm 1.2$ V. We size the burden so the signal at our 30 A target uses the ADC range efficiently while providing generous headroom for motor inrush transients.

Following the ADS131M02 datasheet recommendation (Section 9.2.2.1), the burden is implemented as a **split configuration**: two equal resistors of $R_b/2$ in series, with the center tap connected to AGND. This produces a balanced differential signal — the positive ADC input sees $+V_{b}/2$ and the negative input sees $-V_{b}/2$ relative to ground, both 180 degrees out of phase. The split configuration provides best THD performance because any even-order distortion in the ADC's input stage is cancelled by the balanced drive.

We select $R_b = 10 \text{ } \Omega$ (2 $\times$ 5 $\Omega$). At 30 A target, the peak burden voltage is:

$$V_{b,\text{pk}}\big|_{30\text{A}} = I_{s,\text{pk}} \cdot R_b = 42.43 \times 10^{-3} \times 10 = 0.424 \text{ V}$$

This is $0.424 / 1.2 = 35.4$% of the ADC full-scale range at $G = 1$ — relatively low utilisation, but the design compensates for this by running the ADS131M02 at **PGA = 2 as the default operating mode** (see Section 2.3). The clipping threshold at $G = 1$:

$$I_{p,\text{clip}}\big|_{G=1} = \frac{V_{\text{ref}}}{R_b} \cdot n = \frac{1.2}{10} \times 1000 = 120 \text{ A (peak)} = 84.9 \text{ A (RMS)}$$

This provides ample headroom for motor startup inrush transients. Even a 1.5 HP aircon compressor drawing 50 A peak inrush is well within the clipping threshold.

### 2.2 Current Measurement Resolution

At the default operating mode of $G = 2$, the effective full-scale range is:

$$V_{\text{FSR}}\big|_{G=2} = \pm \frac{V_{\text{ref}}}{G} = \pm \frac{1.2}{2} = \pm 0.6 \text{ V}$$

The peak burden voltage at 30 A target relative to this range:

$$\text{Utilisation}\big|_{G=2} = \frac{0.424}{0.6} = 70.7\%$$

This is good ADC utilisation. The clipping threshold at $G = 2$:

$$I_{p,\text{clip}}\big|_{G=2} = \frac{0.6}{10} \times 1000 = 60 \text{ A (peak)} = 42.4 \text{ A (RMS)}$$

At 42.4 A RMS, clipping occurs above 141% of the 30 A target — adequate for steady-state measurements. Motor inrush events that exceed 42.4 A RMS are handled by the gain-ranging firmware (Section 2.3).

The ADC's least significant bit (LSB) voltage at $G = 2$ is:

$$V_{\text{LSB}} = \frac{2 \cdot V_{\text{ref}} / G}{2^N} = \frac{2 \times 0.6}{2^{24}} = \frac{1.2}{16{,}777{,}216} = 71.53 \text{ nV}$$

The effective resolution is limited by noise, not by the 24-bit code space. At 4 kSPS with $G = 2$, the input-referred RMS noise is approximately 3.5 $\mu$V (noise improves at higher PGA gain because the PGA amplifies before the ADC's quantisation noise floor). The noise-limited effective resolution in current terms:

$$\Delta I_{p,\text{noise}} = \frac{e_{n,G=2}}{R_b} \cdot n = \frac{3.5 \times 10^{-6}}{10} \times 1000 = 0.350 \text{ mA (rms primary)}$$

The effective current dynamic range at $G = 2$:

$$\text{DR}_I = \frac{I_{\text{max}}}{\Delta I_{p,\text{noise}} \cdot \sqrt{2}} = \frac{30}{0.350 \times 10^{-3} \times 1.414} \approx 60{,}600 : 1$$

This far exceeds the 5000:1 design target and outperforms the PZEM-004T's usable range.

### 2.3 PGA Gain-Ranging Strategy

The ADS131M02's PGA is used as the primary mechanism to optimise ADC utilisation across the operating range. Rather than increasing the burden resistance (which would reduce the inrush clipping threshold), the burden is kept at 10 $\Omega$ and the PGA gain is adjusted in firmware based on the measured signal amplitude.

**Default mode: $G = 2$.** This is the normal operating mode for loads up to 30 A. ADC utilisation is 70.7% at 30 A, and the clipping threshold is 42.4 A RMS — sufficient for all steady-state residential loads.

**Inrush mode: $G = 1$.** When the firmware detects the signal approaching clipping (e.g., absolute sample value exceeds 90% of FSR at $G = 2$), it immediately switches to $G = 1$. The clipping threshold doubles to 84.9 A RMS, capturing the full motor inrush waveform. Once the signal stabilises below the $G = 2$ threshold, the firmware switches back.

**High-sensitivity mode: $G = 4$ or higher.** For loads drawing less than approximately 5 A (where the signal at $G = 2$ is below 15% of FSR), the firmware can increase the gain to $G = 4$, $G = 8$, or $G = 16$ for better waveform fidelity during classification. At $G = 16$:

$$V_{\text{FSR}}\big|_{G=16} = \pm 75 \text{ mV}$$

$$I_{p,\text{clip}}\big|_{G=16} = \frac{0.075}{10} \times 1000 = 7.5 \text{ A (peak)} = 5.3 \text{ A (RMS)}$$

The noise floor at $G = 16$ is approximately 0.95 $\mu$V rms, giving a current resolution of:

$$\Delta I_{p,\text{noise}}\big|_{G=16} = \frac{0.95 \times 10^{-6}}{10} \times 1000 = 0.095 \text{ mA}$$

This allows meaningful waveform capture and harmonic analysis down to loads as small as 50–100 mA (phone chargers, LED bulbs).

The gain-ranging transition thresholds:

| PGA Gain | FSR ($\pm$ V) | Clip Threshold (A RMS) | Noise Floor (mA) | Typical Use |
|----------|-------------|----------------------|-----------------|-------------|
| $G = 1$ | $\pm 1.2$ | 84.9 | 0.530 | Motor inrush transients |
| $G = 2$ (default) | $\pm 0.6$ | 42.4 | 0.350 | Normal operation, 5–30 A loads |
| $G = 4$ | $\pm 0.3$ | 21.2 | 0.200 | Medium loads, 2–15 A |
| $G = 16$ | $\pm 0.075$ | 5.3 | 0.095 | Small loads, 0.05–5 A |

### 2.4 Burden Resistor Specification

The burden resistor dissipates power at the 30 A target:

$$P_b = I_{s,\text{target}}^2 \cdot R_b = (30 \times 10^{-3})^2 \times 10 = 9 \text{ mW}$$

At the CT's full rated current of 100 A (brief inrush event):

$$P_{b,\text{max}} = (100 \times 10^{-3})^2 \times 10 = 100 \text{ mW}$$

Both are well within a 1/4 W resistor's rating. The resistor's temperature coefficient (tempco) directly affects metering accuracy. For 0.5% total accuracy budget, the burden resistor should contribute no more than 0.1% drift over the operating temperature range:

$$\text{Required tempco} \leq \frac{0.001}{(70 - 25)} = 22.2 \text{ ppm/}^\circ\text{C}$$

Select: **2 $\times$ 5 $\Omega$, 0.1% tolerance, 15 ppm/$^\circ$C metal film resistor** (e.g., Vishay MRS25 or Yageo MFR series). Matched pair from the same reel to minimise differential imbalance.

---

## 3. Voltage Channel Design — ZMPT101B Isolated Voltage Transformer

### 3.1 Operating Principle

The ZMPT101B is a current-type precision voltage transformer with a 1:1 turns ratio (1000:1000). Unlike a conventional voltage transformer where the secondary voltage directly represents the primary voltage, the ZMPT101B operates by establishing a current proportional to the mains voltage in its primary winding, and faithfully reproducing that same current in the secondary winding. The primary current is set by an external current-limiting resistor $R_L$, and the secondary current is converted to a measurable voltage by a sampling (burden) resistor $R_s$.

The signal flow is:

$$V_{\text{mains}} \xrightarrow{R_L} I_{\text{primary}} \xrightarrow{1:1} I_{\text{secondary}} \xrightarrow{R_s} V_{\text{out}}$$

This provides 4 kV galvanic isolation between the mains and the measurement circuit, eliminating the open-neutral hazard that exists with a direct resistive voltage divider.

### 3.2 Current-Limiting Resistor Selection

The primary operating current at nominal mains voltage is:

$$I_{\text{op}} = \frac{V_{\text{rms}}}{R_L}$$

The ZMPT101B datasheet recommends an operating current of 1–2 mA when the rated input voltage is 220 V or above. However, the OpenEnergyMonitor project's characterisation of this transformer found that operating at approximately 4 mA significantly reduces both the magnitude and the variation of the phase angle error. The phase error curve is flatter between 2 mA and 4 mA than between 1 mA and 2 mA.

We design for $I_{\text{op}} = 2$ mA as a starting point (the datasheet-recommended value at 220 V), with an option to increase to 4 mA if phase error calibration proves insufficient:

At $I_{\text{op}} = 2$ mA:

$$R_L = \frac{V_{\text{nom}}}{I_{\text{op}}} = \frac{220}{2 \times 10^{-3}} = 110 \text{ k}\Omega$$

The nearest standard value is **110 k$\Omega$**. The actual operating current at 220 V:

$$I_{\text{op,actual}} = \frac{220}{110{,}000} = 2.0 \text{ mA}$$

At $I_{\text{op}} = 4$ mA (reduced phase error variant):

$$R_L = \frac{V_{\text{nom}}}{I_{\text{op}}} = \frac{220}{4 \times 10^{-3}} = 55 \text{ k}\Omega$$

Use 3 $\times$ 18 k$\Omega$ = 54 k$\Omega$ in series (for voltage rating, each resistor sees $\leq 220/3 = 73$ V, well within 200 V rating), or a single 56 k$\Omega$.

### 3.3 Voltage Rating of the Current-Limiting Resistor

At 220 V rms ($\pm 10$% = up to 242 V rms, 342 V peak), the current-limiting resistor must withstand the full mains voltage. Standard through-hole resistors are rated for 200–250 V. For the 110 k$\Omega$ design:

$$V_{R_L} \approx V_{\text{mains}} \quad \text{(since the transformer winding impedance is negligible compared to } R_L \text{)}$$

At 242 V rms, a single 110 k$\Omega$ resistor would see the full voltage. Using **two 56 k$\Omega$ resistors in series** (total 112 k$\Omega$), each sees half the voltage ($\leq 121$ V rms), safely within the 200 V rating per resistor.

The power dissipation in $R_L$:

$$P_{R_L} = I_{\text{op}}^2 \cdot R_L = (2 \times 10^{-3})^2 \times 112{,}000 = 0.448 \text{ W}$$

At 4 mA operation ($R_L = 56$ k$\Omega$):

$$P_{R_L} = (4 \times 10^{-3})^2 \times 56{,}000 = 0.896 \text{ W}$$

For the 2 mA variant, two 1/2 W resistors in series are adequate. For the 4 mA variant, three 1 W resistors in series (3 $\times$ 18 k$\Omega$) provide both voltage rating and power dissipation margin.

The datasheet recommends selecting resistors rated at least 4$\times$ the computed dissipation for reliability:

$$P_{\text{rated}} \geq 4 \times P_{R_L} = 4 \times 0.448 = 1.79 \text{ W (for 2 mA variant)}$$

Two 1 W metal film resistors in series satisfy this.

### 3.4 Sampling Resistor Selection

The secondary current equals the primary current (1:1 ratio). The sampling resistor $R_s$ converts this to a voltage for the ADC:

$$V_{\text{out}} = I_{\text{secondary}} \cdot R_s = I_{\text{op}} \cdot R_s$$

The peak secondary current at maximum mains voltage (242 V rms):

$$I_{s,\text{pk}} = \frac{V_{\text{pk,max}}}{R_L} = \frac{242 \times \sqrt{2}}{112{,}000} = \frac{342.2}{112{,}000} = 3.056 \text{ mA}$$

The peak output voltage must not exceed the ADS131M02's full-scale input ($\pm 1.2$ V at $G = 1$):

$$V_{\text{out,pk}} = I_{s,\text{pk}} \cdot R_s \leq V_{\text{ref}} = 1.2 \text{ V}$$

$$R_{s,\text{max}} = \frac{V_{\text{ref}}}{I_{s,\text{pk}}} = \frac{1.2}{3.056 \times 10^{-3}} = 392.7 \text{ } \Omega$$

We target approximately 80% of full-scale at maximum mains voltage for headroom:

$$R_{s,\text{design}} = 0.8 \times R_{s,\text{max}} = 0.8 \times 392.7 = 314 \text{ } \Omega$$

The nearest standard value is **330 $\Omega$**. Let us verify the output levels:

At nominal 220 V rms:

$$V_{\text{out,pk}} = \frac{220 \times \sqrt{2}}{112{,}000} \times 330 = \frac{311.1}{112{,}000} \times 330 = 2.778 \times 10^{-3} \times 330 = 0.917 \text{ V}$$

$$V_{\text{out,rms}} = \frac{220}{112{,}000} \times 330 = 0.6482 \text{ V}$$

At maximum 242 V rms (+10%):

$$V_{\text{out,pk,max}} = \frac{342.2}{112{,}000} \times 330 = 1.008 \text{ V}$$

At minimum 198 V rms ($-10$%):

$$V_{\text{out,pk,min}} = \frac{198 \times \sqrt{2}}{112{,}000} \times 330 = 0.825 \text{ V}$$

The utilisation at nominal voltage is $0.917 / 1.2 = 76.4$% — good use of the ADC range while maintaining adequate headroom for surges.

### 3.5 Effective Voltage Divider Ratio

The overall transfer function from mains voltage to ADC input voltage is:

$$\alpha_V = \frac{R_s}{R_L} = \frac{330}{112{,}000} = 2.946 \times 10^{-3}$$

This is the quantity used in firmware to convert ADC readings back to mains voltage:

$$V_{\text{mains}} = \frac{V_{\text{ADC}}}{\alpha_V}$$

### 3.6 Voltage Measurement Resolution

The noise-limited effective voltage resolution, referred back to the primary mains:

$$\Delta V_{\text{mains}} = \frac{e_n}{\alpha_V} = \frac{7.56 \times 10^{-6}}{2.946 \times 10^{-3}} = 2.57 \text{ mV rms}$$

As a fraction of nominal voltage:

$$\frac{\Delta V_{\text{mains}}}{V_{\text{nom}}} = \frac{2.57 \times 10^{-3}}{220} = 0.00117\% \approx 0.001\%$$

This is approximately 50x better than the PZEM-004T's 0.1 V resolution (0.045% at 220 V).

### 3.7 Verification of Error Sources

**ADC loading effect on the sampling resistor.** The ADS131M02's input impedance at $G \leq 4$ is approximately 300 k$\Omega$ (differential). The sampling resistor $R_s = 330 \text{ } \Omega$ is in parallel with this:

$$R_{s,\text{loaded}} = R_s \| Z_{\text{in}} = \frac{330 \times 300{,}000}{330 + 300{,}000} = \frac{99{,}000{,}000}{300{,}330} = 329.64 \text{ } \Omega$$

The gain error from loading:

$$\epsilon_{\text{load}} = \frac{R_s - R_{s,\text{loaded}}}{R_s} \times 100 = \frac{330 - 329.64}{330} \times 100 = 0.11\%$$

This is a constant systematic error, fully absorbed by gain calibration.

**Bias current offset.** The ADC's input bias current ($I_B \approx 0.6 \text{ } \mu$A) through $R_s$:

$$V_{\text{offset,bias}} = I_B \cdot R_s = 0.6 \times 10^{-6} \times 330 = 0.198 \text{ mV}$$

As a fraction of RMS output voltage:

$$\epsilon_{\text{bias}} = \frac{0.198 \times 10^{-3}}{0.6482} \times 100 = 0.031\%$$

Negligible. This is a DC offset on an AC signal; it is automatically rejected by the RMS computation in firmware (which removes the DC component before computing RMS).

**Transformer accuracy.** The ZMPT101B datasheet specifies a permissible error of $-0.3$% to $+0.2$% and linearity of $\leq 0.2$% across 20–120% of rated input. After gain calibration at the nominal operating point, the residual error across the $\pm 10$% mains voltage range is bounded by the linearity specification: $\leq 0.2$%.

**Sampling resistor specification.** The sampling resistor $R_s$ operates at very low power:

$$P_{R_s} = I_{\text{op}}^2 \cdot R_s = (2 \times 10^{-3})^2 \times 330 = 1.32 \text{ mW}$$

Select: **2 $\times$ 160 $\Omega$, 0.1% tolerance, 15 ppm/$^\circ$C metal film resistor**. The sampling resistor is split into two equal halves with the center tap connected to AGND, identical to the current channel's split burden configuration, for balanced differential drive and optimal THD.

### 3.8 Circuit Topology — Voltage Channel

```
               MAINS SIDE (isolated by transformer)     │    LOW-VOLTAGE SIDE
                                                        │
AC Line (L) ── R_La ── R_Lb ──┐                        │
                   (56k)(56k)  │                        │
                               │  ┌──────────┐          │
                               ├──┤ ZMPT101B ├──────────┼── Rs/2 ──┬── Rfilt ──┬── AIN1P
                               │  │  (1:1)   │          │  (165Ω)  │   (100Ω)  │
AC Neutral (N) ───────────────┘  └──────────┘          │         AGND        Cdiff
                                                        │          │   (680nF) │
                                    4kV isolation       │  Rs/2 ──┘── Rfilt ──┴── AIN1N
                                       barrier         │  (165Ω)      (100Ω)
```

The ZMPT101B provides 4 kV galvanic isolation. The primary winding is connected to the AC mains through the current-limiting resistor $R_L$. The secondary winding feeds a split sampling resistor ($2 \times 160 \text{ } \Omega$, center-tapped to AGND) for balanced differential drive. Matched filter resistors ($R_{\text{filt}} = 100 \text{ } \Omega$ on each line) and a differential capacitor ($C_{\text{diff}} = 680 \text{ nF}$) form the anti-aliasing filter. There is no galvanic connection between the mains and the measurement circuit.

---

## 4. Anti-Aliasing Filter Design

### 4.1 Nyquist Consideration

At a sample rate of $f_s = 4$ kSPS (the recommended operating point, see Section 7), the Nyquist frequency is:

$$f_{\text{Nyquist}} = \frac{f_s}{2} = 2 \text{ kHz}$$

The ADS131M02 uses a sinc3 digital decimation filter internally, which provides significant rejection of frequencies above $f_s / 2$. However, the sigma-delta modulator's analog input bandwidth extends far beyond $f_s$, so an external anti-aliasing filter is still needed to reject high-frequency noise (switching noise, EMI) that could alias into the signal band.

The signal bandwidth of interest extends to the 15th harmonic:

$$f_{\text{sig,max}} = 15 \times f_{\text{mains}} = 15 \times 60 = 900 \text{ Hz}$$

We need the filter to pass this bandwidth with minimal attenuation while suppressing content above $f_s / 2 = 2$ kHz. A first-order RC low-pass filter is sufficient because the sinc3 digital filter provides additional rolloff.

### 4.2 Filter Topology — Differential RC Filter

Per the ADS131M02 datasheet (Section 9.2.2.1), the recommended anti-aliasing filter for differential inputs uses matched series resistors on each input line with a differential capacitor across the ADC pins. This topology is applied identically to both channels:

```
Burden/Sampling         Anti-Aliasing Filter              ADC
Resistor (split)        (identical on both channels)
────────────────        ────────────────────────          ─────

                        Rfilt               Cdiff
Sensor+ ── R/2 ──┬──── (100Ω) ────┬──────────────────── AINxP
                  │                │
                 AGND            (680nF)                ADS131M02
                  │                │
Sensor- ── R/2 ──┘──── (100Ω) ────┴──────────────────── AINxN
                        Rfilt
```

The split burden/sampling resistor (center-tapped to AGND) produces a balanced differential signal. The filter resistors $R_{\text{filt}}$ are separate components from the burden/sampling resistors — they serve exclusively as the R in the RC filter and as current limiters for ADC input protection. The differential capacitor $C_{\text{diff}}$ spans the two ADC input pins.

This topology has three important properties. First, the burden/sampling resistor value is decoupled from the filter cutoff — each can be optimised independently. Second, because both channels use identical $R_{\text{filt}}$ and $C_{\text{diff}}$ values, the filter introduces zero differential phase error between the voltage and current channels. Third, the matched series resistors on each line reject common-mode noise that couples through transformer winding capacitance.

### 4.3 Filter Cutoff Frequency Selection

For this differential topology, the $-3$ dB cutoff frequency is:

$$f_c = \frac{1}{2\pi R_{\text{filt}} C_{\text{diff}}}$$

We want minimal attenuation at 900 Hz (less than 0.5 dB) and significant attenuation above 2 kHz. The attenuation of a first-order filter at frequency $f$ is:

$$A(f) = \frac{1}{\sqrt{1 + (f / f_c)^2}}$$

At $f = 900$ Hz with $A \geq 0.944$ ($-0.5$ dB):

$$0.944 = \frac{1}{\sqrt{1 + (900 / f_c)^2}}$$

$$f_c = \frac{900}{\sqrt{0.1225}} = \frac{900}{0.350} = 2571 \text{ Hz}$$

### 4.4 Component Values (Both Channels Identical)

Selecting $R_{\text{filt}} = 100 \text{ } \Omega$:

$$C_{\text{diff}} = \frac{1}{2\pi R_{\text{filt}} f_c} = \frac{1}{2\pi \times 100 \times 2571} = 619 \text{ nF}$$

Choose the nearest standard value: **$C_{\text{diff}} = 680$ nF** (ceramic C0G/NP0 for low distortion).

Actual cutoff:

$$f_c = \frac{1}{2\pi \times 100 \times 680 \times 10^{-9}} = 2341 \text{ Hz}$$

Attenuation at 900 Hz:

$$A(900) = \frac{1}{\sqrt{1 + (900 / 2341)^2}} = \frac{1}{\sqrt{1.1478}} = 0.934 = -0.59 \text{ dB}$$

This is a systematic 6.6% amplitude reduction at the 15th harmonic. Since we know the filter's exact transfer function, this can be corrected in firmware when computing harmonic magnitudes. At the fundamental (60 Hz):

$$A(60) = \frac{1}{\sqrt{1 + (60 / 2341)^2}} = \frac{1}{\sqrt{1.000657}} = 0.99967 = -0.003 \text{ dB}$$

Negligible — 0.033% amplitude error at the fundamental frequency.

### 4.5 Differential Phase Error Between Channels

Because both channels use identical filter components ($R_{\text{filt}} = 100 \text{ } \Omega$, $C_{\text{diff}} = 680 \text{ nF}$), the phase lag introduced by the filter is the same on both channels:

$$\phi_{\text{filter}} = -\arctan\left(\frac{60}{2341}\right) = -1.47^\circ \quad \text{(both channels)}$$

The differential filter phase error is therefore:

$$\Delta\phi_{\text{filter}} = \phi_{V,\text{filter}} - \phi_{I,\text{filter}} = -1.47^\circ - (-1.47^\circ) = 0^\circ$$

This eliminates the filter contribution to the phase error budget entirely — a direct consequence of using matched filter components on both channels.

### 4.6 SPICE Netlist — Complete Current Channel

```spice
* ============================================================
* Current Channel: PZEM CT (1:1000) + Split Burden + Differential Filter
* ============================================================

.subckt PZEM_CT pri_p pri_n sec_p sec_n
  K1 Lpri Lsec 0.999
  Lpri pri_p pri_n 1H
  Lsec sec_p sec_n 1H
.ends PZEM_CT

.subckt ADS131M02_CH ainp ainn
  Rin ainp ainn 300k
.ends ADS131M02_CH

* --- Primary conductor (load current, 30A target) ---
Iload load_p load_n SIN(0 42.43 60)
Rload load_p load_n 1

* --- Current transformer (PZEM-004T bundled, 1:1000) ---
X_CT load_p load_n ct_p ct_n PZEM_CT

* --- Split burden resistor (2 x 5 ohm, center-tapped to AGND) ---
Rb_top ct_p  0 5
Rb_bot ct_n  0 5

* --- Anti-aliasing filter (differential) ---
Rfilt_p ct_p  ain0p 100
Rfilt_n ct_n  ain0n 100
Cdiff   ain0p ain0n 680n

* --- ADC differential input ---
X_ADC ain0p ain0n ADS131M02_CH

.end
```

### 4.7 SPICE Netlist — Complete Voltage Channel

```spice
* ============================================================
* Voltage Channel: ZMPT101B + Split Sampling R + Differential Filter
* ============================================================

.subckt ZMPT101B pri_p pri_n sec_p sec_n
  K1 Lpri Lsec 0.999
  Lpri pri_p pri_n 10H
  Lsec sec_p sec_n 10H
.ends ZMPT101B

.subckt ADS131M02_CH ainp ainn
  Rin ainp ainn 300k
.ends ADS131M02_CH

* --- Mains source ---
Vmains vline vneutral SIN(0 311.13 60)

* --- Current-limiting resistors (2 x 56k in series) ---
RLa vline    net_rl  56k
RLb net_rl   vt_pri  56k

* --- ZMPT101B voltage transformer ---
X_VT vt_pri vneutral vt_sec_p vt_sec_n ZMPT101B

* --- Split sampling resistor (2 x 160 ohm, center-tapped to AGND) ---
Rs_top vt_sec_p 0 160
Rs_bot vt_sec_n 0 160

* --- Anti-aliasing filter (differential, identical to current channel) ---
Rfilt_vp vt_sec_p ain1p 100
Rfilt_vn vt_sec_n ain1n 100
Cdiff_v  ain1p    ain1n 680n

* --- ADC differential input ---
X_ADC1 ain1p ain1n ADS131M02_CH

.end
```

---

## 5. Phase Error Budget

### 5.1 Sources of Phase Error

The total differential phase error between the voltage and current measurement channels is the sum of contributions from four sources. Each source introduces a phase lag (or lead) at the fundamental frequency (60 Hz), and the net differential determines the systematic error in power factor measurement.

### 5.2 ZMPT101B Voltage Transformer Phase Error

The ZMPT101B datasheet specifies a phase angle error of $\leq 20$ arcminutes at 2 mA operating current:

$$\phi_{\text{ZMPT}} \leq 20' = 0.333^\circ \quad \text{(lagging, voltage output lags input)}$$

At 4 mA operating current, the OpenEnergyMonitor project's characterisation shows the phase error is significantly reduced and the curve is flatter. Conservatively estimate:

$$\phi_{\text{ZMPT}} \approx 0.2^\circ \text{ at } 4 \text{ mA, } \approx 0.33^\circ \text{ at } 2 \text{ mA}$$

This phase shift is constant because the mains voltage (and therefore the transformer operating current) does not change significantly during operation.

### 5.3 PZEM-004T Bundled CT Phase Error

The PZEM-004T's bundled CT does not have published phase error specifications. However, several properties of the CT allow a reasonable estimate of its phase behaviour.

The CT is a solid-core toroidal design (no air gap), which generally produces lower and more stable phase error than split-core CTs. It is rated for 100 A but operates at 30 A (30% of rated capacity), placing it deep in the linear region of its magnetization curve where the magnetizing current is a small fraction of the load current. The turns ratio (1:1000) and the modest burden resistance (10 $\Omega$) result in low secondary voltage (0.424 V peak at 30 A), minimising the voltage-driven component of magnetizing current error.

Based on analogous solid-core toroidal CTs of similar construction (e.g., HWCT-004, ZMCT103C), the phase error is estimated at:

| Primary Current | Estimated CT Phase Error |
|----------------|-------------------------|
| 30 A (target) | $\approx 0.2^\circ$ – $0.5^\circ$ |
| 10 A | $\approx 0.3^\circ$ – $0.8^\circ$ |
| 5 A | $\approx 0.5^\circ$ – $1.0^\circ$ |
| 1 A | $\approx 0.5^\circ$ – $1.5^\circ$ |

These values are estimates. The actual phase error must be determined empirically during the calibration procedure (Section 10.3). The calibration procedure is designed to accommodate CTs with unknown phase characteristics — a single-point or two-point calibration using a known resistive load will determine the actual correction needed.

The CT is operating at 30% of its rated capacity, which is favorable: at this operating point, the core is far from saturation, the magnetizing current is proportionally small, and the phase error tends to be stable and well-behaved.

### 5.4 Anti-Aliasing Filter Differential Phase Error

Both channels use identical differential RC filters ($R_{\text{filt}} = 100 \text{ } \Omega$, $C_{\text{diff}} = 680 \text{ nF}$, $f_c = 2341 \text{ Hz}$). The phase lag at 60 Hz is the same on both channels:

$$\phi_{\text{filter}} = -\arctan\left(\frac{60}{2341}\right) = -\arctan(0.0256) = -1.47^\circ \quad \text{(both channels)}$$

Because the filters are matched, the differential phase error is:

$$\Delta\phi_{\text{filter}} = 0^\circ$$

This is a direct consequence of using identical filter components on both channels rather than relying on the burden/sampling resistor as part of the filter network (which would give different cutoffs due to different R values).

### 5.5 ADC Sampling Phase Error

The ADS131M02 samples both channels simultaneously — the two sigma-delta modulators are clocked by the same edge. Therefore:

$$\Delta\phi_{\text{ADC}} = 0^\circ$$

This is the fundamental advantage of the ADS131M02 over multiplexed ADC architectures.

### 5.6 Total Phase Error Budget

| Source | Voltage Channel | Current Channel | Differential ($V - I$) | Type |
|--------|----------------|-----------------|----------------------|------|
| ZMPT101B transformer | $-0.33^\circ$ (lag) | — | $-0.33^\circ$ | Constant |
| PZEM CT (estimated, at 30 A) | — | $-0.2^\circ$ to $-0.5^\circ$ (lag) | $+0.2^\circ$ to $+0.5^\circ$ | Needs characterisation |
| Anti-aliasing filter (matched) | $-1.47^\circ$ | $-1.47^\circ$ | $0^\circ$ | Matched (zero) |
| ADC simultaneous sampling | $0^\circ$ | $0^\circ$ | $0^\circ$ | — |
| **Total (uncalibrated, at 30 A)** | — | — | **$-0.13^\circ$ to $+0.17^\circ$** | **Requires calibration** |

### 5.7 Phase Error Correction Strategy

The total uncalibrated phase error at 30 A is estimated between $-0.13°$ and $+0.17°$ depending on the CT's actual phase characteristics. Since the CT's phase error is unknown, the correction value must be determined empirically during calibration (Section 10.3) rather than computed from datasheet values.

The PHASE register applies a sub-sample time shift in increments of one modulator clock period:

$$\Delta t_{\text{step}} = \frac{1}{f_{\text{CLKIN}}} = \frac{1}{8.192 \times 10^6} = 122.07 \text{ ns}$$

The corresponding phase resolution at 60 Hz:

$$\Delta\phi_{\text{step}} = 360^\circ \times f_{\text{mains}} \times \Delta t_{\text{step}} = 360 \times 60 \times 122.07 \times 10^{-9} = 0.00264^\circ$$

For a hypothetical measured phase error of $0.15^\circ$:

$$\text{PHASE register value} = \text{round}\left(\frac{0.15}{0.00264}\right) = 57 \text{ (integer steps)}$$

The PHASE register has sufficient range to correct errors up to several degrees, so even if the CT's phase error turns out to be larger than estimated, the hardware correction mechanism is adequate. One register write during firmware initialisation — no runtime correction needed for a stable, well-characterised CT.

### 5.8 Post-Calibration Residual Phase Error

After the single-point correction at 30 A, the residual phase error is bounded by the CT's phase variation across the operating current range. Based on the estimated phase error table in Section 5.3, the worst-case deviation from the 30 A calibration point occurs at low currents (1–5 A), where the phase error may increase by up to $0.5°$ – $1.0°$ from the calibrated value.

If initial calibration reveals that the CT's phase error varies significantly with current (more than $0.5°$ from the calibration point), a two-point correction strategy can be employed: calibrate at 5 A and 25 A using known resistive loads, store both (current, phase_error) pairs in firmware, and interpolate at runtime. This reduces the worst-case residual to approximately $\pm 0.25°$.

The resulting power measurement error at $\text{PF} = 0.8$ ($\theta = 36.87°$) with a residual phase error of $\Delta\phi = 0.5°$ (worst case, single-point calibration):

$$\epsilon_P = \frac{\cos(36.87° + 0.5°) - \cos(36.87°)}{\cos(36.87°)} = \frac{0.7960 - 0.8}{0.8} = -0.50\%$$

At $\text{PF} = 0.5$ ($\theta = 60°$) with $\Delta\phi = 0.5°$:

$$\epsilon_P = \frac{\cos(60.5°) - \cos(60°)}{\cos(60°)} = \frac{0.4957 - 0.5}{0.5} = -0.87\%$$

The single-point calibration meets the 0.5% target at PF $\geq 0.8$. For loads with PF $< 0.8$, a two-point calibration would be needed to stay within budget. Since your primary classification targets (aircon, fridge, fan) operate at PF = 0.6–0.9, the two-point calibration is recommended if the initial characterisation reveals significant current-dependent phase variation.

### 5.9 Phase Behaviour at Harmonics

Both the ZMPT101B and the PZEM-004T CT have frequency-dependent phase characteristics. At higher harmonics, the phase shifts differ from the fundamental. However, for load characterisation, the primary features used are harmonic magnitudes (H3/H1, H5/H1, THD, crest factor), not harmonic phase angles. The magnitude response of both transformers is expected to be flat within $\pm 1$% from 50 Hz to 1 kHz for toroidal cores of this construction. The matched anti-aliasing filters ensure no additional differential magnitude or phase error is introduced at any harmonic frequency.

The one harmonic-phase-sensitive quantity is the displacement power factor, which is measured at the fundamental (60 Hz) where our calibration is precise.

---

## 6. Power Supply and Isolation Architecture

### 6.1 ADS131M02 Power Requirements

| Rail | Voltage | Typical Current | Notes |
|------|---------|-----------------|-------|
| AVDD | 3.0 V (nom.) | 3.5 mA | Analog supply, 2.7 – 3.6 V range |
| DVDD | 3.3 V | 0.5 mA | Digital supply, shared with ESP32-S3 |
| Total | — | ~4 mA | — |

Since the ADS131M02 operates at 3.0 – 3.3 V and the ESP32-S3 also runs at 3.3 V, the ADS131M02 shares the same 3.3 V regulated supply as the ESP32-S3.

### 6.2 Isolation Architecture — Fully Galvanically Isolated

With the ZMPT101B replacing the resistive voltage divider, both measurement channels now have full galvanic isolation from the AC mains:

- **Current channel:** The PZEM-004T bundled CT is a solid-core toroidal transformer providing magnetic coupling between the primary conductor and the secondary winding. Isolation voltage is estimated at $\geq 1$ kV based on the construction of similar toroidal CTs, which is adequate for 220 V residential mains (the peak voltage is 311 V).
- **Voltage channel:** The ZMPT101B provides magnetic coupling with 4 kV dielectric strength between the primary (mains-connected) and secondary (measurement-side) windings.

The ADS131M02 and the ESP32-S3 sit entirely on the low-voltage side. There is no resistive DC path from the mains to any point on the low-voltage circuit. An open-neutral fault cannot elevate the low-voltage ground to mains potential.

No digital isolator (ADuM1401) is needed on the SPI bus. The ADS131M02 connects directly to the ESP32-S3's SPI peripheral at full speed (up to 25 MHz SCLK).

```
    MAINS SIDE                    ║ ISOLATION ║    LOW-VOLTAGE SIDE
    (dangerous)                   ║  BARRIER  ║    (safe, 3.3V)
                                  ║           ║
    ┌─── AC Live (L)              ║           ║
    │                             ║           ║
    │  R_La ── R_Lb ──┐           ║           ║
    │  (56k)  (56k)   │           ║           ║
    │                  │ ┌──────┐ ║           ║  Rs/2    Rfilt_vp       Cdiff_v
    │                  ├─┤ZMPT  ├─╫───────────╫─(160Ω)─┬─(100Ω)──┬── AIN1P ──┐
    │                  │ │101B  │ ║           ║        │         │           │
    └── AC Neutral ────┘ │(1:1) │ ║  4kV      ║       AGND    (680nF)       │
                         └──────┘ ║ isolation ║        │         │           │
                                  ║           ╠─(160Ω)─┘─(100Ω)──┴── AIN1N   │
                                  ║           ║  Rs/2    Rfilt_vn             │
                                  ║           ║                               │
    ┌─── Load wire ───────────┐   ║           ║                               │
    │  (passes through CT)    │   ║           ║  Rb/2    Rfilt_ip   Cdiff_i   │
    │                         │   ║           ║                               │
    │         ┌──────┐        │   ║           ║                               │
    │         │PZEM  │        │   ║           ║                               │
    │         │CT    ├────────╫───╫──(5Ω)──┬─╫─(100Ω)──┬── AIN0P             │
    │         │1:1000│        ║   ║        │ ║         │                      │
    │         │100A  ├────────╫───╫──     AGND       (680nF)          ADS131M02
    │         └──────┘        ║   ║  (5Ω)──┘ ║         │           (3.3V, shared
    │                         ║   ║  Rb/2    ╫─(100Ω)──┴── AIN0N    GND w/ ESP32)
    │          ~1kV isolation ║   ║          ║  Rfilt_in             │
    │                         ║   ║          ║                       │
    │                         ║   ║          ║     SCLK ────────────┤── GPIO 18
    │                         ║   ║          ║     DIN  ────────────┤── GPIO 23
    │                         ║   ║          ║     DOUT ────────────┤── GPIO 19
    │                         ║   ║          ║     CS   ────────────┤── GPIO 5
    │                         ║   ║          ║     DRDY ────────────┤── GPIO 4
    │                         ║   ║          ║                       │
    │                         ║   ║          ║     AVDD ────────────┤── 3.3V
    │                         ║   ║          ║     DVDD ────────────┤── 3.3V
    │                         ║   ║          ║     AGND ────────────┤── GND
    │                         ║   ║          ║     DGND ────────────┤── GND
    │                         ║   ║          ║                       │
    └─────────────────────────┘   ║          ║     CLKIN ───────────┤── 8.192 MHz
                                  ║          ║                       │
```

### 6.3 Decoupling

The ADS131M02 datasheet specifies:

- AVDD: 1 $\mu$F ceramic (X7R or C0G) to AGND, placed within 5 mm of pin
- DVDD: 1 $\mu$F ceramic to DGND
- CAP (LDO output): 220 nF ceramic to DGND

The AGND and DGND pins should be connected together at a single point near the ADS131M02, forming a star ground. On a perfboard, this means running the analog ground and digital ground as separate wires to a common point at the ADC.

---

## 7. ADC Operating Mode Selection

### 7.1 Data Rate vs. Effective Resolution Trade-Off

The ADS131M02's noise performance improves at lower data rates because the sinc3 decimation filter averages more modulator samples. The relevant entries from the datasheet noise table (at $G = 1$, internal 1.2 V reference):

| Data Rate (SPS) | RMS Noise ($\mu$V) | Effective Resolution (bits) | Samples per 60 Hz Cycle |
|----------------|---------------------|----------------------------|------------------------|
| 1000 | 2.4 | 19.9 | 16.7 |
| 2000 | 3.7 | 19.3 | 33.3 |
| 4000 | 5.3 | 18.8 | 66.7 |
| 8000 | 7.56 | 18.3 | 133.3 |
| 16000 | 10.6 | 17.8 | 266.7 |
| 32000 | 15.1 | 17.3 | 533.3 |

### 7.2 Recommended Operating Point: 4 kSPS

At 4 kSPS, each 60 Hz cycle is sampled 66.7 times. By Nyquist, this supports harmonic analysis up to the 33rd harmonic (1980 Hz) — more than sufficient for the target of 15 harmonics.

The effective resolution of 18.8 bits gives a noise-limited dynamic range of:

$$\text{DR} = 6.02 \times 18.8 + 1.76 = 115 \text{ dB}$$

In linear terms, this is a dynamic range of approximately $562{,}000 : 1$, far exceeding the 5000:1 dynamic range target.

The noise-limited current resolution at 4 kSPS (at default $G = 2$, input-referred noise $\approx 3.5 \text{ } \mu$V):

$$\Delta I_{p,\text{noise}} = \frac{3.5 \times 10^{-6}}{10} \times 1000 = 0.350 \text{ mA rms (primary)}$$

The noise-limited voltage resolution at 4 kSPS:

$$\Delta V_{\text{mains}} = \frac{5.3 \times 10^{-6}}{2.946 \times 10^{-3}} = 1.80 \text{ mV rms (primary)}$$

Both are well below the PZEM-004T's resolution (1 mA current, 100 mV voltage). The current resolution is improved by running at PGA $G = 2$ as the default mode, which reduces input-referred noise while maintaining adequate dynamic range for the 30 A target.

### 7.3 SPI Throughput at 4 kSPS

The ADS131M02 SPI frame format contains:

| Field | Bits |
|-------|------|
| Status word | 16 |
| Channel 0 data | 24 |
| Channel 1 data | 24 |
| CRC | 16 |
| **Total** | **80** |

At 4 kSPS:

$$\text{SPI throughput} = 4000 \times 80 = 320 \text{ kbps}$$

The ADS131M02 supports SCLK up to 25 MHz. The bus utilisation:

$$\text{Utilisation} = \frac{320 \times 10^3}{25 \times 10^6} \times 100 = 1.28\%$$

The SPI bus is essentially idle. Each frame read takes:

$$t_{\text{frame}} = \frac{80}{25 \times 10^6} = 3.2 \text{ } \mu\text{s}$$

The inter-sample period at 4 kSPS is 250 $\mu$s, so the SPI read occupies only 1.28% of each sample period.

---

## 8. Complete Bill of Materials — Analog Front-End

| Ref | Description | Value | Spec | Qty |
|-----|-------------|-------|------|-----|
| U1 | ADS131M02IPWR | — | TSSOP-20, 24-bit dual-ch simultaneous ADC | 1 |
| VT1 | ZMPT101B voltage transformer | 1:1, 2 mA | 4 kV isolation, PCB mount | 1 |
| CT1 | PZEM-004T bundled CT | 100 A rated, 1:1000 | Solid-core toroidal, ~1 kV isolation (salvaged from PZEM module) | 1 |
| R_La | Current-limiting resistor (ZMPT101B) | 56 k$\Omega$ | 1 W, 1%, metal film, 200V rated | 1 |
| R_Lb | Current-limiting resistor (ZMPT101B) | 56 k$\Omega$ | 1 W, 1%, metal film, 200V rated | 1 |
| R_s1, R_s2 | Split sampling resistor (ZMPT101B) | 160 $\Omega$ | 0.1%, 15 ppm/$^\circ$C, metal film, matched pair | 2 |
| R_b1, R_b2 | Split CT burden resistor | 5 $\Omega$ | 0.1%, 15 ppm/$^\circ$C, metal film, matched pair | 2 |
| R_filt (x4) | Anti-alias filter resistor (both channels) | 100 $\Omega$ | 1%, metal film | 4 |
| C_diff (x2) | Anti-alias differential cap (both channels) | 680 nF | C0G/NP0 ceramic, 50 V | 2 |
| C1 | AVDD decoupling | 1 $\mu$F | C0G or X7R ceramic, 10 V | 1 |
| C2 | DVDD decoupling | 1 $\mu$F | X7R ceramic, 10 V | 1 |
| C3 | CAP pin (internal LDO) | 220 nF | X7R ceramic, 10 V | 1 |
| Y1 | Crystal oscillator | 8.192 MHz | HC-49S or CMOS oscillator, 20 ppm | 1 |
| CY1, CY2 | Crystal load caps (if using crystal) | 12 pF | C0G ceramic | 2 |
| J1 | TSSOP-20 to DIP adapter board | — | 0.65 mm pitch breakout | 1 |

---

## 9. Performance Summary and Comparison

| Parameter | PZEM-004T v3 | ADS131M02 + ZMPT101B + PZEM CT | Improvement |
|-----------|-------------|--------------------------------------|-------------|
| Voltage resolution | 0.1 V | 1.80 mV | ~56x |
| Current resolution (at $G = 2$) | 1 mA | 0.35 mA | ~2.9x |
| Voltage accuracy (after cal.) | 0.5% | $< 0.2$% | ~2.5x |
| Current accuracy (after cal.) | 0.5% | $< 0.2$% | ~2.5x |
| Power accuracy at PF=0.8 (after cal.) | 0.5% | $< 0.5$% | Comparable (CT-dependent) |
| Power factor resolution | 0.01 | $< 0.001$ (computed) | ~10x |
| Simultaneous V/I sampling | No (internal mux) | Yes | Eliminates phase skew |
| Waveform access | None | Full raw samples | Enables NILM |
| Harmonic analysis | Not possible | Up to 33rd harmonic | Enables classification |
| Sample rate | Internal (no access) | 4 kSPS configurable | Full control |
| Energy accumulation | Hardware counter | Firmware integration | More flexible |
| Interface to MCU | UART Modbus (9600 baud) | SPI (up to 25 MHz) | ~2600x bandwidth |
| Galvanic isolation (V channel) | Optocoupler (comms only) | ZMPT101B, 4 kV | Full signal isolation |
| Galvanic isolation (I channel) | CT (built-in) | Same CT (salvaged), ~1 kV | Reused |
| Galvanic isolation (digital bus) | Optocoupler (built-in) | Not needed (both sensors isolated) | Simpler |
| Phase error (total, uncalibrated) | Unknown (internal) | $-0.13°$ to $+0.17°$ (estimated) | Requires characterisation |
| Phase calibration complexity | N/A | Single PHASE register write (+ optional 2-point LUT) | Low |
| PGA gain ranging | Not available | $G = 1$ to $G = 128$, firmware-controlled | Adaptive sensitivity |
| Power consumption (meter) | ~1 W (from AC mains) | ~13 mW + 0.45 W ($R_L$) | Comparable |
| Current range (target) | 0 – 100 A | 0 – 30 A (CT rated to 100 A) | Adequate for residential |
| CT installation | Toroidal (wire pass-through) | Same CT reused | No change |
| Component count | 1 module | ~16 discrete parts + salvaged CT | More, but cheaper |

---

## 10. Calibration Procedure

### 10.1 Gain Calibration

The ADS131M02 has 24-bit gain calibration registers for each channel. The calibration coefficient is applied as:

$$\text{output}_{\text{cal}} = \text{output}_{\text{raw}} \times \frac{\text{GAIN\_REG}}{0x400000}$$

Where $0x400000$ represents unity gain (1.0). To calibrate:

1. Apply a known AC voltage (measured with a calibrated multimeter) to the mains input.
2. Read the ADC's computed Vrms over 100 cycles.
3. Compute the gain coefficient: $k_V = V_{\text{true}} / V_{\text{measured}}$.
4. Write $\text{GAIN\_REG} = \text{round}(k_V \times 0x400000)$.
5. Repeat for the current channel with a known load and calibrated clamp meter.

### 10.2 Offset Calibration

1. Short the differential inputs of each channel (disconnect sensors).
2. Read 1000 samples and compute the mean.
3. Write the negated mean into the channel's offset calibration register.

The ADS131M02 also provides a global-chop mode that automatically cancels DC offset by alternating input polarity between consecutive conversions. This halves the effective data rate (4 kSPS becomes 2 kSPS effective). At 2 kSPS you still get 33 samples per cycle — adequate for harmonics up to the 16th.

### 10.3 Phase Calibration

Since the PZEM-004T bundled CT has no published phase error data, the phase correction must be determined empirically. The procedure is:

1. Apply a purely resistive load of known power (e.g., 100 W incandescent bulb or high-power resistor at a current level near 30 A if possible, or at whatever current is available). The true power factor is 1.00.
2. Capture several hundred cycles of simultaneous V and I waveforms.
3. Compute the phase angle between V and I fundamentals using cross-correlation or FFT phase extraction.
4. The measured phase angle is the system's total phase error at that current level.
5. Write the correction into the ADS131M02's PHASE register:

$$\text{PHASE\_REG} = \text{round}\left(\frac{\phi_{\text{error}} \times f_{\text{CLKIN}}}{360^\circ \times f_{\text{mains}}}\right) = \text{round}\left(\frac{\phi_{\text{error}} \times 8{,}192{,}000}{360 \times 60}\right)$$

6. Optionally repeat at a second current level (e.g., 5 A) to check for current-dependent phase variation. If the phase error differs by more than $0.3°$ between the two current levels, implement a two-point firmware correction: store both (current, phase_error) pairs and interpolate at runtime based on the measured RMS current.

This empirical approach accommodates CTs with unknown phase characteristics and is the same procedure used in commercial energy meter production calibration.

### 10.5 Calibration Verification

After all calibration steps, verify the system against known loads:

| Test Load | Expected PF | Acceptable PF Error | Expected P Error |
|-----------|-------------|-------------------|-----------------|
| 100 W incandescent bulb | 1.00 | $\pm 0.003$ | $\pm 0.3$% |
| 60 W ceiling fan (low speed) | 0.55 – 0.75 | $\pm 0.005$ | $\pm 0.5$% |
| Laptop charger (SMPS) | 0.50 – 0.70 | $\pm 0.005$ | $\pm 0.5$% |
| Window-type aircon | 0.75 – 0.90 | $\pm 0.005$ | $\pm 0.5$% |

---

## 11. Firmware Integration Notes

### 11.1 FreeRTOS Task Architecture — ESP32-S3 (Dual-Core)

```
Core 0:
  ├── adcTask (priority 5)
  │     Triggered by DRDY GPIO interrupt
  │     Reads SPI frame (3.2 μs)
  │     Stores V[n], I[n] into circular buffer
  │     Every 67 samples (1 cycle at 4 kSPS):
  │       - Compute Vrms, Irms, P, Q, S, PF (SIMD-accelerated)
  │       - Accumulate energy delta (Wh)
  │       - Update shared MeterData struct (mutex)
  │     Every 2 seconds (triggered or periodic):
  │       - Run 128-point FFT on V and I buffers (esp-dsp SIMD)
  │       - Extract harmonic magnitudes and phases
  │       - Compute THD, crest factor, harmonic ratios
  │       - Run classification decision tree
  │       - Update shared ClassificationData struct (mutex)
  │
  └── other Core 0 tasks (relay control, watchdog, etc.)

Core 1:
  └── tdmaTask (priority 6, unchanged)
        Snapshots MeterData + ClassificationData at transmit time
        Packs into extended TelemetryPacket
        Transmits in assigned TDMA slot
```

### 11.2 Memory Requirements — ESP32-S3 (512 KB SRAM)

| Buffer | Size | Notes |
|--------|------|-------|
| ADC circular buffer (V) | 256 × 4 bytes = 1024 B | 3.8 cycles at 4 kSPS |
| ADC circular buffer (I) | 256 × 4 bytes = 1024 B | Mirrored |
| FFT working buffer | 256 × 8 bytes = 2048 B | Complex float pairs |
| Harmonic feature vector | 30 × 4 bytes = 120 B | 15 harmonics × 2 channels |
| Total DSP RAM | ~4.2 KB | Well within ESP32-S3's 512 KB SRAM |

### 11.3 Computational Budget (ESP32-S3, LX7 @ 240 MHz with SIMD)

| Operation | Frequency | Time per Execution | CPU Load |
|-----------|-----------|-------------------|----------|
| SPI frame read | 4000/s | 3.2 μs | 1.28% |
| Per-sample multiply-accumulate | 4000/s | ~0.1 μs | 0.04% |
| Cycle-end RMS + power computation | 60/s | ~10 μs | 0.06% |
| 128-point FFT (SIMD) | 0.5/s | ~15 μs | 0.0008% |
| Classification decision tree | 0.5/s | ~5 μs | 0.0003% |
| **Total DSP load on Core 0** | — | — | **~1.4%** |

This leaves over 98% of Core 0's capacity free for other tasks.

### 11.4 Computational Budget (STM32F411, Cortex-M4F @ 100 MHz, no SIMD)

The STM32F411 uses an ARM Cortex-M4 core with a hardware single-precision FPU but no SIMD instructions. All DSP operations execute as scalar floating-point, and the clock is 2.4x slower than the ESP32-S3. Additionally, the STM32F411 is single-core — the ADC acquisition, metering DSP, classification, and TDMA radio all share a single execution context and must be managed through interrupt priorities and cooperative scheduling.

**DSP library.** The ARM CMSIS-DSP library provides optimised implementations of FFT (`arm_cfft_f32`), RMS, dot product, and other vector operations. While not SIMD-accelerated on the M4 (SIMD requires Cortex-M7 or higher with Helium/MVE), the CMSIS-DSP routines use mixed-radix algorithms and loop unrolling that are significantly faster than naive implementations.

**Timing estimates.** A 128-point complex FFT using `arm_cfft_f32` on the Cortex-M4 at 168 MHz has been benchmarked at approximately 40 $\mu$s. Scaling to 100 MHz:

$$t_{\text{FFT,128}} = 40 \times \frac{168}{100} \approx 67 \text{ } \mu\text{s per FFT}$$

Two FFTs (one for V, one for I) require approximately 134 $\mu$s total — still negligible relative to the 250 $\mu$s inter-sample period at 4 kSPS.

**SPI bus allocation.** The STM32F411 has three SPI peripherals (SPI1, SPI2, SPI3). The ADS131M02 and SX1276 are assigned to separate SPI buses, eliminating bus contention:

- SPI1: ADS131M02 (SCLK up to 25 MHz)
- SPI2: SX1276 LoRa radio (SCLK up to 10 MHz)

Both SPI peripherals can use DMA for data transfer, freeing the CPU during multi-byte transactions.

**ADC acquisition via DMA.** The DRDY pin of the ADS131M02 triggers a GPIO EXTI interrupt. The ISR initiates a DMA-driven SPI read of the 80-bit frame. When the DMA transfer completes, a DMA completion ISR copies the two 24-bit sample values into the circular buffer. The CPU overhead per sample is limited to the ISR entry/exit and the DMA setup — approximately 5 $\mu$s total, not the full SPI transaction time.

| Operation | Frequency | Time per Execution | CPU Load |
|-----------|-----------|-------------------|----------|
| DRDY ISR + DMA SPI read | 4000/s | ~5 μs | 2.00% |
| Per-sample multiply-accumulate (V×I, V², I²) | 4000/s | ~0.2 μs | 0.08% |
| Cycle-end RMS + power computation | 60/s | ~20 μs | 0.12% |
| PGA gain-ranging check | 60/s | ~2 μs | 0.01% |
| 128-point FFT × 2 channels (CMSIS-DSP) | 0.5/s | ~134 μs | 0.007% |
| Harmonic magnitude + phase extraction | 0.5/s | ~20 μs | 0.001% |
| Classification decision tree | 0.5/s | ~10 μs | 0.0005% |
| **Total DSP load** | — | — | **~2.2%** |
| TDMA radio (packet build + SPI + TX wait) | ~0.1/s | ~5 ms | ~0.05% |
| **Total CPU load (DSP + radio)** | — | — | **~2.3%** |

The STM32F411 handles the full workload with approximately 97.7% idle capacity. The DSP load is roughly 1.6x higher than the ESP32-S3 (2.2% vs 1.4%), primarily due to the slower clock and scalar-only FPU. The FFT is the operation most affected by the absence of SIMD — approximately 4.5x slower per FFT (67 $\mu$s vs 15 $\mu$s) — but since it runs only twice per second, the impact on total CPU load is negligible.

**Interrupt priority scheme.** On the single-core STM32F411, correct interrupt nesting is essential to prevent the TDMA radio from blocking ADC sample acquisition:

```
Priority 0 (highest): DRDY EXTI ISR — initiates DMA SPI read for ADS131M02
Priority 1:           DMA completion ISR — stores samples, triggers cycle-end processing
Priority 2:           SysTick — FreeRTOS tick (1 ms)
Priority 3:           Radio DIO0 ISR — SX1276 TX/RX done interrupt
Priority 4 (lowest):  PendSV — FreeRTOS context switch
```

The DRDY interrupt at priority 0 ensures that no ADC sample is ever missed, even if the CPU is in the middle of a radio SPI transaction or FreeRTOS context switch. The DMA handles the actual SPI data transfer in hardware, so the ISR returns quickly (~1 $\mu$s).

### 11.5 FreeRTOS Task Architecture — STM32F411 (Single-Core)

```
Single core (Cortex-M4F @ 100 MHz):

  ├── adcTask (priority: osPriorityAboveNormal)
  │     Woken by DMA completion notification (binary semaphore)
  │     Copies V[n], I[n] from DMA buffer into circular buffer
  │     Per-sample: accumulate V², I², V×I for running RMS/power
  │     Every 67 samples (1 cycle at 4 kSPS):
  │       - Finalise Vrms, Irms, P, Q, S, PF
  │       - Check PGA gain range, adjust if needed (SPI register write)
  │       - Accumulate energy delta (Wh)
  │       - Update shared MeterData struct (mutex)
  │     Every 2 seconds (or on power-change trigger):
  │       - Run 128-point FFT on V and I buffers (CMSIS-DSP arm_cfft_f32)
  │       - Extract harmonic magnitudes and phases
  │       - Compute THD, crest factor, harmonic ratios
  │       - Run classification decision tree
  │       - Update shared ClassificationData struct (mutex)
  │
  ├── tdmaTask (priority: osPriorityNormal)
  │     Sleeps until TDMA slot timer fires
  │     Snapshots MeterData + ClassificationData under mutex
  │     Builds TelemetryPacket
  │     Transmits via SX1276 on SPI2 (DMA-driven)
  │     Waits for DIO0 TX-done interrupt
  │     Returns to sleep
  │
  └── sysTask (priority: osPriorityBelowNormal)
        Watchdog feed
        LED status blink
        Relay control (if applicable)
        Low-priority housekeeping
```

The adcTask runs at higher priority than tdmaTask to ensure metering computation is never starved by radio operations. The DRDY ISR and DMA run at interrupt level above all tasks, guaranteeing zero dropped samples regardless of task-level activity.

### 11.6 Memory Requirements — STM32F411 (128 KB SRAM)

| Buffer | Size | Notes |
|--------|------|-------|
| ADC circular buffer (V) | 256 × 4 bytes = 1024 B | 3.8 cycles at 4 kSPS |
| ADC circular buffer (I) | 256 × 4 bytes = 1024 B | Mirrored |
| DMA SPI receive buffer | 10 × 2 = 20 B | Double-buffered, 10 bytes per frame |
| FFT working buffer | 256 × 8 bytes = 2048 B | Complex float pairs (CMSIS-DSP in-place) |
| Harmonic feature vector | 30 × 4 bytes = 120 B | 15 harmonics × 2 channels |
| MeterData struct | ~48 B | Vrms, Irms, P, Q, S, PF, energy, timestamp |
| ClassificationData struct | ~32 B | Class, subclass, confidence, THD, crest |
| SX1276 TX/RX buffer | 256 B | LoRa packet buffer |
| FreeRTOS kernel + 3 task stacks | ~10 KB | 3 tasks × ~2 KB stack + kernel objects |
| **Total RAM usage** | **~14.6 KB** | **11.4% of 128 KB SRAM** |

The STM32F411's 128 KB SRAM is more than adequate. Over 113 KB remains free for application code variables, calibration tables, and any future expansion.

### 11.7 Comparison: ESP32-S3 (LX7) vs STM32F411 (Cortex-M4F)

| Parameter | ESP32-S3 (LX7) | STM32F411 (Cortex-M4F) |
|-----------|----------------|----------------------|
| Clock speed | 240 MHz | 100 MHz |
| Cores | 2 (dual-core) | 1 (single-core) |
| FPU | Single-precision | Single-precision |
| SIMD | PIE (128-bit, 4-wide) | None |
| DSP library | esp-dsp (SIMD-optimised) | CMSIS-DSP (scalar-optimised) |
| 128-pt FFT time | ~15 μs | ~67 μs |
| Total DSP CPU load | ~1.4% (Core 0 only) | ~2.2% |
| Radio isolation | Core 1 (hardware isolation) | Priority-based preemption |
| SRAM | 512 KB | 128 KB |
| DSP RAM usage | ~4.2 KB (0.8% of SRAM) | ~14.6 KB (11.4% of SRAM) |
| SPI peripherals | 4 (FSPI, SPI2, SPI3, +) | 3 (SPI1, SPI2, SPI3) |
| DMA channels | Limited (GDMA) | 16 streams (2 controllers) |
| ADC sample drop risk | None (dedicated core) | Negligible (ISR priority 0) |
| Overall headroom | 98.6% free on Core 0 | 97.7% free |
