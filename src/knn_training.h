/**
 * knn_training.h
 * Per-node streaming training accumulators for K-NN appliance enrollment.
 * Nodes are entirely unaware -- training operates on normal telemetry packets.
 */
#pragma once
#ifdef KNN_INFERENCE

#include <Arduino.h>
#include "knn_db.h"
#include "tdma_protocol.h"

// Maximum operating states detectable during one training session
#define KNN_TRAIN_MAX_STATES  8
// Wattage gap threshold for detecting a new operating state
#define KNN_TRAIN_GAP_W      15.0f
// Minimum samples per state; states below this are discarded as transients
#define KNN_TRAIN_MIN_SAMPLES 10
// Packets at or below this wattage are treated as no-load and skipped
#define KNN_TRAIN_MIN_W       3.0f

/** Per-state online streaming accumulator. */
struct KnnTrainState {
  float    sumF[KNN_FEATURES];
  float    sumF2[KNN_FEATURES];
  uint32_t cnt;
  float    wCenter;  // representative W (mean) used for gap detection
};

/** Full training session for one slot. */
struct KnnTrainSession {
  volatile bool   active;
  bool            finalized;
  char            label[KNN_LABEL_LEN];
  uint32_t        deadlineMs;
  uint8_t         nStates;
  KnnTrainState   states[KNN_TRAIN_MAX_STATES];
};

extern KnnTrainSession   g_knnTrain[MAX_NODES];
extern SemaphoreHandle_t g_knnTrainMutex;

/** Initialize g_knnTrainMutex. Must be called before tasks start. */
void knnTrainInit();

/**
 * Start a training session for slotIdx.
 * Called from Core 0 (REST handler).
 */
void knnTrainStart(uint8_t slotIdx, const char* label, uint32_t durationMs);

/**
 * Stop and finalize a training session immediately.
 * Called from Core 0 (REST handler).
 */
void knnTrainStop(uint8_t slotIdx);

/**
 * Accumulate one telemetry packet into the active session for slotIdx.
 * Also checks deadline; finalizes automatically if expired.
 * Called from Core 1 (TDMA task) after processUplink releases g_nodesMutex.
 */
void knnTrainAccumulate(uint8_t slotIdx, const TelemetryPacket& pkt);

/** Return true if slotIdx has an active training session. */
bool knnTrainIsActive(uint8_t slotIdx);

#endif // KNN_INFERENCE
