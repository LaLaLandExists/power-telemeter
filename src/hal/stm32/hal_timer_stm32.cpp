/**
 * hal_timer_stm32.cpp
 *
 * STM32F411 one-shot hardware timer using STM32duino HardwareTimer on TIM5.
 *
 * TIM5 is a 32-bit general-purpose timer on APB1 (84 MHz after PLL on F411).
 * HardwareTimer wraps the TIM5_IRQHandler, so we must use its API rather than
 * defining our own ISR (which would cause a multiple-definition link error).
 *
 * The callback fires from ISR context. Only xQueueSendFromISR / portYIELD_FROM_ISR
 * are used inside it, which is legal when the NVIC priority is >= the FreeRTOS
 * max-syscall priority (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5).
 * We set priority 6 explicitly after constructing the HardwareTimer instance.
 *
 * TIM5 is chosen over TIM2 to avoid a potential conflict with the Arduino
 * SrcWrapper HardwareTimer.cpp that already claims TIM2_IRQHandler.
 *
 * Only compiled when both BOARD_STM32_F411 and TDMA_IRQ_DRIVEN are defined.
 */
#if defined(BOARD_STM32_F411) && defined(TDMA_IRQ_DRIVEN)

#include "hal/hal_timer.h"
#include <HardwareTimer.h>
#include <stm32f4xx_hal.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static HardwareTimer* s_tim    = nullptr;
static void (*s_timerCb)(void) = nullptr;

// ---------------------------------------------------------------------------
// ISR callback (called by HardwareTimer from TIM5_IRQHandler)
// ---------------------------------------------------------------------------

static void onTimerUpdate() {
  // One-shot: stop the timer immediately so it doesn't repeat.
  s_tim->pause();
  void (*cb)(void) = s_timerCb;
  s_timerCb = nullptr;
  if (cb) cb();
}

// ---------------------------------------------------------------------------
// Lazy init -- called once on the first halTimerOnce() invocation
// ---------------------------------------------------------------------------

static void timerInit() {
  if (s_tim) return;
  s_tim = new HardwareTimer(TIM5);
  s_tim->attachInterrupt(onTimerUpdate);

  // Set NVIC priority to 6 (lower urgency number = higher priority on ARM, but
  // here we use the STM32 NVIC encoding where 6 > 5 means less urgent).
  // FreeRTOS requires FromISR callers to be at priority >= MAX_SYSCALL (5),
  // so priority 6 is the lowest priority that still allows FromISR APIs.
  NVIC_SetPriority(TIM5_IRQn, 6);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void halTimerOnce(uint32_t delayUs, void (*cb)(void)) {
  timerInit();

  // Stop any running count before re-arming.
  s_tim->pause();
  s_timerCb = cb;

  // setOverflow sets both the period and the prescaler automatically.
  // MICROSEC_FORMAT tells HardwareTimer the period is in microseconds.
  // Minimum is 1 us; clamp to avoid a zero period.
  s_tim->setOverflow((delayUs > 0) ? delayUs : 1, MICROSEC_FORMAT);
  s_tim->setCount(0, MICROSEC_FORMAT);
  s_tim->resume();
}

void halTimerCancel() {
  if (!s_tim) return;
  s_tim->pause();
  s_timerCb = nullptr;
}

#endif // BOARD_STM32_F411 && TDMA_IRQ_DRIVEN
