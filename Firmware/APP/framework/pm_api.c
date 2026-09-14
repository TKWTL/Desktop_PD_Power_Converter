/* Application layer of the power framework: owns the controller instance, the
 * block mask and the wake bookkeeping.  thread_pm (APP/app_tasks.c) calls
 * pm_api_poll() every 50 ms; thread_ui reads pm_api_ui_should_block() and
 * forwards key activity through pm_api_refresh_idle().
 *
 * Ported from the two reference projects, keeping the Pocket_PowerBank fixes:
 *  - pm_api_force_sleep() drops any pending refresh request before latching
 *    the forced expiry, otherwise an already-queued key/plug event would pull
 *    the state machine straight back to RUN;
 *  - the "No Auto Sleep" timeout uses pm_sleep_timer_disable() (countdown
 *    stopped) instead of the old magic maximum timeout value.
 *
 * Force-flag lifetime (differs from Pocket by design): the reference clears
 * the latch inside its DEEPSLEEP/STANDBY branches, which are unreachable here
 * because PM_MAX_SLEEP_DEPTH is PM_SLEEP_DEPTH_UI_OFF.  The latch must stay
 * set for as long as UI_OFF lasts - otherwise "manual sleep + No Auto Sleep"
 * would bounce back to RUN on the next poll (idle_timeout would read 0) - so
 * it is cleared exactly on the UI_OFF -> RUN wake transition. */
#include "pm_api.h"

#include "pm_controller.h"
#include "pm_device.h"
#include "pm_sleep_timer.h"

/* Power-on default: UI_OFF after one idle minute (the Sleep menu can change
 * it at runtime; the setting is RAM-only, like in both reference projects). */
#define PM_DEFAULT_TIMEOUT_MS  60000U

/* Polls with input swallowed after a wake: the key that caused the wake must
 * not also act on the menu (~100 ms at the 50 ms poll period). */
#define PM_WAKE_SILENCE_POLLS  2u

static pm_controller_t s_pm_ctrl;
static uint8_t s_pm_ready;
static volatile uint8_t s_pm_refresh_req;
static uint8_t s_pm_block_mask;
static uint8_t s_pm_sleep_blocked;
static uint8_t s_wk_silence;
static uint8_t s_unstable_wake;

void pm_api_init(void)
{
    if (s_pm_ready != 0u) {
        return;
    }

    pm_controller_init(&s_pm_ctrl, PM_DEFAULT_TIMEOUT_MS);
    pm_controller_mark_ui_active(&s_pm_ctrl, 1u);
    pm_device_register_all();

    s_pm_ready = 1u;
}

void pm_api_refresh_idle(void)
{
    s_pm_refresh_req = 1u;
}

void pm_api_force_sleep(void)
{
    pm_api_init();

    /* Drop a refresh queued in the same pass (key/menu edge) - otherwise the
     * state machine would return to RUN immediately. */
    s_pm_refresh_req = 0u;
    pm_sleep_timer_force_expire();
    pm_controller_mark_ui_active(&s_pm_ctrl, 0u);
}

void pm_api_set_sleep_timeout(int timeout_sec)
{
    pm_api_init();

    if (timeout_sec <= 0) {
        pm_sleep_timer_disable();                     /* No Auto Sleep */
    } else {
        pm_sleep_timer_set((uint32_t)timeout_sec * 1000u);
    }
}

void pm_api_set_sleep_block(uint8_t mask, uint8_t enable)
{
    if (enable != 0u) {
        s_pm_block_mask |= mask;
    } else {
        s_pm_block_mask &= (uint8_t)~mask;
    }
}

uint8_t pm_api_get_block_mask(void)
{
    return s_pm_block_mask;
}

uint8_t pm_api_ui_should_block(void)
{
    return pm_controller_is_ui_blocked(&s_pm_ctrl);
}

uint8_t pm_api_is_sleeping(void)
{
    return (s_pm_ctrl.state != PM_STATE_RUN) ? 1u : 0u;
}

void pm_api_set_unstable_wake(uint8_t unstable)
{
    s_unstable_wake = (unstable != 0u) ? 1u : 0u;
}

uint8_t pm_api_is_unstable_wake(void)
{
    return s_unstable_wake;
}

void pm_api_poll(void)
{
    pm_power_state_t state;
    pm_power_state_t prev;

    pm_api_init();

    if (s_pm_refresh_req != 0u) {
        pm_controller_refresh_idle(&s_pm_ctrl);
        s_pm_refresh_req = 0u;
    }

    if (s_pm_block_mask != 0u) {
        if (s_pm_sleep_blocked == 0u) {
            pm_controller_pause_idle(&s_pm_ctrl);
            s_pm_sleep_blocked = 1u;
        }
        pm_controller_notify_wake(&s_pm_ctrl);
    } else if (s_pm_sleep_blocked != 0u) {
        pm_controller_resume_idle(&s_pm_ctrl);
        s_pm_sleep_blocked = 0u;
    }

    /* Idle timeout: clear the UI-active flag so the state machine may leave
     * RUN (expired() already short-circuits to 0 while No Auto Sleep is set,
     * and to 1 after pm_api_force_sleep()). */
    if ((s_pm_refresh_req == 0u) && (pm_sleep_timer_expired() != 0u)) {
        pm_controller_mark_ui_active(&s_pm_ctrl, 0u);
    }

    prev = s_pm_ctrl.state;
    state = pm_controller_step(&s_pm_ctrl);

    if ((prev != PM_STATE_RUN) && (state == PM_STATE_RUN)) {
        /* Woke up: end the forced latch, re-arm the idle timer and swallow the
         * waking key for ~100 ms. */
        pm_sleep_timer_clear_force();
        pm_controller_mark_ui_active(&s_pm_ctrl, 1u);
        s_wk_silence = PM_WAKE_SILENCE_POLLS;
        s_unstable_wake = 1u;
    }

    if ((s_wk_silence > 0u) && (--s_wk_silence == 0u)) {
        s_unstable_wake = 0u;
    }
}
