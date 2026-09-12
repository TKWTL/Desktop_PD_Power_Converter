/********************************** (C) COPYRIGHT *******************************
* File Name          : PD_process.c
* Author             : WCH
* Version            : V1.0.1
* Date               : 2025/10/27
* Description        : This file provides all the PD firmware functions.
*********************************************************************************
* Copyright (c) 2023 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#include "debug.h"
#include <string.h>
#include "pd.h"
#include "pd_port.h"

#define PD_EXT_TYPE_EXTENDED_CONTROL      0x10U
#define PD_EXT_TYPE_EPR_SOURCE_CAP        0x11U
#define PD_EXT_CTRL_EPR_GET_SOURCE_CAP    1U
#define PD_EXT_CTRL_EPR_GET_SINK_CAP      2U
#define PD_EXT_CTRL_EPR_KEEPALIVE         3U
#define PD_EXT_CTRL_EPR_KEEPALIVE_ACK     4U
#define PD_EPR_MODE_ENTER                 1U
#define PD_EPR_MODE_ENTER_ACK             2U
#define PD_EPR_MODE_ENTER_SUCCESS         3U
#define PD_EPR_MODE_ENTER_FAILED          4U
#define PD_EPR_MODE_EXIT                  5U
#define PD_EXT_CHUNK_DATA_MAX             26U
#define PD_EPR_CAP_BUFFER_SIZE            64U
/* Match the field-verified DemoBoard/C140 sequence.  Do not transmit
 * EPR_Get_Source_Cap from the Enter_Succeeded handler itself: let the
 * Source finish the EPR entry transition and send the Get after 120 ms. */
#define PD_EPR_GET_CAP_DELAY_MS           120U
/* Some Sources answer EPR_Get_Source_Cap late; the old 500 ms bail-out made us
 * send EPR_Mode(Exit) before the caps had a chance to arrive. */
#define PD_EPR_SOURCE_CAP_TIMEOUT_MS      1200U
/* The Source can be busy (internal rail / cable work) for a while right after
 * Enter_Succeeded and miss the first EPR_Get_Source_Cap outright: with the
 * same firmware, field runs either got the first Get acknowledged or none of
 * its three frame attempts.  Retry the Get spaced out (RX stays armed in
 * between, a recovering Source may also send the caps by itself) and only
 * fall back to SPR when the tries are used up. */
#define PD_EPR_GET_MAX_TRIES              6U
#define PD_EPR_GET_RETRY_GAP_MS           250U
#define PD_EPR_ENTER_TIMEOUT_MS           550U
#define PD_EPR_KEEPALIVE_PERIOD_MS        375U
#define PD_EPR_KEEPALIVE_ACK_TIMEOUT_MS   100U
#define PD_VBUS_POWERED_SUPPRESS_HARD_RESET 1U
#define PD_GET_SOURCE_CAP_RETRY_MS          700U
#define PD_GET_SOURCE_CAP_MAX_RETRIES       3U
#define PD_VBUS_DETACH_THRESHOLD_MV         3500U
#define PD_VBUS_DETACH_DEBOUNCE_COUNT       3U
/* One EPR attempt per power session: a failed attempt is only retried after
 * VBUS has been absent this long - long enough to be a human re-plug, far
 * longer than a Hard Reset's vSafe0V gap. */
#define PD_EPR_REATTACH_VBUS_OFF_MS         1000U

/* Internal WCH-derived protocol helpers.  Only PD_Init/PD_Task/getters are
 * exported through pd.h. */
static void PD_Rx_Mode(void);
static void PD_SINK_Init(void);
static void PD_PHY_Reset(void);
static UINT8 PD_Detect(void);
static void PD_Det_Proc(void);
static void PD_Load_Header(UINT8 ex, UINT8 msg_type);
static UINT8 PD_Send_Handle(const UINT8 *pbuf, UINT8 len, UINT8 job);
static void PD_Main_Proc(void);
static void PD_PDO_Analyse(UINT8 pdo_idx, UINT8 *srccap, UINT16 *current, UINT16 *voltage);



static __attribute__ ((aligned(4))) uint8_t PD_Rx_Buf[34];                           /* PD receive buffer */
static __attribute__ ((aligned(4))) uint8_t PD_Tx_Buf[34];                           /* PD send buffer */


static UINT16 Tmr_Ms_Dlt;                                                         /* elapsed milliseconds from free-running SysTick */

static PD_CONTROL PD_Ctl;                                                              /* PD Control Related Structures */

static UINT8 Adapter_SrcCap[30];                                                    /* SrcCap message from the adapter */

static __IO UINT8 PDO_Len;
static __IO UINT8 PD_Selected_PDO;
static __IO UINT16 PD_Selected_mV;
static __IO UINT16 PD_Selected_mA;
static __IO UINT8 PD_SourcePDO_Count;
static UINT8 PD_SourcePDO_Raw[28];
static __IO UINT8 PD_GetSrcCap_Sent;

/* USB PD 3.1 EPR sink state.  The CH32X035 PHY can transport these
 * messages; the WCH USBPD_SNK example does not provide the EPR policy
 * engine, so this file implements the small subset needed for 28V Fixed. */
static __IO UINT8 PD_Source_EPR_Capable;
static __IO UINT8 PD_EPR_ModeActive;
static __IO UINT8 PD_EPR_ContractActive;
static __IO UINT8 PD_SPR_ContractActive;
static __IO UINT8 PD_EPR_SourcePDO_Count;
static UINT8 PD_EPR_SourcePDO_Raw[PD_EPR_CAP_BUFFER_SIZE];

#define EPR_ST_OFF                 0u
#define EPR_ST_SPR_NEGOTIATING     1u
#define EPR_ST_WAIT_ENTER_ACK      2u
#define EPR_ST_WAIT_ENTER_SUCCESS  3u
#define EPR_ST_WAIT_SOURCE_CAP     4u
#define EPR_ST_WAIT_REQUEST_ACCEPT 5u
#define EPR_ST_WAIT_REQUEST_PSRDY  6u
#define EPR_ST_ACTIVE              7u

static __IO UINT8  PD_EPR_State;
static __IO UINT8  PD_SoftResetRecoveryPending;

/* Last SOP message consumed by the policy layer.  The dispatch below silently
 * ignores message types it does not implement, which made "ACKed but no reply"
 * situations impossible to diagnose: these fields keep the evidence. */
static __IO UINT8  PD_RxLast_Type;
static __IO UINT8  PD_RxLast_Ext;
static __IO UINT8  PD_RxLast_Ndo;
static __IO UINT16 PD_RxCount;
static UINT16 PD_RxWarnCount;
static UINT8  PD_VDM_LogCount;
static __IO UINT8  PD_ProtocolRecoveryCount;

/* Purpose of the frame currently owned by the asynchronous PHY.  The central
 * completion handler PD_TxCompletion_Proc() dispatches GoodCRC success and the
 * per-job failure recovery. */
#define PD_TXJOB_NONE        0u
#define PD_TXJOB_REPLY       1u
#define PD_TXJOB_REQUEST     2u
#define PD_TXJOB_GET_SRC_CAP 3u
#define PD_TXJOB_EPR_MODE    4u
#define PD_TXJOB_EPR_GET_CAP 5u
#define PD_TXJOB_EPR_CHUNK   6u
#define PD_TXJOB_EPR_REQ     7u
#define PD_TXJOB_KEEPALIVE   8u
#define PD_TXJOB_SOFT_RESET  9u

static __IO UINT8 PD_TxJob;
static __IO UINT8  PD_EPR_FailedForAttach;
static __IO UINT8  PD_EPR_GetCapSent;
static UINT8 PD_EPR_GetCapTries;
static __IO UINT8  PD_EPR_KeepAliveWaitAck;
static UINT16 PD_EPR_TimerMs;
static UINT16 PD_EPR_KeepAliveMs;
static UINT16 PD_EPR_KeepAliveAckMs;
static UINT16 PD_EPR_CapDataSize;
static UINT8  PD_EPR_LastChunk;
static UINT32 PD_EPR_SelectedRawPDO;

static uint32_t s_pd_task_last_ms;
static uint32_t s_pd_detect_last_ms;
static uint8_t s_pd_task_started;
static uint8_t s_pd_get_src_cap_retries;
static uint8_t s_pd_vbus_valid;
static UINT16 s_pd_vbus_low_ms;
static uint8_t s_pd_vbus_detach_count;
static uint16_t s_pd_vbus_mv;

/* SrcCap Table */
static const UINT8 SrcCap_5V3A_Tab[4]  = { 0X2C, 0X91, 0X01, 0X3E };
static const UINT8 SinkCap_5V1A_Tab[4] = { 0X64, 0X90, 0X01, 0X36 };

/* PD3.0 */
static const UINT8 SrcCap_Ext_Tab[28] =
{
    0X18, 0X80, 0X63, 0X00,
    0X00, 0X00, 0X00, 0X00,
    0X00, 0X00, 0X01, 0X00,
    0X00, 0X00, 0X07, 0X03,
    0X00, 0X00, 0X00, 0X00,
    0X00, 0X00, 0X00, 0X03,
    0X00, 0X12, 0X00, 0X00,
};

static const UINT8 Status_Ext_Tab[8] =
{
    0X06, 0X80, 0X16, 0X00,
    0X00, 0X00, 0X00, 0X00,
};


/*********************************************************************
 * @fn      PD_Rx_Mode
 *
 * @brief   This function uses to enter reception mode.
 *
 * @return  none
 */
static void PD_Rx_Mode( void )
{
    PD_Port_RxStart();
}

/*********************************************************************
 * @fn      PD_SINK_Init
 *
 * @brief   This function uses to initialize SNK mode.
 *
 * @return  none
 */
static void PD_SINK_Init( )
{
    PD_Ctl.Flag.Bit.PR_Role = 0;
    PD_Ctl.Flag.Bit.Auto_Ack_PRRole = 0;
    PD_Port_SetPowerRole(PD_PORT_ROLE_SINK);
}

/*********************************************************************
 * @fn      PD_PHY_Reset
 *
 * @brief   This function uses to reset PD PHY.
 *
 * @return  none
 */
static void PD_PHY_Reset( void )
{
    static UINT16 resets;

    /* One line per link reset: together with the reason printed by the caller
     * it pins down what happened immediately before Connected was cleared. */
    printf("[PD] PHY reset #%u (EPR state=%u)\r\n",
           (unsigned)(++resets), (unsigned)PD_EPR_State);

    PD_Port_AbortTx();
    /* Drop any policy context tied to a transmission that no longer exists. */
    PD_TxJob = PD_TXJOB_NONE;
    PD_SINK_Init( );
    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
    PD_Ctl.Flag.Bit.Connected = 0;
    PD_Ctl.Det_Cnt = 0;
    PD_Ctl.Msg_ID = 0;
    PD_Ctl.Err_Op_Cnt = 0;
    PD_Ctl.Flag.Bit.PD_Version = 0;
    PD_Ctl.PD_State = STA_IDLE;
    PD_Ctl.Flag.Bit.PD_Comm_Succ = 0;
    PD_Selected_PDO = 0;
    PD_Selected_mV = 0;
    PD_Selected_mA = 0;
    PD_SourcePDO_Count = 0;
    PD_GetSrcCap_Sent = 0;
    s_pd_get_src_cap_retries = 0u;
    s_pd_vbus_valid = 0u;
    s_pd_vbus_detach_count = 0u;

    PD_Source_EPR_Capable = 0;
    PD_EPR_ModeActive = 0;
    PD_EPR_ContractActive = 0;
    PD_SPR_ContractActive = 0;
    PD_EPR_SourcePDO_Count = 0;
    PD_EPR_State = EPR_ST_OFF;
    /* PD_EPR_FailedForAttach is deliberately NOT cleared here: a Hard Reset
     * re-enters this function, and retrying EPR after every Hard Reset turns
     * a Source that aborts EPR into an endless
     * SPR -> EPR -> Hard Reset -> SPR power-flap loop.  It is released only
     * by a real re-plug (see PD_Det_Proc). */
    PD_EPR_GetCapSent = 0;
    PD_EPR_GetCapTries = 0;
    PD_EPR_KeepAliveWaitAck = 0;
    PD_EPR_TimerMs = 0;
    PD_EPR_KeepAliveMs = 0;
    PD_EPR_KeepAliveAckMs = 0;
    PD_EPR_CapDataSize = 0;
    PD_EPR_LastChunk = 0;
    PD_EPR_SelectedRawPDO = 0;
    PD_SoftResetRecoveryPending = 0;
}

/*********************************************************************
 * @fn      PD_Init
 *
 * @brief   This function uses to initialize PD registers and states.
 *
 * @return  none
 */
void PD_Init( void )
{
    PD_Port_CC initial_cc;

    PD_Port_Init(PD_Rx_Buf, sizeof(PD_Rx_Buf));

    /* Initialize protocol/policy state. */
    memset(&PD_Ctl.PD_State, 0x00, sizeof(PD_CONTROL));
    Adapter_SrcCap[0] = 1;
    memcpy(&Adapter_SrcCap[1], SrcCap_5V3A_Tab, 4);
    PD_PHY_Reset();

    /* Dead-battery / bus-powered startup: the passive 5.1k Rd is already
     * visible to the Source before the MCU boots. Probe the active CC once
     * immediately and arm the receiver on that wire before the rest of the
     * application is initialized. Normal 5-sample attach debounce still runs
     * later if no valid CC is present at this instant. */
    initial_cc = PD_Port_DetectAttach();
    if(initial_cc != PD_PORT_CC_NONE)
    {
        PD_Port_SelectCC(initial_cc);
        PD_Ctl.Flag.Bit.Connected = 1u;
        PD_Ctl.PD_State = STA_SRC_CONNECT;
    }
    PD_Rx_Mode();
    s_pd_task_started = 0u;
    s_pd_task_last_ms = 0u;
    s_pd_detect_last_ms = 0u;
    s_pd_get_src_cap_retries = 0u;
    s_pd_vbus_valid = 0u;
    s_pd_vbus_detach_count = 0u;
    s_pd_vbus_mv = 0u;
}

/*********************************************************************
 * @fn      PD_Detect
 *
 * @brief   This function uses to detect CC connection.
 *
 * @return  0:No connection; 1:CC1 connection; 2:CC2 connection
 */
static UINT8 PD_Detect( void )
{
    if(PD_Ctl.Flag.Bit.Connected)
        return 0;

    return (UINT8)PD_Port_DetectAttach();
}

/*********************************************************************
 * @fn      PD_Det_Proc
 *
 * @brief   This function uses to process the return value of PD_Detect.
 *
 * @return  none
 */
static void PD_Det_Proc( void )
{
    UINT8  status;

    if( PD_Ctl.Flag.Bit.Connected )
    {
        /* WCH's SNK reference notes that detach should be judged from VBUS
         * for a bus-powered Sink.  APP feeds the PA7 resistor-divider ADC sample
         * through PD_SetVbusMillivolts(); keep the policy decision here. */
        if(s_pd_vbus_valid && s_pd_vbus_mv < PD_VBUS_DETACH_THRESHOLD_MV)
        {
            if(s_pd_vbus_detach_count < 0xFFu)
                s_pd_vbus_detach_count++;

            if(s_pd_vbus_detach_count >= PD_VBUS_DETACH_DEBOUNCE_COUNT)
            {
                printf("[PD] Disconnect: VBUS=%u mV; clearing contract and re-arming CC detection\r\n",
                       (unsigned)s_pd_vbus_mv);
                PD_PHY_Reset();
                PD_Rx_Mode();
            }
        }
        else
        {
            s_pd_vbus_detach_count = 0u;
        }
    }
    else
    {
        /* PD disconnected, check connection */
        status = PD_Detect( );
        /* Determine connection status */
        if( status == 0 )
        {
            PD_Ctl.Det_Cnt = 0;
        }
        else
        {
            PD_Ctl.Det_Cnt++;
        }
        if( PD_Ctl.Det_Cnt >= 5 )
        {
            PD_Ctl.Det_Cnt = 0;
            PD_Ctl.Flag.Bit.Connected = 1;
            if( PD_Ctl.Flag.Bit.Stop_Det_Chk == 0 )
            {
                /* PD_Detect() already guarantees a valid Sink attach. */
                PD_Port_SelectCC((status == 2) ? PD_PORT_CC2 : PD_PORT_CC1);
                PD_Ctl.PD_State = STA_SRC_CONNECT;
                s_pd_get_src_cap_retries = 0u;
                s_pd_vbus_detach_count = 0u;
                /* A fresh plug (VBUS stayed away long enough) may try EPR
                 * again; a re-attach right after a Hard Reset keeps the
                 * failed-attach flag so the retry loop cannot start. */
                if(s_pd_vbus_low_ms >= PD_EPR_REATTACH_VBUS_OFF_MS)
                    PD_EPR_FailedForAttach = 0u;
                s_pd_vbus_low_ms = 0u;
                printf("CC%d SRC Connect\r\n", status);
                PD_Ctl.PD_Comm_Timer = 0;
            }
        }
    }
}

/*********************************************************************
 * @fn      PD_Load_Header
 *
 * @brief   This function uses to load pd header packets.
 *
 * @return  none
 */
static void PD_Load_Header( UINT8 ex, UINT8 msg_type )
{
    /* Message Header
       BIT15 - Extended;
       BIT[14:12] - Number of Data Objects
       BIT[11:9] - Message ID
       BIT8 - PortPower Role/Cable Plug  0: SINK; 1: SOURCE
       BIT[7:6] - Revision, 00: V1.0; 01: V2.0; 10: V3.0;
       BIT5 - Port Data Role, 0: UFP; 1: DFP
       BIT[4:0] - Message Type
    */
    PD_Tx_Buf[ 0 ] = msg_type;
    if( PD_Ctl.Flag.Bit.PD_Role )
    {
        PD_Tx_Buf[ 0 ] |= 0x20;
    }
    if( PD_Ctl.Flag.Bit.PD_Version )
    {
        /* PD3.0 */
        PD_Tx_Buf[ 0 ] |= 0x80;
    }
    else
    {
        /* PD2.0 */
        PD_Tx_Buf[ 0 ] |= 0x40;
    }

    PD_Tx_Buf[ 1 ] = PD_Ctl.Msg_ID & 0x0E;
    if( PD_Ctl.Flag.Bit.PR_Role )
    {
        PD_Tx_Buf[ 1 ] |= 0x01;
    }
    if( ex )
    {
        PD_Tx_Buf[ 1 ] |= 0x80;
    }
}

/*********************************************************************
 * @fn      PD_Send_Handle
 *
 * @brief   This function uses to handle sending transactions.
 *
 * @return  0:success; 1:fail
 */
static UINT8 PD_Send_Handle( const UINT8 *pbuf, UINT8 len, UINT8 job )
{
    UINT8 cnt;

    if( ( len % 4 ) != 0 )
    {
        return( DEF_PD_TX_FAIL );
    }
    if( len > 28 )
    {
        return( DEF_PD_TX_FAIL );
    }

    cnt = len >> 2;
    PD_Tx_Buf[ 1 ] |= ( cnt << 4 );
    for( cnt = 0; cnt != len; cnt++ )
    {
        PD_Tx_Buf[ 2 + cnt ] = pbuf[ cnt ];
    }

    /* Keep only the sender-response critical section atomic.  This is the
     * DemoBoard/C140-proven path: SOP TX -> TX_END -> immediate RX -> GoodCRC.
     * Do not printf/yield/I2C/UI inside PD_Port_TransactSOP(). */
    PD_TxJob = PD_TXJOB_NONE;
    if(!PD_Port_TransactSOP(PD_Tx_Buf, (UINT8)(len + 2u), 3u))
        return DEF_PD_TX_FAIL;

    /* A retry keeps the same Message ID; only a GoodCRC completes the
     * transaction and advances the next outbound Message ID. */
    PD_Ctl.Msg_ID = (UINT8)((PD_Ctl.Msg_ID + 2u) & 0x0Eu);

    /* These two flags/timers used to be set by the asynchronous completion
     * dispatcher.  The transaction is already complete when this function
     * returns, so update them here without adding timing-sensitive prints. */
    if(job == PD_TXJOB_GET_SRC_CAP)
    {
        PD_GetSrcCap_Sent = 1u;
    }
    else if(job == PD_TXJOB_EPR_GET_CAP)
    {
        PD_EPR_TimerMs = 0u;
    }

    return DEF_PD_TX_OK;
}

/*********************************************************************
 * @fn      PDO_Request
 *
 * @brief   This function uses to Send the specified PDO.
 *
 * @return  none
 */
void PDO_Request( UINT8 pdo_index )
{
    UINT16 Current, Voltage;
    UINT16 request_ma;
    UINT32 rdo;
    UINT8 payload[4];
    UINT8 status;

    if ((pdo_index > PDO_Len) || (pdo_index == 0))
    {
        printf("[PD] invalid SPR PDO index %u\r\n", pdo_index);
        PD_Ctl.PD_State = STA_TX_SOFTRST;
        return;
    }

    PD_PDO_Analyse(pdo_index, &Adapter_SrcCap[1], &Current, &Voltage);
    request_ma = Current;
    if(request_ma > PD_SPR_REQUEST_MAX_MA) request_ma = PD_SPR_REQUEST_MAX_MA;

    /* Fixed/Variable RDO: Object Position B31:28, No USB Suspend B24,
     * EPR Mode Capable B22, operating/max current in 10mA units.
     * We deliberately leave B23=0 so the Source must use chunked Extended
     * Messages; PD_Rx_Buf is sized for one 26-byte Extended chunk. */
    rdo = ((UINT32)(pdo_index & 0x0Fu) << 28) | (1UL << 24);
#if PD_EPR_ENABLE
    if(PD_Source_EPR_Capable && !PD_EPR_FailedForAttach)
        rdo |= (1UL << 22);
#endif
    rdo |= ((UINT32)((request_ma / 10u) & 0x03FFu) << 10);
    rdo |=  (UINT32)((request_ma / 10u) & 0x03FFu);

    payload[0] = (UINT8)(rdo);
    payload[1] = (UINT8)(rdo >> 8);
    payload[2] = (UINT8)(rdo >> 16);
    payload[3] = (UINT8)(rdo >> 24);

    PD_Selected_PDO = pdo_index;
    PD_Selected_mV = Voltage;
    PD_Selected_mA = request_ma;
    PD_Ctl.ReqPDO_Idx = pdo_index;

    /* Do not print/flush between Source_Capabilities GoodCRC and Request.
     * The Source's SenderResponseTimer is already running in this interval. */
    PD_Load_Header(0x00, DEF_TYPE_REQUEST);
    status = PD_Send_Handle(payload, 4, PD_TXJOB_REQUEST);

    if(status == DEF_PD_TX_OK)
        PD_Ctl.PD_State = STA_RX_ACCEPT_WAIT;

    printf("[PD] SPR Request PDO%u: %u mV, %u mA%s\r\n",
           pdo_index, Voltage, request_ma,
           (rdo & (1UL << 22)) ? ", EPR-capable RDO" : "");

    if(status != DEF_PD_TX_OK && PD_Port_HardResetPending())
    {
        printf("[PD] SPR Request terminated by Source Hard Reset\r\n");
        PD_Ctl.PD_State = STA_IDLE;
    }
    else if(status != DEF_PD_TX_OK)
    {
        printf("[PD] SPR Request TX/GoodCRC failed; scheduling Soft Reset\r\n");
        PD_Ctl.PD_State = STA_TX_SOFTRST;
    }

    PD_Ctl.PD_Comm_Timer = 0;
    PD_Ctl.Flag.Bit.PD_Comm_Succ = 1;
}

/*********************************************************************
 * @fn      PD_Save_Adapter_SrcCap
 *
 * @brief   This function uses to save the adapter SrcCap information.
 *
 * @return  none
 */
static void PD_Save_Adapter_SrcCap( void )
{
    UINT8  i, len;

    /* Calculate the number of NDO's (Number of Data Objects) in the Message Header. */
    len = ( ( PD_Rx_Buf[ 1 ] >> 4 ) & 0x07 );

    /* Preserve the exact Source_Capabilities payload for terminal diagnostics
     * before the original WCH sample trims APDOs for its fixed-PDO request path. */
    PD_SourcePDO_Count = len;
    memcpy(PD_SourcePDO_Raw, &PD_Rx_Buf[2], (len << 2));

    /* Remove the PPS section */
    for( i = 0; i < len; i++ )
    {
        if( ( PD_Rx_Buf[ 2 + ( i << 2 ) + 3 ] & 0xC0 ) == 0xC0 )
        {
            break;
        }
    }

    PDO_Len = i;

    /* Modify SrcCap information */
       /* BIT[31:30] - Fixed Supply */
       /* BIT29 - Dual-Role Power */
       /* BIT28 - USB Suspend Power */
       /* BIT27 - Unconstrained Power */
       /* BIT26 - USB Communications */
       /* BIT25 - Dual-Role Data */
       /* BIT24 - Unchunked Extended Message Supported */
       /* BIT23 - EPR Mode Capable */
       /* BIT22 - Reserved,shall be set to zero */
       /* BIT[21:20] - Peak Current */
       /* BIT[19:10] - Voltage in 50mV units */
       /* BIT[9:0] - Maximum Current in 10mA units */
    /* Keep the received PDO bytes unchanged so printed raw PDO values remain exact. */

    /* Save the adapter's SrcCap information */
    PD_Rx_Buf[ 1 ] &= 0x8F;
    PD_Rx_Buf[ 1 ] |= i << 4;
    Adapter_SrcCap[ 0 ] = i;
    memcpy( &Adapter_SrcCap[ 1 ], &PD_Rx_Buf[ 2 ], ( i << 2 ) );
}

/*********************************************************************
 * @fn      PD_PDO_Analyse
 *
 * @brief   This function uses to analyse PDO's voltage and current.
 *
 * @return  none
 */
static void PD_PDO_Analyse( UINT8 pdo_idx, UINT8 *srccap, UINT16 *current, UINT16 *voltage )
{
    UINT32 temp32;

    temp32 = srccap[ (  ( pdo_idx - 1 ) << 2 ) + 0 ] +
                        ( (UINT32)srccap[ ( ( pdo_idx - 1 ) << 2 ) + 1 ] << 8 ) +
                        ( (UINT32)srccap[ ( ( pdo_idx - 1 ) << 2 ) + 2 ] << 16 );

    /* Calculation of current values */
    if( current != NULL )
    {
        *current = ( temp32 & 0x000003FF ) * 10;
    }

    /* Calculation of voltage values */
    if( voltage != NULL )
    {
        temp32 = temp32 >> 10;
        *voltage = ( temp32 & 0x000003FF ) * 50;
    }
}


/*********************************************************************
 * @fn      PD_Select_Highest_Fixed_PDO
 *
 * @brief   Select highest fixed PDO not exceeding board policy limit.
 *          Tie-breaker: higher available current.
 *
 * @return  PDO index (1..PDO_Len), 0 if none is usable.
 */
static UINT8 PD_Select_Highest_Fixed_PDO(void)
{
    UINT8 i;
    UINT8 best = 0;
    UINT16 best_mv = 0;
    UINT16 best_ma = 0;

    for(i = 1; i <= PDO_Len; i++)
    {
        UINT8 *p = &Adapter_SrcCap[1 + ((i - 1) << 2)];
        UINT32 raw = (UINT32)p[0] |
                     ((UINT32)p[1] << 8) |
                     ((UINT32)p[2] << 16) |
                     ((UINT32)p[3] << 24);
        UINT8 supply_type = (UINT8)(raw >> 30);
        UINT16 mv;
        UINT16 ma;

        if(supply_type != 0) continue;   /* fixed PDO only */
        mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
        ma = (UINT16)((raw & 0x3FFu) * 10u);
        if(mv > PD_REQUEST_MAX_FIXED_MV) continue;

        if((mv > best_mv) || ((mv == best_mv) && (ma > best_ma)))
        {
            best = i;
            best_mv = mv;
            best_ma = ma;
        }
    }

    PD_Selected_PDO = best;
    PD_Selected_mV = best_mv;
    PD_Selected_mA = best_ma;
    PD_Ctl.ReqPDO_Idx = best;
    return best;
}

static void PD_Print_Source_PDOs(void)
{
    UINT8 i;
    static UINT8 printed_tables;

    /* The full table is six lines.  Printing it on every re-negotiation fills
     * the 256-byte USART ring and the driver then drops lines - which is how
     * the decisive "who reset the link" evidence disappeared from earlier
     * captures.  Keep the first two tables of a session complete, afterwards
     * only a one-line summary. */
    if(printed_tables >= 2u)
    {
        printf("[PD] Source_Capabilities: %u PDO(s) (table already printed)\r\n",
               (unsigned)PD_SourcePDO_Count);
        return;
    }
    printed_tables++;

    printf("[PD] Source_Capabilities: %u PDO(s); fixed-request policy max %u mV:\r\n",
           PD_SourcePDO_Count, (unsigned)PD_REQUEST_MAX_FIXED_MV);

    for(i = 1; i <= PD_SourcePDO_Count; i++)
    {
        UINT8 *p = &PD_SourcePDO_Raw[(i - 1) << 2];
        UINT32 raw = (UINT32)p[0] |
                     ((UINT32)p[1] << 8) |
                     ((UINT32)p[2] << 16) |
                     ((UINT32)p[3] << 24);
        UINT8 supply_type = (UINT8)(raw >> 30);

        if(supply_type == 0)
        {
            UINT16 mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 ma = (UINT16)((raw & 0x3FFu) * 10u);
            printf("  PDO%u raw=%08lx FIXED %u mV %u mA%s\r\n",
                   i, (unsigned long)raw, mv, ma,
                   (i == PD_Selected_PDO) ? "  <REQUEST>" : "");
        }
        else if(supply_type == 1)
        {
            UINT16 min_mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 max_mv = (UINT16)(((raw >> 20) & 0x3FFu) * 50u);
            UINT32 max_mw = (UINT32)(raw & 0x3FFu) * 250u;
            printf("  PDO%u raw=%08lx BATTERY %u-%u mV, %lu mW\r\n",
                   i, (unsigned long)raw, min_mv, max_mv, (unsigned long)max_mw);
        }
        else if(supply_type == 2)
        {
            UINT16 min_mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 max_mv = (UINT16)(((raw >> 20) & 0x3FFu) * 50u);
            UINT16 ma = (UINT16)((raw & 0x3FFu) * 10u);
            printf("  PDO%u raw=%08lx VARIABLE %u-%u mV, %u mA\r\n",
                   i, (unsigned long)raw, min_mv, max_mv, ma);
        }
        else
        {
            UINT8 apdo_type = (UINT8)((raw >> 28) & 0x03u);
            if(apdo_type == 0)
            {
                UINT16 min_mv = (UINT16)(((raw >> 8) & 0xFFu) * 100u);
                UINT16 max_mv = (UINT16)(((raw >> 17) & 0xFFu) * 100u);
                UINT16 ma = (UINT16)((raw & 0x7Fu) * 50u);
                printf("  PDO%u raw=%08lx PPS_APDO %u-%u mV, %u mA (not requested)\r\n",
                       i, (unsigned long)raw, min_mv, max_mv, ma);
            }
            else
            {
                printf("  PDO%u raw=%08lx APDO subtype=%u (not requested)\r\n",
                       i, (unsigned long)raw, apdo_type);
            }
        }
    }
}

static UINT32 PD_ReadU32LE(const UINT8 *p)
{
    return (UINT32)p[0] |
           ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) |
           ((UINT32)p[3] << 24);
}

static void PD_WriteU32LE(UINT8 *p, UINT32 v)
{
    p[0] = (UINT8)v;
    p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)(v >> 16);
    p[3] = (UINT8)(v >> 24);
}

static UINT8 PD_Send_Extended(UINT8 msg_type, UINT16 ext_header,
                              const UINT8 *data, UINT8 data_len, UINT8 job)
{
    UINT8 payload[28];
    UINT8 total = (UINT8)(data_len + 2u);
    UINT8 padded = (UINT8)((total + 3u) & 0xFCu);

    if(padded == 0) padded = 4;
    if(padded > sizeof(payload)) return DEF_PD_TX_FAIL;

    memset(payload, 0, padded);
    payload[0] = (UINT8)ext_header;
    payload[1] = (UINT8)(ext_header >> 8);
    if(data_len && data) memcpy(&payload[2], data, data_len);

    PD_Load_Header(0x01, msg_type);
    return PD_Send_Handle(payload, padded, job);
}

static UINT8 PD_Send_Extended_Control(UINT8 ctrl_type, UINT8 job)
{
    UINT8 ecdb[2];
    UINT16 ext_header;

    /* We advertise no unchunked-extended support in the RDO, so use the
     * chunked format even though this ECDB is only two bytes. */
    ext_header = (UINT16)(0x8000u | 2u);   /* Chunked=1, Chunk#0, DataSize=2 */
    ecdb[0] = ctrl_type;
    ecdb[1] = 0;
    return PD_Send_Extended(PD_EXT_TYPE_EXTENDED_CONTROL, ext_header, ecdb, 2, job);
}

static UINT8 PD_Send_Extended_Chunk_Request(UINT8 msg_type, UINT8 chunk_number, UINT8 job)
{
    UINT16 ext_header;
    ext_header = (UINT16)(0x8000u | 0x0400u |
                          ((UINT16)(chunk_number & 0x0Fu) << 11));
    /* RequestChunk=1 requires DataSize=0.  Two padding bytes are emitted by
     * PD_Send_Extended so NDO=1. */
    return PD_Send_Extended(msg_type, ext_header, NULL, 0, job);
}

static UINT8 PD_Send_EPR_Mode(UINT8 action, UINT8 data, UINT8 job)
{
    UINT8 p[4];
    UINT32 eprmdo = ((UINT32)action << 24) | ((UINT32)data << 16);
    PD_WriteU32LE(p, eprmdo);
    PD_Load_Header(0x00, DEF_TYPE_EPR_MODE);
    return PD_Send_Handle(p, 4, job);
}

static void PD_LocalProtocolRecover(const char *reason)
{
    PD_ProtocolRecoveryCount++;
    if(reason)
        printf("[PD] protocol recovery #%u: %s; Hard Reset suppressed on VBUS-powered board\r\n",
               (unsigned)PD_ProtocolRecoveryCount, reason);

    PD_PHY_Reset();
    PD_EPR_FailedForAttach = 1u;
    PD_Ctl.Flag.Bit.Connected = 0u;
    PD_Ctl.Det_Cnt = 0u;
    PD_Ctl.PD_Comm_Timer = 0u;
    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0u;
    PD_GetSrcCap_Sent = 0u;
    PD_Rx_Mode();
}

static void PD_EPR_Fallback(const char *reason)
{
    if(reason) printf("[PD] EPR fallback: %s; keeping SPR contract\r\n", reason);
    PD_EPR_FailedForAttach = 1;
    PD_EPR_State = EPR_ST_OFF;
    PD_EPR_ModeActive = 0;
    PD_EPR_ContractActive = 0;
    PD_EPR_GetCapSent = 0;
    PD_EPR_GetCapTries = 0;
    PD_EPR_CapDataSize = 0;
    PD_EPR_KeepAliveWaitAck = 0;
    PD_EPR_TimerMs = 0;
    PD_EPR_KeepAliveMs = 0;
}

static void PD_EPR_Exit_To_SPR(const char *reason)
{
    if(reason) printf("[PD] EPR exit: %s\r\n", reason);
    (void)PD_Send_EPR_Mode(PD_EPR_MODE_EXIT, 0, PD_TXJOB_REPLY);
    PD_EPR_Fallback(NULL);
}

/* Central completion dispatcher for asynchronous transmissions.  PD_TxJob
 * carries the caller's context; the recovery actions here mirror what the old
 * blocking PD_Send_Handle() callers used to do inline. */
static void PD_TxCompletion_Proc(void)
{
    UINT8 result = PD_Port_GetTxResult();
    UINT8 job;

    if((result != PD_PORT_TX_RESULT_OK) && (result != PD_PORT_TX_RESULT_ERR))
        return;

    PD_Port_ClearTxResult();

    /* Take the finished job out before dispatching it: several failure handlers
     * below start a new transmission (PD_EPR_Exit_To_SPR() sends EPR_Mode Exit,
     * for example), and that fresh PD_TxJob must not be wiped by this frame's
     * bookkeeping - the old code assigned PD_TXJOB_NONE after the switch and
     * silently killed the follow-up transaction's context. */
    job = PD_TxJob;
    PD_TxJob = PD_TXJOB_NONE;

    /* Advance the Message ID when the transaction ends, never at send time: a
     * PHY retry keeps the ID the Source may already have seen, and the next
     * message cannot be taken for a duplicate.  Hard Reset carries no header
     * and is sent without a job, so it does not consume an ID. */
    if(job != PD_TXJOB_NONE)
        PD_Ctl.Msg_ID = (UINT8)((PD_Ctl.Msg_ID + 2u) & 0x0Eu);

    if(result == PD_PORT_TX_RESULT_OK)
    {
        switch(job)
        {
            case PD_TXJOB_GET_SRC_CAP:
                PD_GetSrcCap_Sent = 1u;
                printf("[PD] Get_Source_Cap sent; waiting for Source_Capabilities\r\n");
                break;

            case PD_TXJOB_EPR_GET_CAP:
                printf("[PD] EPR_Get_Source_Cap sent (try %u)\r\n",
                       (unsigned)PD_EPR_GetCapTries);
                PD_EPR_TimerMs = 0;
                break;

            case PD_TXJOB_EPR_REQ:
                /* The Source answers with Accept/PS_RDY (or Reject); until then
                 * this line marks that the 28 V request is on the wire. */
                printf("[PD] EPR_Request sent; waiting for Accept\r\n");
                break;

            default:
                break;
        }
    }
    else
    {
        {
            PD_Port_PhyDiag phy;
            PD_Port_GetPhyDiag(&phy);
            /* TO = tx_end / goodcrc / ack-response timeout counters: the split
             * tells "PHY never finished the frame" apart from "partner never
             * acknowledged it" instead of blaming the PHY for both. */
            printf("[PD] TX failed (job=%u) ST=%02X CNT=%u\r\n",
                   (unsigned)job, (unsigned)phy.status,
                   (unsigned)phy.bmc_byte_count);
            printf("[PD] TX phy TO=%u/%u/%u CC%u CFG=%04X\r\n",
                   (unsigned)phy.tx_end_timeouts,
                   (unsigned)phy.goodcrc_timeouts,
                   (unsigned)phy.ack_tx_timeouts,
                   (unsigned)phy.selected_cc, (unsigned)phy.config);
        }

        switch(job)
        {
            case PD_TXJOB_REQUEST:
                if(PD_Port_HardResetPending())
                {
                    printf("[PD] SPR Request terminated by Source Hard Reset\r\n");
                    PD_Ctl.PD_State = STA_IDLE;
                }
                else
                {
                    printf("[PD] SPR Request TX/GoodCRC failed; scheduling Soft Reset\r\n");
                    PD_Ctl.PD_State = STA_TX_SOFTRST;
                }
                break;

            case PD_TXJOB_GET_SRC_CAP:
                printf("[PD] Get_Source_Cap TX failed; keeping RX armed for retry\r\n");
                PD_GetSrcCap_Sent = 0u;
                break;

            case PD_TXJOB_EPR_MODE:
                PD_EPR_Fallback("EPR Mode Enter TX failed");
                break;

            case PD_TXJOB_EPR_GET_CAP:
                /* The Source can be busy/deaf for a while right after
                 * Enter_Succeeded (same firmware: sometimes the first Get is
                 * acknowledged, sometimes none of its frame attempts are).
                 * Retry with a gap instead of dropping EPR; only leave when
                 * the try budget is used up.  A failure while caps are already
                 * arriving / the session moved on is stale - ignore it. */
                if((PD_EPR_State == EPR_ST_WAIT_SOURCE_CAP) &&
                   (PD_EPR_CapDataSize == 0u))
                {
                    PD_EPR_GetCapSent = 0;
                    PD_EPR_TimerMs = 0;
                    if(PD_EPR_GetCapTries < PD_EPR_GET_MAX_TRIES)
                        printf("[PD] EPR_Get_Source_Cap TX failed; retrying (%u/%u)\r\n",
                               (unsigned)PD_EPR_GetCapTries, (unsigned)PD_EPR_GET_MAX_TRIES);
                    else
                        PD_EPR_Exit_To_SPR("EPR_Get_Source_Cap TX failed");
                }
                else
                {
                    printf("[PD] stale EPR_Get_Source_Cap failure ignored (EPRst=%u)\r\n",
                           (unsigned)PD_EPR_State);
                }
                break;

            case PD_TXJOB_EPR_CHUNK:
                PD_EPR_Exit_To_SPR("EPR chunk request TX failed");
                break;

            case PD_TXJOB_EPR_REQ:
                PD_EPR_Exit_To_SPR("EPR_Request TX failed");
                break;

            case PD_TXJOB_KEEPALIVE:
                PD_EPR_KeepAliveWaitAck = 0;
                PD_EPR_KeepAliveAckMs = 0;
                break;

            case PD_TXJOB_SOFT_RESET:
                PD_SoftResetRecoveryPending = 0u;
                PD_LocalProtocolRecover("Soft Reset TX failed");
                break;

            default:
                break;
        }
    }
}

static void PD_Print_EPR_Source_PDOs(void)
{
    UINT8 i;
    printf("[PD] EPR_Source_Capabilities: %u PDO slots (%u bytes)\r\n",
           PD_EPR_SourcePDO_Count, PD_EPR_CapDataSize);

    for(i = 1; i <= PD_EPR_SourcePDO_Count; i++)
    {
        UINT32 raw = PD_ReadU32LE(&PD_EPR_SourcePDO_Raw[(i - 1u) << 2]);
        UINT8 supply_type = (UINT8)(raw >> 30);

        if(raw == 0)
        {
            printf("  PDO%u raw=00000000 <empty>\r\n", i);
        }
        else if(supply_type == 0)
        {
            UINT16 mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 ma = (UINT16)((raw & 0x3FFu) * 10u);
            printf("  PDO%u raw=%08lx FIXED %u mV %u mA%s\r\n",
                   i, (unsigned long)raw, mv, ma,
                   (i == PD_Selected_PDO) ? "  <EPR TARGET>" : "");
        }
        else if(supply_type == 1)
        {
            UINT16 min_mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 max_mv = (UINT16)(((raw >> 20) & 0x3FFu) * 50u);
            UINT32 max_mw = (UINT32)(raw & 0x3FFu) * 250u;
            printf("  PDO%u raw=%08lx BATTERY %u-%u mV %lu mW\r\n",
                   i, (unsigned long)raw, min_mv, max_mv, (unsigned long)max_mw);
        }
        else if(supply_type == 2)
        {
            UINT16 min_mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 max_mv = (UINT16)(((raw >> 20) & 0x3FFu) * 50u);
            UINT16 ma = (UINT16)((raw & 0x3FFu) * 10u);
            printf("  PDO%u raw=%08lx VARIABLE %u-%u mV %u mA\r\n",
                   i, (unsigned long)raw, min_mv, max_mv, ma);
        }
        else
        {
            UINT8 apdo_type = (UINT8)((raw >> 28) & 0x03u);
            if(apdo_type == 0)
            {
                UINT16 min_mv = (UINT16)(((raw >> 8) & 0xFFu) * 100u);
                UINT16 max_mv = (UINT16)(((raw >> 17) & 0xFFu) * 100u);
                UINT16 ma = (UINT16)((raw & 0x7Fu) * 50u);
                printf("  PDO%u raw=%08lx PPS %u-%u mV %u mA\r\n",
                       i, (unsigned long)raw, min_mv, max_mv, ma);
            }
            else if(apdo_type == 1)
            {
                UINT16 min_mv = (UINT16)(((raw >> 8) & 0xFFu) * 100u);
                UINT16 max_mv = (UINT16)(((raw >> 17) & 0x1FFu) * 100u);
                UINT16 pdp_w = (UINT16)(raw & 0xFFu);
                printf("  PDO%u raw=%08lx AVS %u-%u mV PDP=%u W\r\n",
                       i, (unsigned long)raw, min_mv, max_mv, pdp_w);
            }
            else
            {
                printf("  PDO%u raw=%08lx APDO subtype=%u\r\n",
                       i, (unsigned long)raw, apdo_type);
            }
        }
    }
}

static UINT8 PD_Select_EPR_28V_Fixed(void)
{
    UINT8 i;
    UINT8 best = 0;
    UINT16 best_ma = 0;

    for(i = 8; i <= PD_EPR_SourcePDO_Count; i++)
    {
        UINT32 raw = PD_ReadU32LE(&PD_EPR_SourcePDO_Raw[(i - 1u) << 2]);
        if((raw >> 30) == 0)
        {
            UINT16 mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
            UINT16 ma = (UINT16)((raw & 0x3FFu) * 10u);
            if(mv == PD_EPR_TARGET_MV && ma > best_ma)
            {
                best = i;
                best_ma = ma;
                PD_EPR_SelectedRawPDO = raw;
            }
        }
    }

    if(best)
    {
        PD_Selected_PDO = best;
        PD_Selected_mV = PD_EPR_TARGET_MV;
        PD_Selected_mA = best_ma;
        if(PD_Selected_mA > PD_EPR_REQUEST_MAX_MA)
            PD_Selected_mA = PD_EPR_REQUEST_MAX_MA;
        PD_Ctl.ReqPDO_Idx = best;
    }
    return best;
}

static UINT8 PD_Send_EPR_Request_Fixed(UINT8 pdo_index)
{
    UINT8 payload[8];
    UINT32 rdo;
    UINT16 units = (UINT16)(PD_Selected_mA / 10u);

    /* Field set identical to the DemoBoard build that is field-verified on
     * this very charger (Lenovo C140 -> stable 28 V): Object Position, No USB
     * Suspend (B24), EPR Capable (B22), operating/maximum current in 10 mA
     * units.  B25 (USB Communications Capable) stays 0 - the verified build
     * leaves it clear; it was added here only for parity with a different
     * reference and is a byte-level deviation from the working frame. */
    rdo = ((UINT32)(pdo_index & 0x0Fu) << 28) |
          (1UL << 24) |                 /* No USB Suspend */
          (1UL << 22) |                 /* EPR Mode Capable */
          ((UINT32)(units & 0x03FFu) << 10) |
          (UINT32)(units & 0x03FFu);

    PD_WriteU32LE(&payload[0], rdo);
    /* Second data object: the selected source PDO verbatim.  Measured against
     * the Source, this is the form it accepts at the link layer: with NDO = 2
     * it acknowledges the frame and stays silent (it never grants), while
     * dropping to the RDO alone (NDO = 1) makes it answer with an immediate
     * Hard Reset - i.e. it treats the shorter message as invalid.  Keep NDO=2
     * until a capture of a known-good Sink exchanging EPR says otherwise. */
    PD_WriteU32LE(&payload[4], PD_EPR_SelectedRawPDO);

    PD_Load_Header(0x00, DEF_TYPE_EPR_REQUEST);
    return PD_Send_Handle(payload, 8, PD_TXJOB_EPR_REQ);
}

static void PD_EPR_Capabilities_Complete(void)
{
    UINT8 target;
    UINT8 status;

    if((PD_EPR_CapDataSize == 0) || ((PD_EPR_CapDataSize & 3u) != 0))
    {
        PD_EPR_Exit_To_SPR("invalid EPR Source_Capabilities length");
        return;
    }

    PD_EPR_SourcePDO_Count = (UINT8)(PD_EPR_CapDataSize >> 2);
    target = PD_Select_EPR_28V_Fixed();

    /* Print before the EPR_Request.  After transmitting a request, the Source
     * may answer Accept immediately; avoiding printf after TX prevents the
     * next RX packet from being overwritten/lost by the old WCH main-loop
     * receive pattern. */
    PD_Print_EPR_Source_PDOs();

    if(target == 0)
    {
        PD_EPR_Exit_To_SPR("no 28V Fixed PDO");
        return;
    }

    printf("[PD] EPR Request PDO%u: %u mV, %u mA\r\n",
           target, PD_Selected_mV, PD_Selected_mA);
    status = PD_Send_EPR_Request_Fixed(target);
    if(status == DEF_PD_TX_OK)
    {
        PD_EPR_State = EPR_ST_WAIT_REQUEST_ACCEPT;
        PD_Ctl.PD_State = STA_RX_ACCEPT_WAIT;
        PD_Ctl.PD_Comm_Timer = 0;
        PD_EPR_TimerMs = 0;
    }
    else
    {
        PD_EPR_Exit_To_SPR("EPR_Request TX failed");
    }
}

static void PD_Handle_EPR_Source_Capabilities(void)
{
    UINT16 ext_header;
    UINT16 data_size;
    UINT16 offset;
    UINT16 remain;
    UINT8 ndo;
    UINT8 chunked;
    UINT8 request_chunk;
    UINT8 chunk_number;
    UINT8 available;
    UINT8 copy_len;

    ext_header = (UINT16)PD_Rx_Buf[2] | ((UINT16)PD_Rx_Buf[3] << 8);
    data_size = (UINT16)(ext_header & 0x01FFu);
    request_chunk = (UINT8)((ext_header >> 10) & 1u);
    chunk_number = (UINT8)((ext_header >> 11) & 0x0Fu);
    chunked = (UINT8)((ext_header >> 15) & 1u);
    ndo = (UINT8)((PD_Rx_Buf[1] >> 4) & 0x07u);

    if(request_chunk) return;
    if(ndo == 0 || (UINT16)ndo * 4u < 2u) return;
    available = (UINT8)((UINT16)ndo * 4u - 2u);

    if(data_size == 0 || data_size > PD_EPR_CAP_BUFFER_SIZE)
    {
        PD_EPR_Exit_To_SPR("EPR Source_Capabilities too large");
        return;
    }

    if(!chunked)
    {
        if(data_size > available)
        {
            PD_EPR_Exit_To_SPR("unchunked EPR Source_Capabilities exceeds RX buffer");
            return;
        }
        memset(PD_EPR_SourcePDO_Raw, 0, sizeof(PD_EPR_SourcePDO_Raw));
        memcpy(PD_EPR_SourcePDO_Raw, &PD_Rx_Buf[4], data_size);
        PD_EPR_CapDataSize = data_size;
        PD_EPR_Capabilities_Complete();
        return;
    }

    if(chunk_number == 0)
    {
        memset(PD_EPR_SourcePDO_Raw, 0, sizeof(PD_EPR_SourcePDO_Raw));
        PD_EPR_CapDataSize = data_size;
        PD_EPR_LastChunk = 0;
    }
    else
    {
        if(PD_EPR_CapDataSize != data_size || chunk_number != (UINT8)(PD_EPR_LastChunk + 1u))
        {
            PD_EPR_Exit_To_SPR("unexpected EPR chunk sequence");
            return;
        }
        PD_EPR_LastChunk = chunk_number;
    }

    offset = (UINT16)chunk_number * PD_EXT_CHUNK_DATA_MAX;
    if(offset >= data_size || offset >= PD_EPR_CAP_BUFFER_SIZE)
    {
        PD_EPR_Exit_To_SPR("invalid EPR chunk offset");
        return;
    }

    remain = (UINT16)(data_size - offset);
    copy_len = (remain > PD_EXT_CHUNK_DATA_MAX) ? PD_EXT_CHUNK_DATA_MAX : (UINT8)remain;
    if(copy_len > available) copy_len = available;
    memcpy(&PD_EPR_SourcePDO_Raw[offset], &PD_Rx_Buf[4], copy_len);

    if((UINT16)(offset + copy_len) >= data_size)
    {
        PD_EPR_Capabilities_Complete();
    }
    else
    {
        UINT8 next_chunk = (UINT8)(chunk_number + 1u);
        if(PD_Send_Extended_Chunk_Request(PD_EXT_TYPE_EPR_SOURCE_CAP, next_chunk, PD_TXJOB_EPR_CHUNK) != DEF_PD_TX_OK)
            PD_EPR_Exit_To_SPR("EPR chunk request TX failed");
    }
}

static void PD_Handle_Extended_Message(UINT8 msg_type)
{
    if(msg_type == PD_EXT_TYPE_EPR_SOURCE_CAP)
    {
        PD_EPR_GetCapSent = 1;
        PD_EPR_TimerMs = 0;
        PD_Handle_EPR_Source_Capabilities();
    }
    else if(msg_type == PD_EXT_TYPE_EXTENDED_CONTROL)
    {
        UINT16 ext_header = (UINT16)PD_Rx_Buf[2] | ((UINT16)PD_Rx_Buf[3] << 8);
        UINT16 data_size = (UINT16)(ext_header & 0x01FFu);
        UINT8 request_chunk = (UINT8)((ext_header >> 10) & 1u);
        if(!request_chunk && data_size >= 2u)
        {
            UINT8 ctrl_type = PD_Rx_Buf[4];
            if(ctrl_type == PD_EXT_CTRL_EPR_KEEPALIVE_ACK)
            {
                PD_EPR_KeepAliveWaitAck = 0;
                PD_EPR_KeepAliveAckMs = 0;
            }
            else if(PD_RxWarnCount < 12u)
            {
                /* Any other Extended Control request (EPR_Get_Sink_Cap,
                 * Get_Status, ...) is not answered by this build.  It used to
                 * vanish without a trace - no reply and no log line - which
                 * reads exactly like "the Source stayed silent".  Surface it. */
                PD_RxWarnCount++;
                printf("[PD] RX ext ctrl type=%u not answered\r\n",
                       (unsigned)ctrl_type);
            }
        }
    }
    else if(PD_RxWarnCount < 12u)
    {
        /* Any other extended type is unhandled; surface it once so a reply we
         * cannot parse never looks like "the Source said nothing". */
        PD_RxWarnCount++;
        printf("[PD] RX unhandled extended type=0x%02X ndo=%u\r\n",
               (unsigned)msg_type, (unsigned)PD_RxLast_Ndo);
    }
}

static void PD_Handle_EPR_Mode_Message(void)
{
    UINT32 eprmdo = PD_ReadU32LE(&PD_Rx_Buf[2]);
    UINT8 action = (UINT8)(eprmdo >> 24);
    UINT8 data = (UINT8)(eprmdo >> 16);

    if(action == PD_EPR_MODE_ENTER_ACK)
    {
        if(PD_EPR_State == EPR_ST_WAIT_ENTER_ACK)
        {
            printf("[PD] EPR Mode: Enter Acknowledged\r\n");
            PD_EPR_State = EPR_ST_WAIT_ENTER_SUCCESS;
            PD_EPR_TimerMs = 0;
        }
        else
        {
            printf("[PD] EPR Mode: Enter_ACK in state %u (ignored)\r\n",
                   (unsigned)PD_EPR_State);
        }
    }
    else if(action == PD_EPR_MODE_ENTER_SUCCESS)
    {
        if(PD_EPR_State == EPR_ST_WAIT_ENTER_ACK ||
           PD_EPR_State == EPR_ST_WAIT_ENTER_SUCCESS)
        {
            printf("[PD] EPR Mode: Enter Succeeded\r\n");
            PD_EPR_ModeActive = 1;
            PD_EPR_State = EPR_ST_WAIT_SOURCE_CAP;
            PD_EPR_TimerMs = 0;
            PD_EPR_GetCapSent = 0;
            PD_EPR_GetCapTries = 0;
            PD_EPR_CapDataSize = 0;
            PD_EPR_LastChunk = 0;
            /* Start the KeepAlive duty from zero so the first one leaves 375 ms
             * after the Source confirmed the EPR entry. */
            PD_EPR_KeepAliveMs = 0;
            PD_EPR_KeepAliveWaitAck = 0;
            PD_EPR_KeepAliveAckMs = 0;

            /* DemoBoard/C140-proven ordering: do not start a new transmit from
             * inside the Enter_Succeeded receive pass.  The WAIT_SOURCE_CAP
             * state sends EPR_Get_Source_Cap after PD_EPR_GET_CAP_DELAY_MS. */
        }
        else
        {
            printf("[PD] EPR Mode: Enter_Success in state %u (retransmit?)\r\n",
                   (unsigned)PD_EPR_State);
        }
    }
    else if(action == PD_EPR_MODE_ENTER_FAILED)
    {
        const char *reason = "unknown";
        if(data == 1) reason = "cable not EPR capable";
        else if(data == 2) reason = "Source failed to become VCONN Source";
        else if(data == 3) reason = "EPR-capable bit missing in RDO";
        else if(data == 4) reason = "Source unable to enter EPR now";
        else if(data == 5) reason = "Source PDO not EPR capable";
        PD_EPR_Fallback(reason);
    }
    else if(action == PD_EPR_MODE_EXIT)
    {
        PD_EPR_Fallback("Source exited EPR Mode");
    }
    else
    {
        printf("[PD] EPR Mode: unhandled action=%u data=%u\r\n",
               (unsigned)action, (unsigned)data);
    }
}

/*********************************************************************
 * @fn      PD_Main_Proc
 *
 * @brief   This function uses to process PD status.
 *
 * @return  none
 */
static void PD_Main_Proc( )
{
    UINT8 status;
    UINT8 pd_header;
    UINT8 var;

    /* Hardware IRQs are terminated inside the BSP and surfaced as events. */
    if(PD_Port_HardResetPending())
    {
        PD_Port_PhyDiag hr;
        UINT8 was_epr = (PD_EPR_State != EPR_ST_OFF) ? 1u : 0u;

        PD_Port_GetPhyDiag(&hr);
        PD_Port_ClearHardResetEvent();
        PD_Port_ClearMessageEvent();

        /* IF_RX_RESET + PD_RX_SOP1_HRST (ST=2) + CNT=0 is a Hard Reset
         * ordered set.  VBUS can still be near 20 V at the instant the CC
         * reset is decoded; the Source removes/restarts VBUS afterwards. */
        printf("[PD] Source Hard Reset: ST=%u CNT=%u VBUS=%u mV EPRst=%u\r\n",
               (unsigned)hr.hr_pd_stat, (unsigned)hr.hr_byte_count,
               (unsigned)s_pd_vbus_mv, (unsigned)PD_EPR_State);

        /* If the Source aborted an EPR attempt, do not immediately re-enter
         * EPR during the same physical attachment.  This prevents the
         * SPR->EPR->HardReset power-flap loop; a real unplug/replug clears it. */
        if(was_epr)
            PD_EPR_FailedForAttach = 1u;

        PD_PHY_Reset();
        PD_Rx_Mode();
        return;
    }

    /* Asynchronous PHY engine: enforce the TX/GoodCRC deadlines, then dispatch
     * any completed (or failed) transmission before the policy timers run. */
    PD_Port_Service();
    PD_TxCompletion_Proc();

    PD_Ctl.PD_BusIdle_Timer += Tmr_Ms_Dlt;

    /* Track (saturating) how long the input sits below the detach threshold.
     * The value is consumed at the next attach: only a re-plug that kept VBUS
     * away for PD_EPR_REATTACH_VBUS_OFF_MS may try EPR again. */
    if(s_pd_vbus_valid && (s_pd_vbus_mv < PD_VBUS_DETACH_THRESHOLD_MV))
    {
        if(s_pd_vbus_low_ms < 60000u)
            s_pd_vbus_low_ms = (UINT16)(s_pd_vbus_low_ms + Tmr_Ms_Dlt);
    }

    if(PD_EPR_State != EPR_ST_OFF)
        PD_EPR_TimerMs = (UINT16)(PD_EPR_TimerMs + Tmr_Ms_Dlt);

    /* KeepAlive maintains an *existing* EPR contract, so it is gated on the
     * contract state (EPR_ST_ACTIVE) - the same condition the WCH reference
     * uses.  Gating it on PD_EPR_ModeActive made it start at Enter_Succeeded,
     * i.e. while the EPR_Request was still awaiting Accept/PS_RDY: the field
     * log shows the Source then staying silent until our own 506 ms response
     * timeout tore the session down. */
    if(PD_EPR_State == EPR_ST_ACTIVE)
    {
        PD_EPR_KeepAliveMs = (UINT16)(PD_EPR_KeepAliveMs + Tmr_Ms_Dlt);
        if(PD_EPR_KeepAliveWaitAck)
            PD_EPR_KeepAliveAckMs = (UINT16)(PD_EPR_KeepAliveAckMs + Tmr_Ms_Dlt);

        if(PD_EPR_KeepAliveWaitAck && PD_EPR_KeepAliveAckMs > PD_EPR_KEEPALIVE_ACK_TIMEOUT_MS)
        {
            printf("[PD] EPR KeepAlive ACK timeout; retrying\r\n");
            PD_EPR_KeepAliveWaitAck = 0;
            PD_EPR_KeepAliveAckMs = 0;
        }

        if(!PD_EPR_KeepAliveWaitAck && PD_EPR_KeepAliveMs >= PD_EPR_KEEPALIVE_PERIOD_MS)
        {
            if(PD_Send_Extended_Control(PD_EXT_CTRL_EPR_KEEPALIVE, PD_TXJOB_KEEPALIVE) == DEF_PD_TX_OK)
            {
                if(PD_EPR_State == EPR_ST_WAIT_SOURCE_CAP)
                    printf("[PD] EPR KeepAlive sent while waiting for EPR caps\r\n");
                PD_EPR_KeepAliveWaitAck = 1;
                PD_EPR_KeepAliveMs = 0;
                PD_EPR_KeepAliveAckMs = 0;
            }
        }
    }
    else if((PD_EPR_State == EPR_ST_WAIT_ENTER_ACK ||
             PD_EPR_State == EPR_ST_WAIT_ENTER_SUCCESS) &&
            PD_EPR_TimerMs > PD_EPR_ENTER_TIMEOUT_MS)
    {
        PD_EPR_Fallback("EPR Mode entry timeout");
    }

    /* Deliberately a separate test, not an "else" of the branch above:
     * EPR_ModeActive is already set when the Source confirms the entry, so an
     * else-chain would make this state - and the EPR_Get_Source_Capabilities
     * that has to leave from it - unreachable. */
    if(PD_EPR_State == EPR_ST_WAIT_SOURCE_CAP)
    {
        /* The first Get leaves right after Enter_Succeeded (tries == 0 unless
         * the message handler already sent it).  Later tries are spaced out so
         * a Source that is briefly busy still gets caught once it listens
         * again; RX stays armed between tries, so caps sent on the Source's
         * own initiative are consumed too. */
        if(!PD_EPR_GetCapSent &&
           ((PD_EPR_GetCapTries == 0u)
                ? (PD_EPR_TimerMs > PD_EPR_GET_CAP_DELAY_MS)
                : (PD_EPR_TimerMs > PD_EPR_GET_RETRY_GAP_MS)))
        {
            PD_EPR_GetCapSent = 1;
            PD_EPR_GetCapTries++;
            if(PD_Send_Extended_Control(PD_EXT_CTRL_EPR_GET_SOURCE_CAP, PD_TXJOB_EPR_GET_CAP) != DEF_PD_TX_OK)
            {
                PD_EPR_GetCapSent = 0;
                PD_EPR_TimerMs = 0;
                if(PD_EPR_GetCapTries >= PD_EPR_GET_MAX_TRIES)
                    PD_EPR_Exit_To_SPR("EPR_Get_Source_Cap TX failed");
            }
            /* "sent" print + timer reset happen in PD_TxCompletion_Proc()
             * when the Source's GoodCRC has arrived. */
        }
        else if(PD_EPR_GetCapSent && PD_EPR_TimerMs > PD_EPR_SOURCE_CAP_TIMEOUT_MS)
        {
            /* Site dump: did the Source stay silent, or send a type we never
             * dispatch (that would have looked like silence before)? */
            printf("[PD] EPR caps timeout: RX total=%u last ext=%u type=0x%02X ndo=%u\r\n",
                   (unsigned)PD_RxCount, (unsigned)PD_RxLast_Ext,
                   (unsigned)PD_RxLast_Type, (unsigned)PD_RxLast_Ndo);
            /* ACKed but no caps yet: another Get within the try budget instead
             * of leaving EPR on the first deadline. */
            PD_EPR_GetCapSent = 0;
            PD_EPR_TimerMs = 0;
            if(PD_EPR_GetCapTries >= PD_EPR_GET_MAX_TRIES)
                PD_EPR_Exit_To_SPR("EPR_Source_Capabilities timeout");
            else
                printf("[PD] EPR caps missing; retrying Get (%u/%u)\r\n",
                       (unsigned)PD_EPR_GetCapTries, (unsigned)PD_EPR_GET_MAX_TRIES);
        }
    }

    switch( PD_Ctl.PD_State )
    {
        case STA_DISCONNECT:
            printf("Disconnect\r\n");
            PD_PHY_Reset( );
            break;

        case STA_SRC_CONNECT:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;

            /* A Source normally sends Source_Capabilities after Attach.  If
             * our PHY joined late (for example after holding RST), explicitly
             * ask for them.  Previous code only tried once, then called
             * PD_PHY_Reset() without clearing Connected, which could leave the
             * state machine permanently stuck in STA_IDLE. */
            if(PD_Ctl.PD_Comm_Timer >= PD_GET_SOURCE_CAP_RETRY_MS)
            {
                if(s_pd_get_src_cap_retries < PD_GET_SOURCE_CAP_MAX_RETRIES)
                {
                    s_pd_get_src_cap_retries++;
                    printf("[PD] no Source_Capabilities; Get_Source_Cap retry %u/%u\r\n",
                           (unsigned)s_pd_get_src_cap_retries,
                           (unsigned)PD_GET_SOURCE_CAP_MAX_RETRIES);
                    PD_Load_Header(0x00, DEF_TYPE_GET_SRC_CAP);
                    status = PD_Send_Handle(NULL, 0, PD_TXJOB_GET_SRC_CAP);
                    PD_Ctl.PD_Comm_Timer = 0;

                    if(status != DEF_PD_TX_OK)
                    {
                        /* Do not call PD_PHY_Reset() here.  On this product the
                         * 5.1k Rd is permanently wired, so a local PHY reset is
                         * NOT a physical Type-C detach.  PD_PHY_Reset() also
                         * clears s_pd_get_src_cap_retries, which previously
                         * caused an endless "retry 1/3 -> CC1 SRC Connect" loop. */
                        printf("[PD] Get_Source_Cap TX failed; keeping RX armed for retry %u/%u\r\n",
                               (unsigned)s_pd_get_src_cap_retries,
                               (unsigned)PD_GET_SOURCE_CAP_MAX_RETRIES);
                        PD_GetSrcCap_Sent = 0u;
                        PD_Rx_Mode();
                    }
                    /* PD_GetSrcCap_Sent + the "sent" print are produced by
                     * PD_TxCompletion_Proc() once the GoodCRC arrived. */
                }
                else
                {
                    /* Remain electrically attached and listen passively.  A
                     * later Source_Capabilities packet is still consumed from
                     * STA_IDLE; unplugging VBUS performs the real detach. */
                    printf("[PD] Source_Capabilities unavailable after %u retries; staying attached at 5V and listening\r\n",
                           (unsigned)PD_GET_SOURCE_CAP_MAX_RETRIES);
                    PD_Ctl.PD_State = STA_IDLE;
                    PD_Ctl.PD_Comm_Timer = 0u;
                    PD_GetSrcCap_Sent = 0u;
                    PD_Rx_Mode();
                }
            }
            break;

        case STA_RX_ACCEPT_WAIT:
        case STA_RX_PS_RDY_WAIT:
            PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
            /* EPR requests are answered far later than an SPR handshake: the
             * Source ramps 20 V -> 28 V before it sends PS_RDY, and a slow
             * charger can stretch Accept -> PS_RDY well past the SPR window.
             * The reference Sink on this PHY does not time these EPR waits out
             * at all - it keeps waiting for the messages.  Keep the half-second
             * fuse for SPR, but give an in-flight EPR exchange seconds so a
             * late-but-valid PS_RDY is accepted instead of triggering the
             * Soft Reset cascade. */
            if(PD_Ctl.PD_Comm_Timer >
               (((PD_EPR_State == EPR_ST_WAIT_REQUEST_ACCEPT) ||
                 (PD_EPR_State == EPR_ST_WAIT_REQUEST_PSRDY)) ? 3000u : 499u))
            {
                printf("[PD] response timeout in state %u after %u ms (RX=%u last=0x%02X); trying Soft Reset\r\n",
                       (unsigned)PD_Ctl.PD_State, (unsigned)PD_Ctl.PD_Comm_Timer,
                       (unsigned)PD_RxCount, (unsigned)PD_RxLast_Type);
                PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
                PD_SoftResetRecoveryPending = 1u;
                PD_Ctl.PD_State = STA_TX_SOFTRST;
                PD_Ctl.PD_Comm_Timer = 0;
            }
            break;

        case STA_RX_PS_RDY:
            PD_Ctl.PD_State = STA_IDLE;
            break;

        case STA_TX_SOFTRST:
            PD_Load_Header(0x00, DEF_TYPE_SOFT_RESET);
            status = PD_Send_Handle(NULL, 0, PD_TXJOB_SOFT_RESET);
            PD_Ctl.PD_Comm_Timer = 0;
            if(status == DEF_PD_TX_OK)
            {
                PD_SoftResetRecoveryPending = 1u;
                PD_Ctl.PD_State = STA_RX_ACCEPT_WAIT;
                printf("[PD] Soft Reset sent; waiting for Accept\r\n");
            }
            else
            {
#if PD_VBUS_POWERED_SUPPRESS_HARD_RESET
                PD_SoftResetRecoveryPending = 0u;
                PD_LocalProtocolRecover("Soft Reset TX failed");
#else
                PD_Ctl.PD_State = STA_TX_HRST;
#endif
            }
            break;

        case STA_TX_HRST:
#if PD_VBUS_POWERED_SUPPRESS_HARD_RESET
            PD_SoftResetRecoveryPending = 0u;
            PD_LocalProtocolRecover("Hard Reset requested by policy engine");
#else
            PD_Ctl.Flag.Bit.Stop_Det_Chk = 1;
            PD_SPR_ContractActive = 0;
            PD_EPR_ContractActive = 0;
            PD_TxJob = PD_TXJOB_REPLY;
            PD_Port_SendHardReset();
            PD_Rx_Mode();
            PD_Ctl.PD_State = STA_IDLE;
            PD_Ctl.PD_Comm_Timer = 0;
            printf("[PD] Hard Reset sent; previous contract invalid\r\n");
            PD_EPR_Fallback(NULL);
#endif
            break;

        default:
            break;
    }

    if(PD_Port_MessagePending())
    {
        UINT8 is_extended;

        /* Consume the current IRQ event. If another packet arrives while a
         * response helper has RX enabled, the BSP raises a fresh event. */
        PD_Port_ClearMessageEvent();
        PD_Ctl.Adapter_Idle_Cnt = 0;
        pd_header = PD_Rx_Buf[0] & 0x1F;
        is_extended = (PD_Rx_Buf[1] & 0x80u) ? 1u : 0u;

        PD_RxLast_Type = pd_header;
        PD_RxLast_Ext = is_extended;
        PD_RxLast_Ndo = (UINT8)((PD_Rx_Buf[1] >> 4) & 0x07u);
        PD_RxCount++;

        /* Track the partner revision from every received SOP message.  EPR
         * still uses the Rev3.x Message Header encoding (10b). */
        if(((PD_Rx_Buf[0] >> 6) & 0x03u) >= DEF_PD_REVISION_30)
            PD_Ctl.Flag.Bit.PD_Version = 1;

        if(is_extended)
        {
            PD_Handle_Extended_Message(pd_header);
        }
        else
        {
            switch(pd_header)
            {
                case DEF_TYPE_SRC_CAP:
                {
                    UINT32 pdo1;
                    PD_GetSrcCap_Sent = 1;
                    s_pd_get_src_cap_retries = 0u;
                    Delay_Ms(5);
                    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
                    PD_Save_Adapter_SrcCap();

                    pdo1 = (PD_SourcePDO_Count > 0) ? PD_ReadU32LE(&PD_SourcePDO_Raw[0]) : 0;
                    PD_Source_EPR_Capable =
                        (PD_Ctl.Flag.Bit.PD_Version && (pdo1 & (1UL << 23))) ? 1u : 0u;

                    var = PD_Select_Highest_Fixed_PDO();
                    if(var == 0)
                    {
                        var = PDO_INDEX_1;
                        PD_Selected_PDO = var;
                    }

#if PD_EPR_ENABLE
                    if(PD_Source_EPR_Capable && !PD_EPR_FailedForAttach)
                    {
                        PD_EPR_State = EPR_ST_SPR_NEGOTIATING;
                        PD_EPR_TimerMs = 0;
                    }
#endif

                    /* Request first; diagnostics come afterwards so they cannot
                     * consume the Source's tSenderResponse budget. */
                    PDO_Request(var);

                    printf("\r\n");
                    PD_Print_Source_PDOs();
#if PD_EPR_ENABLE
                    if(PD_Source_EPR_Capable && !PD_EPR_FailedForAttach)
                        printf("[PD] Source advertises EPR capability; establish SPR contract first\r\n");
                    else if(!PD_Source_EPR_Capable)
                        printf("[PD] Source is not EPR capable; staying in SPR\r\n");
                    else
                        printf("[PD] EPR already failed for this attachment; staying in SPR\r\n");
#endif
                    break;
                }

                case DEF_TYPE_ACCEPT:
                    if(PD_Ctl.PD_State != STA_RX_ACCEPT_WAIT)
                    {
                        printf("[PD] ignored unexpected Accept in state %u\r\n",
                               (unsigned)PD_Ctl.PD_State);
                        break;
                    }

                    if(PD_SoftResetRecoveryPending)
                    {
                        /* Soft Reset terminates the old protocol sequence.  After
                         * Accept both sides restart their message-ID counters; ask
                         * for Source_Capabilities again without dropping VBUS. */
                        PD_SoftResetRecoveryPending = 0u;
                        PD_Ctl.Msg_ID = 0u;
                        PD_GetSrcCap_Sent = 0u;
                        PD_Ctl.PD_State = STA_SRC_CONNECT;
                        PD_Ctl.PD_Comm_Timer = 701u;
                        PD_EPR_State = EPR_ST_OFF;
                        PD_EPR_ModeActive = 0u;
                        PD_EPR_ContractActive = 0u;
                        PD_SPR_ContractActive = 0u;
                        printf("[PD] Soft Reset accepted; requesting Source_Capabilities again\r\n");
                    }
                    else
                    {
                        if(PD_EPR_State == EPR_ST_WAIT_REQUEST_ACCEPT)
                        {
                            PD_EPR_State = EPR_ST_WAIT_REQUEST_PSRDY;
                            /* Make the Accept visible: the PS_RDY wait that
                             * follows is the long one, and without this line a
                             * stalled log cannot tell "Accept never arrived"
                             * apart from "Accept consumed, PS_RDY missing". */
                            printf("[PD] EPR Accept received; waiting for PS_RDY\r\n");
                        }
                        PD_Ctl.PD_State = STA_RX_PS_RDY_WAIT;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                    break;

                case DEF_TYPE_REJECT:
                    if(PD_Ctl.PD_State != STA_RX_ACCEPT_WAIT)
                    {
                        printf("[PD] ignored unexpected Reject in state %u\r\n",
                               (unsigned)PD_Ctl.PD_State);
                        break;
                    }

                    if(PD_SoftResetRecoveryPending)
                    {
                        PD_SoftResetRecoveryPending = 0u;
                        PD_LocalProtocolRecover("Soft Reset rejected by Source");
                    }
                    else if(PD_EPR_State == EPR_ST_WAIT_REQUEST_ACCEPT)
                    {
                        PD_EPR_Exit_To_SPR("28V EPR_Request rejected");
                    }
                    else
                    {
                        PD_Ctl.PD_State = STA_IDLE;
                    }
                    break;

                case DEF_TYPE_PS_RDY:
                    /* PS_RDY is meaningful only after an Accept for our current
                     * Request.  Never infer a power contract merely because a
                     * PS_RDY-shaped packet exists in the RX buffer. */
                    if(PD_Ctl.PD_State != STA_RX_PS_RDY_WAIT)
                    {
                        printf("[PD] ignored unexpected PS_RDY in state %u\r\n",
                               (unsigned)PD_Ctl.PD_State);
                        break;
                    }

                    if((PD_Selected_PDO == 0u) || (PD_Selected_mV < 4500u))
                    {
                        printf("[PD] invalid PS_RDY without a selected PDO; re-arming protocol\r\n");
                        PD_LocalProtocolRecover("PS_RDY without valid Request context");
                        break;
                    }

                    PD_ProtocolRecoveryCount = 0u;
                    if(PD_EPR_State == EPR_ST_SPR_NEGOTIATING)
                    {
                        PD_SPR_ContractActive = 1;
                        printf("[PD] SPR contract ready: PDO%u, %u mV / %u mA\r\n",
                               PD_Selected_PDO, PD_Selected_mV, PD_Selected_mA);
                        printf("[PD] EPR Mode: sending Enter, Sink PDP=%u W\r\n",
                               (unsigned)PD_EPR_SINK_PDP_W);
                        PD_EPR_State = EPR_ST_WAIT_ENTER_ACK;
                        PD_EPR_TimerMs = 0;
                        PD_Ctl.PD_State = STA_IDLE;
                        if(PD_Send_EPR_Mode(PD_EPR_MODE_ENTER, (UINT8)PD_EPR_SINK_PDP_W, PD_TXJOB_EPR_MODE) != DEF_PD_TX_OK)
                            PD_EPR_Fallback("EPR Mode Enter TX failed");
                    }
                    else if(PD_EPR_State == EPR_ST_WAIT_REQUEST_PSRDY)
                    {
                        if(PD_Selected_mV <= 20000u)
                        {
                            /* This PS_RDY answers the SPR request that followed
                             * a recovery, not the EPR request.  Consuming it as
                             * the EPR contract left the port "EPR active" at
                             * 20 V with KeepAlive running - exactly the
                             * "stuck at 60 W" state seen in the field. */
                            printf("[PD] EPR PS_RDY without a 28 V request; leaving EPR\r\n");
                            PD_EPR_Exit_To_SPR(NULL);
                            break;
                        }
                        PD_SPR_ContractActive = 1;
                        PD_EPR_ContractActive = 1;
                        PD_EPR_ModeActive = 1;
                        PD_EPR_State = EPR_ST_ACTIVE;
                        PD_EPR_TimerMs = 0;
                        PD_EPR_KeepAliveMs = 0;
                        PD_EPR_KeepAliveWaitAck = 0;
                        PD_Ctl.PD_State = STA_IDLE;
                        printf("[PD] EPR contract ready: PDO%u, %u mV / %u mA\r\n",
                               PD_Selected_PDO, PD_Selected_mV, PD_Selected_mA);
                        printf("[PD] EPR KeepAlive period=%u ms\r\n",
                               (unsigned)PD_EPR_KEEPALIVE_PERIOD_MS);
                    }
                    else
                    {
                        PD_SPR_ContractActive = 1;
                        printf("[PD] Contract ready: PDO%u, target %u mV / %u mA\r\n",
                               PD_Selected_PDO, PD_Selected_mV, PD_Selected_mA);
                        PD_Ctl.PD_State = STA_RX_PS_RDY;
                    }
                    break;

                case DEF_TYPE_WAIT:
                    if(PD_Ctl.PD_State != STA_RX_ACCEPT_WAIT)
                    {
                        printf("[PD] ignored unexpected WAIT in state %u\r\n",
                               (unsigned)PD_Ctl.PD_State);
                        break;
                    }

                    if(PD_SoftResetRecoveryPending)
                    {
                        PD_SoftResetRecoveryPending = 0u;
                        PD_LocalProtocolRecover("WAIT received during Soft Reset recovery");
                    }
                    else if(PD_EPR_State == EPR_ST_WAIT_REQUEST_ACCEPT)
                    {
                        printf("[PD] WAIT to EPR_Request is a protocol error; Hard Reset\r\n");
                        PD_Ctl.PD_State = STA_TX_HRST;
                    }
                    else
                    {
                        /* For the normal SPR Request, drop the pending transaction
                         * and wait for a fresh Source_Capabilities/renegotiation. */
                        PD_Ctl.PD_State = STA_SRC_CONNECT;
                        PD_Ctl.PD_Comm_Timer = 701u;
                    }
                    break;

                case DEF_TYPE_EPR_MODE:
                    PD_Handle_EPR_Mode_Message();
                    break;

                case DEF_TYPE_NOT_SUPPORT:
                    printf("[PD] RX Not_Supported (state=%u epr=%u)\r\n",
                           (unsigned)PD_Ctl.PD_State, (unsigned)PD_EPR_State);
                    if(PD_EPR_State == EPR_ST_WAIT_ENTER_ACK ||
                       PD_EPR_State == EPR_ST_WAIT_ENTER_SUCCESS)
                        PD_EPR_Fallback("EPR Mode not supported by Source");
                    break;

                case DEF_TYPE_GET_SNK_CAP:
                    PD_Load_Header(0x00, DEF_TYPE_SNK_CAP);
                    PD_Send_Handle(SinkCap_5V1A_Tab, sizeof(SinkCap_5V1A_Tab), PD_TXJOB_REPLY);
                    break;

                case DEF_TYPE_SOFT_RESET:
                    PD_Load_Header(0x00, DEF_TYPE_ACCEPT);
                    PD_Send_Handle(NULL, 0, PD_TXJOB_REPLY);
                    break;

                case DEF_TYPE_GET_SRC_CAP_EX:
                    PD_Load_Header(0x01, DEF_TYPE_SRC_CAP);
                    PD_Send_Handle(SrcCap_Ext_Tab, sizeof(SrcCap_Ext_Tab), PD_TXJOB_REPLY);
                    break;

                case DEF_TYPE_GET_STATUS:
                    PD_Load_Header(0x01, DEF_TYPE_GET_STATUS_R);
                    PD_Send_Handle(Status_Ext_Tab, sizeof(Status_Ext_Tab), PD_TXJOB_REPLY);
                    break;

                case DEF_TYPE_VCONN_SWAP:
                    PD_Load_Header(0x00, DEF_TYPE_REJECT);
                    PD_Send_Handle(NULL, 0, PD_TXJOB_REPLY);
                    break;

                case DEF_TYPE_VENDOR_DEFINED:
                    if(PD_VDM_LogCount < 8u)
                    {
                        PD_VDM_LogCount++;
                        printf("[PD] RX VDM: %02X %02X %02X %02X cmd=%u\r\n",
                               (unsigned)PD_Rx_Buf[2], (unsigned)PD_Rx_Buf[3],
                               (unsigned)PD_Rx_Buf[4], (unsigned)PD_Rx_Buf[5],
                               (unsigned)(PD_Rx_Buf[2] & 0x1Fu));
                    }

                    /* Structured VDM request from the Source: byte2 & 0xC0 == 0
                     * means Command Type = REQ (SVID 0xFF00 sits in bytes 4/5).
                     *
                     * The answer now mirrors the DemoBoard build that is
                     * field-verified to reach 28 V on this charger: echo the
                     * received VDM header back as a short 4-byte NAK (Command
                     * Type 10b) and nothing else.  The crafted 16-byte
                     * Discover-Identity ACK this case used to send is the prime
                     * remaining byte-level deviation from the proven partner
                     * behaviour; the charger does not need a *valid* identity
                     * (the verified build never supplies one), so the minimal
                     * trivially-well-formed answer is the safer move. */
                    if((PD_Rx_Buf[2] & 0xC0) == 0)
                    {
                        UINT8 cmd = (UINT8)(PD_Rx_Buf[2] & 0x1Fu);

                        Delay_Ms(1);
                        PD_Load_Header(0x00, DEF_TYPE_VENDOR_DEFINED);
                        if((PD_Rx_Buf[3] & 0x60) == 0) PD_Ctl.Flag.Bit.VDM_Version = 0;
                        else PD_Ctl.Flag.Bit.VDM_Version = 1;

                        /* Exact frame the verified build sends: the request's
                         * own header bytes with Command Type forced to NAK. */
                        PD_Rx_Buf[2] |= 0x80u;

                        if(PD_VDM_LogCount < 8u)
                            printf("[PD] VDM cmd=%u -> 4-byte NAK\r\n", (unsigned)cmd);

                        PD_Send_Handle(&PD_Rx_Buf[2], 4, PD_TXJOB_REPLY);
                    }
                    else if(PD_VDM_LogCount < 8u)
                    {
                        printf("[PD] VDM: no reply sent (command type not a request)\r\n");
                    }
                    break;

                default:
                    if(PD_RxWarnCount < 12u)
                    {
                        PD_RxWarnCount++;
                        printf("[PD] RX unhandled type=0x%02X ndo=%u\r\n",
                               (unsigned)pd_header, (unsigned)PD_RxLast_Ndo);
                    }
                    break;
            }
        }

        /* Re-arm RX only when there is neither a completed packet waiting nor
         * an automatic GoodCRC still on the wire.  The latter is the narrow
         * race that produced diagnostics such as started/completed=6/5. */
        if(!PD_Port_MessagePending() && !PD_Port_TxBusy())
            PD_Rx_Mode();
        PD_Ctl.PD_BusIdle_Timer = 0;
    }
}



void PD_Task(uint32_t now_ms)
{
    uint32_t elapsed;

    if(!s_pd_task_started)
    {
        s_pd_task_started = 1u;
        s_pd_task_last_ms = now_ms;
        s_pd_detect_last_ms = now_ms;
        Tmr_Ms_Dlt = 0u;
    }
    else
    {
        elapsed = (uint32_t)(now_ms - s_pd_task_last_ms);
        if(elapsed > 0xFFFFu) elapsed = 0xFFFFu;
        Tmr_Ms_Dlt = (UINT16)elapsed;
        s_pd_task_last_ms = now_ms;
    }

    if((uint32_t)(now_ms - s_pd_detect_last_ms) >= 5u)
    {
        s_pd_detect_last_ms = now_ms;
        PD_Det_Proc();
    }

    PD_Main_Proc();
}

void PD_SetVbusMillivolts(uint16_t mv)
{
    s_pd_vbus_mv = mv;
    s_pd_vbus_valid = 1u;
}

uint8_t PD_IsConnected(void)
{
    return PD_Ctl.Flag.Bit.Connected ? 1u : 0u;
}

uint8_t PD_IsEPRContractActive(void)
{
    return PD_EPR_ContractActive ? 1u : 0u;
}

uint8_t PD_IsPowerReady(void)
{
    if(!PD_SPR_ContractActive)
        return 0u;

    /* Safety backstop: a real contract can never be PDO0 / 0V. */
    if((PD_Selected_PDO == 0u) || (PD_Selected_mV < 4500u))
        return 0u;

#if PD_EPR_ENABLE
    /* Keep nonessential loads off while an EPR-capable source is moving from
     * the stable SPR contract into the final EPR contract.  If EPR entry
     * fails, PD_EPR_FailedForAttach makes the existing SPR contract usable. */
    if(PD_Source_EPR_Capable && !PD_EPR_FailedForAttach)
        return PD_EPR_ContractActive ? 1u : 0u;
#endif

    return 1u;
}

uint16_t PD_GetContractVoltageMv(void)
{
    return PD_Selected_mV;
}

uint16_t PD_GetContractCurrentMa(void)
{
    return PD_Selected_mA;
}

uint8_t PD_WantsFastPoll(void)
{
    /* The Sink response latency (tSenderResponse, 24..30 ms) is produced by this
     * policy state machine, and that state machine only advances from the
     * cooperative main loop.  Sleeping 1 ms per pass (the WFI idle hook)
     * stretched the measured Source_Capabilities -> Request latency from ~5 ms
     * to ~31 ms, which the Source answers with a missing GoodCRC plus a retry
     * storm.  So the loop must stay at full speed while a PD source is attached;
     * WFI is only safe when nothing is negotiated and no GoodCRC is on the
     * wire. */
    if(PD_IsConnected() || PD_Port_TxBusy())
        return 1u;

    return PD_Port_AutoAckBusy();
}

static UINT8 PD_DecodeDisplayPDO(UINT32 raw,
                                 UINT8 index,
                                 UINT8 from_epr_table,
                                 PD_DisplayPDO *pdo)
{
    UINT8 supply_type;

    if((raw == 0u) || (pdo == NULL))
        return 0u;

    memset(pdo, 0, sizeof(*pdo));
    pdo->index = index;
    supply_type = (UINT8)(raw >> 30);

    if(supply_type == 0u)
    {
        pdo->max_mv = (UINT16)(((raw >> 10) & 0x3FFu) * 50u);
        pdo->min_mv = pdo->max_mv;
        pdo->current_ma = (UINT16)((raw & 0x3FFu) * 10u);
        pdo->type = (from_epr_table && index >= 8u) ?
                    PD_DISPLAY_PDO_EPR : PD_DISPLAY_PDO_SPR;
        return (pdo->max_mv != 0u) ? 1u : 0u;
    }

    if(supply_type == 3u)
    {
        UINT8 apdo_type = (UINT8)((raw >> 28) & 0x03u);

        if(apdo_type == 0u)
        {
            pdo->type = PD_DISPLAY_PDO_PPS;
            pdo->min_mv = (UINT16)(((raw >> 8) & 0xFFu) * 100u);
            pdo->max_mv = (UINT16)(((raw >> 17) & 0xFFu) * 100u);
            pdo->current_ma = (UINT16)((raw & 0x7Fu) * 50u);
            return (pdo->max_mv != 0u) ? 1u : 0u;
        }

        if(apdo_type == 1u)
        {
            pdo->type = PD_DISPLAY_PDO_AVS;
            pdo->min_mv = (UINT16)(((raw >> 8) & 0xFFu) * 100u);
            pdo->max_mv = (UINT16)(((raw >> 17) & 0x1FFu) * 100u);
            pdo->power_w = (UINT16)(raw & 0xFFu);
            return (pdo->max_mv != 0u) ? 1u : 0u;
        }
    }

    /* Battery, Variable and unsupported APDO subtypes are not useful in the
     * compact 5-slot front-panel list. */
    return 0u;
}

static UINT8 PD_DisplayPDOComesBefore(const PD_DisplayPDO *a,
                                      const PD_DisplayPDO *b)
{
    if(a->type != b->type)
        return (a->type > b->type) ? 1u : 0u;
    if(a->max_mv != b->max_mv)
        return (a->max_mv > b->max_mv) ? 1u : 0u;
    return (a->index < b->index) ? 1u : 0u;
}

uint8_t PD_GetDisplayPDOs(PD_DisplayPDO *out, uint8_t max_count)
{
    UINT8 i;
    UINT8 source_count;
    UINT8 from_epr_table;
    UINT8 count = 0u;
    PD_DisplayPDO candidate;

    if((out == NULL) || (max_count == 0u))
        return 0u;

    /* A detached port owns no visible Source capabilities. Counts are reset by
     * PD_PHY_Reset(), but keep this public API defensive against stale RAM/UI. */
    if(!PD_Ctl.Flag.Bit.Connected)
        return 0u;

    /* EPR Source_Capabilities contains the complete PDO slot table. Before
     * that message is available, fall back to ordinary Source_Capabilities.
     * Only use the EPR table while EPR Mode is actually active, so a failed or
     * ended EPR attempt cannot leave a previous EPR table on the display. */
    from_epr_table = (PD_EPR_ModeActive && (PD_EPR_SourcePDO_Count != 0u)) ? 1u : 0u;
    source_count = from_epr_table ? PD_EPR_SourcePDO_Count : PD_SourcePDO_Count;

    for(i = 1u; i <= source_count; ++i)
    {
        UINT32 raw;
        UINT8 pos;
        UINT8 j;

        if(from_epr_table)
            raw = PD_ReadU32LE(&PD_EPR_SourcePDO_Raw[(i - 1u) << 2]);
        else
            raw = PD_ReadU32LE(&PD_SourcePDO_Raw[(i - 1u) << 2]);

        if(!PD_DecodeDisplayPDO(raw, i, from_epr_table, &candidate))
            continue;

        pos = 0u;
        while(pos < count && !PD_DisplayPDOComesBefore(&candidate, &out[pos]))
            pos++;

        if(pos >= max_count)
            continue;

        if(count < max_count)
            count++;

        j = count;
        while(j > (UINT8)(pos + 1u))
        {
            out[j - 1u] = out[j - 2u];
            j--;
        }
        out[pos] = candidate;
    }

    return count;
}
