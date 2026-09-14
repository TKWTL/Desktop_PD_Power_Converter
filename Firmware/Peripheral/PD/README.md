# USB-PD 策略与 PHY 层

> 职责:作为 USB-PD Sink 完成 SPR → EPR 协商;对上层只暴露策略状态与 PDO 快照。

## 分层

| 文件 | 层次 | 内容 |
|---|---|---|
| `pd.c` | 协议/策略 | Type-C attach 后的 Sink 状态推进;SPR PDO 解析与 Fixed PDO 选择;Request / Accept / PS_RDY;EPR Mode Enter;EPR Source Capabilities 分块接收;28 V Fixed Request;EPR KeepAlive;VBUS 掉线、Soft Reset 与超时恢复 |
| `pd_port.c` | PHY/CC | `USBPD`/CC/GPIO/RCC/NVIC 寄存器;原子 SOP 发送 `PD_Port_TransactSOP()`(屏蔽 IRQ → SOP TX → 等 TX_END → 立即转 RX → 轮询匹配 GoodCRC);ISR 内自动 GoodCRC(直写策略缓冲,无 mailbox);Hard Reset 物理发送 |
| `pd.h` | 策略参数 | 请求上限(SPR ≤ 20 V / 5 A;EPR 目标 28 V / 5 A;Sink PDP 140 W)与对外查询 API |

分层约束:`pd.c` 不得直接访问 `USBPD->...`、GPIO、RCC、NVIC;所有寄存器操作都封装在 `pd_port.c`。

## 对外接口

| 接口 | 说明 |
|---|---|
| `PD_Init()` / `PD_Task(now_ms)` | 初始化与每轮调度 |
| `PD_SetVbusMillivolts(mv)` | 由 VBUS 任务喂入实测输入电压,参与掉线判断 |
| `PD_IsConnected()` / `PD_IsPowerReady()` | 连接状态与供电就绪状态 |
| `PD_IsEPRContractActive()` | EPR 合同是否生效 |
| `PD_GetContractVoltageMv()` / `PD_GetContractCurrentMa()` | 当前合同电压/电流 |
| `PD_GetDisplayPDOs()` | 供 UI 展示的 PDO 快照(按 AVS > EPR Fixed > PPS > SPR 排序) |

## 时序硬规则(必须保留)

1. 普通 SOP 发送必须走 `PD_Port_TransactSOP()` 原子事务(屏蔽 USBPD IRQ → SOP TX →
   等 `IF_TX_END` → 立即转 RX → 轮询匹配的 GoodCRC);不要拆成“Send + WaitGoodCRC”分裂接口;
2. 帧正在发送时(自动 GoodCRC 或前台 TX)不得执行 `PD_ALL_CLR` 或重新启动 RX
   ——`PD_Port_RxStart()` 已按 PHY 状态自我门控;
3. 所有 PHY 忙等必须有界(TX_END 2 ms),超时把 PHY 清回 RX 并计入 `PhyDiag.tx_end_timeouts`;
4. 本板由 VBUS 供电,因此本地协议恢复默认**不主动发送 Hard Reset**(避免 Source 关闭 VBUS 导致 MCU 掉电)。

## 相关文档

- 同目录 [`pd_snk_debug.md`](pd_snk_debug.md):实测协商日志、故障现象与最终结论(中文调试记录);
- `Docs/FIRMWARE_ARCHITECTURE.md`:PD 在整体调度中的位置。
