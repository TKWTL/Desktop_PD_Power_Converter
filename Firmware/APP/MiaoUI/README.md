# MiaoUI 产品界面移植

> 职责:提供产品级菜单/页面框架,适配 128×80 横屏 OLED 与双按键输入。

本目录是对 `Firmware_0.zip` 中 MiaoUI 源码的精简移植(保留首版点亮所需部分)。

## 目录构成

| 目录/文件 | 内容 |
|---|---|
| `core/` | 页面框架与主循环(`MiaoUi_Setup()`、`ui_loop()`) |
| `display/` | 显示适配(`dispDriver`,绑定 SH1107 + u8g2) |
| `indev/` | 输入适配(按键事件 → UI 事件) |
| `widget/` | 文本与数值/参数控件 |
| `fonts/` | 唯一字库 `font_menu_main_h12w6` |
| `images/` | 仪表盘/设置/烧屏/关于四个 30×30 XBM 图标(烧屏图标为本项目自绘,其余来自素材包) |
| `ui_conf.h` | 分辨率、默认旋转 `U8G2_R1`、字体与布局常量 |

> 产品页面函数不放在本目录,而是放在 `APP/Pages/`(例如 `dashboard.c/.h`)。

## 保留的能力

- 图标页与文本页;
- 剪裁窗口;
- 矩形/边框与圆角矩形/边框图元;
- 圆、直线、像素与 XBM 图片;
- 全缓冲文本渲染;
- 横屏旋转 R1/R3(产品使用 128×80);
- 背景色、对比度、横屏翻转等设置项。

## 开机 Dashboard 页

`MiaoUi_Setup()` 把 `-Dashboard` 项直接置为运行态(`ui->menuState = UI_ITEM_RUNING`),
上电即进入函数页而不是图标菜单。页面函数放在 `APP/Pages/`(不在本目录),
保持 `ui_conf.c` 只负责菜单树:

- `Pages/dashboard.c/.h`:`Dashboard_Page()`(开机仪表盘);

- 第 1 行:温度(GX21M15U,显示为 `25.3C` 一位小数,传感器离线时 `--.-C`)、
  输入电压(`20V`,整数伏)、总功率限制(`270Wmax`;数字 3 位右对齐固定 `W` 位置,
  UVP 0 W、PD 合同 95%、DC 用
  DOWN 以 10 W 步进调节、60 W 后回绕 270 W)、PD 输入状态(`UVP`/`EPR`/`SPR`/
  `NEG`/`DC`,输入 <8 V 显示 `UVP`,PD 未连接一律 `DC`;状态文本右对齐到屏幕右缘);
- 第 2~6 行:3 列 = SW3538(A+C 汇总:共享 VOUT、两口电流之和、芯片协议)、
  SW3526#1(`C1`)、SW3526#2(`C2`);每列依次为端口名、输出电压、电流、功率、协议
  (协议行:未接设备 `NC`、接了但无快充 `5V`、否则协议名;文本在列内居中,
  SW3538 列再右移 3 px);
- 数值右对齐到列右边界(43/85/127 px);功率 >=10 W 两位小数、<10 W 三位小数
  (普通浮点 printf:MiaoUI 本身已用 `%.2f/%.3f`);
- 数据取自已刷新的器件镜像,芯片离线时该列显示 `---`;内容签名不变就不重复刷屏,
  但 `Disp_GetFlushCount()` 变化时(菜单/fade/设置页刷过帧)强制重发本页;
- 吞掉导航键(DC 状态下 DOWN 调节总功率限制),按 K2(ENTER) 返回图标菜单。
- 入页时 `Disp_SetMaxClipWindow()` 恢复全屏剪裁窗口(菜单页会留下标题/数据区剪裁)。

> 温度由 `dash_read_temperature()` 取自 `APP_GetGX21M15()` 的 500 ms 镜像
> (`GX21M15_IsOnline()` + `GX21M15_ReadTemperatureMilliC()`,四舍五入到 0.1 °C),
> UI 不直接访问 I2C。

## 服务项与设置项

菜单树(`ui_conf.c`)直接挂载,没有 "Tools" 二级菜单:

- 主菜单 `-Burn-in Test`(`Pages/service_pages.c` 的 `Burnin_Page()`):
  先显示说明(约 3 s,任意键跳过),随后整屏涂白用于烧屏观察,任意键返回菜单;
- 主菜单 `-Dino Game`(`Pages/game_dinosaur.c`,由 NUEDC_2025B Firmware_0 移植):
  小恐龙跑酷——K1(DOWN) 跳/失败后重开、K2(ENTER) 退出且**保留本次进度**
  (再次进入继续,不重开);16 ms 帧率(每两次页面调用跑一帧),每帧内跑 2 个 8 ms
  子步:世界/生成倒计时/起跳下落整体快 2 倍(跳跃轨迹与原版同形、滞空减半),
  而分数仍按原版速率每帧累加一次;仙人掌/云按原版方式直接以(可能为负的)坐标
  交给 u8g2 绘制,可正常滚出左边缘;随机数用本工程自研 `rand()/srand()`
  (以 `TIME_Millis()` 播种);
- 主菜单 `-Sleep` 图标(低功耗框架,`APP/framework/pm_ui_register.c`):**按下立即关屏**;
  设置页 `-Sleep`(紧随 `[Home]` 菜单项后)→ `[Sleep]` 页:9 档超时单选
  (No Auto Sleep…30min,默认 1 min);休眠由 `thread_pm` 推进,详见 `APP/framework/README.md`;
- 设置页(紧跟在显示类设置项之后,由 `Add_Service_Items()` 注册):
  - ` Soft Reset`:`Board_SoftReset()`(PFIC 软件复位,BOOT_MODE 不动);
  - ` Reboot to ISP`:`Board_RebootToISP()`(下一次复位进入 CH32 出厂 USB ISP);
  - 临时 `Fan Test` 项已于 2026-09-14 删除:风扇改由 `thread_fan` 按
    `APP/fan_control.c` 的曲线自动调速,后续 UI 只保留"延后风扇触发点"设置
    (调用 `FAN_Control_SetTemperatureDelay()`)。

## 输入约定

刻意只用两键:K1 = DOWN(页面导航;在数值编辑弹窗内递增数值),
K2 = ENTER/确认。Dashboard 页内 K1 被忽略,按 K2 返回图标菜单。

## 注意事项

- 本版本只链接 `font_menu_main_h12w6` 一个字库;增加字体需同时更新链接列表与显存预算。
- 全屏自绘页(如 `APP/Pages/dashboard.c`)若要缓存帧,必须用 `Disp_GetFlushCount()`
  判断“面板是否还是自己的内容”:计数变化(菜单/淡入淡出刷过帧)时必须重发；
  只看自己的内容签名会在“退出菜单再进入”时漏帧(屏幕停在 fade 残留)。
- UI 运行在 `thread_ui`,启动即初始化屏幕与按键,**不依赖 PD 状态**
  (DC 输入、5 V 普通电源或 EPR 协商失败时界面都正常显示);
  刷屏由中断驱动,不会长时间占用调度器。
