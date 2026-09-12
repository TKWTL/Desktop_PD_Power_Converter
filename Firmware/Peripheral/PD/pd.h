#ifndef PD_H_
#define PD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board power policy. */
#define PD_REQUEST_MAX_FIXED_MV         20000U
/* SPR current ceiling: request the Source's full advertised current.
 *
 * Product policy is maximum power in every scenario.  The board is field-
 * tested with a 5 A e-marked cable (the verified reference flow on this
 * charger is itself SPR 20 V / 5 A -> EPR -> 28 V / 5 A), and the old "5 A
 * request collapsed the rail" scare was the PHY deadline bug, not the cable.
 * This constant only clamps runaway PDO values; the actual request is always
 * min(PDO current, this). */
#define PD_SPR_REQUEST_MAX_MA           5000U
#define PD_EPR_ENABLE                   1U
#define PD_EPR_TARGET_MV                28000U
#define PD_EPR_REQUEST_MAX_MA           5000U
#define PD_EPR_SINK_PDP_W               140U

/* USB identity VDO values, kept for a future identity experiment.
 *
 * The current build deliberately does NOT answer VDM identity requests with
 * these values: it mirrors the field-verified DemoBoard behaviour (short NAK)
 * because the crafted ACK was followed by this charger withholding the EPR
 * grant, while the verified build never supplies a real identity at all.
 * Values remain here (pid.codes open-project test assignment - replace before
 * shipping) for the case that a real identity exchange turns out to be
 * required after all. */
#define PD_IDENTITY_VID                0x1209U
#define PD_IDENTITY_PID                0x0001U
#define PD_IDENTITY_BCD_DEVICE         0x0001U

/* Public PD service API.  Protocol state, timers, PHY access and EPR chunking
 * are internal to Peripheral/PD. */
void PD_Init(void);
void PD_Task(uint32_t now_ms);
/* Feed board VBUS measurement into the policy engine.  This board supplies it from the PA7 resistor-divider ADC; PD owns
 * detach debounce/contract teardown. */
void PD_SetVbusMillivolts(uint16_t mv);
uint8_t PD_IsConnected(void);
uint8_t PD_IsEPRContractActive(void);
uint8_t PD_IsPowerReady(void);
uint16_t PD_GetContractVoltageMv(void);
uint16_t PD_GetContractCurrentMa(void);
/* Fast-poll request for the scheduler idle hook: true while PD needs main-loop
 * passes at full speed (source attached, GoodCRC in flight).  Sleeping here
 * stretches the Sink sender-response latency outside the PD timing windows. */
uint8_t PD_WantsFastPoll(void);

typedef enum
{
    PD_DISPLAY_PDO_SPR = 1,
    PD_DISPLAY_PDO_PPS = 2,
    PD_DISPLAY_PDO_EPR = 3,
    PD_DISPLAY_PDO_AVS = 4
} PD_DisplayPDOType;

typedef struct
{
    uint8_t index;
    uint8_t type;
    uint16_t min_mv;
    uint16_t max_mv;
    uint16_t current_ma;
    uint16_t power_w;
} PD_DisplayPDO;

/* Snapshot up to max_count display-worthy PDOs, sorted for the UI as
 * AVS > EPR Fixed > PPS > SPR Fixed, then by maximum voltage descending.
 * Voltage/current/power are decoded directly from the received Source PDO and
 * are never clamped to the Sink request policy. Detached always returns 0.
 * Battery/Variable PDOs and unsupported APDO subtypes are intentionally omitted. */
uint8_t PD_GetDisplayPDOs(PD_DisplayPDO *out, uint8_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* PD_H_ */
