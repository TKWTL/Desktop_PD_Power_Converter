# 按键输入(Buttons)

> 职责:把 K1/K2 的电气电平转换成“短按/长按/释放/连击”事件,供 UI 层消费。

## 硬件映射

| 按键 | 引脚 | 索引 | 电气特性 |
|---|---|---|---|
| K1 | PA5 | `KeyIndex_Down` | 低电平有效,`Board_Init()` 打开内部上拉 |
| K2 | PA6 | `KeyIndex_Enter` | 低电平有效,同上 |

`Board_Key1Pressed()` / `Board_Key2Pressed()` 把低有效电平规范化为 `1 = 按下`。

## 主要接口

| 接口 | 说明 |
|---|---|
| `Key_Init()` | 清空按键状态 |
| `Key_DebounceService_10ms()` | 10 ms 节拍:递减消抖/长按/连击计时 |
| `Key_Scand()` | 扫描 IO 并更新状态机(建议每个 10 ms 节拍调用一次) |
| `Key_EdgeDetect(KeyIndex_t)` | 取边沿:`Rising`(按下)/`Falling`(释放)/`Holding`(长按) |
| `KEY_GetState(KeyIndex_t)` | 当前状态:`None` / `ShortPress` / `LongPress` / `Release` |
| `KEY_GetDASClick()` / `KEY_GetClickTimes()` | 连击(DAS)计数查询 |

## 参数与实现要点

- 消抖 20 ms(`DEBOUNCE_TIME_10MS = 2` 个 10 ms 节拍);
- 长按 100 个节拍(约 1 s);进入长按后重复触发(DAS):K1(DOWN)间隔 12 个节拍
  (120 ms;菜单滚动/数值调节/Dashboard 功率调节的步进速率),K2(ENTER)间隔 16 个节拍(160 ms);
- 连击超时 50 个节拍(500 ms),超时后连击计数清零;
- 状态机开销恒定、不阻塞,由 `thread_ui` 每 10 ms 驱动一次。

## 注意事项

- 按键读取回调经 `board.c` 间接访问 GPIO,本模块不直接操作寄存器;
- MiaoUI 的输入适配位于 `APP/MiaoUI/indev/`,映射关系:K1 = DOWN,K2 = ENTER。
