#ifndef FAN_PWM_H
#define FAN_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TIM1_CH1 is routed to PB9 on CH32X035. With a 48 MHz timer clock,
 * PSC=0 and ARR=255 produce exactly 187.5 kHz.  The 256-count reload keeps
 * the whole compare range inside 0..255, so CCR1 equals the 8-bit duty
 * one-to-one and the old 481-level (ARR=479) range is gone; the higher
 * carrier is the expected side effect of the smaller reload value. */
#define FAN_PWM_FREQUENCY_HZ      187500UL
#define FAN_PWM_PERIOD_COUNTS     256U
#define FAN_PWM_MAX_LEVEL         255U

/* Fan characterisation measured in this duty scale (2026-09-13), to be used
 * by the future automatic thermal policy - details in BSP/Fan/README.md:
 *   0..48    does not start;
 *   48..72   runs with a high-frequency electrical buzz - avoid;
 *   80..104  silent band; when starting from rest jump 0 -> 80, never ramp
 *            through 48..72;
 *   112+     air noise starts to rise with duty;
 *   200+     only acceptable above 100 C. */
void FAN_PWM_Init(void);
void FAN_PWM_SetLevel(uint16_t level);
void FAN_PWM_SetDuty8(uint8_t duty);
uint16_t FAN_PWM_GetLevel(void);
uint32_t FAN_PWM_GetFrequencyHz(void);

#ifdef __cplusplus
}
#endif

#endif /* FAN_PWM_H */
