#ifndef APP_TASKS_H_
#define APP_TASKS_H_

#include "sw3526.h"
#include "sw3538.h"

void APP_Tasks_Init(void);
void APP_Tasks_RunOnce(void);
/* Scheduler idle point: call right after APP_Tasks_RunOnce().  It parks the
 * core in WFI unless a software-I2C transfer must keep being serviced. */
void APP_Tasks_Idle(void);
/* Freeze diagnostics: called from the 1 ms SysTick handler; prints the
 * checkpoint over a polled UART path when the main loop stops advancing. */
void APP_DbgStallCheck(void);

/* Shared read-only/control access points for future UI/power-policy modules. */
SW3538_Handle *APP_GetSW3538(void);
SW3526_Handle *APP_GetSW3526_1(void);
SW3526_Handle *APP_GetSW3526_2(void);

#endif /* APP_TASKS_H_ */
