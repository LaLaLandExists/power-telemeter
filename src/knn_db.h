/**
 * knn_db.h
 * In-SRAM K-NN appliance profile database and serialization helpers.
 * FRAM persistence is managed by fram_store.cpp.
 */
#pragma once
#ifdef KNN_INFERENCE

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ---------------------------------------------------------------------------
// Limits and feature layout
// ---------------------------------------------------------------------------
#define KNN_MAX_LABELS    48
#define KNN_MAX_PROFILES  48
#define KNN_MAX_STATES     4
#define KNN_LABEL_LEN     24
#define KNN_FEATURES       6  // W, VA, VAR, A, V, PF -- in that order

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/** One operating-state centroid within an appliance profile. */
struct KnnStateCentroid {
  float    centroid[KNN_FEATURES];
  float    radiusSq;   // acceptance radius^2 in normalized feature space
  uint16_t samples;    // number of training samples that built this centroid
  uint8_t  _pad[2];   // explicit pad to keep sizeof == 32
};

/** All operating states of one appliance type. */
struct ApplianceProfile {
  uint8_t          labelIdx;              // index into KnnDb.labels[]
  uint8_t          nStates;
  uint8_t          _pad[2];
  KnnStateCentroid states[KNN_MAX_STATES];
};

/** Full in-SRAM K-NN database. */
struct KnnDb {
  char             labels[KNN_MAX_LABELS][KNN_LABEL_LEN];
  uint8_t          labelCount;
  float            normMu[KNN_FEATURES];
  float            normSigma[KNN_FEATURES];
  ApplianceProfile profiles[KNN_MAX_PROFILES];
  uint8_t          profileCount;
  bool             loaded;
  bool             fromEmbed;   // true when last loaded from embedded model
};

extern KnnDb             g_knnDb;
extern SemaphoreHandle_t g_knnMutex;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/** Create g_knnMutex. Must be called once before any task starts. */
void knnDbInit();

/** Zero all profiles/labels; set loaded=false. Thread-safe. */
void knnDbClear();

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

/**
 * Parse a JSON model buffer into g_knnDb.
 * Takes g_knnMutex internally.
 * @param from_embed  true when called for the embedded factory model.
 * @return true on success.
 */
bool knnDeserialize(const uint8_t* buf, size_t len, bool from_embed);

/**
 * Serialize g_knnDb to JSON, writing to a Print sink.
 * Takes g_knnMutex internally.
 */
void knnSerialize(Print& out);

// ---------------------------------------------------------------------------
// Label helpers (caller must NOT hold g_knnMutex)
// ---------------------------------------------------------------------------

/**
 * Find an existing label or add a new one.
 * Must be called with g_knnMutex held.
 * @return label index, or 0xFF if table is full.
 */
uint8_t knnFindOrAddLabel(const char* label);

/** Return label string for index; "" if invalid or unidentified. */
const char* knnGetLabel(uint8_t idx);

/** Return human-readable machine state string. */
const char* knnMachineStateName(uint8_t state);

// ---------------------------------------------------------------------------
// Normalization helper -- recomputes mu/sigma from enrolled centroids.
// Called under g_knnMutex by knn_training.cpp after each finalize.
// ---------------------------------------------------------------------------
void knnRecomputeNorm();

#endif // KNN_INFERENCE
