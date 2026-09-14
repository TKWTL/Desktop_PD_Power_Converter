/* Idle timer for the power framework (see pm_sleep_timer.h).  Single-threaded
 * cooperative use only: everything runs in thread context, so no critical
 * sections are needed. */
#include "pm_sleep_timer.h"

#include "time_api.h"

static uint32_t s_timeout_ms;
static uint32_t s_deadline;
static uint8_t s_countdown_disabled;   /* 1 = No Auto Sleep: expired() stays 0 */
static uint8_t s_force_sleep;          /* 1 = manual sleep: expired() stays 1 */

void pm_sleep_timer_init(uint32_t timeout_ms)
{
    s_timeout_ms = timeout_ms;
    s_countdown_disabled = 0u;
    s_force_sleep = 0u;

    s_deadline = TIME_Millis() + timeout_ms;
}

void pm_sleep_timer_set(uint32_t timeout_ms)
{
    s_timeout_ms = timeout_ms;
    s_countdown_disabled = 0u;   /* an explicit timeout re-enables the countdown */

    s_deadline = TIME_Millis() + timeout_ms;
}

void pm_sleep_timer_disable(void)
{
    /* Stopping the countdown only blocks the expiry: refresh() keeps reloading
     * the deadline and expired()/left_ms() short-circuit on the flag, so no
     * magic timeout value is involved. */
    s_countdown_disabled = 1u;
}

void pm_sleep_timer_refresh(void)
{
    /* Unconditional reload - the disabled flag is honoured by the readers. */
    s_deadline = TIME_Millis() + s_timeout_ms;
}

void pm_sleep_timer_force_expire(void)
{
    s_force_sleep = 1u;          /* also works while the countdown is stopped */
}

void pm_sleep_timer_clear_force(void)
{
    s_force_sleep = 0u;          /* cleared once the forced sleep actually ran */
}

uint32_t pm_sleep_timer_left_ms(void)
{
    uint32_t left;

    if (s_force_sleep != 0u) {
        return 0u;
    }
    if (s_countdown_disabled != 0u) {
        return PM_SLEEP_INFINITE;
    }

    left = s_deadline - TIME_Millis();
    if ((int32_t)left <= 0) {
        return 0u;
    }
    return left;
}

uint8_t pm_sleep_timer_expired(void)
{
    if (s_force_sleep != 0u) {
        return 1u;
    }
    if (s_countdown_disabled != 0u) {
        return 0u;
    }

    return ((int32_t)(TIME_Millis() - s_deadline) >= 0) ? 1u : 0u;
}

uint8_t pm_sleep_timer_is_disabled(void)
{
    return (s_countdown_disabled != 0u) ? 1u : 0u;
}
