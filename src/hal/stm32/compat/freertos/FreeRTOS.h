/* FreeRTOS.h compat shim for STM32 (ststm32 / STM32duino).
 *
 * Existing source files include <freertos/FreeRTOS.h> using the ESP-IDF path.
 * On STM32, this directory is prepended to the search path so that include
 * resolves here, then forwards to STM32FreeRTOS which provides all RTOS types.
 */
#pragma once
#include <STM32FreeRTOS.h>
