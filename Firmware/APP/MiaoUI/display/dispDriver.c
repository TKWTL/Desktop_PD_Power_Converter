/*
 * MiaoUI display adaptation for the desktop PD converter.
 * Original MiaoUI display abstraction copyright/license: MIT, JFeng-Z.
 */
#include "display/dispDriver.h"
#include "core/ui.h"
#include "sh1107_display.h"

u8g2_t u8g2;
int Contrast = 32;

static ui_item_t s_contrast_item;
static ui_item_t s_background_item;
static ui_item_t s_rotation_item;

void Create_Disp_Parameters(ui_t *ui)
{
    static ui_data_t contrast_data;
    static ui_data_t background_data;
    static ui_data_t rotation_data;
    static ui_element_t contrast_element;
    static ui_element_t background_element;
    static ui_element_t rotation_element;

    contrast_data.name = "Contrast";
    contrast_data.ptr = &Contrast;
    contrast_data.function = Disp_SetContrast;
    contrast_data.functionType = UI_DATA_FUNCTION_STEP_EXECUTE;
    contrast_data.dataType = UI_DATA_INT;
    contrast_data.actionType = UI_DATA_ACTION_RW;
    contrast_data.max = 248;
    contrast_data.min = 0;
    contrast_data.step = 8;
    contrast_data.decimals = 0;
    contrast_element.data = &contrast_data;
    Create_element(&s_contrast_item, &contrast_element);

    background_data.name = "Background";
    background_data.ptr = &ui->bgColor;
    background_data.dataType = UI_DATA_SWITCH;
    background_data.actionType = UI_DATA_ACTION_RW;
    background_element.data = &background_data;
    Create_element(&s_background_item, &background_element);

    rotation_data.name = "Landscape Flip";
    rotation_data.ptr = &ui->rotation;
    rotation_data.function = Disp_ResumeRotation;
    rotation_data.dataType = UI_DATA_SWITCH;
    rotation_data.actionType = UI_DATA_ACTION_RW;
    rotation_element.data = &rotation_data;
    Create_element(&s_rotation_item, &rotation_element);
}

void Add_Disp_Items(ui_page_t *parent_page)
{
    AddItem(" Background", UI_ITEM_DATA, 0, &s_background_item, parent_page, 0, 0);
    AddItem(" Contrast", UI_ITEM_DATA, 0, &s_contrast_item, parent_page, 0, 0);
    AddItem(" Landscape Flip", UI_ITEM_DATA, 0, &s_rotation_item, parent_page, 0, 0);
}

void diapInit(void)
{
    SH1107_Display_Init(&u8g2);
    u8g2_SetFont(&u8g2, UI_FONT);
    SH1107_Display_RequestContrast((uint8_t)Contrast);
    u8g2_ClearBuffer(&u8g2);
}

/* Monotonic count of frames handed to the display transport.  Pages that cache
 * their own pixels use Disp_GetFlushCount() to notice that another page (menu,
 * fade animation, settings) flushed in between, so the panel no longer shows
 * their content and a frame must be pushed again. */
static uint32_t s_flush_count = 0u;

void Disp_SendBuffer(void)
{
    s_flush_count++;
    SH1107_Display_RequestFlush(u8g2_GetBufferPtr(&u8g2));
}

uint32_t Disp_GetFlushCount(void)
{
    return s_flush_count;
}

void Disp_UpdateDisplayArea(uint8_t tx, uint8_t ty, uint8_t tw, uint8_t th)
{
    /* Runtime transport is deliberately full-frame and asynchronous. MiaoUI's
     * first-light pages are small enough that coalescing a 1280-byte frame is
     * simpler and safer for PD timing than synchronous partial updates. */
    (void)tx;
    (void)ty;
    (void)tw;
    (void)th;
    Disp_SendBuffer();
}

void Disp_SetContrast(ui_t *ui)
{
    (void)ui;
    SH1107_Display_RequestContrast((uint8_t)Contrast);
}

void Disp_SetContrast2(uint8_t contrast)
{
    Contrast = (int)contrast;
    SH1107_Display_RequestContrast(contrast);
}

void Disp_SetPowerSave(uint8_t is_enable)
{
    SH1107_Display_RequestPowerSave(is_enable);
}

void Disp_ClearBuffer(void) { u8g2_ClearBuffer(&u8g2); }
void Disp_SetFont(const uint8_t *font) { u8g2_SetFont(&u8g2, font); }
void Disp_DrawPixel(uint16_t x, uint16_t y) { u8g2_DrawPixel(&u8g2, x, y); }
void Disp_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) { u8g2_DrawLine(&u8g2, x1, y1, x2, y2); }
uint16_t Disp_Putchar(uint16_t x, uint16_t y, uint16_t encoding) { return u8g2_DrawGlyph(&u8g2, x, y, encoding); }
uint16_t Disp_DrawStr(uint16_t x, uint16_t y, const char *str) { return u8g2_DrawStr(&u8g2, x, y, str); }
void Disp_SetDrawColor(void *color) { u8g2_SetDrawColor(&u8g2, *(uint8_t *)color); }
void Disp_SetBitmapMode(uint8_t is_transparent) { u8g2_SetBitmapMode(&u8g2, is_transparent); }
void Disp_DrawFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { u8g2_DrawFrame(&u8g2, x, y, w, h); }
void Disp_DrawRFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r) { u8g2_DrawRFrame(&u8g2, x, y, w, h, r); }
void Disp_DrawBox(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { u8g2_DrawBox(&u8g2, x, y, w, h); }
void Disp_DrawRBox(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r) { u8g2_DrawRBox(&u8g2, x, y, w, h, r); }
void Disp_DrawCircle(uint16_t x, uint16_t y, uint16_t r, uint8_t opt) { u8g2_DrawCircle(&u8g2, x, y, r, opt); }
void Disp_DrawDisc(uint16_t x, uint16_t y, uint16_t r, uint8_t opt) { u8g2_DrawDisc(&u8g2, x, y, r, opt); }
void Disp_DrawXBMP(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *bitmap) { u8g2_DrawXBMP(&u8g2, x, y, w, h, bitmap); }
uint8_t Disp_GetBufferTileHeight(void) { return u8g2_GetBufferTileHeight(&u8g2); }
uint8_t Disp_GetBufferTileWidth(void) { return u8g2_GetBufferTileWidth(&u8g2); }
uint8_t *Disp_GetBufferPtr(void) { return u8g2_GetBufferPtr(&u8g2); }
void Disp_SetClipWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) { u8g2_SetClipWindow(&u8g2, x0, y0, x1, y1); }
void Disp_SetMaxClipWindow(void) { u8g2_SetMaxClipWindow(&u8g2); }
void Disp_SetBufferCurrTileRow(uint8_t row) { u8g2_SetBufferCurrTileRow(&u8g2, row); }
uint16_t Disp_DrawUTF8(uint16_t x, uint16_t y, const char *str) { return u8g2_DrawUTF8(&u8g2, x, y, str); }
uint16_t Disp_GetUTF8Width(const char *str) { return u8g2_GetUTF8Width(&u8g2, str); }

void Disp_SetRotation(uint8_t rotation)
{
    u8g2_SetDisplayRotation(&u8g2, rotation ? U8G2_R1 : U8G2_R3);
}

void Disp_SetRotationMode(uint8_t rotation_mode)
{
    static const u8g2_cb_t *const rotations[4] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    u8g2_SetDisplayRotation(&u8g2, rotations[rotation_mode & 3U]);
}

void Disp_ResumeRotation(ui_t *ui)
{
    if(ui != 0)
        Disp_SetRotation(ui->rotation);
}

void Disp_UpdateRotation(ui_t *ui, uint8_t rotation_state)
{
    if(ui != 0)
    {
        ui->rotation = rotation_state ? 1U : 0U;
        Disp_ResumeRotation(ui);
    }
}
