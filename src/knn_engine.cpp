#ifdef KNN_INFERENCE
#ifdef NODE_GATEWAY

#include "knn_engine.h"
#include "knn_db.h"
#include "gateway_state.h"
#include "log_async.h"
#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------
#define SETTLE_THRESHOLD_W   10.0f  // W delta that triggers re-identification
#define ZERO_THRESHOLD_W      1.0f  // W below this = zero-power gap
#define ZERO_COUNT_MIN           2  // consecutive zero samples = appliance swap
#define SETTLE_COUNT_MIN         3  // stable samples required before inference
#define VOTE_SIZE                5  // majority-vote window depth
#define REJECT_MULT           4.0f  // reject if dist > REJECT_MULT * radiusSq

// ---------------------------------------------------------------------------
// Feature derivation
// ---------------------------------------------------------------------------

static void deriveFeatures(const TelemetryPacket& pkt, float* feat)
{
  float W   = pkt.power       / 10.0f;
  float V   = pkt.voltage     / 10.0f;
  float A   = pkt.current     / 1000.0f;
  float PF  = pkt.powerFactor / 100.0f;
  float VA  = V * A;
  float q2  = VA * VA - W * W;
  float VAR = (q2 > 0.0f) ? sqrtf(q2) : 0.0f;
  feat[0] = W;
  feat[1] = VA;
  feat[2] = VAR;
  feat[3] = A;
  feat[4] = V;
  feat[5] = PF;
}

static void normalizeFeatures(const float* raw, float* norm)
{
  for (uint8_t f = 0; f < KNN_FEATURES; f++) {
    float s = g_knnDb.normSigma[f];
    norm[f] = (s > 1e-6f) ? (raw[f] - g_knnDb.normMu[f]) / s : 0.0f;
  }
}

// ---------------------------------------------------------------------------
// Distance search -- IRAM to avoid instruction cache pressure
// Caller must hold g_knnMutex.
// Returns profile index (0xFF = no profiles), sets *out_state and *out_dist.
// ---------------------------------------------------------------------------

static uint8_t IRAM_ATTR knnSearch(const float* norm_feat,
                                   uint8_t*     out_state,
                                   float*       out_dist,
                                   uint8_t*     out_label)
{
  float   best    = 1e30f;
  uint8_t best_p  = 0xFF;
  uint8_t best_s  = 0;
  uint8_t best_lbl = 0xFF;

  for (uint8_t p = 0; p < g_knnDb.profileCount; p++) {
    const ApplianceProfile& pr = g_knnDb.profiles[p];
    for (uint8_t s = 0; s < pr.nStates; s++) {
      const KnnStateCentroid& c = pr.states[s];
      float d = 0.0f;
      for (uint8_t f = 0; f < KNN_FEATURES; f++) {
        float sig  = (g_knnDb.normSigma[f] > 1e-6f) ? g_knnDb.normSigma[f] : 1.0f;
        float diff = norm_feat[f] - (c.centroid[f] - g_knnDb.normMu[f]) / sig;
        d += diff * diff;
      }
      if (d < best) {
        best     = d;
        best_p   = p;
        best_s   = s;
        best_lbl = pr.labelIdx;
      }
    }
  }

  *out_state = best_s;
  *out_dist  = best;
  *out_label = best_lbl;
  return best_p;
}

// ---------------------------------------------------------------------------
// Minimum normSigma^2 across features -- used to convert raw radiusSq to
// normalized space conservatively (dividing by the smallest scale^2 gives
// the largest normalized radius, so we never reject valid matches).
// Must be called under g_knnMutex.
// ---------------------------------------------------------------------------
static float knnMinNormSigSq()
{
  float mn = 1.0f;
  for (uint8_t f = 0; f < KNN_FEATURES; f++) {
    float s2 = g_knnDb.normSigma[f] * g_knnDb.normSigma[f];
    if (s2 > 1e-6f && s2 < mn) mn = s2;
  }
  return mn;
}

// ---------------------------------------------------------------------------
// Majority-vote helpers
// ---------------------------------------------------------------------------

static void votePush(uint8_t* buf, uint8_t* pos, uint8_t val)
{
  buf[*pos] = val;
  *pos = (*pos + 1) % VOTE_SIZE;
}

static uint8_t voteMajority(const uint8_t* buf)
{
  uint8_t best_val = buf[0];
  uint8_t best_cnt = 0;
  for (uint8_t i = 0; i < VOTE_SIZE; i++) {
    uint8_t v = buf[i];
    uint8_t c = 0;
    for (uint8_t j = 0; j < VOTE_SIZE; j++) if (buf[j] == v) c++;
    if (c > best_cnt) { best_cnt = c; best_val = v; }
  }
  return best_val;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void knnNodeReset(uint8_t slotIdx)
{
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  NodeState& ns          = g_nodes[slotIdx];
  ns.knnMachineState     = KNN_ST_UNIDENTIFIED;
  ns.knnLabelIdx         = 0xFF;
  ns.knnStateIdx         = 0;
  ns.knnDistSq           = 0.0f;
  ns.knnPrevW            = 0.0f;
  ns.knnSettleCnt        = 0;
  ns.knnZeroCnt          = 0;
  ns.knnVotePos          = 0;
  memset(ns.knnVoteBuf, 0xFF, VOTE_SIZE);
  xSemaphoreGive(g_nodesMutex);
}

void knnInferStep(uint8_t slotIdx, const TelemetryPacket& pkt)
{
  if (!g_knnDb.loaded || g_knnDb.profileCount == 0) return;

  // Derive raw features from packet (no shared state)
  float feat[KNN_FEATURES];
  deriveFeatures(pkt, feat);
  float W = feat[0];

  // --- Step 1: snapshot per-node KNN state (brief mutex) ---
  uint8_t mst, zeroCnt, settleCnt, votePos, curLabel;
  uint8_t voteBuf[VOTE_SIZE];
  float   prevW;

  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  mst       = g_nodes[slotIdx].knnMachineState;
  zeroCnt   = g_nodes[slotIdx].knnZeroCnt;
  settleCnt = g_nodes[slotIdx].knnSettleCnt;
  votePos   = g_nodes[slotIdx].knnVotePos;
  curLabel  = g_nodes[slotIdx].knnLabelIdx;
  prevW     = g_nodes[slotIdx].knnPrevW;
  memcpy(voteBuf, g_nodes[slotIdx].knnVoteBuf, VOTE_SIZE);
  xSemaphoreGive(g_nodesMutex);

  // --- Step 2: zero-gap detection (Layer 1) ---
  zeroCnt = (W < ZERO_THRESHOLD_W) ? (zeroCnt < 255 ? zeroCnt + 1 : 255) : 0;

  if (zeroCnt >= ZERO_COUNT_MIN) {
    mst       = KNN_ST_UNIDENTIFIED;
    settleCnt = 0;
    curLabel  = 0xFF;
    memset(voteBuf, 0xFF, VOTE_SIZE);
    goto write_state;
  }

  // --- Step 3: load-change detection (Layer 2) ---
  {
    float deltaW = fabsf(W - prevW);
    if (deltaW > SETTLE_THRESHOLD_W) {
      if (mst == KNN_ST_IDENTIFIED && curLabel != 0xFF) {
        // Check if new W matches another operating state of the current appliance
        float norm_feat[KNN_FEATURES];
        normalizeFeatures(feat, norm_feat);
        bool state_match = false;

        if (xSemaphoreTake(g_knnMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          float min_sig2 = knnMinNormSigSq();
          for (uint8_t p = 0; p < g_knnDb.profileCount && !state_match; p++) {
            if (g_knnDb.profiles[p].labelIdx != curLabel) continue;
            for (uint8_t s = 0; s < g_knnDb.profiles[p].nStates; s++) {
              const KnnStateCentroid& c = g_knnDb.profiles[p].states[s];
              float d = 0.0f;
              for (uint8_t f = 0; f < KNN_FEATURES; f++) {
                float sig  = (g_knnDb.normSigma[f] > 1e-6f) ? g_knnDb.normSigma[f] : 1.0f;
                float diff = norm_feat[f] - (c.centroid[f] - g_knnDb.normMu[f]) / sig;
                d += diff * diff;
              }
              if (d <= (c.radiusSq / min_sig2) * REJECT_MULT) { state_match = true; break; }
            }
          }
          xSemaphoreGive(g_knnMutex);
        }

        if (!state_match) {
          mst       = KNN_ST_SETTLING;
          settleCnt = 0;
        }
        // If state_match: keep identity, let settle counter hold
      } else {
        mst       = KNN_ST_SETTLING;
        settleCnt = 0;
      }
    }
  }

  // --- Step 4: temporal voter (Layer 3) ---
  if (mst == KNN_ST_SETTLING || mst == KNN_ST_UNIDENTIFIED) {
    settleCnt = (settleCnt < 255) ? settleCnt + 1 : 255;

    if (settleCnt >= SETTLE_COUNT_MIN) {
      float norm_feat[KNN_FEATURES];
      normalizeFeatures(feat, norm_feat);

      uint8_t best_state  = 0;
      uint8_t best_lbl    = 0xFF;
      float   dist        = 0.0f;
      uint8_t best_p      = 0xFF;

      if (xSemaphoreTake(g_knnMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        best_p = knnSearch(norm_feat, &best_state, &dist, &best_lbl);
        if (best_p != 0xFF) {
          // Rejection threshold: max radiusSq of nearest profile scaled to
          // normalized space (raw radiusSq / min normSigma^2) * REJECT_MULT.
          float min_sig2 = knnMinNormSigSq();
          float max_r = 0.0f;
          for (uint8_t s = 0; s < g_knnDb.profiles[best_p].nStates; s++) {
            float r = g_knnDb.profiles[best_p].states[s].radiusSq / min_sig2;
            if (r > max_r) max_r = r;
          }
          if (dist > max_r * REJECT_MULT) { best_p = 0xFF; best_lbl = 0xFF; }
        }
        xSemaphoreGive(g_knnMutex);
      }

      votePush(voteBuf, &votePos, best_lbl);
      uint8_t winner = voteMajority(voteBuf);

      if (winner != 0xFF) {
        mst      = KNN_ST_IDENTIFIED;
        curLabel = winner;

        if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
          g_nodes[slotIdx].knnMachineState = mst;
          g_nodes[slotIdx].knnLabelIdx     = curLabel;
          g_nodes[slotIdx].knnStateIdx     = best_state;
          g_nodes[slotIdx].knnDistSq       = dist;
          g_nodes[slotIdx].knnSettleCnt    = settleCnt;
          g_nodes[slotIdx].knnZeroCnt      = zeroCnt;
          g_nodes[slotIdx].knnPrevW        = W;
          g_nodes[slotIdx].knnVotePos      = votePos;
          memcpy(g_nodes[slotIdx].knnVoteBuf, voteBuf, VOTE_SIZE);
          xSemaphoreGive(g_nodesMutex);
        }
        logAsync("[KNN] Slot%d identified: \"%s\" (dist=%.1f)\n",
                 slotIdx + 1, knnGetLabel(curLabel), dist);
        return;
      }
    }
  }

write_state:
  if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    g_nodes[slotIdx].knnMachineState = mst;
    g_nodes[slotIdx].knnLabelIdx     = curLabel;
    g_nodes[slotIdx].knnSettleCnt    = settleCnt;
    g_nodes[slotIdx].knnZeroCnt      = zeroCnt;
    g_nodes[slotIdx].knnPrevW        = W;
    g_nodes[slotIdx].knnVotePos      = votePos;
    memcpy(g_nodes[slotIdx].knnVoteBuf, voteBuf, VOTE_SIZE);
    xSemaphoreGive(g_nodesMutex);
  }
}

#endif // NODE_GATEWAY
#endif // KNN_INFERENCE
