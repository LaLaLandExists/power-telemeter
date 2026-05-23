/**
 * hal_rtos.h
 *
 * Platform HAL -- ESP32 IDF FreeRTOS extensions only.
 *
 * Standard FreeRTOS APIs (xSemaphore*, xQueue*, vTaskDelay, pdMS_TO_TICKS,
 * ulTaskNotifyTake, xTaskNotifyGive, configASSERT, etc.) are NOT wrapped here
 * because they are part of the official freertos.org spec and portable as-is.
 *
 * Wrapped here:
 *   - xTaskCreatePinnedToCore()  -- ESP32 IDF extension (core affinity).
 *   - portMUX_TYPE / portENTER_CRITICAL(mux*) / portEXIT_CRITICAL(mux*) -- ESP32
 *     IDF spinlock; standard FreeRTOS uses taskENTER/EXIT_CRITICAL() with no arg.
 */
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Core affinity constants for halTaskCreatePinned
// ---------------------------------------------------------------------------
#define HAL_CORE_0  0
#define HAL_CORE_1  1

/**
 * Create a task pinned to a specific core (replaces xTaskCreatePinnedToCore).
 *
 * @param fn         Task function.
 * @param name       Debug name string.
 * @param stackBytes Stack size in bytes.
 * @param param      Argument forwarded to fn.
 * @param priority   FreeRTOS priority (0 = lowest).
 * @param core       HAL_CORE_0 or HAL_CORE_1.
 * @param outHandle  Receives the TaskHandle_t, or nullptr to discard.
 */
void halTaskCreatePinned(TaskFunction_t fn, const char* name,
                         uint32_t stackBytes, void* param,
                         UBaseType_t priority, int core,
                         TaskHandle_t* outHandle);

// ---------------------------------------------------------------------------
// ISR-safe spinlock (replaces portMUX_TYPE + portENTER/EXIT_CRITICAL(mux*))
//
// portMUX_TYPE is an ESP32 IDF-specific struct; it does not exist in upstream
// FreeRTOS.  The opaque buffer below is sized to hold any reasonable platform
// spinlock.  hal_rtos_esp32.cpp static_asserts that portMUX_TYPE fits inside.
// ---------------------------------------------------------------------------
#define HAL_CRITSEC_OPAQUE_BYTES 16
typedef union {
  uint8_t  _b[HAL_CRITSEC_OPAQUE_BYTES];
  uint32_t _align;  // force 4-byte alignment
} HalCritSec_t;

/** Static initializer -- use for statically-allocated HalCritSec_t. */
#define HAL_CRITSEC_INITIALIZER { ._b = {0} }

/**
 * Initialize a HalCritSec_t that was NOT statically initialized.
 * Must be called before the first halCritSecEnter().
 */
void halCritSecInit(HalCritSec_t* cs);

/** Enter the critical section (disables preemption / uses spinlock). */
void halCritSecEnter(HalCritSec_t* cs);

/** Exit the critical section. */
void halCritSecExit(HalCritSec_t* cs);
