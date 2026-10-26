# sdgoods-radio 实行方案（参考 → 重构 → 超越）

> 本文件是 [ARCHITECTURE.md](./ARCHITECTURE.md) 技术方案对应的**可执行落地计划**。
> 基调：参考现成开源实现（leo-radio）的底层链路，应用层按谷仓平台**重构**适配，
> UI 重新设计并做出**超越**参考的水平——**不是「移植」**。

## 0. 基调与红线

- **基调**：文档与后续代码遵循「参考 → 重构 → 超越」，**不是「移植」**。leo-radio 的价值
  只在于证明「WiFi 拉 HTTP 流 → esp_audio_codec 解码 → I2S 播放」这条路走得通，以及它踩过的坑；
  所有接口、数据结构、文件划分、UI 一律按谷仓平台重写。
- **红线（工程隔离）**：`sdgoods-radio` 工程**不得包含任何 leo-radio 代码 / 组件**
  （尤其其 `esp-wifi-connect`、`bsp_audio` ES8311 外挂 codec 那套）。leo-radio 仅存在于工程之外做阅读理解。
- **产品形态**：独立应用工程，自成一派，**不上架 AI Passport**。

## 1. 工程形态：独立派生应用

- 用 `tools/new_standalone_project.py` 从平台基座派生为独立固件（产物 `SDGOODS_RADIO.bin`，
  与 AVATAR 互不干扰、独立编译烧录）。
- **派生基座必须携带 `wifi_prov.c`**：裸基座 SDGOODS-ESP32S3 的 `sdgoods_wifi.c` 只有 `init + scan`，
  **没有配网状态机**；而 `wifi_prov.c`（SoftAP + 192.168.4.1 网页配网 + NVS + STA 状态机，已真机验证）
  是本项目硬依赖。因此从携带 `wifi_prov.c` 的工程（如 AVATAR）派生，直接复用配网，
  **不使用 leo-radio 的 esp-wifi-connect**。
- 派生后瘦身：`apps_registry.c` 的 `s_apps[]` 只保留电台一个 app；删除 GIF / 云端 / 录音等
  宿主专属源码与 `main/CMakeLists.txt` 对应 SRCS；可选 `--boot-direct` 开机直入电台（桌面摆件形态合理）。

## 2. 音频通路（关键技术决策）

两平台音频架构**根本不同**，不可照搬：

- leo-radio：外挂 ES8311 codec，`bsp_audio_set_format(hz,bits,ch)` 靠 esp_codec_dev 动态重配采样率/声道。
- 谷仓平台：`sdgoods_audio.c` 里 I2S0（`s_tx`）**固定 16kHz / 16bit / stereo 直推功放**；
  现有 `sdgoods_audio_play_pcm()` 是**一次性阻塞、播完即关功放**，不适合连续电台流；
  电台流多为 44.1k/48k stereo MP3/AAC，存在采样率缺口。

**方案：平台组件内「新增」流式播放 API（自有仓库，以新增方式提供，不改既有 record/play/bgm 行为契约）**

在 `components/sdgoods_board/src/sdgoods_audio.c` + `include/sdgoods_audio.h` 新增三接口
（与既有 `play_pcm` 并存）：

- `sdgoods_audio_stream_start(uint32_t hz, uint8_t ch)`：按需重配 I2S0 TX 时钟到流采样率、
  打开功放、置流模式；记录当前格式避免重复重配。
- `sdgoods_audio_stream_write(const int16_t *pcm, size_t bytes)`：把解码后的 PCM 分块喂
  `i2s_channel_write`（stereo 直发 / mono 复制双 slot）。
- `sdgoods_audio_stream_stop(void)`：淡出 → 关功放 → 恢复默认 16kHz，与 BGM / 录音互斥仲裁
  （复用 `sdgoods_audio_bgm_stop()` 的让位逻辑）。

应用层解码输出（esp_audio_simple_dec 得到原生采样率 PCM）直接喂该流式 API；
平台负责采样率落地与功放时序。**兜底方案**：若不便动 I2S 时钟，则应用层重采样到 16k 再喂
`stream_write`（音质略降）。

## 3. 目标模块划分（全部重写，命名对齐谷仓 app 规范）

```
仓库根（独立派生工程，基座携带 wifi_prov）
├── main/
│   ├── apps/ui_radio.c/.h     # 圆屏 FM 表盘 UI + 触摸手势状态机（点台/滑动切台/长按收藏/上滑返回）
│   ├── radio_player.c/.h      # HTTP 拉流(esp_http_client) → esp_audio_codec 解码 → 调平台流式 API 输出
│   ├── radio_catalog.c/.h     # Radio Browser API 客户端 + 定位 + 内置兜底列表 + 频率解析
│   ├── radio_settings.c/.h    # 音量/亮度/定时(sleep)/收藏，走 sdgoods_appdata / NVS
│   └── wifi_prov.c/.h         # 复用现有配网（不改）
├── components/
│   ├── sdgoods_board          # 平台层：新增 3 个流式播放 API（§2）
│   └── sdgoods_launcher       # 依赖自带（appdata 分区 / 控制中心）
└── tools/                     # 兜底电台列表打包（可选）
```

数据流：

```
Radio Browser JSON → radio_catalog 缓存
电台服务器 HTTP 流(MP3/AAC) → radio_player 拉流 → esp_audio_codec 解码 → PCM
  → sdgoods_audio_stream_write → I2S0 → 功放 → 喇叭
```

## 4. 分层借鉴边界（重写映射表）

| 参考点（leo-radio 思路） | 谷仓重构做法 |
|---|---|
| `radio_player` 的 generation 竞态控制、失败 N 次跳台、断流重连 | 保留同类**状态机思路**，用谷仓 FreeRTOS 任务 + `atomic` 重写；接口名 / 结构体全换 |
| HTTP 拉流缓冲（1KB in / 8KB out）、`Icy-MetaData:0`、crt_bundle 按需 attach | 按谷仓内存预算（S3 + PSRAM）重定缓冲；HTTPS 走平台 mbedtls 现状 |
| 仅接 MP3（`open_mp3_decoder`） | **超越**：esp_audio_codec 同时接 AAC，选台优先 MP3/AAC，OGG 优雅降级 |
| 从电台名解析 FM 频率（87.0–108.0，三段置信度） | 复用该**解析算法思路**独立实现，供表盘指针落点装饰（设备无收音硬件） |
| ip-api/ipwho.is 双源定位 + 城市中英映射 + clickcount 排序 | 保留定位兜底思路，按谷仓网络栈重写；镜像测速选最快 |
| 方屏平铺刻度 + 3 键（ADC）交互 | **必须重构**：全触摸 + 360×360 环形 FM 刻度表盘 + 指针沿弧扫动动画（超越点） |
| esp-wifi-connect 自建全键盘配网 | **丢弃**，改用谷仓 `wifi_prov`（网页配网 + NVS） |
| `calculate_levels` 18 段频谱驱动 UI | **超越机会**：解码 PCM 顺带算能量，做声波动效（可选，非首屏必需） |

## 5. 里程碑（约 1–2 周）

| 阶段 | 内容 | 估时 |
|---|---|---|
| 0 | 阅读理解（工程外 leo-radio）已完成；确认派生基座（携带 wifi_prov），跑通新工程空编译 | 0.5 天 |
| 1 | 平台层新增流式播放 API（§2），应用层接 esp_audio_codec，播一个固定测试流出声 | 1 天 |
| 2 | radio_player 链路（HTTP 拉流 + 解码 + 流式输出 + 跳台/重连状态机），真机播通网络流 | 2–3 天 |
| 3 | radio_catalog（Radio Browser 客户端 + 定位 + 兜底列表 + 频率解析） | 2 天 |
| 4 | ui_radio 圆屏 FM 环形刻度表盘 + 指针扫动 + 触摸手势 + 网络状态（按超越目标） | 3–5 天 |
| 5 | radio_settings（音量/亮度/定时/收藏）+ 中文字体子集化 + `screenshot_recv.py` 真机验证 | 2–3 天 |

## 6. 风险与对策

- **基座无配网（高）**：派生前确认基座携带 `wifi_prov.c`，否则开机无网可配 → 本项目一切依赖联网。
  对策：从携带 wifi_prov 的工程派生或显式并入 wifi_prov。
- **采样率落地（中）**：I2S0 固定 16k，流常 44.1/48k。对策：§2 流式 API 内重配 TX 时钟；
  若平台不愿动时钟，则应用层重采样到 16k（兜底，音质略降）。
- Radio Browser 海外慢 → 内置兜底 + 最快镜像；流失效 → 失败跳台 + 定期刷新；
  格式混编 → MP3/AAC 优先；HTTPS 证书 → crt_bundle / 实测；断网 → 复用配网状态机重连。

## 7. 验证方式

- 每个里程碑本地 `idf build` 通过；真机烧录 + `tools/screenshot_recv.py` 截图核对 UI
  （串口 UART0 不可读，按现有约定）。
- 首屏验收：3 秒内看懂在放什么台、怎么换台，不依赖说明书。
- 不触碰任何公网部署实例；所有跑批仅本地 / 真机。

## 8. 已采纳决策

- 工程形态：**独立派生应用**（非并入 AVATAR、不上架 AI Passport）。
- 派生基座：选择携带 `wifi_prov.c` 的工程（AVATAR 或同规格基座）。
- 音频通路：**平台组件新增 `stream_start/write/stop` 流式 API（首选）**，
  应用层重采样 16k 作兜底。
- 代码隔离：`sdgoods-radio` 全仓不含 leo-radio 任何代码 / 组件。
