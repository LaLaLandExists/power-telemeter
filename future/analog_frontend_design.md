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
| Maximum load current | $I_{\text{max}}$ | 50 | A |
| Peak current at full load | $I_{\text{pk}}$ | $50\sqrt{2} \approx 70.7$ | A |

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

### 1.3 Current Transformer Parameters (HWCT-004)

| Parameter | Symbol | Value | Unit |
|-----------|--------|-------|------|
| Rated primary current | $I_{p,\text{rated}}$ | 50 | A |
| Turns ratio | $n$ | 1 : 1000 | — |
| Rated secondary current | $I_{s,\text{rated}}$ | $50 / 1000 = 50$ | mA |
| Accuracy class | — | 0.1 | — |
| Proportional error | — | $\leq \pm 0.1$% | — |
| Phase angle error (above 10 A) | $\phi_{\text{CT}}$ | $< 0.2^\circ$ | — |
| Phase angle error (below 10 A) | $\phi_{\text{CT,low}}$ | $< 0.5^\circ$ | — |
| Linear range | — | 5% – 120% of rated | — |
| Isolation voltage | $V_{\text{iso,CT}}$ | 1000 | V |
| Mounting | — | PCB through-hole (toroidal) | — |

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
| Dynamic range for current | 5000:1 (10 mA to 50 A) |
| Galvanic isolation | Full isolation — both V and I channels |

---

## 2. Current Channel Design

### 2.1 Burden Resistor Selection

The CT secondary current is proportional to the primary current through the turns ratio $n$. For a primary current $I_p$, the secondary current is:

$$I_s = \frac{I_p}{n}$$

At full-scale primary current:

$$I_{s,\text{max}} = \frac{I_{\text{max}}}{n} = \frac{50}{1000} = 50 \text{ mA (rms)}$$

The corresponding peak secondary current:

$$I_{s,\text{pk}} = I_{s,\text{max}} \cdot \sqrt{2} = 50 \times 1.414 = 70.71 \text{ mA}$$

The burden resistor $R_b$ converts this current to a voltage across the ADC differential input. The ADS131M02 at PGA gain $G = 1$ accepts a full-scale differential input of $\pm V_{\text{ref}} = \pm 1.2$ V. We must ensure that the peak voltage across the burden never exceeds this:

$$V_{b,\text{pk}} = I_{s,\text{pk}} \cdot R_b \leq V_{\text{ref}}$$

Solving for the maximum burden resistance:

$$R_{b,\text{max,ADC}} = \frac{V_{\text{ref}}}{I_{s,\text{pk}}} = \frac{1.2}{70.71 \times 10^{-3}} = 16.97 \text{ } \Omega$$

We select $R_b = 10 \text{ } \Omega$ to provide generous headroom for motor startup inrush transients. At $R_b = 10 \text{ } \Omega$, the peak burden voltage at rated current is:

$$V_{b,\text{pk}} = I_{s,\text{pk}} \cdot R_b = 70.71 \times 10^{-3} \times 10 = 0.707 \text{ V}$$

This is $0.707 / 1.2 = 58.9$% of the ADC full-scale range. The inrush clipping threshold:

$$I_{p,\text{clip}} = \frac{V_{\text{ref}}}{R_b} \cdot n = \frac{1.2}{10} \times 1000 = 120 \text{ A (peak primary)}$$

which corresponds to an RMS inrush of $120 / \sqrt{2} \approx 84.9$ A — adequate headroom for motor startup transients on residential circuits (a 1.5 HP aircon compressor may draw 40–50 A peak inrush for 1–2 seconds).

### 2.2 Current Measurement Resolution

The RMS voltage developed across the burden at full-scale current (50 A rms primary) is:

$$V_{b,\text{rms}} = I_{s,\text{max}} \cdot R_b = 50 \times 10^{-3} \times 10 = 0.500 \text{ V}$$

The ADC's least significant bit (LSB) voltage at gain $G = 1$ is:

$$V_{\text{LSB}} = \frac{2 \cdot V_{\text{ref}}}{2^N} = \frac{2 \times 1.2}{2^{24}} = \frac{2.4}{16{,}777{,}216} = 143.05 \text{ nV}$$

The number of ADC codes spanning the full-scale burden voltage (peak-to-peak):

$$N_{\text{codes}} = \frac{2 \cdot V_{b,\text{pk}}}{V_{\text{LSB}}} = \frac{2 \times 0.707}{143.05 \times 10^{-9}} = 9{,}885{,}355 \text{ codes}$$

This gives an ideal current resolution of:

$$\Delta I_p = \frac{I_{\text{max,pk}} \cdot 2}{N_{\text{codes}}} = \frac{2 \times 70.71}{9{,}885{,}355} \approx 14.3 \text{ } \mu\text{A}$$

The effective resolution is limited by noise, not by the 24-bit code space. At 4 kSPS with $G = 1$, the input-referred RMS noise is 5.3 $\mu$V. The noise-limited effective resolution in current terms:

$$\Delta I_{p,\text{noise}} = \frac{e_n}{R_b} \cdot n = \frac{5.3 \times 10^{-6}}{10} \times 1000 = 0.530 \text{ mA (rms primary)}$$

The effective current dynamic range:

$$\text{DR}_I = \frac{I_{\text{max}}}{\Delta I_{p,\text{noise}} \cdot \sqrt{2}} = \frac{50}{0.530 \times 10^{-3} \times 1.414} \approx 66{,}700 : 1$$

This far exceeds the PZEM-004T's usable range (starting current of 20 mA at 100 A full-scale = 5000:1).

### 2.3 Using PGA to Improve Low-Current Sensitivity

For loads drawing very small currents (e.g., phone chargers at 50–200 mA), the burden voltage is tiny:

$$V_{b,\text{rms}} \big|_{200\text{ mA}} = \frac{0.2}{1000} \times 10 = 2.0 \text{ mV rms}$$

At $G = 1$, the SNR is $2.0 \text{ mV} / (5.3 \text{ } \mu\text{V} \times \sqrt{2}) = 267$, which is adequate for RMS measurement but marginal for waveform feature extraction. The ADS131M02's PGA can be programmed to higher gain for such loads. At $G = 16$:

$$V_{\text{FSR}} = \pm \frac{V_{\text{ref}}}{G} = \pm \frac{1.2}{16} = \pm 75 \text{ mV}$$

The effective noise drops proportionally (the PGA amplifies before the ADC's quantisation noise floor), and the input-referred noise at $G = 16$ is approximately 0.95 $\mu$V rms. A firmware gain-ranging algorithm can switch PGA settings based on the measured signal amplitude, maximising SNR across the full 20 mA – 50 A range.

### 2.4 Burden Resistor Specification

The burden resistor dissipates power at full-scale:

$$P_b = I_{s,\text{max}}^2 \cdot R_b = (50 \times 10^{-3})^2 \times 10 = 25 \text{ mW}$$

This is negligible. A standard 1/4 W through-hole metal film resistor is more than adequate. However, the resistor's temperature coefficient (tempco) directly affects metering accuracy. For 0.5% total accuracy budget, the burden resistor should contribute no more than 0.1% drift over the operating temperature range:

$$\text{Required tempco} \leq \frac{0.001}{(70 - 25)} = 22.2 \text{ ppm/}^\circ\text{C}$$

Select: **10 $\Omega$, 0.1% tolerance, 15 ppm/$^\circ$C metal film resistor** (e.g., Vishay MRS25 or Yageo MFR series).

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

Select: **330 $\Omega$, 0.1% tolerance, 15 ppm/$^\circ$C metal film resistor**.

### 3.8 Circuit Topology — Voltage Channel

```
                MAINS SIDE (isolated by transformer)     │    LOW-VOLTAGE SIDE
                                                         │
AC Line (L) ── R_La ── R_Lb ──┐                         │
                    (56k)(56k) │                         │
                               │  ┌──────────┐          │
                               ├──┤ ZMPT101B ├──── R_s ─┼──┬── AIN1P (ADS131M02)
                               │  │  (1:1)   │   (330Ω) │  │
AC Neutral (N) ───────────────┘  └──────────┘──────────┼──┤
                                                         │  │
                                     4kV isolation       │ C_v (22nF)
                                        barrier         │  │
                                                         │ GND (low-voltage)
```

The ZMPT101B provides 4 kV galvanic isolation. The primary winding is connected to the AC mains through the current-limiting resistor $R_L$. The secondary winding is connected to the sampling resistor $R_s$ on the low-voltage side. There is no galvanic connection between the mains and the measurement circuit.

---

## 4. Anti-Aliasing Filter Design

### 4.1 Nyquist Consideration

At a sample rate of $f_s = 4$ kSPS (the recommended operating point, see Section 5), the Nyquist frequency is:

$$f_{\text{Nyquist}} = \frac{f_s}{2} = 2 \text{ kHz}$$

The ADS131M02 uses a sinc3 digital decimation filter internally, which provides significant rejection of frequencies above $f_s / 2$. However, the sigma-delta modulator's analog input bandwidth extends far beyond $f_s$, so an external anti-aliasing filter is still needed to reject high-frequency noise (switching noise, EMI) that could alias into the signal band.

The signal bandwidth of interest extends to the 15th harmonic:

$$f_{\text{sig,max}} = 15 \times f_{\text{mains}} = 15 \times 60 = 900 \text{ Hz}$$

We need the filter to pass this bandwidth with minimal attenuation while suppressing content above $f_s / 2 = 2$ kHz. A first-order RC low-pass filter is sufficient because the sinc3 digital filter provides additional rolloff.

### 4.2 Filter Cutoff Frequency Selection

For a first-order RC filter, the $-3$ dB cutoff frequency is:

$$f_c = \frac{1}{2\pi R_f C_f}$$

We want:
- Minimal attenuation at 900 Hz: less than 0.5 dB ($\leq 5.6$% amplitude error)
- Significant attenuation at $f_s = 4$ kHz and above

The attenuation of a first-order filter at frequency $f$ is:

$$A(f) = \frac{1}{\sqrt{1 + (f / f_c)^2}}$$

At $f = 900$ Hz with $A \geq 0.944$ ($-0.5$ dB):

$$0.944 = \frac{1}{\sqrt{1 + (900 / f_c)^2}}$$

$$1 + (900 / f_c)^2 = \frac{1}{0.944^2} = 1.1225$$

$$(900 / f_c)^2 = 0.1225$$

$$f_c = \frac{900}{\sqrt{0.1225}} = \frac{900}{0.350} = 2571 \text{ Hz}$$

Choose $f_c \approx 2.5$ kHz as a round design target.

### 4.3 Current Channel Filter

The filter is placed in series with the ADC input, between the burden resistor and the ADC pins:

```
CT Burden (R_b) ── R_f ──┬── AIN0P
                          │
                         C_f
                          │
                  GND ───┴── AIN0N
```

Selecting $R_f = 1$ k$\Omega$ (total added source impedance):

$$C_f = \frac{1}{2\pi R_f f_c} = \frac{1}{2\pi \times 1000 \times 2500} = 63.7 \text{ nF}$$

Choose the nearest standard value: **$C_f = 68$ nF** (ceramic C0G/NP0 for low distortion).

Actual cutoff:

$$f_{c,I} = \frac{1}{2\pi \times 1000 \times 68 \times 10^{-9}} = 2341 \text{ Hz}$$

Attenuation at 900 Hz:

$$A_I(900) = \frac{1}{\sqrt{1 + (900 / 2341)^2}} = \frac{1}{\sqrt{1.1478}} = 0.934 = -0.59 \text{ dB}$$

This is a systematic 6.6% amplitude reduction at the 15th harmonic. Since we know the filter's exact transfer function, this can be corrected in firmware when computing harmonic magnitudes. At the fundamental (60 Hz):

$$A_I(60) = \frac{1}{\sqrt{1 + (60 / 2341)^2}} = \frac{1}{\sqrt{1.000657}} = 0.99967 = -0.003 \text{ dB}$$

Negligible — 0.033% amplitude error at the fundamental frequency.

### 4.4 Voltage Channel Filter

For the voltage channel, the anti-aliasing filter capacitor is placed across the sampling resistor $R_s$ on the secondary side of the ZMPT101B:

```
ZMPT101B secondary ── R_s (330Ω) ──┬── AIN1P
                                     │
                                    C_v
                                     │
                             GND ───┴── AIN1N
```

The filter cutoff formed by $R_s$ and $C_v$:

$$C_v = \frac{1}{2\pi R_s f_c} = \frac{1}{2\pi \times 330 \times 2500} = 193 \text{ nF}$$

Choose the nearest standard value: **$C_v = 180$ nF** (C0G/NP0 or X7R ceramic).

Actual cutoff:

$$f_{c,V} = \frac{1}{2\pi \times 330 \times 180 \times 10^{-9}} = 2679 \text{ Hz}$$

Attenuation at 900 Hz:

$$A_V(900) = \frac{1}{\sqrt{1 + (900 / 2679)^2}} = \frac{1}{\sqrt{1.1129}} = 0.949 = -0.45 \text{ dB}$$

At the fundamental (60 Hz):

$$A_V(60) = \frac{1}{\sqrt{1 + (60 / 2679)^2}} = 0.99975 = -0.002 \text{ dB}$$

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

### 5.3 HWCT-004 Current Transformer Phase Error

The HWCT-004's phase error is dramatically lower than that of split-core CTs like the SCT-013-000. Independent testing by the OpenEnergyMonitor project found phase shifts of less than 0.2 degrees above 10 A primary current, and less than 0.5 degrees at lower currents. This is attributed to the solid toroidal core (no air gap from a split-core hinge), and the low turns ratio (1:1000).

| Primary Current | Approx. CT Phase Error |
|----------------|----------------------|
| 50 A (full scale) | $< 0.2^\circ$ |
| 20 A | $< 0.2^\circ$ |
| 10 A | $\approx 0.3^\circ$ |
| 5 A | $\approx 0.4^\circ$ |
| 1 A | $< 0.5^\circ$ |

The variation across the operating range (0.2° to 0.5°) is much smaller than the SCT-013-000's variation (1° to 4°), and critically, the absolute magnitude is small enough that a single-point calibration is sufficient.

### 5.4 Anti-Aliasing Filter Differential Phase Error

A first-order RC filter introduces a phase lag of:

$$\phi(f) = -\arctan\left(\frac{f}{f_c}\right)$$

At 60 Hz:

Voltage channel ($f_{c,V} = 2679$ Hz):

$$\phi_{V,\text{filter}} = -\arctan\left(\frac{60}{2679}\right) = -\arctan(0.0224) = -1.28^\circ$$

Current channel ($f_{c,I} = 2341$ Hz):

$$\phi_{I,\text{filter}} = -\arctan\left(\frac{60}{2341}\right) = -\arctan(0.0256) = -1.47^\circ$$

Differential filter phase error:

$$\Delta\phi_{\text{filter}} = \phi_{V,\text{filter}} - \phi_{I,\text{filter}} = -1.28^\circ - (-1.47^\circ) = +0.19^\circ$$

### 5.5 ADC Sampling Phase Error

The ADS131M02 samples both channels simultaneously — the two sigma-delta modulators are clocked by the same edge. Therefore:

$$\Delta\phi_{\text{ADC}} = 0^\circ$$

This is the fundamental advantage of the ADS131M02 over multiplexed ADC architectures.

### 5.6 Total Phase Error Budget

| Source | Voltage Channel | Current Channel | Differential ($V - I$) | Type |
|--------|----------------|-----------------|----------------------|------|
| ZMPT101B transformer | $-0.33^\circ$ (lag) | — | $-0.33^\circ$ | Constant |
| HWCT-004 CT | — | $-0.2^\circ$ (lag, above 10 A) | $+0.2^\circ$ | Nearly constant |
| Anti-aliasing filter | $-1.28^\circ$ | $-1.47^\circ$ | $+0.19^\circ$ | Constant |
| ADC simultaneous sampling | $0^\circ$ | $0^\circ$ | $0^\circ$ | — |
| **Total (uncalibrated, above 10 A)** | — | — | **$+0.06^\circ$** | **Nearly constant** |

### 5.7 Phase Error Correction Strategy

The total uncalibrated phase error of $+0.06^\circ$ above 10 A is nearly constant and extremely small. Unlike the SCT-013-000 design (which would require a multi-point current-dependent correction LUT), the HWCT-004's low and stable phase error allows correction with a single value in the ADS131M02's hardware PHASE register.

The PHASE register applies a sub-sample time shift in increments of one modulator clock period:

$$\Delta t_{\text{step}} = \frac{1}{f_{\text{CLKIN}}} = \frac{1}{8.192 \times 10^6} = 122.07 \text{ ns}$$

The corresponding phase resolution at 60 Hz:

$$\Delta\phi_{\text{step}} = 360^\circ \times f_{\text{mains}} \times \Delta t_{\text{step}} = 360 \times 60 \times 122.07 \times 10^{-9} = 0.00264^\circ$$

To correct $+0.06^\circ$:

$$\text{PHASE register value} = \text{round}\left(\frac{0.06}{0.00264}\right) = 23 \text{ (integer steps)}$$

This corrects the constant phase offset to within $\pm 0.0013^\circ$. One register write during firmware initialisation — no runtime correction needed.

### 5.8 Post-Calibration Residual Phase Error

After the single-point correction, the residual phase error is bounded by the HWCT-004's phase variation across the current range. At currents below 10 A, the CT phase error increases from $0.2^\circ$ to approximately $0.5^\circ$, a deviation of $0.3^\circ$ from the calibration point. This is the worst-case residual.

The resulting power measurement error at $\text{PF} = 0.8$ ($\theta = 36.87^\circ$) with a residual phase error of $\Delta\phi = 0.3^\circ$:

$$\epsilon_P = \frac{\cos(36.87^\circ + 0.3^\circ) - \cos(36.87^\circ)}{\cos(36.87^\circ)} = \frac{0.7976 - 0.8}{0.8} = -0.30\%$$

At $\text{PF} = 0.5$ ($\theta = 60^\circ$) with $\Delta\phi = 0.3^\circ$:

$$\epsilon_P = \frac{\cos(60.3^\circ) - \cos(60^\circ)}{\cos(60^\circ)} = \frac{0.4974 - 0.5000}{0.5000} = -0.52\%$$

The worst case ($-0.52$% at PF = 0.5 and low current) is at the boundary of the 0.5% accuracy target. For loads below 10 A with PF $< 0.6$, the accuracy is approximately 0.5%. For all other operating conditions, it is well within budget. If tighter accuracy is needed at low currents, an optional two-point correction (one calibration at 5 A, one at 25 A) would reduce the worst-case residual to approximately $\pm 0.15^\circ$ and the power error to $\pm 0.26$%.

### 5.9 Phase Behaviour at Harmonics

Both the ZMPT101B and SCT-013-000 have frequency-dependent phase characteristics. At higher harmonics, the phase shifts differ from the fundamental. However, for load characterisation, the primary features used are harmonic magnitudes (H3/H1, H5/H1, THD, crest factor), not harmonic phase angles. The magnitude response of both transformers is flat within $\pm 1$% from 50 Hz to 1 kHz (the SCT-013-000 is specified to 1 kHz; the ZMPT101B's small ferrite core provides even better high-frequency response).

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

- **Current channel:** The HWCT-004 CT provides magnetic coupling with approximately 1 kV dielectric strength between the primary conductor and the secondary winding. While lower than the SCT-013-000's 6 kV rating, 1 kV is adequate for 220 V residential mains (the peak voltage is 311 V, giving a 3.2x margin).
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
    │                  │ ┌──────┐ ║           ║
    │                  ├─┤ZMPT  ├─╫──── R_s ──╫──┬── AIN1P ──┐
    │                  │ │101B  │ ║  (330Ω)   ║  │           │
    └── AC Neutral ────┘ │(1:1) │ ║           ║  │  C_v      │
                         └──────┘ ║  4kV      ║  │ (180nF)   │
                                  ║ isolation ║  │           │
                                  ║           ║  └── AIN1N   │
                                  ║           ║              │
    ┌─── Load wire ───────────┐   ║           ║              │
    │  (passes through CT)    │   ║           ║              │
    │                         │   ║           ║              │
    │         ┌──────┐        │   ║           ║              │
    │         │HWCT  │        │   ║           ║              │
    │         │ -004 ├────────╫───╫── R_b ────╫──┬── AIN0P   │
    │         │1:1000│        ║   ║  (10Ω)    ║  │           │
    │         │      ├────────╫───╫── R_f ──┬─╫──┤           │
    │         └──────┘        ║   ║  (1kΩ)  │ ║  │  C_f      │ ADS131M02
    │                         ║   ║         │ ║  │ (68nF)    │ (3.3V, shared
    │           1kV isolation ║   ║     GND─┘ ║  │           │  GND w/ ESP32)
    │                         ║   ║           ║  └── AIN0N   │
    │                         ║   ║           ║              │
    │                         ║   ║           ║     SCLK ────┤── GPIO 18
    │                         ║   ║           ║     DIN  ────┤── GPIO 23
    │                         ║   ║           ║     DOUT ────┤── GPIO 19
    │                         ║   ║           ║     CS   ────┤── GPIO 5
    │                         ║   ║           ║     DRDY ────┤── GPIO 4
    │                         ║   ║           ║              │
    │                         ║   ║           ║     AVDD ────┤── 3.3V
    │                         ║   ║           ║     DVDD ────┤── 3.3V
    │                         ║   ║           ║     AGND ────┤── GND
    │                         ║   ║           ║     DGND ────┤── GND
    │                         ║   ║           ║              │
    └─────────────────────────┘   ║           ║     CLKIN ───┤── 8.192 MHz
                                  ║           ║              │
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

The noise-limited current resolution at 4 kSPS:

$$\Delta I_{p,\text{noise}} = \frac{5.3 \times 10^{-6}}{10} \times 1000 = 0.530 \text{ mA rms (primary)}$$

The noise-limited voltage resolution at 4 kSPS:

$$\Delta V_{\text{mains}} = \frac{5.3 \times 10^{-6}}{2.946 \times 10^{-3}} = 1.80 \text{ mV rms (primary)}$$

Both are well below the PZEM-004T's resolution (1 mA current, 100 mV voltage). The current resolution is nearly 2x better due to the HWCT-004's lower turns ratio (1:1000 vs. 1:1800) producing a larger secondary current per ampere of primary current.

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
| CT1 | HWCT-004 current transformer | 50 A : 50 mA | 1 kV isolation, PCB-mount toroidal | 1 |
| R_La | Current-limiting resistor (ZMPT101B) | 56 k$\Omega$ | 1 W, 1%, metal film, 200V rated | 1 |
| R_Lb | Current-limiting resistor (ZMPT101B) | 56 k$\Omega$ | 1 W, 1%, metal film, 200V rated | 1 |
| R_s | Sampling resistor (ZMPT101B) | 330 $\Omega$ | 0.1%, 15 ppm/$^\circ$C, metal film | 1 |
| R_b | CT burden resistor | 10 $\Omega$ | 0.1%, 15 ppm/$^\circ$C, metal film | 1 |
| R_f | Current channel filter resistor | 1 k$\Omega$ | 1%, metal film | 1 |
| C_f | Current channel anti-alias cap | 68 nF | C0G/NP0 ceramic, 50 V | 1 |
| C_v | Voltage channel anti-alias cap | 180 nF | C0G/NP0 or X7R ceramic, 50 V | 1 |
| C1 | AVDD decoupling | 1 $\mu$F | C0G or X7R ceramic, 10 V | 1 |
| C2 | DVDD decoupling | 1 $\mu$F | X7R ceramic, 10 V | 1 |
| C3 | CAP pin (internal LDO) | 220 nF | X7R ceramic, 10 V | 1 |
| Y1 | Crystal oscillator | 8.192 MHz | HC-49S or CMOS oscillator, 20 ppm | 1 |
| CY1, CY2 | Crystal load caps (if using crystal) | 12 pF | C0G ceramic | 2 |
| J1 | TSSOP-20 to DIP adapter board | — | 0.65 mm pitch breakout | 1 |

---

## 9. Performance Summary and Comparison

| Parameter | PZEM-004T v3 | ADS131M02 + ZMPT101B + HWCT-004 | Improvement |
|-----------|-------------|--------------------------------------|-------------|
| Voltage resolution | 0.1 V | 1.80 mV | ~56x |
| Current resolution | 1 mA | 0.53 mA | ~1.9x |
| Voltage accuracy (after cal.) | 0.5% | $< 0.2$% | ~2.5x |
| Current accuracy (after cal.) | 0.5% | $< 0.2$% | ~2.5x |
| Power accuracy at PF=0.8 (after cal.) | 0.5% | $< 0.3$% | ~1.7x |
| Power factor resolution | 0.01 | $< 0.001$ (computed) | ~10x |
| Simultaneous V/I sampling | No (internal mux) | Yes | Eliminates phase skew |
| Waveform access | None | Full raw samples | Enables NILM |
| Harmonic analysis | Not possible | Up to 33rd harmonic | Enables classification |
| Sample rate | Internal (no access) | 4 kSPS configurable | Full control |
| Energy accumulation | Hardware counter | Firmware integration | More flexible |
| Interface to MCU | UART Modbus (9600 baud) | SPI (up to 25 MHz) | ~2600x bandwidth |
| Galvanic isolation (V channel) | Optocoupler (comms only) | ZMPT101B, 4 kV | Full signal isolation |
| Galvanic isolation (I channel) | CT (built-in) | CT (HWCT-004), 1 kV | Adequate for 220 V |
| Galvanic isolation (digital bus) | Optocoupler (built-in) | Not needed (both sensors isolated) | Simpler |
| Phase error (total, uncalibrated) | Unknown (internal) | $+0.06^\circ$ (above 10 A) | Nearly zero |
| Phase calibration complexity | N/A | Single register write | Trivial |
| Power consumption (meter) | ~1 W (from AC mains) | ~13 mW + 0.45 W ($R_L$) | Comparable |
| Current range | 0 – 100 A | 0 – 50 A | Adequate for residential |
| CT installation | Split-core (clip-on) | Toroidal (thread wire) | Requires initial wiring |
| Component count | 1 module | ~16 discrete parts | More, but cheaper |

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

With the HWCT-004's low and stable phase error, only a single-point calibration is required:

1. Apply a purely resistive load of known power (e.g., 100 W incandescent bulb or high-power resistor). The true power factor is 1.00.
2. Capture several hundred cycles of simultaneous V and I waveforms.
3. Compute the phase angle between V and I fundamentals using cross-correlation or FFT phase extraction.
4. The measured phase angle is the system's total phase error.
5. Write the correction into the ADS131M02's PHASE register:

$$\text{PHASE\_REG} = \text{round}\left(\frac{\phi_{\text{error}} \times f_{\text{CLKIN}}}{360^\circ \times f_{\text{mains}}}\right) = \text{round}\left(\frac{\phi_{\text{error}} \times 8{,}192{,}000}{360 \times 60}\right)$$

For a measured phase error of $0.06^\circ$:

$$\text{PHASE\_REG} = \text{round}\left(\frac{0.06 \times 8{,}192{,}000}{21{,}600}\right) = \text{round}(22.8) = 23$$

This single register write during initialisation is the only phase correction needed. No current-dependent lookup table or multi-point calibration is required, thanks to the HWCT-004's nearly constant phase response.

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

### 11.1 FreeRTOS Task Architecture

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

### 11.2 Memory Requirements

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
