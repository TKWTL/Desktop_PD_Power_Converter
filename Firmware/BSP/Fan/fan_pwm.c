#include "fan_pwm.h"
#include "main.h"
#include "ch32x035.h"

static uint16_t s_fan_level;

void FAN_PWM_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    TIM_TimeBaseInitTypeDef time_base = {0};
    TIM_OCInitTypeDef oc = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_TIM1, ENABLE);

    /* PB9 is the native TIM1_CH1 output on CH32X035. */
    gpio.GPIO_Pin = FAN_Pin;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(FAN_GPIO_Port, &gpio);

    TIM_TimeBaseStructInit(&time_base);
    time_base.TIM_Period = FAN_PWM_PERIOD_COUNTS - 1U;
    time_base.TIM_Prescaler = 0U;
    time_base.TIM_ClockDivision = TIM_CKD_DIV1;
    time_base.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM1, &time_base);

    TIM_OCStructInit(&oc);
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse = 0U;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(TIM1, &oc);
    TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM1, ENABLE);

    TIM_CtrlPWMOutputs(TIM1, ENABLE);
    TIM_Cmd(TIM1, ENABLE);
    s_fan_level = 0U;
}

void FAN_PWM_SetLevel(uint16_t level)
{
    if(level > FAN_PWM_MAX_LEVEL)
        level = FAN_PWM_MAX_LEVEL;

    s_fan_level = level;
    TIM_SetCompare1(TIM1, level);
}

void FAN_PWM_SetDuty8(uint8_t duty)
{
    /* Map 0..255 onto 0..480 with rounding.  This preserves all 256 caller
     * levels while the timer itself offers 481 physical compare levels. */
    uint32_t level = ((uint32_t)duty * FAN_PWM_MAX_LEVEL + 127U) / 255U;
    FAN_PWM_SetLevel((uint16_t)level);
}

uint16_t FAN_PWM_GetLevel(void)
{
    return s_fan_level;
}

uint32_t FAN_PWM_GetFrequencyHz(void)
{
    return FAN_PWM_FREQUENCY_HZ;
}
