#ifdef KNN_INFERENCE
#ifdef NODE_GATEWAY

#include "knn_training.h"
#include "knn_db.h"
#include "fram_store.h"
#include "log_async.h"
#include <string.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
KnnTrainSession  g_knnTrain[MAX_NODES];
SemaphoreHandle_t g_knnTrainMutex = nullptr;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void knnTrainInit()
{
  memset(g_knnTrain, 0, sizeof(g_knnTrain));
  g_knnTrainMutex = xSemaphoreCreateMutex();
  configASSERT(g_knnTrainMutex);
}

bool knnTrainIsActive(uint8_t slotIdx)
{
  return slotIdx < MAX_NODES && g_knnTrain[slotIdx].active;
}

// ---------------------------------------------------------------------------
// Finalize -- called under g_knnTrainMutex, then updates g_knnDb
// ---------------------------------------------------------------------------

static void knnTrainingFinalize(uint8_t slotIdx)
{
  KnnTrainSession& sess = g_knnTrain[slotIdx];

  // Build ApplianceProfile from accumulators
  ApplianceProfile prof;
  memset(&prof, 0, sizeof(prof));
  uint8_t nValid = 0;

  for (uint8_t s = 0; s < sess.nStates; s++) {
    KnnTrainState& st = sess.states[s];
    if (st.cnt < KNN_TRAIN_MIN_SAMPLES) continue;
    if (st.wCenter < KNN_TRAIN_MIN_W) continue;  // discard no-load buckets
    if (nValid >= KNN_MAX_STATES) break;

    KnnStateCentroid& cen = prof.states[nValid];
    float maxVar = 0.0f;
    float sumVar = 0.0f;
    for (uint8_t f = 0; f < KNN_FEATURES; f++) {
      float mu  = st.sumF[f]  / (float)st.cnt;
      float mu2 = st.sumF2[f] / (float)st.cnt;
      float var = mu2 - mu * mu;
      if (var < 0.0f) var = 0.0f;
      cen.centroid[f] = mu;
      if (var > maxVar) maxVar = var;
      sumVar += var;
    }
    // radiusSq: 3-sigma^2 of the worst feature, floored at 0.01
    cen.radiusSq = (maxVar * 9.0f > 0.01f) ? maxVar * 9.0f : 0.01f;
    cen.samples  = (uint16_t)(st.cnt > 65535 ? 65535 : st.cnt);
    nValid++;
    (void)sumVar;
  }

  prof.nStates = nValid;
  if (nValid == 0) {
    logAsync("[KNN] Slot%d: training finalized with no valid states (insufficient samples)\n",
             slotIdx + 1);
    sess.active    = false;
    sess.finalized = true;
    return;
  }

  // Add profile to g_knnDb under g_knnMutex
  if (xSemaphoreTake(g_knnMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    uint8_t lidx = knnFindOrAddLabel(sess.label);
    if (lidx != 0xFF && g_knnDb.profileCount < KNN_MAX_PROFILES) {
      prof.labelIdx = lidx;
      memcpy(&g_knnDb.profiles[g_knnDb.profileCount], &prof, sizeof(prof));
      g_knnDb.profileCount++;
      g_knnDb.loaded = true;
      knnRecomputeNorm();
      logAsync("[KNN] Slot%d: enrolled \"%s\" (%d states, %d total profiles)\n",
               slotIdx + 1, sess.label, nValid, g_knnDb.profileCount);
    } else {
      logAsync("[KNN] Slot%d: label table or profile table full\n", slotIdx + 1);
    }
    xSemaphoreGive(g_knnMutex);
    framQueueSaveKnn();
  }

  sess.active    = false;
  sess.finalized = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void knnTrainStart(uint8_t slotIdx, const char* label, uint32_t durationMs)
{
  if (slotIdx >= MAX_NODES) return;

  if (xSemaphoreTake(g_knnTrainMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  KnnTrainSession& sess = g_knnTrain[slotIdx];
  memset(&sess, 0, sizeof(sess));
  strlcpy(sess.label, label, KNN_LABEL_LEN);
  sess.deadlineMs = millis() + durationMs;
  sess.finalized  = false;
  sess.active     = true;  // set last so Core 1 sees a fully initialized session

  xSemaphoreGive(g_knnTrainMutex);
  logAsync("[KNN] Slot%d: training started label=\"%s\" duration=%lus\n",
           slotIdx + 1, label, (unsigned long)(durationMs / 1000));
}

void knnTrainStop(uint8_t slotIdx)
{
  if (slotIdx >= MAX_NODES) return;
  if (!g_knnTrain[slotIdx].active) return;

  if (xSemaphoreTake(g_knnTrainMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  knnTrainingFinalize(slotIdx);
  xSemaphoreGive(g_knnTrainMutex);
}

void knnTrainAccumulate(uint8_t slotIdx, const TelemetryPacket& pkt)
{
  if (slotIdx >= MAX_NODES) return;

  KnnTrainSession& sess = g_knnTrain[slotIdx];
  if (!sess.active) return;

  // Auto-finalize on deadline
  if ((int32_t)(millis() - sess.deadlineMs) >= 0) {
    if (xSemaphoreTake(g_knnTrainMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (sess.active) knnTrainingFinalize(slotIdx);
      xSemaphoreGive(g_knnTrainMutex);
    }
    return;
  }

  float W   = pkt.power       / 10.0f;
  if (W < KNN_TRAIN_MIN_W) return;  // skip no-load / interruption packets
  float V   = pkt.voltage     / 10.0f;
  float A   = pkt.current     / 1000.0f;
  float PF  = pkt.powerFactor / 100.0f;
  float VA  = V * A;
  float q2  = VA * VA - W * W;
  float VAR = (q2 > 0.0f) ? sqrtf(q2) : 0.0f;
  float feat[KNN_FEATURES] = { W, VA, VAR, A, V, PF };

  // Find nearest state by W distance
  int   nearest = -1;
  float min_gap = 1e30f;
  for (uint8_t s = 0; s < sess.nStates; s++) {
    float gap = fabsf(W - sess.states[s].wCenter);
    if (gap < min_gap) { min_gap = gap; nearest = (int)s; }
  }

  if (nearest < 0 || (min_gap > KNN_TRAIN_GAP_W && sess.nStates < KNN_TRAIN_MAX_STATES)) {
    // Create a new state
    nearest = (int)sess.nStates++;
    memset(&sess.states[nearest], 0, sizeof(sess.states[nearest]));
    sess.states[nearest].wCenter = W;
  }

  // Update running accumulators
  KnnTrainState& st = sess.states[nearest];
  for (uint8_t f = 0; f < KNN_FEATURES; f++) {
    st.sumF[f]  += feat[f];
    st.sumF2[f] += feat[f] * feat[f];
  }
  st.cnt++;
  // Update wCenter as running mean of W
  st.wCenter = st.sumF[0] / (float)st.cnt;
}

#endif // NODE_GATEWAY
#endif // KNN_INFERENCE
