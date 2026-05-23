/**
 * hal_rtos_esp32.cpp
 *
 * ESP32 implementation of hal_rtos.h.
 * Wraps xTaskCreatePinnedToCore and the portMUX_TYPE spinlock API.
 * Standard FreeRTOS APIs (semaphores, queues, vTaskDelay, etc.) are used
 * directly at call sites -- they are not wrapped.
 */
#include "hal/hal_rtos.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static_assert(sizeof(portMUX_TYPE) <= HAL_CRITSEC_OPAQUE_BYTES,
              "HalCritSec_t opaque buffer too small for portMUX_TYPE -- increase HAL_CRITSEC_OPAQUE_BYTES");

// ---------------------------------------------------------------------------
// Task creation
// ---------------------------------------------------------------------------

void halTaskCreatePinned(TaskFunction_t fn, const char* name,
                         uint32_t stackBytes, void* param,
                         UBaseType_t priority, int core,
                         TaskHandle_t* outHandle) {
  xTaskCreatePinnedToCore(fn, name, stackBytes, param, priority, outHandle, core);
}

// ---------------------------------------------------------------------------
// Critical section (portMUX_TYPE spinlock)
// ---------------------------------------------------------------------------

void halCritSecInit(HalCritSec_t* cs) {
  portMUX_TYPE init = portMUX_INITIALIZER_UNLOCKED;
  memcpy(cs->_b, &init, sizeof(portMUX_TYPE));
}

void halCritSecEnter(HalCritSec_t* cs) {
  portENTER_CRITICAL((portMUX_TYPE*)cs->_b);
}

void halCritSecExit(HalCritSec_t* cs) {
  portEXIT_CRITICAL((portMUX_TYPE*)cs->_b);
}
