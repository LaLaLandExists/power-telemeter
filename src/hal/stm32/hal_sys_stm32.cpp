/**
 * hal_sys_stm32.cpp
 *
 * STM32F411 implementation of hal_sys.h.
 * Wrapped in BOARD_STM32_F411 guard so this file compiles as an empty
 * translation unit on ESP32 builds (which also scan src/hal/stm32/).
 */
#if defined(BOARD_STM32_F411)

#include "hal/hal_sys.h"
#include <stdint.h>
#include <string.h>
#include <Arduino.h>
#include <STM32FreeRTOS.h>

// ---------------------------------------------------------------------------
// Reboot
// ---------------------------------------------------------------------------

void halReboot(void) {
  NVIC_SystemReset();
}

// ---------------------------------------------------------------------------
// Random -- UID-seeded xorshift32 PRNG.
// STM32F411 has no hardware TRNG.  The 96-bit unique device ID at 0x1FFF7A10
// guarantees a chip-unique seed.  micros() stirs in runtime entropy on first
// call so two devices booted simultaneously still diverge quickly.
// Not cryptographically random; adequate for TDMA contention backoff.
// ---------------------------------------------------------------------------

static uint32_t s_rngState = 0;

static void rngSeedOnce() {
  if (s_rngState != 0) return;
  uint32_t u0 = *((const volatile uint32_t*)0x1FFF7A10U);
  uint32_t u1 = *((const volatile uint32_t*)0x1FFF7A14U);
  uint32_t u2 = *((const volatile uint32_t*)0x1FFF7A18U);
  s_rngState = u0 ^ (u1 * 1000003U) ^ (u2 * 999983U) ^ (uint32_t)micros();
  if (s_rngState == 0) s_rngState = 0xDEADBEEFU;
}

static uint32_t xorshift32() {
  rngSeedOnce();
  s_rngState ^= s_rngState << 13;
  s_rngState ^= s_rngState >> 17;
  s_rngState ^= s_rngState << 5;
  return s_rngState;
}

uint32_t halRandom(void) {
  return xorshift32();
}

void halFillRandom(void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  while (len >= 4) {
    uint32_t r = xorshift32();
    memcpy(p, &r, 4);
    p   += 4;
    len -= 4;
  }
  if (len > 0) {
    uint32_t r = xorshift32();
    memcpy(p, &r, len);
  }
}

// ---------------------------------------------------------------------------
// Free heap -- delegate to FreeRTOS allocator
// ---------------------------------------------------------------------------

uint32_t halFreeHeap(void) {
  return (uint32_t)xPortGetFreeHeapSize();
}

// ---------------------------------------------------------------------------
// MAC -- derived from the STM32 96-bit unique device ID.
// computeDeviceUID() in tdma_protocol.h hashes these 6 bytes into a uint16_t
// node UID, so chip-uniqueness of the UID is sufficient.
// ---------------------------------------------------------------------------

void halGetMac(uint8_t mac[6]) {
  const volatile uint32_t* uid = (const volatile uint32_t*)0x1FFF7A10U;
  uint32_t u0 = uid[0], u1 = uid[1], u2 = uid[2];
  mac[0] = (uint8_t)(u0 >>  0);
  mac[1] = (uint8_t)(u0 >> 16);
  mac[2] = (uint8_t)(u1 >>  0);
  mac[3] = (uint8_t)(u1 >> 16);
  mac[4] = (uint8_t)(u2 >>  0);
  mac[5] = (uint8_t)(u2 >> 16);
}

#endif // BOARD_STM32_F411
