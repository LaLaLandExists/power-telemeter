/**
 * hal_rtos_stm32.cpp
 *
 * STM32F411 implementation of hal_rtos.h.
 *
 * Core-pinning: STM32F411 is single-core, so halTaskCreatePinned() ignores the
 * core parameter and falls back to xTaskCreate().  Tasks that were pinned to
 * Core 1 on ESP32 (TDMA radio timing) now compete on the single core with all
 * other tasks.  In the node role there is no web task, so the real contenders
 * are low-priority PZEM/sched/log tasks -- timing impact is acceptable.
 *
 * Critical section: standard upstream FreeRTOS uses the parameterless
 * taskENTER_CRITICAL() / taskEXIT_CRITICAL() (backed by BASEPRI on Cortex-M).
 * The HalCritSec_t opaque buffer is unused; it exists only to satisfy the
 * shared header interface.
 */
#if defined(BOARD_STM32_F411)

#include "hal/hal_rtos.h"
#include <STM32FreeRTOS.h>

// ---------------------------------------------------------------------------
// Task creation
// ---------------------------------------------------------------------------

void halTaskCreatePinned(TaskFunction_t fn, const char* name,
                         uint32_t stackBytes, void* param,
                         UBaseType_t priority, int /*core*/,
                         TaskHandle_t* outHandle) {
  // Convert bytes to words: FreeRTOS stack depth is in StackType_t units.
  xTaskCreate(fn, name, (uint16_t)(stackBytes / sizeof(StackType_t)),
              param, priority, outHandle);
}

// ---------------------------------------------------------------------------
// Critical section (parameterless Cortex-M BASEPRI mask)
// ---------------------------------------------------------------------------

void halCritSecInit(HalCritSec_t* /*cs*/) {
  // No per-instance state on standard FreeRTOS.
}

void halCritSecEnter(HalCritSec_t* /*cs*/) {
  taskENTER_CRITICAL();
}

void halCritSecExit(HalCritSec_t* /*cs*/) {
  taskEXIT_CRITICAL();
}

#endif // BOARD_STM32_F411
