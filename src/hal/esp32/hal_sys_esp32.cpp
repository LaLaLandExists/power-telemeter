/**
 * hal_sys_esp32.cpp
 *
 * ESP32 implementation of hal_sys.h.
 * This is the only file that includes ESP32-exclusive system headers.
 */
#include "hal/hal_sys.h"
#include <Arduino.h>
#include <esp_random.h>
#include <esp_efuse.h>
#include <string.h>

void halReboot(void) {
  ESP.restart();
}

uint32_t halRandom(void) {
  return esp_random();
}

void halFillRandom(void* buf, size_t len) {
  esp_fill_random(buf, len);
}

uint32_t halFreeHeap(void) {
  return (uint32_t)ESP.getFreeHeap();
}

void halGetMac(uint8_t mac[6]) {
  esp_efuse_mac_get_default(mac);
}
