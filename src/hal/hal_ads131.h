/**
 * hal_ads131.h
 * ADS131M02 24-bit sigma-delta ADC -- SPI acquisition interface.
 *
 * The ADS131M02 asserts DRDY low when a new sample pair is ready.
 * halAdsReadSample() blocks until DRDY is asserted, then reads the 9-byte
 * SPI frame (STATUS[2:0] + CH0[5:3] + CH1[8:6]) and sign-extends both codes.
 *
 * Usage:
 *   halAdsInit(pins)         -- configure SPI bus + GPIO, reset device
 *   halAdsStart()            -- send WAKEUP command, enable continuous mode
 *   halAdsReadSample(&v, &i) -- blocking read; returns sign-extended codes
 *
 * Assumptions for the placeholder implementation:
 *   - CRC is disabled in the ADS131M02 register (CRCEN = 0).
 *   - CH0 = voltage channel, CH1 = current channel.
 *   - Caller loops at the natural sample rate (~4 kSPS) by always calling
 *     halAdsReadSample() immediately after processing the previous sample.
 *
 * TODO (when hardware is available):
 *   - Replace polling DRDY with GPIO ISR + DMA SPI for true 4 kHz jitter-free
 *     acquisition. The halFft / processSample call sites in meter_afe.cpp are
 *     unchanged by this upgrade -- only this HAL file changes.
 *   - Verify SPI frame format against ADS131M02 datasheet section 8.5.3.
 *   - Program OCAL registers from NVS at halAdsStart() for hardware DC offset.
 *
 * Implementations:
 *   hal/esp32/hal_ads131_esp32.cpp -- SPIClass(HSPI) + GPIO polling (S3)
 *   hal/stm32/hal_ads131_stm32.cpp -- SPIClass(SPI2) + GPIO polling (STM32F411)
 */
#pragma once
#include <stdint.h>

struct HalAdsPins {
  uint8_t sck;
  uint8_t mosi;
  uint8_t miso;
  uint8_t cs;
  uint8_t drdy;
  uint8_t rst;
};

/** Configure SPI bus, GPIO pins, and reset the ADS131M02. */
void halAdsInit(HalAdsPins pins);

/** Send WAKEUP command and enable continuous conversion mode. */
void halAdsStart(void);

/**
 * Block until DRDY goes low, read one sample frame, sign-extend.
 * codeV and codeI are 24-bit two's complement values in [-2^23, 2^23-1].
 * Times out after 10 ms and returns zeros if DRDY never asserts.
 */
void halAdsReadSample(int32_t* codeV, int32_t* codeI);
