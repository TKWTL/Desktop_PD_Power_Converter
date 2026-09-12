/* Two-key input adapter for MiaoUI: K1=DOWN, K2=ENTER. */
#include "indev/indevDriver.h"
#include "buttons.h"
#include "core/ui.h"

extern ui_t ui;

UI_ACTION indevScan(void)
{
    /* ENTER is edge-triggered: holding K2 must not repeatedly enter/leave a
     * page or confirm a dialog. DOWN keeps DAS repeat for menu scrolling. */
    if(Key_EdgeDetect(KeyIndex_Enter) == KeyEdge_Rising)
        return UI_ACTION_ENTER;

    if(KEY_GetDASClick(KeyIndex_Down))
    {
        /* With only two physical keys, K1 remains DOWN for page navigation.
         * Inside an editable numeric dialog the same key advances the value;
         * K2 confirms/exits. Background/rotation switches toggle on ENTER. */
        if((ui.menuState == UI_ITEM_RUNING || ui.menuState == UI_ITEM_DRAWING) &&
           ui.nowItem != 0 && ui.nowItem->itemType == UI_ITEM_DATA &&
           ui.nowItem->element != 0 && ui.nowItem->element->data != 0 &&
           (ui.nowItem->element->data->dataType == UI_DATA_INT ||
            ui.nowItem->element->data->dataType == UI_DATA_FLOAT))
            return UI_ACTION_PLUS;

        return UI_ACTION_DOWN;
    }

    return UI_ACTION_NONE;
}
