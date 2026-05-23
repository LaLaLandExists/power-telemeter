/**
 * hal_crypto.h
 *
 * Platform HAL -- AES-128-CTR cipher.
 * Wraps mbedTLS on ESP32 (hardware-accelerated).
 * Swap hal/esp32/hal_crypto_esp32.cpp for another platform's AES implementation.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * AES-128-CTR in-place encrypt or decrypt (same operation).
 *
 * @param buf          Data buffer, modified in place.
 * @param len          Number of bytes to process.
 * @param key16        16-byte AES-128 key.
 * @param nonceCounter 16-byte nonce/counter block -- updated in place on return.
 *
 * The caller assembles the nonce before calling; this function does not interpret
 * the nonce structure -- it is opaque bytes to the HAL.
 */
void halAesCtr(uint8_t* buf, size_t len,
               const uint8_t key16[16],
               uint8_t nonceCounter[16]);
