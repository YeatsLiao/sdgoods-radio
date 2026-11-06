# 电台工具入口

从仓库根目录执行以下命令，Python 环境需要 Pillow，字体生成还需要本地 lv_font_conv。

- `python tools/radio/check_project_layout.py`：只读检查本目录 Python 语法、应用 CMake 源文件、相对 include、文档本地链接和运行时皮肤清单。
- `python tools/radio/check_radio_keyboard.py`：只读检查可打印字符、分页和圆屏键盘几何。
- `python tools/radio/test_radio_host.py`：需要原生 gcc，会编译并运行 LVGL、输入、播放器与 Wi-Fi 主机回归，输出到 output/radio-host-current。它不是只读静态检查，也不能替代真机。
- `python tools/radio/prepare_radio_assets.py --atlas main/assets/radio/atlas-source.png`：重写角色图片和对应 C 数组。
- `python tools/radio/prepare_radio_redraw.py`：从 docs/radio/design/skin 的确认图重新提取纸感与唱片素材，输出 C 数组和 manifest。
- `python tools/radio/prepare_radio_title_font.py`：重新生成高精度标题字体源码，保留 OFL 许可。

通用环境、构建、烧录、串口截图、字体、打包与发布工具仍位于 tools/ 根。输出截图需注明设备或离线渲染来源及对应版本；常用索引和未完成验收见 ../../docs/radio/STATUS.md。
