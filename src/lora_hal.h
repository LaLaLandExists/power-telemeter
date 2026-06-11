#pragma once
#include <stdint.h>

// SX1278 RegOpMode (0x01) bit 7: 1 = LoRa, 0 = FSK/OOK
// Bits [2:0]: 0=Sleep 1=Stdby 2=FSTX 3=TX 4=FSRX 5=RxCont 6=RxSingle 7=CAD
#define SX1278_REG_OP_MODE      0x01
#define SX1278_LORA_MODE_BIT    0x80
#define SX1278_MODE_MASK        0x07

/**
 * Read one SX1278 register via direct SPI.
 * Bypasses RadioLib so it can be called even when the library's internal
 * state machine is stuck (e.g. after EMI-induced register corruption).
 * Caller must ensure no concurrent RadioLib SPI transaction is in flight.
 */
uint8_t loraReadReg(uint8_t nssPin, uint8_t addr);

/**
 * Return true if the SX1278 RegOpMode bit 7 indicates LoRa mode.
 * Use before startReceive() to detect EMI-induced FSK fallback.
 */
bool loraIsLoRaMode(uint8_t nssPin);
