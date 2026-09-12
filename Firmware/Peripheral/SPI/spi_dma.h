#ifndef SPI_DMA_H
#define SPI_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display SPI transport for the board's remapped SPI1 pins:
 *   PA11 = SCK (AF)
 *   PA10 = MOSI (AF)
 *   PA12 = SPI1 hardware NSS -> panel chip select, driven by the SPI cell
 *   PA9  = GPIO D/C
 * SPI1 TX uses DMA1 Channel3 and completion is interrupt-driven: there is no
 * scheduler service function and no DMA flag polling in the application.
 *
 * With SSOE=1 the NSS pin follows the SPI enable bit, so CS is asserted by
 * SPI_Cmd(ENABLE) and released by SPI_Cmd(DISABLE). */
#define SPI_DMA_CLOCK_HZ_DEFAULT    12000000UL

typedef void (*SPI_DMA_DoneCallback)(void);

void SPI_DMA_Init(void);

/* Register the transfer-complete callback.  It runs in DMA1_Channel3 IRQ
 * context after SPI BSY has fallen and the channel has been disabled. */
void SPI_DMA_SetDoneCallback(SPI_DMA_DoneCallback callback);

/* Start a non-blocking DMA transmit.  Returns 0 when the transport is busy. */
uint8_t SPI_DMA_TryTransmit(const uint8_t *data, uint16_t length);
/* Short command path used by SH1107 init/control only. Returns 0 if DMA owns SPI. */
uint8_t SPI_DMA_WriteBlocking(const uint8_t *data, uint16_t length);
uint8_t SPI_DMA_IsBusy(void);
uint32_t SPI_DMA_GetTransferCount(void);
uint32_t SPI_DMA_GetClockHz(void);

/* Called only by APP/ch32x035_it.c vector wrapper. */
void SPI_DMA_TxDMA_IRQHandler(void);

/* Board display control helpers kept here so the u8g2 byte callback can use
 * one transport header without touching GPIO registers directly. */
void SPI_DMA_DisplaySelect(uint8_t selected);
void SPI_DMA_DisplaySetDataMode(uint8_t data_mode);

#ifdef __cplusplus
}
#endif

#endif /* SPI_DMA_H */
