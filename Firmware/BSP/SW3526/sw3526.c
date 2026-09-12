#include "sw3526.h"
#include <string.h>

static SW3526_IOResult map_result(SoftI2C_Result result)
{
    switch(result)
    {
        case SOFT_I2C_RESULT_OK:      return SW3526_IO_OK;
        case SOFT_I2C_RESULT_NACK:    return SW3526_IO_NACK;
        case SOFT_I2C_RESULT_TIMEOUT:   return SW3526_IO_TIMEOUT;
        case SOFT_I2C_RESULT_BUS_STUCK: return SW3526_IO_BUS_STUCK;
        default:                        return SW3526_IO_ERROR;
    }
}

static void note_result(SW3526_Handle *handle, SoftI2C_Result result)
{
    handle->last_io = map_result(result);
    handle->status.online = (result == SOFT_I2C_RESULT_OK) ? 1u : 0u;

    /* Recovery is deliberately restricted to fault paths; normal runtime I/O
     * remains cooperative and non-blocking. */
    if(result == SOFT_I2C_RESULT_TIMEOUT || result == SOFT_I2C_RESULT_BUS_STUCK)
        (void)SoftI2C_Recover(handle->i2c);
}

#define SW3526_SPAWN_IO(call_expr)                                      \
    do {                                                                 \
        CORO_INIT(&handle->io_pt);                                       \
        CORO_WAIT_UNTIL(pt, ((call_expr) == CORO_ENDED));                \
        if(handle->last_io != SW3526_IO_OK)                              \
        {                                                                \
            handle->status.online = 0u;                                  \
            SW3526_RETURN_END();                                         \
        }                                                                \
    } while(0)

static uint16_t decode_adc12(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 4) | ((uint16_t)bytes[1] & 0x0Fu));
}

void SW3526_HandleInit(SW3526_Handle *handle, SoftI2C_Handle *i2c)
{
    if(handle == 0)
        return;
    memset(handle, 0, sizeof(*handle));
    handle->i2c = i2c;
    handle->address = SW3526_I2C_ADDR;
    handle->last_io = SW3526_IO_IDLE;
    CORO_INIT(&handle->io_pt);
}

SW3526_RET SW3526_ByteWrite(SW3526_ARGS(uint8_t reg, uint8_t data))
{
    SoftI2C_Result result;
    SW3526_FUNC_BEGIN;

    if(handle == 0 || handle->i2c == 0)
        SW3526_RETURN_END();

    handle->txbuf[0] = reg;
    handle->txbuf[1] = data;
    SW3526_UNTIL(SoftI2C_TryWrite(handle->i2c, handle->address, handle->txbuf, 2u));
    SW3526_UNTIL(SoftI2C_GetResult(handle->i2c) != SOFT_I2C_RESULT_ACTIVE);
    result = SoftI2C_TakeResult(handle->i2c);
    note_result(handle, result);

    SW3526_FUNC_END;
}

SW3526_RET SW3526_ByteRead(SW3526_ARGS(uint8_t reg, uint8_t *data))
{
    SoftI2C_Result result;
    SW3526_FUNC_BEGIN;

    if(handle == 0 || handle->i2c == 0 || data == 0)
        SW3526_RETURN_END();

    handle->txbuf[0] = reg;
    SW3526_UNTIL(SoftI2C_TryWriteRead(handle->i2c, handle->address,
                                      handle->txbuf, 1u, data, 1u));
    SW3526_UNTIL(SoftI2C_GetResult(handle->i2c) != SOFT_I2C_RESULT_ACTIVE);
    result = SoftI2C_TakeResult(handle->i2c);
    note_result(handle, result);

    SW3526_FUNC_END;
}

SW3526_RET SW3526_BytesRead(SW3526_ARGS(uint8_t reg, uint8_t *data, uint16_t len))
{
    SoftI2C_Result result;
    SW3526_FUNC_BEGIN;

    if(handle == 0 || handle->i2c == 0 || data == 0 || len == 0u)
        SW3526_RETURN_END();

    handle->txbuf[0] = reg;
    SW3526_UNTIL(SoftI2C_TryWriteRead(handle->i2c, handle->address,
                                      handle->txbuf, 1u, data, len));
    SW3526_UNTIL(SoftI2C_GetResult(handle->i2c) != SOFT_I2C_RESULT_ACTIVE);
    result = SoftI2C_TakeResult(handle->i2c);
    note_result(handle, result);

    SW3526_FUNC_END;
}

SW3526_RET SW3526_UnlockWrite(SW3526_NOARG)
{
    SW3526_FUNC_BEGIN;
    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_WREN, SW3526_WREN_STEP1));
    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_WREN, SW3526_WREN_STEP2));
    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_WREN, SW3526_WREN_STEP3));
    SW3526_FUNC_END;
}

SW3526_RET SW3526_ADCRead(SW3526_ARGS(uint8_t channel, uint16_t *raw))
{
    SW3526_FUNC_BEGIN;
    if(raw == 0)
        SW3526_RETURN_END();

    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_ADC_SET, channel));
    SW3526_SPAWN_IO(SW3526_BytesRead(&handle->io_pt, handle, SW3526_STRG_ADCH, handle->rxbuf, 2u));
    *raw = decode_adc12(handle->rxbuf);
    SW3526_FUNC_END;
}

SW3526_RET SW3526_ADCLoad(SW3526_NOARG)
{
    SW3526_FUNC_BEGIN;

    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_ADC_SET, SW3526_ADC_SET_VIN));
    SW3526_SPAWN_IO(SW3526_BytesRead(&handle->io_pt, handle, SW3526_STRG_ADCH, handle->rxbuf, 2u));
    handle->status.vin_raw = decode_adc12(handle->rxbuf);
    handle->status.vin_mv = (uint16_t)(handle->status.vin_raw * 10u);

    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_ADC_SET, SW3526_ADC_SET_VOUT));
    SW3526_SPAWN_IO(SW3526_BytesRead(&handle->io_pt, handle, SW3526_STRG_ADCH, handle->rxbuf, 2u));
    handle->status.vout_raw = decode_adc12(handle->rxbuf);
    handle->status.vout_mv = (uint16_t)(handle->status.vout_raw * 6u);

    SW3526_SPAWN_IO(SW3526_ByteWrite(&handle->io_pt, handle, SW3526_CTRG_ADC_SET, SW3526_ADC_SET_IOUT));
    SW3526_SPAWN_IO(SW3526_BytesRead(&handle->io_pt, handle, SW3526_STRG_ADCH, handle->rxbuf, 2u));
    handle->status.iout_raw = decode_adc12(handle->rxbuf);
    handle->status.iout_ma_x10 = (uint32_t)handle->status.iout_raw * 25u;

    SW3526_FUNC_END;
}

SW3526_RET SW3526_ProtocolLoad(SW3526_NOARG)
{
    SW3526_FUNC_BEGIN;
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_PROTOCOL, &handle->status.protocol));
    SW3526_FUNC_END;
}

SW3526_RET SW3526_PortStatusLoad(SW3526_NOARG)
{
    SW3526_FUNC_BEGIN;
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_SYS_STAT, &handle->status.sys_stat));
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_FAULT, &handle->status.fault));
    SW3526_FUNC_END;
}

SW3526_RET SW3526_StatusLoad(SW3526_NOARG)
{
    SW3526_FUNC_BEGIN;

    /* Keep the composite routine flat: nested composite coroutines would need
     * a second child continuation.  Leaf I/O calls safely reuse io_pt. */
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_REV, &handle->status.version));
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_PROTOCOL, &handle->status.protocol));
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_SYS_STAT, &handle->status.sys_stat));
    SW3526_SPAWN_IO(SW3526_ByteRead(&handle->io_pt, handle, SW3526_STRG_FAULT, &handle->status.fault));

    SW3526_FUNC_END;
}

const struct SW3526_StatusTypedef *SW3526_GetStatus(const SW3526_Handle *handle)
{
    return (handle != 0) ? &handle->status : 0;
}

uint8_t SW3526_IsOnline(const SW3526_Handle *handle)
{
    return (handle != 0) ? handle->status.online : 0u;
}

uint8_t SW3526_IsProtocolOnline(const SW3526_Handle *handle)
{
    return (handle != 0 && (handle->status.protocol & SW3526_STRG_PROTOCOL_ONLINE) != 0u) ? 1u : 0u;
}

uint8_t SW3526_IsPortOn(const SW3526_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat & SW3526_STRG_SYS_STAT_PORT_ON) != 0u) ? 1u : 0u;
}

uint8_t SW3526_IsBuckOn(const SW3526_Handle *handle)
{
    return (handle != 0 && (handle->status.sys_stat & SW3526_STRG_SYS_STAT_BUCK_ON) != 0u) ? 1u : 0u;
}

uint8_t SW3526_GetPDVersion(const SW3526_Handle *handle)
{
    if(handle == 0)
        return 0u;
    return (uint8_t)((handle->status.protocol & SW3526_STRG_PROTOCOL_PD_VER_MSK) >> SW3526_STRG_PROTOCOL_PD_VER_SHIFT);
}

SW3526_Protocol SW3526_GetProtocol(const SW3526_Handle *handle)
{
    if(handle == 0)
        return SW3526_PROTOCOL_NONE;
    return (SW3526_Protocol)(handle->status.protocol & SW3526_STRG_PROTOCOL_TYPE_MSK);
}

uint16_t SW3526_ReadVINmV(const SW3526_Handle *handle)
{
    return (handle != 0) ? handle->status.vin_mv : 0u;
}

uint16_t SW3526_ReadVOUTmV(const SW3526_Handle *handle)
{
    return (handle != 0) ? handle->status.vout_mv : 0u;
}

uint32_t SW3526_ReadIOUTmA_x10(const SW3526_Handle *handle)
{
    return (handle != 0) ? handle->status.iout_ma_x10 : 0u;
}
