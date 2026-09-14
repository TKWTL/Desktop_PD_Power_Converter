# Images

仓库图片素材按用途分目录:

| 目录 | 用途 | 被引用于 |
|---|---|---|
| `Product/` | 成品外观、接口、点亮界面 | 根 `README.md`、`Docs/USER_MANUAL.md` |
| `Assembly/` | 装配过程照片(按装配顺序) | `Docs/ASSEMBLY_GUIDE.md` |
| `Testing/` | 测试环境、波形、温升曲线 | `Docs/TEST_REPORT.md` |

命名建议:

- 装配类:`01-pcb-set.jpg`、`02-bare-pcb.jpg`、`06-oled.jpg` …按流程编号;
- 成品类:`hero-front.jpg`、`hero-back.jpg`、`ports.jpg`、`dashboard-on.jpg`;
- 测试类:`pd-epr.png`、`thermal-step.png`、`fan-curve.png`、`ripple-full.png`。

约定:

- 文档中的图片占位符已写好在对应章节,放入同名文件即可显示;
- 建议单张 ≤ 500 KB(长边 ≤ 2000 px),避免仓库过大;
- 测试原始截图/多组波形不必全部入库,只保留文档引用的精选图。
