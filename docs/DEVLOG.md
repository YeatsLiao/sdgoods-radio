# 城市电台 · 重构过程记录（DEVLOG）

> 目标：把 `main/apps/` 里的电台应用从「功能演示」重做成**可长期放在桌面上的产品**。
> 设计规格见 [UI_REDESIGN.md](UI_REDESIGN.md)；本文件记录**为什么这么改、踩了哪些坑、怎么验证**。
>
> 硬件：SDGOODS ESP32-S3 + 360×360 圆屏（ST77916）+ PCM5101 I2S + 无键盘鼠标
> 软件：ESP-IDF 5.5.3 / LVGL 8.3 / 原生 ESP-IDF 工程（非 Arduino）

---

## 0. 一句话结论

旧实现「效果很差劲」不是配色问题，而是**三层错位**叠加：

1. **主视觉错位** —— 信息量最大的位置放的是 16px 小字，纯网络台没有频率时长期显示 `--.-`；
2. **交互错位** —— 全屏轻点=播放、上滑=下一台，与平台的「上滑回主页」抢手势；
3. **运行时错位** —— Wi-Fi/BT/LVGL 把**内部 RAM** 吃到只剩十几 KB，
   `xTaskCreate(8KB)` 静默失败，于是「目录永远加载不出来、所有台都播放失败」。

前两层靠设计与重写解决，第三层是这次真正的硬骨头（见 §4）。

---

## 1. 起点：旧实现的病灶清单

| 维度 | 旧实现 | 用户感受 |
| --- | --- | --- |
| 视觉 | 深蓝底 + 天蓝弧 + 琥珀大字 + 绿电平，四种高饱和色互相打架 | 像 2010 年车机 |
| 主视觉 | 中央 16px 频率字 | 远看只有一个空心圆 |
| 内容 | 大量网络台无 FM 频率 → 常年 `--.- MHz` | 产品像坏的 |
| 交互 | 全屏轻点=播放/暂停、上滑=下一台 | 误触，且与平台手势冲突 |
| 结构 | 设置是一个 5 个等宽按钮的浮层 | 找不到东西，无处扩展 |
| 文案 | 走 `SDG_T()`，出厂英文 | 中文产品开机显示 `Connecting…` |
| 内容源 | 只按国家取台，无「城市」；含 OGG/HLS 不可播流 | 一半的台播放失败 |

对照 `leo-radio` 与「城市电台」产品页，确定三条硬规则：单色 + 一个点缀色；
唯一的大元素是点阵英雄；信息分层只留三级（主视觉 → 台名 → 元信息）。

---

## 2. 落地形态（文件职责）

| 文件 | 职责 |
| --- | --- |
| `main/apps/ui_radio_kit.h/.c` | 设计系统：主题 token、5×7 点阵、表盘环、矢量图标、胶囊、页面骨架、文本安全化、**PSRAM 后台任务帮助函数** |
| `main/apps/ui_radio_pages.h/.c` | 列表页 / 设置页 / 通用选择器 / WiFi 配网浮层 |
| `main/apps/ui_radio.c` | 主屏构建、拖拽调台、tick（目录/状态/电平/时钟/休眠/跳台）、串口诊断键 |
| `main/radio_catalog.c/.h` | 城市查询（name=城市 + countrycode 补足）、URL 去重、不可播编码过滤 |
| `main/radio_settings.c/.h` | NVS：城市、主题、定时休眠、续播索引 |
| `main/radio_player.c` | 生产者（HTTP 拉流）/ 消费者（解码 → I2S）双任务 + 64KB PSRAM 环形缓冲 |
| `tools/gen_fonts.py` | 字体子集生成（源码字符集 + GB2312 一级字库） |
| `tools/screenshot_recv.py` | 串口截屏接收（`--pre` 按键序列驱动二级页） |

设计原则：**全部自绘**（`LV_EVENT_DRAW_MAIN` + `lv_draw_rect/arc/line/triangle`），
不用 canvas 缓冲、不依赖任何图标字体 —— 图标字体缺字形、canvas 吃 RAM 的问题从根上消失。

---

## 3. 设计与交互决策（摘要）

* **点阵英雄**：5×7 点阵、`dot=6 gap=4`，单字宽 46、字距 8。有 FM 频率显示频率（`90.7`），
  纯网络台显示**频道号**（`07`）。曾试过 `CH07`，实测 `C`/`H` 在 5×7 下糊成一团，纯数字清楚得多。
* **表盘环**：圆心 (180,180)、r=168、底部 60° 缺口（LVGL 角度 0° 在 3 点钟、顺时针，故 `120°→420°`）；
  轨道 3px hair、进度 4px accent、端点 11px 实心圆。
* **双主题**：PAPER（`#F4F1E8`）/ NIGHT（`#0C0C0D`），全部颜色只在绘制时读 `rk_theme()`；
  **切主题 = 重建屏幕**，播放器不动、播放不中断。
* **交互**：点按空白 = 播放/暂停；横向拖拽 = 调台（每 26px 一台，只预览，松手起播）；
  胶囊 = 列表；品牌字 = 设置（隐藏入口）；顶部下滑交给平台控制中心。
* **文案中文化**：不再走 `SDG_T()`，界面文案直接写中文（并因此进入字体子集）。

---

## 4. 关键坑与修复（按发现顺序）

### 4.1 内部 RAM 枯竭 → 后台任务静默失败（本周期最大坑）

**现象**：电台目录永远不出现；串口完全没有 `catalog:` 日志。补日志后看到每 120ms 一条：

```
W (3823) UI_RADIO: catalog task create failed (heap=7035376)
```

`xTaskCreate(..., 10240, ...)` 返回 `pdFAIL`。`heap=7MB` 是**总量**（主要是 PSRAM），
而 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 只有 ~14KB —— Wi-Fi/BT/LVGL 已把内部 RAM 吃光，
8KB 栈 + TCB 找不到连续块。

**同一根因的第二个症状**：目录好了以后，**所有台播放都失败**（`reconnect 1/2 → giving up`，每台约 1.5s），
因为 `radio_player.c` 里的生产者任务 `xTaskCreatePinnedToCore(rp_net_task, 8192)` 同样静默失败。

**修法**：本项目开了 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` + `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`，
把所有应用层后台任务的**栈放到 PSRAM**：

```c
/* main/apps/ui_radio_kit.h */
bool rk_task_spawn(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned prio);
void rk_task_exit(void);

/* main/apps/ui_radio_kit.c —— 头文件是 freertos/idf_additions.h */
bool rk_task_spawn(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned prio)
{
    return xTaskCreateWithCaps((TaskFunction_t)fn, name, stack, arg, (UBaseType_t)prio, NULL,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}
void rk_task_exit(void) { vTaskDeleteWithCaps(NULL); }
```

`radio_player.c` 用 `xTaskCreatePinnedToCoreWithCaps(..., RP_NET_CAPS)` + `rp_task_exit()`；
**任务退出必须成对用 `vTaskDeleteWithCaps()`**（`vTaskDelete(NULL)` 会漏还 PSRAM 栈）。

**结果**：`catalog: 26 stations (city=auto cc=CN)` → `net: connected, streaming` → `playing: 48000 Hz ch=2 MP3`。

> 经验：在 ESP32-S3 + LVGL 的工程里，`xTaskCreate` 的返回值**必须检查**并打日志；
> 只要日志里出现 `heap=xxxxxxx` 这种「看起来很大」的数字，就要立刻怀疑它统计的是 PSRAM 总量。

### 4.2 `pdMS_TO_TICKS(5) == 0` → 空转饿死 IDLE、`rp_net_shutdown` 死循环

**现象**：播放一阵后串口每 5s 刷一次任务看门狗：

```
E (84553) task_wdt: Task watchdog got triggered. - IDLE1 (CPU 1)
E (84553) task_wdt: CPU 1: radio_pl
```

**根因**：`CONFIG_FREERTOS_HZ=100` ⇒ 1 tick = 10ms，`pdMS_TO_TICKS(5)` **算出来是 0**，
而 `vTaskDelay(0)` 不是「让出 1 tick」而是**立即返回**。于是：

* 消费者任务（优先级 6、钉 core 1）在「环形缓冲空 / 缓冲满」路径上空转；
* 更要命的是 `rp_net_shutdown()` 的 `while (s_net_alive) vTaskDelay(0)` —— 生产者任务
  同在 core 1、优先级只有 5，**永远抢不到 CPU**，`s_net_alive` 永不落地 ⇒ 卡在「连接中」。

**修法**：毫秒延时统一走一个至少 1 tick 的包装：

```c
static void rp_delay_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}
```

**结果**：看门狗刷屏消失，换台不再卡死。

### 4.3 台名缺字 → `?`（`杭州城市资?广播`）

**根因**：`cn_font_*` 只收「源码里出现过的字」（1170 个），而台名来自网络，
`rk_text_safe()` 会把子集外的字换成 `?`。

**修法**：新增专供运行时字符串的 `net_font_20`（GB2312 一级 3755 字 ∪ 源码字符集，
20px/2bpp，`.fallback = &cn_font_16`），并删掉只为英雄位存在的 `cn_font_20` /
`si_yuan_black_icon_20` 以腾体积。列表页同时改用 `rk_page_row_net()`。

体积账：`0x2c8d10`（2,919,696 B）/ 分区 3MB，**剩 7%**。

> 判定：`RK_FONT_TITLE` 只给「网络字符串」用；界面自身文案仍走 `cn_font_16`，
> 因为它们的字符集在编译期就确定了。

### 4.4 `LV_LABEL_LONG_DOT` 只折行、不出省略号

**现象**：列表里长台名折成两行、顶出 48px 行高，与下一行叠在一起。

**根因**：LVGL 8.3 的截断条件是「**换行后仍超出高度**」。只设宽度不设高度 → 它永远只折行。

**修法**：`lv_obj_set_size(l, w, 26)`（锁一行）+ **先设尺寸/长模式再设文本**
（反过来的话文本已按默认 WRAP 排好版）。主屏的台名/元信息同样补齐高度。

### 4.5 截图总是开机加载态 / 二级页截不到

`screenshot_recv.py` 打开串口时 CP210x/CH343 会瞬时拉一下 DTR，设备被复位，
所以**打开串口那一瞬间发的字符全落在启动阶段被丢掉**——拿到的一直是开机加载态。

修法（工具侧，`tools/screenshot_recv.py`）：

* `--pre-boot N`：发键前先等 N 秒（等设备真启动完，实测 ≥12s 稳妥）；
* `--pre "k,w"`：**逗号分隔的按键序列**，每键间隔 `--pre-gap`（默认 2.5s），
  这样才能进「设置 → WiFi」这种二级页；
* `--wait N`：最后一键之后再等 N 秒让 LVGL 渲染完，才发 `s` 截屏。

```powershell
# 主屏（纸张）
python tools\screenshot_recv.py -p COM6 -t --pre-boot 16 --wait 2 -o docs\shots\main_paper.jpg
# 设置 → WiFi 配网 二级页
python tools\screenshot_recv.py -p COM6 -t --pre "k,w" --pre-boot 10 --pre-gap 3 --wait 5 `
       -o docs\shots\page_wifi.jpg
```

> 教训：`--pre-boot` 给短了（我一开始给 3s）会截到「点阵四点在闪 + 正在获取城市电台…」的
> 加载态，很容易被误判成「目录又加载不出来」。看时间戳和台数就能区分。

### 4.6 验收通道：串口诊断键

本机没有键鼠，截屏一次只能拍当前那一屏。`ui_radio.c` 覆盖平台弱符号
`sdgoods_console_ext_cmd()`，把字母映射成**合成触摸**（`sdgoods_tap_synth()`），
并全部经 `lv_async_call` 排回 UI 上下文（跨任务直接动 LVGL 会撞车）：

| 键 | 行为 |
| --- | --- |
| `l` | 点「电台列表」胶囊 → 列表页 |
| `k` | 点左上品牌字 → 设置页 |
| `w` | 点设置页首行（y=124）→ WiFi 配网 |
| `n` | 纸张 / 夜色互换 |
| `x` | 关掉所有页面回主屏（连续截屏收尾用） |
| `1`–`9` | 直接播第 N 台 |

用法：`python tools/screenshot_recv.py -p COM6 -t --pre "l" --pre-boot 12 --wait 5 -o docs/shots/page_list.jpg`

> 为什么用「点坐标」而不是直接调 `radio_pages_open_*()`：坐标走的是真实点按链路
> （`sdgoods_tap_bind` 的位移守卫 + LVGL 命中测试），这样截到「页面能打开」才等价于
> 「用户点得开」。直接调函数只能证明渲染没问题。

### 4.7 设置页第 5 项落在折行外 = 不存在

**现象**：设置页只显示 4 行，`刷新电台` 要下滑才出现，而圆屏上**没有任何「这页能滚」的提示**。

**修法**：新增 `rk_page_row_compact()`（行高 40px），设置页 5 行 × 40 = 200px，
正好塞进 body（`y=100..300`），不需要滚动。列表页仍用 48px（行少、点击目标要更大）。

```c
/* ui_radio_kit.c */
void rk_page_row_compact(...) { page_row_impl(..., RK_FONT_BODY, 110, 10, 40); }
/* 行内分隔线也跟着行高走，不再硬编码 47 */
rk_hairline(row, 10, row_h - 1, 236, th->hair);
```

顺带把设置页顺序按使用频率重排：**WiFi 配网 → 城市 → 主题 → 定时休眠 → 刷新电台**，
并去掉「返回」行（页头 ✕ 已足够）。

### 4.8 「缓冲中」卡很久：加首帧看门狗

**现象**：某些台连接成功、HTTP 头也回来了，但音频数据涓流或不发，
`probe` 永远涨不到 `RP_PROBE_BYTES`（16384），UI 就长期停在「缓冲中」。

**修法**：进 `RP_BUFFERING` 记一个起点，**12s 内解不出第一帧**就判这台失败
（`RP_END_HTTP_FAIL`），走已有的「重试 2 次 → ERROR → 自动跳下一台」链路；
暂停期间重置起点，避免暂停回来立刻超时。

```c
#define RP_FIRST_FRAME_MAX_MS 12000
if (!stream_started && !s_paused &&
    (uint32_t)((xTaskGetTickCount() - t_first_frame) * portTICK_PERIOD_MS) >= RP_FIRST_FRAME_MAX_MS) {
    rc = RP_END_HTTP_FAIL;
    break;
}
```

---

## 5. 真机验收记录（COM6）

> 截图实际存放在 `docs/shots/`（下文表内路径相对于仓库根）。

| 结论 | 证据 |
| --- | --- |
| 主屏排版符合安全区，无控件落在 y<50 | `docs/shots/main_paper.jpg` |
| 点阵英雄可读：有 FM 频率显示 `90.7`，纯网络台显示频道号 `01` | `main_paper.jpg` / `main_play.jpg` |
| 台名无 `?`：`杭州市广播电视台西湖之声`、`杭州城市资讯广播 FM90.7杭州…` | `main_play.jpg`、`main_night.jpg` |
| 播放态：台名 + `城市电台 · 1/26` + 18 根电平条 + PAUSE 图标 | `main_play.jpg` |
| 列表页：20px 台名、单行省略不折行、无缺字、页头「设置 / ✕」 | `page_list.jpg` |
| 设置页：5 行（WiFi 配网 / 城市 / 主题 / 定时休眠 / 刷新电台）**一屏全可见、无需滚动** | `page_settings.jpg` |
| WiFi 配网：标题 + 「重扫」+ ✕ + 扫描到的 4 个 SSID（点行进密码页） | `page_wifi.jpg` |
| 夜色主题：深底浅字、accent 端点仍突出 | `main_night.jpg` |
| 目录加载：`catalog loaded: 26 stations (city=杭州)` | 串口日志 |
| 开流成功：`net: connected, streaming` → `playing: 48000 Hz ch=2 MP3` | 串口日志 |
| 看门狗刷屏消失（连续观察 55s） | 串口日志无 `task_wdt` |
| 固件体积：`0x2c8d10` = 2,919,696 B，分区剩 7%（`0x372f0`） | `idf.py build` 输出 |

> 注：`page_settings.jpg` 里 `WiFi 配网` 显示「已连接」是稳态；刚复位那几秒抓会显示「未连接」，
> 因为行内值是**打开页面时的快照**，此时 STA 还在重连。属于预期行为，不是 bug。

---

## 6. 已知问题 / 下一步

1. **可用率**：Radio Browser 里不少 CN 台本身已死或只服务本地网络
   （`zeno.fm` / `stm.rthk.hk` 在测试网段打不开）。当前策略是「重连 2 次 → 自动跳下一台，
   连续 3 台失败则停下」，且新增了 12s 首帧看门狗（见 §4.8），死台不再无限「缓冲中」。
   下一步：开流失败立刻放弃预缓冲等待、给目录加「上次连通性」记忆。
2. **台名冗余**：部分台名自带 `FM90.7杭州…`，与英雄位频率重复；
   可在 `radio_catalog.c` 里裁掉尾部 `FM\d+(\.\d)?`。
3. **列表 / WiFi 页仍要滚动**（可视区所致）：列表 26 台里只看得到 4 行，
   滚动与惯性未专门调过手感，圆屏也没有滚动指示。
4. **收藏 / 节目单 / ICY 曲名**：见 UI_REDESIGN §12。

---

## 7. 命令速查

```powershell
# 环境（本机 ESP-IDF 5.5.3 装在 C:\Users\shea_\esp\esp-idf，工具链在 D:\1.Soft\Espressif）
$env:IDF_TOOLS_PATH='D:\1.Soft\Espressif'
$env:IDF_PATH='C:\Users\shea_\esp\esp-idf'
$env:IDF_PYTHON_ENV_PATH='D:\1.Soft\Espressif\python_env\idf5.5_py3.11_env'
$env:PATH='D:\1.Soft\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Users\shea_\esp\esp-idf\tools;' + $env:PATH
. 'C:\Users\shea_\esp\esp-idf\export.ps1'

# 字体（新增中文一定要重跑，否则新字变 '?'）
python tools\gen_fonts.py --sizes 14,16 --net-size 20 --net-bpp 2 `
    --bin "node_modules\.bin\lv_font_conv.cmd"

# 编译 + 烧录
idf.py build
idf.py -p COM6 flash

# 截屏（常用三种形态）
# 1) 主屏：--pre-boot 等设备启动完（打开串口会复位，≥12s 稳妥）
python tools\screenshot_recv.py -p COM6 -t --pre-boot 16 --wait 2 -o docs\shots\main.jpg
# 2) 一级页：l=列表 / k=设置 / n=切主题
python tools\screenshot_recv.py -p COM6 -t --pre "l" --pre-boot 12 --wait 4 -o docs\shots\page_list.jpg
# 3) 二级页：逗号分隔按序发送（k → 设置，w → 点首行进 WiFi）
python tools\screenshot_recv.py -p COM6 -t --pre "x,k,w" --pre-boot 12 --pre-gap 3 --wait 5 `
       -o docs\shots\page_wifi.jpg
```

---

## 8. 仓库结构整理（`firmware/RADIO/` → 仓库根）

原来代码埋在 `sdgoods-radio/firmware/RADIO/` 两层下，编码时要去翻五层目录。现已摊平：

```
sdgoods-radio/
├── README.md          # 项目 README（路由：特性 / 结构 / 构建烧录 / 文档索引）
├── docs/              # 全部文档（电台 + 平台 + 验收截图）
│   ├── UI_REDESIGN.md / DEVLOG.md / ARCHITECTURE.md / PLAN.md
│   ├── PLATFORM.md / PLATFORM_EN.md / PLATFORM_ARCHITECTURE.md   # 原平台 README 与平台架构
│   ├── shots/         # 6 张当前验收截图
│   └── archive/       # 历史日志与旧截图（已 gitignore）
├── main/ components/ platform/ tools/ sdgoods-ai/
└── CMakeLists.txt / sdkconfig / sdkconfig.defaults / dependencies.lock
```

整理过程中要注意的三件事：

1. **`build/` 必须删掉重建**：`build/CMakeCache.txt` 里全是绝对路径，目录一移就会报找不到源文件。
2. **两份 `ARCHITECTURE.md` 不能撞名**：电台那份留在 `docs/ARCHITECTURE.md`，
   平台那份改名为 `docs/PLATFORM_ARCHITECTURE.md`；平台 README 同样变成 `docs/PLATFORM.md`。
3. **`.gitignore` 里的 `shots/` 会连 `docs/shots/` 一起忽略**（该规则本意是忽略根目录截图输出），
   已改成只忽略 `shot_*.jpg/png`，保证验收截图能进仓库。

> 内层 `.git` 当时是 0 commit 的空仓，所以直接上移，无历史损失。

顺手修的另外一个工具 bug：`--pre-boot` 原先只在传了 `--pre` 时才生效，纯 `-t` 截屏会
在开机瞬间发 `s`，拍到“正在获取城市电台…”的加载态。现已改为**无条件生效**（见 §4.5）。
