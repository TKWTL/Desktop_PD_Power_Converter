# SW3538 驱动(双口快充控制器)

> 职责:通过共享硬件 I2C1,读取 SW3538 的协议、系统/端口状态与 VIN/VOUT/双路电流。

- 寄存器手册依据:**SW3538 Register List RG108_3 v1.2**;
- 后端:`Peripheral/I2C` 异步硬件 I2C/DMA;
- 命名:沿用 SW6306 惯例 —— `STRG` = 状态/回读,`CTRG` = 控制/配置;
- 公开对象是 `SW3538_Handle`:即使本板只装一颗,也不使用全局单例。

## 已实现的状态读取路径

| 寄存器 | 内容 |
|---|---|
| `0x09` | 快充协议(PD 版本;QC/FCP/SCP/PD/PPS/PE/VOOC/SFCP/AFC/TFCP 编码) |
| `0x0A` | 系统状态(Buck、通路 1、通路 2) |
| `0x0D` | 设备在线状态(通路 1 / 通路 2 online) |
| `0x40` + `0x41/0x42` | 锁存 ADC:VIN、VOUT、通路 1 电流、通路 2 电流 |

## 接口要点

- `SW3538_StatusLoad()` / `SW3538_ADCLoad()` 等为协程式函数,内部自行分步等待 I2C;
- `SW3538_ADCLoad()` 会依次执行:写使能序列 → force 解锁 → 打开 VIN/VOUT/电流采样,再读数;
- 量程:VIN 10 mV/bit,VOUT 6 mV/bit,两路电流 2.5 mA/bit(5 mΩ 采样电阻);
- 解码结果缓存在句柄的 `status` 中,由 `SW3538_ReadVINmV()` 等访问器暴露。

## 注意事项

- 目前只有状态/遥测能力,**尚未提供逐端口的协议使能/配置 API**(`CTRG` 寄存器已定义);
- 硬件 I2C1 为多属主共享总线,事务期间可能被其它 `I2C_API_OWNER_*` 占用。
