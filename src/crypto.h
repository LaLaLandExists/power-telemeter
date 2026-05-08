/**
 * crypto.h
 *
 * AES-128 CTR encryption/decryption for packets.
 * Compiled only when -D PKT_ENCRYPTION is set.
 *
 * Key storage: NVS namespace "lora-net", key "aeskey" (16-byte blob).
 * Gateway: cryptoLoadKey() → if false → cryptoGenerateKey() → rfidProvisionWrite().
 * Node:    cryptoLoadKey() → if false → rfidProvisionRead() → cryptoSetKey() → reboot.
 */
#pragma once
#ifdef PKT_ENCRYPTION

#include <stdint.h>
#include <stddef.h>

/** Load 16-byte key from NVS. Returns false if not yet stored. */
bool cryptoLoadKey();

/** True if a key is resident in RAM (loaded or set this session). */
bool cryptoHasKey();

/** Store key in RAM and persist to NVS. Called by rfid_provision after card read. */
void cryptoSetKey(const uint8_t* k16);

/** Generate a random key via ESP32 hardware RNG, store in RAM and NVS. Gateway only. */
void cryptoGenerateKey();

/** Return pointer to the in-RAM key (16 bytes). Valid only when cryptoHasKey() is true. */
const uint8_t* cryptoGetKey();

/**
 * AES-128 CTR encrypt / decrypt (same operation) in-place.
 * Nonce: [sfCount_lo | sfCount_hi | slotId | dir | 0x00×12]
 * No-op if cryptoHasKey() is false.
 */
void cryptoProcess(uint8_t* buf, size_t len,
                   uint16_t sfCount, uint8_t slotId, uint8_t dir);

/** Erase key from RAM and remove from NVS. Node will require re-provisioning on next boot. */
void cryptoClearKey();

#endif // PKT_ENCRYPTION
