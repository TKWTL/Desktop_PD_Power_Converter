/* Dashboard page for the Desktop PD Power Converter.
 *
 * This is the function page the UI boots into.  It is a pure consumer of the
 * 500 ms device mirrors: the UI never touches I2C itself, so the page only
 * formats cached telemetry.
 *
 * Layout (128x80 panel, 6x12 font, 21 characters per line):
 *   row 1              board input state: temperature / input voltage /
 *                      power limit / PD input state
 *   rows 2..6          three columns: SW3538 "TypeA+C", SW3526 #1 "TypeC1",
 *                      SW3526 #2 "TypeC2"; from top to bottom:
 *                      port name / output voltage / current / power / protocol
 *
 *   row baseline      y = 12 + 13 * (row - 1)   (1 px top margin, 1 px gap)
 *   column right edge 43 px (7 chars) / 85 px (6 chars) / 127 px (6 chars)
 * Values are right aligned, so a wider number eats into the one character gap
 * instead of leaving its column.
 */
#include "dashboard.h"

#include "app_tasks.h"
#include "display/dispDriver.h"
#include "pd.h"
#include "vbus_sense.h"

#include <stdio.h>
#include <string.h>

#define DASH_ROW1_Y        12u
#define DASH_ROW_STEP      13u
#define DASH_X_MARGIN      1u
#define DASH_COL1_RIGHT    43u
#define DASH_COL2_RIGHT    85u
#define DASH_COL3_RIGHT    127u
#define DASH_COL2_X        (DASH_X_MARGIN + 8u * UI_FONT_WIDTH)
#define DASH_COL3_X        (DASH_X_MARGIN + 15u * UI_FONT_WIDTH)

#define DASH_NAME_SW3538    "TypeA+C"
#define DASH_NAME_SW3526_1  "TypeC1"
#define DASH_NAME_SW3526_2  "TypeC2"

#define DASH_OFFLINE_TEXT   "---"
#define DASH_TEXT_LEN       24u
#define DASH_VALUE_LEN      12u
#define DASH_SIGNATURE_LEN  160u
#define DASH_COLUMN_COUNT   3u

typedef struct
{
    const char *name;
    char volt[DASH_VALUE_LEN];
    char amp[DASH_VALUE_LEN];
    char watt[DASH_VALUE_LEN];
    const char *protocol;
} dash_column_t;

static void dash_draw_right(uint16_t right_x, uint16_t y, const char *text)
{
    int16_t x = (int16_t)right_x - (int16_t)(strlen(text) * UI_FONT_WIDTH);

    if(x < 0)
        x = 0;

    Disp_DrawStr((uint16_t)x, y, text);
}

static void dash_draw_column(const dash_column_t *col, uint16_t name_x, uint16_t right_x)
{
    uint16_t y = (uint16_t)(DASH_ROW1_Y + DASH_ROW_STEP);   /* row 2: port name */

    Disp_DrawStr(name_x, y, col->name);
    dash_draw_right(right_x, (uint16_t)(y + DASH_ROW_STEP), col->volt);
    dash_draw_right(right_x, (uint16_t)(y + 2u * DASH_ROW_STEP), col->amp);
    dash_draw_right(right_x, (uint16_t)(y + 3u * DASH_ROW_STEP), col->watt);
    dash_draw_right(right_x, (uint16_t)(y + 4u * DASH_ROW_STEP), col->protocol);
}

/* Value formatting.  Everything is integer/fixed-point on this MCU: the
 * firmware provides its own integer-only printf (Project/Debug/debug.c), so
 * the image no longer carries soft-float printf support and "%f" must not be
 * used.  Values are printed as "whole.fraction" with the same number of
 * decimals as before (the fraction is truncated, not rounded). */
static void dash_fmt_volt(char *buf, uint16_t mv)
{
    snprintf(buf, DASH_VALUE_LEN, "%u.%02uV",
             (unsigned)(mv / 1000u), (unsigned)((mv % 1000u) / 10u));
}

static void dash_fmt_amp(char *buf, uint32_t ma_x10)
{
    snprintf(buf, DASH_VALUE_LEN, "%u.%03uA",
             (unsigned)(ma_x10 / 10000u), (unsigned)((ma_x10 % 10000u) / 10u));
}

/* Power keeps two decimals from 10 W up and three below that: the SW3538 column
 * is seven characters wide ("139.02W") while SW3526 never exceeds 65 W
 * ("65.120W"), so a one-decimal form is not needed. */
static void dash_fmt_watt(char *buf, uint32_t mw)
{
    if(mw >= 10000u)
        snprintf(buf, DASH_VALUE_LEN, "%u.%02uW",
                 (unsigned)(mw / 1000u), (unsigned)((mw % 1000u) / 10u));
    else
        snprintf(buf, DASH_VALUE_LEN, "%u.%03uW",
                 (unsigned)(mw / 1000u), (unsigned)(mw % 1000u));
}

static void dash_column_offline(dash_column_t *col)
{
    snprintf(col->volt, DASH_VALUE_LEN, "%s", DASH_OFFLINE_TEXT);
    snprintf(col->amp, DASH_VALUE_LEN, "%s", DASH_OFFLINE_TEXT);
    snprintf(col->watt, DASH_VALUE_LEN, "%s", DASH_OFFLINE_TEXT);
    col->protocol = DASH_OFFLINE_TEXT;
}

static const char *dash_protocol_sw3538(uint8_t protocol)
{
    switch(protocol)
    {
        case SW3538_PROTOCOL_QC2:       return "QC2";
        case SW3538_PROTOCOL_QC3:       return "QC3";
        case SW3538_PROTOCOL_QC3P:      return "QC3+";
        case SW3538_PROTOCOL_FCP:       return "FCP";
        case SW3538_PROTOCOL_SCP:       return "SCP";
        case SW3538_PROTOCOL_PD_FIXED:  return "PD";
        case SW3538_PROTOCOL_PD_PPS:    return "PPS";
        case SW3538_PROTOCOL_PE11:      return "PE1.1";
        case SW3538_PROTOCOL_PE20:      return "PE2.0";
        case SW3538_PROTOCOL_VOOC10:    return "VOOC";
        case SW3538_PROTOCOL_VOOC40:    return "VOOC";
        case SW3538_PROTOCOL_SFCP:      return "SFCP";
        case SW3538_PROTOCOL_AFC:       return "AFC";
        case SW3538_PROTOCOL_TFCP:      return "TFCP";
        default:                        return DASH_OFFLINE_TEXT;
    }
}

static const char *dash_protocol_sw3526(uint8_t protocol)
{
    switch(protocol)
    {
        case SW3526_PROTOCOL_QC2:       return "QC2";
        case SW3526_PROTOCOL_QC3:       return "QC3";
        case SW3526_PROTOCOL_FCP:       return "FCP";
        case SW3526_PROTOCOL_SCP:       return "SCP";
        case SW3526_PROTOCOL_PD_FIXED:  return "PD";
        case SW3526_PROTOCOL_PD_PPS:    return "PPS";
        case SW3526_PROTOCOL_PE11:      return "PE1.1";
        case SW3526_PROTOCOL_PE20:      return "PE2.0";
        case SW3526_PROTOCOL_VOOC:      return "VOOC";
        case SW3526_PROTOCOL_SFCP:      return "SFCP";
        case SW3526_PROTOCOL_AFC:       return "AFC";
        default:                        return DASH_OFFLINE_TEXT;
    }
}

/* SW3538 drives two ports (A + C) from one buck with a single protocol register,
 * so this column shows the chip summary: shared VOUT, the sum of both port
 * currents and the resulting power. */
static void dash_fill_sw3538(dash_column_t *col)
{
    SW3538_Handle *handle = APP_GetSW3538();
    uint16_t vout_mv = SW3538_ReadVOUTmV(handle);
    uint32_t iout_ma_x10 = SW3538_ReadPort1IOUTmA_x10(handle) + SW3538_ReadPort2IOUTmA_x10(handle);
    uint32_t mw = (((uint32_t)vout_mv * (iout_ma_x10 / 10u)) / 1000u);   /* mV * mA / 1000 = mW */

    col->name = DASH_NAME_SW3538;
    col->protocol = DASH_OFFLINE_TEXT;

    if(SW3538_IsOnline(handle))
    {
        dash_fmt_volt(col->volt, vout_mv);
        dash_fmt_amp(col->amp, iout_ma_x10);
        dash_fmt_watt(col->watt, mw);

        if(SW3538_IsPort1DeviceOnline(handle) || SW3538_IsPort2DeviceOnline(handle))
            col->protocol = dash_protocol_sw3538((uint8_t)SW3538_GetProtocol(handle));
    }
    else
    {
        dash_column_offline(col);
    }
}

static void dash_fill_sw3526(dash_column_t *col, SW3526_Handle *handle, const char *name)
{
    const struct SW3526_StatusTypedef *status = SW3526_GetStatus(handle);
    uint16_t vout_mv = (status != 0) ? status->vout_mv : 0u;
    uint32_t iout_ma_x10 = (status != 0) ? status->iout_ma_x10 : 0u;
    uint32_t mw = (((uint32_t)vout_mv * (iout_ma_x10 / 10u)) / 1000u);   /* mV * mA / 1000 = mW */

    col->name = name;
    col->protocol = DASH_OFFLINE_TEXT;

    if((status != 0) && SW3526_IsOnline(handle))
    {
        dash_fmt_volt(col->volt, vout_mv);
        dash_fmt_amp(col->amp, iout_ma_x10);
        dash_fmt_watt(col->watt, mw);

        if(SW3526_IsProtocolOnline(handle))
            col->protocol = dash_protocol_sw3526((uint8_t)status->protocol);
    }
    else
    {
        dash_column_offline(col);
    }
}

/* Placeholder hook for the board temperature: the GX21M15U sensor has no driver
 * yet, so the dashboard renders "--C".  This is the single place where the real
 * reading will be plugged in. */
static uint8_t dash_read_temperature(int16_t *tenths_c)
{
    (void)tenths_c;
    return 0u;
}

static const char *dash_pd_state_text(void)
{
    if(PD_IsConnected())
    {
        if(!PD_IsPowerReady())
            return "NEG";

        return PD_IsEPRContractActive() ? "EPR" : "SPR";
    }

    /* VBUS without a PD contract means the DC input path supplies the board. */
    return (VBUS_Sense_ReadMillivolts() > 5000u) ? "DC" : "OFF";
}

static uint32_t dash_input_limit_watts(void)
{
    uint32_t mv;
    uint32_t ma;

    if(!PD_IsConnected())
        return 0u;

    mv = PD_GetContractVoltageMv();
    ma = PD_GetContractCurrentMa();
    if((mv != 0u) && (ma != 0u))
        return (mv * ma) / 1000000u;   /* 32-bit 足够：50 V * 5 A = 250e6 */

    return PD_IsPowerReady() ? (uint32_t)PD_EPR_SINK_PDP_W : 0u;
}

static void dash_signature_append(char *signature, size_t size, const char *text)
{
    size_t used = strlen(signature);
    size_t len = strlen(text);

    if((used + len + 2u) >= size)
        return;

    signature[used] = '|';
    signature[used + 1u] = '\0';
    strcat(signature, text);
}

void Dashboard_Page(ui_t *ui)
{
    static char s_signature[DASH_SIGNATURE_LEN];
    static uint8_t s_frame_valid = 0u;
    static uint32_t s_sent_mark = 0u;   /* flush count after our last frame */

    dash_column_t columns[DASH_COLUMN_COUNT];
    char signature[DASH_SIGNATURE_LEN];
    char row1[DASH_TEXT_LEN];
    char temperature[8];
    char input_volts[8];
    char input_limit[8];
    uint32_t tenths;
    uint32_t limit_w;
    int16_t tenths_c;
    uint8_t color;
    uint8_t i;

    /* Read-only page: navigation actions are swallowed here, so only ENTER/BACK
     * leave for the icon menu. */
    if((ui->action == UI_ACTION_UP) || (ui->action == UI_ACTION_DOWN))
        ui->action = UI_ACTION_NONE;

    dash_fill_sw3538(&columns[0]);
    dash_fill_sw3526(&columns[1], APP_GetSW3526_1(), DASH_NAME_SW3526_1);
    dash_fill_sw3526(&columns[2], APP_GetSW3526_2(), DASH_NAME_SW3526_2);

    if(dash_read_temperature(&tenths_c))
        snprintf(temperature, sizeof temperature, "%dC", (int)(tenths_c / 10));
    else
        snprintf(temperature, sizeof temperature, "--C");

    tenths = ((uint32_t)VBUS_Sense_ReadMillivolts() + 50u) / 100u;
    snprintf(input_volts, sizeof input_volts, "%lu.%luV",
             (unsigned long)(tenths / 10u), (unsigned long)(tenths % 10u));

    limit_w = dash_input_limit_watts();
    if(limit_w != 0u)
        snprintf(input_limit, sizeof input_limit, "%luW", (unsigned long)limit_w);
    else
        snprintf(input_limit, sizeof input_limit, "%s", "--W");

    snprintf(row1, sizeof row1, "%s %s %s %s",
             temperature, input_volts, input_limit, dash_pd_state_text());

    /* Content signature: identical strings mean identical pixels, so the frame
     * is pushed to the panel only when something really changed. */
    memset(signature, 0, sizeof signature);
    dash_signature_append(signature, sizeof signature, row1);
    for(i = 0u; i < DASH_COLUMN_COUNT; i++)
    {
        dash_signature_append(signature, sizeof signature, columns[i].name);
        dash_signature_append(signature, sizeof signature, columns[i].volt);
        dash_signature_append(signature, sizeof signature, columns[i].amp);
        dash_signature_append(signature, sizeof signature, columns[i].watt);
        dash_signature_append(signature, sizeof signature, columns[i].protocol);
    }

    color = ui->bgColor;
    Disp_SetFont(UI_FONT);
    /* The menu leaves a restricted clip window behind (title/data areas), so the
     * full-screen page restores the maximum window before drawing. */
    Disp_SetMaxClipWindow();
    Disp_SetDrawColor(&color);
    Disp_DrawBox(0u, 0u, UI_HOR_RES, UI_VER_RES);

    color = (uint8_t)(ui->bgColor ^ 1u);
    Disp_SetDrawColor(&color);
    Disp_DrawStr(DASH_X_MARGIN, DASH_ROW1_Y, row1);
    dash_draw_column(&columns[0], DASH_X_MARGIN, DASH_COL1_RIGHT);
    dash_draw_column(&columns[1], DASH_COL2_X, DASH_COL2_RIGHT);
    dash_draw_column(&columns[2], DASH_COL3_X, DASH_COL3_RIGHT);

    /* Another page owns the same u8g2 buffer and every fade step flushes its own
     * frame, so the transport count is the reliable "is the panel still mine?"
     * check: it covers menu re-entry (same menu state, same content) as well as
     * real content changes. */
    if((s_frame_valid == 0u) || (Disp_GetFlushCount() != s_sent_mark) ||
       (memcmp(s_signature, signature, sizeof signature) != 0))
    {
        memcpy(s_signature, signature, sizeof signature);
        s_frame_valid = 1u;
        Disp_SendBuffer();
        s_sent_mark = Disp_GetFlushCount();   /* our own request bumped the count */
    }
}
