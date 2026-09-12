# 桌面 PD 电源转换器(Desktop PD Power Converter)

基于 **CH32X035C8T6** 的多口桌面 USB-PD 电源转换器固件。本仓库沿用经过验证的
`TKWTL/CH32X035_DemoBoard` 软件架构,BSP 层则按本板自己的网表整体重写,
不复用 DemoBoard 的引脚映射。

## 主要功能

把 **直流(DC)输入或 Type-C PD 输入**,转换为 **4 路 USB 输出**,
整机输出能力最高 **65 W + 65 W + 140 W**。

| 功能 | 说明 | 当前状态 |
|---|---|---|
| 输入转换 | 支持 DC 输入或 Type-C PD 输入;Type-C 通路由 CH32X035 原生 USB-PD PHY 以 Sink 身份协商(SPR 最高 20 V → EPR 目标 28 V / 5 A) | 已实现 |
| 4 路 USB 输出 | 由 SW3538 双口(PT1/PT2)+ 两颗 SW3526 单口驱动,合计最高 65 W + 65 W + 140 W | 驱动/遥测已实现,分配策略规划中 |
| 屏幕显示 | 0.78 英寸 SH1107 OLED(SPI1 12 MHz + DMA1 CH3 + 硬件 NSS),MiaoUI 首版界面:欢迎页 / 设置页 / 关于页 | 已实现 |
| 端口状态检测 | 端口在线状态、快充协议类型、VIN / VOUT / IOUT 遥测(500 ms 轮询镜像) | 已实现 |
| 温度检测 | GX21M15U 温度传感器(硬件 I2C1,与 SW3538 同总线) | 驱动待实现 |
| 自动风扇 | PB9 / TIM1_CH1 100 kHz PWM 调速 | PWM 驱动已实现,温度联动策略规划中 |
| 自动功率分配 | 根据输入能力与各端口需求动态分配输出功率 | 规划中 |
| 端口协议使能 | 通过 `CTRG` 寄存器逐端口使能/配置快充协议 | 规划中(寄存器已定义) |

## 当前固件基线(已实现)

- USB-PD Sink 策略:SPR 协商、USB PD 3.1 EPR 进入与 28 V Fixed EPR 请求路径,继承自 DemoBoard 的已验证实现。
- 时间敏感的 USB-PD TX / RX 换向 / GoodCRC 处理封装在 CH32X035 PD 端口层内部。
- 协作式 `coroOS` 前台调度器(无栈协程,当前 8 个任务);SPI 显示与硬件 I2C 事务由中断推进。
- USART1 PB10/PB11,921600 波特率,DMA 异步收发。
- 硬件 I2C1 400 kHz,位于 **PA13/PA14**(`I2C1_RM=001`),供 SW3538 与 GX21M15U 使用;
  事务由 I2C EV/ER + DMA TX/RX 中断推进,10 ms 看门狗负责超时与总线恢复。
- 两路独立 GPIO 软件 I2C 总线(PA4/PA3、PA1/PA2),分别连接两颗固定地址 `0x3c` 的
  SW3526;每条总线、每个设备都有独立句柄/状态,两颗 `0x3c` 可并发工作。
- SW3538 与 SW3526 驱动均支持 ADC 采集、快充协议解码和端口/在线状态回读,
  寄存器命名沿用 SW6306 的 `STRG` / `CTRG` 风格。
- `Board_RebootToISP()`:固件 API,直接复位进入 CH32X035 出厂 ISP 引导。
- VBUS 检测:PA7 / ADC A7,使用原理图的 75 kΩ / 6.8 kΩ 分压,结果参与 PD 掉线判断。
- CPU 可用完整 **20 KiB SRAM**;PIOC 未启用、也未保留。
- 0.78 英寸 SH1107 OLED,经重映射 SPI1(12 MHz)+ DMA1 CH3 驱动:PA11 SCK /
  PA10 MOSI / PA12 硬件 NSS(片选)/ PA9 D/C;整帧分页由 DMA 完成中断推进。
  产品界面为逻辑 **128×80**;控制器后端是 80×128 原生 TK078F288 布局,旋转为横屏使用。
- MiaoUI 首版界面:欢迎、设置、关于三页。K1 为 DOWN,K2 为 ENTER,
  两路输入低电平有效,使用 CH32X035 内部上拉。

## 未从 CH32X035_DemoBoard 携带过来的部分

参考板使用了本硬件上不存在的器件,本移植刻意移除:

- INA226 支持;
- SSD1306 支持及旧的 I2C OLED / u8g2 胶水;
- WS2812 支持;
- 整个项目专用的 PIOC 运行时及其 4 KiB SRAM 预留。

被移除的 SSD1306 传输层不会被复用。实际显示屏是 0.78 英寸 SH1107 SPI OLED,
位于 PA10/PA11/PA12(PA9 为 D/C),现已作为独立的 SH1107 / u8g2 / MiaoUI 层实现。
OLED 的 VPP 由板卡直接提供,因此 SH1107 内部 DC-DC/电荷泵保持关闭(`0xAD, 0x8A`)。

## 文档索引

| 文档 | 内容 |
|---|---|
| `Docs/FIRMWARE_ARCHITECTURE.md` | 固件分层与调度总览 |
| `Docs/PORTING_NOTES.md` | 与 DemoBoard 的移植对照(引脚/器件差异) |
| `Firmware/README.md` | 固件目录结构、模块清单与驱动状态 |
| `Firmware/APP/MiaoUI/README.md` | UI 框架(菜单/控件/输入适配) |
| `Firmware/BSP/Display/README.md` | SH1107 显示传输与 DMA 刷屏 |
| `Firmware/BSP/Fan/README.md` | 风扇 PWM 驱动 |
| `Firmware/BSP/Buttons/README.md` | 双按键消抖/边沿状态机 |
| `Firmware/BSP/SW3538/README.md` | SW3538 双口快充控制器驱动 |
| `Firmware/BSP/SW3526/README.md` | SW3526 单口快充控制器驱动(双实例) |
| `Firmware/Peripheral/PD/README.md` | USB-PD 策略 / PHY 分层与调试要点 |
| `Firmware/Peripheral/I2C/README.md` | 硬件 I2C1 异步 DMA 服务与恢复 |
| `Firmware/Peripheral/SoftI2C/README.md` | 软件 I2C 引擎 |
| `Firmware/Peripheral/SPI/README.md` | 显示 SPI + DMA 传输 |
| `Firmware/Peripheral/USART/README.md` | 调试串口(DMA 异步) |
| `Firmware/ThirdParty/u8g2/README.md` | 精简 u8g2 图形库与 SH1107 后端 |

## 构建

1. 用 MounRiver Studio II 打开 `Firmware/Desktop_PD_Power_Converter.wvproj`。
2. 应用结构性改动后先执行 **Clean Project**。
3. 构建工程;产物输出到 `Firmware/Project/obj/`。

`.project`、`.cproject` 与 `.wvproj` 属于构建元数据,特意纳入版本管理;
`.mrs`、launch 文件及生成对象被忽略。

## 安全与现状

28 V 通路是 USB-PD **EPR**,不是普通 SPR。不要仅凭策略状态显示 28 V 就打开
大功率负载级:量产固件应交叉校验实测 VBUS、功率级状态与温度/保护条件。
当前固件建立了板级平台、SW3538/SW3526 遥测驱动以及首版 SH1107/MiaoUI 界面;
完整的功率分配策略仍属于后续层。

## 许可

项目自写代码:**GNU AGPL-3.0-only**。WCH 厂商库的例外/声明边界见 `LICENSE`
与 `THIRD_PARTY_NOTICES.md`。

## 芯片手册与参考资料

### 芯片数据手册

| 芯片 | 用途 | 资料 |
|---|---|---|
| CH32X035C8T6 | 主控 MCU(RISC-V4C,内置 USB + PD PHY) | [沁恒产品页](https://www.wch.cn/products/CH32X035.html) · [数据手册 CH32X035DS0](http://www.wch.cn/downloads/CH32X035DS0_PDF.html) · [参考手册 CH32X035RM](https://www.wch.cn/downloads/CH32X035RM_PDF.html) · [EVT 例程包](https://www.wch.cn/downloads/CH32X035EVT_ZIP.html) |
| SW3538 | 双口快充输出控制器(PT1/PT2) | [智融科技官网](https://www.ismartware.com/)(数据手册见官网「产品中心」) · [双口快充方案资料(PDF)](https://www.ismartware.com/upload/goods/20220811/202208111603413353.pdf) |
| SW3526 | 单口快充输出控制器(两颗,软件 I²C) | [SW3526 数据手册(PDF,智融科技)](https://www.ismartware.com/upload/goods/20220721/202207211750013134.pdf) |
| SH1107 | 0.78" OLED 显示控制器(Sino Wealth) | [SH1107 数据手册 V2.3(PDF)](https://files.waveshare.com/upload/1/16/SH1107V2.3.pdf) |
| GX21M15U | I²C 温度传感器(中科银河芯) | [产品页](https://www.gxcas.com/en/prodetail.html?id=281) · [数据手册 V2.3(PDF)](https://gxcas.com/uploads/files/202509/GX21M15_%E6%95%B0%E6%8D%AE%E6%89%8B%E5%86%8C_V2.3_20250919110520.pdf) |

### 参考项目

- [0wQ/CH32X035-PD-Tester](https://github.com/0wQ/CH32X035-PD-Tester) — 同款 CH32X035 PHY 的 USB-PD 3.2 Sink 实现(SPR / EPR Fixed / SPR AVS / EPR AVS 与 MIPPS);本工程 EPR 调优时的协议行为与位域对照参考。
- [TKWTL/CH32X035_DemoBoard](https://github.com/TKWTL/CH32X035_DemoBoard) — 本工程软件架构蓝本(PD 状态机与 CH32X035 端口层的最初版本)。
