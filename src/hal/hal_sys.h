/**
 * hal_sys.h
 *
 * Platform HAL -- system primitives.
 * Wraps ESP32-exclusive APIs (ESP.restart, esp_random, esp_efuse_mac_get_default, etc.)
 * behind portable names. Swap hal/esp32/hal_sys_esp32.cpp for a new platform impl.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/** Immediately reset the SoC. Does not return. */
void halReboot(void);

/** Return a 32-bit hardware-random number. */
uint32_t halRandom(void);

/** Fill buf with len bytes of hardware-random data. */
void halFillRandom(void* buf, size_t len);

/** Return available heap in bytes. */
uint32_t halFreeHeap(void);

/**
 * Copy the 6-byte factory MAC address into mac[6].
 * On ESP32 this reads the eFUSE base MAC (the same one used to derive the node UID).
 */
void halGetMac(uint8_t mac[6]);
