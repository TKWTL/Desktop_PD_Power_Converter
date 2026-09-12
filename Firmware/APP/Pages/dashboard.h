/* Dashboard (startup telemetry) page for the Desktop PD Power Converter.
 * Lives outside APP/MiaoUI on purpose: ui_conf.c only owns the menu tree. */
#ifndef APP_PAGES_DASHBOARD_H_
#define APP_PAGES_DASHBOARD_H_

#include "ui_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Startup function page: row 1 carries the board input state (temperature,
 * input voltage, power limit, PD input state), rows 2..6 hold one column per
 * converter block -- SW3538 "A+C", SW3526 #1 "C1", SW3526 #2 "C2" -- with the
 * port name, output voltage, current, power and negotiated protocol. */
void Dashboard_Page(ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* APP_PAGES_DASHBOARD_H_ */
