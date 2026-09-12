/* SW3526 operation library
 * TKWTL Desktop PD Power Converter
 * Register map basis: iSW RG012_1 v1.0
 */
#ifndef __SW3526_H__
#define __SW3526_H__

#include <stdint.h>
#include "coroOS.h"
#include "soft_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SW3526_I2C_ADDR                 0x3CU

/* Register naming follows the SW6306 library convention:
 *   STRG = status/readback register
 *   CTRG = control/configuration register
 */
#define SW3526_STRG_REV                 0x01U
#define SW3526_STRG_BUCK_VOUTH          0x03U
#define SW3526_STRG_BUCK_VOUTL          0x04U
#define SW3526_STRG_BUCK_ILIM           0x05U
#define SW3526_STRG_PROTOCOL            0x06U
#define SW3526_STRG_SYS_STAT            0x07U
#define SW3526_STRG_FAULT               0x0BU
#define SW3526_CTRG_WREN                0x12U
#define SW3526_CTRG_FORCE_OFF           0x13U
#define SW3526_STRG_ADC_VIN             0x30U
#define SW3526_STRG_ADC_VOUT            0x31U
#define SW3526_STRG_ADC_IOUT            0x33U
#define SW3526_CTRG_ADC_SET             0x3AU
#define SW3526_STRG_ADCH                0x3BU
#define SW3526_STRG_ADCL                0x3CU
#define SW3526_STRG_POWER               0x68U

/* 0x06 protocol indication. */
#define SW3526_STRG_PROTOCOL_ONLINE          0x80U
#define SW3526_STRG_PROTOCOL_HIGH_VOLTAGE    0x40U
#define SW3526_STRG_PROTOCOL_PD_VER_MSK      0x30U
#define SW3526_STRG_PROTOCOL_PD_VER_SHIFT    4U
#define SW3526_STRG_PROTOCOL_TYPE_MSK        0x0FU

/* 0x07 system status. */
#define SW3526_STRG_SYS_STAT_PORT_ON         0x02U
#define SW3526_STRG_SYS_STAT_BUCK_ON         0x01U

/* 0x0B abnormal case. */
#define SW3526_STRG_FAULT_VIN_OVP            0x10U
#define SW3526_STRG_FAULT_OTP_ALARM          0x04U
#define SW3526_STRG_FAULT_OTP_SHUTDOWN       0x02U
#define SW3526_STRG_FAULT_SHORT              0x01U

/* 0x3A ADC data type. Full 12-bit data appears at 0x3B/0x3C. */
#define SW3526_ADC_SET_VIN              0x01U  /* 10 mV/bit */
#define SW3526_ADC_SET_VOUT             0x02U  /* 6 mV/bit */
#define SW3526_ADC_SET_IOUT             0x03U  /* 2.5 mA/bit */

#define SW3526_WREN_STEP1               0x20U
#define SW3526_WREN_STEP2               0x40U
#define SW3526_WREN_STEP3               0x80U

typedef enum
{
    SW3526_PROTOCOL_NONE = 0,
    SW3526_PROTOCOL_QC2 = 1,
    SW3526_PROTOCOL_QC3 = 2,
    SW3526_PROTOCOL_FCP = 3,
    SW3526_PROTOCOL_SCP = 4,
    SW3526_PROTOCOL_PD_FIXED = 5,
    SW3526_PROTOCOL_PD_PPS = 6,
    SW3526_PROTOCOL_PE11 = 7,
    SW3526_PROTOCOL_PE20 = 8,
    SW3526_PROTOCOL_VOOC = 9,
    SW3526_PROTOCOL_SFCP = 10,
    SW3526_PROTOCOL_AFC = 11
} SW3526_Protocol;

typedef enum
{
    SW3526_IO_IDLE = 0,
    SW3526_IO_OK,
    SW3526_IO_NACK,
    SW3526_IO_TIMEOUT,
    SW3526_IO_BUS_STUCK,
    SW3526_IO_ERROR
} SW3526_IOResult;

struct SW3526_StatusTypedef
{
    uint8_t online;
    uint8_t version;
    uint8_t protocol;
    uint8_t sys_stat;
    uint8_t fault;

    uint16_t vin_raw;
    uint16_t vout_raw;
    uint16_t iout_raw;
    uint16_t vin_mv;
    uint16_t vout_mv;
    uint32_t iout_ma_x10; /* 0.1 mA units; 2.5 mA/bit -> raw*25 */
};

typedef struct
{
    SoftI2C_Handle *i2c;
    uint8_t address;
    volatile SW3526_IOResult last_io;
    struct SW3526_StatusTypedef status;

    /* Per-handle persistent coroutine/I2C storage. No global transfer state. */
    coro_pt_t io_pt;
    uint8_t txbuf[4];
    uint8_t rxbuf[4];
} SW3526_Handle;

/* SW6306-style coroutine syntax adapted to coroOS and explicit handles. */
#define SW3526_RET              uint8_t
#define SW3526_ARGS(...)        coro_pt_t *pt, SW3526_Handle *handle, __VA_ARGS__
#define SW3526_NOARG            coro_pt_t *pt, SW3526_Handle *handle
#define SW3526_FUNC_BEGIN       THRD_BEGIN
#define SW3526_FUNC_END         THRD_END
#define SW3526_UNTIL(cond)      THRD_UNTIL(cond)
#define SW3526_RETURN_END()     do { CORO_INIT(pt); return CORO_ENDED; } while(0)

void SW3526_HandleInit(SW3526_Handle *handle, SoftI2C_Handle *i2c);

SW3526_RET SW3526_ByteWrite(SW3526_ARGS(uint8_t reg, uint8_t data));
SW3526_RET SW3526_ByteRead(SW3526_ARGS(uint8_t reg, uint8_t *data));
SW3526_RET SW3526_BytesRead(SW3526_ARGS(uint8_t reg, uint8_t *data, uint16_t len));
SW3526_RET SW3526_UnlockWrite(SW3526_NOARG);
SW3526_RET SW3526_ADCRead(SW3526_ARGS(uint8_t channel, uint16_t *raw));
SW3526_RET SW3526_ADCLoad(SW3526_NOARG);
SW3526_RET SW3526_ProtocolLoad(SW3526_NOARG);
SW3526_RET SW3526_PortStatusLoad(SW3526_NOARG);
SW3526_RET SW3526_StatusLoad(SW3526_NOARG);

const struct SW3526_StatusTypedef *SW3526_GetStatus(const SW3526_Handle *handle);
uint8_t SW3526_IsOnline(const SW3526_Handle *handle);
uint8_t SW3526_IsProtocolOnline(const SW3526_Handle *handle);
uint8_t SW3526_IsPortOn(const SW3526_Handle *handle);
uint8_t SW3526_IsBuckOn(const SW3526_Handle *handle);
uint8_t SW3526_GetPDVersion(const SW3526_Handle *handle);
SW3526_Protocol SW3526_GetProtocol(const SW3526_Handle *handle);
uint16_t SW3526_ReadVINmV(const SW3526_Handle *handle);
uint16_t SW3526_ReadVOUTmV(const SW3526_Handle *handle);
uint32_t SW3526_ReadIOUTmA_x10(const SW3526_Handle *handle);

#ifdef __cplusplus
}
#endif

#endif /* __SW3526_H__ */
