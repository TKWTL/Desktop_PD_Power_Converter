/* MiaoUI display adaptation for CH32X035 + SH1107. MiaoUI MIT license applies. */
#ifndef MIAOUI_DISPDRIVER_H
#define MIAOUI_DISPDRIVER_H

#include "ui_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u8g2_t u8g2;
extern int Contrast;

void diapInit(void);
void Disp_SendBuffer(void);
/* Frames handed to the transport so far; see dispDriver.c for the page cache
 * use case (a page must re-send when another page flushed in between). */
uint32_t Disp_GetFlushCount(void);
void Disp_UpdateDisplayArea(uint8_t tx, uint8_t ty, uint8_t tw, uint8_t th);
void Disp_SetContrast(ui_t *ui);
void Disp_SetContrast2(uint8_t contrast);
void Disp_SetPowerSave(uint8_t is_enable);

void Disp_ClearBuffer(void);
void Disp_SetFont(const uint8_t *font);
void Disp_DrawPixel(uint16_t x, uint16_t y);
void Disp_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
uint16_t Disp_Putchar(uint16_t x, uint16_t y, uint16_t encoding);
uint16_t Disp_DrawStr(uint16_t x, uint16_t y, const char *str);
void Disp_SetDrawColor(void *color);
void Disp_SetBitmapMode(uint8_t is_transparent);
void Disp_DrawFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void Disp_DrawRFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r);
void Disp_DrawBox(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void Disp_DrawRBox(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r);
void Disp_DrawCircle(uint16_t x, uint16_t y, uint16_t r, uint8_t opt);
void Disp_DrawDisc(uint16_t x, uint16_t y, uint16_t r, uint8_t opt);
void Disp_DrawXBMP(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bitmap);
uint8_t Disp_GetBufferTileHeight(void);
uint8_t Disp_GetBufferTileWidth(void);
uint8_t *Disp_GetBufferPtr(void);
void Disp_SetClipWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void Disp_SetMaxClipWindow(void);
void Disp_SetBufferCurrTileRow(uint8_t row);
uint16_t Disp_DrawUTF8(uint16_t x, uint16_t y, const char *str);
uint16_t Disp_GetUTF8Width(const char *str);

/* MiaoUI uses two landscape directions (R1/R3). Full R0..R3 remains exposed
 * for bring-up/testing through Disp_SetRotationMode(). */
void Disp_SetRotation(uint8_t rotation);
void Disp_SetRotationMode(uint8_t rotation_mode);
void Disp_ResumeRotation(ui_t *ui);
void Disp_UpdateRotation(ui_t *ui, uint8_t rotation_state);

void Create_Disp_Parameters(ui_t *ui);
void Add_Disp_Items(ui_page_t *parent_page);

#ifdef __cplusplus
}
#endif

#endif /* MIAOUI_DISPDRIVER_H */
