#ifndef APP_TASKS_H_
#define APP_TASKS_H_

#include "sw3526.h"
#include "sw3538.h"
#include "GX21M15/gx21m15.h"

void APP_Tasks_Init(void);
void APP_Tasks_RunOnce(void);
void APP_Tasks_Idle(void);

SW3538_Handle *APP_GetSW3538(void);
SW3526_Handle *APP_GetSW3526_1(void);
SW3526_Handle *APP_GetSW3526_2(void);
GX21M15_Handle *APP_GetGX21M15(void);

#endif /* APP_TASKS_H_ */
