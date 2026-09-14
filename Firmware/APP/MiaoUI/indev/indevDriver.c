/* Two-key input adapter for MiaoUI: K1=DOWN, K2=ENTER/BACK.
 *
 * K1 keeps the early-MiaoUI scheme: DAS auto-repeat, DOWN everywhere.
 *
 * K2 is *release* triggered, with long press = BACK:
 *   - press and let go before the long-press threshold -> UI_ACTION_ENTER;
 *   - hold past the threshold -> one UI_ACTION_BACK (fired while the key is
 *     still held) and the release of that press is swallowed, so a long press
 *     can never confirm something as well;
 *   - only a press this scanner has seen (KeyEdge_Rising) can produce an
 *     action: the K2 press that wakes a sleeping panel is handled by the power
 *     manager (ui_loop()/indevScan() do not run while the UI is off), so its
 *     release must not fire ENTER on the page that just lit up; presses that
 *     start while the UI is blocked (PD quiet window) are dropped too. */
#include "indev/indevDriver.h"
#include "buttons.h"
#include "core/ui.h"

static uint8_t s_enter_down;   /* K2 press seen by the scanner */
static uint8_t s_enter_back;   /* ... already consumed as a long press (BACK) */

UI_ACTION indevScan(void)
{
    KeyEdge_t edge = Key_EdgeDetect(KeyIndex_Enter);

    if(edge == KeyEdge_Rising)
    {
        s_enter_down = 1u;
        s_enter_back = 0u;
    }
    else if((edge == KeyEdge_Holding) && (s_enter_down != 0u) && (s_enter_back == 0u))
    {
        /* Long press recognised while K2 is still held: back one level.  The
         * latch also keeps the one-shot edge from firing twice (the edge
         * survives ~10 ms, the UI frame is 8 ms). */
        s_enter_back = 1u;
        return UI_ACTION_BACK;
    }
    else if(edge == KeyEdge_Falling)
    {
        uint8_t was_short = (uint8_t)((s_enter_down != 0u) && (s_enter_back == 0u));

        s_enter_down = 0u;
        s_enter_back = 0u;

        if(was_short != 0u)
            return UI_ACTION_ENTER;
    }

    if(KEY_GetDASClick(KeyIndex_Down))
    {
        /* Menus scroll with K1 and the numeric dialog in parameter.c treats
         * DOWN as "+step" (its scrollbar wraps at both ends, so the one key
         * covers the full range). */
        return UI_ACTION_DOWN;
    }

    return UI_ACTION_NONE;
}
