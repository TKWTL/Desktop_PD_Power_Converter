/* GX21M15/GX21M15U digital temperature sensor driver.
 * LM75/LM75A-compatible register map, 11-bit / 0.125 C temperature data.
 */
#ifndef __GX21M15_H__
#define __GX21M15_H__

#include <stdint.h>
#include "coroOS.h"
#include "i2c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Strap pins on the main board: A2/A1/A0 are all tied to 3V3 (netlist
 * "3V3 ; ... U11.5 U11.6 U11.7 U11.8"), so the 7-bit address is
 * 0x48 | 0b111 = 0x4F.  The valid strap window stays 0x48..0x4F. */
#define GX21M15_I2C_ADDR_DEFAULT       0x4FU

#define GX21M15_REG_TEMP               0x00U
#define GX21M15_REG_CONFIG             0x01U
#define GX21M15_REG_THYST              0x02U
#define GX21M15_REG_TOS                0x03U

#define GX21M15_CONFIG_SHUTDOWN        0x01U
#define GX21M15_CONFIG_OS_INT_MODE     0x02U
#define GX21M15_CONFIG_OS_POL_HIGH     0x04U
#define GX21M15_CONFIG_FAULT_QUEUE_MSK 0x18U

typedef enum
{
    GX21M15_IO_IDLE = 0,
    GX21M15_IO_OK,
    GX21M15_IO_NACK,
    GX21M15_IO_TIMEOUT,
    GX21M15_IO_ERROR
} GX21M15_IOResult;

typedef struct
{
    uint8_t online;
    uint8_t config;
    int16_t temperature_x8; /* signed 0.125 C units */
    int32_t temperature_mc; /* milli-Celsius */
    uint32_t sample_count;
} GX21M15_Status;

typedef struct
{
    uint8_t address;
    I2C_API_Owner i2c_owner;
    volatile GX21M15_IOResult last_io;
    GX21M15_Status status;

    coro_pt_t io_pt;
    uint8_t txbuf[3];
    uint8_t rxbuf[2];
} GX21M15_Handle;

#define GX21M15_RET              uint8_t
#define GX21M15_ARGS(...)        coro_pt_t *pt, GX21M15_Handle *handle, __VA_ARGS__
#define GX21M15_NOARG            coro_pt_t *pt, GX21M15_Handle *handle
#define GX21M15_FUNC_BEGIN       THRD_BEGIN
#define GX21M15_FUNC_END         THRD_END
#define GX21M15_UNTIL(cond)      THRD_UNTIL(cond)
#define GX21M15_RETURN_END()     do { CORO_INIT(pt); return CORO_ENDED; } while(0)

void GX21M15_HandleInit(GX21M15_Handle *handle,
                        I2C_API_Owner owner,
                        uint8_t address_7bit);

GX21M15_RET GX21M15_ByteWrite(GX21M15_ARGS(uint8_t reg, uint8_t data));
GX21M15_RET GX21M15_ByteRead(GX21M15_ARGS(uint8_t reg, uint8_t *data));
GX21M15_RET GX21M15_BytesRead(GX21M15_ARGS(uint8_t reg, uint8_t *data, uint16_t len));
GX21M15_RET GX21M15_TemperatureLoad(GX21M15_NOARG);
GX21M15_RET GX21M15_StatusLoad(GX21M15_NOARG);

const GX21M15_Status *GX21M15_GetStatus(const GX21M15_Handle *handle);
uint8_t GX21M15_IsOnline(const GX21M15_Handle *handle);
int16_t GX21M15_ReadTemperatureX8(const GX21M15_Handle *handle);
int32_t GX21M15_ReadTemperatureMilliC(const GX21M15_Handle *handle);

#ifdef __cplusplus
}
#endif

#endif /* __GX21M15_H__ */
