/**
 * hal_nvs_stm32.cpp
 *
 * STM32F411 implementation of hal_nvs.h.
 *
 * Storage layout -- flash sector 7 (0x08060000, 128 KB):
 *
 *   [0x000] Sector header (16 bytes)
 *              magic    uint32_t  0xBEEFCAFE
 *              version  uint32_t  0x00000002
 *              pad      uint8_t[8]
 *   [0x010] Record[0]   (64 bytes each)
 *   [0x050] Record[1]
 *   ...
 *   [0x1FFEF] Record[2046]  -- sector end (128 KB - 16 B) / 64 B = 2047 slots
 *
 * Record format (64 bytes, word-aligned):
 *   status    uint8_t   0xFF=free, 0xAA=active, 0x00=deleted
 *   type      uint8_t   1=str, 2=blob, 3=int32
 *   data_len  uint16_t  byte count of data payload
 *   ns        char[12]  namespace (null-padded)
 *   key       char[12]  key (null-padded)
 *   data      uint8_t[32]  payload
 *   pad       uint8_t[4]   word-alignment pad
 *
 * Payloads on this node:
 *   lora-net/aeskey   : 16 B  (AES-128 key)
 *   afe-cal/kv,ki     :  4 B  (calibration floats)
 *   afe-cal/phcorr    :  4 B
 *   clf/thresholds    : 28 B  (ClassifierThresholds, 7 x float32)
 *   clf/mode,model_ver:  4 B  (int32)
 * Largest payload: 28 B (ClassifierThresholds).  32-byte data field
 * covers all current and anticipated keys with margin.
 *
 * Write policy: mark-deleted + append.  Compact (erase + rewrite active
 * records) when the first free slot is past NVS_COMPACT_THRESHOLD.  The
 * compact buffer holds up to NVS_COMPACT_MAX records in SRAM -- 16 * 64 B
 * = 1 KB, well within the F411's 128 KB RAM.
 *
 * NOTE: Flash sector erase stalls instruction fetch from flash for ~1-4 s
 * (STM32F4, 128 KB sector at 3.3 V).  This only occurs on first boot
 * (magic absent) or during rare compaction.  Acceptable for NVS use.
 *
 * To grow: increase NVS_DATA_MAX (must stay <= 60 for 64-byte record budget)
 * or increase NVS_COMPACT_MAX if more than 16 active keys are anticipated.
 */
#if defined(BOARD_STM32_F411)

#include "hal/hal_nvs.h"
#include <stm32f4xx_hal.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Flash region constants
// ---------------------------------------------------------------------------

#define NVS_FLASH_SECTOR    FLASH_SECTOR_7
#define NVS_BASE_ADDR       0x08060000UL
#define NVS_SECTOR_SIZE     (128UL * 1024UL)

#define NVS_MAGIC           0xBEEFCAFEUL
#define NVS_VERSION         2UL    // v2: 64-byte record (was 96-byte in v1)

#define NVS_HDR_SIZE        16U
#define NVS_REC_SIZE        64U    // sizeof(NvsRecord), asserted below
#define NVS_NS_MAX          12U
#define NVS_KEY_MAX         12U
#define NVS_DATA_MAX        32U    // 28 B ClassifierThresholds is the largest payload

#define NVS_STATUS_FREE     0xFFU
#define NVS_STATUS_ACTIVE   0xAAU
#define NVS_STATUS_DEAD     0x00U

#define NVS_TYPE_STR        1U
#define NVS_TYPE_BLOB       2U
#define NVS_TYPE_INT        3U

// Compact when slot index exceeds this (7 active keys x headroom; 2047 total slots).
#define NVS_COMPACT_THRESHOLD  128U
// Max active records preserved during compaction (SRAM = 16 * 64 = 1 KB).
// 7 anticipated keys; 16 gives 2x headroom for future growth.
#define NVS_COMPACT_MAX        16U

// ---------------------------------------------------------------------------
// Record struct
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct NvsRecord {
  uint8_t  status;           // 1
  uint8_t  type;             // 1
  uint16_t data_len;         // 2
  char     ns[NVS_NS_MAX];   // 12
  char     key[NVS_KEY_MAX]; // 12
  uint8_t  data[NVS_DATA_MAX]; // 32
  uint8_t  pad[4];           // 4 -- word-align to 64 bytes
};
#pragma pack(pop)

static_assert(sizeof(NvsRecord) == NVS_REC_SIZE, "NvsRecord size mismatch");

#pragma pack(push, 1)
struct NvsHdr {
  uint32_t magic;
  uint32_t version;
  uint8_t  pad[8];
};
#pragma pack(pop)

static_assert(sizeof(NvsHdr) == NVS_HDR_SIZE, "NvsHdr size mismatch");

// ---------------------------------------------------------------------------
// Flash helpers
// ---------------------------------------------------------------------------

static bool flashWriteWords(uint32_t addr, const void* src, size_t len) {
  // len must be a multiple of 4 (caller's responsibility)
  HAL_FLASH_Unlock();
  const uint32_t* p = (const uint32_t*)src;
  bool ok = true;
  for (size_t i = 0; i < len / 4U; i++) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i * 4U, (uint64_t)p[i]) != HAL_OK) {
      ok = false;
      break;
    }
  }
  HAL_FLASH_Lock();
  return ok;
}

static bool flashEraseSector7() {
  FLASH_EraseInitTypeDef er = {};
  er.TypeErase    = FLASH_TYPEERASE_SECTORS;
  er.Sector       = NVS_FLASH_SECTOR;
  er.NbSectors    = 1;
  er.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  uint32_t err = 0;
  HAL_FLASH_Unlock();
  bool ok = (HAL_FLASHEx_Erase(&er, &err) == HAL_OK);
  HAL_FLASH_Lock();
  return ok;
}

// ---------------------------------------------------------------------------
// Sector / record accessors (read-only; flash mapped to CPU address space)
// ---------------------------------------------------------------------------

static inline const NvsHdr* nvsSectorHdr() {
  return (const NvsHdr*)NVS_BASE_ADDR;
}

static inline const NvsRecord* nvsRecord(uint32_t idx) {
  return (const NvsRecord*)(NVS_BASE_ADDR + NVS_HDR_SIZE + idx * NVS_REC_SIZE);
}

static inline uint32_t nvsMaxRecords() {
  return (NVS_SECTOR_SIZE - NVS_HDR_SIZE) / NVS_REC_SIZE;
}

// ---------------------------------------------------------------------------
// Sector initialisation
// ---------------------------------------------------------------------------

static bool nvsEnsureInit() {
  const NvsHdr* h = nvsSectorHdr();
  if (h->magic == NVS_MAGIC && h->version == NVS_VERSION) return true;
  // First boot or version mismatch: erase and write fresh header.
  if (!flashEraseSector7()) return false;
  NvsHdr init = { NVS_MAGIC, NVS_VERSION, {} };
  return flashWriteWords(NVS_BASE_ADDR, &init, NVS_HDR_SIZE);
}

// ---------------------------------------------------------------------------
// Record search (linear scan stops at first free slot)
// ---------------------------------------------------------------------------

// Returns slot index of the most-recently-written active record for ns/key,
// or -1 if not found.
static int nvsFindRecord(const char* ns, const char* key) {
  uint32_t max = nvsMaxRecords();
  int found = -1;
  for (uint32_t i = 0; i < max; i++) {
    const NvsRecord* r = nvsRecord(i);
    if (r->status == NVS_STATUS_FREE) break;
    if (r->status != NVS_STATUS_ACTIVE) continue;
    if (strncmp(r->ns,  ns,  NVS_NS_MAX)  == 0 &&
        strncmp(r->key, key, NVS_KEY_MAX) == 0) {
      found = (int)i;  // keep going -- last write wins
    }
  }
  return found;
}

// Returns the index of the next free slot, or nvsMaxRecords() if full.
static uint32_t nvsFreeSlot() {
  uint32_t max = nvsMaxRecords();
  for (uint32_t i = 0; i < max; i++) {
    if (nvsRecord(i)->status == NVS_STATUS_FREE) return i;
  }
  return max;
}

// ---------------------------------------------------------------------------
// Compaction -- erase sector and rewrite all active records.
// Uses a SRAM buffer (NVS_COMPACT_MAX records).
// ---------------------------------------------------------------------------

static void nvsCompact() {
  static NvsRecord buf[NVS_COMPACT_MAX];
  uint8_t count = 0;

  uint32_t max = nvsMaxRecords();
  for (uint32_t i = 0; i < max && count < NVS_COMPACT_MAX; i++) {
    const NvsRecord* r = nvsRecord(i);
    if (r->status == NVS_STATUS_FREE) break;
    if (r->status == NVS_STATUS_ACTIVE) {
      buf[count++] = *r;
    }
  }

  flashEraseSector7();
  NvsHdr h = { NVS_MAGIC, NVS_VERSION, {} };
  flashWriteWords(NVS_BASE_ADDR, &h, NVS_HDR_SIZE);

  for (uint8_t i = 0; i < count; i++) {
    uint32_t addr = NVS_BASE_ADDR + NVS_HDR_SIZE + (uint32_t)i * NVS_REC_SIZE;
    flashWriteWords(addr, &buf[i], NVS_REC_SIZE);
  }
}

// ---------------------------------------------------------------------------
// Record append (with optional compaction)
// ---------------------------------------------------------------------------

static bool nvsAppend(const char* ns, const char* key,
                       uint8_t type, const void* data, uint16_t data_len) {
  if (data_len > NVS_DATA_MAX) return false;

  uint32_t slot = nvsFreeSlot();
  if (slot >= NVS_COMPACT_THRESHOLD) {
    nvsCompact();
    slot = nvsFreeSlot();
    if (slot >= nvsMaxRecords()) return false;  // compact failed somehow
  }

  NvsRecord rec = {};
  rec.status   = NVS_STATUS_ACTIVE;
  rec.type     = type;
  rec.data_len = data_len;
  strncpy(rec.ns,  ns,  NVS_NS_MAX  - 1);
  strncpy(rec.key, key, NVS_KEY_MAX - 1);
  memcpy(rec.data, data, data_len);

  uint32_t addr = NVS_BASE_ADDR + NVS_HDR_SIZE + slot * NVS_REC_SIZE;
  return flashWriteWords(addr, &rec, NVS_REC_SIZE);
}

// ---------------------------------------------------------------------------
// Mark a slot deleted by clearing its status byte to 0x00.
// STM32 flash can only clear bits (1->0), so writing 0x00 to a byte that
// was 0xAA (status ACTIVE) is always valid.
// The status byte is at offset 0 in the record; we rewrite the entire first
// 4-byte word (status + type + data_len) with status forced to 0x00.
// ---------------------------------------------------------------------------

static void nvsMarkDeleted(int idx) {
  const NvsRecord* r = nvsRecord(idx);
  uint32_t addr = NVS_BASE_ADDR + NVS_HDR_SIZE + (uint32_t)idx * NVS_REC_SIZE;
  // First word: status(0x00) | type<<8 | data_len(LE) packed as little-endian uint32
  uint32_t word = (uint32_t)NVS_STATUS_DEAD
                | ((uint32_t)r->type     <<  8)
                | ((uint32_t)(r->data_len & 0xFFU) << 16)
                | ((uint32_t)(r->data_len >> 8)    << 24);
  HAL_FLASH_Unlock();
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, (uint64_t)word);
  HAL_FLASH_Lock();
}

// ---------------------------------------------------------------------------
// Namespace handle pool
// ---------------------------------------------------------------------------

#define NS_POOL_SIZE 4

static char s_nsPool[NS_POOL_SIZE][NVS_NS_MAX + 1] = {};
static bool s_nsUsed[NS_POOL_SIZE] = {};

HalNvs_t halNvsOpen(const char* ns, bool readOnly) {
  (void)readOnly;
  if (!nvsEnsureInit()) return nullptr;
  for (int i = 0; i < NS_POOL_SIZE; i++) {
    if (!s_nsUsed[i]) {
      s_nsUsed[i] = true;
      strncpy(s_nsPool[i], ns, NVS_NS_MAX);
      s_nsPool[i][NVS_NS_MAX] = '\0';
      return (HalNvs_t)(uintptr_t)(i + 1);  // 1-based index as opaque handle
    }
  }
  return nullptr;  // pool exhausted
}

void halNvsClose(HalNvs_t h) {
  if (!h) return;
  int i = (int)(uintptr_t)h - 1;
  if (i >= 0 && i < NS_POOL_SIZE) s_nsUsed[i] = false;
}

static const char* nsFromHandle(HalNvs_t h) {
  if (!h) return nullptr;
  int i = (int)(uintptr_t)h - 1;
  if (i < 0 || i >= NS_POOL_SIZE || !s_nsUsed[i]) return nullptr;
  return s_nsPool[i];
}

// ---------------------------------------------------------------------------
// HAL read/write implementations
// ---------------------------------------------------------------------------

bool halNvsGetStr(HalNvs_t h, const char* key, char* buf, size_t bufLen) {
  const char* ns = nsFromHandle(h);
  if (!ns || !buf || bufLen == 0) return false;
  int idx = nvsFindRecord(ns, key);
  if (idx < 0) return false;
  const NvsRecord* r = nvsRecord((uint32_t)idx);
  if (r->type != NVS_TYPE_STR) return false;
  size_t copy = r->data_len < bufLen ? r->data_len : bufLen - 1;
  memcpy(buf, r->data, copy);
  buf[copy] = '\0';
  return true;
}

bool halNvsPutStr(HalNvs_t h, const char* key, const char* val) {
  const char* ns = nsFromHandle(h);
  if (!ns || !val) return false;
  int old = nvsFindRecord(ns, key);
  uint16_t len = (uint16_t)strlen(val);
  if (len > NVS_DATA_MAX) len = NVS_DATA_MAX;
  bool ok = nvsAppend(ns, key, NVS_TYPE_STR, val, len);
  if (ok && old >= 0) nvsMarkDeleted(old);
  return ok;
}

size_t halNvsGetBlobLen(HalNvs_t h, const char* key) {
  const char* ns = nsFromHandle(h);
  if (!ns) return 0;
  int idx = nvsFindRecord(ns, key);
  if (idx < 0) return 0;
  const NvsRecord* r = nvsRecord((uint32_t)idx);
  return (r->type == NVS_TYPE_BLOB) ? r->data_len : 0;
}

size_t halNvsGetBlob(HalNvs_t h, const char* key, void* buf, size_t maxLen) {
  const char* ns = nsFromHandle(h);
  if (!ns || !buf) return 0;
  int idx = nvsFindRecord(ns, key);
  if (idx < 0) return 0;
  const NvsRecord* r = nvsRecord((uint32_t)idx);
  if (r->type != NVS_TYPE_BLOB) return 0;
  size_t copy = r->data_len < (uint16_t)maxLen ? r->data_len : (uint16_t)maxLen;
  memcpy(buf, r->data, copy);
  return copy;
}

bool halNvsPutBlob(HalNvs_t h, const char* key, const void* data, size_t len) {
  const char* ns = nsFromHandle(h);
  if (!ns || !data || len == 0 || len > NVS_DATA_MAX) return false;
  int old = nvsFindRecord(ns, key);
  bool ok = nvsAppend(ns, key, NVS_TYPE_BLOB, data, (uint16_t)len);
  if (ok && old >= 0) nvsMarkDeleted(old);
  return ok;
}

int32_t halNvsGetInt(HalNvs_t h, const char* key, int32_t defaultVal) {
  const char* ns = nsFromHandle(h);
  if (!ns) return defaultVal;
  int idx = nvsFindRecord(ns, key);
  if (idx < 0) return defaultVal;
  const NvsRecord* r = nvsRecord((uint32_t)idx);
  if (r->type != NVS_TYPE_INT || r->data_len < 4U) return defaultVal;
  int32_t v;
  memcpy(&v, r->data, 4);
  return v;
}

bool halNvsPutInt(HalNvs_t h, const char* key, int32_t val) {
  const char* ns = nsFromHandle(h);
  if (!ns) return false;
  int old = nvsFindRecord(ns, key);
  bool ok = nvsAppend(ns, key, NVS_TYPE_INT, &val, 4U);
  if (ok && old >= 0) nvsMarkDeleted(old);
  return ok;
}

bool halNvsRemove(HalNvs_t h, const char* key) {
  const char* ns = nsFromHandle(h);
  if (!ns) return false;
  int idx = nvsFindRecord(ns, key);
  if (idx < 0) return false;
  nvsMarkDeleted(idx);
  return true;
}

bool halNvsClear(HalNvs_t h) {
  if (!nsFromHandle(h)) return false;
  if (!flashEraseSector7()) return false;
  NvsHdr init = { NVS_MAGIC, NVS_VERSION, {} };
  return flashWriteWords(NVS_BASE_ADDR, &init, NVS_HDR_SIZE);
}

#endif // BOARD_STM32_F411
