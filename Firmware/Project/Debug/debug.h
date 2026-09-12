/********************************** (C) COPYRIGHT *******************************
 * Minimal debug/time compatibility layer for the CH32X035 board.
 * printf is retargeted only to USART1_Async; SDI support is intentionally absent.
 *******************************************************************************/
#ifndef __DEBUG_H
#define __DEBUG_H

#include <stdio.h>
#include <stdint.h>
#include "ch32x035.h"

#ifdef __cplusplus
extern "C" {
#endif

void Delay_Init(void);
void Delay_Us(uint32_t n);
void Delay_Ms(uint32_t n);
uint8_t Debug_Flush(uint32_t timeout_us);

/* ------------------------------------------------------------------------ *
 * 复位黑匣子（存放于 .noinit，IWDG 复位后仍保留）
 *
 * 用途：定位“主循环停住 → IWDG 复位”。上电后 APP_Tasks_Init() 会打印上一
 * 次复位前的快照：运行时长(uptime)、HardFault 次数、以及“最后 1 秒各中断
 * 的入口次数”——若某个中断值异常大就是中断风暴；若全部接近 0 且主循环
 * beat 不涨，则是中断被关/CPU 停住；两者都正常就是主循环内部死循环
 * （此时看 [STUCK] cp= 给出的检查点）。
 * ------------------------------------------------------------------------ */
enum
{
    DBG_ISR_USBPD = 0,   /* USBPD（PD PHY，优先级最高） */
    DBG_ISR_SPI_TX,      /* DMA1_CH3：OLED SPI TX */
    DBG_ISR_I2C_EV,      /* I2C1 事件 */
    DBG_ISR_I2C_ER,      /* I2C1 错误 */
    DBG_ISR_I2C_TX,      /* DMA1_CH6：I2C TX */
    DBG_ISR_I2C_RX,      /* DMA1_CH7：I2C RX */
    DBG_ISR_UART_TX,     /* DMA1_CH4：USART1 TX */
    DBG_ISR_UART_RX,     /* DMA1_CH5：USART1 RX */
    DBG_ISR_COUNT
};

extern volatile uint32_t g_dbg_isr[DBG_ISR_COUNT];
extern volatile uint32_t g_dbg_isr_snap[DBG_ISR_COUNT];
extern volatile uint32_t g_dbg_tick;        /* 上电以来的 1 ms 节拍数 */
extern volatile uint32_t g_dbg_tick_snap;
extern volatile uint32_t g_dbg_fault_count; /* HardFault 次数 */
extern volatile uint32_t g_dbg_i2c_star1;   /* 最近一次 I2C 事件异常的 STAR1 */
extern volatile uint32_t g_dbg_i2c_star2;
extern volatile uint32_t g_dbg_i2c_state;
extern volatile uint32_t g_dbg_i2c_aborts;  /* I2C 事件风暴/标志清不掉而中止的次数 */

#define DBG_ISR_BUMP(i)     do { g_dbg_isr[(i)]++; } while(0)

void DBG_TickHook(void);        /* 由 SysTick 中断调用（计数 + 1s 快照） */
void DBG_BlackboxReport(void);  /* 上电打印上一次复位前的快照 */
void DBG_BlackboxReset(void);   /* 清零并置有效标志（快照打印之后调用） */
void DBG_RecordI2cStorm(uint16_t star1, uint16_t star2, uint32_t state);

#ifndef DEBUG_PRINT_ENABLE
#define DEBUG_PRINT_ENABLE 1
#endif

#if DEBUG_PRINT_ENABLE
#define PRINT(format, ...) printf(format, ##__VA_ARGS__)
#else
#define PRINT(...)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DEBUG_H */
