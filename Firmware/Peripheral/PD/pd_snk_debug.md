# CH32X035 USB-PD Sink 调试记录

## 当前稳定基线

- MCU：CH32X035C8T6，USB-PD Sink。
- 已验证路径：SPR 20 V / 5 A -> EPR Mode Enter -> EPR Source Capabilities -> Fixed PDO 28 V / 5 A。
- 已连续多次插拔稳定进入 28 V；本板 PA7 分压 ADC 应确认 VBUS 约 28 V。
- EPR KeepAlive 周期：375 ms。
- 板卡由 VBUS 供电，因此本地协议恢复默认不主动发送 Hard Reset，避免 Source 关闭 VBUS 导致 MCU 自己掉电。

## 最终确认的关键问题

### 1. 自动 GoodCRC 正在发送时被重新初始化 RX

早期代码可能在 USBPD ISR 已启动自动 GoodCRC、但 TX_END 尚未完成时再次调用 RX 初始化。
`PD_ALL_CLR` 会清状态并可能截断正在发送的 GoodCRC。典型诊断是：

```text
auto-GoodCRC started/completed=6/5
```

修复后必须满足：收到普通 SOP 消息 -> ISR 自动发 GoodCRC -> GoodCRC TX_END -> 才把消息交给策略层。
`PD_Port_RxStart()` 不允许在 `auto_ack_inflight` 时重置 PHY。

### 2. 普通 SOP 发送事务不能被拆开

已验证稳定的发送顺序是：

```text
mask USBPD IRQ
  -> blocking SOP TX
  -> TX_END
  -> immediately switch PHY to RX
  -> poll GoodCRC in the WCH/C140 timing window
```

因此 `PD_Port_TransactSOP()` 是有意保留的原子 PHY 操作。
不要把 TX、RX turnaround、GoodCRC wait 拆成多个高层 API。

### 3. 调试输出改变了 USB-PD 时序

曾经在 Source_Capabilities 的 GoodCRC 与 Sink Request 之间输出 PDO、Request 内容并等待 UART，
导致 Source 对 Sink 响应超时。现象是 Request 内容本身正确，但 Source 连 GoodCRC 都不返回，随后 VBUS 降低并触发板卡 POR/PDR。

移除关键路径中的日志后，实测：

```text
Source_Capabilities GoodCRC 完成 -> 第一次 Request TX ≈ 5.1 ms
Request attempts = 1
```

随后可稳定协商 20 V，再进入 EPR 28 V。

### 4. TX_END 无界等待会冻死整个系统（2026-09-12）

`PD_Port_SendRaw(wait_complete=1)` 原来是一个无界忙等：

```c
while((USBPD->STATUS & IF_TX_END) == 0) { }
```

它跑在**主循环上下文**，而且此前已经 `NVIC_DisableIRQ(USBPD_IRQn)`。
一旦 PHY 因 CC 掉线 / 上电沿抖动（实测场景：测试源 VBUS 掉到 3.3~3.7 V、CC2 反复 attach/detach）
没能给出 TX_END，这个循环永不退出 ——
**PD 策略和 UI 一起冻死**（二者共用同一个协作式主循环），
串口日志停在最后一条打印（如 `[PD] SPR Request PDO5: ...`），按键无任何反应。

修复：

- `pd_port_wait_tx_end(PD_PORT_TX_END_TIMEOUT_US = 2000 us)`：有界等待
  （600 kbit/s 下最长 SOP 帧远小于 500 us）；
- 无论成功还是超时都执行 `pd_port_leave_tx()`：释放 `CC_LVE` → `PD_ALL_CLR` 脉冲
  → 清 `PD_TX_EN` → 重新武装 RX。避免把 PHY 留在 `CTL=06`（TX_EN|BMC_START）的卡死态；
- 超时计入 `PD_Port_PhyDiag.tx_end_timeouts`，失败日志里的 `TXTO=` 可直接看出有否发生。

### 5. EPR 进入后必须立刻发 EPR_Get_Source_Cap（2026-09-12）

现象：日志里 `EPR Mode: Enter Succeeded` 之后**紧接着**就是 `Source Hard Reset received`，
EPR 永远进不去，然后 20 V SPR ↔ 重新协商反复循环。

原因：固件在 `EPR_ST_WAIT_SOURCE_CAP` 里要等 `PD_EPR_GET_CAP_DELAY_MS = 120 ms` 才发
`EPR_Get_Source_Cap`。PD3.1 要求 Sink 在 Enter_Succeeded 后**立即**请求 EPR 源能力，
120 ms 加上期间的 printf 已经超出 Source 的 EPR 进入时限 → Source Hard Reset。

修复：`PD_EPR_GET_CAP_DELAY_MS` 120 → **2 ms**（`PD_Send_Handle()` 内部已会等
auto-GoodCRC 结束，2 ms 只是离开 ACK 窗口）。

### 6. Hard Reset 真伪判定（2026-09-12）

ISR 原来只看 `IF_RX_RESET` 就认定 Source Hard Reset。为区分「真 HR」与「RX 错误/线缆毛刺
误报」，锁存事件时同时记录：

- `ST=` → `USBPD->STATUS & MASK_PD_STAT`（真 HR = `PD_RX_SOP1_HRST`）；
- `CNT=` → `USBPD->BMC_BYTE_CNT`（真 HR ordered set 无数据对象，字节数很小）。

日志变为 `[PD] Source Hard Reset received (ST=%u CNT=%u); ...`。
若看到 `ST=2` 但 `CNT` 很大，说明是 RX 错误被误判成 HR，策略层不应因此撕掉合同。

## 已验证的 SPR Request

20 V / 5 A、PDO5、EPR-capable RDO 的首个 Request：

```text
82 10 F4 D1 47 51
```

- Header：PD3.x Request，1 Data Object，Sink，Message ID 0。
- RDO（little-endian）：`0x5147D1F4`。
- Object Position = 5。
- No USB Suspend = 1。
- EPR Mode Capable = 1。
- Operating / Max current = 5 A。

这个 RDO 已通过真实适配器验证，不应再作为首要怀疑对象。

## 健康协商日志特征

正常路径应接近：

```text
Source_Capabilities
SPR Request PDO5: 20000 mV, 5000 mA
SPR contract ready
EPR Mode: Enter Acknowledged
EPR Mode: Enter Succeeded
EPR_Source_Capabilities
EPR Request PDO8: 28000 mV, 5000 mA
EPR contract ready
EPR KeepAlive period=375 ms
PA7 VBUS ADC ~= 28000 mV
```

## 代码分层约束

### `pd.c`：协议 / 策略层

负责：

- Type-C attach 后的 Sink 状态推进。
- SPR PDO 解析与 Fixed PDO 选择。
- Request / Accept / PS_RDY 状态。
- EPR Mode Enter、EPR Source Capabilities、28 V Request。
- EPR KeepAlive。
- VBUS detach、Soft Reset、超时和恢复策略。

禁止直接访问 `USBPD->...`、GPIO、RCC、NVIC。

### `pd_port.c`：CH32X035 PHY / CC 层

负责：

- USBPD / CC / GPIO / RCC / NVIC 寄存器。
- RX DMA。
- USBPD ISR。
- 自动 GoodCRC。
- 异步 TX 引擎（`PD_Port_StartTx()` + USBPD IRQ + deadline/重发）。
- Hard Reset 物理发送。

除必须保证微秒级连续时序的 PHY 操作外，尽量保持小函数和清晰接口。

## 永久规则

1. **PD sender-response 关键路径禁止 `printf()` / `Debug_Flush()`。**
2. **关键路径禁止 I2C、OLED/UI、其他低优先级外设、scheduler yield、毫秒延时。**
3. 自动 GoodCRC 尚未 TX_END 时，不得 `PD_ALL_CLR` 或重新启动 RX。
4. 普通 SOP TX 必须通过 `PD_Port_TransactSOP()`；不要重新引入“Send + WaitGoodCRC”分裂接口。
5. 收到 Source Hard Reset 要先软件锁存，再清硬件 flag，避免下一次 `PD_ALL_CLR` 抹掉证据。
6. Source-originated Hard Reset 会使 VBUS 掉向 vSafe0V；VBUS 供电板会因此 POR/PDR，不能简单当作 MCU 软件崩溃。
7. 正常运行只打印阶段性状态；raw frame、GoodCRC 计数、ack->TX 等详细信息只在 TX 失败时输出。
8. 修改 PD PHY 后先验证 SPR 5/20 V，再验证 EPR 28 V 和 KeepAlive，最后做多次热插拔。
9. **PHY 层所有硬件等待都必须有界。** TX_END 用 2 ms 上限；超时后必须把 PHY 清回 RX
   （`pd_port_leave_tx()`）。禁止在主循环上下文写无界 `while(硬件 flag)` —— 那会同时冻死 PD 与 UI。
10. **PD 已 attach 时主循环不得降速。** WFI 空闲钩子必须由 `PD_WantsFastPoll()` 门控：
    1 ms 休眠粒度会把 Source_Capabilities → Request 的 SenderResponse 从 ~5 ms 拉到 ~31 ms，
    Source 直接不回 GoodCRC（典型日志：`ack->TX≈31 ms` + `no RX frame observed`）。
11. **EPR：收到 `EPR_Mode Enter_Succeeded` 后必须立刻发 `EPR_Get_Source_Cap`**（数 ms 级），
    中间不得插 printf/延时，否则 Source 直接 Hard Reset（见“最终确认的关键问题 5”）。
12. **EPR KeepAlive 从 `Enter_Succeeded` 就要开始值守**，不能等 28 V EPR 合同成立：
    实测充电器在 Enter_Succeeded 后 ≈500 ms 收不到 KeepAlive 就 Hard Reset，而
    “进 EPR → 等 caps → EPR_Request”这段本来就可能超过 500 ms。实现上门控改为
    `PD_EPR_ModeActive`（原为 `EPR_ST_ACTIVE`）。
13. **EPR caps 超时 1200 ms**（原 500 ms 会把慢 Source 的 caps 掐死在半路）。
14. 诊断盲区补打印：`[PD] RX VDM:`（0x0F 厂商自定义消息原始 DO）、`[PD] RX Not_Supported`、
    `[PD] EPR Mode: unhandled action`。这三种报文以前完全静默，是“对方 ACK 了却像没回”的元凶级盲区。
15. **IWDG + 检查点 + `[STUCK]` 上报**：合作式调度器里任一线程死等 = 整机冻结（UI 也不刷新）。
    主循环 600 ms 无推进 → SysTick 钩子用轮询 UART 打印 `[STUCK] cp=N`（卡死点编号：
    1 idle / 11 PD / 12 VBUS / 21-22 UI / 31-33 soft-I2C / 41 I2C watchdog / 51-53 SW35xx），
    随后 IWDG（约 1–3 s）复位；下次启动打印 `[RESET] cause: … IWDG` 与 `[DBG] checkpoint retained=N`。
    排查“卡死”先看 `[STUCK]`（或复位后的这两行）。⚠️ 检查点存 `.noinit`，必须排在 `_sbrk` 堆起点之前
    （newlib 第一次 printf 会 malloc stdio 缓冲，曾把检查点冲掉）。
16. **EPR 每个电源会话只尝试一次**（2026-09-12）：Source 在 EPR 入口后中止（典型是立即 Hard Reset）时，
    `PD_EPR_FailedForAttach` 保持置位，直到 VBUS 连续低于 3.5 V ≥ 1 s（= 人工重新插拔）才允许再试；
    ⚠️ 两处置位点：EPR 失败路径（`PD_EPR_Fallback`）**以及** EPR 进行中收到的 Hard Reset；
    `PD_PHY_Reset()` 不再清这个标志——否则 HR 后都会重启 EPR 尝试，
    形成 SPR→EPR→HR→SPR 的电源抖动循环（实测就是这个充电器的行为）。
17. **PD PHY 异步事件引擎（2026-09-12 重构）**：`PD_Port_StartTx()` → USBPD 专用 DMA → USBPD IRQ
    （TX_END→RX 转向→GoodCRC 匹配）→ `PD_Port_GetTxResult()`；`PD_Port_Service()` 负责 deadline 与
    3 次重发；**禁止再引入任何 TX 忙等/轮询**。RX：ISR 内先回 GoodCRC，ACK 发完后整包拷入 mailbox
    （`PD_Port_Init` 传入的 `PD_Rx_Buf`）。`pd.c` 侧用 `PD_TxJob` + `PD_TxCompletion_Proc()` 统一
    分发成功/失败；“Get_Source_Cap sent / EPR_Get_Source_Cap sent”现在在收到 GoodCRC 后打印。

## 回归测试

每次修改 `Peripheral/PD/` 后至少验证：

- 冷启动后插入适配器可到 28 V。
- 连续拔插至少 10 次，均能重新建立合同。
- 28 V 保持运行至少数分钟，不因 KeepAlive 超时退出 EPR。
- 周期性 ADC/I2C/UI 任务不得阻塞 PD 关键时序。
- 拔出后能由 VBUS 下降正确清合同并重新进入 attach detection。
- TX 失败诊断中 `auto-GoodCRC started/completed` 不应长期出现 started > completed。
- TX 异常（拔 CC、测试源掉压）后系统不应死机：UI 仍刷新、日志能继续输出，
  且 `PHY diag` 的 `TXTO` 只在真的发生 TX_END 超时时递增。
- 空闲钩子/WFI 改动后复测 `ack->TX`：带负载协商时应回到 ~5 ms 量级（而非 ~31 ms）。
- EPR：`Enter Succeeded` 后应看到 `EPR_Get_Source_Cap sent`，且不应紧跟 `Source Hard Reset`。
- EPR 等 caps 期间应周期性出现 `EPR KeepAlive sent while waiting for EPR caps`，
  随后或是 `EPR_Source_Capabilities`，或是明确的超时原因；不再出现异常 HR。
- `[PD] RX VDM:` 会记录充电器 VDM 的原始 DO（用于判断它是否在等 Discover Identity 应答）。
- 长时间运行不应出现 `[RESET] cause: … IWDG`；若出现，记录同一次启动里的 `[DBG] last checkpoint`。
- 异步 TX 改造后：`Get_Source_Cap sent` / `EPR_Get_Source_Cap sent` 必须出现在 GoodCRC 之后；
  失败时应先看到 `[PD] TX failed (job=…)` 再看到对应的恢复行；拔线/测试源掉压时
  `TXTO` 仅在实际 TX_END 超时时递增。
- HR 日志里的 `ST=`/`CNT=`：真 HR 应为 `ST=2` 且 `CNT` 很小。

## 备注

这份记录只保留已被当前硬件和稳定日志验证过的结论。后续如果再次出现 PD 问题，优先检查时序和 PHY 事件生命周期，不要先在 EPR RDO 或应用任务上大范围改动。
