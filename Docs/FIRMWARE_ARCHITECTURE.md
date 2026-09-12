# Firmware architecture

```text
APP
  main -> app_tasks -> coroOS
       |-> PD task --------------------> Peripheral/PD
       |-> VBUS ADC task --------------> BSP/VBUS
       |-> UI task (MiaoUI) -----------> APP/MiaoUI -> BSP/Display -> Peripheral/SPI
       |-> I2C watchdog task ----------> Peripheral/I2C
       |-> Soft-I2C service -----------> Peripheral/SoftI2C (x2 handles)
       |-> SW3538 mirror task ---------> BSP/SW3538 -> hardware I2C
       |-> SW3526 #1 mirror task ------> BSP/SW3526 -> SoftI2C #1
       |-> SW3526 #2 mirror task ------> BSP/SW3526 -> SoftI2C #2

BSP
  board       pin-safe startup + factory-ISP reboot API
  VBUS        PA7 ADC measurement
  SW3538      protocol/port/ADC driver, handle based
  SW3526      protocol/port/ADC driver, multi-handle safe
  Display     SH1107 transport, IRQ-driven full-frame flush
  Fan         PB9 / TIM1_CH1 100 kHz PWM
  Buttons     K1/K2 debounce + edge/DAS state machine

Peripheral
  PD          SPR/EPR policy + CH32X035 USBPD port
  I2C         shared PA13/PA14 interrupt-driven DMA master + watchdog recovery
  SoftI2C     independent non-blocking GPIO I2C state machines
  SPI         remapped SPI1 TX + DMA1 CH3, hardware NSS chip select
  Time        free-running µs counter + 1 ms SysTick tick IRQ
  USART       PB10/PB11 asynchronous DMA console

Project
  Core        WCH device/SPL + system/core files
  Debug       self-contained printf/rand/sbrk runtime glue (no newlib stdio/float)
  Scheduler   stackless coroOS
  Startup     vector/startup assembly
  Ld          62 KiB Flash / 20 KiB RAM linker layout
```

## Scheduling

1. PD runs every scheduler pass.
2. VBUS is sampled every 20 ms and fed into the PD policy engine.
3. The SH1107 page chain and every hardware-I2C transaction are interrupt
   driven: the SPI TX-DMA completion interrupt advances the display pages, and
   the I2C1 event/error interrupts plus the TX/RX DMA interrupts carry a
   transaction from START to STOP. No scheduler service polls them.
4. A single 10 ms coroutine still owns the I2C watchdog: transaction timeout
   detection, bus recovery and the rare deferred START when the bus was busy.
5. Both software-I2C handles advance one timing edge/state per scheduler pass.
   GPIO-I2C therefore never busy-waits during normal SW3526 transactions.
6. SW3538 and both SW3526 mirrors are refreshed cooperatively at 500 ms cadence.
7. Every pass ends in `APP_Tasks_Idle()`: the core executes `WFI` unless a
   software-I2C transfer is in flight (its edges advance once per pass, so a
   1 ms sleep would stretch a byte by two orders of magnitude). The 1 ms
   SysTick tick and every peripheral interrupt wake it, so the timed
   coroutines, the PD poll and the UI keep their cadence while the core is
   idle. The millisecond API itself is maintained by that tick interrupt
   (`TIME_TickHandler()`), while the microsecond API keeps reading the
   free-running counter.

All data required after a coroutine yield is stored either in the device handle
or in static task storage. Device drivers do not use a single global status
object, which is essential for the two SW3526 instances.

## Driver register naming

The SW3538/SW3526 headers intentionally mirror the existing SW6306 style:

- `*_STRG_*` — status/readback register;
- `*_CTRG_*` — control/configuration register;
- `*_ADC_SET_*` — ADC channel selector values;
- `*_StatusTypedef` — decoded/cached register mirror attached to a handle.

## Factory ISP reset

`Board_RebootToISP()` uses the CH32 factory boot-mode sequence: unlock
`FLASH->BOOT_MODEKEYR`, set `FLASH_STATR_BOOT_MODE`, clear reset-cause flags and
issue the keyed PFIC system reset. This reset-based handoff is preferred over a
direct jump into ROM while PD/I2C/DMA/application state is still active.
