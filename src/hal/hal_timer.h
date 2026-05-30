/**
 * hal_timer.h
 * HAL for a one-shot hardware countdown timer.
 *
 * Used by the interrupt-driven TDMA implementation (TDMA_IRQ_DRIVEN) to hit
 * slot boundaries with hardware precision instead of FreeRTOS tick granularity.
 *
 * Contract:
 *   - halTimerOnce() arms the timer; cb fires from ISR context when it expires.
 *   - cb MUST be minimal: post to a queue / give a semaphore only.
 *     No RadioLib calls, no blocking, no FreeRTOS API beyond FromISR variants.
 *   - Calling halTimerOnce() while a timer is already armed implicitly cancels
 *     the previous one and re-arms with the new delay + callback.
 *   - halTimerCancel() stops the timer before it fires; safe to call even if
 *     no timer is armed.
 *   - Not reentrant; call only from the TDMA task.
 */
#pragma once
#include <stdint.h>

/**
 * Arm a one-shot countdown timer.
 * @param delayUs  Delay in microseconds before cb fires.
 * @param cb       Callback invoked from ISR context when the timer expires.
 */
void halTimerOnce(uint32_t delayUs, void (*cb)(void));

/** Cancel a pending one-shot timer. Safe to call when no timer is armed. */
void halTimerCancel(void);
