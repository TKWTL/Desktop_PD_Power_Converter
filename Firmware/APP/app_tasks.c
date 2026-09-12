#include "app_tasks.h"
#include "coroOS.h"
#include "debug.h"
#include "pd.h"
#include "board.h"
#include "vbus_sense.h"
#include "i2c_api.h"
#include "soft_i2c.h"
#include "sw3526.h"
#include "sw3538.h"
#include "fan_pwm.h"
#include "spi_dma.h"
#include "sh1107_display.h"
#include "buttons.h"
#include "ui_conf.h"
#include "core/ui.h"
#include "display/dispDriver.h"
#include "time_api.h"

/* ------------------------------------------------------------------------
 * Freeze diagnostics (checkpoint + stall reporter + watchdog)
 *
 * coroOS is cooperative with no preemption, so one stuck busy-wait freezes the
 * whole product - UI included.  Three aids:
 *
 *  - DBG_CP(n) records the last code region entered (map below);
 *  - APP_DbgStallCheck(), called from the 1 ms SysTick handler, notices that
 *    the main loop stopped advancing and dumps the checkpoint over a polled
 *    UART path BEFORE the IWDG fires - this works even when RAM is not kept;
 *  - the IWDG then resets the MCU instead of leaving the board dead until
 *    someone unplugs it.
 *
 * The checkpoint is also mirrored across the reset in RAM: the startup code
 * clears only .bss, and Link.ld keeps .noinit outside it - and, important,
 * before the _sbrk() heap (newlib allocates the stdio buffer on the first
 * printf and used to overwrite exactly these bytes).
 *
 * Checkpoint map: 1 idle | 11 PD | 12 VBUS | 13 PD done | 14 VBUS done |
 *                 21 UI keys | 22 UI loop | 23 UI done |
 *                 31 soft-I2C #1 | 32 soft-I2C #2 | 33 soft-I2C done |
 *                 41 I2C watchdog | 42 watchdog done | 51 SW3538 | 52 SW3526#1 |
 *                 53 SW3526#2
 * ---------------------------------------------------------------------- */
#define DBG_CP_MAGIC   0xD06F5A5Au
#define DBG_STALL_MS   600u

volatile uint32_t g_dbg_checkpoint __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_magic      __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_loop_beat;

#define DBG_CP(code)    do { g_dbg_checkpoint = (uint32_t)(code); } while(0)

static coro_scheduler_t s_scheduler;

static SoftI2C_Handle s_sw3526_bus1;
static SoftI2C_Handle s_sw3526_bus2;
static SW3526_Handle s_sw3526_1;
static SW3526_Handle s_sw3526_2;
static SW3538_Handle s_sw3538;

/* MiaoUI input adapter intentionally references this product-level handle. */
ui_t ui;

static void APP_PrintResetCause(void)
{
    uint8_t any = 0u;
    printf("[RESET] cause:");
    if(RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET) { printf(" POR/PDR"); any = 1u; }
    if(RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET) { printf(" PIN"); any = 1u; }
    if(RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET) { printf(" SW"); any = 1u; }
    if(RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET) { printf(" IWDG"); any = 1u; }
    if(RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET) { printf(" WWDG"); any = 1u; }
    if(RCC_GetFlagStatus(RCC_FLAG_LPWRRST) != RESET) { printf(" LPWR"); any = 1u; }
    if(!any) printf(" unknown/cleared");
    printf("\r\n");
    RCC_ClearFlag();
}

/* Polled UART fallback used only when the scheduler has stalled: the normal
 * DMA/ring printf path cannot be trusted and the IWDG reset is imminent. */
static void APP_DbgEmergencyPutc(char c)
{
    uint32_t guard = 200000u;

    while((USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) && guard)
        guard--;

    if(guard)
        USART_SendData(USART1, (uint8_t)c);
}

static void APP_DbgEmergencyWrite(const char *text)
{
    while(*text)
        APP_DbgEmergencyPutc(*text++);
}

static void APP_DbgEmergencyU32(uint32_t v)
{
    char buf[11];
    uint8_t i = 0u;

    do
    {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while((v != 0u) && (i < (uint8_t)(sizeof(buf) - 1u)));

    while(i != 0u)
        APP_DbgEmergencyPutc(buf[--i]);
}

void APP_DbgStallCheck(void)
{
    static uint32_t beat_last;
    static uint32_t idle_since_ms;
    static uint8_t  reported;
    uint32_t beat = g_dbg_loop_beat;
    uint32_t now = TIME_Millis();

    if(beat != beat_last)
    {
        beat_last = beat;
        idle_since_ms = now;
        reported = 0u;
        return;
    }

    if((reported != 0u) || ((uint32_t)(now - idle_since_ms) < DBG_STALL_MS))
        return;

    reported = 1u;

    /* No main-loop progress for DBG_STALL_MS: dump where it stopped, then let
     * the IWDG finish the recovery. */
    USART_DMACmd(USART1, USART_DMAReq_Tx, DISABLE);
    DMA_Cmd(DMA1_Channel4, DISABLE);
    APP_DbgEmergencyWrite("\r\n[STUCK] cp=");
    APP_DbgEmergencyU32(g_dbg_checkpoint);
    APP_DbgEmergencyWrite("\r\n");
}

THRD_DECLARE(thread_pd)
{
    THRD_BEGIN;
    while(1)
    {
        DBG_CP(11);
        PD_Task(TIME_Millis());
        DBG_CP(13);
        THRD_YIELD;
    }
    THRD_END;
}

THRD_DECLARE(thread_vbus)
{
    static uint16_t mv;
    static uint8_t log_div;
    THRD_BEGIN;
    log_div = 0u;
    while(1)
    {
        THRD_DELAY(20u);
        DBG_CP(12);
        mv = VBUS_Sense_ReadMillivolts();
        PD_SetVbusMillivolts(mv);
        DBG_CP(14);
        if(++log_div >= 50u)
        {
            log_div = 0u;
            printf("[VBUS] %u mV\r\n", (unsigned)mv);
        }
    }
    THRD_END;
}


THRD_DECLARE(thread_ui)
{
    THRD_BEGIN;

    /* The product UI is deliberately independent of the USB-PD state machine:
     * it must come up on DC input, on a plain 5 V source, or when EPR
     * negotiation never completes.  The SH1107 init is a short command burst on
     * the interrupt-driven SPI transport (the TK078F288 sequence carries no
     * ms delays) and frames are DMA/IRQ driven, so nothing here needs to wait
     * for PD_IsPowerReady() any more. */
    diapInit();
    Key_Init();
    MiaoUi_Setup(&ui);
    printf("[OLED] SH1107 ready: logical 128x80, native 80x128, external VPP, charge pump disabled\r\n");

    while(1)
    {
        THRD_DELAY(10u);
        DBG_CP(21);
        Key_DebounceService_10ms();
        Key_Scand();
        DBG_CP(22);
        ui_loop(&ui);
        DBG_CP(23);
    }
    THRD_END;
}

THRD_DECLARE(thread_soft_i2c_service)
{
    THRD_BEGIN;
    while(1)
    {
        DBG_CP(31);
        SoftI2C_Service(&s_sw3526_bus1);
        DBG_CP(32);
        SoftI2C_Service(&s_sw3526_bus2);
        DBG_CP(33);
        THRD_YIELD;
    }
    THRD_END;
}

THRD_DECLARE(thread_i2c_watchdog)
{
    static uint32_t last_recovery_count;
    static I2C_API_Diagnostic d;
    THRD_BEGIN;
    last_recovery_count = I2C_API_GetRecoveryCount();
    while(1)
    {
        THRD_DELAY(10u);
        DBG_CP(41);
        I2C_API_WatchdogService(TIME_Millis());
        DBG_CP(42);
        if(I2C_API_GetRecoveryCount() != last_recovery_count)
        {
            last_recovery_count = I2C_API_GetRecoveryCount();
            I2C_API_GetDiagnostic(&d);
            printf("[I2C] recovery #%lu: %s SCL=%u SDA=%u STAR1=0x%04x STAR2=0x%04x\r\n",
                   (unsigned long)last_recovery_count, I2C_API_GetLastErrorName(),
                   d.scl_high, d.sda_high, d.star1, d.star2);
        }
    }
    THRD_END;
}

/* Device mirror polling.
 *
 * Each converter keeps a cached status/telemetry mirror (protocol, port state,
 * VIN/VOUT/IOUT) that the UI reads without touching I2C.  All three mirrors are
 * refreshed on one plain 500 ms cadence; the I2C chain time (write-enable
 * sequence plus 3-4 ADC channels) adds a few ms on top, which is irrelevant for
 * telemetry. */
#define SW_MIRROR_POLL_MS   500u

/* Keep device mirrors fresh without blocking the cooperative scheduler.  Each
 * handle has its own persistent transfer buffers/sub-coroutine state. */
THRD_DECLARE(thread_sw3538)
{
    THRD_BEGIN;
    while(1)
    {
        THRD_DELAY(SW_MIRROR_POLL_MS);
        DBG_CP(51);
        THRD_SPAWN_ARGS(SW3538_StatusLoad, &s_sw3538);
        if(SW3538_IsOnline(&s_sw3538))
            THRD_SPAWN_ARGS(SW3538_ADCLoad, &s_sw3538);
    }
    THRD_END;
}

THRD_DECLARE(thread_sw3526_1)
{
    THRD_BEGIN;
    while(1)
    {
        THRD_DELAY(SW_MIRROR_POLL_MS);
        DBG_CP(52);
        THRD_SPAWN_ARGS(SW3526_StatusLoad, &s_sw3526_1);
        if(SW3526_IsOnline(&s_sw3526_1))
            THRD_SPAWN_ARGS(SW3526_ADCLoad, &s_sw3526_1);
    }
    THRD_END;
}

THRD_DECLARE(thread_sw3526_2)
{
    THRD_BEGIN;
    while(1)
    {
        THRD_DELAY(SW_MIRROR_POLL_MS);
        DBG_CP(53);
        THRD_SPAWN_ARGS(SW3526_StatusLoad, &s_sw3526_2);
        if(SW3526_IsOnline(&s_sw3526_2))
            THRD_SPAWN_ARGS(SW3526_ADCLoad, &s_sw3526_2);
    }
    THRD_END;
}

static const coro_thread_fn_t s_threads[] =
{
    thread_pd,
    thread_vbus,
    thread_ui,
    thread_soft_i2c_service,
    thread_i2c_watchdog,
    thread_sw3538,
    thread_sw3526_1,
    thread_sw3526_2
};
static coro_pt_t s_thread_states[sizeof(s_threads) / sizeof(s_threads[0])];

void APP_Tasks_Init(void)
{
    printf("\r\n=== Desktop PD Power Converter / CH32X035C8T6 ===\r\n");
    APP_PrintResetCause();
    if(g_dbg_magic == DBG_CP_MAGIC)
        printf("[DBG] checkpoint retained=%lu\r\n",
               (unsigned long)g_dbg_checkpoint);
    else
        printf("[DBG] checkpoint not retained (magic=%08lX)\r\n",
               (unsigned long)g_dbg_magic);
    g_dbg_magic = DBG_CP_MAGIC;
    g_dbg_checkpoint = 0u;

    /* 上一次复位前的黑匣子快照（中断风暴/停机判定，见 debug.h） */
    DBG_BlackboxReport();
    DBG_BlackboxReset();
    printf("SystemClk:%lu Hz ChipID:%08lx\r\n",
           (unsigned long)SystemCoreClock, (unsigned long)DBGMCU_GetCHIPID());
    printf("UART1: PB10 TX / PB11 RX @ 921600, DMA async\r\n");
    printf("Fan PWM: PB9 TIM1_CH1 @ %lu Hz, %u levels\r\n",
           (unsigned long)FAN_PWM_GetFrequencyHz(), (unsigned)(FAN_PWM_MAX_LEVEL + 1U));
    printf("Display SPI1: PA11 SCK / PA10 MOSI @ %lu Hz, DMA1 CH3 TX + IRQ, NSS=PA12\r\n",
           (unsigned long)SPI_DMA_GetClockHz());

    Board_Init();
    FAN_PWM_Init();
    SPI_DMA_Init();
    VBUS_Sense_Init();

    I2C_API_Init(I2C_API_DEFAULT_CLOCK_HZ);
    SW3538_HandleInit(&s_sw3538, I2C_API_OWNER_SW3538);
    printf("I2C1: PA13 SCL / PA14 SDA @ %lu Hz, EV/ER + TX/RX DMA IRQ; SW3538 handle ready\r\n",
           (unsigned long)I2C_API_GetClockHz());

    s_sw3526_bus1.scl_port = SW3526_1_SCL_GPIO_Port;
    s_sw3526_bus1.scl_pin = SW3526_1_SCL_Pin;
    s_sw3526_bus1.sda_port = SW3526_1_SDA_GPIO_Port;
    s_sw3526_bus1.sda_pin = SW3526_1_SDA_Pin;
    s_sw3526_bus1.half_period_us = SOFT_I2C_DEFAULT_HALF_PERIOD_US;
    s_sw3526_bus1.stretch_timeout_us = SOFT_I2C_DEFAULT_STRETCH_TIMEOUT_US;
    SoftI2C_Init(&s_sw3526_bus1);
    SW3526_HandleInit(&s_sw3526_1, &s_sw3526_bus1);

    s_sw3526_bus2.scl_port = SW3526_2_SCL_GPIO_Port;
    s_sw3526_bus2.scl_pin = SW3526_2_SCL_Pin;
    s_sw3526_bus2.sda_port = SW3526_2_SDA_GPIO_Port;
    s_sw3526_bus2.sda_pin = SW3526_2_SDA_Pin;
    s_sw3526_bus2.half_period_us = SOFT_I2C_DEFAULT_HALF_PERIOD_US;
    s_sw3526_bus2.stretch_timeout_us = SOFT_I2C_DEFAULT_STRETCH_TIMEOUT_US;
    SoftI2C_Init(&s_sw3526_bus2);
    SW3526_HandleInit(&s_sw3526_2, &s_sw3526_bus2);
    printf("Soft-I2C: SW3526#1 PA4/PA3, SW3526#2 PA1/PA2; independent handles\r\n");

    printf("PD Sink: SPR <= %u mV / %u mA; EPR <= %u mV / %u mA\r\n",
           (unsigned)PD_REQUEST_MAX_FIXED_MV,
           (unsigned)PD_SPR_REQUEST_MAX_MA,
           (unsigned)PD_EPR_TARGET_MV,
           (unsigned)PD_EPR_REQUEST_MAX_MA);

    CoroOS_Init(&s_scheduler, s_threads, s_thread_states,
                (uint8_t)(sizeof(s_threads) / sizeof(s_threads[0])));

    /* Last line of defence against a stuck cooperative thread: reset instead
     * of freezing until someone unplugs the board.  The next boot prints the
     * reset cause (IWDG) and the checkpoint captured in .noinit. */
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_32);
    IWDG_SetReload(4000u);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void APP_Tasks_RunOnce(void)
{
    CoroOS_RunOnce(&s_scheduler);
}

void APP_Tasks_Idle(void)
{
    /* Every main-loop pass reaches this hook, so a healthy scheduler keeps the
     * IWDG fed (see the checkpoint block at the top of this file). */
    IWDG_ReloadCounter();
    g_dbg_loop_beat++;
    DBG_CP(1);

    /* coroOS is a flat round-robin scheduler with no idle thread, so this hook
     * is the idle point of the application.  Raw interrupt paths (SPI/DMA,
     * I2C EV/ER + DMA, USART DMA, 1 ms SysTick tick) wake the core straight out
     * of WFI, but two software state machines are advanced *only* from the main
     * loop and must not be slowed to one pass per millisecond:
     *
     *  - software I2C: SoftI2C_Service() moves one edge per pass with a 5 us
     *    edge spacing, so a 1 ms sleep would stretch a byte by two orders of
     *    magnitude;
     *  - USB-PD: the Sink policy state machine produces the sender-response
     *    latency (measured 5 ms with a fast loop) and a Source gives up once it
     *    grows past its ~24 ms window.
     *
     * Both keep the core spinning, everything else lets it sleep. */
    if(SoftI2C_IsBusy(&s_sw3526_bus1) || SoftI2C_IsBusy(&s_sw3526_bus2))
        return;

    if(PD_WantsFastPoll())
        return;

    __WFI();
}

SW3538_Handle *APP_GetSW3538(void)
{
    return &s_sw3538;
}

SW3526_Handle *APP_GetSW3526_1(void)
{
    return &s_sw3526_1;
}

SW3526_Handle *APP_GetSW3526_2(void)
{
    return &s_sw3526_2;
}
