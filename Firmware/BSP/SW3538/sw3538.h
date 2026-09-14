/* SW3538 operation library
 * TKWTL Desktop PD Power Converter
 * Register map basis: iSW RG108_3 v1.2
 */
#ifndef __SW3538_H__
#define __SW3538_H__

#include <stdint.h>
#include "coroOS.h"
#include "i2c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SW3538_I2C_ADDR                 0x3CU

/* Register naming follows the SW6306 library convention:
 *   STRG = status/readback register
 *   CTRG = control/configuration register
 */
#define SW3538_STRG_REV                 0x00U
#define SW3538_STRG_PMAX                0x02U
#define SW3538_STRG_DAC_VOLL            0x05U
#define SW3538_STRG_DAC_VOLH            0x06U
#define SW3538_STRG_PT1_ILIM            0x07U
#define SW3538_STRG_PT2_ILIM            0x08U
#define SW3538_STRG_PROTOCOL            0x09U
#define SW3538_STRG_SYS_STAT0           0x0AU
#define SW3538_STRG_SYS_STAT1           0x0DU
#define SW3538_CTRG_WREN                0x10U
#define SW3538_CTRG_SYS0                0x11U
#define SW3538_CTRG_SYS1                0x13U
#define SW3538_CTRG_SYS2                0x14U
#define SW3538_CTRG_FORCE_WREN          0x15U
#define SW3538_CTRG_FORCE0              0x16U
#define SW3538_CTRG_FORCE1              0x17U
#define SW3538_CTRG_FORCE2              0x18U
#define SW3538_CTRG_FORCE3              0x19U
#define SW3538_STRG_IRQ                 0x20U
#define SW3538_CTRG_IRQ_EN              0x28U
#define SW3538_CTRG_PT1_ILIM            0x38U
#define SW3538_CTRG_PT2_ILIM            0x39U
#define SW3538_CTRG_ADC_SET             0x40U
#define SW3538_STRG_ADCL                0x41U
#define SW3538_STRG_ADCH                0x42U
#define SW3538_STRG_NTC_CURR            0x44U
#define SW3538_CTRG_PD_CMD              0xA7U

/* Extended 0x1xx register bank.  Reg0x10=0x81 selects the bank; while selected,
 * low addresses 0x15/0x2A/0x2B refer to Reg0x115/0x12A/0x12B.  Writing zero to
 * low address 0x80 (Reg0x180) clears the high address bit. */
#define SW3538_WREN_EXT_BANK            0x81U
#define SW3538_XREG_SYS_POWER_SELECT    0x15U
#define SW3538_XREG_PD20_CUR_HI         0x2AU
#define SW3538_XREG_PD_CUR_LO           0x2BU
#define SW3538_XREG_BANK_CLEAR          0x80U
#define SW3538_XREG_SYS_PWR_PSET_BIT    0x10U

#define SW3538_WREN_STEP1               0x20U
#define SW3538_WREN_STEP2               0x40U
#define SW3538_WREN_STEP3               0x80U
#define SW3538_FORCE_WREN_STEP1         0x20U
#define SW3538_FORCE_WREN_STEP2         0x40U
#define SW3538_FORCE_WREN_STEP3         0x80U

/* 0x09 protocol indication. */
#define SW3538_STRG_PROTOCOL_VOLTAGE_FAST    0x80U
#define SW3538_STRG_PROTOCOL_FAST            0x40U
#define SW3538_STRG_PROTOCOL_PD_VER_MSK      0x30U
#define SW3538_STRG_PROTOCOL_PD_VER_SHIFT    4U
#define SW3538_STRG_PROTOCOL_TYPE_MSK        0x0FU

/* 0x0A system status 0. */
#define SW3538_STRG_SYS_STAT0_BUCK_ON        0x04U
#define SW3538_STRG_SYS_STAT0_PORT2_ON       0x02U
#define SW3538_STRG_SYS_STAT0_PORT1_ON       0x01U

/* 0x0D system status 1. */
#define SW3538_STRG_SYS_STAT1_PORT1_ONLINE   0x02U
#define SW3538_STRG_SYS_STAT1_PORT2_ONLINE   0x01U

/* 0x18 force ADC channels on. */
#define SW3538_CTRG_FORCE2_ADC_VOUT          0x80U
#define SW3538_CTRG_FORCE2_ADC_VIN           0x40U
#define SW3538_CTRG_FORCE2_ADC_PT1_I         0x20U
#define SW3538_CTRG_FORCE2_ADC_PT2_I         0x10U
#define SW3538_CTRG_FORCE2_ADC_NTC           0x02U

/* 0x40 ADC data select. */
#define SW3538_ADC_SET_PT1_IOUT         0x01U /* 2.5 mA/bit @ 5 mOhm */
#define SW3538_ADC_SET_PT2_IOUT         0x02U /* 2.5 mA/bit @ 5 mOhm */
#define SW3538_ADC_SET_VOUT             0x05U /* 6 mV/bit */
#define SW3538_ADC_SET_VIN              0x06U /* 10 mV/bit */
#define SW3538_ADC_SET_VNTC             0x07U /* 1.2 mV/bit */
#define SW3538_ADC_SET_VOUT_MV          0x0BU /* 1 mV/bit, wider result */

typedef enum
{
    SW3538_PROTOCOL_NONE = 0,
    SW3538_PROTOCOL_QC2 = 1,
    SW3538_PROTOCOL_QC3 = 2,
    SW3538_PROTOCOL_QC3P = 3,
    SW3538_PROTOCOL_FCP = 4,
    SW3538_PROTOCOL_SCP = 5,
    SW3538_PROTOCOL_PD_FIXED = 6,
    SW3538_PROTOCOL_PD_PPS = 7,
    SW3538_PROTOCOL_PE11 = 8,
    SW3538_PROTOCOL_PE20 = 9,
    SW3538_PROTOCOL_VOOC10 = 10,
    SW3538_PROTOCOL_VOOC40 = 11,
    SW3538_PROTOCOL_SFCP = 13,
    SW3538_PROTOCOL_AFC = 14,
    SW3538_PROTOCOL_TFCP = 15
} SW3538_Protocol;

typedef enum
{
    SW3538_IO_IDLE = 0,
    SW3538_IO_OK,
    SW3538_IO_NACK,
    SW3538_IO_TIMEOUT,
    SW3538_IO_ERROR
} SW3538_IOResult;

struct SW3538_StatusTypedef
{
    uint8_t online;
    uint8_t version;
    uint8_t protocol;
    uint8_t sys_stat0;
    uint8_t sys_stat1;
    uint8_t pmax_w;
    uint8_t pt1_ilim_raw;
    uint8_t pt2_ilim_raw;
    uint8_t configured_power_w;

    uint16_t vin_raw;
    uint16_t vout_raw;
    uint16_t pt1_iout_raw;
    uint16_t pt2_iout_raw;
    uint16_t vin_mv;
    uint16_t vout_mv;
    uint32_t pt1_iout_ma_x10;
    uint32_t pt2_iout_ma_x10;
};

typedef struct
{
    uint8_t online;
    uint8_t path_on;
    uint8_t device_online;
    uint16_t current_limit_ma;
} SW3538_PortStatus;

typedef struct
{
    uint8_t address;
    I2C_API_Owner i2c_owner;
    volatile SW3538_IOResult last_io;
    struct SW3538_StatusTypedef status;

    coro_pt_t io_pt;
    uint8_t txbuf[4];
    uint8_t rxbuf[4];
} SW3538_Handle;

#define SW3538_RET              uint8_t
#define SW3538_ARGS(...)        coro_pt_t *pt, SW3538_Handle *handle, __VA_ARGS__
#define SW3538_NOARG            coro_pt_t *pt, SW3538_Handle *handle
#define SW3538_FUNC_BEGIN       THRD_BEGIN
#define SW3538_FUNC_END         THRD_END
#define SW3538_UNTIL(cond)      THRD_UNTIL(cond)
#define SW3538_RETURN_END()     do { CORO_INIT(pt); return CORO_ENDED; } while(0)

void SW3538_HandleInit(SW3538_Handle *handle, I2C_API_Owner owner);

SW3538_RET SW3538_ByteWrite(SW3538_ARGS(uint8_t reg, uint8_t data));
SW3538_RET SW3538_ByteRead(SW3538_ARGS(uint8_t reg, uint8_t *data));
SW3538_RET SW3538_BytesRead(SW3538_ARGS(uint8_t reg, uint8_t *data, uint16_t len));
SW3538_RET SW3538_UnlockWrite(SW3538_NOARG);
SW3538_RET SW3538_UnlockForce(SW3538_NOARG);
SW3538_RET SW3538_ADCRead(SW3538_ARGS(uint8_t channel, uint16_t *raw));
SW3538_RET SW3538_ADCLoad(SW3538_NOARG);
SW3538_RET SW3538_ProtocolLoad(SW3538_NOARG);
SW3538_RET SW3538_PortStatusLoad(SW3538_NOARG);
SW3538_RET SW3538_StatusLoad(SW3538_NOARG);
/* Set the register-controlled system/PD power budget through the documented
 * 20 V PDO current field.  Valid range for this product: 18..140 W. */
SW3538_RET SW3538_SetPowerLimitW(SW3538_ARGS(uint8_t watts));

const struct SW3538_StatusTypedef *SW3538_GetStatus(const SW3538_Handle *handle);
uint8_t SW3538_GetPortStatus(const SW3538_Handle *handle,
                             uint8_t port,
                             SW3538_PortStatus *status);
uint8_t SW3538_IsOnline(const SW3538_Handle *handle);
uint8_t SW3538_IsProtocolFast(const SW3538_Handle *handle);
uint8_t SW3538_IsPort1On(const SW3538_Handle *handle);
uint8_t SW3538_IsPort2On(const SW3538_Handle *handle);
uint8_t SW3538_IsPort1DeviceOnline(const SW3538_Handle *handle);
uint8_t SW3538_IsPort2DeviceOnline(const SW3538_Handle *handle);
uint8_t SW3538_IsBuckOn(const SW3538_Handle *handle);
uint8_t SW3538_GetPDVersion(const SW3538_Handle *handle);
SW3538_Protocol SW3538_GetProtocol(const SW3538_Handle *handle);
uint16_t SW3538_ReadVINmV(const SW3538_Handle *handle);
uint16_t SW3538_ReadVOUTmV(const SW3538_Handle *handle);
uint32_t SW3538_ReadPort1IOUTmA_x10(const SW3538_Handle *handle);
uint32_t SW3538_ReadPort2IOUTmA_x10(const SW3538_Handle *handle);

#ifdef __cplusplus
}
#endif

#endif /* __SW3538_H__ */
