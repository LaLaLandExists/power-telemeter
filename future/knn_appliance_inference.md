# K-NN Appliance Inference Integration — Brainstorming Summary

## On-Gateway Appliance-Level Identification for the AGILASX Power Telemetry System

---

## 1. Problem Statement

The existing load classification system on the ESP32-S3 telemetry nodes identifies loads into six general electrical categories (RESISTIVE, CAPACITIVE, MOTOR_1PH, FAN, SMPS, LIGHTING) using a hand-crafted decision tree operating on a 10-feature harmonic analysis vector. While effective for class-level separation, this approach cannot distinguish specific appliances within a class — a laptop charger, phone charger, and television are all reported as "SMPS."

The goal is to add appliance-level identification (e.g., "Refrigerator," "Television," "Rice Cooker") on top of the existing 6-class system, using K-Nearest Neighbors (K-NN) as the inference algorithm.

---

## 2. Why K-NN

K-NN was evaluated against extending the existing MLP neural network upgrade path for several reasons:

- **No retraining on new appliances.** Adding a new appliance to the MLP requires offline retraining in TensorFlow/Keras, quantization, and OTA deployment. K-NN requires only adding reference vectors to the database — no code change, no recompilation, no OTA update.
- **No fixed output layer.** The MLP's softmax output has a fixed number of neurons set at compile time. Going from 6 to 30 outputs changes the architecture. K-NN returns whatever label the nearest neighbors carry. The labels are data, not structure.
- **Small-N regime.** A household has 20–40 distinct appliances. K-NN thrives in low-N, moderate-dimensionality problems. The curse of dimensionality is not a concern with 6–14 features and a few dozen reference points.
- **Transparent confidence.** K-NN confidence derives directly from distance — physically interpretable as "how similar is this measurement to the enrolled reference." No black-box probability calibration required.

---

## 3. Feature Set: AGILASx Base

### 3.1 The AGILASx Dataset

AGILASx is a dataset of 50 common Philippine home appliances created by Villanueva, Dumlao, and Reyes at Ateneo de Manila University (IEEE CIoT 2016, DOI: 10.1109/CIoT.2016.7872910). It contains 100 appliance signatures in XML format, each comprising six electrical characteristics:

| Feature | Symbol | Unit |
|---------|--------|------|
| Real power | W | Watts |
| Apparent power | VA | Volt-Amperes |
| Reactive power | VAR | Volt-Amperes Reactive |
| RMS current | A | Amperes |
| RMS voltage | V | Volts |
| Power factor | PF | Dimensionless |

The study reported 99% recognition accuracy using K-NN at 10–1 Hz acquisition frequency for both intersession and unseen-instance test protocols.

### 3.2 Critical Advantage: Voltage Regime Match

AGILASx was recorded at 220V/60Hz in the Philippines — the same grid as the target deployment. This eliminates the voltage regime incompatibility that renders US-based datasets (e.g., PLAID at 120V/60Hz) unusable for appliance-level K-NN.

The voltage regime problem is fundamental, not merely a calibration issue. For any load with a rectifier input (SMPS, LED drivers, CFL), the conduction angle of the bridge rectifier changes nonlinearly with peak voltage. At 220V the current pulses are narrower and peakier than at 120V, producing measurably different THD, crest factor, and harmonic ratios. The transformation depends on each appliance's input capacitance, making a universal 120V-to-220V correction impossible.

### 3.3 Existing Telemetry Already Contains All AGILASx Features

The current 32-byte `TelemetryPacket` transmitted by every node already includes:

```
voltage      (uint16, ÷10 → V)       → AGILASx "V"
current      (uint32, ÷1000 → A)     → AGILASx "A"
power        (uint32, ÷10 → W)       → AGILASx "W"
powerFactor  (uint16, ÷100 → PF)     → AGILASx "PF"
```

The gateway can derive the remaining two features with trivial arithmetic:

```
S = V × I                             → AGILASx "VA"
Q = sqrt(S² - P²)                     → AGILASx "VAR"
```

**No node firmware changes are required. No protocol changes. No packet extension. The K-NN engine is a gateway-only software addition operating on data the gateway already receives.**

### 3.4 Extended Feature Set (Future Enhancement)

For improved intra-class discrimination, the AGILASx base can be extended with the harmonic features already computed on-node via the `ClassifierFeatures` struct:

```
AGILASx base (6 features):   W, VA, VAR, A, V, PF
Harmonic extension (8):      THD_I, H3/H1, H5/H1, H7/H1, CF, OE, H5/H3, Prms
```

This extension requires transmitting the quantized harmonic features (10–11 additional bytes per telemetry packet), which increases airtime from ~34ms to ~49ms but remains within the 100ms UL window. This is a future enhancement; the 6-feature AGILASx base is sufficient for initial deployment.

---

## 4. Architecture

### 4.1 Two-Tier Classification

```
Tier 1 — On-node (decision tree, existing):
  Input:  128-point FFT → 10-feature ClassifierFeatures
  Output: 1 of 6 general classes (RESISTIVE, SMPS, etc.)
  Runs:   Every 2 seconds on Core 0

Tier 2 — On-gateway (K-NN, new):
  Input:  {W, VA, VAR, A, V, PF} derived from TelemetryPacket
  Output: Specific appliance label (e.g., "Refrigerator")
  Runs:   On Core 0 after each telemetry reception
```

### 4.2 Why Gateway-Side

- The gateway already has V, I, W, PF from the telemetry packet — no protocol changes needed.
- The web dashboard (where users see results) runs on the gateway.
- The reference database is stored centrally in FRAM, not duplicated across nodes.
- More SRAM available for the K-NN search (~2.5 KB for the training manager, ~8 KB for profiles).
- The nodes remain completely unaware of the K-NN mechanism; they share no contract with it.

### 4.3 Load Change Detection

Since outlets are not restricted to a single permanent appliance, the gateway detects load transitions by monitoring power discontinuities:

```
If |W_current - W_previous| > THRESHOLD (e.g., 10W):
  Reset settle counter
  Display "Identifying..."
After SETTLE_COUNT consecutive stable readings (e.g., 3 × 3s = 9s):
  Run K-NN on the settled feature vector
  Display identified appliance label
```

This works because the system architecture guarantees one CT per outlet — one analog frontend maps to exactly one load. The K-NN inference is unambiguous by hardware design, not by assumption.

---

## 5. Appliance Labeling: Type, Not Brand

### 5.1 Rationale

Appliance labels should identify the **appliance type** (e.g., "Washing Machine") rather than the brand (e.g., "Panasonic Washing Machine").

**Technical reason:** Electrical signatures cluster by power electronics topology, not by manufacturer. A Panasonic and a Carrier non-inverter window aircon of similar tonnage use the same single-phase induction motor type and produce nearly identical harmonic signatures. K-NN cannot reliably separate them. Conversely, a Panasonic non-inverter and a Panasonic inverter aircon have radically different signatures despite sharing a brand.

**Practical reason:** If the reference database contains "Panasonic Washing Machine" and the end user has a Samsung washing machine, K-NN will match it (the signatures are close) but display the wrong brand. This erodes user trust. Labeling by type makes the match correct regardless of brand.

### 5.2 Recommended Taxonomy

| General Class | Appliance Type Labels |
|---|---|
| RESISTIVE | Flat Iron, Rice Cooker, Electric Kettle, Water Heater, Incandescent Bulb |
| MOTOR_1PH | Refrigerator, Non-Inverter Aircon, Washing Machine (conventional), Water Pump |
| FAN | Ceiling Fan, Desk Fan, Exhaust Fan, Stand Fan |
| SMPS | Phone Charger, Laptop Charger, Television, Wi-Fi Router, Desktop Computer |
| LIGHTING | CFL, LED Lamp, Fluorescent (magnetic ballast) |
| CAPACITIVE | Inverter Aircon, Inverter Refrigerator, Laptop Charger (PFC) |

Technology variant is encoded where it matters electrically (e.g., "Inverter Aircon" vs. "Non-Inverter Aircon") because these produce fundamentally different signatures.

### 5.3 Label Storage

Labels are stored as interned strings in FRAM rather than compiled enums, preserving K-NN's flexibility to accept arbitrary appliance types without firmware changes:

```
FRAM label table: up to 48 unique labels × 24 chars = 1,153 bytes
Per-profile storage: 1-byte labelIdx instead of 24-byte string
Consistency: multiple profiles sharing one type always display the same string
```

New labels are added to the table at enrollment time. No recompilation, no OTA.

---

## 6. Time-Varying Appliance States

### 6.1 The Problem

Many appliances cycle through distinct electrical states. A refrigerator in compressor-off standby (3–5W, SMPS-like) looks nothing like the same refrigerator in compressor-on mode (125–145W, induction motor). A single centroid would represent no actual operating state.

| Appliance | States | Typical Power Levels |
|---|---|---|
| Refrigerator | Compressor on, standby, defrost | 130W / 4W / 300W |
| Non-inverter aircon | Compressor on, compressor off | 800W / 5W |
| Washing machine | Wash, rinse, spin, idle | 250W / 200W / 400W / 3W |
| Rice cooker | Cook, keep-warm | 650W / 50W |
| Flat iron | Heating, thermostat off | 1000W / 0W |

### 6.2 Multi-State Profiles

Each appliance profile stores up to 4 operating-state centroids, all sharing the same label:

```
ApplianceProfile {
    labelIdx:  "Refrigerator"
    nStates:   2
    states[0]: centroid={W=3.8, ...}   // standby
    states[1]: centroid={W=133, ...}   // compressor running
}
```

K-NN distance is computed as the minimum distance across all states:

```
d(query, appliance) = min over all states s:
                        distance(query, appliance.states[s].centroid)
```

The system returns "Refrigerator" regardless of which operating state matched.

### 6.3 Automatic State Detection

States are detected automatically during enrollment by gap-based clustering on wattage. Samples are sorted by W; gaps larger than a threshold (e.g., 15W) define state boundaries. Micro-clusters with fewer than 2% of total samples are discarded as transients.

### 6.4 Runtime State Tracking and Appliance Swap Detection

At runtime, the K-NN must distinguish two types of power transitions that look similar in the telemetry stream: an appliance cycling between its own operating states (refrigerator compressor turning off) and a user physically swapping one appliance for another at the same outlet.

**Purely instantaneous K-NN fails** because electrically similar states of different appliances produce ambiguous matches — a refrigerator in standby (4W, PF=0.45) and a phone charger (5W, PF=0.52) are close in feature space, causing label flickering.

**Purely history-locked K-NN fails** because if the system commits to an identity and the user swaps the appliance, the system never reclassifies and displays the wrong label indefinitely.

The distinguishing signal is the **zero-power gap.** When a user physically unplugs an appliance and plugs in another, at least one telemetry sample shows ~0W. An internal state transition (compressor cycling off) does not produce a zero-power reading — standby electronics continue drawing 3–5W.

The runtime logic uses a three-layer state machine:

```
Layer 1 — Load change detector:
  Watches |ΔW| between consecutive samples.
  Significant change triggers re-evaluation.

Layer 2 — Swap-vs-state discriminator:
  IF power crossed through zero (W < 1W for ≥2 consecutive samples):
    → SWAP: clear identity, wait for settle, run fresh K-NN
  ELSE:
    → STATE TRANSITION: check if new reading matches any other
      operating state of the currently identified appliance.
      If yes: update state index, keep identity.
      If no:  assume hot-swap, clear identity and re-identify.

Layer 3 — Temporal voter:
  Between load change events, K-NN runs on each sample.
  Majority vote of last 5 results suppresses single-sample noise.
```

The state machine has three states:

```
                    power > 0
        ┌──────────────────────────┐
        │                          │
        ▼                          │
  ┌──────────┐   W drops to 0   ┌─┴──────────┐
  │IDENTIFIED├──────────────────►│ UNIDENTIFIED│◄── initial state
  │(locked)  │   for ≥2 samples │             │
  └──┬───┬───┘                  └──────┬──────┘
     │   │                             │
     │   │ big ΔW,                     │ 3 stable
     │   │ no zero                     │ samples
     │   │                             │
     │   ▼                             ▼
     │ ┌─────────────────┐      ┌────────────┐
     │ │Check: does new W │ no   │Fresh K-NN  │
     │ │match another     ├─────►│(no bias)   │
     │ │state of current? │      └─────┬──────┘
     │ └────────┬────────┘             │
     │     yes  │                      │
     │          ▼                      ▼
     │   Update stateIdx         Lock identity
     │   Keep identity           Fill vote buffer
     │                                 │
     └────────────────────────────────►│
       Temporal voting ◄───────────────┘
       (majority of last 5 K-NN results)
```

**Edge case — hot-swap without zero gap:** If a user swaps plugs within a single 3-second telemetry window, no zero-power sample is captured. The state-check fails (new features don't match any state of the old appliance), triggering re-identification with a 9-second delay (3 settle samples × 3 seconds).

**Unresolvable ambiguity:** Replacing a refrigerator in standby (4W) with a phone charger (5W) without a visible zero gap. The phone charger falls within the refrigerator's standby state enrollment radius. No algorithm can resolve this without the zero-power gap. In practice this scenario is rare — dedicated outlets (refrigerator, aircon) rarely see appliance swaps.

---

## 7. Training Mode

### 7.1 Design Principle

Training mode is a gateway-only concept. The telemetry nodes are completely unaware — they transmit the same 32-byte `TelemetryPacket` as in normal operation. The gateway simply reinterprets the telemetry it already receives, accumulating statistics when training is active for a given node.

### 7.2 Online State Accumulators

To avoid buffering thousands of raw samples (which would consume ~460 KB for 8 concurrent nodes), the gateway uses streaming state accumulators. Each incoming sample is either merged into an existing state centroid or, after a debounce period, promotes a new state. No raw data is stored.

Memory per node: ~317 bytes. Eight concurrent training sessions: ~2,536 bytes (0.5% of ESP32-S3 SRAM).

### 7.3 Training Workflow

```
For the developer building the reference database:

1.  Plug up to 8 appliances into 8 nodes simultaneously
2.  Open dashboard → System → Training Mode
3.  For each node, type the appliance label and set the training duration:
      Node 1: "Refrigerator"     duration: 2h
      Node 2: "Window Aircon"    duration: 2h
      Node 3: "Television"       duration: 30min
      Node 4: "Ceiling Fan"      duration: 10min
      Node 5: "Rice Cooker"      duration: 45min
      ...
4.  Walk away. The gateway collects and clusters automatically.
5.  All nodes finish → profiles stored in FRAM → next batch.
```

Seven batches of 8 appliances covers 50+ appliance types. Total wall-clock time: ~14 hours (dominated by refrigerator and aircon cycle periods), mostly unattended.

### 7.4 Training Duration by Appliance Type

| Appliance Type | Minimum Duration | Reason |
|---|---|---|
| Steady-state loads (chargers, lamps) | 2–5 minutes | Single state, settles immediately |
| Thermostat-cycled (flat iron) | 5–15 minutes | Thermostat cycle: 30–90s |
| Cook-then-warm (rice cooker) | 30–45 minutes | Full cook cycle required |
| Compressor-cycled (fridge, aircon) | 2 hours | Compressor cycle: 15–45 min |
| Full mechanical cycle (washing machine) | 1 full cycle (~45 min) | Multiple modes: wash/rinse/spin |
| Inverter aircon | 2 hours | Continuous modulation, no discrete states |

### 7.5 Training Redundancy

Training multiple instances of the same appliance type improves K-NN coverage. A single enrolled ceiling fan produces one tight cluster in feature space representing that specific motor. A second ceiling fan of different wattage or manufacturer shifts the centroid, expanding the enrollment radius to encompass both. A third fan the developer never tested likely falls between the two enrolled clusters and matches correctly.

The recommended strategy is 2–3 instances per appliance type, spanning the expected wattage and brand range:

| Type | Instance 1 | Instance 2 | Instance 3 |
|---|---|---|---|
| Ceiling Fan | 50W cheap unit | 75W mid-range | — |
| Rice Cooker | 350W (small, 1L) | 700W (large, 1.8L) | — |
| Laptop Charger | 30W (ultrabook) | 65W (standard) | 100W (gaming) |
| LED Lamp | 5W bulb | 12W bulb | 18W downlight |
| Refrigerator | 6 cu.ft. (small) | 10 cu.ft. (medium) | — |

All instances of the same type share the same label. Diminishing returns set in around 3 instances — beyond that, new reference points land in already-covered regions of feature space.

### 7.6 Label Coherence: The Wattage Spread Rule

Different wattage ratings of the same abstract category must **not** share a label if their wattage spreads are large enough to overlap with other appliance types.

Wattage is the strongest discriminating feature in the AGILASx feature set. Merging wildly different wattages under one label destroys the feature that does most of the classification work.

**Failure example:** Training a 5W phone charger, a 25W tablet charger, and a 65W laptop charger all as "Charger" creates three states scattered across a huge region of feature space. The 5W state overlaps with LED lamps (7W). The 25W state overlaps with Wi-Fi routers (12W). The 65W state borders televisions (45W). K-NN misclassifies because the "Charger" profile's states pass through other appliance types' territories.

**Rule of thumb:** If the wattage spread within a label exceeds 2× the wattage of the lowest instance, the label is too broad. A 55W and 72W fan (ratio 1.3×) is fine. A 5W and 65W charger (ratio 13×) is not.

Labels should group appliances that are **electrically similar** — occupying a compact, bounded region in feature space. This aligns naturally with functional appliance types:

| Too broad (breaks K-NN) | Correct (electrically coherent) |
|---|---|
| "Charger" (5W–100W) | "Phone Charger" (3–10W) |
| | "Tablet Charger" (15–30W) |
| | "Laptop Charger" (45–100W) |
| "Fan" (15W–75W) | "Desk Fan" (15–30W) |
| | "Stand Fan" (35–50W) |
| | "Ceiling Fan" (50–80W) |
| "Light" (3W–60W) | "LED Lamp" (3–12W) |
| | "LED Downlight" (15–25W) |
| | "Fluorescent" (30–60W) |

These categories emerge from electrical reality, not arbitrary naming. A phone charger cannot be 65W because no phone battery accepts that. A ceiling fan cannot be 5W because that cannot turn blades. The wattage bands are a consequence of what these appliances physically do.

---

## 8. Model Export and Import

### 8.1 Format

The K-NN "model" is pure data — centroids, variances, and labels. JSON is used for human readability, editability, and compatibility with the existing REST API:

```json
{
  "version": 1,
  "gridVoltage": 220,
  "gridFrequency": 60,
  "normalization": {
    "mu":    [85.2, 112.4, 68.3, 0.42, 219.8, 0.71],
    "sigma": [95.1, 120.8, 82.5, 0.44,   3.2, 0.22]
  },
  "appliances": [
    {
      "label": "Refrigerator",
      "states": [
        {
          "centroid":  [3.8, 8.4, 7.5, 0.038, 220.1, 0.45],
          "variance":  [0.5, 1.2, 1.1, 0.003,   2.1, 0.03],
          "radiusSq":  42.5,
          "samples":   1600
        },
        {
          "centroid":  [133.0, 177.3, 117.0, 0.81, 219.4, 0.75],
          "variance":  [12.3,  18.5,  14.2, 0.08,   2.3, 0.02],
          "radiusSq":  285.0,
          "samples":   800
        }
      ]
    }
  ]
}
```

Total model size: ~5 KB for 48 appliance types.

### 8.2 REST Endpoints

```
GET  /api/knn/export    → returns the JSON model
POST /api/knn/import    → loads a JSON model into SRAM and FRAM
```

### 8.3 Deployment Pipeline

| Method | Workflow | Use Case |
|---|---|---|
| Pre-flash (LittleFS) | Model baked into firmware image via PlatformIO data folder | Manufacturing |
| Post-flash upload | `POST /api/knn/import` via curl or dashboard after power-on | Field deployment |
| OTA bundle | Model included in OTA update payload | Remote updates |

### 8.4 Version Compatibility

The JSON `version` field supports future feature set extensions:

- **Version 1:** 6-feature AGILASx base (W, VA, VAR, A, V, PF)
- **Version 2 (future):** 14-feature extended set (AGILASx base + harmonics)

Old gateways reject V2 models gracefully. New gateways accept both.

---

## 9. FRAM Storage

### 9.1 Current Capacity Constraint

The MB85RC256V (32 KB) is currently used with 15,760 bytes occupied by the existing node data layout (8 × 1,968 bytes per node block + 16-byte header). The K-NN database requires an additional ~5–8 KB.

### 9.2 Upgrade Path

The Fujitsu MB85RC series includes drop-in upgrades:

| Part | Capacity | Addressing | Drop-in? |
|---|---|---|---|
| MB85RC256V | 32 KB | 16-bit | Current chip |
| MB85RC512T | 64 KB | 16-bit | Direct swap, same pinout and protocol |
| MB85RC1MT | 128 KB | 17-bit | Requires library support for 17th address bit |

The **MB85RC512T** (64 KB) is recommended. It doubles the available space, allowing expanded history buffers (120 → 256 points), the K-NN database, and future features — with a single component swap and one firmware constant change.

### 9.3 Revised FRAM Layout (with MB85RC512T)

```
0x0000 - 0x000F   Header (16 bytes)
0x0010 - 0x3D8F   Node blocks [0..7] (15,744 bytes)
0x3D90 - 0x3D9F   K-NN header (16 bytes)
0x3DA0 - 0x3DEF   Normalization stats (80 bytes)
0x3DF0 - 0x5C6F   Appliance profiles [0..47] (~8 KB)
0x5C70 - 0xFFFF   Free (~41 KB for history expansion or future use)
```

---

## 10. Compute and Memory Budget

| Component | Gateway SRAM | FRAM | Compute per Inference |
|---|---|---|---|
| K-NN engine (profiles in memory) | ~8 KB | ~8 KB | — |
| Normalization stats | 48 B | 48 B | — |
| K-NN inference (48 appliances × 4 states) | Stack: ~200 B | — | ~4 us |
| Training manager (8 concurrent nodes) | 2,536 B | — | — |
| Training finalization (per node) | Stack: ~100 B | — | ~50 us (one-shot) |
| Feature derivation from telemetry | — | — | ~0.2 us (trivial arithmetic) |

Total gateway SRAM impact: ~11 KB (2.1% of 512 KB). The ESP32-S3 handles this comfortably with no need for a more powerful MCU.

---

## 11. Practical Discrimination Limits

### 11.1 What K-NN Can Reliably Separate

- Appliances differing in wattage by more than ~30% (phone charger vs. laptop charger vs. TV PSU)
- Appliances in different general classes (ceiling fan vs. refrigerator)
- Appliances using different power electronics topologies (CFL vs. LED lamp)
- Operating states of the same appliance (compressor on vs. standby)

### 11.2 What K-NN Will Struggle With

- Two appliances of similar wattage and identical topology from different manufacturers (e.g., two 45W laptop chargers using reference designs from the same controller IC vendor)
- Very low-power loads (<50 mA) where measurement noise degrades feature accuracy
- Gradually modulating loads (inverter aircon) where the operating point sweeps continuously rather than clustering into discrete states

### 11.3 Graceful Degradation

When K-NN cannot confidently identify an appliance (nearest distance exceeds the rejection threshold), the system falls back to the Tier 1 decision tree label. The user sees "SMPS" rather than nothing. This ensures the system always provides useful information, even for unrecognized appliances.

---

## 12. Dataset Strategy

### 12.1 Per-Deployment Data Collection

The developer (not the end user) builds the reference database by testing 30–50 common Philippine appliances on the actual hardware. Since the same ADS131M02 + ZMCT118F + ESP32-S3 signal chain is used for both enrollment and runtime inference, there is no hardware-dependent feature mismatch. This eliminates the transfer learning problem that makes public datasets (PLAID, COOLL) unsuitable for appliance-level K-NN.

### 12.2 AGILASx as Foundation

The AGILASx paper validates that K-NN on {W, VA, VAR, A, V, PF} achieves 99% accuracy on 50 Philippine appliances. This informs the feature set choice and provides confidence in the approach. If the dataset itself can be obtained (via the authors at Ateneo de Manila, CentraleSupélec, or Kyoto University), it could serve as a pre-populated reference database — though per-hardware enrollment is still preferred for maximum accuracy.

### 12.3 Harmonic Extension

When using per-deployment enrollment, the feature set can be extended beyond the AGILASx base to include harmonic features (THD, H3/H1, H5/H1, H7/H1, CF, OE, H5/H3, Prms). This is possible because the enrollment and inference signal chains are identical. The extended features improve intra-class discrimination (e.g., separating two SMPS loads of similar wattage) at the cost of requiring quantized feature transmission from the node (10–11 additional bytes per telemetry packet).

---

## 13. Summary of Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Algorithm | K-NN | No retraining, flexible label set, transparent confidence |
| Feature set | AGILASx 6D base (W, VA, VAR, A, V, PF) | Already in telemetry packet, validated at 99% accuracy |
| Execution location | Gateway only | Zero node firmware changes, central database |
| Label granularity | Appliance type, not brand | Signatures cluster by topology, not manufacturer |
| Label coherence | Wattage spread < 2× lowest instance | Prevents label territories from overlapping other types |
| State handling | Multi-state profiles (up to 4 per appliance) | Covers cycling appliances like refrigerators |
| State detection | Online streaming accumulators | 317 bytes/node vs. 57 KB/node for raw buffering |
| Runtime tracking | Zero-power gap swap detection + temporal voting | Distinguishes appliance swaps from internal state transitions |
| Training redundancy | 2–3 instances per appliance type | Expands coverage without overfitting; diminishing returns at 3 |
| Training mode | Gateway-internal, nodes unaware | No protocol changes, 8 concurrent sessions |
| Model format | JSON via REST API | Human-readable, editable, versionable |
| Persistence | FRAM (MB85RC512T upgrade recommended) | Wear-free, existing I2C bus, drop-in swap |
| Fallback | Decision tree class label | Graceful degradation for unknown appliances |
