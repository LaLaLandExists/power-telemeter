#ifdef KNN_INFERENCE
#ifdef NODE_GATEWAY

#include "knn_db.h"
#include "fram_store.h"
#include "log_async.h"
#include <ArduinoJson.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
KnnDb             g_knnDb;
SemaphoreHandle_t g_knnMutex = nullptr;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void knnDbInit()
{
  memset(&g_knnDb, 0, sizeof(g_knnDb));
  g_knnMutex = xSemaphoreCreateMutex();
  configASSERT(g_knnMutex);
}

void knnDbClear()
{
  if (xSemaphoreTake(g_knnMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  memset(&g_knnDb, 0, sizeof(g_knnDb));
  xSemaphoreGive(g_knnMutex);
}

// ---------------------------------------------------------------------------
// Label helpers (caller must hold g_knnMutex)
// ---------------------------------------------------------------------------

uint8_t knnFindOrAddLabel(const char* label)
{
  for (uint8_t i = 0; i < g_knnDb.labelCount; i++) {
    if (strncmp(g_knnDb.labels[i], label, KNN_LABEL_LEN - 1) == 0) return i;
  }
  if (g_knnDb.labelCount >= KNN_MAX_LABELS) return 0xFF;
  uint8_t idx = g_knnDb.labelCount++;
  strlcpy(g_knnDb.labels[idx], label, KNN_LABEL_LEN);
  return idx;
}

const char* knnGetLabel(uint8_t idx)
{
  if (idx == 0xFF || idx >= KNN_MAX_LABELS) return "";
  return g_knnDb.labels[idx];
}

const char* knnMachineStateName(uint8_t state)
{
  switch (state) {
    case 0: return "unidentified";
    case 1: return "settling";
    case 2: return "identified";
    default: return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Normalization recomputation (call under g_knnMutex)
// ---------------------------------------------------------------------------

void knnRecomputeNorm()
{
  float sum[KNN_FEATURES]  = {};
  float sum2[KNN_FEATURES] = {};
  int   n = 0;

  for (uint8_t p = 0; p < g_knnDb.profileCount; p++) {
    const ApplianceProfile& pr = g_knnDb.profiles[p];
    for (uint8_t s = 0; s < pr.nStates; s++) {
      for (uint8_t f = 0; f < KNN_FEATURES; f++) {
        float v  = pr.states[s].centroid[f];
        sum[f]  += v;
        sum2[f] += v * v;
      }
      n++;
    }
  }

  if (n < 2) return;

  for (uint8_t f = 0; f < KNN_FEATURES; f++) {
    float mu  = sum[f] / (float)n;
    float var = sum2[f] / (float)n - mu * mu;
    g_knnDb.normMu[f]    = mu;
    g_knnDb.normSigma[f] = (var > 1e-6f) ? sqrtf(var) : 1.0f;
  }
}

// ---------------------------------------------------------------------------
// Deserialization
// ---------------------------------------------------------------------------

bool knnDeserialize(const uint8_t* buf, size_t len, bool from_embed)
{
  JsonDocument doc;
  auto err = deserializeJson(doc, buf, len);
  if (err != DeserializationError::Ok) {
    logAsync("[KNN] JSON parse error: %s\n", err.c_str());
    return false;
  }

  int version = doc["version"] | 0;
  if (version != 1) {
    logAsync("[KNN] Unsupported model version %d\n", version);
    return false;
  }

  if (xSemaphoreTake(g_knnMutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

  memset(&g_knnDb, 0, sizeof(g_knnDb));
  g_knnDb.fromEmbed = from_embed;

  // Normalization
  JsonObject norm = doc["normalization"];
  if (norm) {
    JsonArray mu    = norm["mu"];
    JsonArray sigma = norm["sigma"];
    for (uint8_t f = 0; f < KNN_FEATURES && f < (uint8_t)mu.size(); f++)
      g_knnDb.normMu[f] = mu[f].as<float>();
    for (uint8_t f = 0; f < KNN_FEATURES && f < (uint8_t)sigma.size(); f++)
      g_knnDb.normSigma[f] = sigma[f].as<float>();
  }

  // Appliances
  for (JsonObject app : doc["appliances"].as<JsonArray>()) {
    if (g_knnDb.profileCount >= KNN_MAX_PROFILES) break;
    const char* label = app["label"] | "";
    if (label[0] == '\0') continue;

    uint8_t lidx = knnFindOrAddLabel(label);
    if (lidx == 0xFF) break;

    ApplianceProfile& prof = g_knnDb.profiles[g_knnDb.profileCount];
    memset(&prof, 0, sizeof(prof));
    prof.labelIdx = lidx;

    for (JsonObject st : app["states"].as<JsonArray>()) {
      if (prof.nStates >= KNN_MAX_STATES) break;
      KnnStateCentroid& cen = prof.states[prof.nStates];
      JsonArray c = st["centroid"];
      for (uint8_t f = 0; f < KNN_FEATURES && f < (uint8_t)c.size(); f++)
        cen.centroid[f] = c[f].as<float>();
      cen.radiusSq = st["radiusSq"] | 1.0f;
      cen.samples  = (uint16_t)(st["samples"] | 0);
      prof.nStates++;
    }

    if (prof.nStates > 0) g_knnDb.profileCount++;
  }

  g_knnDb.loaded = true;
  xSemaphoreGive(g_knnMutex);

  logAsync("[KNN] Deserialized: %d profiles, %d labels\n",
           g_knnDb.profileCount, g_knnDb.labelCount);
  return true;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void knnSerialize(Print& out)
{
  if (xSemaphoreTake(g_knnMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  JsonDocument doc;
  doc["version"]       = 1;
  doc["gridVoltage"]   = 220;
  doc["gridFrequency"] = 60;

  JsonObject norm  = doc["normalization"].to<JsonObject>();
  JsonArray  mu    = norm["mu"].to<JsonArray>();
  JsonArray  sigma = norm["sigma"].to<JsonArray>();
  for (uint8_t f = 0; f < KNN_FEATURES; f++) {
    mu.add(g_knnDb.normMu[f]);
    sigma.add(g_knnDb.normSigma[f]);
  }

  JsonArray apps = doc["appliances"].to<JsonArray>();
  for (uint8_t p = 0; p < g_knnDb.profileCount; p++) {
    const ApplianceProfile& prof = g_knnDb.profiles[p];
    JsonObject app = apps.add<JsonObject>();
    app["label"] = knnGetLabel(prof.labelIdx);
    JsonArray sts = app["states"].to<JsonArray>();
    for (uint8_t s = 0; s < prof.nStates; s++) {
      const KnnStateCentroid& cen = prof.states[s];
      JsonObject st = sts.add<JsonObject>();
      JsonArray  c  = st["centroid"].to<JsonArray>();
      for (uint8_t f = 0; f < KNN_FEATURES; f++) c.add(cen.centroid[f]);
      st["radiusSq"] = cen.radiusSq;
      st["samples"]  = cen.samples;
    }
  }

  xSemaphoreGive(g_knnMutex);
  serializeJson(doc, out);
}

#endif // NODE_GATEWAY
#endif // KNN_INFERENCE
