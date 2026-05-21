/**
 * rfid_provision.cpp
 *
 * PN532 I2C key provisioning for PKT_ENCRYPTION builds.
 */
#ifdef PKT_ENCRYPTION

#include "rfid_provision.h"
#include "crypto.h"
#include "log_async.h"
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <string.h>

// PN532 I2C pins differ by role and board.
// Gateway:           shares existing FRAM bus (SDA=16 SCL=17); PN532 addr 0x48 != FRAM 0x50.
// Node (ESP32-dev):  shared layout with gateway (SDA=16 SCL=17).
// Node (S3 Super Mini): dedicated bus on THT GPIOs (SDA=1 SCL=2).
#ifdef NODE_GATEWAY
  #define PN532_SDA       16
  #define PN532_SCL       17
  #define PN532_IRQ_DUMMY 35
#elif defined(NODE_TELEMETRY) && defined(BOARD_S3_SUPERMINI)
  #define PN532_SDA       1
  #define PN532_SCL       2
  #define PN532_IRQ_DUMMY 8    // freed by LoRa pin assignment; dummy only -- not physically wired
#elif defined(NODE_TELEMETRY)
  #define PN532_SDA       16
  #define PN532_SCL       17
  #define PN532_IRQ_DUMMY 35
#else
  #error "rfid_provision: role not defined -- set NODE_GATEWAY or NODE_TELEMETRY"
#endif

// PN532_IRQ_DUMMY / GPIO 0 as RST dummy.
// Adafruit_PN532 calls pinMode() unconditionally in its I2C constructor, so
// passing 0xFF (-> int8_t -1 -> uint8_t 255) triggers the ESP32 HAL invalid-pin
// warning. Neither dummy is physically connected to the PN532; the IRQ line is
// never read (polling mode) and the RST pulse during begin() is harmless on an
// unconnected GPIO.
static Adafruit_PN532 s_nfc(PN532_IRQ_DUMMY, 0, &Wire);
static bool           s_initOk = false;

// MIFARE Classic sector 1, block 0 -- stores 16-byte AES key
static const uint8_t RFID_KEY_BLOCK = 4;
// Default factory Key A -- change if you want physical write protection
static const uint8_t KEY_A[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

bool rfidProvisionInit() {
  Wire.begin(PN532_SDA, PN532_SCL);
  s_nfc.begin();

  uint32_t ver = s_nfc.getFirmwareVersion();
  if (!ver) {
    logAsync("[RFID] PN532 not found (SDA=%d SCL=%d)\n", PN532_SDA, PN532_SCL);
    return false;
  }

  s_nfc.SAMConfig();
  s_initOk = true;
  logAsync("[RFID] PN532 OK (fw=0x%08X, SDA=%d SCL=%d)\n",
           (unsigned)ver, PN532_SDA, PN532_SCL);
  return true;
}

bool rfidProvisionRead(uint8_t* key16) {
  if (!s_initOk) return false;

  uint8_t uid[7];
  uint8_t uidLen;
  // 3 second timeout for card detection
  if (!s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 3000)) {
    return false;
  }

  if (!s_nfc.mifareclassic_AuthenticateBlock(
        uid, uidLen, RFID_KEY_BLOCK, 0,
        const_cast<uint8_t*>(KEY_A))) {
    logAsync("[RFID] Auth failed on read\n");
    return false;
  }

  uint8_t data[16];
  if (!s_nfc.mifareclassic_ReadDataBlock(RFID_KEY_BLOCK, data)) {
    logAsync("[RFID] Block read failed\n");
    return false;
  }

  // Reject all-zero (unprogrammed card)
  bool nonzero = false;
  for (int i = 0; i < 16; i++) {
    if (data[i]) { nonzero = true; break; }
  }
  if (!nonzero) {
    logAsync("[RFID] Card not provisioned (all zeros in block %d)\n", RFID_KEY_BLOCK);
    return false;
  }

  memcpy(key16, data, 16);
  cryptoSetKey(key16);
  logAsync("[RFID] Key read from card\n");
  return true;
}

bool rfidProvisionWrite(const uint8_t* key16) {
  if (!s_initOk) return false;

  uint8_t uid[7];
  uint8_t uidLen;
  // 5 second timeout -- give the user time to present the card
  if (!s_nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 5000)) {
    logAsync("[RFID] No card detected (5 s timeout)\n");
    return false;
  }

  if (!s_nfc.mifareclassic_AuthenticateBlock(
        uid, uidLen, RFID_KEY_BLOCK, 0,
        const_cast<uint8_t*>(KEY_A))) {
    logAsync("[RFID] Auth failed on write\n");
    return false;
  }

  // mifareclassic_WriteDataBlock requires a non-const 16-byte buffer
  uint8_t data[16];
  memcpy(data, key16, 16);
  if (!s_nfc.mifareclassic_WriteDataBlock(RFID_KEY_BLOCK, data)) {
    logAsync("[RFID] Block write failed\n");
    return false;
  }

  logAsync("[RFID] Key written to card (block %d)\n", RFID_KEY_BLOCK);
  return true;
}

#endif // PKT_ENCRYPTION
