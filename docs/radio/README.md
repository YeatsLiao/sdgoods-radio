# Radio 应用文档索引

本目录收 **radio 应用专属文档**（平台基座文档在 `docs/` 根，设计档案与截图在 [design/](design/README.md)）。
当前源码版本以根目录 `version.txt` 为准（目前 1.1.0），共 13 套皮肤。历史测试与当前源码验收分开记录，见 [STATUS.md](STATUS.md)。

| 文档 | 内容 |
| --- | --- |
| [RADIO_OPERATIONS.md](RADIO_OPERATIONS.md) | **操作清单**：手势、页面、状态与失败表现（现行交互的权威口径） |
| [RELEASE.md](RELEASE.md) | **发布手册**：编译 → 打包 → 组装 dist → 平台上传全流程命令 |
| [RADIO_STATION_IDENTITY.md](RADIO_STATION_IDENTITY.md) | 电台目录来源、台名/FM 频率识别与显示规则 |
| [RADIO_KEYBOARD.md](RADIO_KEYBOARD.md) | 中央分页大键盘设计：5×3 字符区，覆盖全部 95 个可打印 ASCII |
| [RADIO_STREAMING_PLAN.md](RADIO_STREAMING_PLAN.md) | 拉流与连播改进建议（**未实施**，待办路线） |
| [DEVLOG.md](DEVLOG.md) | 重构过程史（至 2026-10-27）：病灶、关键坑、验收 |

> 历史逐版交付记录（RADIO_V3_RC1–RC7、V3_FIRMWARE_PLAN、VISUAL_TUNING、REDRAW_INTEGRATION、
> PLAN、UI_REDESIGN、ARCHITECTURE）由之前的整理归档；需要回查时查看 git 历史及本机保留的 `dist/radio-<ver>/`，不能假设每个版本都有完整发布包。
