/* Service items/pages for the Desktop PD Power Converter.
 *
 * The menu tree (APP/MiaoUI/ui_conf.c) registers them directly; there is no
 * "Tools" submenu:
 *   Fan Test        settings DATA item (temporary - the automatic thermal
 *                   policy will replace it); every value step drives the
 *                   TIM1_CH1 fan PWM live (0..255, scrollbar wraps at ends);
 *   Soft Reset      Board_SoftReset() - plain PFIC software reset;
 *   Reboot to ISP   Board_RebootToISP() - factory bootloader on next reset;
 *   Burn-in Test    full white screen after a short explanation; any key
 *                   during the white phase leaves.
 *
 * Input map is the product's two-key adapter (K1 = DOWN, K2 = ENTER).  Page
 * functions are pass-based: ui_loop() calls them every ~10 ms, actions arrive
 * in ui->action, and the page either swallows an action (ui->action =
 * UI_ACTION_NONE) or leaves it set so ui.c returns to the menu - the same
 * convention Dashboard_Page() uses.
 */
#include "service_pages.h"

#include "board.h"
#include "debug.h"
#include "fan_pwm.h"
#include "core/ui.h"
#include "display/dispDriver.h"

/* Intro dwell before the white screen; the UI task ticks every 10 ms. */
#define BURNIN_INTRO_TICKS   300u

#define BURNIN_LINE_X        4u
#define BURNIN_LINE1_Y       12u
#define BURNIN_LINE_STEP     13u

static ui_data_t    s_fan_data;
static ui_element_t s_fan_element;
static ui_item_t    s_fan_item;
static ui_item_t    s_soft_reset_item;
static ui_item_t    s_isp_reboot_item;
static int          s_fan_duty;

/* ------------------------------------------------------------------ */
/* Fan speed test                                                     */
/* ------------------------------------------------------------------ */

/* TEMPORARY bring-up control: once the temperature sensor is wired up, an
 * automatic fan policy takes over, this item is deleted and the UI keeps only
 * a "delayed fan trigger point" item.
 *
 * STEP_EXECUTE callback: the dialog's scrollbar has already moved the value,
 * this pushes it to the PWM.  Until the policy lands the duty simply persists
 * after leaving the dialog - wrap it back to 0 to stop. */
static void fan_test_step(ui_t *ui)
{
    (void)ui;

    FAN_PWM_SetDuty8((uint8_t)s_fan_duty);
}

/* ------------------------------------------------------------------ */
/* Menu registration                                                  */
/* ------------------------------------------------------------------ */

void Add_Service_Items(ui_page_t *parent_page)
{
    s_fan_data.name = "Fan Duty";
    s_fan_data.ptr = &s_fan_duty;
    s_fan_data.function = fan_test_step;
    s_fan_data.functionType = UI_DATA_FUNCTION_STEP_EXECUTE;
    s_fan_data.dataType = UI_DATA_INT;
    s_fan_data.actionType = UI_DATA_ACTION_RW;
    s_fan_data.max = 248;
    s_fan_data.min = 0;
    s_fan_data.step = 8;      /* small enough to hear each step while testing */
    s_fan_data.decimals = 0;
    s_fan_element.data = &s_fan_data;
    Create_element(&s_fan_item, &s_fan_element);

    AddItem(" Fan Test", UI_ITEM_DATA, 0, &s_fan_item,
            parent_page, 0, 0);
    AddItem(" Soft Reset", UI_ITEM_WORD, 0, &s_soft_reset_item,
            parent_page, 0, SoftReset_Action);
    AddItem(" Reboot to ISP", UI_ITEM_WORD, 0, &s_isp_reboot_item,
            parent_page, 0, IspReboot_Action);
}

/* ------------------------------------------------------------------ */
/* Reset actions                                                      */
/* ------------------------------------------------------------------ */

void SoftReset_Action(ui_t *ui)
{
    (void)ui;

    printf("[SYS] software reset requested\r\n");
    Delay_Ms(3);              /* let the async UART shift the line out */
    Board_SoftReset();        /* never returns */
}

void IspReboot_Action(ui_t *ui)
{
    (void)ui;

    printf("[SYS] reboot to ISP requested (BOOT_MODE)\r\n");
    Delay_Ms(3);
    Board_RebootToISP();      /* never returns */
}

/* ------------------------------------------------------------------ */
/* Burn-in test                                                       */
/* ------------------------------------------------------------------ */

static void burnin_paint_intro(const ui_t *ui)
{
    uint8_t bg = ui->bgColor;
    uint8_t fg = (uint8_t)(bg ? 0u : 1u);

    Disp_SetDrawColor(&bg);
    Disp_DrawBox(0u, 0u, UI_HOR_RES, UI_VER_RES);
    Disp_SetDrawColor(&fg);
    Disp_DrawStr(BURNIN_LINE_X, BURNIN_LINE1_Y, "Burn-in Test");
    Disp_DrawStr(BURNIN_LINE_X, BURNIN_LINE1_Y + BURNIN_LINE_STEP, "Checks the OLED for");
    Disp_DrawStr(BURNIN_LINE_X, BURNIN_LINE1_Y + 2u * BURNIN_LINE_STEP, "decay after many");
    Disp_DrawStr(BURNIN_LINE_X, BURNIN_LINE1_Y + 3u * BURNIN_LINE_STEP, "hours of light-up.");
    Disp_DrawStr(BURNIN_LINE_X, BURNIN_LINE1_Y + 4u * BURNIN_LINE_STEP, "White screen - press");
    Disp_DrawStr(BURNIN_LINE_X, BURNIN_LINE1_Y + 5u * BURNIN_LINE_STEP, "any key to leave.");
    Disp_SendBuffer();
}

static void burnin_paint_white(void)
{
    uint8_t color = 1u;       /* full white, independent of the menu theme */

    Disp_SetDrawColor(&color);
    Disp_DrawBox(0u, 0u, UI_HOR_RES, UI_VER_RES);
    Disp_SendBuffer();
}

void Burnin_Page(ui_t *ui)
{
    static uint8_t  s_phase;   /* 0 = explanation, 1 = white screen */
    static uint16_t s_ticks;

    switch(s_phase)
    {
        case 0u:
            if(ui->action != UI_ACTION_NONE)
            {
                /* A key during the explanation skips straight to the white
                 * screen (same behaviour as the Firmware_0 reference); the
                 * action is consumed so the page stays open. */
                ui->action = UI_ACTION_NONE;
                burnin_paint_white();
                s_phase = 1u;
                break;
            }

            if(s_ticks == 0u)
                burnin_paint_intro(ui);

            s_ticks++;
            if(s_ticks >= BURNIN_INTRO_TICKS)
            {
                burnin_paint_white();
                s_phase = 1u;
            }
            break;

        case 1u:
            if(ui->action != UI_ACTION_NONE)
            {
                /* Any key ends the white screen.  Re-arm the state before
                 * returning; ui.c switches back to the menu because the
                 * action is left set (same exit convention as the dashboard). */
                s_phase = 0u;
                s_ticks = 0u;
            }
            break;

        default:
            s_phase = 0u;
            s_ticks = 0u;
            break;
    }
}
