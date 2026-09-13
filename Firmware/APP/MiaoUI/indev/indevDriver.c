/* Two-key input adapter for MiaoUI: K1=DOWN, K2=ENTER. */
#include "indev/indevDriver.h"
#include "buttons.h"
#include "core/ui.h"

UI_ACTION indevScan(void)
{
    /* ENTER is edge-triggered: holding K2 must not repeatedly enter/leave a
     * page or confirm a dialog. */
    if(Key_EdgeDetect(KeyIndex_Enter) == KeyEdge_Rising)
        return UI_ACTION_ENTER;

    if(KEY_GetDASClick(KeyIndex_Down))
    {
        /* Early-MiaoUI scheme: K1 is DOWN everywhere.  Menus scroll with it,
         * and the numeric dialog in parameter.c treats DOWN as "+step" (its
         * scrollbar wraps at both ends, so the one key covers the full range).
         * No role-specific action values are generated here any more. */
        return UI_ACTION_DOWN;
    }

    return UI_ACTION_NONE;
}
