/**
 * rfid_provision.h
 *
 * PN532 NFC/RFID key provisioning for network packet encryption.
 * Compiled only when -D PKT_ENCRYPTION is set.
 *
 * Gateway role: writes the 16-byte AES key to a MIFARE Classic card (block 4).
 * Node role:    reads the 16-byte AES key from the card and passes it to cryptoSetKey().
 *
 * Hardware: PN532 in I2C mode.
 *   Gateway: SDA=16, SCL=17 (shared I2C bus with FRAM; PN532 addr=0x48, FRAM addr=0x50)
 *   Node:    SDA=16, SCL=17  (shared layout with gateway; GPIO16=RX2, GPIO17=TX2)
 *
 * MIFARE Classic key storage: sector 1 block 4, default Key A (0xFF x 6).
 */
#pragma once
#ifdef PKT_ENCRYPTION
#include <cstdint>

/**
 * Initialise the PN532 module.
 * On gateway, Wire.begin() is already called by framInit() -- calling it again
 * with the same pins is safe and a no-op.
 * Returns true if the PN532 firmware version is readable.
 */
bool rfidProvisionInit();

/**
 * Wait for a MIFARE card, read the 16-byte key from block 4.
 * Stores the key via cryptoSetKey() and copies it into key16.
 * Blocks up to ~3 seconds waiting for a card.
 * Returns true on success.
 */
bool rfidProvisionRead(uint8_t* key16);

/**
 * Wait for a MIFARE card, write key16 (16 bytes) to block 4.
 * Blocks up to ~5 seconds waiting for a card.
 * Returns true on success.
 */
bool rfidProvisionWrite(const uint8_t* key16);

#endif // PKT_ENCRYPTION
