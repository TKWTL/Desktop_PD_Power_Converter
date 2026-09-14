#ifndef PM_SLEEP_TIMER_H
#define PM_SLEEP_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* left_ms() value while the countdown is stopped ("no expiry"). */
#define PM_SLEEP_INFINITE   0xFFFFFFFFUL

/* Idle timer - bare-metal port of the Pocket_PowerBank version (the one that
 * replaced the NUEDC original's "huge timeout value means disabled" trick):
 *   set()          restart the countdown with a new timeout;
 *   disable()      stop the countdown (No Auto Sleep), expired() stays 0;
 *   refresh()      reload the deadline (activity);
 *   force_expire() manual sleep: expired() stays 1 even while stopped;
 *   clear_force()  cleared once the state machine reached the sleep level.
 * TIME_Millis() is a free-running 32-bit millisecond counter; all compares are
 * wrap-safe ((int32_t)(now - deadline) >= 0). */
void pm_sleep_timer_init(uint32_t timeout_ms);
void pm_sleep_timer_set(uint32_t timeout_ms);
void pm_sleep_timer_disable(void);
void pm_sleep_timer_refresh(void);
void pm_sleep_timer_force_expire(void);
void pm_sleep_timer_clear_force(void);
uint32_t pm_sleep_timer_left_ms(void);
uint8_t pm_sleep_timer_expired(void);
uint8_t pm_sleep_timer_is_disabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PM_SLEEP_TIMER_H */
