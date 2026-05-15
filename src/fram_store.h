/**
 * fram_store.h
 *
 * UID-keyed hint persistence for per-node accumEnergy, history[], and label
 * via MB85RC256V FRAM (32 KB, I²C) using the robtillaart/FRAM_I2C library.
 *
 * Each FRAM slot is owned by a deviceUID independent of TDMA slot assignment.
 * When all 8 slots are occupied and a 9th unique node needs storage, the
 * oldest slot (FIFO, tracked by nextWriteIdx) is evicted.
 *
 * On node join, framQueueRestore() searches by UID:
 *   found    → restore label, energy, history as best-effort hints
 *   not found → novel node; a FIFO slot is allocated and stamped immediately
 *
 * FRAM memory layout
 * ------------------
 *   0x0000  Magic        (uint32_t) = 0xDEADF00D
 *   0x0004  Version      (uint8_t)  = 2
 *   0x0005  nextWriteIdx (uint8_t)  — FIFO write pointer (0–7)
 *   0x0006  Padding      (10 bytes) → align to 0x0010
 *   0x0010  Node[0] block (1968 bytes):
 *             +0   deviceUID   (uint16_t) — slot owner UID (0x0000 = empty)
 *             +2   _pad        (uint16_t)
 *             +4   accumEnergy (uint32_t)
 *             +8   histHead    (int32_t)
 *             +12  histCount   (int32_t)
 *             +16  label       (char[30]) — human-readable node name
 *             +46  _pad2       (uint16_t) — align history to 48
 *             +48  history[120] (1920 bytes, 120 × 16 bytes each)
 *   0x07C0  Node[1] block …
 *   …
 *   Total: 16 + 8×1968 = 15 760 bytes  (<32 768 bytes)
 *
 * Write policy
 * ------------
 *   framSaveEnergy / framSaveHistory: deferred via dirty-counter (n=10),
 *   dispatched via framQueueSave() from gateway_tdma_task.cpp.
 *   framSaveLabel: written immediately on every rename (low frequency).
 *   Label is only updated in FRAM when non-empty, preserving user-assigned
 *   names across eviction (which clears the in-memory label).
 *
 * Version history
 * ---------------
 *   v1 — initial layout (no label field, block size 1936)
 *   v2 — added label[30] at +16, history shifted to +48, block size 1968;
 *        header byte 5 repurposed from padding to nextWriteIdx (was 0x00,
 *        so existing v2 chips upgrade transparently)
 */
#pragma once
#include <stdint.h>

/**
 * Initialise I²C bus and verify FRAM is reachable.
 * Writes magic + version on the very first boot (blank chip).
 * On version mismatch (layout upgrade), stamps the new version and skips
 * restore so stale data is never applied to the wrong offsets.
 *
 * @param sda   SDA GPIO (e.g. 32)
 * @param scl   SCL GPIO (e.g. 33)
 * @param addr  I²C address (default 0x50)
 * @return true  if FRAM responded and is ready
 *         false if device not found (persistence silently disabled)
 */
bool framInit(uint8_t sda, uint8_t scl, uint8_t addr = 0x50);

/**
 * No-op. All data is restored on demand via framQueueRestore() when a node
 * joins. Kept so call-sites in main.cpp need no change.
 */
void framLoadAll();

/**
 * Persist accumEnergy (and deviceUID as validation key) for one node slot.
 * @param nodeIdx  0-based index into g_nodes[] (0–7)
 */
void framSaveEnergy(uint8_t nodeIdx);

/**
 * Persist deviceUID, histHead, histCount, and the full history[] ring buffer
 * for one node slot.
 * @param nodeIdx  0-based index into g_nodes[] (0–7)
 */
void framSaveHistory(uint8_t nodeIdx);

/**
 * Persist the human-readable label for one node slot.
 * Written immediately on rename (not deferred).
 * @param nodeIdx  0-based index into g_nodes[] (0–7)
 */
void framSaveLabel(uint8_t nodeIdx);

/**
 * Start the background FRAM drain task (Core 0, priority 0).
 * Must be called once from setup() after framInit() succeeds.
 */
void framTaskStart();

/**
 * Queue a deferred FRAM save from any task or core.
 * Non-blocking: the request is dropped silently if the queue is full.
 * @param nodeIdx     0-based node index (0–7)
 * @param saveEnergy  true → persist accumEnergy
 * @param saveHistory true → persist history[]
 */
void framQueueSave(uint8_t nodeIdx, bool saveEnergy, bool saveHistory);

/**
 * Queue a deferred FRAM restore for a newly joined node.
 * The FRAM task searches all 8 slots by g_nodes[nodeIdx].deviceUID:
 *   found     → restore label, energy, history as hints
 *   not found → allocate a FIFO slot and stamp UID + default label
 * Non-blocking: the request is dropped silently if the queue is full.
 * Call after setting g_nodes[nodeIdx].deviceUID (i.e. after JoinAck is sent).
 * @param nodeIdx  0-based node index (0–7)
 */
void framQueueRestore(uint8_t nodeIdx);

/**
 * Synchronously invalidate the FRAM magic so the next boot treats the chip
 * as blank (all node data wiped). Call immediately before ESP.restart().
 * No-op if framInit() never succeeded.
 */
void framErase();
