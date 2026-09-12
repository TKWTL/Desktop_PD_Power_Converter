#ifndef FAN_PWM_H
#define FAN_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TIM1_CH1 is routed to PB9 on CH32X035. With a 48 MHz timer clock,
 * PSC=0 and ARR=479 produce exactly 100 kHz. CCR1 accepts 0..480,
 * giving 481 usable duty levels (including fully off/on). */
#define FAN_PWM_FREQUENCY_HZ      100000UL
#define FAN_PWM_PERIOD_COUNTS     480U
#define FAN_PWM_MAX_LEVEL         FAN_PWM_PERIOD_COUNTS

void FAN_PWM_Init(void);
void FAN_PWM_SetLevel(uint16_t level);
void FAN_PWM_SetDuty8(uint8_t duty);
uint16_t FAN_PWM_GetLevel(void);
uint32_t FAN_PWM_GetFrequencyHz(void);

#ifdef __cplusplus
}
#endif

#endif /* FAN_PWM_H */
