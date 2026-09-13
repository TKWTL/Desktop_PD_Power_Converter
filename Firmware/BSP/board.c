#include "board.h"

uint8_t Board_Key1Pressed(void)
{
    return GPIO_ReadInputDataBit(KEY1_GPIO_Port, KEY1_Pin) == Bit_RESET;
}

uint8_t Board_Key2Pressed(void)
{
    return GPIO_ReadInputDataBit(KEY2_GPIO_Port, KEY2_Pin) == Bit_RESET;
}

void Board_RebootToISP(void)
{
    /* CH32 factory-boot entry sequence:
     *  1. unlock FLASH_STATR.BOOT_MODE;
     *  2. select BOOT flash for the next reset;
     *  3. clear reset-cause flags;
     *  4. request a PFIC software reset with the mandatory 0xBEEF key.
     *
     * This is deliberately a reset-based handoff. Do not jump directly into
     * ROM with active USB-PD/I2C/DMA peripherals and a live application stack. */
    __disable_irq();
    FLASH->BOOT_MODEKEYR = BOOT_MODEKEYP_MODEKEYR1;
    FLASH->BOOT_MODEKEYR = BOOT_MODEKEYP_MODEKEYR2;
    FLASH->STATR = FLASH_STATR_BOOT_MODE;
    RCC->RSTSCKR |= RCC_RMVF;
    PFIC->CFGR = NVIC_KEY3 | (1u << 7);

    while(1) { }
}

void Board_SoftReset(void)
{
    /* Unlike Board_RebootToISP() the factory BOOT_MODE is left alone: this is
     * the same PFIC software-reset request NVIC_SystemReset() uses, so the
     * core restarts in the application from flash.  Clearing the reset flags
     * first keeps the boot banner honest - the next [RESET] cause reads "SW"
     * instead of a stale power-on flag. */
    __disable_irq();
    RCC->RSTSCKR |= RCC_RMVF;
    PFIC->CFGR = NVIC_KEY3 | (1u << 7);

    while(1) { }
}

void Board_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);

    /* Buttons are active-low; the present schematic does not require external
     * pull-ups because CH32X035 internal pull-ups are enabled here. */
    gpio.GPIO_Pin = KEY1_Pin | KEY2_Pin | POWER_INT_Pin;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* Keep OLED deselected until the SH1107/MiaoUI transport is initialized. */
    GPIO_SetBits(OLED_CS_GPIO_Port, OLED_CS_Pin);
    gpio.GPIO_Pin = OLED_CS_Pin | OLED_DC_Pin;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_SetBits(OLED_CS_GPIO_Port, OLED_CS_Pin);
    GPIO_ResetBits(OLED_DC_GPIO_Port, OLED_DC_Pin);
}
