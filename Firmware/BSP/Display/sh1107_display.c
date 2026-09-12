#include "sh1107_display.h"
#include "spi_dma.h"
#include "time_api.h"
#include "ch32x035.h"
#include <string.h>

static u8g2_t *s_u8g2;
static uint8_t s_tx_frame[SH1107_FRAMEBUFFER_BYTES];
static const uint8_t *s_latest_frame;
static volatile uint8_t s_ready;
static volatile uint8_t s_frame_active;
static volatile uint8_t s_flush_pending;
static volatile uint8_t s_page;
static volatile uint8_t s_contrast_pending;
static uint8_t s_contrast;
static volatile uint8_t s_power_pending;
static uint8_t s_power_save;
static volatile uint32_t s_frame_count;

static void SH1107_ApplyPendingControl(void);
static void SH1107_OnSpiDone(void);

static uint8_t SH1107_U8X8_Byte(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;

    switch(msg)
    {
        case U8X8_MSG_BYTE_INIT:
            return 1U;

        case U8X8_MSG_BYTE_START_TRANSFER:
            SPI_DMA_DisplaySelect(1U);
            return 1U;

        case U8X8_MSG_BYTE_END_TRANSFER:
            SPI_DMA_DisplaySelect(0U);
            return 1U;

        case U8X8_MSG_BYTE_SET_DC:
            SPI_DMA_DisplaySetDataMode(arg_int ? 1U : 0U);
            return 1U;

        case U8X8_MSG_BYTE_SEND:
            return SPI_DMA_WriteBlocking((const uint8_t *)arg_ptr, arg_int);

        default:
            return 0U;
    }
}

static uint8_t SH1107_U8X8_GPIOAndDelay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch(msg)
    {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            return 1U;

        /* OLED RES is an RC-only hardware reset on this board. */
        case U8X8_MSG_GPIO_RESET:
            return 1U;

        case U8X8_MSG_DELAY_MILLI:
            if(arg_int)
                TIME_DelayMs(arg_int);
            return 1U;

        case U8X8_MSG_DELAY_10MICRO:
            if(arg_int)
                TIME_DelayUs((uint32_t)arg_int * 10U);
            return 1U;

        case U8X8_MSG_DELAY_100NANO:
        case U8X8_MSG_DELAY_NANO:
            /* Hardware SPI satisfies these sub-us setup/hold requirements. */
            return 1U;

        default:
            return 1U;
    }
}

static void SH1107_CopyLatestFrame(void)
{
    if(s_latest_frame != 0)
        memcpy(s_tx_frame, s_latest_frame, SH1107_FRAMEBUFFER_BYTES);
}

static uint8_t SH1107_StartCurrentPage(void)
{
    uint8_t cmd[3];
    const uint8_t *page_data;

    if(s_page >= SH1107_NATIVE_PAGES || SPI_DMA_IsBusy())
        return 0U;

    cmd[0] = 0x10U;                         /* column high nibble, x=0 */
    cmd[1] = 0x00U;                         /* column low nibble, x=0 */
    cmd[2] = (uint8_t)(0xB0U | s_page);     /* page 0..15 */
    page_data = &s_tx_frame[(uint16_t)s_page * SH1107_NATIVE_WIDTH];

    SPI_DMA_DisplaySelect(1U);
    SPI_DMA_DisplaySetDataMode(0U);
    if(!SPI_DMA_WriteBlocking(cmd, (uint16_t)sizeof(cmd)))
    {
        SPI_DMA_DisplaySelect(0U);
        return 0U;
    }

    SPI_DMA_DisplaySetDataMode(1U);
    if(!SPI_DMA_TryTransmit(page_data, SH1107_NATIVE_WIDTH))
    {
        SPI_DMA_DisplaySelect(0U);
        return 0U;
    }
    return 1U;
}

void SH1107_Display_Init(u8g2_t *u8g2)
{
    s_u8g2 = u8g2;
    s_ready = 0U;
    s_frame_active = 0U;
    s_flush_pending = 0U;
    s_latest_frame = 0;
    s_page = 0U;
    s_contrast_pending = 0U;
    s_power_pending = 0U;
    s_frame_count = 0U;

    /* TK078F288 is electrically/native 80x128. R1 exposes the product's
     * desired logical 128x80 landscape canvas; R3 is the alternate landscape. */
    u8g2_Setup_sh1107_tk078f288_80x128_f(s_u8g2,
                                           U8G2_R3,
                                           SH1107_U8X8_Byte,
                                           SH1107_U8X8_GPIOAndDelay);
    u8g2_InitDisplay(s_u8g2);
    u8g2_SetPowerSave(s_u8g2, 0U);
    u8g2_ClearBuffer(s_u8g2);

    /* The page chain is continued from the SPI TX-DMA completion interrupt. */
    SPI_DMA_SetDoneCallback(SH1107_OnSpiDone);
    s_ready = 1U;
}

void SH1107_Display_RequestFlush(const uint8_t *framebuffer)
{
    if(!s_ready || framebuffer == 0)
        return;

    /* Keep the page chain coherent while the frame reference is retargeted. */
    NVIC_DisableIRQ(DMA1_Channel3_IRQn);

    s_latest_frame = framebuffer;
    if(!s_frame_active)
    {
        SH1107_CopyLatestFrame();
        s_latest_frame = 0;
        s_page = 0U;
        s_frame_active = 1U;
        s_flush_pending = 0U;
        if(!SH1107_StartCurrentPage())
        {
            /* Nothing is in flight (SPI cell wedged / transfer still busy).
             * Release the state machine so a later request can start a fresh
             * frame instead of leaving s_frame_active set forever. */
            s_frame_active = 0U;
            s_page = 0U;
        }
    }
    else
    {
        /* Coalesce intermediate animation frames; the newest one is copied as
         * soon as the active frame has been sent. */
        s_flush_pending = 1U;
    }

    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

void SH1107_Display_RequestContrast(uint8_t contrast)
{
    s_contrast = contrast;
    s_contrast_pending = 1U;

    /* Apply right away when the transport is idle; otherwise the completion
     * callback runs it at the end of the active frame. */
    NVIC_DisableIRQ(DMA1_Channel3_IRQn);
    if(!s_frame_active && (SPI_DMA_IsBusy() == 0U))
        SH1107_ApplyPendingControl();
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

void SH1107_Display_RequestPowerSave(uint8_t enable)
{
    s_power_save = enable ? 1U : 0U;
    s_power_pending = 1U;

    NVIC_DisableIRQ(DMA1_Channel3_IRQn);
    if(!s_frame_active && (SPI_DMA_IsBusy() == 0U))
        SH1107_ApplyPendingControl();
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}

static void SH1107_ApplyPendingControl(void)
{
    if(s_contrast_pending)
    {
        s_contrast_pending = 0U;
        u8g2_SetContrast(s_u8g2, s_contrast);
    }

    if(s_power_pending)
    {
        s_power_pending = 0U;
        u8g2_SetPowerSave(s_u8g2, s_power_save);
    }
}

/* SPI TX-DMA completion (IRQ context): SPI BSY has fallen and the DMA channel
 * is disabled, so the panel chip select can be released and the next page
 * started.  Command writes issued here are two or three bytes at 12 MHz. */
static void SH1107_OnSpiDone(void)
{
    SPI_DMA_DisplaySelect(0U);

    if(!s_frame_active)
        return;

    ++s_page;
    if(s_page < SH1107_NATIVE_PAGES)
    {
        (void)SH1107_StartCurrentPage();
        return;
    }

    s_frame_active = 0U;
    ++s_frame_count;

    if(s_flush_pending && (s_latest_frame != 0))
    {
        SH1107_CopyLatestFrame();
        s_latest_frame = 0;
        s_flush_pending = 0U;
        s_page = 0U;
        s_frame_active = 1U;
        if(!SH1107_StartCurrentPage())
        {
            s_frame_active = 0U;
            SH1107_ApplyPendingControl();
        }
        return;
    }

    /* Bus idle: pending contrast/power commands can go out now. */
    SH1107_ApplyPendingControl();
}

uint8_t SH1107_Display_IsReady(void)
{
    return s_ready;
}

uint8_t SH1107_Display_IsBusy(void)
{
    return (s_frame_active || s_flush_pending || SPI_DMA_IsBusy()) ? 1U : 0U;
}

uint32_t SH1107_Display_GetFrameCount(void)
{
    return s_frame_count;
}
