#include "soft_i2c.h"
#include "time_api.h"

static void release_line(GPIO_TypeDef *port, uint32_t pin)
{
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = pin;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(port, &gpio);
}

static void drive_low(GPIO_TypeDef *port, uint32_t pin)
{
    GPIO_InitTypeDef gpio = {0};
    GPIO_ResetBits(port, pin);
    gpio.GPIO_Pin = pin;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(port, &gpio);
    GPIO_ResetBits(port, pin);
}

static uint8_t line_high(GPIO_TypeDef *port, uint32_t pin)
{
    return (GPIO_ReadInputDataBit(port, pin) != Bit_RESET) ? 1u : 0u;
}

static uint8_t time_reached(uint32_t now, uint32_t target)
{
    return ((int32_t)(now - target) >= 0) ? 1u : 0u;
}

static void schedule_after(SoftI2C_Handle *bus, uint32_t now)
{
    bus->next_edge_us = now + (uint32_t)bus->half_period_us;
}

static void finish_now(SoftI2C_Handle *bus, SoftI2C_Result result)
{
    release_line(bus->scl_port, bus->scl_pin);
    release_line(bus->sda_port, bus->sda_pin);
    bus->state = SOFT_I2C_STATE_IDLE;
    bus->result = result;
}

static void begin_stop(SoftI2C_Handle *bus, SoftI2C_Result result, uint32_t now)
{
    bus->pending_result = result;
    bus->state = SOFT_I2C_STATE_STOP_SDA_LOW;
    bus->next_edge_us = now;
}

static uint8_t wait_scl_high(SoftI2C_Handle *bus, uint32_t now)
{
    release_line(bus->scl_port, bus->scl_pin);
    if(line_high(bus->scl_port, bus->scl_pin))
    {
        bus->stretch_started_us = now;
        return 1u;
    }

    if((uint32_t)(now - bus->stretch_started_us) >= (uint32_t)bus->stretch_timeout_us)
    {
        finish_now(bus, SOFT_I2C_RESULT_TIMEOUT);
        return 2u;
    }
    return 0u;
}

static void tx_begin_byte(SoftI2C_Handle *bus, uint8_t byte, SoftI2C_TxStage stage, uint32_t now)
{
    bus->tx_stage = stage;
    bus->tx_byte = byte;
    bus->tx_mask = 0x80u;
    bus->state = SOFT_I2C_STATE_TX_BIT_SETUP;
    bus->next_edge_us = now;
}

static void rx_begin_byte(SoftI2C_Handle *bus, uint32_t now)
{
    bus->rx_byte = 0u;
    bus->rx_bits_left = 8u;
    release_line(bus->sda_port, bus->sda_pin);
    bus->state = SOFT_I2C_STATE_RX_SCL_HIGH_WAIT;
    schedule_after(bus, now);
}

static void after_tx_ack(SoftI2C_Handle *bus, uint32_t now)
{
    switch(bus->tx_stage)
    {
        case SOFT_I2C_TX_STAGE_ADDRESS_WRITE:
            if(bus->tx_len != 0u)
            {
                bus->tx_index = 0u;
                tx_begin_byte(bus, bus->tx[0], SOFT_I2C_TX_STAGE_DATA, now);
            }
            else if(bus->rx_len != 0u)
            {
                bus->state = SOFT_I2C_STATE_RESTART_RELEASE_SDA;
                bus->next_edge_us = now;
            }
            else
            {
                begin_stop(bus, SOFT_I2C_RESULT_OK, now);
            }
            break;

        case SOFT_I2C_TX_STAGE_DATA:
            bus->tx_index++;
            if(bus->tx_index < bus->tx_len)
            {
                tx_begin_byte(bus, bus->tx[bus->tx_index], SOFT_I2C_TX_STAGE_DATA, now);
            }
            else if(bus->rx_len != 0u)
            {
                bus->state = SOFT_I2C_STATE_RESTART_RELEASE_SDA;
                bus->next_edge_us = now;
            }
            else
            {
                begin_stop(bus, SOFT_I2C_RESULT_OK, now);
            }
            break;

        case SOFT_I2C_TX_STAGE_ADDRESS_READ:
            bus->rx_index = 0u;
            rx_begin_byte(bus, now);
            break;

        default:
            begin_stop(bus, SOFT_I2C_RESULT_ERROR, now);
            break;
    }
}

static uint8_t begin_transfer(SoftI2C_Handle *bus,
                              uint8_t address_7bit,
                              const uint8_t *tx,
                              uint16_t tx_len,
                              uint8_t *rx,
                              uint16_t rx_len)
{
    if(bus == 0 || address_7bit > 0x7Fu)
        return 0u;
    if(bus->result != SOFT_I2C_RESULT_IDLE || bus->state != SOFT_I2C_STATE_IDLE)
        return 0u;
    if(tx_len != 0u && tx == 0)
        return 0u;
    if(rx_len != 0u && rx == 0)
        return 0u;

    bus->address_7bit = address_7bit;
    bus->tx = tx;
    bus->tx_len = tx_len;
    bus->tx_index = 0u;
    bus->rx = rx;
    bus->rx_len = rx_len;
    bus->rx_index = 0u;
    bus->pending_result = SOFT_I2C_RESULT_OK;
    bus->result = SOFT_I2C_RESULT_ACTIVE;
    bus->state = SOFT_I2C_STATE_BUS_CHECK;
    bus->stretch_started_us = TIME_Micros();
    bus->next_edge_us = bus->stretch_started_us;
    return 1u;
}

void SoftI2C_Init(SoftI2C_Handle *bus)
{
    if(bus == 0)
        return;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                           RCC_APB2Periph_GPIOC, ENABLE);

    if(bus->half_period_us == 0u)
        bus->half_period_us = SOFT_I2C_DEFAULT_HALF_PERIOD_US;
    if(bus->stretch_timeout_us == 0u)
        bus->stretch_timeout_us = SOFT_I2C_DEFAULT_STRETCH_TIMEOUT_US;

    bus->state = SOFT_I2C_STATE_IDLE;
    bus->result = SOFT_I2C_RESULT_IDLE;
    bus->pending_result = SOFT_I2C_RESULT_IDLE;
    bus->recovery_count = 0u;
    release_line(bus->scl_port, bus->scl_pin);
    release_line(bus->sda_port, bus->sda_pin);
}

uint8_t SoftI2C_TryPing(SoftI2C_Handle *bus, uint8_t address_7bit)
{
    return begin_transfer(bus, address_7bit, 0, 0u, 0, 0u);
}

uint8_t SoftI2C_TryWrite(SoftI2C_Handle *bus, uint8_t address_7bit,
                         const uint8_t *data, uint16_t length)
{
    if(length == 0u)
        return 0u;
    return begin_transfer(bus, address_7bit, data, length, 0, 0u);
}

uint8_t SoftI2C_TryWriteRead(SoftI2C_Handle *bus, uint8_t address_7bit,
                             const uint8_t *tx, uint16_t tx_len,
                             uint8_t *rx, uint16_t rx_len)
{
    if(tx_len == 0u || rx_len == 0u)
        return 0u;
    return begin_transfer(bus, address_7bit, tx, tx_len, rx, rx_len);
}

void SoftI2C_Service(SoftI2C_Handle *bus)
{
    uint32_t now;
    uint8_t w;

    if(bus == 0 || bus->result != SOFT_I2C_RESULT_ACTIVE)
        return;

    now = TIME_Micros();
    if(!time_reached(now, bus->next_edge_us))
        return;

    switch(bus->state)
    {
        case SOFT_I2C_STATE_BUS_CHECK:
            release_line(bus->sda_port, bus->sda_pin);
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            if(!line_high(bus->sda_port, bus->sda_pin))
            {
                finish_now(bus, SOFT_I2C_RESULT_BUS_STUCK);
                return;
            }
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_START_SDA_LOW;
            break;

        case SOFT_I2C_STATE_START_SDA_LOW:
            drive_low(bus->sda_port, bus->sda_pin);
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_START_SCL_LOW;
            break;

        case SOFT_I2C_STATE_START_SCL_LOW:
            drive_low(bus->scl_port, bus->scl_pin);
            tx_begin_byte(bus, (uint8_t)(bus->address_7bit << 1),
                          SOFT_I2C_TX_STAGE_ADDRESS_WRITE, now);
            break;

        case SOFT_I2C_STATE_TX_BIT_SETUP:
            if((bus->tx_byte & bus->tx_mask) != 0u)
                release_line(bus->sda_port, bus->sda_pin);
            else
                drive_low(bus->sda_port, bus->sda_pin);
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_TX_SCL_HIGH_WAIT;
            bus->stretch_started_us = now;
            break;

        case SOFT_I2C_STATE_TX_SCL_HIGH_WAIT:
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_TX_SCL_LOW;
            break;

        case SOFT_I2C_STATE_TX_SCL_LOW:
            drive_low(bus->scl_port, bus->scl_pin);
            if(bus->tx_mask > 1u)
            {
                bus->tx_mask >>= 1;
                bus->state = SOFT_I2C_STATE_TX_BIT_SETUP;
                bus->next_edge_us = now;
            }
            else
            {
                release_line(bus->sda_port, bus->sda_pin);
                schedule_after(bus, now);
                bus->state = SOFT_I2C_STATE_TX_ACK_SCL_HIGH_WAIT;
                bus->stretch_started_us = now;
            }
            break;

        case SOFT_I2C_STATE_TX_ACK_SCL_HIGH_WAIT:
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_TX_ACK_SAMPLE;
            break;

        case SOFT_I2C_STATE_TX_ACK_SAMPLE:
            w = line_high(bus->sda_port, bus->sda_pin);
            drive_low(bus->scl_port, bus->scl_pin);
            if(w)
                begin_stop(bus, SOFT_I2C_RESULT_NACK, now);
            else
                after_tx_ack(bus, now);
            break;

        case SOFT_I2C_STATE_RESTART_RELEASE_SDA:
            release_line(bus->sda_port, bus->sda_pin);
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_RESTART_SCL_HIGH_WAIT;
            bus->stretch_started_us = now;
            break;

        case SOFT_I2C_STATE_RESTART_SCL_HIGH_WAIT:
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_RESTART_SDA_LOW;
            break;

        case SOFT_I2C_STATE_RESTART_SDA_LOW:
            drive_low(bus->sda_port, bus->sda_pin);
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_RESTART_SCL_LOW;
            break;

        case SOFT_I2C_STATE_RESTART_SCL_LOW:
            drive_low(bus->scl_port, bus->scl_pin);
            tx_begin_byte(bus, (uint8_t)((bus->address_7bit << 1) | 1u),
                          SOFT_I2C_TX_STAGE_ADDRESS_READ, now);
            break;

        case SOFT_I2C_STATE_RX_SCL_HIGH_WAIT:
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_RX_SAMPLE;
            break;

        case SOFT_I2C_STATE_RX_SAMPLE:
            bus->rx_byte = (uint8_t)(bus->rx_byte << 1);
            if(line_high(bus->sda_port, bus->sda_pin))
                bus->rx_byte |= 1u;
            drive_low(bus->scl_port, bus->scl_pin);
            bus->rx_bits_left--;
            if(bus->rx_bits_left != 0u)
            {
                schedule_after(bus, now);
                bus->state = SOFT_I2C_STATE_RX_SCL_HIGH_WAIT;
                bus->stretch_started_us = now;
            }
            else
            {
                bus->rx[bus->rx_index] = bus->rx_byte;
                if((uint16_t)(bus->rx_index + 1u) < bus->rx_len)
                    drive_low(bus->sda_port, bus->sda_pin);   /* ACK */
                else
                    release_line(bus->sda_port, bus->sda_pin); /* NACK */
                schedule_after(bus, now);
                bus->state = SOFT_I2C_STATE_RX_ACK_SCL_HIGH_WAIT;
                bus->stretch_started_us = now;
            }
            break;

        case SOFT_I2C_STATE_RX_ACK_SCL_HIGH_WAIT:
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_RX_ACK_SCL_LOW;
            break;

        case SOFT_I2C_STATE_RX_ACK_SCL_LOW:
            drive_low(bus->scl_port, bus->scl_pin);
            release_line(bus->sda_port, bus->sda_pin);
            bus->rx_index++;
            if(bus->rx_index < bus->rx_len)
                rx_begin_byte(bus, now);
            else
                begin_stop(bus, SOFT_I2C_RESULT_OK, now);
            break;

        case SOFT_I2C_STATE_STOP_SDA_LOW:
            drive_low(bus->sda_port, bus->sda_pin);
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_STOP_SCL_HIGH_WAIT;
            bus->stretch_started_us = now;
            break;

        case SOFT_I2C_STATE_STOP_SCL_HIGH_WAIT:
            w = wait_scl_high(bus, now);
            if(w != 1u)
                return;
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_STOP_SDA_RELEASE;
            break;

        case SOFT_I2C_STATE_STOP_SDA_RELEASE:
            release_line(bus->sda_port, bus->sda_pin);
            schedule_after(bus, now);
            bus->state = SOFT_I2C_STATE_STOP_DONE;
            break;

        case SOFT_I2C_STATE_STOP_DONE:
            bus->state = SOFT_I2C_STATE_IDLE;
            bus->result = bus->pending_result;
            break;

        default:
            finish_now(bus, SOFT_I2C_RESULT_ERROR);
            break;
    }
}

SoftI2C_Result SoftI2C_GetResult(const SoftI2C_Handle *bus)
{
    return (bus != 0) ? bus->result : SOFT_I2C_RESULT_ERROR;
}

SoftI2C_Result SoftI2C_TakeResult(SoftI2C_Handle *bus)
{
    SoftI2C_Result result;
    if(bus == 0)
        return SOFT_I2C_RESULT_ERROR;
    result = bus->result;
    if(result != SOFT_I2C_RESULT_ACTIVE)
        bus->result = SOFT_I2C_RESULT_IDLE;
    return result;
}

uint8_t SoftI2C_IsBusy(const SoftI2C_Handle *bus)
{
    return (bus != 0 && bus->result == SOFT_I2C_RESULT_ACTIVE) ? 1u : 0u;
}

uint32_t SoftI2C_GetRecoveryCount(const SoftI2C_Handle *bus)
{
    return (bus != 0) ? bus->recovery_count : 0u;
}

uint8_t SoftI2C_Recover(SoftI2C_Handle *bus)
{
    uint8_t i;
    if(bus == 0 || SoftI2C_IsBusy(bus))
        return 0u;

    release_line(bus->sda_port, bus->sda_pin);
    release_line(bus->scl_port, bus->scl_pin);

    for(i = 0u; i < 9u && !line_high(bus->sda_port, bus->sda_pin); ++i)
    {
        drive_low(bus->scl_port, bus->scl_pin);
        TIME_DelayUs(bus->half_period_us);
        release_line(bus->scl_port, bus->scl_pin);
        TIME_DelayUs(bus->half_period_us);
    }

    drive_low(bus->sda_port, bus->sda_pin);
    TIME_DelayUs(bus->half_period_us);
    release_line(bus->scl_port, bus->scl_pin);
    TIME_DelayUs(bus->half_period_us);
    release_line(bus->sda_port, bus->sda_pin);
    TIME_DelayUs(bus->half_period_us);

    bus->recovery_count++;
    bus->state = SOFT_I2C_STATE_IDLE;
    bus->result = SOFT_I2C_RESULT_IDLE;
    return (line_high(bus->scl_port, bus->scl_pin) &&
            line_high(bus->sda_port, bus->sda_pin)) ? 1u : 0u;
}

static uint8_t wait_blocking(SoftI2C_Handle *bus)
{
    SoftI2C_Result result;
    while(SoftI2C_GetResult(bus) == SOFT_I2C_RESULT_ACTIVE)
    {
        SoftI2C_Service(bus);
        TIME_DelayUs(1u);
    }
    result = SoftI2C_TakeResult(bus);
    return (result == SOFT_I2C_RESULT_OK) ? 1u : 0u;
}

uint8_t SoftI2C_Write(SoftI2C_Handle *bus, uint8_t address_7bit,
                      const uint8_t *data, uint16_t length)
{
    if(!SoftI2C_TryWrite(bus, address_7bit, data, length))
        return 0u;
    return wait_blocking(bus);
}

uint8_t SoftI2C_WriteRead(SoftI2C_Handle *bus, uint8_t address_7bit,
                          const uint8_t *tx, uint16_t tx_len,
                          uint8_t *rx, uint16_t rx_len)
{
    if(!SoftI2C_TryWriteRead(bus, address_7bit, tx, tx_len, rx, rx_len))
        return 0u;
    return wait_blocking(bus);
}
