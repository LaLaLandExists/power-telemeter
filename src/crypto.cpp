/**
 * crypto.cpp
 *
 * AES-128 CTR implementation using ESP32 hardware-accelerated mbedTLS.
 * Compiled only when -D PKT_ENCRYPTION is set.
 */
#ifdef PKT_ENCRYPTION

#include "crypto.h"
#include "log_async.h"
#include "hal/hal_sys.h"
#include "hal/hal_nvs.h"
#include "hal/hal_crypto.h"
#include <string.h>

static uint8_t s_key[16] = {};
static bool    s_hasKey  = false;

static const char* NVS_NS  = "lora-net";
static const char* NVS_KEY = "aeskey";

bool cryptoLoadKey() {
  HalNvs_t h = halNvsOpen(NVS_NS, true);
  if (!h) return false;
  size_t len = halNvsGetBlobLen(h, NVS_KEY);
  bool ok = (len == 16);
  if (ok) {
    halNvsGetBlob(h, NVS_KEY, s_key, 16);
    s_hasKey = true;
    logAsync("[CRYPTO] Key loaded from NVS\n");
  }
  halNvsClose(h);
  return ok;
}

bool cryptoHasKey() {
  return s_hasKey;
}

void cryptoSetKey(const uint8_t* k16) {
  memcpy(s_key, k16, 16);
  s_hasKey = true;
  HalNvs_t h = halNvsOpen(NVS_NS, false);
  if (h) {
    halNvsPutBlob(h, NVS_KEY, s_key, 16);
    halNvsClose(h);
  }
  logAsync("[CRYPTO] Key stored in NVS\n");
}

void cryptoGenerateKey() {
  halFillRandom(s_key, 16);
  s_hasKey = true;
  HalNvs_t h = halNvsOpen(NVS_NS, false);
  if (h) {
    halNvsPutBlob(h, NVS_KEY, s_key, 16);
    halNvsClose(h);
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
  HalNvs_t h = halNvsOpen(NVS_NS, false);
  if (h) {
    halNvsRemove(h, NVS_KEY);
    halNvsClose(h);
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

  halAesCtr(buf, len, s_key, nonce_counter);
}

#endif // PKT_ENCRYPTION
