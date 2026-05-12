# AutoRule Feature Plan

## Node-Local Rule Engine for Automated Relay Control

---

## 1. Overview

AutoRule replaces the existing single-window relay schedule system with a layered, priority-resolved rule engine evaluated locally on each telemetry node. The engine supports three rule types — protection rules that react to PZEM measurements, schedule rules that define time-of-day windows, and a default state — composed into per-node RuleSets of up to 8 rules. All rules are configured from the dashboard, delivered over the existing TDMA downlink within the current 7-byte `MAX_DL_PAYLOAD_LEN` constraint, persisted in NVS across power cycles, and evaluated every 500 ms by the node without gateway involvement.

The feature directly addresses the demand-response automation and protective-action use cases identified in the concept paper's extensibility roadmap, while preserving the system's core properties: no cloud dependency, no firmware reflashing for new automation logic, and no changes to the TDMA timing budget.

---

## 2. Motivation

### 2.1 Limitations of the Current Schedule System

The existing `schedTask` evaluates a single time window per node — one ON time and one OFF time — every 10 seconds. This model cannot express:

- Multiple time windows (e.g., morning and afternoon shifts with a lunch break).
- Measurement-conditional actions (e.g., trip relay on overcurrent).
- Combined time-and-measurement logic (e.g., run the aircon from 8 AM to 6 PM but only if voltage is above 190 V).
- A configurable default state when no schedule is active.

Every one of these requires either multiple time windows, compound conditions, or a mix of time and measurement triggers — none of which the current single-window `RelaySchedulePacket` supports.

### 2.2 Why Node-Local Evaluation

Protection actions like overcurrent shutdown are time-critical. The current gateway-mediated path has a worst-case latency of 6 seconds (node uplinks the reading, gateway evaluates, gateway downlinks a relay command in the next superframe). With node-local rule evaluation against fresh PZEM data every 500 ms, the worst-case reaction time drops to 500 ms — a 12x improvement. Node-local rules also continue operating if the gateway reboots or radio contact is temporarily lost.

---

## 3. Rule Type Hierarchy

Rules are organized into three types with a fixed priority order. This hierarchy is not configurable — it reflects the universal operational principle that safety overrides scheduling, and scheduling overrides the resting state.

```
Priority    Type            Evaluation cadence    Trigger source
────────────────────────────────────────────────────────────────
Highest     PROTECTION      Every 500 ms          PZEM readings
            SCHEDULE        Every 500 ms          Wall clock (RTC)
Lowest      DEFAULT         Constant              None (fallback)
```

The rule evaluator processes all three layers bottom-up: the default state is set first, then any matching schedule rule overrides it, then any firing protection rule overrides everything. Within each layer, all rules are evaluated; the last matching rule in that layer determines the output. In practice, all protection rules should agree on their action (relay OFF for fault conditions), so ordering within the protection layer is not operationally significant.

---

## 4. Data Structures

### 4.1 AutoRule Struct (Node-Side Storage)

```cpp
#pragma pack(1)
struct AutoRule {
    uint8_t  flags;       // [enabled:1][type:2][action:1][reserved:4]
    uint8_t  field;       // PZEM field index (protection only)
    uint8_t  op;          // comparison operator
    uint16_t param_a;     // threshold (protection) or ON time (schedule)
    uint16_t param_b;     // hysteresis (protection) or OFF time (schedule)
    uint8_t  _pad;        // alignment / future use
};
// sizeof(AutoRule) = 8 bytes
```

#### 4.1.1 `flags` Bit Layout

```
Bit 7       enabled     1 = rule is active, 0 = rule is skipped during evaluation
Bits 6-5    type        0 = PROTECTION, 1 = SCHEDULE, 2 = DEFAULT
Bit 4       action      0 = RELAY_OFF, 1 = RELAY_ON
Bits 3-0    reserved    set to 0 (available for future flags)
```

The `enabled` bit allows the dashboard to disable a rule without deleting it, so the user can temporarily suspend a protection rule during maintenance without losing its configuration.

#### 4.1.2 `field` Values (Protection Rules Only)

```
Value   PZEM Field        Encoding (matches TelemetryPacket)
──────────────────────────────────────────────────────────────
0       Voltage            uint16_t, value × 10    (e.g., 2200 = 220.0 V)
1       Current            uint16_t, value × 1000  (e.g., 15000 = 15.000 A)
2       Active Power       uint16_t, value × 10    (e.g., 20000 = 2000.0 W)
3       Energy             uint32_t, value in Wh   (not typically used for rules)
4       Frequency          uint16_t, value × 10    (e.g., 600 = 60.0 Hz)
5       Power Factor       uint16_t, value × 100   (e.g., 85 = 0.85)
```

For protection rules, `param_a` holds the threshold in the same scaled-integer encoding used by the TelemetryPacket. `param_b` holds the hysteresis margin in the same encoding.

#### 4.1.3 `op` Values

```
Value   Operator    Description
──────────────────────────────────
0       GT          Greater than
1       LT          Less than
2       GE          Greater than or equal
3       LE          Less than or equal
4       BETWEEN     Time window (schedule rules only)
```

Schedule rules always use `op = BETWEEN`. Protection rules use `GT`, `LT`, `GE`, or `LE`. The `BETWEEN` operator is not valid for protection rules; the node rejects any rule combining a protection type with `op = BETWEEN`.

#### 4.1.4 `param_a` / `param_b` Interpretation by Rule Type

```
Rule Type       param_a                     param_b
────────────────────────────────────────────────────────────────
PROTECTION      Threshold (scaled int)      Hysteresis margin (scaled int)
SCHEDULE        ON time (minutes since      OFF time (minutes since
                midnight, 0-1439)           midnight, 0-1439)
DEFAULT         Unused (set to 0)           Unused (set to 0)
```

For schedule rules, time is encoded as minutes since midnight. 06:00 = 360, 14:30 = 870, 22:00 = 1320. The 16-bit field accommodates the full 0-1439 range with room to spare.

### 4.2 RuleState Struct (Runtime State, Not Transmitted)

```cpp
struct RuleState {
    bool latched;           // protection rule has fired and not yet cleared
    uint32_t lastToggleMs;  // millis() of last relay state change caused by this rule
};
```

One `RuleState` per rule slot (8 total). The `latched` flag implements hysteresis for protection rules. The `lastToggleMs` field enforces the minimum relay toggle interval (Section 7.3). Both are persisted in NVS to survive power cycles.

### 4.3 RuleSet (Complete Per-Node Configuration)

```cpp
struct RuleSet {
    uint8_t   count;            // number of active rules (0-8)
    AutoRule  rules[8];         // rule definitions
    RuleState state[8];         // runtime state (latched flags, toggle timestamps)
};
// Total: 1 + 64 + 40 = 105 bytes
```

---

## 5. Downlink Packet Design

### 5.1 Constraint

All downlink frames are zero-padded to `MAX_DL_PAYLOAD_LEN = 7` bytes to maintain the SF6 implicit-header fixed-length RX window. The existing `DlHeader` (2 bytes: `pktType` + `nodeId`) is prepended by the TDMA layer, so the rule packet payload must fit in 7 bytes.

### 5.2 DlRulePacket

```cpp
#pragma pack(1)
struct DlRulePacket {
    uint8_t  ruleIndex;     // 0-7: set rule at this index
                            // 0xFE: clear all rules
                            // 0xFF: commit (apply pending changes)
    uint8_t  flags;         // same layout as AutoRule.flags
    uint8_t  field_op;      // [field:4 | op:4] packed
    uint16_t param_a;       // threshold or ON time
    uint16_t param_b;       // hysteresis or OFF time
};
// sizeof(DlRulePacket) = 7 bytes — exactly MAX_DL_PAYLOAD_LEN
```

New packet type identifier: `PKT_SET_RULE = 0x07` (next available after existing packet types in the DlHeader `pktType` enum).

### 5.3 Delivery Protocol

Deploying a full RuleSet to a node requires a multi-superframe sequence. The gateway queues the following DL command sequence and delivers one per superframe in the target node's DL window:

```
Superframe N:    PKT_SET_RULE { ruleIndex=0xFE }           — clear all rules
Superframe N+1:  PKT_SET_RULE { ruleIndex=0, ... }         — rule 0
Superframe N+2:  PKT_SET_RULE { ruleIndex=1, ... }         — rule 1
...
Superframe N+K:  PKT_SET_RULE { ruleIndex=K-1, ... }       — rule K-1
Superframe N+K+1: PKT_SET_RULE { ruleIndex=0xFF }          — commit
```

The **clear** command wipes the active rule table and sets the relay to its power-on default (OFF). Individual rule packets are buffered but not applied until the **commit** command arrives. This prevents a partially-delivered rule set from producing inconsistent behavior during the multi-superframe transfer window.

For a typical 5-rule RuleSet, delivery takes 7 superframes (21 seconds): 1 clear + 5 rules + 1 commit. This latency is acceptable because rule configuration is an infrequent operation (the user sets it up once and it persists in NVS).

### 5.4 Confirmation

After the commit command, the node writes the complete RuleSet to NVS and begins evaluation. The node reports its active rule count and current rule-derived relay state in the next uplink TelemetryPacket. The gateway confirms delivery by comparing the reported rule count against the expected value. If a mismatch is detected (e.g., a DL packet was lost), the gateway retransmits the full sequence.

The TelemetryPacket already carries a `relayState` field and `scheduleState` field. The `scheduleState` byte is repurposed to encode:

```
Bits 7-4:   activeRuleCount (0-8)
Bit 3:      protectionLatched (1 = at least one protection rule is currently latched)
Bit 2:      ruleEngineEnabled (1 = rules loaded and evaluating)
Bits 1-0:   relaySource (0 = manual, 1 = protection, 2 = schedule, 3 = default)
```

This gives the dashboard enough information to display why the relay is in its current state.

---

## 6. Evaluation Logic

### 6.1 Evaluator Function

The evaluator runs every 500 ms, merged into the existing `pzemTask` cycle on Core 0 at priority 1. It replaces `schedTask` entirely.

```cpp
// Pseudocode — actual implementation is C++11 without STL

enum RelayAction : uint8_t {
    ACTION_NONE = 0,
    ACTION_OFF  = 1,
    ACTION_ON   = 2
};

enum RelaySource : uint8_t {
    SOURCE_MANUAL     = 0,
    SOURCE_PROTECTION = 1,
    SOURCE_SCHEDULE   = 2,
    SOURCE_DEFAULT    = 3
};

struct EvalResult {
    RelayAction action;
    RelaySource source;
};

EvalResult evaluateRules(const RuleSet& rs,
                         const PzemData& pzem,
                         uint16_t minutesSinceMidnight)
{
    EvalResult result;
    result.action = ACTION_OFF;       // implicit default if no DEFAULT rule
    result.source = SOURCE_DEFAULT;

    // Phase 1: DEFAULT (lowest priority, set first)
    for (uint8_t i = 0; i < rs.count; i++) {
        if (!isEnabled(rs.rules[i])) continue;
        if (getType(rs.rules[i]) != TYPE_DEFAULT) continue;
        result.action = getAction(rs.rules[i]) ? ACTION_ON : ACTION_OFF;
        result.source = SOURCE_DEFAULT;
        break;  // at most one default rule
    }

    // Phase 2: SCHEDULE (overrides default)
    for (uint8_t i = 0; i < rs.count; i++) {
        if (!isEnabled(rs.rules[i])) continue;
        if (getType(rs.rules[i]) != TYPE_SCHEDULE) continue;

        uint16_t onTime  = rs.rules[i].param_a;
        uint16_t offTime = rs.rules[i].param_b;
        bool inWindow;

        if (onTime <= offTime) {
            // normal window: e.g., 06:00 (360) to 22:00 (1320)
            inWindow = (minutesSinceMidnight >= onTime &&
                        minutesSinceMidnight < offTime);
        } else {
            // overnight window: e.g., 22:00 (1320) to 06:00 (360)
            inWindow = (minutesSinceMidnight >= onTime ||
                        minutesSinceMidnight < offTime);
        }

        if (inWindow) {
            result.action = getAction(rs.rules[i]) ? ACTION_ON : ACTION_OFF;
            result.source = SOURCE_SCHEDULE;
        }
    }

    // Phase 3: PROTECTION (highest priority, overrides everything)
    for (uint8_t i = 0; i < rs.count; i++) {
        if (!isEnabled(rs.rules[i])) continue;
        if (getType(rs.rules[i]) != TYPE_PROTECTION) continue;

        int16_t reading = getPzemField(pzem, rs.rules[i].field);
        bool tripped = compareThreshold(reading, rs.rules[i].op, rs.rules[i].param_a);

        if (tripped) {
            rs.state[i].latched = true;
        }

        if (rs.state[i].latched) {
            bool cleared = checkHysteresisClear(
                reading, rs.rules[i].op,
                rs.rules[i].param_a, rs.rules[i].param_b
            );
            if (cleared) {
                rs.state[i].latched = false;
            }
        }

        if (rs.state[i].latched) {
            result.action = getAction(rs.rules[i]) ? ACTION_ON : ACTION_OFF;
            result.source = SOURCE_PROTECTION;
        }
    }

    return result;
}
```

### 6.2 Manual Override

Dashboard relay commands (received via `PKT_RELAY_CMD` in the TDMA DL window) set a manual override flag with a configurable timeout:

```cpp
volatile bool     g_manualOverride = false;
volatile uint32_t g_manualOverrideUntil = 0;  // millis() expiry
const uint32_t    MANUAL_OVERRIDE_MS = 60000; // 60 seconds default

// In DL handler (Core 1, TDMA task):
void onRelayCommand(bool relayOn) {
    digitalWrite(RELAY_PIN, relayOn ? HIGH : LOW);
    g_manualOverride = true;
    g_manualOverrideUntil = millis() + MANUAL_OVERRIDE_MS;
}

// In rule evaluator (Core 0, ruleTask):
void applyRuleResult(EvalResult result) {
    if (g_manualOverride) {
        if (millis() >= g_manualOverrideUntil) {
            g_manualOverride = false;
            // fall through to rule-driven state
        } else {
            return;  // manual override active, skip rule output
        }
    }
    // apply result.action to relay with toggle rate limiting
}
```

Complete priority hierarchy, highest to lowest:

```
1. Manual dashboard command (60 s override window)
2. Protection rules (latching with hysteresis)
3. Schedule rules (time-of-day windows)
4. Default state
```

---

## 7. Hysteresis

### 7.1 Purpose

Hysteresis prevents relay chatter when a PZEM reading oscillates around a protection threshold. Without hysteresis, a current reading fluctuating between 14.9 A and 15.1 A against a 15 A threshold would toggle the relay every 500 ms — damaging contactors and producing audible clicking.

### 7.2 Mechanism

Each protection rule has a trip threshold (`param_a`) and a hysteresis margin (`param_b`). The relay trips when the reading crosses `param_a` in the triggering direction, and clears only when the reading crosses `param_a - param_b` (for GT/GE rules) or `param_a + param_b` (for LT/LE rules) in the opposite direction.

```
Example: Current > 15.00 A, hysteresis 2.00 A
  param_a = 15000  (15.00 A × 1000)
  param_b = 2000   (2.00 A × 1000)

  Trip condition:   current > 15000   (reading exceeds 15.00 A)
  Clear condition:  current < 13000   (reading drops below 13.00 A)
  Dead band:        13.00 A to 15.00 A — relay stays in whatever state it was last set to
```

```
Example: Voltage < 190 V, hysteresis 10 V
  param_a = 1900   (190.0 V × 10)
  param_b = 100    (10.0 V × 10)

  Trip condition:   voltage < 1900    (reading drops below 190.0 V)
  Clear condition:  voltage > 2000    (reading exceeds 200.0 V)
  Dead band:        190.0 V to 200.0 V
```

### 7.3 Clear Logic Implementation

```cpp
bool checkHysteresisClear(int16_t reading, uint8_t op,
                          uint16_t threshold, uint16_t hysteresis)
{
    switch (op) {
        case OP_GT:
        case OP_GE:
            // tripped because reading was too high
            // clears when reading drops below (threshold - hysteresis)
            return reading < (int16_t)(threshold - hysteresis);

        case OP_LT:
        case OP_LE:
            // tripped because reading was too low
            // clears when reading rises above (threshold + hysteresis)
            return reading > (int16_t)(threshold + hysteresis);

        default:
            return false;
    }
}
```

### 7.4 Schedule Hysteresis

Schedule rules inherit the existing dual-clock hysteresis from the current system. The schedule RTC is a free-running `millis()`-based wall clock initialized from the beacon H/M/S and corrected only if the delta exceeds the `RTC_CORRECTION_THRESHOLD_MS` (2 seconds). This prevents browser-clock jitter from causing relay chatter at schedule window boundaries. No additional hysteresis mechanism is needed for schedule rules because the time source is already stabilized.

---

## 8. Persistence

### 8.1 NVS Layout

```
NVS Namespace: "rules"

Key          Type        Size     Description
────────────────────────────────────────────────────────────────
"count"      uint8_t     1 byte   Number of active rules (0-8)
"r0".."r7"   blob        8 bytes  AutoRule structs
"s0".."s7"   blob        5 bytes  RuleState (latched flag + lastToggleMs)
```

Total NVS footprint: 1 + (8 × 8) + (8 × 5) = 105 bytes.

### 8.2 Write Policy

Rules are written to NVS only on commit (when `PKT_SET_RULE` with `ruleIndex = 0xFF` is received). This is an infrequent operation — typically once during initial setup and occasionally when the user modifies rules. NVS write endurance (approximately 100,000 cycles for the ESP32's NOR flash) is not a concern at this write frequency.

RuleState (latched flags and toggle timestamps) is written to NVS on every latch state change. Since protection trips are infrequent events (ideally never, in normal operation), this does not create a write-endurance concern. In the worst case of a flapping fault that repeatedly trips and clears, the hysteresis dead band limits the toggle rate, and therefore the NVS write rate, to at most once every few seconds — well within endurance budgets over any realistic deployment lifetime.

### 8.3 Boot Sequence

On node boot:

1. Load `count` and all `AutoRule` structs from NVS namespace `"rules"`.
2. Load all `RuleState` structs from NVS.
3. Read the first PZEM sample (500 ms after boot).
4. Evaluate rules against the fresh reading and current RTC time.
5. Apply the result — including any latched protection states carried over from before the reboot.

Step 5 is safety-critical: if the node rebooted during an overcurrent event (because the overcurrent tripped the upstream breaker, cutting power to the node), the relay must not come back ON when power returns if the fault condition is still present. The persisted `latched = true` flag ensures the protection rule remains active until the reading genuinely enters the hysteresis clear band.

---

## 9. Relay Toggle Rate Limiting

### 9.1 Purpose

Rapid relay toggling (multiple state changes per second) damages contactors, produces audible noise, and can cause voltage transients on the monitored circuit. The rule evaluator enforces a minimum interval between relay state changes regardless of the rule source.

### 9.2 Implementation

```cpp
const uint32_t MIN_RELAY_TOGGLE_MS = 5000; // 5 seconds minimum between toggles

void applyRelayState(RelayAction action, uint32_t nowMs) {
    static uint32_t lastToggleMs = 0;
    static RelayAction currentState = ACTION_OFF;

    if (action == currentState) return;  // no change, nothing to do

    if ((nowMs - lastToggleMs) < MIN_RELAY_TOGGLE_MS) {
        return;  // too soon since last toggle, suppress
    }

    digitalWrite(RELAY_PIN, (action == ACTION_ON) ? HIGH : LOW);
    currentState = action;
    lastToggleMs = nowMs;
}
```

This rate limiter applies to all relay state changes — whether from manual commands, protection rules, schedule rules, or default state transitions. The 5-second minimum is a conservative default; it could be made configurable via a separate DL command if needed.

---

## 10. Task Model Changes

### 10.1 Before (Current System)

```
Task        Core    Priority    Period      Purpose
──────────────────────────────────────────────────────────────
PZEM        0       1           500 ms      Modbus sampling
SCHED       0       1           10 s        Relay schedule evaluation
LED         0       1           —           State indicator
FRAM        0       0           queue       Deferred I²C writes
Log drain   0       1           —           Serial + WebSocket relay
```

### 10.2 After (AutoRule)

```
Task        Core    Priority    Period      Purpose
──────────────────────────────────────────────────────────────
PZEM        0       1           500 ms      Modbus sampling + rule evaluation
LED         0       1           —           State indicator
FRAM        0       0           queue       Deferred I²C writes
Log drain   0       1           —           Serial + WebSocket relay
```

`schedTask` is removed. Rule evaluation is merged into `pzemTask` at the end of each sampling cycle, after the PZEM reading completes and before the mutex is released. This is natural because the rule evaluator needs the PZEM data, and running it inside `pzemTask` avoids an additional mutex acquisition. The added execution time is negligible — the evaluator walks at most 8 rules with simple integer comparisons, taking under 10 microseconds on the Xtensa core.

The evaluation cadence changes from 10 seconds (schedTask) to 500 ms (pzemTask), improving schedule boundary accuracy from ±10 seconds to ±500 ms and enabling sub-second protection response.

---

## 11. Gateway-Side Changes

### 11.1 Rule Storage

The gateway does not evaluate rules — it only stores and delivers them. Per-node rule configurations are stored in the gateway's FRAM alongside the existing per-node data (energy totals, history, labels). The FRAM address map is extended to include a RuleSet block per node.

### 11.2 FRAM Address Map Extension

```
Current per-node block (existing):
    Offset +0x00:  UID (2 bytes)
    Offset +0x02:  Label (16 bytes)
    Offset +0x12:  Energy total (4 bytes)
    Offset +0x16:  History ring buffer (1920 bytes)

New fields (appended):
    Offset +0x796: Rule count (1 byte)
    Offset +0x797: AutoRule[0..7] (64 bytes)
```

Total per-node block growth: 65 bytes. Total FRAM usage increase across 8 nodes: 520 bytes. The MB85RC256V has 32,768 bytes; the existing per-node block is approximately 1,942 bytes × 8 = 15,536 bytes, leaving 17,232 bytes free. The 520-byte increase is well within budget.

### 11.3 WebSocket API Extension

New WebSocket message types for rule management:

```
Client -> Gateway:
{
    "type": "setRules",
    "nodeId": 3,
    "rules": [
        { "type": "default", "action": "off" },
        { "type": "schedule", "onTime": "06:00", "offTime": "12:00", "action": "on" },
        { "type": "schedule", "onTime": "14:00", "offTime": "18:00", "action": "on" },
        { "type": "protection", "field": "current", "op": "gt",
          "threshold": 20.0, "hysteresis": 3.0, "action": "off" },
        { "type": "protection", "field": "voltage", "op": "lt",
          "threshold": 190, "hysteresis": 10, "action": "off" }
    ]
}

Gateway -> Client (confirmation after commit ACK):
{
    "type": "rulesApplied",
    "nodeId": 3,
    "ruleCount": 5,
    "deliveryMs": 21000
}

Gateway -> Client (in telemetry push, per-node status):
{
    "type": "telemetry",
    "nodeId": 3,
    ...existing fields...
    "ruleStatus": {
        "count": 5,
        "engineActive": true,
        "protectionLatched": false,
        "relaySource": "schedule"
    }
}
```

### 11.4 REST API Extension

```
GET  /api/node/{id}/rules        — returns the current RuleSet for this node
POST /api/node/{id}/rules        — accepts a JSON RuleSet, queues DL delivery
```

---

## 12. Dashboard UI

### 12.1 Simple View (Backward Compatible)

For users who only need a single time window (equivalent to the current schedule), the existing schedule panel is preserved. Internally, it generates one DEFAULT rule (action: OFF) and one SCHEDULE rule with the specified ON/OFF times. The user never sees the rule abstraction.

### 12.2 Advanced Rules Panel

An expandable "Automation Rules" section for each node, showing:

```
Node 3: Workshop Aircon
─────────────────────────────────────────
Default state: [OFF ▼]

Schedule windows:
  [06:00] — [12:00]  ON   [× remove]
  [14:00] — [18:00]  ON   [× remove]
  [+ Add window]

Protection limits:
  Current  [> ▼] [20.0] A  → OFF  Hysteresis: [3.0] A   [× remove]
  Voltage  [< ▼] [190]  V  → OFF  Hysteresis: [10]  V   [× remove]
  [+ Add limit]

Status: ● Engine active | Relay: ON (schedule) | No faults
─────────────────────────────────────────
[Apply Rules]
```

The status line reflects the `ruleStatus` fields from the telemetry push, giving the user immediate feedback on why the relay is in its current state.

### 12.3 Validation

The dashboard validates the rule set before submitting:

- At most 8 rules total.
- At most 1 DEFAULT rule.
- Schedule ON/OFF times must be valid (0-1439 minutes).
- Protection thresholds must be within PZEM sensor range.
- Hysteresis must be positive and less than the threshold value.
- No two protection rules may target the same field with the same operator (ambiguous).

---

## 13. Backward Compatibility

### 13.1 Legacy Schedule Migration

On firmware upgrade, if the node has an existing `RelaySchedulePacket` stored in NVS (from the old schedule system), the boot sequence converts it to a 2-rule RuleSet:

```
Rule 0: DEFAULT, action = OFF
Rule 1: SCHEDULE, param_a = oldOnTime, param_b = oldOffTime, action = ON
```

The old NVS keys (`schedOn`, `schedOff`) are deleted after migration. This ensures that a firmware upgrade preserves the user's existing schedule without manual reconfiguration.

### 13.2 Gateway Protocol Compatibility

The gateway continues to accept the old `RelaySchedulePacket` from the dashboard for backward compatibility with older node firmware versions. If the target node reports a firmware version that supports AutoRule (indicated by a version flag in the TelemetryPacket `fwVersion` field), the gateway translates the schedule into `DlRulePacket` commands. Otherwise, it sends the legacy `RelaySchedulePacket`.

---

## 14. Example RuleSets

### 14.1 Simple Schedule (Replaces Current System)

```
DEFAULT:    relay OFF
SCHEDULE:   06:00-22:00 → relay ON
```

Equivalent to the current `schedTask` with ON=06:00, OFF=22:00. Two rules, delivered in 4 superframes (12 s).

### 14.2 Multi-Window with Lunch Break

```
DEFAULT:    relay OFF
SCHEDULE:   06:00-12:00 → relay ON     (morning shift)
SCHEDULE:   14:00-18:00 → relay ON     (afternoon shift)
```

Three rules. Relay is OFF from 12:00-14:00 (lunch) and from 18:00-06:00 (overnight).

### 14.3 Overnight Schedule

```
DEFAULT:    relay OFF
SCHEDULE:   22:00-06:00 → relay ON     (overnight, param_a > param_b)
```

Two rules. The overnight window detection uses the inverted comparison: `minutesSinceMidnight >= 1320 OR minutesSinceMidnight < 360`.

### 14.4 Protection Only (No Schedule)

```
DEFAULT:    relay ON                    (normally running)
PROTECTION: current > 20.0 A → OFF     (hysteresis: 3.0 A)
PROTECTION: voltage < 190 V → OFF      (hysteresis: 10 V)
```

Three rules. The relay is normally ON (default). If current exceeds 20 A, the relay trips and stays off until current drops below 17 A. If voltage drops below 190 V (brownout), the relay trips and stays off until voltage recovers above 200 V. Both conditions are evaluated independently — if either fires, the relay is OFF.

### 14.5 Full Routine (Schedule + Protection)

```
DEFAULT:    relay OFF
SCHEDULE:   08:00-12:00 → relay ON
SCHEDULE:   13:00-17:00 → relay ON
PROTECTION: current > 15.0 A → OFF     (hysteresis: 2.0 A)
PROTECTION: voltage < 185 V → OFF      (hysteresis: 15 V)
PROTECTION: pf < 0.60 → OFF            (hysteresis: 0.10)
```

Six rules. The relay runs during business hours (with lunch break), but trips on overcurrent, undervoltage, or extremely poor power factor. All protection conditions are independent; any one tripping is sufficient to override the schedule.

---

## 15. Implementation Scope Estimate

### 15.1 Node Firmware

```
Component                            Estimated LOC    Files affected
────────────────────────────────────────────────────────────────────
AutoRule / RuleSet structs           ~40              auto_rule.h (new)
Rule evaluator function              ~80              auto_rule.cpp (new)
Hysteresis clear logic                ~30              auto_rule.cpp
Relay toggle rate limiter             ~20              auto_rule.cpp
NVS persistence (save/load)          ~60              auto_rule.cpp
DlRulePacket handler                  ~50              tdma_node.cpp (modify)
pzemTask integration                  ~20              pzem_task.cpp (modify)
Legacy schedule migration             ~30              auto_rule.cpp
TelemetryPacket rule status field     ~10              packets.h (modify)
schedTask removal                    –60              sched_task.cpp (delete)
────────────────────────────────────────────────────────────────────
Net change:                          ~280 lines       3 new + 3 modified + 1 deleted
```

### 15.2 Gateway Firmware

```
Component                            Estimated LOC    Files affected
────────────────────────────────────────────────────────────────────
DlRulePacket queueing                ~60              tdma_gw.cpp (modify)
FRAM rule storage (save/load)        ~40              fram_driver.cpp (modify)
WebSocket rule API handler            ~80              web_api.cpp (modify)
REST rule endpoints                   ~40              web_api.cpp (modify)
TelemetryPacket rule status parsing   ~15              tdma_gw.cpp (modify)
────────────────────────────────────────────────────────────────────
Total:                               ~235 lines       3 modified
```

### 15.3 Dashboard SPA

```
Component                            Estimated LOC    Files affected
────────────────────────────────────────────────────────────────────
Rule builder UI (HTML/CSS/JS)        ~200             index.html (modify)
Rule validation logic                 ~60             index.html (modify)
WebSocket rule API integration        ~40             index.html (modify)
Rule status display in node card      ~30             index.html (modify)
────────────────────────────────────────────────────────────────────
Total:                               ~330 lines       1 modified
```

### 15.4 Total

Approximately 845 lines of new/modified code across node firmware, gateway firmware, and dashboard SPA. No new hardware. No changes to the TDMA timing budget, superframe structure, frequency-hopping plan, or DL payload size. No new build environments or preprocessor flags.
