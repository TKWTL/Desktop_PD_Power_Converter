#ifndef PM_API_H
#define PM_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Application-facing API of the power framework (UI_OFF level port).
 *
 * Activity sources that keep the idle timer reset (all call
 * pm_api_refresh_idle()):
 *   - any key press (thread_ui);
 *   - PD/VBUS or output-port plug/unplug edges (thread_pm);
 *   - total output power above 5 W (thread_pm).
 *
 * The sleep block mask is app-defined: define your own PM_BLOCK_* bits and
 * call pm_api_set_sleep_block(); no bit is predefined for this product yet. */
void pm_api_init(void);
void pm_api_poll(void);                          /* thread_pm: advance the state machine */
void pm_api_refresh_idle(void);                  /* activity: restart the idle countdown */
void pm_api_force_sleep(void);                   /* manual sleep: enter UI_OFF now */
void pm_api_set_sleep_timeout(int timeout_sec);  /* 0 = No Auto Sleep, >0 = seconds */
void pm_api_set_sleep_block(uint8_t mask, uint8_t enable);
uint8_t pm_api_get_block_mask(void);
uint8_t pm_api_ui_should_block(void);            /* 1 = thread_ui must stop drawing */
uint8_t pm_api_is_sleeping(void);
void pm_api_set_unstable_wake(uint8_t unstable);
uint8_t pm_api_is_unstable_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* PM_API_H */
