# 软件 I2C(SoftI2C)

> 职责:用 GPIO 实现句柄式 I2C 主机,为两颗固定地址的 SW3526 提供独立总线。

`soft_i2c.c/.h` 是面向本板两颗 SW3526 的 GPIO 软件 I2C 主机。

## 运行方式(非阻塞协作)

1. 用 `SoftI2C_TryWrite()` / `SoftI2C_TryWriteRead()` 发起传输;
2. 每个 coroOS 调度周期调用一次 `SoftI2C_Service()`;
3. 等待 `SoftI2C_GetResult()` 不再为 `SOFT_I2C_RESULT_ACTIVE`;
4. 用 `SoftI2C_TakeResult()` 取走最终状态。

每个 `SoftI2C_Handle` 独立持有引脚与传输状态,因此两颗同为 `0x3c` 地址的
SW3526 可以并发工作且互不干扰。

## 电气行为

- 开漏模拟:驱动只拉低,高电平靠释放 GPIO 由外部上拉电阻产生;
- 时钟拉伸(clock stretching)以有限超时轮询;
- `SoftI2C_Recover()` 提供显式同步 9 时钟总线清理,仅用于启动/故障恢复;
- 兼容用的阻塞封装(`SoftI2C_Write()` 等)仍然存在,但新代码应优先使用 Try/Service 路径。
