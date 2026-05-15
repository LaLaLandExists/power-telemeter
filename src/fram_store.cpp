/**
 * fram_store.cpp
 *
 * UID-keyed FRAM persistence for gateway-side node hints.
 *
 * Each of the 8 FRAM slots is owned by a deviceUID, independent of TDMA
 * slot assignment. When all 8 are occupied and a 9th unique UID needs
 * storage the oldest slot (FIFO, tracked by nextWriteIdx in the header)
 * is evicted and its data is lost.
 *
 * On node join:
 *   framQueueRestore() → UID found → restore label, energy, history as hints
 *                     → UID not found → allocate FIFO slot, stamp UID+label
 *
 * Every reboot is a full re-contention; persistence is best-effort only.
 *
 * See fram_store.h for memory layout documentation.
 */
#ifdef NODE_GATEWAY

#include "fram_store.h"
#include "log_async.h"
#include <Wire.h>
#include <FRAM.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "gateway_state.h"

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr uint32_t FRAM_MAGIC           = 0xDEADF00DUL;
static constexpr uint8_t  FRAM_VERSION         = 2;
static constexpr uint16_t FRAM_HEADER_BASE     = 0x0000;  // magic(4)+ver(1)+nextWr(1)+pad(10)
static constexpr uint16_t FRAM_NEXT_WRITE_OFF  = 5;       // nextWriteIdx byte in header
static constexpr uint16_t FRAM_NODES_BASE      = 0x0010;  // first node block

// Per-node block offsets (relative to node block start)
static constexpr uint16_t OFF_UID        = 0;   // uint16_t
static constexpr uint16_t OFF_PAD        = 2;   // uint16_t (alignment pad)
static constexpr uint16_t OFF_ENERGY     = 4;   // uint32_t
static constexpr uint16_t OFF_HIST_HEAD  = 8;   // int32_t
static constexpr uint16_t OFF_HIST_COUNT = 12;  // int32_t
static constexpr uint16_t OFF_LABEL      = 16;  // char[30]
static constexpr uint16_t OFF_PAD2       = 46;  // uint16_t (align history to 48)
static constexpr uint16_t OFF_HISTORY    = 48;  // HistoryPoint[120]

static constexpr uint16_t LABEL_BYTES    = 30;
static constexpr uint16_t HISTORY_BYTES  = HISTORY_MAX_POINTS * sizeof(HistoryPoint);
static constexpr uint16_t NODE_BLOCK_SIZE = OFF_HISTORY + HISTORY_BYTES;  // 1968

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static FRAM s_fram(&Wire);
static bool s_framOk = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static inline uint16_t nodeBase(uint8_t idx)
{
  return FRAM_NODES_BASE + (uint16_t)idx * NODE_BLOCK_SIZE;
}

/**
 * Search all 8 FRAM slots for a given UID.
 * @return slot index 0-7, or 0xFF if not found.
 */
static uint8_t framFindSlot(uint16_t uid)
{
  for (uint8_t s = 0; s < MAX_NODES; s++) {
    if (s_fram.read16(nodeBase(s) + OFF_UID) == uid) return s;
  }
  return 0xFF;
}

/**
 * Allocate the next FIFO slot, evicting whatever is there.
 * Logs the evicted UID if the slot was occupied.
 * @return FRAM slot index (0-7).
 */
static uint8_t framAllocFifoSlot()
{
  uint8_t slot = s_fram.read8(FRAM_HEADER_BASE + FRAM_NEXT_WRITE_OFF) % MAX_NODES;
  uint16_t evicted = s_fram.read16(nodeBase(slot) + OFF_UID);
  if (evicted != 0x0000 && evicted != 0xFFFF) {
    logAsync("[FRAM] FIFO evict FRAM slot %u (UID=0x%04X)\n", slot + 1, evicted);
  }
  s_fram.write8(FRAM_HEADER_BASE + FRAM_NEXT_WRITE_OFF, (slot + 1) % MAX_NODES);
  return slot;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool framInit(uint8_t sda, uint8_t scl, uint8_t addr)
{
  Wire.begin((int)sda, (int)scl, 400000U);

  int rc = s_fram.begin(addr);
  if (rc != FRAM_OK || !s_fram.isConnected()) {
    logAsync("[FRAM] begin() rc=%d  isConnected=%d\n",
             rc, (int)s_fram.isConnected());
    return false;
  }

  uint32_t magic = s_fram.read32(FRAM_HEADER_BASE);
  if (magic != FRAM_MAGIC) {
    logAsync("[FRAM] Blank chip detected - writing header\n");
    s_fram.write32(FRAM_HEADER_BASE, FRAM_MAGIC);
    s_fram.write8(FRAM_HEADER_BASE + 4, FRAM_VERSION);
    s_fram.write8(FRAM_HEADER_BASE + FRAM_NEXT_WRITE_OFF, 0);
  } else {
    uint8_t ver = s_fram.read8(FRAM_HEADER_BASE + 4);
    if (ver != FRAM_VERSION) {
      logAsync("[FRAM] Layout v%u -> v%u: skipping restore, "
               "data will repopulate\n", ver, FRAM_VERSION);
      s_fram.write8(FRAM_HEADER_BASE + 4, FRAM_VERSION);
      s_fram.write8(FRAM_HEADER_BASE + FRAM_NEXT_WRITE_OFF, 0);
      s_framOk = true;
      return true;
    }
  }

  s_framOk = true;
  return true;
}

void framLoadAll()
{
  if (!s_framOk) return;
  // All data restored on demand via framQueueRestore() after node joins.
  (void)0;
}

void framSaveEnergy(uint8_t nodeIdx)
{
  if (!s_framOk) return;

  const NodeState& ns = g_nodes[nodeIdx];
  if (ns.deviceUID == 0x0000 || ns.deviceUID == 0xFFFF) return;

  uint8_t framSlot = framFindSlot(ns.deviceUID);
  if (framSlot == 0xFF) framSlot = framAllocFifoSlot();

  uint16_t base = nodeBase(framSlot);
  s_fram.write16(base + OFF_UID,    ns.deviceUID);
  s_fram.write32(base + OFF_ENERGY, ns.accumEnergy);
  if (ns.label[0] != '\0') {
    s_fram.write(base + OFF_LABEL,
                 reinterpret_cast<uint8_t*>(const_cast<char*>(ns.label)),
                 LABEL_BYTES);
  }
}

void framSaveHistory(uint8_t nodeIdx)
{
  if (!s_framOk) return;

  const NodeState& ns = g_nodes[nodeIdx];
  if (ns.deviceUID == 0x0000 || ns.deviceUID == 0xFFFF) return;

  uint8_t framSlot = framFindSlot(ns.deviceUID);
  if (framSlot == 0xFF) framSlot = framAllocFifoSlot();

  uint16_t base = nodeBase(framSlot);
  s_fram.write16(base + OFF_UID,        ns.deviceUID);
  s_fram.write32(base + OFF_HIST_HEAD,  (uint32_t)ns.histHead);
  s_fram.write32(base + OFF_HIST_COUNT, (uint32_t)ns.histCount);
  if (ns.label[0] != '\0') {
    s_fram.write(base + OFF_LABEL,
                 reinterpret_cast<uint8_t*>(const_cast<char*>(ns.label)),
                 LABEL_BYTES);
  }
  s_fram.write(base + OFF_HISTORY,
               reinterpret_cast<uint8_t*>(
               const_cast<HistoryPoint*>(ns.history)),
               HISTORY_BYTES);
}

void framSaveLabel(uint8_t nodeIdx)
{
  if (!s_framOk) return;

  const NodeState& ns = g_nodes[nodeIdx];
  if (ns.deviceUID == 0x0000 || ns.deviceUID == 0xFFFF) return;

  uint8_t framSlot = framFindSlot(ns.deviceUID);
  if (framSlot == 0xFF) framSlot = framAllocFifoSlot();

  uint16_t base = nodeBase(framSlot);
  s_fram.write16(base + OFF_UID, ns.deviceUID);
  s_fram.write(base + OFF_LABEL,
               reinterpret_cast<uint8_t*>(const_cast<char*>(ns.label)),
               LABEL_BYTES);

  logAsync("[FRAM] UID=0x%04X: saved label \"%s\" to FRAM slot %u\n",
           ns.deviceUID, ns.label, framSlot + 1);
}

// ---------------------------------------------------------------------------
// Deferred save/restore task — keeps I2C off the Core 1 TDMA task
// ---------------------------------------------------------------------------

#define FRAM_SAVE_ENERGY  0x10u
#define FRAM_SAVE_HISTORY 0x20u
#define FRAM_RESTORE      0x40u

static QueueHandle_t s_framQueue = nullptr;

// Static snapshot buffer — framTask is sequential so one buffer suffices.
// Not on the task stack: avoids a 1930-byte stack frame.
static struct {
  uint16_t     deviceUID;
  uint32_t     accumEnergy;
  int32_t      histHead;
  int32_t      histCount;
  char         label[LABEL_BYTES];
  HistoryPoint history[HISTORY_MAX_POINTS];
} s_snap;

void framQueueSave(uint8_t nodeIdx, bool saveEnergy, bool saveHistory)
{
  if (!s_framQueue) return;
  uint8_t msg = (nodeIdx & 0x0Fu)
                | (saveEnergy  ? FRAM_SAVE_ENERGY  : 0u)
                | (saveHistory ? FRAM_SAVE_HISTORY : 0u);
  xQueueSend(s_framQueue, &msg, 0);
}

void framQueueRestore(uint8_t nodeIdx)
{
  if (!s_framQueue) return;
  uint8_t msg = (nodeIdx & 0x0Fu) | FRAM_RESTORE;
  xQueueSend(s_framQueue, &msg, 0);
}

static void framTask(void* /*params*/)
{
  uint8_t msg;
  while (true) {
    if (xQueueReceive(s_framQueue, &msg, portMAX_DELAY) != pdTRUE) continue;

    uint8_t idx = msg & 0x0Fu;
    if (idx >= MAX_NODES) continue;

    // --- RESTORE ------------------------------------------------------------
    if (msg & FRAM_RESTORE) {
      uint16_t nodeUID = 0;
      char initLabel[LABEL_BYTES] = {};
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        nodeUID = g_nodes[idx].deviceUID;
        strlcpy(initLabel, g_nodes[idx].label, LABEL_BYTES);
        xSemaphoreGive(g_nodesMutex);
      }
      if (nodeUID == 0x0000 || nodeUID == 0xFFFF) continue;

      uint8_t framSlot = framFindSlot(nodeUID);

      if (framSlot == 0xFF) {
        // Novel node — stamp a FIFO slot so subsequent saves land correctly
        // without needing another UID search.
        framSlot = framAllocFifoSlot();
        uint16_t base = nodeBase(framSlot);
        s_fram.write16(base + OFF_UID, nodeUID);
        s_fram.write(base + OFF_LABEL,
                     reinterpret_cast<uint8_t*>(initLabel), LABEL_BYTES);
        s_fram.write32(base + OFF_ENERGY, 0);
        s_fram.write32(base + OFF_HIST_HEAD, 0);
        s_fram.write32(base + OFF_HIST_COUNT, 0);
        logAsync("[FRAM] UID=0x%04X: novel node, FIFO slot %u allocated\n",
                 nodeUID, framSlot + 1);
        continue;
      }

      // Known node — read snapshot without holding mutex (~47 ms at 400 kHz
      // for the history block).
      uint16_t base = nodeBase(framSlot);
      s_fram.read(base + OFF_LABEL,
                  reinterpret_cast<uint8_t*>(s_snap.label), LABEL_BYTES);
      s_snap.label[LABEL_BYTES - 1] = '\0';
      s_snap.accumEnergy = s_fram.read32(base + OFF_ENERGY);
      s_snap.histHead    = (int32_t)s_fram.read32(base + OFF_HIST_HEAD);
      s_snap.histCount   = (int32_t)s_fram.read32(base + OFF_HIST_COUNT);
      s_fram.read(base + OFF_HISTORY,
                  reinterpret_cast<uint8_t*>(s_snap.history),
                  HISTORY_BYTES);

      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_snap.label[0] != '\0') {
          strlcpy(g_nodes[idx].label, s_snap.label, sizeof(g_nodes[idx].label));
        }
        g_nodes[idx].accumEnergy = s_snap.accumEnergy;
        g_nodes[idx].histHead    = (int)s_snap.histHead;
        g_nodes[idx].histCount   = (int)s_snap.histCount;
        memcpy(g_nodes[idx].history, s_snap.history, HISTORY_BYTES);
        xSemaphoreGive(g_nodesMutex);
      }

      logAsync("[FRAM] UID=0x%04X (slot %u) restored from FRAM slot %u: "
               "label=\"%s\" energy=%lu Wh histCount=%d\n",
               nodeUID, idx + 1, framSlot + 1,
               s_snap.label[0] ? s_snap.label : "(none)",
               (unsigned long)s_snap.accumEnergy, (int)s_snap.histCount);
      continue;
    }

    // --- SAVE ENERGY --------------------------------------------------------
    if (msg & FRAM_SAVE_ENERGY) {
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_snap.deviceUID   = g_nodes[idx].deviceUID;
        s_snap.accumEnergy = g_nodes[idx].accumEnergy;
        strlcpy(s_snap.label, g_nodes[idx].label, LABEL_BYTES);
        xSemaphoreGive(g_nodesMutex);
      }
      if (s_snap.deviceUID == 0x0000 || s_snap.deviceUID == 0xFFFF) continue;

      uint8_t framSlot = framFindSlot(s_snap.deviceUID);
      if (framSlot == 0xFF) framSlot = framAllocFifoSlot();

      uint16_t base = nodeBase(framSlot);
      s_fram.write16(base + OFF_UID,    s_snap.deviceUID);
      s_fram.write32(base + OFF_ENERGY, s_snap.accumEnergy);
      // Only update the label when non-empty so a cleared label (post-eviction)
      // does not overwrite a previously saved user-assigned name.
      if (s_snap.label[0] != '\0') {
        s_fram.write(base + OFF_LABEL,
                     reinterpret_cast<uint8_t*>(s_snap.label), LABEL_BYTES);
      }
    }

    // --- SAVE HISTORY -------------------------------------------------------
    if (msg & FRAM_SAVE_HISTORY) {
      if (xSemaphoreTake(g_nodesMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_snap.deviceUID  = g_nodes[idx].deviceUID;
        s_snap.histHead   = g_nodes[idx].histHead;
        s_snap.histCount  = g_nodes[idx].histCount;
        strlcpy(s_snap.label, g_nodes[idx].label, LABEL_BYTES);
        memcpy(s_snap.history, g_nodes[idx].history, sizeof(s_snap.history));
        xSemaphoreGive(g_nodesMutex);
      }
      if (s_snap.deviceUID == 0x0000 || s_snap.deviceUID == 0xFFFF) continue;

      uint8_t framSlot = framFindSlot(s_snap.deviceUID);
      if (framSlot == 0xFF) framSlot = framAllocFifoSlot();

      uint16_t base = nodeBase(framSlot);
      s_fram.write16(base + OFF_UID,        s_snap.deviceUID);
      s_fram.write32(base + OFF_HIST_HEAD,  (uint32_t)s_snap.histHead);
      s_fram.write32(base + OFF_HIST_COUNT, (uint32_t)s_snap.histCount);
      if (s_snap.label[0] != '\0') {
        s_fram.write(base + OFF_LABEL,
                     reinterpret_cast<uint8_t*>(s_snap.label), LABEL_BYTES);
      }
      s_fram.write(base + OFF_HISTORY,
                   reinterpret_cast<uint8_t*>(s_snap.history),
                   HISTORY_BYTES);
    }
  }
}

void framTaskStart()
{
  s_framQueue = xQueueCreate(16, sizeof(uint8_t));
  configASSERT(s_framQueue);
  xTaskCreatePinnedToCore(framTask, "FRAM", 2048, nullptr, 0, nullptr, 0);
}

void framErase()
{
  if (!s_framOk) return;
  s_fram.write32(FRAM_HEADER_BASE, 0x00000000UL);
  logAsync("[FRAM] Magic erased — chip will reinitialise as blank on next boot\n");
}

#endif // NODE_GATEWAY
