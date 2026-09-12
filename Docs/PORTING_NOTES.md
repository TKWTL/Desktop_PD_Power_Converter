# Porting notes from CH32X035_DemoBoard

This project reuses software architecture, not the DemoBoard hardware mapping.

| Function | DemoBoard | Desktop PD Power Converter |
|---|---|---|
| USB-PD CC | PC14 / PC15 via external front-end control | PC14 / PC15, direct board CC network |
| Hardware I2C1 | PA10 / PA11 | PA13 / PA14, `GPIO_PartialRemap1_I2C1` |
| VBUS monitor | INA226 | PA7 ADC, 75k / 6.8k divider |
| OLED | SSD1306 128x64 over I2C | 128x80 OLED over SPI; future SH1107/MiaoUI layer |
| Address-conflicting power ICs | n/a | SW3526 #1 PA4/PA3; #2 PA1/PA2 Soft-I2C |
| WS2812 | PC7 / PIOC | removed |
| PIOC SRAM | upper 4 KiB reserved | no reservation; CPU RAM restored to 20 KiB |
| Debug UART | PB10 / PB11 | PB10 TX / PB11 RX |

## Netlist-derived pin names

Functional names such as `UART_DBG_TX`, `PWR_I2C_SCL`, `SW3526_1_SDA` and
`VBUS_SENSE` are used instead of the reference project's generic `RX`, `SCL`
and other board-specific aliases. This reduces the chance of silently using the
reference PCB wiring on this PCB.

## USB-PD timing rule

Do not put `printf`, I2C transactions, UI rendering, scheduler yields or
millisecond delays inside the sender-response critical path in `Peripheral/PD`.
The port keeps SOP TX, immediate RX turnaround and GoodCRC observation atomic
for this reason.
