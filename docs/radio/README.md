# Radio 应用文档索引

本目录收 **radio 应用专属文档**（平台基座文档在 `docs/` 根，设计档案与截图在 [design/](design/README.md)）。
当前版本 **1.1.0-rc8**：13 套皮肤已接入源码并编译通过（主机回归 PASS）。

| 文档 | 内容 |
| --- | --- |
| [RADIO_OPERATIONS.md](RADIO_OPERATIONS.md) | **操作清单**：手势、页面、状态与失败表现（现行交互的权威口径） |
| [RELEASE.md](RELEASE.md) | **发布手册**：编译 → 打包 → 组装 dist → 平台上传全流程命令 |
| [RADIO_STATION_IDENTITY.md](RADIO_STATION_IDENTITY.md) | 电台目录来源、台名/FM 频率识别与显示规则 |
| [RADIO_KEYBOARD.md](RADIO_KEYBOARD.md) | 中央分页大键盘设计：5×3 字符区，覆盖全部 95 个可打印 ASCII |
| [RADIO_STREAMING_PLAN.md](RADIO_STREAMING_PLAN.md) | 拉流与连播改进建议（**未实施**，待办路线） |
| [DEVLOG.md](DEVLOG.md) | 重构过程史（至 2026-10-27）：病灶、关键坑、验收 |

> 历史逐版交付记录（RADIO_V3_RC1–RC7、V3_FIRMWARE_PLAN、VISUAL_TUNING、REDRAW_INTEGRATION、
> PLAN、UI_REDESIGN、ARCHITECTURE）已移除：git 历史与各 `dist/radio-<ver>/` 发布包内均有存档。
