#include "lora_hal.h"
#include <SPI.h>
#include <Arduino.h>

// SX1278 SPI: max 10 MHz, MSBFIRST, mode 0 -- matches RadioLib defaults
static const SPISettings SX1278_SPI_SETTINGS(2000000, MSBFIRST, SPI_MODE0);

uint8_t loraReadReg(uint8_t nssPin, uint8_t addr) {
  SPI.beginTransaction(SX1278_SPI_SETTINGS);
  digitalWrite(nssPin, LOW);
  SPI.transfer(addr & 0x7F);  // MSB=0 -> read
  uint8_t val = SPI.transfer(0x00);
  digitalWrite(nssPin, HIGH);
  SPI.endTransaction();
  return val;
}

bool loraIsLoRaMode(uint8_t nssPin) {
  return (loraReadReg(nssPin, SX1278_REG_OP_MODE) & SX1278_LORA_MODE_BIT) != 0;
}
