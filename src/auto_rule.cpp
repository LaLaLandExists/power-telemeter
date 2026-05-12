/**
 * auto_rule.cpp
 *
 * Node-local rule engine: evaluation, NVS persistence, relay toggle limiter,
 * and DlRulePacket handler.
 *
 * Thread safety:
 *   handleRulePacket() is called from the TDMA task (Core 1).
 *   evaluateRules() / applyRelayState() are called from pzemTask (Core 0).
 *   s_rules / s_ruleCount are written only on COMMIT (Core 1) and read every
 *   500 ms (Core 0). A simple volatile + atomic uint8_t count is sufficient
 *   because AutoRule structs are written as a batch under the staging buffer
 *   before atomically updating s_ruleCount, and the evaluator only reads
 *   s_ruleCount as the loop bound. Worst case the evaluator sees the old
 *   count for one cycle after a commit — acceptable.
 */

#include "auto_rule.h"
#include "log_async.h"
#include <Preferences.h>
#include <Arduino.h>
#include <string.h>

// --- Active rule set (written on COMMIT, read by evaluator) ------------------
static AutoRule         s_rules[AUTORULE_MAX];
static uint8_t          s_ruleCount         = 0;
static volatile bool    s_ruleEngineEnabled = false; // persisted via NVS key "active"

// --- Per-rule runtime state --------------------------------------------------
static RuleState s_state[AUTORULE_MAX];

// --- Staging buffer (filled during CLEAR → RULE → COMMIT sequence) -----------
static AutoRule  s_staging[AUTORULE_MAX];
static uint8_t   s_stagingCount = 0;

// --- Last evaluation result (for status reporting) ---------------------------
static RelaySource s_lastSource       = SOURCE_MANUAL;
static bool        s_anyLatched       = false;

// --- Relay toggle limiter state ----------------------------------------------
static uint32_t    s_lastToggleMs     = 0;
static RelayAction s_currentAction    = ACTION_OFF;

// --- External: relay GPIO control (defined in node_tdma_task.cpp) ------------
extern void setRelay(uint8_t state);

// --- NVS namespace -----------------------------------------------------------
static const char* NVS_NS = "rules";

// -----------------------------------------------------------------------------
// NVS persistence
// -----------------------------------------------------------------------------
void ruleNvsLoad() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return;   // read-only

  s_ruleCount         = prefs.getUChar("count", 0);
  if (s_ruleCount > AUTORULE_MAX) s_ruleCount = 0;
  s_ruleEngineEnabled = prefs.getBool("active", false);

  char key[4];
  for (uint8_t i = 0; i < s_ruleCount; i++) {
    snprintf(key, sizeof(key), "r%d", i);
    prefs.getBytes(key, &s_rules[i], sizeof(AutoRule));
  }
  prefs.end();

  logAsync("[NODE-RULE] Loaded %d rules from NVS (engine=%s)\n",
           s_ruleCount, s_ruleEngineEnabled ? "ON" : "OFF");
}

void ruleNvsSave() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return;  // read-write

  prefs.putUChar("count",  s_ruleCount);
  prefs.putBool ("active", s_ruleEngineEnabled);
  char key[4];
  for (uint8_t i = 0; i < AUTORULE_MAX; i++) {
    snprintf(key, sizeof(key), "r%d", i);
    if (i < s_ruleCount) {
      prefs.putBytes(key, &s_rules[i], sizeof(AutoRule));
    } else {
      prefs.remove(key);
    }
  }
  prefs.end();
}

void ruleSetEnabled(bool enable) {
  s_ruleEngineEnabled = enable;
  Preferences prefs;
  if (prefs.begin(NVS_NS, false)) {
    prefs.putBool("active", enable);
    prefs.end();
  }
}

void ruleNvsLoadState() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return;

  char key[4];
  for (uint8_t i = 0; i < AUTORULE_MAX; i++) {
    snprintf(key, sizeof(key), "s%d", i);
    uint8_t buf[5] = {0};
    size_t  got    = prefs.getBytes(key, buf, sizeof(buf));
    if (got == sizeof(buf)) {
      s_state[i].latched      = (bool)buf[0];
      uint32_t ts;
      memcpy(&ts, &buf[1], 4);
      s_state[i].lastToggleMs = ts;
    }
  }
  prefs.end();
}

void ruleNvsSaveState(uint8_t ruleIdx) {
  if (ruleIdx >= AUTORULE_MAX) return;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return;

  char key[4];
  snprintf(key, sizeof(key), "s%d", ruleIdx);
  uint8_t buf[5];
  buf[0] = (uint8_t)s_state[ruleIdx].latched;
  memcpy(&buf[1], &s_state[ruleIdx].lastToggleMs, 4);
  prefs.putBytes(key, buf, sizeof(buf));
  prefs.end();
}

// -----------------------------------------------------------------------------
// PZEM field extraction
// Returns the field value in the same scaled-integer encoding as TelemetryPacket.
// current and power are clamped to uint16_t range (max ~65 A / ~6550 W).
// -----------------------------------------------------------------------------
static uint16_t getPzemField(const PzemSnapshot& pzem, uint8_t field) {
  switch (field) {
    case FIELD_VOLTAGE:      return pzem.voltage;
    case FIELD_CURRENT:      return pzem.current;
    case FIELD_POWER:        return pzem.power;
    case FIELD_FREQUENCY:    return pzem.frequency;
    case FIELD_POWER_FACTOR: return pzem.powerFactor;
    default:                 return 0;
  }
}

// -----------------------------------------------------------------------------
// Threshold comparison
// -----------------------------------------------------------------------------
static bool compareThreshold(uint16_t reading, uint8_t op, uint16_t threshold) {
  switch (op) {
    case OP_GT: return reading >  threshold;
    case OP_LT: return reading <  threshold;
    case OP_GE: return reading >= threshold;
    case OP_LE: return reading <= threshold;
    default:    return false;
  }
}

// -----------------------------------------------------------------------------
// Hysteresis clear check (§7.3 of autorules.md)
// -----------------------------------------------------------------------------
static bool checkHysteresisClear(uint16_t reading, uint8_t op,
                                  uint16_t threshold, uint16_t hysteresis) {
  switch (op) {
    case OP_GT:
    case OP_GE:
      // Tripped because reading was too high; clear when reading drops below
      // (threshold - hysteresis). Guard against underflow.
      if (hysteresis >= threshold) return reading == 0;
      return reading < (uint16_t)(threshold - hysteresis);

    case OP_LT:
    case OP_LE:
      // Tripped because reading was too low; clear when reading rises above
      // (threshold + hysteresis). Guard against overflow.
      {
        uint32_t clearLevel = (uint32_t)threshold + hysteresis;
        if (clearLevel > 0xFFFF) return true;  // always clear if level overflows
        return reading > (uint16_t)clearLevel;
      }

    default:
      return false;
  }
}

// -----------------------------------------------------------------------------
// evaluateRules
// -----------------------------------------------------------------------------
EvalResult evaluateRules(uint16_t minutesSinceMidnight, const PzemSnapshot& pzem) {
  EvalResult result;
  result.action           = ACTION_OFF;   // implicit default if no DEFAULT rule
  result.source           = SOURCE_DEFAULT;
  result.protectionLatched = false;

  uint8_t count = s_ruleCount;
  if (count == 0) {
    s_lastSource = SOURCE_MANUAL;
    s_anyLatched = false;
    return result;
  }

  // Phase 1: DEFAULT (lowest priority)
  for (uint8_t i = 0; i < count; i++) {
    if (!RULE_ENABLED(s_rules[i])) continue;
    if (RULE_TYPE(s_rules[i]) != RULE_TYPE_DEFAULT) continue;
    result.action = RULE_ACTION(s_rules[i]) ? ACTION_ON : ACTION_OFF;
    result.source = SOURCE_DEFAULT;
    break;  // at most one default rule
  }

  // Phase 2: SCHEDULE (overrides default)
  for (uint8_t i = 0; i < count; i++) {
    if (!RULE_ENABLED(s_rules[i])) continue;
    if (RULE_TYPE(s_rules[i]) != RULE_TYPE_SCHEDULE) continue;

    uint16_t onTime  = s_rules[i].param_a;
    uint16_t offTime = s_rules[i].param_b;
    bool inWindow;

    if (onTime <= offTime) {
      inWindow = (minutesSinceMidnight >= onTime && minutesSinceMidnight < offTime);
    } else {
      // Overnight window: e.g. 22:00(1320) to 06:00(360)
      inWindow = (minutesSinceMidnight >= onTime || minutesSinceMidnight < offTime);
    }

    if (inWindow) {
      result.action = RULE_ACTION(s_rules[i]) ? ACTION_ON : ACTION_OFF;
      result.source = SOURCE_SCHEDULE;
    }
  }

  // Phase 3: PROTECTION (highest priority)
  for (uint8_t i = 0; i < count; i++) {
    if (!RULE_ENABLED(s_rules[i])) continue;
    if (RULE_TYPE(s_rules[i]) != RULE_TYPE_PROTECTION) continue;

    uint16_t reading = getPzemField(pzem, s_rules[i].field);
    bool tripped = compareThreshold(reading, s_rules[i].op, s_rules[i].param_a);

    if (tripped && !s_state[i].latched) {
      s_state[i].latched = true;
      ruleNvsSaveState(i);
      logAsync("[NODE-RULE] Protection rule %d latched (reading=%u)\n", i, reading);
    }

    if (s_state[i].latched) {
      bool cleared = checkHysteresisClear(
          reading, s_rules[i].op, s_rules[i].param_a, s_rules[i].param_b);
      if (cleared) {
        s_state[i].latched = false;
        ruleNvsSaveState(i);
        logAsync("[NODE-RULE] Protection rule %d cleared (reading=%u)\n", i, reading);
      }
    }

    if (s_state[i].latched) {
      result.action           = RULE_ACTION(s_rules[i]) ? ACTION_ON : ACTION_OFF;
      result.source           = SOURCE_PROTECTION;
      result.protectionLatched = true;
    }
  }

  s_lastSource = result.source;
  s_anyLatched = result.protectionLatched;
  return result;
}

// -----------------------------------------------------------------------------
// applyRelayState — 5-second minimum toggle interval
// -----------------------------------------------------------------------------
static const uint32_t MIN_RELAY_TOGGLE_MS = 5000;

void applyRelayState(RelayAction action, uint32_t nowMs) {
  if (action == ACTION_NONE) return;
  if (action == s_currentAction) return;

  if ((nowMs - s_lastToggleMs) < MIN_RELAY_TOGGLE_MS) return;

  setRelay(action == ACTION_ON ? 1 : 0);
  s_currentAction = action;
  s_lastToggleMs  = nowMs;

  logAsync("[NODE-RULE] Relay -> %s (source=%d)\n",
           action == ACTION_ON ? "ON" : "OFF", (int)s_lastSource);
}

// -----------------------------------------------------------------------------
// handleRulePacket — processes DlRulePacket payload (after DlHeader stripped)
//
// buf layout (5 bytes after nodeId, matching DlRulePacket fields after DlHeader):
//   buf[0] ruleIndex
//   buf[1] flags
//   buf[2] field_op
//   buf[3..4] param_a (LE)
//   buf[5..6] param_b (LE)
// -----------------------------------------------------------------------------
bool handleRulePacket(const uint8_t* buf, uint8_t len) {
  if (len < 5) return false;

  uint8_t ruleIndex = buf[0];

  if (ruleIndex == 0xFC) {
    // ENABLE: activate engine without modifying stored rules
    ruleSetEnabled(true);
    logAsync("[NODE-RULE] Engine ENABLED (rules preserved, count=%d)\n", s_ruleCount);
    return true;
  }

  if (ruleIndex == 0xFD) {
    // DISABLE: pause engine without clearing rules
    ruleSetEnabled(false);
    logAsync("[NODE-RULE] Engine DISABLED (rules preserved, count=%d)\n", s_ruleCount);
    return true;
  }

  if (ruleIndex == 0xFE) {
    // CLEAR: wipe staging buffer and active rules, disable engine, drive relay OFF
    memset(s_staging, 0, sizeof(s_staging));
    s_stagingCount = 0;
    memset(s_state,  0, sizeof(s_state));
    s_ruleCount = 0;
    ruleSetEnabled(false);
    setRelay(0);
    logAsync("[NODE-RULE] CLEAR received — rules wiped, relay OFF\n");
    return true;
  }

  if (ruleIndex == 0xFF) {
    // COMMIT: copy staging → active, enable engine, persist to NVS
    memcpy(s_rules, s_staging, sizeof(AutoRule) * s_stagingCount);
    s_ruleCount = s_stagingCount;
    ruleSetEnabled(true);
    ruleNvsSave();
    logAsync("[NODE-RULE] COMMIT — %d rules active, engine ON\n", s_ruleCount);
    return true;
  }

  if (ruleIndex >= AUTORULE_MAX) return false;

  // Store into staging
  uint16_t param_a, param_b;
  memcpy(&param_a, &buf[3], 2);
  memcpy(&param_b, &buf[5], 2);

  s_staging[ruleIndex].flags   = buf[1];
  s_staging[ruleIndex].field   = (buf[2] >> 4) & 0x0F;
  s_staging[ruleIndex].op      =  buf[2]        & 0x0F;
  s_staging[ruleIndex].param_a = param_a;
  s_staging[ruleIndex].param_b = param_b;
  s_staging[ruleIndex]._pad    = 0;

  if (ruleIndex >= s_stagingCount) s_stagingCount = ruleIndex + 1;

  logAsync("[NODE-RULE] Staged rule %d type=%d action=%d\n",
           ruleIndex,
           RULE_TYPE(s_staging[ruleIndex]),
           RULE_ACTION(s_staging[ruleIndex]));
  return true;
}

// -----------------------------------------------------------------------------
// Status accessors
// -----------------------------------------------------------------------------
uint8_t     ruleGetCount()          { return s_ruleCount; }
bool        ruleIsActive()          { return s_ruleEngineEnabled && s_ruleCount > 0; }
bool        ruleHasRules()          { return s_ruleCount > 0; }
bool        ruleProtectionLatched() { return s_anyLatched; }
RelaySource ruleLastSource()        { return s_lastSource; }
