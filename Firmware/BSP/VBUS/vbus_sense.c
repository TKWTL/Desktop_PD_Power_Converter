#include "vbus_sense.h"
#include "main.h"
#include "ch32x035.h"

#define VBUS_ADC_TIMEOUT_LOOPS  20000UL

void VBUS_Sense_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    ADC_InitTypeDef adc = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);

    gpio.GPIO_Pin = VBUS_SENSE_Pin;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(VBUS_SENSE_GPIO_Port, &gpio);

    ADC_DeInit(ADC1);
    ADC_CLKConfig(ADC1, ADC_CLK_Div8);
    adc.ADC_Mode = ADC_Mode_Independent;
    adc.ADC_ScanConvMode = DISABLE;
    adc.ADC_ContinuousConvMode = DISABLE;
    adc.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    adc.ADC_DataAlign = ADC_DataAlign_Right;
    adc.ADC_NbrOfChannel = 1u;
    adc.ADC_OutputBuffer = 0u;
    adc.ADC_Pga = 0u;
    ADC_Init(ADC1, &adc);
    ADC_RegularChannelConfig(ADC1, VBUS_SENSE_ADC_CHANNEL, 1u, ADC_SampleTime_11Cycles);
    ADC_Cmd(ADC1, ENABLE);
}

uint16_t VBUS_Sense_ReadRaw(void)
{
    uint32_t guard = VBUS_ADC_TIMEOUT_LOOPS;

    ADC_ClearFlag(ADC1, ADC_FLAG_EOC);
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while((ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET) && guard)
        guard--;
    if(guard == 0u)
        return 0u;
    return ADC_GetConversionValue(ADC1);
}

uint16_t VBUS_Sense_ReadMillivolts(void)
{
    uint32_t raw = VBUS_Sense_ReadRaw();
    uint32_t adc_mv = (raw * VBUS_ADC_VREF_MV + (VBUS_ADC_FULL_SCALE / 2u)) /
                      VBUS_ADC_FULL_SCALE;
    uint32_t vbus_mv = adc_mv * (VBUS_DIVIDER_TOP_OHM + VBUS_DIVIDER_BOTTOM_OHM) /
                       VBUS_DIVIDER_BOTTOM_OHM;
    return (vbus_mv > 65535u) ? 65535u : (uint16_t)vbus_mv;
}
