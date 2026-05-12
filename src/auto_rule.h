/**
 * auto_rule.h
 *
 * AutoRule structs, constants, and function declarations for the node-local
 * rule engine. Included by node firmware only (auto_rule.cpp,
 * node_tdma_task.cpp). The gateway includes this header only to store and
 * forward AutoRule blobs — it never calls the evaluator.
 *
 * Rule priority hierarchy (highest wins):
 *   1. PROTECTION — trips on PZEM readings; latches with hysteresis
 *   2. SCHEDULE   — time-of-day windows using RTC minutes-since-midnight
 *   3. DEFAULT    — constant fallback state
 *
 * Delivery: rules arrive over TDMA via DlRulePacket (PKT_SET_RULE = 0x08).
 * The gateway sends: CLEAR (0xFE) → rule 0 → … → rule N-1 → COMMIT (0xFF).
 * Rules take effect atomically on COMMIT and are persisted to NVS.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

// --- Rule type constants -----------------------------------------------------
#define RULE_TYPE_PROTECTION  0
#define RULE_TYPE_SCHEDULE    1
#define RULE_TYPE_DEFAULT     2

// --- Comparison operators (AutoRule.op) --------------------------------------
#define OP_GT       0   // Greater than              (protection)
#define OP_LT       1   // Less than                 (protection)
#define OP_GE       2   // Greater than or equal     (protection)
#define OP_LE       3   // Less than or equal        (protection)
#define OP_BETWEEN  4   // Time window               (schedule only)

// --- PZEM field indices (AutoRule.field, protection rules only) --------------
// Encodings match TelemetryPacket fixed-point convention.
#define FIELD_VOLTAGE      0   // uint16_t * 10   (e.g. 2200 = 220.0 V)
#define FIELD_CURRENT      1   // uint32_t * 1000 (e.g. 15000 = 15.000 A) — stored as uint16_t, max ~65 A
#define FIELD_POWER        2   // uint32_t * 10   (e.g. 20000 = 2000.0 W) — stored as uint16_t, max ~6550 W
#define FIELD_ENERGY       3   // uint32_t, raw Wh
#define FIELD_FREQUENCY    4   // uint16_t * 10   (e.g. 600 = 60.0 Hz)
#define FIELD_POWER_FACTOR 5   // uint16_t * 100  (e.g. 85 = 0.85)

// --- Rule relay actions ------------------------------------------------------
#define RULE_ACTION_OFF  0
#define RULE_ACTION_ON   1

// --- Capacity ----------------------------------------------------------------
#define AUTORULE_MAX  8

// --- flags bit accessors -----------------------------------------------------
#define RULE_ENABLED(r)  (((r).flags >> 7) & 0x01)
#define RULE_TYPE(r)     (((r).flags >> 5) & 0x03)
#define RULE_ACTION(r)   (((r).flags >> 4) & 0x01)

// Build a flags byte from components
#define RULE_FLAGS(enabled, type, action) \
  (uint8_t)(((enabled) & 0x01) << 7 | ((type) & 0x03) << 5 | ((action) & 0x01) << 4)

// --- AutoRule struct (8 bytes, packed) ---------------------------------------
#pragma pack(push, 1)
struct AutoRule {
  uint8_t  flags;     // [enabled:1][type:2][action:1][reserved:4]
  uint8_t  field;     // PZEM field index (FIELD_* above); ignored for DEFAULT/SCHEDULE
  uint8_t  op;        // OP_* above
  uint16_t param_a;   // protection: threshold (scaled int); schedule: ON  minutes-since-midnight
  uint16_t param_b;   // protection: hysteresis (scaled int); schedule: OFF minutes-since-midnight
  uint8_t  _pad;      // reserved / future flags
};
static_assert(sizeof(AutoRule) == 8, "AutoRule must be 8 bytes");
#pragma pack(pop)

// --- RuleState (runtime, not transmitted) ------------------------------------
struct RuleState {
  bool     latched;         // protection rule has fired and not yet cleared
  uint32_t lastToggleMs;    // millis() of last relay change caused by this rule
};

// --- EvalResult --------------------------------------------------------------
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
  bool        protectionLatched;  // at least one protection rule is latched
};

// --- PzemData snapshot (node-side, from g_pzem via mutex copy) ---------------
struct PzemSnapshot {
  uint16_t voltage;      // * 10
  uint16_t current;      // * 1000 (truncated to uint16 — max 65.535 A)
  uint16_t power;        // * 10   (truncated to uint16 — max 6553.5 W)
  uint16_t frequency;    // * 10
  uint16_t powerFactor;  // * 100
};

// --- Rule staging buffer (filled during CLEAR→RULE→COMMIT sequence) ----------
struct RuleStaging {
  AutoRule rules[AUTORULE_MAX];
  uint8_t  count;
};

// -----------------------------------------------------------------------------
// Public API (implemented in auto_rule.cpp)
// -----------------------------------------------------------------------------

/**
 * Load active rules and RuleState from NVS namespace "rules".
 * Call once from nodeTdmaTaskStart() before any evaluation.
 */
void ruleNvsLoad();

/**
 * Persist active rules to NVS. Called on COMMIT from handleRulePacket().
 */
void ruleNvsSave();

/**
 * Load per-rule latch state from NVS. Called on boot after ruleNvsLoad().
 */
void ruleNvsLoadState();

/**
 * Persist latch state for one rule slot. Called on every latch transition.
 */
void ruleNvsSaveState(uint8_t ruleIdx);

/**
 * Evaluate all active rules against the current PZEM snapshot and RTC time.
 *
 * @param minutesSinceMidnight  Current local time as minutes since midnight (0-1439).
 * @param pzem                  Current PZEM snapshot (scaled integers).
 * @return EvalResult           Recommended relay action, source, and latched flag.
 */
EvalResult evaluateRules(uint16_t minutesSinceMidnight, const PzemSnapshot& pzem);

/**
 * Apply a relay action with a 5-second minimum toggle interval.
 * No-ops if action == current state or if too soon since last toggle.
 *
 * @param action  ACTION_ON or ACTION_OFF
 * @param nowMs   Current millis()
 */
void applyRelayState(RelayAction action, uint32_t nowMs);

/**
 * Handle an incoming DlRulePacket from the TDMA downlink.
 *
 * @param buf  Raw 7-byte payload (starting at ruleIndex, after DlHeader is stripped)
 * @param len  Payload length (must be >= 5 to be valid)
 * @return true if the packet was valid and processed
 */
bool handleRulePacket(const uint8_t* buf, uint8_t len);

/**
 * Return the number of currently active rules (0–8).
 */
uint8_t ruleGetCount();

/**
 * Return whether the rule engine is loaded and has at least one rule.
 */
bool ruleEngineEnabled();

/**
 * Return true if at least one protection rule is currently latched.
 */
bool ruleProtectionLatched();

/**
 * Return the relay source from the last evaluation.
 */
RelaySource ruleLastSource();
