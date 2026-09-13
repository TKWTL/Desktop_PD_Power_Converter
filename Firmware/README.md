# 固件(Firmware)

本目录沿用 `CH32X035_DemoBoard` 已验证的分层组织,但全部引脚/器件映射都基于本板自己的网表。

## 目录结构

```text
APP/                 入口、任务注册、板级别名 + MiaoUI + 页面函数
  MiaoUI/            精简菜单/UI 移植,单字体 + 仪表盘/设置/关于 3 个 XBM 图标
  Pages/             页面函数(Dashboard 仪表盘 `dashboard.c/.h`)
BSP/                 板级启动 + 器件驱动
  SW3538/            硬件 I2C 双口快充控制器驱动
  SW3526/            句柄式双实例快充控制器驱动
  VBUS/              PA7 ADC 输入母线电压检测
  Fan/               PB9 / TIM1_CH1 187.5 kHz PWM 风扇调速
  Buttons/           K1/K2 低有效按键消抖/输入状态机
  Display/           SH1107 传输层 + DMA 完成中断整帧刷屏
Peripheral/PD/       USB-PD Sink / PD3.1 EPR 策略与 PHY 后端
Peripheral/I2C/      PA13/PA14 I2C1 中断驱动事务引擎 + 看门狗恢复
Peripheral/SoftI2C/  句柄式协作 GPIO-I2C 引擎(供 SW3526 使用)
Peripheral/Time/     SysTick 自由运行µs计数器 + 1 ms 节拍中断
Peripheral/USART/    USART1 DMA 异步控制台
Peripheral/SPI/      重映射 SPI1 TX + DMA1 CH3 完成中断(硬件 NSS 片选)
ThirdParty/u8g2/     精简 u8g2 核心 + SH1107 TK078F288 后端
Project/Core/        WCH 器件库/SPL 与 system 文件
Project/Debug/       自研整数 printf + rand/sbrk/延时兼容层
Project/Scheduler/   coroOS
Project/Startup/     启动汇编
Project/Ld/          链接脚本(完整 20 KiB CPU RAM)
Project/obj/         构建产物(除 `.gitkeep` 外被忽略)
```

## 协作式任务总览(`APP/app_tasks.c`)

| 任务 | 周期 | 职责 |
|---|---|---|
| `thread_pd` | 每轮 | USB-PD 协议/策略推进 |
| `thread_vbus` | 20 ms | PA7 采样输入母线电压并喂给 PD 策略 |
| `thread_ui` | 10 ms | 启动即点亮屏幕并进入 Dashboard 仪表盘(与 PD 状态无关);按键扫描 + MiaoUI 主循环 |
| `thread_soft_i2c_service` | 每轮 | 两路软件 I2C 各推进一步 |
| `thread_i2c_watchdog` | 10 ms | I2C 事务超时检测与总线恢复 |
| `thread_sw3538` | 500 ms | 状态 + ADC 镜像刷新 |
| `thread_sw3526_1/2` | 500 ms | 状态 + ADC 镜像刷新(两个独立句柄) |

> SPI 显示与硬件 I2C 事务完全由中断推进(DMA1 CH3 / I2C1 EV+ER / DMA1 CH6+CH7),
> 不再占用调度器线程;无栈协程不保存自动局部变量,
> 凡是在 `THRD_DELAY` / `THRD_YIELD` 之后还要用的数据,必须放在 static 存储或器件句柄里。
>
> 主循环每轮在 `APP_Tasks_RunOnce()` 之后调用 `APP_Tasks_Idle()`:没有需要全速推进的软件状态机时
> 执行 `WFI` 休眠,由 **1 ms SysTick 节拍**或任意外设中断唤醒。两种情况下保持全速轮询:
> 软件 I2C 传输进行中(边沿由调度器逐轮推进,休眠会把单字节拖慢两个数量级),
> 以及 `PD_WantsFastPoll()`(PD 已 attach:Source_Capabilities → Request 的响应延迟必须留在
> Source 的 ~24 ms 窗口内;实测 1 ms 休眠粒度会把 5 ms 拉长到 ~31 ms,Source 直接不回 GoodCRC)。
>
> 看门狗与卡死诊断:`APP_Tasks_Init()` 末尾使能 IWDG(LSI,约 1–3 s),仅在 `APP_Tasks_Idle()` 每轮刷新。
> 合作式调度器无法从线程死等中恢复——主循环 600 ms 无推进时,SysTick 钩子里先用轮询 UART 打出
> `[STUCK] cp=N`(卡死点编号),随后 IWDG 整机复位;下次启动打印 `[RESET] cause: … IWDG` 与
> `[DBG] checkpoint retained=N`(检查点镜像存于 `.noinit`,堆起点 `_sbrk` 在其后)。编号含义见 `APP/app_tasks.c` 顶部。

## 电源控制器驱动现状

- SW3538 通过 `Peripheral/I2C` 共享硬件 I2C1;驱动提供协议解码、端口/在线状态以及
  VIN/VOUT/双路端口电流 ADC 镜像。
- 每颗 SW3526 各自拥有独立的 `SoftI2C_Handle`,因此两个固定地址 `0x3c` 的器件
  可以同时使用,不依赖全局驱动状态。
- 器件 API 遵循 SW6306 命名惯例:`STRG` 表示状态/回读,`CTRG` 表示控制/配置;
  协程式 load 函数把解码后的状态缓存在句柄中。
- 目前两颗芯片的驱动均为“状态/遥测”型,尚不提供逐端口的协议使能/配置 API。

`Board_RebootToISP()` 选择 CH32X035 出厂 BOOT 区并执行软复位,是应用层进入
WCH ISP 模式的接口。

被移除的参考模块不是占位符:INA226、SSD1306、WS2812 与 PIOC 仍然不存在。
u8g2 只是作为刻意精简的 SH1107 图形依赖回归,不再包含旧的 SSD1306/I2C 传输层。

## 风扇 PWM 与 SH1107 显示

- PB9 使用原生 TIM1_CH1 输出 187.5 kHz PWM。PSC=0、ARR=255 使 CCR1 与 8 位占空比
  一一对应(0..255 共 256 级);`FAN_PWM_SetDuty8()` 即该量程的唯一接口。
- SH1107 使用 SPI1 重映射 10:PA11 SCK、PA10 MOSI、PA12(硬件 NSS 片选)、PA9 D/C。
  SPI 时钟 12 MHz,SPI TX 使用 DMA1 CH3,整帧分页由 DMA 完成中断推进。
- 0.78 英寸面板由 TK078F288 80×128 原生 u8g2 后端驱动。`U8G2_R1` 得到产品需要的
  128×80 横屏画布;`U8G2_R3` 是反向横屏,`Disp_SetRotationMode()` 保留 R0..R3
  供台面方向测试。
- VPP 由外部提供。屏幕初始化发送 `0xAD,0x8A`,保持 SH1107 内部 DC-DC/电荷泵关闭。
- u8g2 占用一块 1280 字节全帧缓冲,中断驱动传输层另有一块 1280 字节 DMA 快照;
  帧按 page 分批发运,像素数据传输期间不会长时间占住前台调度器。
- MiaoUI 保留剪裁窗口、圆角矩形/框、XBM 图片、圆/线图元和文本渲染;
  当前只链接 `font_menu_main_h12w6` 一个字体。
- 首屏是 **Dashboard 仪表盘函数页**(见下),图标菜单只保留设置页(背景色、对比度、
  横屏翻转)与关于页;仪表盘上导航键被忽略,按 K2(ENTER) 返回图标菜单。
  K1=DOWN,K2=ENTER,两路 GPIO 使用内部上拉、电气低有效,沿用原有消抖/长按状态机。

## 开机仪表盘(Dashboard)

`MiaoUi_Setup()` 把首项 `-Dashboard` 直接置为运行态,所以上电后不再先画图标菜单,
而是进入 128×80 全屏遥测页(`Dashboard_Page()`,位于 `APP/Pages/dashboard.c`):

```text
--C 20.0V 140W DC            <- 温度(GX21M15U 未接入,暂占位) / 输入电压 / 功率限制 / PD 状态
      A+C       C1      C2 <- 第二行:各块端口名
   20.00V   20.00V   20.00V <- 第三行:输出电压(两位小数)
   1.250A   0.500A   0.000A <- 第四行:输出电流(三位小数)
   25.00W   10.00W    0.00W <- 第五行:输出功率(>=10 W 两位小数,<10 W 三位小数)
      PPS        PD       ---<- 第六行:协商协议
```

- 行高 12 px + 1 px 间隔:行基线 `y = 12 + 13*(行-1)`;列右边界 43/85/127 px,
  数值右对齐 —— 偏宽的数值占用 1 字符间隙,SW3538 列 7 字符可容下 `139.02W`。
- 数据全部取自 500 ms 周期的器件镜像,UI 不做 I2C 访问:
  SW3538 列 = 共享 VOUT + A/C 两路电流之和 + 芯片协议;
  两个 SW3526 列 = 各自的 VOUT/IOUT/协议。芯片离线时该列显示 `---`。
- 数值用**定点整数**格式化：真实值 = 缩放整数 / 10^decimals（`%u.%02uV` 这种写法），
  **禁止 `%f`** —— 见下方「Flash 约束与 printf 规则」；
  内容签名不变就不重复刷屏,但 `Disp_GetFlushCount()` 一旦变化(菜单/fade 刷过帧)
  就强制重发本页——只看内容签名会在“退出菜单再进入”时漏帧。

## Flash 约束与 printf 规则(2026-09-12 定下)

- **不要用 `%f` / `float` / `double`**。本板无 FPU，软浮点库全套约 8.7 KiB；
  浮点显示统一用定点整数：`snprintf(buf, n, "%u.%02uV", mv/1000u, (mv%1000u)/10u)`；
  MiaoUI 数据项用 `UI_DATA_FLOAT`（`ptr` 指向缩放整数，`decimals` 为显示小数位），
  渲染走 `UI_FmtScaled()`。
- **printf 是本工程自带的**（`Project/Debug/debug.c`）：纯整数格式化，支持
  `%% %d %i %u %x %X %c %s` + `'0'`/`'-'`/宽度；`l`/`ll` 修饰被接受但按 32 位处理
  （不要用它打印 64 位值）；单条输出上限 127 字符，超长会被截断。
- **不要调用 newlib 的 `rand()`/`assert()`/`malloc()`/`fprintf()`**：
  `rand()` 会拖进 assert→fprintf→reent stdio→malloc 整条链（实测 ~4.5 KiB）；
  随机数请用自带的 xorshift（`rand()` 已在本工程重实现，直接用即可）。
- MiaoUI 动画/PID 是 **Q10 定点**（`UI_FX()/UI_FXI()`，`ui_conf.h`）：
  调用 `UI_Animation()` 传值/取值都要带 `UI_FX()`，真正画图时再 `UI_FXI()` 转回像素；
  `kp/ki/kd` 也按 Q10 填（0.25 → 256）。
- 改完请用 map 复核：库占用（`_`/`__` 开头符号）应只剩 `_write`/`_sbrk` 与少量
  libgcc 移位辅助。

## 复位诊断(2026-09-12 新增黑匣子)

复位后启动日志会多出几行（数据存于 `.noinit`，掉电丢失、复位保留）：

```
[DBG] checkpoint retained=33        <- 上一次复位时主循环停在哪个代码区
[DBG] bb: uptime=… ms last_snap=… fault=…   <- 运行多久后复位 / HardFault 次数
[DBG] bb isr1s: pd=… spi=… ev=… er=…       <- 复位前最后 ~1 秒的各中断入口次数
[DBG] bb isr1s: i2ctx=… i2crx=… uart=…/…
```

读法：

| 现象 | 结论 |
|---|---|
| 某个 `isr1s` 值异常大（几千以上） | 该外设**中断风暴**（电平触发未清标志）——优先级高于 SysTick 时连 1 ms 节拍都会停，所以不会打 `[STUCK]`。2026-09-12 实测就是 `ev≈10^6/s`：I2C 事件中断风暴（已修，见 `Peripheral/I2C/README.md`） |
| 全为 0、`beat` 不涨、`fault=0` | 中断被关（PRIMASK）或 CPU 停在某个同步死循环；配合 `[STUCK] cp=` 定位 |
| `fault>0` | HardFault（日志里还有 `[FAULT]`）——halt 后由 IWDG 复位 |
| 全部正常、`[STUCK] cp=N` 出现过 | 主循环里同步卡住，查 checkpoint N 对应代码 |
| 全部正常、也没有 `[STUCK]` | 用 `uptime` 先确认“多久后复位”，再怀疑 WFI/节拍 |
| 出现 `[DBG] bb i2c: aborts=…` | I2C 事件中断风暴触发了熔断（`star1/star2` 就是当时没清掉的标志：bit0=SB、bit1=ADDR、bit2=BTF、bit4=STOPF） |

`DBG_TickHook()` 每 1024 ms 把中断计数快照一次，所以 `isr1s` 是“最后一秒窗口”。
新增中断处理时记得在 `APP/ch32x035_it.c` 的中断向量里加 `DBG_ISR_BUMP(…)`。

## 模块文档

- [`APP/MiaoUI/README.md`](APP/MiaoUI/README.md) — UI 框架与输入适配
- [`BSP/Display/README.md`](BSP/Display/README.md) — SH1107 显示 BSP
- [`BSP/Fan/README.md`](BSP/Fan/README.md) — 风扇 PWM
- [`BSP/Buttons/README.md`](BSP/Buttons/README.md) — 按键状态机
- [`BSP/SW3538/README.md`](BSP/SW3538/README.md) — SW3538 驱动
- [`BSP/SW3526/README.md`](BSP/SW3526/README.md) — SW3526 驱动
- [`Peripheral/PD/README.md`](Peripheral/PD/README.md) — USB-PD 策略/PHY 分层
- [`Peripheral/I2C/README.md`](Peripheral/I2C/README.md) — 硬件 I2C1 中断驱动事务引擎
- [`Peripheral/SoftI2C/README.md`](Peripheral/SoftI2C/README.md) — 软件 I2C 引擎
- [`Peripheral/SPI/README.md`](Peripheral/SPI/README.md) — 显示 SPI + DMA 完成中断 + 硬件 NSS
- [`Peripheral/USART/README.md`](Peripheral/USART/README.md) — 异步串口
- [`ThirdParty/u8g2/README.md`](ThirdParty/u8g2/README.md) — 精简 u8g2
