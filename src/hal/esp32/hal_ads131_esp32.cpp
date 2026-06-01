/**
 * hal_ads131_esp32.cpp
 * ADS131M02 SPI driver for ESP32-S3 Super Mini.
 *
 * Uses Arduino SPIClass on the HSPI (SPI3) bus so it is independent of the
 * LoRa module which owns the default FSPI (SPI2) bus. DRDY is polled with a
 * digitalRead() tight loop; replace with a GPIO ISR + DMA when hardware is
 * available and jitter-free 4 kHz sampling is required.
 *
 * Placeholder SPI pins (PCB TBD -- see CLAUDE.md for assignments):
 *   SCK=16  MOSI=17  MISO=18  CS=15  DRDY=14  RST=21
 */
#if defined(BOARD_S3_SUPERMINI) && defined(METER_AFE)

#include "hal/hal_ads131.h"
#include <Arduino.h>
#include <SPI.h>

// ADS131M02 command words (24-bit, MSB first)
static const uint8_t CMD_WAKEUP[3]  = {0x00, 0x33, 0x00};  // WAKEUP
static const uint8_t CMD_NULL[3]    = {0x00, 0x00, 0x00};  // NULL (data read)

static SPIClass    s_spi(HSPI);
static HalAdsPins  s_pins;

void halAdsInit(HalAdsPins pins) {
  s_pins = pins;

  pinMode(s_pins.rst,  OUTPUT);
  pinMode(s_pins.cs,   OUTPUT);
  pinMode(s_pins.drdy, INPUT);

  digitalWrite(s_pins.cs,  HIGH);
  digitalWrite(s_pins.rst, LOW);
  delay(1);
  digitalWrite(s_pins.rst, HIGH);
  delay(5);  // t_reset per datasheet

  s_spi.begin(s_pins.sck, s_pins.miso, s_pins.mosi, s_pins.cs);
  s_spi.setFrequency(8000000);
  s_spi.setDataMode(SPI_MODE1);  // CPOL=0, CPHA=1 per ADS131M02 spec
  s_spi.setBitOrder(MSBFIRST);
}

void halAdsStart(void) {
  // Send WAKEUP command to enable continuous conversion
  s_spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  digitalWrite(s_pins.cs, LOW);
  s_spi.transfer(CMD_WAKEUP[0]);
  s_spi.transfer(CMD_WAKEUP[1]);
  s_spi.transfer(CMD_WAKEUP[2]);
  // Response frame follows immediately -- read STATUS + CH0 + CH1 and discard
  uint8_t discard[9];
  for (int i = 0; i < 9; i++) discard[i] = s_spi.transfer(0x00);
  (void)discard;
  digitalWrite(s_pins.cs, HIGH);
  s_spi.endTransaction();
  delay(1);
  Serial.println("[METER-AFE] ADS131M02 started (S3, polling DRDY)");
}

void halAdsReadSample(int32_t* codeV, int32_t* codeI) {
  // Wait for DRDY low (active-low data-ready signal)
  uint32_t t = millis();
  while (digitalRead(s_pins.drdy) && (millis() - t < 10)) {}

  // SPI frame: NULL cmd (3 bytes) + STATUS (3 bytes) + CH0 (3 bytes) + CH1 (3 bytes)
  // With CRC disabled, response = STATUS[2:0] + CH0[5:3] + CH1[8:6] = 9 bytes.
  // We send 3 NULL bytes (cmd) and receive 9 bytes simultaneously in two phases:
  //   Phase 1: clock out CMD (3 bytes), device clocks out STATUS (3 bytes)
  //   Phase 2: clock out 6 zeros, device clocks out CH0+CH1 (6 bytes)
  // Combined: 9-byte transfer; TX = all 0x00.
  uint8_t frame[9];
  s_spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  digitalWrite(s_pins.cs, LOW);
  for (int i = 0; i < 9; i++) {
    frame[i] = s_spi.transfer(CMD_NULL[i < 3 ? i : 0]);
  }
  digitalWrite(s_pins.cs, HIGH);
  s_spi.endTransaction();

  // frame[0:2] = STATUS word (ignored for now)
  // frame[3:5] = CH0 (voltage), frame[6:8] = CH1 (current)
  uint32_t rawV = ((uint32_t)frame[3] << 16) | ((uint32_t)frame[4] << 8) | frame[5];
  uint32_t rawI = ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 8) | frame[8];

  *codeV = (rawV & 0x00800000u) ? (int32_t)(rawV | 0xFF000000u) : (int32_t)rawV;
  *codeI = (rawI & 0x00800000u) ? (int32_t)(rawI | 0xFF000000u) : (int32_t)rawI;
}

#endif // BOARD_S3_SUPERMINI && METER_AFE
