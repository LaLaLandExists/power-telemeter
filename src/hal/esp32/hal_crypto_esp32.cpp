/**
 * hal_crypto_esp32.cpp
 *
 * ESP32 implementation of hal_crypto.h using mbedTLS AES-128-CTR.
 * This is the only file that includes mbedtls/aes.h.
 * On ESP32 mbedTLS uses the hardware AES accelerator automatically.
 */
#include "hal/hal_crypto.h"
#include "mbedtls/aes.h"
#include <string.h>

void halAesCtr(uint8_t* buf, size_t len,
               const uint8_t key16[16],
               uint8_t nonceCounter[16]) {
  uint8_t stream_block[16] = {};
  size_t  nc_off = 0;
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key16, 128);
  mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonceCounter, stream_block, buf, buf);
  mbedtls_aes_free(&ctx);
}
