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
| `v` | 直接开音量滑块页（平台 `sdgoods_cc_debug_open(3)`） |
| `g` | 在滑块页上合成一次横拖（先 `v` 再 `g`），验收「拖动不卡」 |
| `j` / `J` | 下一台 / 上一台（连切验收缓冲与失败率） |
| `y` | 直接播 `http://stream.antenne.de/antenne`（必返 302，验重定向跟随） |
| `V` | 音量恢复 70% 并落盘（串口拖动把音量拖到 0 后救回来） |
| `m` | 开音量页 + 点一下静音开关（连发两次 = 静音 → 恢复；见 §12.4 为什么要等一帧） |

> 别用 `s`：它被平台串口截屏占了（`sdgoods_console.c`），会被截胡。

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

---

## 9. 首次推送到 GitHub

仓库以 `main` 为默认分支，按功能点分成三个 commit 提交后推送：

| commit | 内容 |
| --- | --- |
| `chore: 导入 SDGOODS 平台基座工程` | components / platform / tools / sdgoods-ai / 构建配置 / 平台文档 |
| `feat: 城市电台应用（点阵主屏 / 设计系统 / 二级页 / 城市目录 / 播放器）` | `main/radio_*`、`main/apps/ui_radio*`、`net_font_20` |
| `docs: 补 UI 重构规格、调试日志与验收截图，重写根 README` | README / UI_REDESIGN / DEVLOG / ARCHITECTURE / PLAN / `docs/shots` |

推送前处理的两处坑：

1. **索引里残留摊平前的路径**：内层 `.git` 是 0 commit 空仓，但 index 里还留着 `firmware/RADIO/`
   时期的条目（含已删除的 `README_EN.md`）。先 `git read-tree --empty` 清空索引再重新 `git add`，
   否则第一次提交会带上磁盘上不存在的文件。
2. **`git push` 直连 GitHub 报 `Connection was reset`**：本机靠 `127.0.0.1:7890` 代理出网，
   而 git 不读 Windows 系统代理设置。给本仓库加 `git config http.proxy http://127.0.0.1:7890`
   （只写进 `.git/config`，不进版本库）之后推送才通。

推送结果：`origin/main` = 3 个 commit / 203 个受版本控制文件；`build/`、`managed_components/`、
字体源文件（`tools/fonts/*.ttf`）与 `docs/archive/` 下的历史产物按 `.gitignore` 留在本地。

---

## 10. 圆屏三连修：键盘按不到 / 出界 / 切音量卡顿（2026-10-27）

用户在真机上提了三个问题（附一张 WiFi 密码页照片）：**按键按不到**、**有些字超出圆形屏幕**、
**一切换音量就卡**。三个都不是「调参数」，各有一处明确成因。

### 10.1 密码页键盘按不到：矩形键盘铺在圆屏上

**现象**：照片里 QWERTY 键盘铺满 360 宽贴底，最左 `1#`、最右退格/回车压在弧线上，
底行 `ZXCVBNM` 几乎贴着圆边 —— 边缘键摸得到看不清、看得到摸不到。

**根因**：`lv_keyboard` 是矩形栅格，只能整体设宽高。原代码 `lv_obj_set_size(kb, 360, 150)` +
底部对齐 ⇒ 每行键带全部横跨 360px。而屏幕是 360 圆（圆心 180,180、R=180），任一 y 处的
可用宽度是弦宽 `w(y) = 2·√(180² - (y-180)²)`：

| 行底 y | 弦宽 |
| --- | --- |
| 190 | 359px |
| 227 | 348px |
| 264 | 319px |
| 301 | 267px |
| 340 | 162px |

底行那条 360px 键带里有 100+ px 在圆外 —— 那里既不可见也点不到。

**修法**：把 `lv_keyboard` 换成自绘键盘（`main/apps/ui_radio_pages.c` 的 `pw_kb_build()`）：

- 4 行固定，**每行宽度按该行底边的弦宽算**，行内按键均分并水平居中 ⇒ 键盘自然成梯形；
- 距圆边留 10px（`PW_EDGE`），弧线处触摸端点抖动大，贴边容易按空；
- 字母页 10 / 9 / 9 键，第三行并入 `1#`（切数字符号页）与 `DEL`；底行只放「空格 + 连接」；
- 底行空格不写字，画一条 28×2 横线居中；
- 换页要重建整块键盘（= 删掉正在处理本事件的按键对象）⇒ 走 `lv_async_call`，
  与「销毁当前页必须异步」的既有规则同源。

排布从 `y=156..301` 调到 `y=168..313`：上方留 18px、下方 47px，重心与圆弧一致。

### 10.2 「字体超出圆屏」= 同一个键盘

主屏、列表页、设置页、WiFi 列表页逐张复核：文字都在圆内（body 在 `x=52..308 / y=100..300`，
`y=300` 处弦宽仍有 268px）。真正出界的只有密码页键盘底行那几个字母，10.1 修完即消失。

### 10.3 切音量就卡：三条阻塞叠在 LVGL 任务上

**现象**：拖音量滑块画面明显卡顿；更糟的是拖完电台就**没声了**。

**根因 A（每事件两条 INFO 日志）**：`CONFIG_ESP_CONSOLE_UART=y` + 115200 + 默认 INFO 级别，
一行日志 ≈ 4ms 阻塞。拖动时每个 `VALUE_CHANGED` 打两行（`cc: slider[...] value -> N%`
与 `audio: volume=N% -> PA ...`），一秒几十次 ⇒ LVGL 任务每秒被串口吞掉几百毫秒。

**根因 B（定时器抖动）**：`save_timer_arm()` 每次 `lv_timer_del()` + `lv_timer_create()`，
等于每几毫秒 malloc/free 一个定时器并重排链表。

**根因 C（功放被关）**：`pa_apply()` 只判断 `s_bgm_run`，而 `sdgoods_audio_stream_start()`
会先 `bgm_stop()`（两者共用 I2S TX，互斥）⇒ **电台播放期间 `s_bgm_run` 恒为 false**。
于是每拖一次音量就 `pa_set(false)` 关功放，且再没人打开 —— 电台彻底静音。

**修法**：

| 文件 | 改动 |
| --- | --- |
| `components/sdgoods_board/src/sdgoods_audio.c` | `pa_apply()` 改为 `(s_bgm_run \|\| s_stream_active) && s_vol_pct > 0`；音量日志只在功放状态**翻转**时 INFO，其余降 DEBUG |
| `components/sdgoods_launcher/src/sdgoods_cc.c` | 滑块回调不再打日志（落盘时打一行终值）；`save_timer` 只创建一次，用 `lv_timer_pause` / `lv_timer_reset` 复用 |

> ⚠️ `sdgoods_audio.c` 的 I2S 与功放启停时序（64ms 淡出）没有动，只纠正了「谁算正在输出」
> 这个判断条件 —— 原条件在直播流模式下必然误判。

**新诊断键**（`main/apps/ui_radio.c` 的 `qa_apply`）：

| 键 | 作用 |
| --- | --- |
| `v` | 直接打开音量滑块页（`sdgoods_cc_debug_open(3)`） |
| `g` | 在滑块页上合成一次横拖（起点 120,180、每帧 +12px、10 帧） |

### 10.4 验收（COM6）

| 结论 | 证据 |
| --- | --- |
| 键盘 4 行全部落在圆内，梯形收窄 | `docs/archive/shots/pw_new4.jpg` |
| 设置页 / 主屏无回归 | `set_page.jpg`、`main_*.jpg` |
| 拖动 10 帧只产生 **1 行**日志：`slider[Volume] persisted -> 70%` | 串口日志 |
| 流播放中调音量：`volume=0% -> PA OFF` → `volume=1% -> PA ON`（旧逻辑永远只会打 OFF） | 串口日志 |
| 无 `task_wdt`、无掉帧日志 | 串口日志 |
| 固件 `0x2c9040` = 2,920,512 B，分区剩 7% | `idf.py build` |

进密码页需要「设置 → WiFi → 点第一个 SSID」，扫描约 3s，所以 `--pre-gap` 要大于扫描耗时：

```bash
python tools/screenshot_recv.py -p COM6 -t --pre "k,w,w" --pre-gap 4.5 --wait 2.5 \
       -o docs/archive/shots/pw_new4.jpg
# 音量页与拖动复现
python tools/screenshot_recv.py -p COM6 -t --pre "v" --wait 2 -o docs/archive/shots/vol_page.jpg
```

## 11. 切台缓冲久 / 有概率失败：30x 重定向没被跟随（2026-10-27）

### 11.1 根因

真机日志里反复出现：

```
W radio_player: http status 302 for https://streams.radiomast.io/ref-128k-mp3
```

`radio_player.c` 用的是 `esp_http_client_open / fetch_headers / read` 手动路径，而 IDF 的
自动重定向只在阻塞式 `esp_http_client_perform()` 里做：

- `esp_http_client_set_redirection()` 源码里 `client->process_again = 1` 后面写着
  `// used only in the blocking mode`；
- `esp_http_check_response()`（真正处理 3xx 的函数）只在 perform 的主循环里调用。

所以非阻塞路径完全不管 3xx，**凡是被 CDN 302 到真实流地址的台每次都判失败** →
重试 2 次 → UI 自动跳下一台。用户感受就是「有概率失败」，概率取决于随机到哪个台。

第二个坑：302 的 `Location` 也拿不到。`esp_http_client_get_header()` 读的是
**请求**头（`client->request->headers`），响应头只能通过 `HTTP_EVENT_ON_HEADER` 回调拿。

### 11.2 改法（全在 `main/radio_player.c`）

| 改 | 说明 |
| --- | --- |
| `rp_http_event()` | 在 `HTTP_EVENT_ON_HEADER` 里抓 `Location`（`strcasecmp`，头名大小写不敏感） |
| 跳转循环 | 3xx → `rp_abs_url()` 拼绝对地址（兼容 `/path`）→ `close` → `set_url` → 重开，最多 3 跳 |
| 超时分离 | 建连/握手 5000ms；进流后 `set_timeout_ms(2500)`，死台与切台少白等 |
| 判死提速 | `RP_FIRST_FRAME_MAX_MS` 12000 → 6000 |
| 起播提速 | `RP_PREBUFFER` 16384 → 8192（约 0.5s 出声，后面还有 64KB 环垫底） |

`esp_http_client_reset_redirect_counter()` 每次跟跳都调：跳数由我们数，库里的计数器
只服务阻塞模式，留着没意义。

### 11.3 验收（COM6）

```
8.0 >>> sent 'y'
8.6 I radio_player: http 302 -> http://s3-webradio.antenne.de/antenne
10.0 I radio_player: net: connected, streaming
10.4 I audio: stream start: 44100 Hz ch=2
10.5 I radio_player: playing: 44100 Hz ch=2 MP3
```

`http://stream.antenne.de/antenne` 必返 302：改前 100% 失败，改后 **2.4s 出声**。
连切 8 台（`j`）：7 台出声，全程没有 `http status 302` 报错。

> `https://streams.radiomast.io/*` 跳到的边缘节点返回 404（本机 curl 也是 404），
> 那是死台，不是跟随逻辑的问题。

固件 `0x2c9370` = 2,921,584 B，分区剩 7%。

### 11.4 偶发断流：读超时被当成断流（同日修复）

真机长播时偶发：

```
E transport_base: esp_tls_conn_read error, errno=No more processes
I audio: stream stop
W radio_player: reconnect attempt 1/2 in 500ms
```

`No more processes` 就是 `EAGAIN`。链路是：`transport_ssl.c:275` 的 `esp_tls_conn_read`
返回 `WANT_READ`（poll 说可读，但 TLS 记录只到了一半）→ 转成
`ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT` → `esp_http_client_read()` 返回
`-ESP_ERR_HTTP_EAGAIN`。

旧代码只有一行 `if (r <= 0) break;`，把「暂时没数据」和「真断流」当成了同一件事：
电台切歌 / 服务器缓冲几秒，整条流就重建一次。把读超时从 6000ms 降到 2500ms 后
更明显（变成了每 8s 断一次）。

改法：`-ESP_ERR_HTTP_EAGAIN` 只累计 `idle_ms`，连续 `RP_NET_IDLE_MAX_MS = 15000`
一个字节都没读到才关闭；`r <= 0` 其余情况（`ESP_FAIL` / 服务端 FIN）仍然立即判死。

验收：`http://stream.antenne.de/antenne` 连播 **5 分钟 0 次断流**（改前必现一次）；
用 `1` 号台（服务端 8s 就 FIN）验证真断流仍能在读完剩余缓冲后立刻重连。

### 11.5 还没解决的

- **台源质量**：radiobrowser 里大量境外台在国内被 RST / 404 / 只发头不发数据。
  查询已带 `hidebroken=true&order=votes`，但 broken 标记靠社区上报，不准。
  固件侧只能快跳（6s 判死 + 2 次重试 + UI 自动跳下一台）。

---

## 12. 音量页加静音开关（2026-10-27）

用户要求：音量调节的地方加一个静音的开关。

### 12.1 设计：静音不新增状态，就是「音量 0」

`sdgoods_audio_set_volume(0)` 本身就是静音（`pa_apply()` 还会顺手关功放、消掉底噪），
所以没有引入第二个真值 —— 这个按钮只是「把滑块拖到 0 / 回到上次音量」的快捷方式：

| 音量 | 按钮 | 行为 |
| --- | --- | --- |
| > 0 | 深灰胶囊 `Mute` | 记下当前值 → 设 0 |
| = 0 | 白底胶囊 `Unmute` | 回到 `s_vol_before_mute`（默认 70） |

滑块位置、`%` 数字、按钮文案三者永远一致：拖到 0 按钮自动变 `Unmute`，
按 `Unmute` 滑块也自动跳回去（`mute_ui_refresh()` 按**实际音量**刷新）。

### 12.2 落盘：多存一格「静音前的音量」

`vol` 被写成 0 之后，「取消静音回到多少」就没地方记得了。新增 NVS key `volunmute`
（ns `sdg_cc`）：**只在 vol > 0 时写**，所以它永远等于「最后一次听得见的音量」。

- 静音 → 关机 → 开机：`restore: vol=0% ... unmute=70`，开机仍静音（这是用户意图）
- 再点 `Unmute` → 回到 70，而不是出厂默认值

### 12.3 UI 与点按槽

- 132×44 胶囊，`LV_ALIGN_CENTER, 0, +72` ⇒ 中心 (180, 252)；该处圆内弦宽 ≈ 331px，余量充足。
- 点按走 `sdgoods_tap_bind`（位移 > 24px 不算点按），与其他按钮一致。
- 固定占 `CC_MUTE_SLOT`（`s_btn_ctx` 最后一格）而**不是**顺序分配：滑块页是
  **覆盖**在一级页之上的，一级页那几个按钮槽还活着，顺序分配会撞槽
  （现象：回控制中心点 Volume 反而触发静音）。
- 只在音量页建（`open_slider(..., with_mute=true)`），亮度页不建。

### 12.4 踩到的坑：LVGL 子对象坐标要等一次布局刷新

串口验收键 `m` 最初写成「同一个 tick 里先开页再合成点击」，结果点击落空。
日志打出来是 `mute btn rect x 0..-1 y 0..-1` —— 对象刚创建，`align` 还没算坐标。

修法：`m` 拆两步，开页后用 150ms 的一次性 `lv_timer` 中转，回调里再
`sdgoods_tap_synth_now()`。验证后按钮真实矩形 `114..245 / 230..273`，中心 (179.5, 251.5)。

> 只影响「建页后立刻合成点击」的调试路径；真实手指点击发生在页面显示之后，不受影响。

### 12.5 验收（COM6）

```
15.3 I cc: tap: Mute 70% -> 0%
15.3 I audio: volume=0% -> PA OFF
16.1 I cc: save: vol=0% (muted) bri=55 -> NVS ns='sdg_cc'
22.3 I cc: tap: Mute 0% -> 70%
22.3 I audio: volume=70% -> PA ON
23.1 I cc: save: vol=70% bri=55 -> NVS ns='sdg_cc'
```

重启后 `stream start: 48000 Hz ch=2 vol=0%`（静音被记住）→ 再点一下 `tap: Mute 0% -> 70%`。
固件 `0x2c9690` = 2,922,128 B，分区剩 7%。

### 12.6 顺带发现：HTTPS 流的 `esp-aes: Failed to allocate memory`

验收时看到 HTTPS 台每 2–3s 断流重连：

```
E esp-aes: Failed to allocate memory
E esp-tls-mbedtls: read error :-0x0001
W radio_player: reconnect attempt 1/2 in 500ms
```

`esp-aes` 来自 `components/mbedtls/port/aes/dma/esp_aes_dma_core.c`：TLS 记录的
AES-GCM 解密要申请 `MALLOC_CAP_DMA` 的**内部** RAM（DMA 描述符 / 对齐缓冲），
申请失败 ⇒ 这条 record 解不开 ⇒ 读失败 ⇒ 判断流。`http` 台（`y` 键）不走 AES，没有这条错误。

与本次静音改动**无关**（本次只动 UI 层 + 两个静态变量），是既有的内部 DMA 内存紧张问题。
**根因已在 §13 定位并修复**（Bluedroid 常驻吃内部 RAM）。

---

## 13. 播放时整屏不刷新：Bluedroid 吃掉了内部 DMA（2026-10-27）

### 13.1 现象

改完手势后烧录，启动正常，但一进播放就开始刷屏报错，持续到屏幕完全不再更新：

```
E spi_master: setup_dma_priv_buffer(1206): Failed to allocate priv TX buffer
E lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E lcd: draw_bitmap FAILED x:87 y:213 w:185 h:28 err=0x101 ext=1 bytes=10360
E lcd:   internal DMA largest_free=3840 bytes free=6175 bytes (chunk=512 x queue=2)
```

`err=0x101` = `ESP_ERR_NO_MEM`，`ext=1` 表示像素缓冲在 PSRAM。

### 13.2 为什么 3840 字节就失败

PSRAM 不在 DMA 窗口内，`spi_master` 必须为**每个在途事务**拷一块内部 DMA 弹跳缓冲：

- `esp_lcd_panel_io_spi` 的 tx 循环按 `spi_bus_get_max_transaction_len()` 分片，实测量级 **4092B**；
- `setup_priv_desc()` → `setup_dma_priv_buffer()` 的分配长度是**分片长度**，不是画区大小；
- `LCD_SPI_TRANS_QUEUE_SZ = 2` ⇒ 需要 **8192B 连续块**（见 `sdgoods_lcd.h` 的弹跳预算说明）。

而 TLS 建起来后 `largest_free` 只剩 **3840B**，连一片 4092 都拿不到 ⇒ 该帧整块不上屏。
LVGL 的脏区会合并，播放时电平条 + meta 行合并成 185×28 一大块（10360B 数据、21 个分片），
于是**每 100ms 的刷新全部失败，屏幕冻结**。

> 这个坑一直在，只是要同时满足「HTTPS 流 + 大区域刷新」才暴露：启动阶段画的是小块，
> `largest_free` 还有 5888，勉强够；目录加载走两轮 HTTPS 后就掉到 3840 了。

### 13.3 元凶是 BLE，不是 WiFi

修前/修后同一位置的内存账：

| 位置 | 修前 | 修后 |
| --- | --- | --- |
| `sdgoods_lvgl` init 时内部 DMA 最大连续块 | 65536 B | 94208 B |
| `app_store` 前 / 后 | 5888 / 5120 B | 31744 / 31744 B |
| `UI_RADIO` tick 时的内部空闲总量 | 13215 B | **96755 B** |
| 播放 + TLS + 目录加载后 | 3840 B（刷新全失败） | 充裕（零失败） |

内部 RAM 总量约 156KiB（`heap_init` 日志）。唯一变量是 `main.c` 里的
`ESP_ERROR_CHECK(sdgoods_ble_init())` —— 这一行启动 Bluedroid + BT controller，常驻约 **80KB**。

而 `sdgoods_ble_list()`（扫 3 个 BLE 设备名）在本工程**零调用**：城市电台不扫描、不联机，
BLE 是平台基础工程默认带过来的。

### 13.4 修法

`main/main.c`：不调用 `sdgoods_ble_init()`（保留函数与头文件，只是不启动）。
额外收益：链接器把整条 BLE 调用链 GC 掉，固件 **0x2c9650 → 0x286710（-264KB）**，
分区余量 7% → **16%**。

```
// ESP_ERROR_CHECK(sdgoods_ble_init());
```

要恢复蓝牙扫描时把那行取消注释即可（`sdgoods_ble.c` 没删）。

### 13.5 验收

```
I sdgoods_lvgl: internal DMA largest free block: 94208 bytes
I app_store: internal DMA largest_free: before=31744 after=31744 bytes
I UI_RADIO: tick alive (wifi=1, pending=1, int free=96755)
I audio: stream start: 48000 Hz ch=2 vol=8%
I radio_player: playing: 48000 Hz ch=2 MP3
```

连续播放 20s：无 `spi_master` / `draw_bitmap` / `esp-aes` 任何一条错误。

### 13.6 顺带解决 §12.6

`esp-aes: Failed to allocate memory` 是同一块内部 DMA 不够：AES-GCM 的 DMA 描述符/对齐缓冲
申请失败 ⇒ record 解不开 ⇒ 读失败 ⇒ 判断流。BLE 关掉后余量回来，这条错误消失，
HTTPS 台不再周期性断流重连。

---

## 14. 主屏重写：先解手势冲突（2026-10-27）

### 14.1 取消长按

`ui_radio.c` 的全屏 `s_drag` 原先同时挂 `PRESSED / PRESSING / RELEASED / LONG_PRESSED`，
其中 `LONG_PRESSED` 在 400ms 时**不看位移**无条件发出，而横滑要位移 > `DRAG_START`(18px)
才成立：

- 用户按住准备滑 → 400ms 先弹设置；
- 手指轻抖 18px → 直接进拖台，长按永远轮不到。

现在删掉 `drag_long_pressed`、`LV_EVENT_LONG_PRESSED`、`s_long_fired`，长按**不存在**：
点按 = 播放暂停，横滑 = 切台，两条路径互不抢。设置入口只留控制中心（临时保留 brand 点按，
待 §16 抽屉上线后移除）。

长按松手会走 `drag_released` 的无位移分支 = 播放/暂停，与点按一致。


## 15. 主屏重排：删掉「说了两遍」的四样东西（2026-10-27）

### 15.1 诊断

改动前主屏从上到下是：品牌字「城市电台」/ 时间 / WiFi 红点 → 点阵 → 台名 →
「城市 · 码率 · 序号」→「电台列表 · 26」胶囊 → 18 根电平条 → 4 个 dock 键。
问题不在元素丑，在于**同一件事说了两遍**：

| 重复项 | 第一遍 | 第二遍 | 处理 |
| --- | --- | --- | --- |
| 城市 | 左上品牌字「城市电台」 | meta 首段 | 两者都删（台名自带地域） |
| 台序 | 环（红针位置） | 胶囊 / 「02 / 26」读数 | 只留环 + 点阵数字 |
| 在线 | WiFi 红点 | meta 里的连接词 | 合成状态行一个「● 直播中」 |

### 15.2 新的信息分层

```
y=54   16:37 · CITY RADIO ········ ● 直播中     页头（时间 / 品牌小字 / 状态）
y=88   ────────────────────────────
y=96   91                                    点阵：唯一焦点
y=170  杭州市广播电视台西湖之声                  台名（20px）
y=202  MP3 · 48 kHz                           副信息（真参数）
y=230  ▮▮▮▮▮▮▮▮▮▮▮▮                      电平（12 根）
y=262  [☰ 68] [◀ 128] [▮▮ 188] [▶ 248]      dock（等距 60）
```

### 15.3 页头：圆屏顶部的硬约束

环半径 168，圆心 (180,180)。y=54 那一行圆上只有 **x∈[70,290]** 在环内，
再往外就是屏幕黑角。上一版把时间放在 x=56（`brand` 热区从 52 起），实拍被红弧
从左边切过一笔。这一版：`brand` 收到 (70,54)，状态右对齐到 282，CITY RADIO 从
x=122 起，三段都退回弧内，两端留 6px 呼吸。

### 15.4 meta：把「网络电台」换成真参数

原代码在 `bitrate == 0` 时兜底填「网络电台」—— 本机只播网络台，这句话恒为真也恒无用。
现在按优先级取真值，全无则**留白**：

1. `codec · 采样率`（如 `MP3 · 48 kHz`）—— 采样率来自 `radio_player_sample_rate()`，
   解码首帧才有；
2. 只要采样率 → `48 kHz`；
3. 只要码率 → `64 kbps`；
4. 只有编码 → `MP3`。

顺带修了 `radio_player_play()`：换台时把 `s_rate` / `s_ch` 清零。原先这俩是静态变量、
只在解码首帧赋值，换台后新台解码前 UI 会拿**上一路的采样率**冒充新台参数。

### 15.5 环：红色从 77% 退到一颗针

| | 改动前 | 第一次改（仅缩线宽） | 定稿 |
| --- | --- | --- | --- |
| 轨道 | hair 3px | hair 2px | hair↔muted 混合 2px |
| 进度 | accent 4px，从起点染到当前 | accent 2px | **muted 2px**（不红） |
| 端点 | accent 10px 圆 | accent 10px | accent 10px（**整屏唯一的红**） |

只把宽度从 4px 降到 2px 时实拍发现：红弧仍盖住 77% 周长，还是第一条被看到的线，
点阵反而退居第二。定稿改成「已走 muted / 未走 淡灰 / 针 accent」——用两种灰分进度，
红色只留端点。同时发现纯 `hair`(#2A2A28) 在 `bg`(#0C0C0D) 上对比只有 1.6:1，
轨道几乎隐形、环退化成一颗浮着的红点，故轨道取 `lv_color_mix(muted, hair, 110)`。

### 15.6 dock：4 键等距内收

```c
DOCK_X0 60 → 68     两侧从 60/252 收到 68/248，中心 92/152/212/272（等距 60），
DOCK_X1 124 → 128    两侧键到环线的余量从 2.5px 拉到 8px，不再贴弧。
DOCK_X2 188 → 188
DOCK_X3 252 → 248
```

### 15.7 验收

- 固件 `0x286750`（分区余量 16%），两套主题各截一张：
  - 暗色：`shot_20261027_163628.jpg`
  - 亮色：`shot_20261027_163744.jpg`
- 核对项：页头两端不越弧、台名 12 字不截断、电平条两端不顶环、4 键不出圆、
  红针在 1/26 时落在左下 120° 且不压列表键。
- 已验证「网络电台」→ `MP3 · 48 kHz`；`bitrate` 有值时回退到 `64 kbps`。

### 15.8 还没做的

- 抽屉（下拉设置）仍是平台 `sdgoods_cc.c` 那套，`ui_radio_drawer.c` 未开工；
  设置入口暂时是「点页头左侧时间」（qa `'k'` 坐标 (100,66)）。
- 拖拽切台仍是隐式（无拖拽条），`ui_radio_slider.c` 未开工。


## 16. 页头三读数 · 列表改上滑 · 关闭键加大（2026-10-27）

### 16.1 页头：`CITY RADIO` → 时间 / 电量 / 直播中

品牌字从页头撤掉。它在圆屏顶上只占 60px，却把这一行挤成「品牌 + 时间 + 状态」三段偏左的排布；
品牌本身又是重复信息（整机只跑这一个 app）。腾出来的位置换成**三个实时读数**：

```
70        114 146      210  228      282
 16:52        [▮] 100%      ● 直播中
```

三段中心落在 92 / 178 / 255（间隔 86 / 77），整行仍在 `x∈[70,290]` 的环内可见区里。

电量是**电池图形 + 百分比**两件套，不用一个 `87%` 文字了事：

- 外框 18×10 + 正极 2×4，1px `muted` 描边；
- 填充条 `BAT_W-4`，宽度按比例改，最小 2px（整条消失会被读成「没装电池」）；
- `pct<=20` 时填充与外框一起转 `accent`；`pct<0`（平台返回 -1，ADC 读失败）填空宽 + `muted`。
- 采样按 5s 节流（`radio_tick` 里 `now - s_bat_ms`）：ADC 比时钟贵得多，电量也没有秒级变化。

两个踩过的坑：

1. **描边别用 60% 透明度**。18×10 这个尺寸下 60% 的 `muted` 在 `bg` 上几乎不可见，实拍时电池退化成
   「一块白条 + 一枚小点」。必须给满不透明，轮廓才立得住。
2. **`radio_player_sample_rate()` 换台要清零**（在 `radio_player_play()` 里和 `s_seq++` 一起做），
   否则 `meta` 行会把上一路的 48 kHz 报给新台。

### 16.2 电台列表：从「dock 第四个键」改成底部上滑

列表键从 dock 拿掉后 dock 收回 3 键（`DOCK_X0/1/2` = 88/156/224，`DOCK_Y` = 272），
等距 68、重心正好在圆心 x=180 —— 比原来 4 键一直偏左的排布稳。

上滑是**应用自己实现**的：平台 `sdgoods_app_shell.c` 的 `BOTTOM_ZONE` 只用于关闭菜单浮层，
不是 app 屏手势（`TOP_ZONE` 才是，它绑的是控制中心）。做法：

```c
/* s_drag 之后、dock 之前建 —— 后建的命中优先级高，dock 三键才不会被带走 */
s_swipe = rk_box(s_scr, 0, LCD_HEIGHT - SWIPE_ZONE, LCD_WIDTH, SWIPE_ZONE);
```

判定只认两条：起手点落在最后 60px 内（`s_swipe_y0 >= 300`）+ 向上位移过 46px。
没推过阈就什么都不发生，所以不会「想点播放却开了列表」。

**可发现性**：dock 少了一个键，页面上就再没有任何「这里能滑」的提示了。
底部中央加一根 18×3 的短横（`mix(muted, bg, 90)`）当移动端通用语汇。
它是 `s_swipe` 的子对象（不新增待清理指针），`rk_box` 默认不可点（不抢手势）。

局限：上滑开的页**只能用 X 关**，没有对称的下滑关。列表页 X 现在很好按（见 16.3），
暂不补；真要补得在 `rk_page_*` 层加手势，不属本次范围。

### 16.3 页头关闭键：34px 浮图标 → 44px 圆按钮

```c
// 改前：34px 裸图标贴 (274,58)
lv_obj_t *close = rk_icon(p->root, 34, RK_ICON_CLOSE, th->muted);
lv_obj_set_pos(close, 274, 58);

// 改后：44×44 圆底，往里收到 (232,43)
lv_obj_t *close = rk_round_btn(p->root, 232, 43, 44, RK_ICON_CLOSE,
                               lv_color_mix(th->muted, th->bg, 70), th->muted);
```

「难点到」不是错觉，是几何问题：环线在 y=58 处只到 `x≈289`，原来的 34px 图标占 274..308，
**右半截连同命中区一起坐在圆屏黑角上**，手指按过去基本落在纯黑区域。

新位置的三个约束都是算过的：

- 右上角 (276,43) 到圆心 = `sqrt(96² + 137²) ≈ 167` < 168，不出环；
- 底边 86 与 y=90 的分隔线留 4px，线不从按钮底下穿出来（取 y=45 时会贴住）；
- 底色取 `mix(muted, bg, 70)`（约 27% 的 muted）：再亮一档就成整页最重的色块，抢过标题。

同步调整：标题宽 130 → 96（`lv_obj_set_pos` 上移到 58 与按钮中心齐）、
`rk_page_action` 从 (196,62) 72×24 改到 (160,56) 56×26 —— 原来的位置会和新按钮重叠。

### 16.4 验收

- 暗色主屏：`shot_20261027_165621.jpg`
- 暗色列表页：`shot_20261027_170008.jpg`
- 暗色设置页：`shot_20261027_170031.jpg`
- 亮色主屏：`shot_20261027_170048.jpg`
- 亮色列表页：`shot_20261027_170105.jpg`
- 核对项：页头三段不越弧、电池轮廓在暗底可辨、底部手势条可见、
  上滑能开列表（qa `'l'` 合成 64px 上推）、三键等距居中、X 与分隔线不叠。

### 16.5 新发现，未做

- 列表行台名截断（「杭州市广播电视台…」）：`rk_page_row_net` 的 `label_w` 固定 160，
  而这些城市台名普遍 9 字以上。要么减字号，要么把 `bitrate` 移到副行。
- 上滑开的页没有下滑关的手势。


## 17. 播放/暂停「点了没反应」：一次点击三种时序（2026-10-27）

### 17.1 症状与错位

点 dock 中间键，屏幕上什么也不变，过一截才突然切成暂停。
但同一次点击，**页头的「已暂停」和电平条的消失是立即的**，只有那颗图标晚一拍 ——
说明不是触摸丢帧，是**同一次状态变更被拆成了两条时序**。

查下来三处各自为政：

| 界面元素 | 判断依据 | 生效时机 |
|---|---|---|
| 电平条 `update_bars` | `s_paused`（UI 本地意图） | 立即 |
| 状态词 `live_word` / 红点 | `s_paused` | 立即 |
| dock 图标 `set_play_icon` | `radio_player_state()` | **等 rp_task** |

`radio_player_set_paused(true)` 过去只置 `s_paused` 标志，**不动 `s_state`**；真正的
`RP_PAUSED` 要等解码循环跑到下一轮才写。而那一轮可能正阻塞在：

1. 一次 `sdgoods_audio_stream_write()`（一帧 MP3 1152 samples × 4B ≈ 24ms）；
2. 或者更久：内层 `while (fill - consumed > 0 && s_seq == sess_seq)` **不检查 s_paused**，
   会把这一轮从环形缓冲读到的整块 raw（最大 `RP_RAW_CAP`）全部解完写完才回到外层。

再叠加 `radio_tick` 的 120ms 轮询和整屏重绘，就是可感知的「卡住」。

### 17.2 三处修复

```c
// 1) UI 改为认意图，不认异步状态
const bool playing = !s_paused &&
                     (state == RP_PLAYING || state == RP_CONNECTING || state == RP_BUFFERING);
set_play_icon(playing);

// 2) 暂停指令立即宣告（radio_player.c）
if (paused) {
    if (s_state == RP_PLAYING) { s_state = RP_PAUSED; }
}

// 3) 解码内层循环每帧先看暂停，别把积压 raw 一次喂完
while (fill - consumed > 0 && s_seq == sess_seq) {
    if (s_paused) { break; }
    ...
}
```

⚠️ 第 2 条**只改 `RP_PLAYING`**。若把 `BUFFERING/CONNECTING` 也标成 `PAUSED`，恢复时
主循环开头那句 `if (s_state == RP_PAUSED) s_state = RP_PLAYING;` 会把「还没出声」误置成
PLAYING —— 状态比事实超前，比慢半拍更糟。

第 3 条的 `break` 是安全的：未消费的尾巴会在 `memmove` 前移，恢复播放后接着解，不丢数据。

### 17.3 按下反馈

`rk_round_btn` 原来只改 `bg_opa`（→ 70%）。反色的播放键按下去只是「底色淡一点」，图标仍全亮，
实拍几乎看不出这一下有没有被接住。改成 obj 级 `opa` 60%，底色和图标一起下沉。

### 17.4 量化验证

加了两个带 `lv_tick` 的日志（长期保留）：

```
toggle tap @61976 (state=4 paused=1)
play icon -> pause @61976      ← 0ms，恢复播放

toggle tap @63166 (state=3 paused=0)
play icon -> play  @63168      ← 2ms，暂停
```

修复前图标要等 100ms 量级 + 120ms tick 轮询。新增 qa `'p'`（合成点 dock 播放键）做回归。

顺带：`set_play_icon` 加了「图标没变就不 invalidate」，因为 `refresh_ui` 在一轮 tick 里
可能被调用多次，原来每次都重绘那颗 48×48。

### 17.5 验收

- `shot_20261027_171122.jpg`（亮色，暂停态）：页头「已暂停」+ 灰点、播放键回▶、
  环变暗、电平条消失 —— 四项同帧到位，没有先后差。
- 还没做：图标翻转本身仍是硬切（无淡入/缩放动画）。如果还觉得「生硬」，
  下一步可以给 `set_play_icon` 套一个 120ms 的 opa 补间，而不是先动真格重写 dock。




