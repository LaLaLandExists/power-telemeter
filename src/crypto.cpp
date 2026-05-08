/**
 * crypto.cpp
 *
 * AES-128 CTR implementation using ESP32 hardware-accelerated mbedTLS.
 * Compiled only when -D PKT_ENCRYPTION is set.
 */
#ifdef PKT_ENCRYPTION

#include "crypto.h"
#include "log_async.h"
#include <Preferences.h>
#include <esp_random.h>
#include "mbedtls/aes.h"
#include <string.h>

static uint8_t s_key[16] = {};
static bool    s_hasKey  = false;

static const char* NVS_NS  = "lora-net";
static const char* NVS_KEY = "aeskey";

bool cryptoLoadKey() {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  size_t len = prefs.getBytesLength(NVS_KEY);
  bool ok = (len == 16);
  if (ok) {
    prefs.getBytes(NVS_KEY, s_key, 16);
    s_hasKey = true;
    logAsync("[CRYPTO] Key loaded from NVS\n");
  }
  prefs.end();
  return ok;
}

bool cryptoHasKey() {
  return s_hasKey;
}

void cryptoSetKey(const uint8_t* k16) {
  memcpy(s_key, k16, 16);
  s_hasKey = true;
  Preferences prefs;
  if (prefs.begin(NVS_NS, false)) {
    prefs.putBytes(NVS_KEY, s_key, 16);
    prefs.end();
  }
  logAsync("[CRYPTO] Key stored in NVS\n");
}

void cryptoGenerateKey() {
  esp_fill_random(s_key, 16);
  s_hasKey = true;
  Preferences prefs;
  if (prefs.begin(NVS_NS, false)) {
    prefs.putBytes(NVS_KEY, s_key, 16);
    prefs.end();
  }
  logAsync("[CRYPTO] New key generated: ");
  for (int i = 0; i < 16; i++) logAsync("%02X", s_key[i]);
  logAsync("\n");
}

const uint8_t* cryptoGetKey() {
  return s_key;
}

void cryptoClearKey() {
  memset(s_key, 0, 16);
  s_hasKey = false;
  Preferences prefs;
  if (prefs.begin(NVS_NS, false)) {
    prefs.remove(NVS_KEY);
    prefs.end();
  }
  logAsync("[CRYPTO] Key cleared from NVS\n");
}

void cryptoProcess(uint8_t* buf, size_t len,
                   uint16_t sfCount, uint8_t slotId, uint8_t dir) {
  if (!s_hasKey || len == 0) return;

  uint8_t nonce_counter[16] = {};
  nonce_counter[0] = (uint8_t)(sfCount & 0xFF);
  nonce_counter[1] = (uint8_t)(sfCount >> 8);
  nonce_counter[2] = slotId;
  nonce_counter[3] = dir;

  uint8_t stream_block[16] = {};
  size_t  nc_off = 0;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, s_key, 128);
  mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce_counter, stream_block, buf, buf);
  mbedtls_aes_free(&ctx);
}

#endif // PKT_ENCRYPTION
