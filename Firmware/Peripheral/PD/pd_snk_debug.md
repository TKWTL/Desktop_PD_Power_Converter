# CH32X035 USB-PD Sink 设计与调试要点

## 当前稳定基线

- MCU：CH32X035C8T6，USB-PD Sink。
- 已验证路径：SPR 20 V / 5 A -> EPR Mode Enter -> EPR Source Capabilities -> Fixed PDO 28 V / 5 A。
- 已连续多次插拔稳定进入 28 V；本板 PA7 分压 ADC 应确认 VBUS 约 28 V。
- EPR KeepAlive 周期：375 ms；全部 EPR 时序宏集中在 `pd.c` 顶部。
- 板卡由 VBUS 供电，因此本地协议恢复默认不主动发送 Hard Reset，避免 Source 关闭 VBUS 导致 MCU 自己掉电。
- **2026-09-14 起冻结 `pd.c` / `pd_port.c` 逻辑**：只允许注释/文档调整，禁止继续“优化” PD。

## 关键设计点（不要退化）

1. **普通 SOP 发送必须保持原子**：`PD_Port_TransactSOP()` = 屏蔽 USBPD IRQ → SOP TX →
   等 `IF_TX_END` → 立即转 RX → 轮询匹配的 Source GoodCRC → 重开 IRQ；重试保持同一
   Message ID。不要把 TX / RX 转向 / GoodCRC 等待拆成分裂接口。
2. **自动 GoodCRC 不能被截断**：普通 SOP 进 → ISR 30 µs 后发 ACK → ACK `TX_END` 之后
   才把消息交给策略层；`auto_ack_inflight` 期间禁止 `PD_ALL_CLR` / 重启 RX
   （`PD_Port_RxStart()` 自带门控）。退化诊断：`started/completed` 长期不闭合。

3. **sender-response 关键路径禁止任何调试输出**：在 Source_Capabilities 的 GoodCRC 与
   Request 之间打日志/等 UART 会把 Request 推出 Source 的响应窗口；去掉后实测
   `Source_Capabilities → Request ≈ 5.1 ms`、一次成功。
4. **PHY 忙等必须有界**：TX_END 等 2 ms（600 kbit/s 下 SOP 帧 < 500 µs），超时后必须把
   PHY 清回 RX（`pd_port_release_cc()` + 清 `PD_TX_EN`），并计入 `PhyDiag.tx_end_timeouts`
   （失败日志 `TXTO=`）。曾因无界忙等 + PD IRQ 已屏蔽 → PD 与 UI 整机冻死。

5. **EPR 入口后的 Get 由策略循环发出**：`Enter_Succeeded` 进入 `WAIT_SOURCE_CAP`，任务侧
   经 `PD_EPR_GET_CAP_DELAY_MS`（120 ms）后发 `EPR_Get_Source_Cap`，随后按 caps/超时推进。
   ⚠️ EPR 时序对充电器很敏感：改任何值前后都必须做完整回归；历史上这里反复出过
   “Enter Succeeded 紧跟 Source Hard Reset”的故障。

6. **Hard Reset 先锁存后处理**：ISR 记录 `ST=`（`MASK_PD_STAT`，真 HR = `PD_RX_SOP1_HRST`）
   与 `CNT=`（`BMC_BYTE_CNT`，真 HR ordered set 无数据对象、字节数很小）。`ST=2` 但 `CNT`
   很大 = RX 误报，策略层不应因此撕合同；VBUS 供电板收到合法 HR 会掉电，不是软件崩溃。

7. **已验证的 RDO 帧**（20 V/5 A、PDO5、EPR-capable 的首个 Request）：`82 10 F4 D1 47 51`
   = Header(PD3.x Request, 1 DO, Sink, MsgID 0) + RDO `0x5147D1F4`（OPOS=5、NoUSB
   Suspend=1、EPR-capable=1、5 A）。已通过真机验证，不要再当首要怀疑对象。

## 代码分层约束

### `pd.c`：协议 / 策略层

- Type-C attach 后的 Sink 状态推进；SPR PDO 解析与 Fixed PDO 选择；Request / Accept / PS_RDY。
- EPR Mode Enter、EPR Source Capabilities、28 V Request、KeepAlive。
- VBUS detach、Soft Reset、超时与恢复策略。**禁止直接访问 `USBPD->...`、GPIO、RCC、NVIC。**

### `pd_port.c`：CH32X035 PHY / CC 层

- USBPD / CC / GPIO / RCC / NVIC 寄存器、RX DMA、USBPD ISR、自动 GoodCRC。
- 原子 SOP 发送 `PD_Port_TransactSOP()`、Hard Reset 物理发送。
- 除微秒级连续时序的 PHY 操作外，保持小函数和清晰接口。

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
11. **EPR 时序以当前基线为准**（`pd.c` 顶部宏）：Get 延迟 120 ms、Source_Cap 超时 500 ms、
    Enter 超时 550 ms、KeepAlive 375 ms / ACK 超时 100 ms。改任何一项都要重新跑完整回归。
12. **EPR 每个电源会话只尝试一次**：Source 在 EPR 入口后中止时，`PD_EPR_FailedForAttach`
    保持置位；只有 VBUS 掉线（<3.5 V，3 个 5 ms 节拍去抖 = 真拔插）后的新 attach 才清零。
    置位点：`PD_EPR_Fallback` 与“EPR 进行中收到 Hard Reset”；`PD_PHY_Reset()` 不清它——
    否则会形成 SPR→EPR→HR→SPR 的电源抖动循环。
13. **诊断打印按需保留**：`[PD] RX VDM:` 记录厂商自定义消息原始 DO；异常报文（Not_Supported、
    unhandled EPR action 等）都有打印，不要让关键分支静默。
14. **IWDG 看门狗**：合作式调度器里任一线程死等 = 整机冻结（UI 也不刷新）。主循环无推进时
    IWDG（约 1–3 s）复位，下次启动的 `[RESET] cause: … IWDG` 可确认来源。

## 回归测试

每次修改 `Peripheral/PD/` 后至少验证：

- 冷启动插适配器可到 28 V；连续拔插 ≥10 次均能重新建立合同；软件复位 ≥10 次正常。
- 28 V 保持数分钟：无 `EPR KeepAlive ACK timeout`，VBUS 稳定 ≈28 V。
- 协商期间 ADC/I2C/UI 任务不得影响时间窗：`TO=0/0/0`、`ack->TX ≈5 ms`。
- 拔出后由 VBUS 下降正确清合同并重新进入 attach detection；`TXTO` 只在真 TX_END 超时时递增。
- 异常（拔 CC、测试源掉压）后系统不死机：UI 仍刷新、日志继续输出。
- 长时间运行不应出现 `[RESET] cause: … IWDG`。
- HR 日志 `ST=`/`CNT=`：真 HR = `ST=2` 且 `CNT` 很小。

## 备注

这份记录只保留已被当前硬件和稳定日志验证过的结论。后续如果再次出现 PD 问题，优先检查时序和 PHY 事件生命周期，不要先在 EPR RDO 或应用任务上大范围改动。
