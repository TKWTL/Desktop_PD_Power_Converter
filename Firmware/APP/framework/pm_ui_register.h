#ifndef PM_UI_REGISTER_H
#define PM_UI_REGISTER_H

#include "core/ui.h"

/* MiaoUI registration of the power framework (ported from the NUEDC_2025B
 * pm_ui_register.c and adapted to this project's menu builder):
 *
 *   PM_UI_AddSleepNowItem()        main icon page: "-Sleep" acts immediately
 *                                  (one-shot -> the screen turns off);
 *   PM_UI_AddSleepSettingsItems()  settings page: "-Sleep" entry opening the
 *                                  "[Sleep]" page with the No Auto Sleep ...
 *                                  30min radio group (place it right after
 *                                  the menu item). */
void PM_UI_AddSleepNowItem(ui_page_t *main_page);
void PM_UI_AddSleepSettingsItems(ui_page_t *settings_page);

#endif /* PM_UI_REGISTER_H */
