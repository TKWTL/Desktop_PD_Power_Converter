#ifndef SH1107_DISPLAY_H
#define SH1107_DISPLAY_H

#include <stdint.h>
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH1107_NATIVE_WIDTH          80U
#define SH1107_NATIVE_HEIGHT         128U
#define SH1107_NATIVE_PAGES          16U
#define SH1107_FRAMEBUFFER_BYTES     1280U
#define SH1107_LOGICAL_WIDTH         128U
#define SH1107_LOGICAL_HEIGHT        80U

/* Initialize the externally-VPP-powered SH1107 and bind it to a full u8g2
 * framebuffer. The default rotation is R1, yielding a logical 128x80 canvas. */
void SH1107_Display_Init(u8g2_t *u8g2);

/* Queue the latest u8g2 framebuffer. A 1280-byte snapshot is used so MiaoUI
 * can render the next frame while the previous frame is still on SPI DMA.
 * Page-to-page continuation is driven by the SPI TX-DMA completion interrupt;
 * there is no service function to poll. */
void SH1107_Display_RequestFlush(const uint8_t *framebuffer);
void SH1107_Display_RequestContrast(uint8_t contrast);
void SH1107_Display_RequestPowerSave(uint8_t enable);

uint8_t SH1107_Display_IsReady(void);
uint8_t SH1107_Display_IsBusy(void);
uint32_t SH1107_Display_GetFrameCount(void);

#ifdef __cplusplus
}
#endif

#endif /* SH1107_DISPLAY_H */
