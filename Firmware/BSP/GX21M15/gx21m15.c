#include "gx21m15.h"
#include <string.h>

static GX21M15_IOResult gx21m15_map_result(I2C_API_Result result)
{
    switch(result)
    {
        case I2C_API_RESULT_OK:      return GX21M15_IO_OK;
        case I2C_API_RESULT_NACK:    return GX21M15_IO_NACK;
        case I2C_API_RESULT_TIMEOUT: return GX21M15_IO_TIMEOUT;
        default:                     return GX21M15_IO_ERROR;
    }
}

#define GX21M15_SPAWN_IO(call_expr)                                    \
    do {                                                                 \
        CORO_INIT(&handle->io_pt);                                       \
        CORO_WAIT_UNTIL(pt, ((call_expr) == CORO_ENDED));                \
        if(handle->last_io != GX21M15_IO_OK)                             \
        {                                                                \
            handle->status.online = 0u;                                  \
            GX21M15_RETURN_END();                                        \
        }                                                                \
    } while(0)

static void gx21m15_decode_temperature(GX21M15_Handle *handle)
{
    int16_t packed;
    int16_t x8;

    /* Temp is an 11-bit two's-complement value in D15..D5.
     * The lower five bits are non-temperature bits; every valid code is an
     * exact multiple of 32, so signed division avoids implementation-defined
     * right shift of a negative value. */
    packed = (int16_t)(((uint16_t)handle->rxbuf[0] << 8) |
                       (uint16_t)handle->rxbuf[1]);
    x8 = (int16_t)(packed / 32);

    handle->status.temperature_x8 = x8;
    handle->status.temperature_mc = (int32_t)x8 * 125;
    handle->status.sample_count++;
}

void GX21M15_HandleInit(GX21M15_Handle *handle,
                        I2C_API_Owner owner,
                        uint8_t address_7bit)
{
    if(handle == 0)
        return;

    memset(handle, 0, sizeof(*handle));
    handle->address = (address_7bit >= 0x48u && address_7bit <= 0x4Fu)
                    ? address_7bit : GX21M15_I2C_ADDR_DEFAULT;
    handle->i2c_owner = owner;
    handle->last_io = GX21M15_IO_IDLE;
    CORO_INIT(&handle->io_pt);
}

GX21M15_RET GX21M15_ByteWrite(GX21M15_ARGS(uint8_t reg, uint8_t data))
{
    I2C_API_Result result;
    GX21M15_FUNC_BEGIN;

    if(handle == 0 || handle->i2c_owner == I2C_API_OWNER_NONE)
        GX21M15_RETURN_END();

    handle->txbuf[0] = reg;
    handle->txbuf[1] = data;
    GX21M15_UNTIL(I2C_API_TryWrite(handle->i2c_owner,
                                    handle->address,
                                    handle->txbuf,
                                    2u));
    GX21M15_UNTIL(I2C_API_GetResult(handle->i2c_owner) != I2C_API_RESULT_ACTIVE);
    result = I2C_API_TakeResult(handle->i2c_owner);
    handle->last_io = gx21m15_map_result(result);
    handle->status.online = (result == I2C_API_RESULT_OK) ? 1u : 0u;

    GX21M15_FUNC_END;
}

GX21M15_RET GX21M15_ByteRead(GX21M15_ARGS(uint8_t reg, uint8_t *data))
{
    I2C_API_Result result;
    GX21M15_FUNC_BEGIN;

    if(handle == 0 || data == 0 || handle->i2c_owner == I2C_API_OWNER_NONE)
        GX21M15_RETURN_END();

    handle->txbuf[0] = reg;
    GX21M15_UNTIL(I2C_API_TryWriteRead(handle->i2c_owner,
                                       handle->address,
                                       handle->txbuf,
                                       1u,
                                       data,
                                       1u));
    GX21M15_UNTIL(I2C_API_GetResult(handle->i2c_owner) != I2C_API_RESULT_ACTIVE);
    result = I2C_API_TakeResult(handle->i2c_owner);
    handle->last_io = gx21m15_map_result(result);
    handle->status.online = (result == I2C_API_RESULT_OK) ? 1u : 0u;

    GX21M15_FUNC_END;
}

GX21M15_RET GX21M15_BytesRead(GX21M15_ARGS(uint8_t reg, uint8_t *data, uint16_t len))
{
    I2C_API_Result result;
    GX21M15_FUNC_BEGIN;

    if(handle == 0 || data == 0 || len == 0u ||
       handle->i2c_owner == I2C_API_OWNER_NONE)
        GX21M15_RETURN_END();

    handle->txbuf[0] = reg;
    GX21M15_UNTIL(I2C_API_TryWriteRead(handle->i2c_owner,
                                       handle->address,
                                       handle->txbuf,
                                       1u,
                                       data,
                                       len));
    GX21M15_UNTIL(I2C_API_GetResult(handle->i2c_owner) != I2C_API_RESULT_ACTIVE);
    result = I2C_API_TakeResult(handle->i2c_owner);
    handle->last_io = gx21m15_map_result(result);
    handle->status.online = (result == I2C_API_RESULT_OK) ? 1u : 0u;

    GX21M15_FUNC_END;
}

GX21M15_RET GX21M15_TemperatureLoad(GX21M15_NOARG)
{
    GX21M15_FUNC_BEGIN;

    GX21M15_SPAWN_IO(GX21M15_BytesRead(&handle->io_pt,
                                        handle,
                                        GX21M15_REG_TEMP,
                                        handle->rxbuf,
                                        2u));
    gx21m15_decode_temperature(handle);

    GX21M15_FUNC_END;
}

GX21M15_RET GX21M15_StatusLoad(GX21M15_NOARG)
{
    GX21M15_FUNC_BEGIN;

    GX21M15_SPAWN_IO(GX21M15_ByteRead(&handle->io_pt,
                                       handle,
                                       GX21M15_REG_CONFIG,
                                       &handle->status.config));
    GX21M15_SPAWN_IO(GX21M15_BytesRead(&handle->io_pt,
                                        handle,
                                        GX21M15_REG_TEMP,
                                        handle->rxbuf,
                                        2u));
    gx21m15_decode_temperature(handle);

    GX21M15_FUNC_END;
}

const GX21M15_Status *GX21M15_GetStatus(const GX21M15_Handle *handle)
{
    return (handle != 0) ? &handle->status : 0;
}

uint8_t GX21M15_IsOnline(const GX21M15_Handle *handle)
{
    return (handle != 0) ? handle->status.online : 0u;
}

int16_t GX21M15_ReadTemperatureX8(const GX21M15_Handle *handle)
{
    return (handle != 0) ? handle->status.temperature_x8 : 0;
}

int32_t GX21M15_ReadTemperatureMilliC(const GX21M15_Handle *handle)
{
    return (handle != 0) ? handle->status.temperature_mc : 0;
}
