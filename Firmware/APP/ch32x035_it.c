/********************************** (C) COPYRIGHT *******************************
 * CH32X035 application interrupt-vector entry points.
 * Peripheral modules expose handler bodies; vector ownership remains in APP.
 *******************************************************************************/
#include "ch32x035_it.h"
#include "app_tasks.h"
#include "debug.h"
#include "usart_async.h"
#include "spi_dma.h"
#include "i2c_api.h"
#include "time_api.h"
#include "ch32x035.h"

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel4_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel5_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel6_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel7_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_EV_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_ER_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void NMI_Handler(void)
{
    while(1) { }
}

static void APP_EmergencyUartWrite(const char *text)
{
    /* Fault-only path: stop TX DMA and use the USART data register directly.
     * This deliberately does not depend on the scheduler/heap/printf. */
    USART_DMACmd(USART1, USART_DMAReq_Tx, DISABLE);
    DMA_Cmd(DMA1_Channel4, DISABLE);

    while(*text)
    {
        uint32_t guard = 200000u;
        while((USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) && guard)
            guard--;
        if(!guard)
            return;
        USART_SendData(USART1, (uint8_t)*text++);
    }
}

void HardFault_Handler(void)
{
    g_dbg_fault_count++;
    APP_EmergencyUartWrite("\r\n[FAULT] HardFault - halted (no software reset)\r\n");
    while(1) { }
}

/* Every ISR entry bumps a .noinit counter: if a reset happens, the boot log
 * shows how many interrupts arrived in the last second (storm detection). */
void DMA1_Channel3_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_SPI_TX);
    SPI_DMA_TxDMA_IRQHandler();
}

void DMA1_Channel4_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_UART_TX);
    USART1_Async_TxDMA_IRQHandler();
}

void DMA1_Channel5_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_UART_RX);
    USART1_Async_RxDMA_IRQHandler();
}

void DMA1_Channel6_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_I2C_TX);
    I2C_API_TxDMA_IRQHandler();
}

void DMA1_Channel7_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_I2C_RX);
    I2C_API_RxDMA_IRQHandler();
}

void I2C1_EV_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_I2C_EV);
    I2C_API_EV_IRQHandler();
}

void I2C1_ER_IRQHandler(void)
{
    DBG_ISR_BUMP(DBG_ISR_I2C_ER);
    I2C_API_ER_IRQHandler();
}

/* 1 ms timebase tick.  It keeps the millisecond counter of the shared time API
 * alive while the main loop is parked in WFI, and refreshes the compare target
 * for the next period. */
void SysTick_Handler(void)
{
    TIME_TickHandler();
    DBG_TickHook();
    APP_DbgStallCheck();
}
