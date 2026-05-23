/**
 * hal_nvs_esp32.cpp
 *
 * ESP32 implementation of hal_nvs.h using the Arduino Preferences class.
 * This is the only file that includes <Preferences.h>.
 *
 * A static pool of 4 Preferences objects avoids dynamic allocation.
 * The codebase never opens more than 2 namespaces simultaneously.
 */
#include "hal/hal_nvs.h"
#include <Preferences.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Static object pool (no heap allocation)
// ---------------------------------------------------------------------------

#define NVS_POOL_SIZE 4

static Preferences s_pool[NVS_POOL_SIZE];
static bool        s_poolUsed[NVS_POOL_SIZE] = {};

static Preferences* poolAlloc() {
  for (int i = 0; i < NVS_POOL_SIZE; i++) {
    if (!s_poolUsed[i]) {
      s_poolUsed[i] = true;
      return &s_pool[i];
    }
  }
  return nullptr;  // pool exhausted
}

static void poolFree(Preferences* p) {
  for (int i = 0; i < NVS_POOL_SIZE; i++) {
    if (&s_pool[i] == p) {
      s_poolUsed[i] = false;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// HAL implementation
// ---------------------------------------------------------------------------

HalNvs_t halNvsOpen(const char* ns, bool readOnly) {
  Preferences* p = poolAlloc();
  if (!p) return nullptr;
  if (!p->begin(ns, readOnly)) {
    poolFree(p);
    return nullptr;
  }
  return (HalNvs_t)p;
}

void halNvsClose(HalNvs_t h) {
  if (!h) return;
  Preferences* p = (Preferences*)h;
  p->end();
  poolFree(p);
}

bool halNvsGetStr(HalNvs_t h, const char* key, char* buf, size_t bufLen) {
  if (!h || !buf || bufLen == 0) return false;
  Preferences* p = (Preferences*)h;
  if (!p->isKey(key)) return false;
  String s = p->getString(key, "");
  strncpy(buf, s.c_str(), bufLen - 1);
  buf[bufLen - 1] = '\0';
  return true;
}

bool halNvsPutStr(HalNvs_t h, const char* key, const char* val) {
  if (!h) return false;
  return ((Preferences*)h)->putString(key, val) > 0;
}

size_t halNvsGetBlobLen(HalNvs_t h, const char* key) {
  if (!h) return 0;
  return ((Preferences*)h)->getBytesLength(key);
}

size_t halNvsGetBlob(HalNvs_t h, const char* key, void* buf, size_t maxLen) {
  if (!h) return 0;
  return ((Preferences*)h)->getBytes(key, buf, maxLen);
}

bool halNvsPutBlob(HalNvs_t h, const char* key, const void* data, size_t len) {
  if (!h) return false;
  return ((Preferences*)h)->putBytes(key, data, len) == len;
}

int32_t halNvsGetInt(HalNvs_t h, const char* key, int32_t defaultVal) {
  if (!h) return defaultVal;
  return ((Preferences*)h)->getInt(key, defaultVal);
}

bool halNvsPutInt(HalNvs_t h, const char* key, int32_t val) {
  if (!h) return false;
  return ((Preferences*)h)->putInt(key, val) > 0;
}

bool halNvsRemove(HalNvs_t h, const char* key) {
  if (!h) return false;
  return ((Preferences*)h)->remove(key);
}

bool halNvsClear(HalNvs_t h) {
  if (!h) return false;
  return ((Preferences*)h)->clear();
}
