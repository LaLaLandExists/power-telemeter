/**
 * knn_engine.h
 * Per-node K-NN appliance inference state machine.
 */
#pragma once
#ifdef KNN_INFERENCE

#include <Arduino.h>
#include "tdma_protocol.h"

// Machine state values stored in NodeState.knnMachineState
#define KNN_ST_UNIDENTIFIED  0u
#define KNN_ST_SETTLING      1u
#define KNN_ST_IDENTIFIED    2u

/**
 * Run one inference step for slotIdx.
 * Derives 6 features from pkt, updates g_nodes[slotIdx].knn* fields.
 * Called from Core 1 after processUplink releases g_nodesMutex.
 * pkt must remain valid for the duration of the call (stack lifetime is fine).
 */
void knnInferStep(uint8_t slotIdx, const TelemetryPacket& pkt);

/**
 * Reset all K-NN inference state for slotIdx.
 * Call on node eviction or fresh join.
 * Takes g_nodesMutex internally.
 */
void knnNodeReset(uint8_t slotIdx);

#endif // KNN_INFERENCE
