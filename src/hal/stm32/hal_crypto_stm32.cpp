/**
 * hal_crypto_stm32.cpp
 *
 * STM32F411 implementation of hal_crypto.h.
 *
 * STM32F411 has no hardware AES engine (the crypto extension is present only
 * on STM32F4x7/F479).  This file provides a compact software AES-128-CTR
 * implementation derived from the FIPS-197 reference (public domain).
 *
 * The CTR counter increment matches mbedTLS mbedtls_aes_crypt_ctr() exactly:
 * the 16-byte nonce is treated as a 128-bit big-endian counter, incremented
 * from byte 15 downward.  This is compatible with the encryption nonce scheme
 * in pkt_crypto.h.
 *
 * Performance: at 168 MHz a single 32-byte packet takes well under 1 ms --
 * negligible for the TDMA slot timing budget.
 */
#if defined(BOARD_STM32_F411)

#include "hal/hal_crypto.h"
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// AES-128 software core -- FIPS-197 column-major state
// ---------------------------------------------------------------------------

// AES forward S-Box
static const uint8_t s_sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// AES round constants (indices 1-10 used by key expansion)
static const uint8_t s_rcon[11] = {
  0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

#define AES_NR  10   // rounds for AES-128
#define AES_NK   4   // 32-bit words in key

// GF(2^8) multiplication with irreducible poly 0x11B
static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1U) p ^= a;
    uint8_t hi = a & 0x80U;
    a = (uint8_t)(a << 1);
    if (hi) a ^= 0x1bU;
    b >>= 1;
  }
  return p;
}

// Expand 16-byte key into 176-byte round-key schedule (rk[])
static void aesKeyExpansion(const uint8_t key[16], uint8_t rk[176]) {
  memcpy(rk, key, 16);
  for (int i = AES_NK; i < 4 * (AES_NR + 1); i++) {
    uint8_t tmp[4];
    memcpy(tmp, rk + (i - 1) * 4, 4);
    if (i % AES_NK == 0) {
      // RotWord: rotate left by 1 byte
      uint8_t t = tmp[0];
      tmp[0] = s_sbox[tmp[1]] ^ s_rcon[i / AES_NK];
      tmp[1] = s_sbox[tmp[2]];
      tmp[2] = s_sbox[tmp[3]];
      tmp[3] = s_sbox[t];
    }
    for (int j = 0; j < 4; j++) {
      rk[i * 4 + j] = rk[(i - AES_NK) * 4 + j] ^ tmp[j];
    }
  }
}

// AES-128 encrypt one 16-byte block (in-place into out[])
static void aesEncryptBlock(const uint8_t in[16], uint8_t out[16],
                             const uint8_t rk[176]) {
  uint8_t s[16];
  uint8_t t;  // scratch for ShiftRows -- declared here to span all rounds
  memcpy(s, in, 16);

  // AddRoundKey -- initial round
  for (int i = 0; i < 16; i++) s[i] ^= rk[i];

  for (int round = 1; round < AES_NR; round++) {
    // SubBytes
    for (int i = 0; i < 16; i++) s[i] = s_sbox[s[i]];

    // ShiftRows (column-major state)
    t = s[1]; s[1] = s[5]; s[5] = s[9];  s[9]  = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;

    // MixColumns
    for (int c = 0; c < 4; c++) {
      uint8_t* col = s + c * 4;
      uint8_t a = col[0], b = col[1], cc = col[2], d = col[3];
      col[0] = gmul(a,2) ^ gmul(b,3) ^ cc        ^ d;
      col[1] = a         ^ gmul(b,2) ^ gmul(cc,3) ^ d;
      col[2] = a         ^ b         ^ gmul(cc,2) ^ gmul(d,3);
      col[3] = gmul(a,3) ^ b         ^ cc         ^ gmul(d,2);
    }

    // AddRoundKey
    for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
  }

  // Final round (no MixColumns)
  for (int i = 0; i < 16; i++) s[i] = s_sbox[s[i]];
  t = s[1]; s[1] = s[5]; s[5] = s[9];  s[9]  = s[13]; s[13] = t;
  t = s[2]; s[2] = s[10]; s[10] = t;
  t = s[6]; s[6] = s[14]; s[14] = t;
  t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
  for (int i = 0; i < 16; i++) s[i] ^= rk[AES_NR * 16 + i];

  memcpy(out, s, 16);
}

// ---------------------------------------------------------------------------
// 128-bit big-endian counter increment (matches mbedtls_aes_crypt_ctr)
// ---------------------------------------------------------------------------

static void ctrIncrement(uint8_t ctr[16]) {
  for (int j = 15; j >= 0; j--) {
    if (++ctr[j] != 0) break;
  }
}

// ---------------------------------------------------------------------------
// HAL entry point
// ---------------------------------------------------------------------------

void halAesCtr(uint8_t* buf, size_t len,
               const uint8_t key16[16],
               uint8_t nonceCounter[16]) {
  uint8_t rk[176];
  aesKeyExpansion(key16, rk);

  uint8_t keystream[16];
  size_t  nc_off = 0;

  for (size_t i = 0; i < len; i++) {
    if (nc_off == 0) {
      aesEncryptBlock(nonceCounter, keystream, rk);
      ctrIncrement(nonceCounter);
    }
    buf[i] ^= keystream[nc_off];
    nc_off = (nc_off + 1U) & 0x0FU;  // mod 16 without division
  }
}

#endif // BOARD_STM32_F411
