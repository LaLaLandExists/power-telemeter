/**
 * hal_nvs.h
 *
 * Platform HAL -- key-value flash storage.
 * Wraps the ESP32 Arduino Preferences class (which wraps esp_nvs).
 * Swap hal/esp32/hal_nvs_esp32.cpp for another platform's KV store.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef void* HalNvs_t;

/**
 * Open a namespace for reading or writing.
 * @param ns        Namespace name (e.g. "wifi-cfg", "lora-net").
 * @param readOnly  true = read-only.
 * @return          Opaque handle, or nullptr on failure.
 */
HalNvs_t halNvsOpen(const char* ns, bool readOnly);

/** Commit and close a namespace handle. */
void halNvsClose(HalNvs_t h);

/** Read a string value. Returns true if key was found and written to buf. */
bool   halNvsGetStr(HalNvs_t h, const char* key, char* buf, size_t bufLen);

/** Write a string value. Returns true on success. */
bool   halNvsPutStr(HalNvs_t h, const char* key, const char* val);

/** Return stored byte length of a blob key, or 0 if absent. */
size_t halNvsGetBlobLen(HalNvs_t h, const char* key);

/** Read a blob. Returns bytes copied, or 0 on failure. */
size_t halNvsGetBlob(HalNvs_t h, const char* key, void* buf, size_t maxLen);

/** Write a blob. Returns true on success. */
bool   halNvsPutBlob(HalNvs_t h, const char* key, const void* data, size_t len);

/** Read a 32-bit signed int. Returns defaultVal if key is absent. */
int32_t halNvsGetInt(HalNvs_t h, const char* key, int32_t defaultVal);

/** Write a 32-bit signed int. Returns true on success. */
bool   halNvsPutInt(HalNvs_t h, const char* key, int32_t val);

/** Remove a single key. Returns true on success. */
bool   halNvsRemove(HalNvs_t h, const char* key);

/** Remove all keys in the namespace. Returns true on success. */
bool   halNvsClear(HalNvs_t h);
