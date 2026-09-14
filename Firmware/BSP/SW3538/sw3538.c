#include "sw3538.h"
#include <string.h>

static SW3538_IOResult map_result(I2C_API_Result result)
{
    switch(result)
    {
        case I2C_API_RESULT_OK:      return SW3538_IO_OK;
        case I2C_API_RESULT_NACK:    return SW3538_IO_NACK;
        case I2C_API_RESULT_TIMEOUT: return SW3538_IO_TIMEOUT;
        default:                     return SW3538_IO_ERROR;
    }
}

#define SW3538_SPAWN_IO(call_expr)                                      \
    do {                                                                 \
        CORO_INIT(&handle->io_pt);                                       \
        CORO_WAIT_UNTIL(pt, ((call_expr) == CORO_ENDED));                \
        if(handle->last_io != SW3538_IO_OK)                              \
        {                                                                \
            handle->status.online = 0u;                                  \
            SW3538_RETURN_END();                                         \
        }                                                                \
    } while(0)

static uint16_t decode_adc12(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (((uint16_t)bytes[1] & 0x0Fu) << 8));
}

static uint16_t decode_ilim_ma(uint8_t raw)
{
    if(raw & 0x80u)
        return (uint16_t)(500u + 25u * (uint16_t)(raw & 0x7Fu));

    return (uint16_t)(1000u + 50u * (uint16_t)(raw & 0x7Fu));
}

static uint8_t adc_force_mask(uint8_t channel)
{
    switch(channel)
    {
        case SW3538_ADC_SET_PT1_IOUT: return SW3538_CTRG_FORCE2_ADC_PT1_I;
        case SW3538_ADC_SET_PT2_IOUT: return SW3538_CTRG_FORCE2_ADC_PT2_I;
        case SW3538_ADC_SET_VOUT:
        case SW3538_ADC_SET_VOUT_MV:  return SW3538_CTRG_FORCE2_ADC_VOUT;
        case SW3538_ADC_SET_VIN:      return SW3538_CTRG_FORCE2_ADC_VIN;
        case SW3538_ADC_SET_VNTC:     return SW3538_CTRG_FORCE2_ADC_NTC;
        default:                       return 0u;
    }
}

void SW3538_HandleInit(SW3538_Handle *handle, I2C_API_Owner owner)
{
    if(handle == 0)
        return;
    memset(handle, 0, sizeof(*handle));
    handle->address = SW3538_I2C_ADDR;
    handle->i2c_owner = owner;
    handle->last_io = SW3538_IO_IDLE;
    CORO_INIT(&handle->io_pt);
}

SW3538_RET SW3538_ByteWrite(SW3538_ARGS(uint8_t reg, uint8_t data))
{
    I2C_API_Result result;
    SW3538_FUNC_BEGIN;

    if(handle == 0 || handle->i2c_owner == I2C_API_OWNER_NONE)
        SW3538_RETURN_END();

    handle->txbuf[0] = reg;
    handle->txbuf[1] = data;
    SW3538_UNTIL(I2C_API_TryWrite(handle->i2c_owner, handle->address, handle->txbuf, 2u));
    SW3538_UNTIL(I2C_API_GetResult(handle->i2c_owner) != I2C_API_RESULT_ACTIVE);
    result = I2C_API_TakeResult(handle->i2c_owner);
    handle->last_io = map_result(result);
    handle->status.online = (result == I2C_API_RESULT_OK) ? 1u : 0u;

    SW3538_FUNC_END;
}

SW3538_RET SW3538_ByteRead(SW3538_ARGS(uint8_t reg, uint8_t *data))
{
    I2C_API_Result result;
    SW3538_FUNC_BEGIN;

    if(handle == 0 || data == 0 || handle->i2c_owner == I2C_API_OWNER_NONE)
        SW3538_RETURN_END();

    handle->txbuf[0] = reg;
    SW3538_UNTIL(I2C_API_TryWriteRead(handle->i2c_owner, handle->address,
                                      handle->txbuf, 1u, data, 1u));
    SW3538_UNTIL(I2C_API_GetResult(handle->i2c_owner) != I2C_API_RESULT_ACTIVE);
    result = I2C_API_TakeResult(handle->i2c_owner);
    handle->last_io = map_result(result);
    handle->status.online = (result == I2C_API_RESULT_OK) ? 1u : 0u;

    SW3538_FUNC_END;
}

SW3538_RET SW3538_BytesRead(SW3538_ARGS(uint8_t reg, uint8_t *data, uint16_t len))
{
    I2C_API_Result result;
    SW3538_FUNC_BEGIN;

    if(handle == 0 || data == 0 || len == 0u || handle->i2c_owner == I2C_API_OWNER_NONE)
        SW3538_RETURN_END();

    handle->txbuf[0] = reg;
    SW3538_UNTIL(I2C_API_TryWriteRead(handle->i2c_owner, handle->address,
                                      handle->txbuf, 1u, data, len));
    SW3538_UNTIL(I2C_API_GetResult(handle->i2c_owner) != I2C_API_RESULT_ACTIVE);
    result = I2C_API_TakeResult(handle->i2c_owner);
    handle->last_io = map_result(result);
    handle->status.online = (result == I2C_API_RESULT_OK) ? 1u : 0u;

    SW3538_FUNC_END;
}

SW3538_RET SW3538_UnlockWrite(SW3538_NOARG)
{
    SW3538_FUNC_BEGIN;
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP3));
    SW3538_FUNC_END;
}

SW3538_RET SW3538_UnlockForce(SW3538_NOARG)
{
    SW3538_FUNC_BEGIN;
    /* Reg0x15 itself is an ordinary writable register, therefore perform the
     * general I2C write-enable sequence before the force-register sequence. */
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP3));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP3));
    SW3538_FUNC_END;
}

SW3538_RET SW3538_SetPowerLimitW(SW3538_ARGS(uint8_t watts))
{
    /* static: value crosses the sub-coroutine yields below (an automatic local
     * is lost when the function resumes at its saved case label). */
    static uint16_t current_10ma;

    SW3538_FUNC_BEGIN;

    if(handle == 0 || watts < 18u || watts > 140u)
    {
        if(handle != 0)
            handle->last_io = SW3538_IO_ERROR;
        SW3538_RETURN_END();
    }

    /* Reg0x12A/0x12B encode the 20 V PDO current in 10 mA units.
     * P = 20 V * I => current_10ma = watts * 5. */
    current_10ma = (uint16_t)watts * 5u;

    /* General write enable followed by extended-register-bank select. */
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP3));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_EXT_BANK));

    /* Reg0x115[4]=0 selects register-controlled system power instead of PSET. */
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle,
                                     SW3538_XREG_SYS_POWER_SELECT,
                                     &handle->rxbuf[0]));
    handle->rxbuf[0] &= (uint8_t)~SW3538_XREG_SYS_PWR_PSET_BIT;
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle,
                                      SW3538_XREG_SYS_POWER_SELECT,
                                      handle->rxbuf[0]));

    /* Preserve the low-bit fields for 9/12/15 V PDO currents in Reg0x12B. */
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle,
                                     SW3538_XREG_PD_CUR_LO,
                                     &handle->rxbuf[0]));
    handle->rxbuf[0] = (uint8_t)((handle->rxbuf[0] & 0x3Fu) |
                         (uint8_t)((current_10ma & 0x03u) << 6));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle,
                                      SW3538_XREG_PD20_CUR_HI,
                                      (uint8_t)(current_10ma >> 2)));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle,
                                      SW3538_XREG_PD_CUR_LO,
                                      handle->rxbuf[0]));

    /* Reg0x180=0 clears address bit 8 and restores normal 0x00..0xFF access. */
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle,
                                      SW3538_XREG_BANK_CLEAR, 0u));

    handle->status.configured_power_w = watts;
    SW3538_FUNC_END;
}

SW3538_RET SW3538_ADCRead(SW3538_ARGS(uint8_t channel, uint16_t *raw))
{
    SW3538_FUNC_BEGIN;
    if(raw == 0)
        SW3538_RETURN_END();

    if(adc_force_mask(channel) == 0u)
    {
        handle->last_io = SW3538_IO_ERROR;
        SW3538_RETURN_END();
    }

    /* Make a standalone ADC read self-contained.  VIN sampling is disabled by
     * default, and the other channels may also be gated by the current scene. */
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP3));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP3));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_CTRG_FORCE2, &handle->rxbuf[0]));
    handle->rxbuf[0] |= adc_force_mask(channel);
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE2, handle->rxbuf[0]));

    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_ADC_SET, channel));
    SW3538_SPAWN_IO(SW3538_BytesRead(&handle->io_pt, handle, SW3538_STRG_ADCL, handle->rxbuf, 2u));
    *raw = decode_adc12(handle->rxbuf);

    SW3538_FUNC_END;
}

SW3538_RET SW3538_ADCLoad(SW3538_NOARG)
{
    SW3538_FUNC_BEGIN;

    /* Unlock ordinary writes, then unlock force-operation registers 0x16..0x19. */
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_WREN, SW3538_WREN_STEP3));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP1));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP2));
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE_WREN, SW3538_FORCE_WREN_STEP3));

    /* VIN ADC is documented as disabled by default; force all four channels
     * used by this driver on, while preserving unrelated force bits. */
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_CTRG_FORCE2, &handle->rxbuf[0]));
    handle->rxbuf[0] |= (SW3538_CTRG_FORCE2_ADC_VOUT | SW3538_CTRG_FORCE2_ADC_VIN |
                         SW3538_CTRG_FORCE2_ADC_PT1_I | SW3538_CTRG_FORCE2_ADC_PT2_I);
    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_FORCE2, handle->rxbuf[0]));

    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_ADC_SET, SW3538_ADC_SET_VIN));
    SW3538_SPAWN_IO(SW3538_BytesRead(&handle->io_pt, handle, SW3538_STRG_ADCL, handle->rxbuf, 2u));
    handle->status.vin_raw = decode_adc12(handle->rxbuf);
    handle->status.vin_mv = (uint16_t)(handle->status.vin_raw * 10u);

    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_ADC_SET, SW3538_ADC_SET_VOUT));
    SW3538_SPAWN_IO(SW3538_BytesRead(&handle->io_pt, handle, SW3538_STRG_ADCL, handle->rxbuf, 2u));
    handle->status.vout_raw = decode_adc12(handle->rxbuf);
    handle->status.vout_mv = (uint16_t)(handle->status.vout_raw * 6u);

    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_ADC_SET, SW3538_ADC_SET_PT1_IOUT));
    SW3538_SPAWN_IO(SW3538_BytesRead(&handle->io_pt, handle, SW3538_STRG_ADCL, handle->rxbuf, 2u));
    handle->status.pt1_iout_raw = decode_adc12(handle->rxbuf);
    handle->status.pt1_iout_ma_x10 = (uint32_t)handle->status.pt1_iout_raw * 25u;

    SW3538_SPAWN_IO(SW3538_ByteWrite(&handle->io_pt, handle, SW3538_CTRG_ADC_SET, SW3538_ADC_SET_PT2_IOUT));
    SW3538_SPAWN_IO(SW3538_BytesRead(&handle->io_pt, handle, SW3538_STRG_ADCL, handle->rxbuf, 2u));
    handle->status.pt2_iout_raw = decode_adc12(handle->rxbuf);
    handle->status.pt2_iout_ma_x10 = (uint32_t)handle->status.pt2_iout_raw * 25u;

    SW3538_FUNC_END;
}

SW3538_RET SW3538_ProtocolLoad(SW3538_NOARG)
{
    SW3538_FUNC_BEGIN;
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PROTOCOL, &handle->status.protocol));
    SW3538_FUNC_END;
}

SW3538_RET SW3538_PortStatusLoad(SW3538_NOARG)
{
    SW3538_FUNC_BEGIN;
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_SYS_STAT0, &handle->status.sys_stat0));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_SYS_STAT1, &handle->status.sys_stat1));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PT1_ILIM, &handle->status.pt1_ilim_raw));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PT2_ILIM, &handle->status.pt2_ilim_raw));
    SW3538_FUNC_END;
}

SW3538_RET SW3538_StatusLoad(SW3538_NOARG)
{
    SW3538_FUNC_BEGIN;

    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_REV, &handle->status.version));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PMAX, &handle->status.pmax_w));
    handle->status.pmax_w &= 0x7Fu;
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PROTOCOL, &handle->status.protocol));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_SYS_STAT0, &handle->status.sys_stat0));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_SYS_STAT1, &handle->status.sys_stat1));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PT1_ILIM, &handle->status.pt1_ilim_raw));
    SW3538_SPAWN_IO(SW3538_ByteRead(&handle->io_pt, handle, SW3538_STRG_PT2_ILIM, &handle->status.pt2_ilim_raw));

    SW3538_FUNC_END;
}

const struct SW3538_StatusTypedef *SW3538_GetStatus(const SW3538_Handle *handle)
{
    return (handle != 0) ? &handle->status : 0;
}

uint8_t SW3538_IsOnline(const SW3538_Handle *handle)
{
    return (handle != 0) ? handle->status.online : 0u;
}

uint8_t SW3538_GetPortStatus(const SW3538_Handle *handle,
                             uint8_t port,
                             SW3538_PortStatus *status)
{
    if(handle == 0 || status == 0 || (port != 1u && port != 2u))
        return 0u;

    status->online = handle->status.online;

    if(port == 1u)
    {
        status->path_on =
            (handle->status.sys_stat0 & SW3538_STRG_SYS_STAT0_PORT1_ON) ? 1u : 0u;
        status->device_online =
            (handle->status.sys_stat1 & SW3538_STRG_SYS_STAT1_PORT1_ONLINE) ? 1u : 0u;
        status->current_limit_ma = decode_ilim_ma(handle->status.pt1_ilim_raw);
    }
    else
    {
        status->path_on =
            (handle->status.sys_stat0 & SW3538_STRG_SYS_STAT0_PORT2_ON) ? 1u : 0u;
        status->device_online =
            (handle->status.sys_stat1 & SW3538_STRG_SYS_STAT1_PORT2_ONLINE) ? 1u : 0u;
        status->current_limit_ma = decode_ilim_ma(handle->status.pt2_ilim_raw);
    }

    return handle->status.online;
}

uint8_t SW3538_IsProtocolFast(const SW3538_Handle *handle)
{
    return (handle != 0 && (handle->status.protocol & SW3538_STRG_PROTOCOL_FAST) != 0u) ? 1u : 0u;
}

uint8_t SW3538_IsPort1On(const SW3538_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat0 & SW3538_STRG_SYS_STAT0_PORT1_ON) != 0u) ? 1u : 0u;
}

uint8_t SW3538_IsPort2On(const SW3538_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat0 & SW3538_STRG_SYS_STAT0_PORT2_ON) != 0u) ? 1u : 0u;
}

uint8_t SW3538_IsPort1DeviceOnline(const SW3538_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat1 & SW3538_STRG_SYS_STAT1_PORT1_ONLINE) != 0u) ? 1u : 0u;
}

uint8_t SW3538_IsPort2DeviceOnline(const SW3538_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat1 & SW3538_STRG_SYS_STAT1_PORT2_ONLINE) != 0u) ? 1u : 0u;
}

uint8_t SW3538_IsBuckOn(const SW3538_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat0 & SW3538_STRG_SYS_STAT0_BUCK_ON) != 0u) ? 1u : 0u;
}

uint8_t SW3538_GetPDVersion(const SW3538_Handle *handle)
{
    if(handle == 0)
        return 0u;
    return (uint8_t)((handle->status.protocol & SW3538_STRG_PROTOCOL_PD_VER_MSK) >> SW3538_STRG_PROTOCOL_PD_VER_SHIFT);
}

SW3538_Protocol SW3538_GetProtocol(const SW3538_Handle *handle)
{
    if(handle == 0)
        return SW3538_PROTOCOL_NONE;
    return (SW3538_Protocol)(handle->status.protocol & SW3538_STRG_PROTOCOL_TYPE_MSK);
}

uint16_t SW3538_ReadVINmV(const SW3538_Handle *handle)
{
    return (handle != 0) ? handle->status.vin_mv : 0u;
}

uint16_t SW3538_ReadVOUTmV(const SW3538_Handle *handle)
{
    return (handle != 0) ? handle->status.vout_mv : 0u;
}

uint32_t SW3538_ReadPort1IOUTmA_x10(const SW3538_Handle *handle)
{
    return (handle != 0) ? handle->status.pt1_iout_ma_x10 : 0u;
}

uint32_t SW3538_ReadPort2IOUTmA_x10(const SW3538_Handle *handle)
{
    return (handle != 0) ? handle->status.pt2_iout_ma_x10 : 0u;
}
