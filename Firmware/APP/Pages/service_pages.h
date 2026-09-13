/* Service items/pages for the Desktop PD Power Converter: fan PWM test,
 * software reset, ISP handoff and the OLED burn-in test.
 * Lives outside APP/MiaoUI on purpose: ui_conf.c only owns the menu tree. */
#ifndef APP_PAGES_SERVICE_PAGES_H_
#define APP_PAGES_SERVICE_PAGES_H_

#include "ui_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings-page entries, added directly (no "Tools" submenu): the fan speed
 * test as a live PWM data item, then the two reset actions. */
void Add_Service_Items(ui_page_t *parent_page);

/* Full-screen OLED burn-in test: short explanation, then a white screen; any
 * key during the white phase returns to the menu. */
void Burnin_Page(ui_t *ui);

/* Reset actions used as function items on the settings page.  Neither ever
 * returns: Soft Reset restarts the application, Reboot to ISP hands over to
 * the CH32 factory bootloader on the next reset. */
void SoftReset_Action(ui_t *ui);
void IspReboot_Action(ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* APP_PAGES_SERVICE_PAGES_H_ */
