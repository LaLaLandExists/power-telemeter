/**
 * pkt_crypto.h
 *
 * Compile-time transparent crypto layer for packet processing.
 *
 * With -D PKT_ENCRYPTION:
 *   pktEncrypt() AES-128 CTR encrypts the buffer in-place before TX.
 *   pktDecrypt() AES-128 CTR decrypts the buffer in-place after RX.
 *   pktReceive<T>() decrypts then casts the received bytes into a typed struct.
 *
 * Without PKT_ENCRYPTION (plain builds):
 *   pktEncrypt() and pktDecrypt() compile to empty inline no-ops.
 *   pktReceive<T>() is a plain memcpy cast — zero overhead.
 *
 * Nonce construction (16 bytes, never transmitted):
 *   [sfCount_lo | sfCount_hi | slotId | dir | 0x00×12]
 *
 * Direction constants (also used as nonce[3]):
 *   PKT_DIR_BEACON  0x04  — beacon: sfCount is plaintext in bytes 0-1; bytes 2-7 encrypted
 *                           with nonce (sfCount, slotId=0, PKT_DIR_BEACON); use pktReceiveBeacon()
 *   PKT_DIR_UL      0x01  — node → gateway telemetry
 *   PKT_DIR_DL      0x02  — gateway → node command (fixed dir avoids circular pktType dependency)
 *   PKT_DIR_JOIN_RQ 0xA0  — node → gateway join request
 *   PKT_DIR_JOIN_AK 0xA1  — gateway → node join ack
 */
#pragma once
#include <Arduino.h>
#include <string.h>

// --- Direction discriminators (used as nonce[3]) ----------------------------
#define PKT_DIR_BEACON   0x04
#define PKT_DIR_UL       0x01
#define PKT_DIR_DL       0x02
#define PKT_DIR_JOIN_RQ  0xA0
#define PKT_DIR_JOIN_AK  0xA1

#ifdef PKT_ENCRYPTION
  #include "crypto.h"

  inline void pktEncrypt(uint8_t* buf, size_t len,
                         uint16_t sfCount, uint8_t slotId, uint8_t dir) {
    cryptoProcess(buf, len, sfCount, slotId, dir);
  }

  inline void pktDecrypt(uint8_t* buf, size_t len,
                         uint16_t sfCount, uint8_t slotId, uint8_t dir) {
    cryptoProcess(buf, len, sfCount, slotId, dir);
  }
#else
  inline void pktEncrypt(uint8_t*, size_t, uint16_t, uint8_t, uint8_t) {}
  inline void pktDecrypt(uint8_t*, size_t, uint16_t, uint8_t, uint8_t) {}
#endif

/**
 * Receive helper — copies raw received bytes into a typed packet struct,
 * decrypting in-place when PKT_ENCRYPTION is defined.
 *
 * Returns false if rxLen < sizeof(T) (packet too short to be type T).
 * In encrypted builds the pktType field inside out is only trustworthy AFTER
 * this call — check it after calling, not before.
 */
template <typename T>
inline bool pktReceive(const uint8_t* raw, size_t rxLen, T& out,
                       uint16_t sfCount, uint8_t slotId, uint8_t dir) {
  if (rxLen < sizeof(T)) return false;
  uint8_t tmp[sizeof(T)];
  memcpy(tmp, raw, sizeof(T));
  pktDecrypt(tmp, sizeof(T), sfCount, slotId, dir);
  memcpy(&out, tmp, sizeof(T));
  return true;
}

/**
 * Beacon receive helper — handles the split plaintext/encrypted layout.
 *
 * BeaconPacket on wire: bytes 0-1 are sfCount in plaintext; bytes 2+ are
 * AES-128 CTR encrypted with nonce (sfCount, slotId=0, PKT_DIR_BEACON).
 * The node reads sfCount from the plaintext prefix to build the nonce before
 * decrypting — this breaks the chicken-and-egg problem while still rejecting
 * foreign-network beacons (wrong key → garbage pktType → caller discards).
 *
 * In plain builds bytes 2+ are not encrypted; this is a plain memcpy cast.
 */
template <typename T>
inline bool pktReceiveBeacon(const uint8_t* raw, size_t rxLen, T& out) {
  if (rxLen < sizeof(T)) return false;
  uint8_t tmp[sizeof(T)];
  memcpy(tmp, raw, sizeof(T));
#ifdef PKT_ENCRYPTION
  uint16_t sfCount = (uint16_t)tmp[0] | ((uint16_t)tmp[1] << 8);
  pktDecrypt(tmp + 2, sizeof(T) - 2, sfCount, 0, PKT_DIR_BEACON);
#endif
  memcpy(&out, tmp, sizeof(T));
  return true;
}
