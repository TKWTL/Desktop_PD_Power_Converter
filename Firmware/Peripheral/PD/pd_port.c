/********************************** (C) COPYRIGHT *******************************
 * USB-PD hardware port for CH32X035.
 *
 * This is the only PD file that should directly touch CH32X035 USBPD/RCC/GPIO/
 * AFIO/NVIC registers.  The upper PD protocol/policy layer talks only through
 * pd_port.h so a future MCU port replaces this file instead of the PD protocol state machine.
 ******************************************************************************/

#include "debug.h"
#include "main.h"
#include "pd_port.h"
#include "time_api.h"
#include <string.h>

#define PD_PORT_GOODCRC_TYPE       0x01u
#define PD_PORT_MIN_RX_FRAME_BYTES 6u
#define PD_PORT_MAX_FRAME_BYTES    34u

/* IF_TX_END must arrive within a few hundred microseconds; 2 ms is a generous
 * watchdog bound.  The GoodCRC window keeps the historical ~750 us polling
 * budget of the C140 sequence plus margin for the cooperative main loop. */
#define PD_PORT_TX_END_TIMEOUT_US  2000u
#define PD_PORT_GOODCRC_TIMEOUT_US 1000u
#define PD_PORT_TX_MAX_ATTEMPTS    3u
/* Automatic GoodCRC responses get one retry: dropping the ACK makes the Source
 * retire its message and, with its retries spent, Hard Reset the port. */
#define PD_PORT_ACK_MAX_ATTEMPTS   2u

/* CONFIG and PORT_CC1/2 are 16-bit registers on CH32X035.
 * Do not narrow complemented masks to uint8_t: that would clear CONFIG
 * bits 8..15, including IE_RX_ACT/IE_RX_RESET/IE_TX_END. */

/* PHY state of the asynchronous transmit engine. */
enum
{
    PD_PHY_IDLE = 0,
    PD_PHY_TX,            /* frame on the wire; waiting for IF_TX_END      */
    PD_PHY_WAIT_GOODCRC,  /* frame finished; waiting for the Source's ACK  */
    PD_PHY_ACK_TX         /* automatic GoodCRC response on the wire        */
};

static uint8_t *s_rx_mailbox;                        /* policy layer's buffer */
static uint8_t s_rx_dma_buf[PD_PORT_MAX_FRAME_BYTES] __attribute__((aligned(4)));
static uint8_t s_tx_dma_buf[PD_PORT_MAX_FRAME_BYTES] __attribute__((aligned(4)));
/* The GoodCRC transmit buffer is full frame size even though only two bytes
 * are sent: while that frame is on the wire (and until the policy re-arms RX)
 * the PHY's receive DMA still points here, and a frame arriving in that window
 * would otherwise run straight past a 2-byte array into the engine's state
 * variables. */
static uint8_t s_ack_buf[PD_PORT_MAX_FRAME_BYTES] __attribute__((aligned(4)));

static volatile uint8_t  s_phy_state;
static volatile uint8_t  s_tx_result;
static volatile uint8_t  s_tx_expect_goodcrc;
static volatile uint8_t  s_tx_attempts_left;
static volatile uint8_t  s_tx_len;
static volatile uint8_t  s_tx_msgid;
static volatile uint32_t s_tx_deadline_us;
static volatile uint8_t  s_rx_capture_len;
static volatile uint8_t  s_message_pending;
static volatile uint8_t  s_hard_reset_pending;
static volatile uint8_t  s_auto_ack_pr_role;
static volatile uint16_t s_auto_ack_started;
static volatile uint16_t s_auto_ack_completed;
static volatile uint32_t s_last_auto_ack_completed_us;
static volatile uint8_t  s_last_detect_cc1;
static volatile uint8_t  s_last_detect_cc2;
static volatile uint16_t s_tx_end_timeouts;
static volatile uint16_t s_goodcrc_timeouts;
static volatile uint16_t s_ack_tx_timeouts;
static volatile uint8_t  s_ack_attempts_left;
static volatile uint8_t  s_ack_fail_dumped;
static volatile uint8_t  s_rx_armed;
static volatile uint32_t s_last_phy_op_us;
static volatile uint8_t  s_hr_pd_stat;
static volatile uint8_t  s_hr_byte_count;

/* Diagnostics for frames that never raise IF_TX_END: what the PHY looked like
 * when the frame was armed, and what it looked like when the wait gave up. */
static volatile uint8_t  s_tx_start_dumps;
static volatile uint8_t  s_tx_start_status;
static volatile uint8_t  s_tx_start_control;
static volatile uint8_t  s_tx_start_sop;
static volatile uint8_t  s_tx_start_len;
static volatile uint8_t  s_tx_start_type;
static volatile uint16_t s_tx_start_config;
static volatile uint16_t s_tx_start_cc1;
static volatile uint16_t s_tx_start_cc2;
static volatile uint8_t  s_tx_fail_dumped;
static volatile uint8_t  s_tx_fail_control;
static volatile uint8_t  s_tx_fail_status;
static volatile uint8_t  s_tx_fail_byte_count;
static volatile uint8_t  s_tx_fail_clk_cnt;
static volatile uint8_t  s_tx_fail_dma_ok;
static volatile uint16_t s_tx_fail_cc1;
static volatile uint16_t s_tx_fail_cc2;
static volatile uint16_t s_tx_fail_config;

/* Keep the timing-critical USBPD vector in this translation unit.  The
 * WCH reference implementation does the same; avoid an extra APP-layer
 * wrapper between the fast vector entry and the GoodCRC response path. */
void USBPD_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* Start one frame on the active CC line.  This is the exact WCH transmit
 * sequence; with the asynchronous engine it is fire-and-forget - completion is
 * signalled by IF_TX_END from the USBPD vector. */
static void pd_port_begin_tx_lowlevel(const uint8_t *buffer,
                                      uint8_t length,
                                      uint8_t wch_tx_sel)
{
    if((USBPD->CONFIG & CC_SEL) == CC_SEL)
        USBPD->PORT_CC2 |= CC_LVE;
    else
        USBPD->PORT_CC1 |= CC_LVE;

    USBPD->BMC_CLK_CNT = UPD_TMR_TX_48M;
    USBPD->DMA = (uint32_t)(uintptr_t)buffer;
    USBPD->TX_SEL = wch_tx_sel;
    USBPD->BMC_TX_SZ = length;
    USBPD->CONTROL |= PD_TX_EN;
    USBPD->STATUS &= BMC_AUX_INVALID;
    USBPD->CONTROL |= BMC_START;

    /* The PHY left its listening state and needs to settle before the next
     * operation is accepted. */
    s_rx_armed = 0u;
    s_last_phy_op_us = TIME_Micros();
}

/* Release the active CC line, drop PD_TX_EN and point the dedicated DMA back
 * at the RX buffer, so the GoodCRC (or the next packet) can be received the
 * moment this frame ends. */
static void pd_port_enter_rx(void)
{
    USBPD->PORT_CC1 &= (uint16_t)~(uint16_t)CC_LVE;
    USBPD->PORT_CC2 &= (uint16_t)~(uint16_t)CC_LVE;

    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= (uint16_t)~(uint16_t)PD_ALL_CLR;
    USBPD->CONTROL &= (uint8_t)~PD_TX_EN;
    USBPD->DMA = (uint32_t)(uintptr_t)s_rx_dma_buf;
    USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
    USBPD->CONTROL |= BMC_START;

    s_rx_armed = 1u;
    s_last_phy_op_us = TIME_Micros();
}

/* Hand one foreground frame to the PHY from a known-good starting point.
 *
 * Three details matter here, all of them ways the Source's GoodCRC can be lost
 * (a lost GoodCRC turns into a retransmission with the same Message ID, which
 * the Source silently discards - and if the retries run out as well, the policy
 * layer believes the frame failed even though the Source acted on it):
 *  - the frame is placed in the *same* buffer the PHY will receive into.  With
 *    a separate transmit buffer, the GoodCRC (which can arrive before the
 *    IF_TX_END interrupt is even serviced) is DMA'd into the transmit buffer
 *    while the vector compares it against the receive buffer - a guaranteed
 *    mismatch whenever the Source answers faster than the interrupt is taken;
 *  - PD_TX_EN/BMC_START are cleared first so the PHY sees a clean 0 -> 1 edge.
 *    A frame armed while the previous transmit's state is still latched can be
 *    ignored outright: no bits on the wire, no IF_TX_END;
 *  - no extra wait is inserted: the completion deadlines in PD_Port_Service()
 *    are the only pacing needed once they are evaluated correctly.
 * The auto-GoodCRC response inside the vector uses the ACK buffer instead: the
 * received message it answers has to stay intact for the policy layer. */
static void pd_port_start_frame(const uint8_t *buffer, uint8_t length,
                                uint8_t wch_tx_sel, uint8_t verbose)
{
    USBPD->STATUS |= IF_TX_END;
    USBPD->CONFIG |= IE_TX_END;
    NVIC_EnableIRQ(USBPD_IRQn);

    USBPD->CONTROL &= (uint8_t)~(PD_TX_EN | BMC_START);

    if((length != 0u) && (length <= PD_PORT_MAX_FRAME_BYTES) &&
       (buffer != s_rx_dma_buf))
        memcpy(s_rx_dma_buf, buffer, length);

    pd_port_begin_tx_lowlevel(s_rx_dma_buf, length, wch_tx_sel);

    /* Snapshot the PHY the instant the frame is armed.  A latched flag here
     * means an echo or a leftover event was still pending. */
    s_tx_start_status = (uint8_t)USBPD->STATUS;
    s_tx_start_control = USBPD->CONTROL;
    s_tx_start_sop = USBPD->TX_SEL;
    s_tx_start_len = length;
    s_tx_start_type = (length != 0u) ? buffer[0] : 0u;
    s_tx_start_config = USBPD->CONFIG;
    s_tx_start_cc1 = USBPD->PORT_CC1;
    s_tx_start_cc2 = USBPD->PORT_CC2;

    if((verbose != 0u) && (s_tx_start_dumps < 4u))
    {
        s_tx_start_dumps++;
        printf("[PD] TXs#%u typ=%02X len=%u CTL=%02X ST=%02X\r\n",
               (unsigned)s_tx_start_dumps, (unsigned)s_tx_start_type,
               (unsigned)s_tx_start_len, (unsigned)s_tx_start_control,
               (unsigned)s_tx_start_status);
        printf("[PD] TXs#%u CFG=%04X SEL=%u CC1=%04X CC2=%04X\r\n",
               (unsigned)s_tx_start_dumps, (unsigned)USBPD->CONFIG,
               (unsigned)s_tx_start_sop, (unsigned)s_tx_start_cc1,
               (unsigned)s_tx_start_cc2);
    }
}

/* Latch the hardware evidence of a reset event before any PD_ALL_CLR can erase
 * it.  A genuine Hard Reset ordered set carries no data objects (MASK_PD_STAT
 * reports PD_RX_SOP1_HRST with a small byte count); a large byte count means the
 * event was an RX error / line glitch instead, which the policy layer has to
 * treat differently. */
static void pd_port_latch_hard_reset(void)
{
    s_hard_reset_pending = 1u;
    s_hr_pd_stat = (uint8_t)(USBPD->STATUS & MASK_PD_STAT);
    s_hr_byte_count = USBPD->BMC_BYTE_CNT;
}

/* Frame length to hand to the policy layer.
 *
 * BMC_BYTE_CNT is not trustworthy here: when a frame lands while the PHY is
 * still closing out a transmit it reads stale values (3 and 7 were observed for
 * a 6-byte GoodCRC), and using it as the capture length truncated extended
 * messages.  The header carries the real length, so the larger of the two
 * wins: 2 header bytes plus the data objects, or the extended header's data
 * size.  The CRC is checked and consumed by hardware and is not part of the
 * captured bytes. */
static uint8_t pd_port_frame_bytes(void)
{
    uint8_t hdr = s_rx_dma_buf[1];
    uint8_t need;
    uint8_t count = USBPD->BMC_BYTE_CNT;

    if((hdr & 0x80u) != 0u)
    {
        /* Extended: 2-byte header + 4-byte extended header + DataSize. */
        uint16_t data_size = (uint16_t)(((uint16_t)s_rx_dma_buf[3] >> 1) |
                                        ((uint16_t)(s_rx_dma_buf[4] & 0x01u) << 7));
        need = (uint8_t)(2u + 4u + (uint8_t)data_size);
    }
    else
    {
        need = (uint8_t)(2u + (4u * (uint8_t)((hdr >> 4) & 0x07u)));
    }

    if(count < need)
        count = need;
    if(count > PD_PORT_MAX_FRAME_BYTES)
        count = PD_PORT_MAX_FRAME_BYTES;

    return count;
}

/* Wrap-safe "has this deadline passed?" test.
 *
 * The deadline variables hold absolute timestamps (start + timeout).  This file
 * used to compare the unsigned difference against the timeout instead:
 *
 *     if((uint32_t)(now - s_tx_deadline_us) >= PD_PORT_TX_END_TIMEOUT_US)
 *
 * With now still before the deadline that difference wraps to ~2^32, making the
 * comparison true: every frame was declared timed out on the next service pass
 * while the PHY was still transmitting it (the field dump showed CONTROL = 0x43
 * / 0xC3 with PD_TX_EN|BMC_START set at "timeout").  Comparing the two
 * timestamps as signed values is the correct test and survives the 32-bit
 * microsecond counter wrapping. */
static inline uint8_t pd_port_time_reached(uint32_t now, uint32_t deadline)
{
    return ((int32_t)(now - deadline) >= 0) ? 1u : 0u;
}

/* One transmit attempt failed (GoodCRC timeout or missing IF_TX_END).  Retry
 * the very same frame - same Message ID, RX stays armed - until the attempts
 * are used up, then publish the error for the policy layer. */
static void pd_port_tx_attempt_failed(void)
{
    if(s_tx_attempts_left != 0u)
        s_tx_attempts_left--;

    if(s_tx_attempts_left != 0u)
    {
        s_phy_state = PD_PHY_TX;
        s_tx_deadline_us = TIME_Micros() + PD_PORT_TX_END_TIMEOUT_US;
        pd_port_start_frame(s_tx_dma_buf, s_tx_len, UPD_SOP0, 1u);
    }
    else
    {
        s_tx_result = PD_PORT_TX_RESULT_ERR;
        s_phy_state = PD_PHY_IDLE;
    }
}

void USBPD_IRQHandler(void)
{
    uint8_t status;

    DBG_ISR_BUMP(DBG_ISR_USBPD);
    status = (uint8_t)USBPD->STATUS;

    /* A received Hard Reset (or cable reset) terminates whatever transaction
     * is in flight.  Latch the hardware evidence before any PD_ALL_CLR. */
    if(status & IF_RX_RESET)
    {
        USBPD->STATUS |= IF_RX_RESET;
        /* A frame may have been in flight when the reset arrived: release the
         * CC drivers, otherwise a stuck CC_LVE/PD_TX_EN keeps the PHY from
         * ever starting the next frame (no IF_TX_END at all). */
        USBPD->PORT_CC1 &= (uint16_t)~(uint16_t)CC_LVE;
        USBPD->PORT_CC2 &= (uint16_t)~(uint16_t)CC_LVE;
        USBPD->CONTROL &= (uint8_t)~PD_TX_EN;
        s_phy_state = PD_PHY_IDLE;
        s_tx_result = PD_PORT_TX_RESULT_NONE;
        s_message_pending = 0u;
        s_rx_armed = 0u;
        pd_port_latch_hard_reset();
        NVIC_DisableIRQ(USBPD_IRQn);
        return;
    }

    if(status & IF_RX_ACT)
    {
        USBPD->STATUS |= IF_RX_ACT;

        if((status & MASK_PD_STAT) == PD_RX_SOP0)
        {
            uint8_t count = USBPD->BMC_BYTE_CNT;
            uint8_t type = 0u;
            uint8_t is_goodcrc;

            /* Ignore the PHY picking up its own transmission.  A loopback copy
             * of our frame must never be treated as a fresh message (it was
             * processed as "EPR Mode: unhandled action=1" once) and must never
             * be answered with a GoodCRC: that ACK collides with the Source's
             * real answer.  Compared against the hold copy, so it works even
             * though the shared PHY buffer now holds the received bytes.  A
             * GoodCRC for our frame starts with 0x41 and is therefore never
             * filtered here. */
            if(((s_phy_state == PD_PHY_TX) ||
                (s_phy_state == PD_PHY_WAIT_GOODCRC)) &&
               (s_tx_len >= 2u) &&
               (s_rx_dma_buf[0] == s_tx_dma_buf[0]) &&
               (s_rx_dma_buf[1] == s_tx_dma_buf[1]))
            {
                return;
            }

            if(count >= 2u)
                type = (uint8_t)(s_rx_dma_buf[0] & 0x1Fu);

            /* A GoodCRC is a control message (NDO = 0) carrying type 1, and it
             * must be recognised from the header alone: BMC_BYTE_CNT reads an
             * unreliable value (3/7 seen) when the frame arrives while the PHY
             * is still closing out a transmit.  A wrong byte count used to make
             * the GoodCRC look like a normal message, so it was answered with a
             * GoodCRC of our own - which collides with the Source's next frame
             * and is exactly how the negotiation kept falling apart. */
            is_goodcrc = ((type == PD_PORT_GOODCRC_TYPE) &&
                          ((s_rx_dma_buf[1] & 0x70u) == 0u)) ? 1u : 0u;

            if(s_phy_state == PD_PHY_WAIT_GOODCRC)
            {
                /* This must be the Source's GoodCRC echoing our Message ID. */
                if((is_goodcrc != 0u) &&
                   ((s_rx_dma_buf[1] & 0x0Eu) == s_tx_msgid))
                {
                    s_tx_result = PD_PORT_TX_RESULT_OK;
                    s_phy_state = PD_PHY_IDLE;
                }
            }
            else if(s_phy_state == PD_PHY_TX)
            {
                /* The Source's GoodCRC can overtake the IF_TX_END of our own
                 * frame in the same interrupt.  Accepting it here keeps the
                 * frame from being retransmitted a moment later (the retry
                 * would reach the Source as a duplicate Message ID). */
                if((is_goodcrc != 0u) &&
                   ((s_rx_dma_buf[1] & 0x0Eu) == s_tx_msgid))
                {
                    USBPD->STATUS |= IF_TX_END;   /* the frame did finish */
                    pd_port_enter_rx();
                    s_tx_result = PD_PORT_TX_RESULT_OK;
                    s_phy_state = PD_PHY_IDLE;
                }
            }
            else if(s_phy_state == PD_PHY_IDLE)
            {
                /* The hardware only raises IF_RX_ACT for a complete SOP0 frame,
                 * so every non-GoodCRC here is a real message to answer.  Do not
                 * gate this on BMC_BYTE_CNT: it reads stale values often enough
                 * that valid messages were being left unacknowledged. */
                if(is_goodcrc == 0u)
                {
                    /* Full SOP message: answer with GoodCRC right here (hard
                     * timing), then hand the bytes to the mailbox once the ACK
                     * frame has physically finished. */
                    TIME_DelayUs(30);

                    s_ack_buf[0] = 0x41;
                    s_ack_buf[1] = (uint8_t)((s_rx_dma_buf[1] & 0x0Eu) |
                                             s_auto_ack_pr_role);
                    s_rx_capture_len = pd_port_frame_bytes();

                    USBPD->STATUS |= IF_TX_END;
                    USBPD->CONFIG |= IE_TX_END;
                    s_auto_ack_started++;
                    s_ack_attempts_left = PD_PORT_ACK_MAX_ATTEMPTS - 1u;
                    s_phy_state = PD_PHY_ACK_TX;
                    s_tx_deadline_us = TIME_Micros() + PD_PORT_TX_END_TIMEOUT_US;
                    pd_port_begin_tx_lowlevel(s_ack_buf, 2u, UPD_SOP0);
                    return;
                }
            }
            else
            {
                /* Frame arrived while a frame or an ACK was still on the wire:
                 * its bytes land in the DMA buffer, but a capture that the new
                 * reception may have partially overwritten must never be
                 * published to the policy layer. */
                s_rx_capture_len = 0u;
            }
        }
    }

    if(status & IF_TX_END)
    {
        USBPD->PORT_CC1 &= (uint16_t)~(uint16_t)CC_LVE;
        USBPD->PORT_CC2 &= (uint16_t)~(uint16_t)CC_LVE;
        USBPD->STATUS |= IF_TX_END;

        if(s_phy_state == PD_PHY_ACK_TX)
        {
            s_auto_ack_completed++;
            s_last_auto_ack_completed_us = TIME_Micros();

            /* Deliver the acknowledged packet to the policy layer, then stop
             * servicing the PHY until the main loop consumed it (PD_Rx_Mode()
             * re-arms; a second packet in that window would overwrite it). */
            if((s_rx_mailbox != 0) && (s_rx_capture_len >= 2u))
            {
                memcpy(s_rx_mailbox, s_rx_dma_buf, s_rx_capture_len);
                s_message_pending = 1u;
            }
            s_phy_state = PD_PHY_IDLE;
            s_rx_armed = 0u;          /* PHY still holds the ACK transmit state */
            NVIC_DisableIRQ(USBPD_IRQn);
        }
        else if(s_phy_state == PD_PHY_TX)
        {
            /* Frame done: switch back to RX so the GoodCRC can arrive in the
             * next few tens of microseconds. */
            pd_port_enter_rx();

            if(s_tx_expect_goodcrc)
            {
                s_phy_state = PD_PHY_WAIT_GOODCRC;
                s_tx_deadline_us = TIME_Micros() + PD_PORT_GOODCRC_TIMEOUT_US;
            }
            else
            {
                s_tx_result = PD_PORT_TX_RESULT_OK;
                s_phy_state = PD_PHY_IDLE;
            }
        }
    }
}

void PD_Port_Init(uint8_t *rx_buffer, uint16_t rx_buffer_size)
{
    GPIO_InitTypeDef gpio = {0};

    s_rx_mailbox = rx_buffer;
    s_phy_state = PD_PHY_IDLE;
    s_tx_result = PD_PORT_TX_RESULT_NONE;
    s_tx_expect_goodcrc = 0u;
    s_tx_attempts_left = 0u;
    s_tx_len = 0u;
    s_tx_msgid = 0u;
    s_tx_deadline_us = 0u;
    s_rx_capture_len = 0u;
    s_message_pending = 0;
    s_hard_reset_pending = 0;
    s_auto_ack_pr_role = 0;
    s_auto_ack_started = 0;
    s_auto_ack_completed = 0;
    s_last_auto_ack_completed_us = 0u;
    s_last_detect_cc1 = 0u;
    s_last_detect_cc2 = 0u;
    s_rx_armed = 0u;
    s_last_phy_op_us = TIME_Micros();

    (void)rx_buffer_size; /* mailbox size is owned by the policy layer */

    RCC_APB2PeriphClockCmd(PD_CC1_GPIO_CLK | PD_CC2_GPIO_CLK, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);

    gpio.GPIO_Pin = PD_CC1_Pin;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(PD_CC1_GPIO_Port, &gpio);

    gpio.GPIO_Pin = PD_CC2_Pin;
    GPIO_Init(PD_CC2_GPIO_Port, &gpio);

    AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;

    /* PD_FILT_ED deliberately NOT set: it was enabled once "for reference
     * parity" and that was the only round in which the post-Enter
     * EPR_Get_Source_Cap stopped being acknowledged (GoodCRC timeouts x3,
     * then ~5 more silent frames).  The build without the filter had that same
     * EPR_Get acknowledged, so the receive path stays exactly as validated
     * until a capture proves otherwise. */
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE |
                    IF_RX_ACT | IF_RX_RESET | IF_TX_END;
}

void PD_Port_SetPowerRole(PD_Port_PowerRole role)
{
    if(role == PD_PORT_ROLE_SOURCE)
    {
        s_auto_ack_pr_role = 1;
        USBPD->PORT_CC1 = CC_CMP_66 | CC_PU_330;
        USBPD->PORT_CC2 = CC_CMP_66 | CC_PU_330;
    }
    else
    {
        s_auto_ack_pr_role = 0;
        USBPD->PORT_CC1 = CC_CMP_66 | CC_PD;
        USBPD->PORT_CC2 = CC_CMP_66 | CC_PD;
    }
}

void PD_Port_RxStart(void)
{
    /* Never reset the PHY while a frame is being transmitted (auto GoodCRC or
     * foreground TX): PD_ALL_CLR here would truncate it. */
    if(s_phy_state != PD_PHY_IDLE)
        return;

    /* Already listening?  Then this call is a no-op: re-writing BMC_START a few
     * tens of microseconds before the policy sends a frame opens a fresh PHY
     * settle window and the frame is never transmitted.  The old code called
     * this on every idle main-loop pass, which is exactly how the EPR handshake
     * and the post-Hard-Reset Get_Source_Cap kept failing. */
    if(s_rx_armed != 0u)
        return;

    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= (uint16_t)~(uint16_t)PD_ALL_CLR;
    USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET | PD_DMA_EN;
    USBPD->PORT_CC1 &= (uint16_t)~(uint16_t)CC_LVE;
    USBPD->PORT_CC2 &= (uint16_t)~(uint16_t)CC_LVE;
    USBPD->DMA = (uint32_t)(uintptr_t)s_rx_dma_buf;
    USBPD->CONTROL &= (uint8_t)~PD_TX_EN;
    USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
    USBPD->CONTROL |= BMC_START;
    s_rx_armed = 1u;
    s_last_phy_op_us = TIME_Micros();
    NVIC_EnableIRQ(USBPD_IRQn);
}

PD_Port_CC PD_Port_DetectAttach(void)
{
    uint8_t cc1_present = 0;
    uint8_t cc2_present = 0;

    /* This backend currently implements the Sink attach path used by the
     * product: test both CC pins against the same comparator threshold as the
     * original WCH USBPD_SNK example. */
    USBPD->PORT_CC1 &= (uint16_t)~(uint16_t)(CC_CMP_Mask | PA_CC_AI);
    USBPD->PORT_CC1 |= CC_CMP_22;
    TIME_DelayUs(2);
    if(USBPD->PORT_CC1 & PA_CC_AI)
        cc1_present = 1;

    USBPD->PORT_CC2 &= (uint16_t)~(uint16_t)(CC_CMP_Mask | PA_CC_AI);
    USBPD->PORT_CC2 |= CC_CMP_22;
    TIME_DelayUs(2);
    if(USBPD->PORT_CC2 & PA_CC_AI)
        cc2_present = 1;

    s_last_detect_cc1 = cc1_present;
    s_last_detect_cc2 = cc2_present;

    if((USBPD->PORT_CC1 & CC_PD) == 0)
        return PD_PORT_CC_NONE;

    if(cc1_present)
        return PD_PORT_CC1;
    if(cc2_present)
        return PD_PORT_CC2;

    return PD_PORT_CC_NONE;
}

void PD_Port_SelectCC(PD_Port_CC cc)
{
    if(cc == PD_PORT_CC2)
        USBPD->CONFIG |= CC_SEL;
    else
        USBPD->CONFIG &= (uint16_t)~(uint16_t)CC_SEL;
}

uint8_t PD_Port_StartTx(const uint8_t *buffer, uint8_t length,
                        uint8_t expect_goodcrc)
{
    if(s_phy_state != PD_PHY_IDLE)
        return 0u;
    if((length != 0u) && (buffer == 0))
        return 0u;
    if(length > PD_PORT_MAX_FRAME_BYTES)
        return 0u;

    if(length != 0u)
        memcpy(s_tx_dma_buf, buffer, length);

    s_tx_len = length;
    s_tx_expect_goodcrc = expect_goodcrc ? 1u : 0u;
    s_tx_attempts_left = s_tx_expect_goodcrc ? PD_PORT_TX_MAX_ATTEMPTS : 1u;
    s_tx_msgid = (length >= 2u) ? (uint8_t)(s_tx_dma_buf[1] & 0x0Eu) : 0u;
    s_tx_result = PD_PORT_TX_RESULT_BUSY;
    s_phy_state = PD_PHY_TX;
    s_tx_deadline_us = TIME_Micros() + PD_PORT_TX_END_TIMEOUT_US;

    pd_port_start_frame(s_tx_dma_buf, length, UPD_SOP0, 1u);
    return 1u;
}

uint8_t PD_Port_GetTxResult(void)
{
    return s_tx_result;
}

void PD_Port_ClearTxResult(void)
{
    if(s_tx_result != PD_PORT_TX_RESULT_BUSY)
        s_tx_result = PD_PORT_TX_RESULT_NONE;
}

uint8_t PD_Port_TxBusy(void)
{
    return (s_phy_state != PD_PHY_IDLE) ? 1u : 0u;
}

void PD_Port_AbortTx(void)
{
    if((s_phy_state == PD_PHY_TX) || (s_phy_state == PD_PHY_ACK_TX))
    {
        USBPD->STATUS |= IF_TX_END;
        pd_port_enter_rx();
    }

    s_phy_state = PD_PHY_IDLE;
    s_tx_result = PD_PORT_TX_RESULT_NONE;
    s_rx_capture_len = 0u;
}

/* Main-loop deadline engine: TX_END watchdog, GoodCRC timeout and retries. */
void PD_Port_Service(void)
{
    uint32_t now = TIME_Micros();

    if(s_phy_state == PD_PHY_TX)
    {
        if(pd_port_time_reached(now, s_tx_deadline_us) != 0u)
        {
            s_tx_end_timeouts++;

            /* Latch the failing PHY state before the turnaround sequence below
             * rewrites DMA/BMC_CLK_CNT and clears STATUS. */
            if(s_tx_fail_dumped == 0u)
            {
                s_tx_fail_dumped = 1u;
                s_tx_fail_control = USBPD->CONTROL;
                s_tx_fail_status = (uint8_t)USBPD->STATUS;
                s_tx_fail_byte_count = USBPD->BMC_BYTE_CNT;
                s_tx_fail_clk_cnt = USBPD->BMC_CLK_CNT;
                s_tx_fail_cc1 = USBPD->PORT_CC1;
                s_tx_fail_cc2 = USBPD->PORT_CC2;
                s_tx_fail_config = USBPD->CONFIG;
                s_tx_fail_dma_ok =
                    (USBPD->DMA == (uint32_t)(uintptr_t)s_rx_dma_buf) ? 1u : 0u;

                printf("[PD] TXto#1 start typ=%02X len=%u CTL=%02X ST=%02X CFG=%04X\r\n",
                       (unsigned)s_tx_start_type, (unsigned)s_tx_start_len,
                       (unsigned)s_tx_start_control,
                       (unsigned)s_tx_start_status,
                       (unsigned)s_tx_start_config);
                printf("[PD] TXto#1 start CC1=%04X CC2=%04X\r\n",
                       (unsigned)s_tx_start_cc1, (unsigned)s_tx_start_cc2);
                printf("[PD] TXto#1 now   CTL=%02X ST=%02X CNT=%u CLK=%u\r\n",
                       (unsigned)s_tx_fail_control,
                       (unsigned)s_tx_fail_status,
                       (unsigned)s_tx_fail_byte_count,
                       (unsigned)s_tx_fail_clk_cnt);
                printf("[PD] TXto#1 now   CC1=%04X CC2=%04X CFG=%04X DMA=%u\r\n",
                       (unsigned)s_tx_fail_cc1, (unsigned)s_tx_fail_cc2,
                       (unsigned)s_tx_fail_config,
                       (unsigned)s_tx_fail_dma_ok);
            }

            USBPD->STATUS |= IF_TX_END;
            pd_port_enter_rx();
            pd_port_tx_attempt_failed();
        }
    }
    else if(s_phy_state == PD_PHY_ACK_TX)
    {
        if(pd_port_time_reached(now, s_tx_deadline_us) != 0u)
        {
            s_ack_tx_timeouts++;

            if(s_ack_fail_dumped == 0u)
            {
                s_ack_fail_dumped = 1u;
                printf("[PD] ACKto#1 CTL=%02X ST=%02X CNT=%u CLK=%u\r\n",
                       (unsigned)USBPD->CONTROL, (unsigned)USBPD->STATUS,
                       (unsigned)USBPD->BMC_BYTE_CNT,
                       (unsigned)USBPD->BMC_CLK_CNT);
                printf("[PD] ACKto#1 CC1=%04X CC2=%04X CFG=%04X DMA=%u\r\n",
                       (unsigned)USBPD->PORT_CC1, (unsigned)USBPD->PORT_CC2,
                       (unsigned)USBPD->CONFIG,
                       (USBPD->DMA == (uint32_t)(uintptr_t)s_ack_buf) ? 1u : 0u);
            }

            USBPD->STATUS |= IF_TX_END;

            /* Re-answer the received frame instead of dropping it.  The frame
             * is still in s_rx_dma_buf: it is only published (or discarded)
             * once this path decides to give up.  The retry uses the ACK's own
             * buffer: the received message must stay intact for the policy. */
            if(s_ack_attempts_left != 0u)
            {
                s_ack_attempts_left--;
                USBPD->STATUS |= IF_TX_END;
                USBPD->CONFIG |= IE_TX_END;
                NVIC_EnableIRQ(USBPD_IRQn);
                USBPD->CONTROL &= (uint8_t)~(PD_TX_EN | BMC_START);
                s_phy_state = PD_PHY_ACK_TX;
                s_tx_deadline_us = TIME_Micros() + PD_PORT_TX_END_TIMEOUT_US;
                pd_port_begin_tx_lowlevel(s_ack_buf, 2u, UPD_SOP0);
            }
            else
            {
                pd_port_enter_rx();
                s_rx_capture_len = 0u;   /* the acknowledged packet is lost */
                s_phy_state = PD_PHY_IDLE;
                PD_Port_RxStart();
            }
        }
    }
    else if(s_phy_state == PD_PHY_WAIT_GOODCRC)
    {
        if(pd_port_time_reached(now, s_tx_deadline_us) != 0u)
        {
            s_goodcrc_timeouts++;
            pd_port_tx_attempt_failed();
        }
    }
}


void PD_Port_SendHardReset(void)
{
    /* Hard Reset has no GoodCRC transaction: fire it and let IF_TX_END close
     * the state.  It must never overlap a normal SOP transmission. */
    if(s_phy_state != PD_PHY_IDLE)
        return;

    s_tx_len = 0u;
    s_tx_expect_goodcrc = 0u;
    s_tx_attempts_left = 1u;
    s_tx_msgid = 0u;
    s_tx_result = PD_PORT_TX_RESULT_BUSY;
    s_phy_state = PD_PHY_TX;
    s_tx_deadline_us = TIME_Micros() + PD_PORT_TX_END_TIMEOUT_US;

    pd_port_start_frame(0, 0u, UPD_HARD_RESET, 1u);
}

uint8_t PD_Port_AutoAckBusy(void)
{
    return (s_phy_state == PD_PHY_ACK_TX) ? 1u : 0u;
}

void PD_Port_GetAutoAckStats(uint16_t *started, uint16_t *completed)
{
    if(started != 0)
        *started = s_auto_ack_started;
    if(completed != 0)
        *completed = s_auto_ack_completed;
}

uint8_t PD_Port_MessagePending(void)
{
    return s_message_pending;
}

void PD_Port_ClearMessageEvent(void)
{
    s_message_pending = 0;
}

uint8_t PD_Port_HardResetPending(void)
{
    return s_hard_reset_pending;
}

void PD_Port_ClearHardResetEvent(void)
{
    s_hard_reset_pending = 0;
}
void PD_Port_GetPhyDiag(PD_Port_PhyDiag *diag)
{
    if(diag == 0)
        return;

    diag->config = USBPD->CONFIG;
    diag->control = USBPD->CONTROL;
    diag->status = USBPD->STATUS;
    diag->port_cc1 = USBPD->PORT_CC1;
    diag->port_cc2 = USBPD->PORT_CC2;
    diag->bmc_byte_count = USBPD->BMC_BYTE_CNT;
    diag->selected_cc = (USBPD->CONFIG & CC_SEL) ? 2U : 1U;
    diag->last_detect_cc1 = s_last_detect_cc1;
    diag->last_detect_cc2 = s_last_detect_cc2;
    diag->tx_end_timeouts = s_tx_end_timeouts;
    diag->goodcrc_timeouts = s_goodcrc_timeouts;
    diag->ack_tx_timeouts = s_ack_tx_timeouts;
    diag->tx_start_status = s_tx_start_status;
    diag->tx_start_control = s_tx_start_control;
    diag->tx_start_sop = s_tx_start_sop;
    diag->tx_start_len = s_tx_start_len;
    diag->tx_start_cc1 = s_tx_start_cc1;
    diag->tx_start_cc2 = s_tx_start_cc2;
    diag->phy_state = s_phy_state;
    diag->tx_dma_intact = s_tx_fail_dma_ok;
    diag->hr_pd_stat = s_hr_pd_stat;
    diag->hr_byte_count = s_hr_byte_count;
}
