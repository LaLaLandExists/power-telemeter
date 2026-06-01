/**
 * hal_ads131_stm32.cpp
 * ADS131M02 SPI driver for STM32F411 Black Pill.
 *
 * Uses Arduino SPIClass on SPI2 (PB13/PB14/PB15/PB12) which is separate from
 * the LoRa module. DRDY is polled with a digitalRead() tight loop; replace
 * with an EXTI ISR + DMA when hardware is available.
 *
 * Placeholder SPI pins (PCB TBD -- see CLAUDE.md for assignments):
 *   SCK=PB13  MOSI=PB15  MISO=PB14  CS=PB12  DRDY=PA0  RST=PA1
 *
 * NVIC note: when upgrading to ISR-driven acquisition, set the EXTI0_IRQn
 * priority to >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (= 5) so that
 * FreeRTOS FromISR APIs (vTaskNotifyGiveFromISR) are legal inside the ISR.
 * See hal_timer_stm32.cpp for the pattern.
 */
#if defined(BOARD_STM32_F411) && defined(METER_AFE)

#include "hal/hal_ads131.h"
#include <Arduino.h>
#include <SPI.h>

static const uint8_t CMD_WAKEUP[3] = {0x00, 0x33, 0x00};
static const uint8_t CMD_NULL[3]   = {0x00, 0x00, 0x00};

// SPIClass default-constructed (uses board-variant default SPI pins), then
// overridden with setMOSI/setMISO/setSCLK before begin() -- the standard
// STM32duino pattern for custom SPI bus configuration.
static SPIClass   s_spi;
static HalAdsPins s_pins;

void halAdsInit(HalAdsPins pins) {
  s_pins = pins;

  pinMode(s_pins.rst,  OUTPUT);
  pinMode(s_pins.cs,   OUTPUT);
  pinMode(s_pins.drdy, INPUT);

  digitalWrite(s_pins.cs,  HIGH);
  digitalWrite(s_pins.rst, LOW);
  delay(1);
  digitalWrite(s_pins.rst, HIGH);
  delay(5);

  // Configure custom pins before begin() -- STM32duino setters accept uint32_t pin
  s_spi.setMOSI(s_pins.mosi);
  s_spi.setMISO(s_pins.miso);
  s_spi.setSCLK(s_pins.sck);
  s_spi.begin();
  s_spi.setDataMode(SPI_MODE1);
  s_spi.setBitOrder(MSBFIRST);
  s_spi.setClockDivider(SPI_CLOCK_DIV8);  // ~12.5 MHz on APB1/2 depending on config
}

void halAdsStart(void) {
  s_spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  digitalWrite(s_pins.cs, LOW);
  s_spi.transfer(CMD_WAKEUP[0]);
  s_spi.transfer(CMD_WAKEUP[1]);
  s_spi.transfer(CMD_WAKEUP[2]);
  uint8_t discard[9];
  for (int i = 0; i < 9; i++) discard[i] = s_spi.transfer(0x00);
  (void)discard;
  digitalWrite(s_pins.cs, HIGH);
  s_spi.endTransaction();
  delay(1);
  Serial.println("[METER-AFE] ADS131M02 started (STM32F411, polling DRDY)");
}

void halAdsReadSample(int32_t* codeV, int32_t* codeI) {
  uint32_t t = millis();
  while (digitalRead(s_pins.drdy) && (millis() - t < 10)) {}

  uint8_t frame[9];
  s_spi.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  digitalWrite(s_pins.cs, LOW);
  for (int i = 0; i < 9; i++) {
    frame[i] = s_spi.transfer(CMD_NULL[i < 3 ? i : 0]);
  }
  digitalWrite(s_pins.cs, HIGH);
  s_spi.endTransaction();

  uint32_t rawV = ((uint32_t)frame[3] << 16) | ((uint32_t)frame[4] << 8) | frame[5];
  uint32_t rawI = ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 8) | frame[8];

  *codeV = (rawV & 0x00800000u) ? (int32_t)(rawV | 0xFF000000u) : (int32_t)rawV;
  *codeI = (rawI & 0x00800000u) ? (int32_t)(rawI | 0xFF000000u) : (int32_t)rawI;
}

#endif // BOARD_STM32_F411 && METER_AFE
