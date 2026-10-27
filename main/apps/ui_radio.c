/*
 * sdgoods-radio · 城市电台主屏（应用层）
 *
 * 设计见 docs/UI_REDESIGN.md。这里只做三件事：
 *   ① 搭主屏（表盘环 / 点阵英雄 / 台名元信息 / dock）；
 *   ② 把触摸翻译成意图（点按 = 播放暂停，横向拖拽 = 调台）；
 *      长按刻意不做任何事：它与横滑抢同一块触摸区，见下方手势段注释。
 *   ③ 每 tick 把播放器状态、目录状态、时钟、电平、休眠映射成界面。
 *
 * 页面（列表/设置/选择器/WiFi）在 ui_radio_pages.c；绘制原语在 ui_radio_kit.c。
 * 主题/城市变化一律「重建屏幕」：播放器不动，播放不中断。
 */

#include "ui_radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "radio_catalog.h"
#include "radio_player.h"
#include "radio_settings.h"
#include "sdgoods_app_shell.h"
#include "sdgoods_audio.h"
#include "sdgoods_cc.h"
#include "sdgoods_lcd.h"
#include "ui_radio_kit.h"
#include "ui_radio_pages.h"
#include "wifi_prov.h"

static const char *TAG = "UI_RADIO";

/* ---- 布局（360×360 圆屏，圆心 180,180；顶部 50px 留给控制中心手势）----
 *
 * 信息分层（主屏重写）：状态行 → 点阵（唯一焦点）→ 台名 → 副信息 → 电平/环 → dock。
 * 页头只放三个实时读数：时间 / 电量 / 播放状态。品牌小字「CITY RADIO」已删：
 * 这一行在圆屏顶上只有 212px 可用，三个读数本来就快站满，品牌字价值最低。
 *
 * 圆屏的硬约束：文字行越靠上，左右可用宽度越窄。环半径 168，y=56 那一行
 * 只有 x∈[70,290] 可用（再往外就是黑角），所以页头左右都往里收，不贴边。 */
#define HERO_X       30
#define HERO_Y       96
#define HERO_W       300
#define HERO_H       66
#define NAME_Y       170
#define META_Y       202
#define BARS_BASE    242
#define BARS_N       12          /* 18 → 12：缩短整行，两端不再顶到环 */
#define BARS_W       6
#define BARS_GAP     6
#define BARS_MAX_H   12          /* 单根最高高度 18 → 12，降为中景 */
#define RING_SIZE    360
#define RING_R       168
#define BAT_X        146         /* 电池外框：页头中段，与时间/状态三等分 */
#define BAT_Y        59
#define BAT_W        18
#define BAT_H        10
#define DOCK_X0      88          /* 三键同一行，中心 112 / 180 / 248（等距 68） */
#define DOCK_X1      156
#define DOCK_X2      224
#define DOCK_Y       272         /* 中心 y=296：到圆心 134，外缘 158 < 168，不出环 */
#define DOCK_S       48

/* 底部上滑：拉起电台列表。
 * 平台的 BOTTOM_ZONE 只用于关闭菜单浮层，不是 app 屏手势；列表页原先靠 dock 里的
 * 一个图标键，占掉四个 action 里的一个。改成「从底部往上推」——和顶部下滑进控制
 * 中心对称，dock 就腾出位置给播放三键。 */
#define SWIPE_ZONE   60
#define SWIPE_UP_DY  46

#define SKIP_MAX     3          /* 连续失败跳台上限 */
#define DRAG_STEP    26         /* 每 26px 换一台 */
#define DRAG_START   18         /* 超过这个位移才进入拖拽态 */

/* ---- 状态 ---- */
static lv_obj_t   *s_scr;
static lv_timer_t *s_tick;
static lv_obj_t   *s_drag;
static rk_dot_t    s_dot;
static rk_ring_t   s_ring;
static lv_obj_t   *s_name;
static lv_obj_t   *s_meta;
static lv_obj_t   *s_clock;
static lv_obj_t   *s_live_dot;    /* 状态行前的红点：只有「在播」才亮 */
static lv_obj_t   *s_live_lbl;    /* 直播中 / 缓冲中 / 已暂停 / 未连接 */
static lv_obj_t   *s_bat_shell;   /* 电池外框（低电量时边框转红） */
static lv_obj_t   *s_bat_fill;    /* 电池内部填充条，宽度随电量 */
static lv_obj_t   *s_bat_lbl;     /* 87% */
static lv_obj_t   *s_swipe;       /* 底部上滑捕获带 */
static lv_obj_t   *s_play_btn;
static lv_obj_t   *s_bars[BARS_N];

static char s_last_hero[12];
static char s_last_name[RADIO_NAME_MAX + 4];
static char s_last_meta[80];
static char s_last_clock[8];
static char s_last_live[16];

static int  s_cur_idx = -1;
static int  s_preview_idx = -1;
static bool s_paused;
static bool s_dragging;
static lv_coord_t s_touch_x, s_touch_y;
static radio_player_state_t s_last_state = RP_IDLE;

static bool s_catalog_ready;          /* 至少成功加载过一次目录 */
static bool s_catalog_pending = true;
static volatile bool s_catalog_done;
static volatile bool s_catalog_running;
static int  s_skip_streak;
static bool s_auto_wifi;              /* 开机未配网：首个 tick 自动弹 WiFi 页 */

static char s_city[20];               /* "" = 自动定位 */
static bool s_tick_seen;
static int  s_sleep_min;
static bool s_sleep_armed;
static uint32_t s_sleep_deadline;
static uint32_t s_clock_ms;
static bool s_ntp_started;
static bool s_wifi_seen;
static lv_coord_t s_swipe_y0;          /* 底部上滑的按下 y */
static int  s_bat_pct = -2;           /* -2 未读过，-1 读失败，0..100 */
static uint32_t s_bat_ms;
static char s_last_bat[8];

/* 点按上下文（静态存储，生命周期覆盖对应对象） */
static sdgoods_tap_ctx_t s_ctx_prev, s_ctx_next, s_ctx_play, s_ctx_brand;

static void build_screen(void);
static void refresh_ui(bool preview);
static void play_index(int idx);
static void open_settings(void);

/* ==========================================================================
 * 小工具
 * ======================================================================== */
static void set_text(lv_obj_t *lbl, char *cache, size_t cap, const char *text)
{
    if (!lbl || !text) {
        return;
    }
    if (strncmp(cache, text, cap) == 0) {
        return;
    }
    snprintf(cache, cap, "%s", text);
    lv_label_set_text(lbl, cache);
}

/* 状态行右侧那一词：整屏唯一的状态出口（不再散落在 meta 里）。
 * 带「直播中」时前面的点才是红色 —— 其余一律 hair，避免红色被普通状态稀释。 */
static const char *live_word(radio_player_state_t st, bool paused)
{
    if (wifi_prov_get_state() != WIFI_STATE_CONNECTED) return "未连接";
    if (paused || st == RP_PAUSED) return "已暂停";
    switch (st) {
    case RP_CONNECTING: return "连接中";
    case RP_BUFFERING:  return "缓冲中";
    case RP_ERROR:      return "重连中";
    case RP_PLAYING:    return "直播中";
    default:            return "待播放";
    }
}

static void set_play_icon(bool playing)
{
    if (!s_play_btn) {
        return;
    }
    lv_obj_t *ic = lv_obj_get_child(s_play_btn, 0);
    if (!ic) {
        return;
    }
    const rk_icon_t want = playing ? RK_ICON_PAUSE : RK_ICON_PLAY;
    if ((rk_icon_t)(uintptr_t)lv_obj_get_user_data(ic) == want) {
        return;                 /* 没变就别 invalidate：refresh_ui 可能一轮跑好几次 */
    }
    lv_obj_set_user_data(ic, (void *)(uintptr_t)want);
    lv_obj_invalidate(ic);
    /* 带毫秒时间戳：和下方 toggle 日志对一下，就能量出「点下去到图标翻」的真实延迟 */
    ESP_LOGI(TAG, "play icon -> %s @%u", playing ? "pause" : "play",
             (unsigned)lv_tick_get());
}

/* ==========================================================================
 * 目录
 * ======================================================================== */
static void catalog_task(void *arg)
{
    (void)arg;
    const char *city = s_city[0] ? s_city : NULL;
    int n = radio_catalog_load(city);
    ESP_LOGI(TAG, "catalog: %d stations (city=%s cc=%s)", n, city ? city : "auto",
             radio_catalog_country());
    s_catalog_running = false;
    s_catalog_done = true;
    rk_task_exit();
}

static void start_catalog(void)
{
    if (s_catalog_running) {
        return;
    }
    s_catalog_running = true;
    s_catalog_done = false;
    if (!rk_task_spawn(catalog_task, "radio_cat", 10240, NULL, 4)) {
        s_catalog_running = false;
        s_catalog_pending = true;      /* 栈拿不到：下一 tick 再试，不要卡死在「加载中」 */
        ESP_LOGW(TAG, "catalog task create failed (int free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        ESP_LOGI(TAG, "catalog task started");
    }
}

/* ==========================================================================
 * 播放
 * ======================================================================== */
static void arm_sleep(void)
{
    if (s_sleep_min > 0) {
        s_sleep_deadline = lv_tick_get() + (uint32_t)s_sleep_min * 60000u;
        s_sleep_armed = true;
    } else {
        s_sleep_armed = false;
    }
}

static void play_index(int idx)
{
    const radio_station_t *st = radio_catalog_at(idx);
    if (!st || st->url[0] == '\0') {
        return;
    }
    s_cur_idx = idx;
    s_preview_idx = idx;
    s_skip_streak = 0;
    s_paused = false;
    radio_player_play(st->url, st->codec[0] ? st->codec : NULL);
    radio_settings_set_last_index(idx);
    arm_sleep();
    refresh_ui(false);
    rk_fade_in(s_name);
    ESP_LOGI(TAG, "play #%d %s", idx, st->name);
}

static void step_play(int dir)
{
    int n = radio_catalog_count();
    if (n <= 0) {
        return;
    }
    int base = s_cur_idx < 0 ? 0 : s_cur_idx;
    int idx = ((base + dir) % n + n) % n;
    play_index(idx);
}

static void toggle_play(void)
{
    radio_player_state_t st = radio_player_state();
    ESP_LOGI(TAG, "toggle tap @%u (state=%d paused=%d)", (unsigned)lv_tick_get(),
             (int)st, (int)s_paused);
    if (radio_catalog_count() <= 0) {
        open_settings();
        return;
    }
    if (st == RP_PAUSED) {
        radio_player_set_paused(false);
        s_paused = false;
        arm_sleep();
    } else if (st == RP_PLAYING || st == RP_CONNECTING || st == RP_BUFFERING) {
        radio_player_set_paused(true);
        s_paused = true;
    } else {
        if (s_cur_idx < 0) {
            s_cur_idx = 0;
        }
        play_index(s_cur_idx);
    }
    refresh_ui(false);
}

/* ==========================================================================
 * 触摸：点按 = 播放暂停；横向拖拽 = 调台（实时预览，松手生效）
 *
 * ⚠️ 这里**没有长按**。原先「长按 = 设置」与「按住左右滑 = 切台」抢同一块触摸区：
 *    LVGL 的 LONG_PRESSED 在 400ms 时**不看位移**就发出，而横滑要位移 > DRAG_START
 *    才成立 ⇒ 用户按住准备滑，400ms 一到设置就先弹出来了；反过来手指轻抖一点又
 *    直接进拖台，长按永远轮不到。现在两个手势各自独占一条判定路径：位移优先，
 *    长按彻底不存在（设置只从控制中心进）。
 * ======================================================================== */
static void drag_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_get_act();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    s_touch_x = p.x;
    s_touch_y = p.y;
    s_dragging = false;
    s_preview_idx = s_cur_idx;
}

static void drag_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_scr || radio_pages_any_open()) {
        return;
    }
    lv_indev_t *in = lv_indev_get_act();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int dx = (int)p.x - (int)s_touch_x;

    if (!s_dragging) {
        if (LV_ABS(dx) < DRAG_START) {
            return;
        }
        s_dragging = true;
    }

    int n = radio_catalog_count();
    if (n <= 0) {
        return;
    }
    int base = s_cur_idx < 0 ? 0 : s_cur_idx;
    int step = -dx / DRAG_STEP;
    int idx = base + step;
    idx = ((idx % n) + n) % n;
    if (idx != s_preview_idx) {
        s_preview_idx = idx;
        refresh_ui(true);
    }
}

static void drag_released(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_get_act();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    int dx = (int)p.x - (int)s_touch_x;
    int dy = (int)p.y - (int)s_touch_y;

    if (s_dragging) {
        s_dragging = false;
        if (s_preview_idx >= 0 && s_preview_idx != s_cur_idx) {
            play_index(s_preview_idx);
        } else {
            s_preview_idx = s_cur_idx;
            refresh_ui(false);
        }
        return;
    }
    if (LV_ABS(dx) <= SDGOODS_TAP_SLOP && LV_ABS(dy) <= SDGOODS_TAP_SLOP && !radio_pages_any_open()) {
        toggle_play();
    }
}

static void drag_press_lost(lv_event_t *e)
{
    (void)e;
    if (s_dragging) {
        s_dragging = false;
        s_preview_idx = s_cur_idx;
        refresh_ui(false);
    }
}

/* dock / 胶囊 */
static void on_prev(void *ud)
{
    (void)ud;
    step_play(-1);
}

static void on_next(void *ud)
{
    (void)ud;
    step_play(+1);
}

static void on_play(void *ud)
{
    (void)ud;
    toggle_play();
}

/* 底部上滑 → 电台列表。只认「从捕获带内起手 + 向上位移过阈」，横向拖拽不触发。 */
static void swipe_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_get_act();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    s_swipe_y0 = p.y;
}

static void swipe_released(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_get_act();
    if (!in || !s_scr || radio_pages_any_open()) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (s_swipe_y0 >= LCD_HEIGHT - SWIPE_ZONE &&
        (int)p.y - (int)s_swipe_y0 <= -SWIPE_UP_DY) {
        radio_pages_open_list(s_scr);
    }
}

static void open_settings(void)
{
    if (!s_scr) {
        return;
    }
    radio_pages_open_settings(s_scr);
}

static void on_brand(void *ud)
{
    (void)ud;
    open_settings();
}

/* ==========================================================================
 * 界面刷新
 * ======================================================================== */
static void refresh_ui(bool preview)
{
    if (!s_scr) {
        return;
    }
    int n = radio_catalog_count();
    int idx = preview ? s_preview_idx : s_cur_idx;
    const radio_station_t *st = radio_catalog_at(idx);
    radio_player_state_t state = radio_player_state();

    /* 点阵英雄：有 FM 频率就显示频率；纯网络台没有频率，就用频道号（纯数字在
     * 5×7 点阵里比 CH 字样清楚得多，也不依赖任何字形）——主视觉永远不空 */
    char hero[12];
    if (!s_catalog_ready && n == 0) {
        snprintf(hero, sizeof(hero), "....");
    } else if (st && st->freq_mhz > 0.0f) {
        snprintf(hero, sizeof(hero), "%.1f", st->freq_mhz);
    } else if (st) {
        snprintf(hero, sizeof(hero), "%02u", (unsigned)(idx + 1) % 100u);
    } else {
        snprintf(hero, sizeof(hero), "----");
    }
    if (strcmp(hero, s_last_hero) != 0) {
        snprintf(s_last_hero, sizeof(s_last_hero), "%s", hero);
        rk_dot_set_text(&s_dot, hero);
    }

    /* 表盘环 */
    rk_ring_set(&s_ring, (n > 0 && idx >= 0) ? idx : -1, n);
    rk_ring_set_dim(&s_ring, state == RP_PAUSED || s_paused || state == RP_ERROR);

    /* 台名 */
    char name[RADIO_NAME_MAX + 4];
    if (st) {
        rk_text_safe(st->name, name, sizeof(name), RK_FONT_TITLE);
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "电台 %d", idx + 1);
        }
    } else if (!s_catalog_ready) {
        snprintf(name, sizeof(name), "城市电台");
    } else {
        snprintf(name, sizeof(name), "还没有电台");
    }
    set_text(s_name, s_last_name, sizeof(s_last_name), name);

    /* 状态行：整屏唯一的状态出口。红点只在真正出声时亮。 */
    if (s_live_lbl) {
        set_text(s_live_lbl, s_last_live, sizeof(s_last_live), live_word(state, s_paused));
    }
    if (s_live_dot) {
        const bool live = (state == RP_PLAYING && !s_paused);
        lv_obj_set_style_bg_color(s_live_dot, live ? rk_theme()->accent : rk_theme()->hair, 0);
    }

    /* 副信息：编码 + 实际采样率（解码首帧后才知道，之前回退到码率）。
     * 这里曾经写死「网络电台」—— 本机只播网络台，这句话恒为真也恒无用，
     * 属于占了版面不传信息的填充字。改成一条真实的技术参数，宁可留白。 */
    char meta[32];
    meta[0] = '\0';
    if (!s_catalog_ready && n == 0) {
        snprintf(meta, sizeof(meta), "正在获取…");
    } else if (n == 0) {
        snprintf(meta, sizeof(meta), "请先连接 WiFi");
    } else {
        const char *codec = (st && st->codec[0]) ? st->codec : NULL;
        uint32_t sr = radio_player_sample_rate();
        if (codec && sr > 0) {
            snprintf(meta, sizeof(meta), "%s · %u kHz", codec, (unsigned)(sr / 1000));
        } else if (sr > 0) {
            snprintf(meta, sizeof(meta), "%u kHz", (unsigned)(sr / 1000));
        } else if (st && st->bitrate > 0) {
            snprintf(meta, sizeof(meta), "%u kbps", (unsigned)st->bitrate);
        } else if (codec) {
            snprintf(meta, sizeof(meta), "%s", codec);
        }
    }
    set_text(s_meta, s_last_meta, sizeof(s_last_meta), meta);

    /* dock 播放图标：以「用户意图」为准（s_paused），不能只读 state。
     * radio_player_set_paused() 过去只置 s_paused，真正的 s_state 翻转要等解码
     * 循环跑到下一轮 —— 而那一轮可能正阻塞在一次 i2s 写里，于是同一次点击在
     * 屏幕上出现两种时序：电平条和状态词（它们本来就用 s_paused）立即变，
     * 只有这颗图标晚一拍才翻，手感就是「点了没反应，过一会才切」。 */
    const bool playing = !s_paused &&
                         (state == RP_PLAYING || state == RP_CONNECTING || state == RP_BUFFERING);
    set_play_icon(playing);
}

static void update_bars(radio_player_state_t st)
{
    bool show = (st == RP_PLAYING && !s_paused);
    uint8_t lv[BARS_N];
    int n = radio_player_levels(lv, BARS_N);
    for (int i = 0; i < BARS_N; i++) {
        if (!show) {
            lv_obj_add_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
        int v = (i < n) ? lv[i] : 0;
        lv_coord_t h = 2 + (BARS_MAX_H - 2) * v / 255;
        lv_obj_set_height(s_bars[i], h);
        lv_obj_set_y(s_bars[i], BARS_BASE - h);
    }
}

static void update_clock(void)
{
    if (!s_clock) {
        return;
    }
    char buf[8];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2024) {
        snprintf(buf, sizeof(buf), "--:--");
    } else {
        snprintf(buf, sizeof(buf), "%d:%02d", tm_now.tm_hour, tm_now.tm_min);
    }
    set_text(s_clock, s_last_clock, sizeof(s_last_clock), buf);
}

static void clock_maybe_start(void)
{
    if (s_ntp_started) {
        return;
    }
    s_ntp_started = true;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "sntp started");
}

/* 电池：一次采样改三处（填充条宽度 / 低电量转红 / 百分比文字）。
 * ADC 不能每秒采，调用方按 5s 节流。pct<0 = 读失败（平台层返回 -1），
 * 此时填充条画满但转 muted，避免「读不到」被误认为「满电」。 */
static void update_battery(void)
{
    int pct = sdgoods_cc_bat_read(NULL);
    if (pct > 100) {
        pct = 100;
    } else if (pct < 0) {
        pct = -1;
    }
    if (pct == s_bat_pct) {
        return;
    }
    s_bat_pct = pct;
    const rk_theme_t *th = rk_theme();
    const bool low = (pct >= 0 && pct <= 20);

    if (s_bat_lbl) {
        char buf[8];
        if (pct < 0) {
            snprintf(buf, sizeof(buf), "--%%");
        } else {
            snprintf(buf, sizeof(buf), "%d%%", pct);
        }
        set_text(s_bat_lbl, s_last_bat, sizeof(s_last_bat), buf);
    }
    if (s_bat_fill) {
        lv_coord_t full = BAT_W - 4;
        lv_coord_t w = (pct < 0) ? full : (lv_coord_t)(full * pct / 100);
        if (w < 2) {
            w = 2;              /* 不能整条消失：那是「没装电池」的观感 */
        }
        lv_obj_set_width(s_bat_fill, w);
        /* 读失败用 muted 填满 =「有电但读不到」；≤20% 用 accent 明说该充电 */
        lv_obj_set_style_bg_color(s_bat_fill,
                                  (pct < 0) ? th->muted : (low ? th->accent : th->ink), 0);
    }
    if (s_bat_shell) {
        lv_obj_set_style_border_color(s_bat_shell, low ? th->accent : th->muted, 0);
    }
}

/* ==========================================================================
 * 主屏构建
 * ======================================================================== */
static void build_screen(void)
{
    const rk_theme_t *th = rk_theme();

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, th->bg, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 表盘环在最底层 */
    rk_ring_create(&s_ring, s_scr, RING_SIZE, RING_R);

    /* 全屏拖拽/点按层：放在视觉元素之下、按钮之上（按钮后建 ⇒ 更靠前） */
    s_drag = rk_box(s_scr, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_add_flag(s_drag, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_drag, drag_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_drag, drag_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_drag, drag_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_drag, drag_press_lost, LV_EVENT_PRESS_LOST, NULL);

    /* 底部上滑捕获带：只取最后 60px。dock 的三个键在本对象之后创建，命中优先级
     * 更高，所以按播放键不会被这条带子吃掉；带内只做「向上推」判定，
     * 没推过阈则什么都不发生（不会误触播放暂停）。 */
    s_swipe = rk_box(s_scr, 0, LCD_HEIGHT - SWIPE_ZONE, LCD_WIDTH, SWIPE_ZONE);
    lv_obj_add_flag(s_swipe, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_swipe, swipe_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_swipe, swipe_released, LV_EVENT_RELEASED, NULL);

    /* 手势条：底部中央一根 18×3 的短横，移动端通用的「这页可上滑」语汇。
     * 列表键从 dock 拿走后，「怎么开列表」就没任何文字提示了，只能靠这根条。
     * 它是 s_swipe 的子对象 ⇒ 不新增指针；rk_box 默认不可点 ⇒ 不抢上滑手势。 */
    lv_obj_t *grip = rk_box(s_swipe, (LCD_WIDTH - 18) / 2, SWIPE_ZONE - 20, 18, 3);
    lv_obj_set_style_bg_color(grip, lv_color_mix(th->muted, th->bg, 90), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grip, 2, 0);

    /* 页头：时间（左）· 电量（中）· 播放状态（右）
     *
     * 圆屏顶部的可见宽度只有中间那一段：环半径 168，y=56 处 x 只到 70 和 290，
     * 再往外就是黑角。三段等分这 208px：时间从 70 起、状态右对齐 282、电量居中。
     * 品牌热区暂时保留——它是抽屉（§16）上线前唯一的「设置」入口。 */
    lv_obj_t *brand = rk_box(s_scr, 70, 54, 60, 24);
    lv_obj_add_flag(brand, LV_OBJ_FLAG_CLICKABLE);
    sdgoods_tap_bind(brand, &s_ctx_brand, (sdgoods_tap_cb_t)on_brand, NULL);

    s_clock = lv_label_create(brand);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_font(s_clock, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_clock, th->ink, 0);
    lv_obj_set_pos(s_clock, 0, 0);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);

    /* 电池：外框 + 正极 + 内部填充条。三截都是普通 rk_box，
     * 不新增任何绘制回调 —— 电量只是改填充条宽度。
     * 18×10 这个尺寸下边框必须给满不透明：60% 的 muted 在暗底上几乎不可见，
     * 实拍下电池就退化成「一块白条 + 一枚小点」，看不出是电池。 */
    s_bat_shell = rk_box(s_scr, BAT_X, BAT_Y, BAT_W, BAT_H);
    lv_obj_set_style_bg_opa(s_bat_shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bat_shell, 1, 0);
    lv_obj_set_style_border_color(s_bat_shell, th->muted, 0);
    lv_obj_set_style_border_opa(s_bat_shell, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bat_shell, 2, 0);
    lv_obj_set_style_pad_all(s_bat_shell, 0, 0);

    lv_obj_t *nub = rk_box(s_scr, BAT_X + BAT_W, BAT_Y + 3, 2, 4);
    lv_obj_set_style_bg_color(nub, th->muted, 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nub, 1, 0);

    s_bat_fill = rk_box(s_bat_shell, 2, 2, BAT_W - 4, BAT_H - 4);
    lv_obj_set_style_bg_color(s_bat_fill, th->ink, 0);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bat_fill, 1, 0);

    s_bat_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_bat_lbl, "--%");
    lv_obj_set_style_text_font(s_bat_lbl, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_bat_lbl, th->muted, 0);
    lv_obj_set_pos(s_bat_lbl, BAT_X + BAT_W + 6, 54);
    lv_obj_clear_flag(s_bat_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_live_dot = rk_box(s_scr, 228, 61, 6, 6);
    lv_obj_set_style_radius(s_live_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live_dot, th->hair, 0);
    lv_obj_set_style_bg_opa(s_live_dot, LV_OPA_COVER, 0);

    s_live_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_live_lbl, "");
    lv_obj_set_style_text_font(s_live_lbl, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_live_lbl, th->muted, 0);
    lv_obj_set_style_text_align(s_live_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_live_lbl, 234, 54);
    lv_obj_set_width(s_live_lbl, 48);
    lv_obj_clear_flag(s_live_lbl, LV_OBJ_FLAG_CLICKABLE);

    rk_hairline(s_scr, 62, 88, 236, th->hair);

    /* 点阵英雄 */
    rk_dot_create(&s_dot, s_scr, HERO_X, HERO_Y, HERO_W, HERO_H, 6, 4);
    rk_dot_set_color(&s_dot, th->ink);
    snprintf(s_last_hero, sizeof(s_last_hero), "%s", "....");
    rk_dot_set_text(&s_dot, s_last_hero);

    /* 台名 */
    s_name = lv_label_create(s_scr);
    lv_label_set_text(s_name, "");
    lv_obj_set_style_text_font(s_name, RK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(s_name, th->ink, 0);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_name, 30, NAME_Y);
    lv_obj_set_size(s_name, 300, 28);      /* 高度锁一行：LONG_DOT 才能在超宽处截断 */
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(s_name, LV_OBJ_FLAG_CLICKABLE);

    /* 元信息 */
    s_meta = lv_label_create(s_scr);
    lv_label_set_text(s_meta, "");
    lv_obj_set_style_text_font(s_meta, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_meta, th->muted, 0);
    lv_obj_set_style_text_align(s_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_meta, 30, META_Y);
    lv_obj_set_size(s_meta, 300, 20);
    lv_label_set_long_mode(s_meta, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(s_meta, LV_OBJ_FLAG_CLICKABLE);

    /* 电平条 */
    lv_coord_t total = BARS_N * (BARS_W + BARS_GAP) - BARS_GAP;
    lv_coord_t x0 = (LCD_WIDTH - total) / 2;
    for (int i = 0; i < BARS_N; i++) {
        s_bars[i] = rk_box(s_scr, x0 + i * (BARS_W + BARS_GAP), BARS_BASE - 2, BARS_W, 2);
        lv_obj_set_style_bg_color(s_bars[i], th->ink, 0);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_50, 0);
        lv_obj_set_style_radius(s_bars[i], 1, 0);
        lv_obj_add_flag(s_bars[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* dock：3 个动作同一行（上一台 / 播放 / 下一台）。
     * 列表改成底部上滑后，这里从 4 键收回 3 键 —— 三键等距 68，重心正落在
     * 圆心 x=180，比原来四键一直偏左的排布稳。播放键反色仍是唯一高对比按钮。 */
    lv_obj_t *prev = rk_round_btn(s_scr, DOCK_X0, DOCK_Y, DOCK_S, RK_ICON_PREV,
                                  th->card, th->ink);
    sdgoods_tap_bind(prev, &s_ctx_prev, (sdgoods_tap_cb_t)on_prev, NULL);

    s_play_btn = rk_round_btn(s_scr, DOCK_X1, DOCK_Y, DOCK_S,
                              RK_ICON_PLAY, th->ink, th->on_accent);
    sdgoods_tap_bind(s_play_btn, &s_ctx_play, (sdgoods_tap_cb_t)on_play, NULL);

    lv_obj_t *next = rk_round_btn(s_scr, DOCK_X2, DOCK_Y, DOCK_S, RK_ICON_NEXT,
                                  th->card, th->ink);
    sdgoods_tap_bind(next, &s_ctx_next, (sdgoods_tap_cb_t)on_next, NULL);

    /* 应用外壳：顶部下滑控制中心 + 设备级手势策略 */
    sdgoods_app_shell_bind(s_scr);

    lv_scr_load(s_scr);
    refresh_ui(false);
}

static void rebuild_ui(void)
{
    radio_pages_detach();
    lv_obj_t *old = s_scr;
    s_scr = NULL;
    s_drag = NULL;
    s_name = NULL;
    s_meta = NULL;
    s_clock = NULL;
    s_live_dot = NULL;
    s_live_lbl = NULL;
    s_bat_shell = NULL;
    s_bat_fill = NULL;
    s_bat_lbl = NULL;
    s_swipe = NULL;
    s_play_btn = NULL;
    memset(&s_ring, 0, sizeof(s_ring));
    memset(&s_dot, 0, sizeof(s_dot));
    for (int i = 0; i < BARS_N; i++) {
        s_bars[i] = NULL;
    }
    s_last_hero[0] = '\0';
    s_last_name[0] = '\0';
    s_last_meta[0] = '\0';
    s_last_clock[0] = '\0';
    s_last_live[0] = '\0';
    s_last_bat[0] = '\0';
    s_bat_pct = -2;             /* 新对象上的填充条是满宽，必须重算一次 */

    build_screen();
    if (old) {
        lv_obj_del(old);
    }
}

/* ==========================================================================
 * 页面回调
 * ======================================================================== */
static void ctl_play_index(int idx)
{
    play_index(idx);
}

static void ctl_set_city(const char *city)
{
    snprintf(s_city, sizeof(s_city), "%s", city ? city : "");
    radio_settings_set_city(s_city);
    s_catalog_ready = false;
    s_catalog_pending = true;
    s_cur_idx = -1;
    s_skip_streak = 0;
    radio_player_stop();
    rebuild_ui();
    if (wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        start_catalog();
    }
}

static void ctl_set_night(bool night)
{
    radio_settings_set_night(night);
    rk_theme_set(night);
    rebuild_ui();
}

static void ctl_set_sleep(int minutes)
{
    s_sleep_min = minutes;
    radio_settings_set_sleep_min(minutes);
    arm_sleep();
    ESP_LOGI(TAG, "sleep timer: %d min", minutes);
}

static void ctl_refresh(void)
{
    s_catalog_pending = true;
    if (wifi_prov_get_state() != WIFI_STATE_CONNECTED) {
        s_catalog_pending = false;
        radio_pages_open_wifi_page(s_scr);
        return;
    }
    start_catalog();
}

static int ctl_cur_index(void)
{
    return s_cur_idx;
}

static int ctl_count(void)
{
    return radio_catalog_count();
}

static const char *ctl_city(void)
{
    return s_city[0] ? s_city : "自动定位";
}

static int ctl_sleep_min(void)
{
    return s_sleep_min;
}

static const radio_pages_ctl_t s_ctl = {
    .play_index = ctl_play_index,
    .set_city   = ctl_set_city,
    .set_night  = ctl_set_night,
    .set_sleep  = ctl_set_sleep,
    .refresh    = ctl_refresh,
    .cur_index  = ctl_cur_index,
    .count      = ctl_count,
    .city       = ctl_city,
    .sleep_min  = ctl_sleep_min,
};

/* ==========================================================================
 * tick
 * ======================================================================== */
static void radio_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_scr) {
        return;
    }
    if (!s_tick_seen) {
        s_tick_seen = true;
        ESP_LOGI(TAG, "tick alive (wifi=%d, pending=%d, int free=%u)",
                 (int)wifi_prov_get_state(), (int)s_catalog_pending,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    /* 开机未配网：第一个 tick 弹配网页（此时 LVGL 循环已在跑，安全） */
    if (s_auto_wifi) {
        s_auto_wifi = false;
        if (!radio_pages_any_open()) {
            radio_pages_open_wifi_page(s_scr);
        }
    }

    radio_pages_tick();

    uint32_t now = lv_tick_get();
    if (now - s_clock_ms >= 1000) {
        s_clock_ms = now;
        update_clock();
        if (wifi_prov_get_state() == WIFI_STATE_CONNECTED && !s_ntp_started) {
            clock_maybe_start();
        }
        if (wifi_prov_get_state() == WIFI_STATE_CONNECTED && !s_wifi_seen) {
            s_wifi_seen = true;
            refresh_ui(false);
        }
    }

    /* 电量 5s 一次：ADC 采样比时钟贵得多，且电量变化本来没这么快 */
    if (now - s_bat_ms >= 5000) {
        s_bat_ms = now;
        update_battery();
    }

    /* 定时休眠到点 */
    if (s_sleep_armed && (int32_t)(now - s_sleep_deadline) >= 0) {
        s_sleep_armed = false;
        s_paused = false;
        radio_player_stop();
        refresh_ui(false);
    }

    /* 目录加载完成：续播上次台（索引越界回 0） */
    if (s_catalog_done) {
        s_catalog_done = false;
        s_catalog_ready = true;
        int n = radio_catalog_count();
        if (n > 0) {
            int last = radio_settings_get_last_index();
            if (s_cur_idx < 0) {
                s_cur_idx = (last >= 0 && last < n) ? last : 0;
            }
            if (s_cur_idx >= n) {
                s_cur_idx = 0;
            }
            s_skip_streak = 0;
            play_index(s_cur_idx);
        } else {
            refresh_ui(false);
        }
    }

    /* 未配网/刚联网时挂起的目录请求 */
    if (s_catalog_pending && !s_catalog_running &&
        wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        s_catalog_pending = false;
        start_catalog();
    }

    radio_player_state_t st = radio_player_state();

    if (st != s_last_state) {
        if (st == RP_PLAYING) {
            s_skip_streak = 0;
            if (s_sleep_min > 0) {
                arm_sleep();
            }
        }
        s_last_state = st;
        refresh_ui(false);
    }

    /* 失败自动换台（限次，避免无限风暴） */
    if (st == RP_ERROR && radio_catalog_count() > 0 && s_skip_streak < SKIP_MAX) {
        s_skip_streak++;
        ESP_LOGW(TAG, "play error, skip #%d (streak %d)", s_cur_idx, s_skip_streak);
        step_play(+1);
    }

    update_bars(st);
}

/* ==========================================================================
 * 生命周期
 * ======================================================================== */
static void close_to_app(void)
{
    if (!s_scr) {
        return;
    }
    /* 页面是当前屏的子对象：先拆页面，再删屏，避免遗留指针 */
    radio_pages_detach();
    radio_player_stop();
    if (s_tick) {
        lv_timer_del(s_tick);
        s_tick = NULL;
    }
    lv_obj_t *gone = s_scr;
    s_scr = NULL;
    s_drag = NULL;
    s_name = NULL;
    s_meta = NULL;
    s_clock = NULL;
    s_live_dot = NULL;
    s_live_lbl = NULL;
    s_bat_shell = NULL;
    s_bat_fill = NULL;
    s_bat_lbl = NULL;
    s_swipe = NULL;
    s_play_btn = NULL;
    memset(&s_ring, 0, sizeof(s_ring));
    memset(&s_dot, 0, sizeof(s_dot));
    for (int i = 0; i < BARS_N; i++) {
        s_bars[i] = NULL;
    }
    lv_obj_del(gone);
}

void ui_radio_start(void)
{
    if (s_scr) {
        return;
    }

    radio_settings_init();
    s_sleep_min = radio_settings_get_sleep_min();
    snprintf(s_city, sizeof(s_city), "%s", radio_settings_get_city());
    rk_theme_set(radio_settings_get_night());
    radio_player_init();

    radio_pages_init(&s_ctl);

    build_screen();

    sdgoods_app_shell_set_exit_cb(close_to_app);
    sdgoods_app_shell_set_pause_cb(NULL);
    sdgoods_app_shell_set_resume_cb(NULL);

    if (!s_tick) {
        s_tick = lv_timer_create(radio_tick, 120, NULL);
    }

    s_auto_wifi = wifi_prov_is_setup_mode();
    if (wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        start_catalog();
    } else {
        s_catalog_pending = true;
    }
    ESP_LOGI(TAG, "started (city=%s, night=%d, sleep=%d)",
             s_city[0] ? s_city : "auto", (int)rk_theme_is_night(), s_sleep_min);
}

void ui_radio_poll(void)
{
    /* 全部逻辑在 LVGL 定时器里（radio_tick）；电源键由平台统一处理。 */
}

bool ui_radio_is_open(void)
{
    return s_scr != NULL;
}

void ui_radio_close(void)
{
    if (s_scr) {
        sdgoods_app_shell_leave(true);
    }
}

/* ==========================================================================
 * 串口诊断键（覆盖平台弱符号 sdgoods_console_ext_cmd）
 *
 * 为什么需要：本机没有键鼠，而截屏一次只能拍「当前那一屏」——列表 / 设置 /
 * WiFi / 夜色主题这些二级状态根本没法远程验收。平台已经提供了合成触摸
 * sdgoods_tap_synth()，这里只需要把字母映射成坐标，不新增任何检测逻辑。
 *
 * 为什么全部走 lv_async_call：控制台在自己的任务里跑，直接建/删 LVGL 对象
 * 会与 LVGL 主循环撞车；异步调用把动作排回 UI 上下文，与真实点按同路径。
 *
 *   'l' 打开电台列表   'k' 打开设置   'w' 设置页首行 WiFi   'n' 切纸张/夜色
 *   'x' 关掉所有页面回主屏                         '1'..'9' 播第 N 台
 *   'v' 直接打开音量滑块页（平台 which=3）——没有手指，
 *       「拖音量卡顿 / 拖完静音」只能靠它离线复现
 *   'g' 在音量页上合成一次横拖（先发 'v' 再发 'g'）
 *   'm' 音量页的静音开关：打开页 + 点一下（再发一次就是点第二下）
 * ========================================================================== */
static char s_qa_cmd;

/* 'm' 的第二段：等页面布局完成后再合成点静音按钮（坐标见 sdgoods_cc.c 的 open_slider） */
static void mute_tap_later(lv_timer_t *t)
{
    lv_timer_del(t);
    sdgoods_tap_synth_now(180, 252, 0, 0, 1);
}

static void qa_apply(void *arg)
{
    (void)arg;
    char c = s_qa_cmd;
    s_qa_cmd = 0;
    if (!s_scr || c == 0) {
        return;
    }
    switch (c) {
    case 'l':
        /* 列表改成底部上滑：从屏幕最下沿往上推 64px（每帧 8px × 8 帧） */
        sdgoods_tap_synth(180, LCD_HEIGHT - 20, 0, -8, 8);
        break;
    case 'k':
        sdgoods_tap_synth(100, 66, 0, 0, 1);                         /* 页头左侧时间（暂兼设置入口） */
        break;
    case 'p':
        sdgoods_tap_synth(DOCK_X1 + DOCK_S / 2, DOCK_Y + DOCK_S / 2, 0, 0, 1);  /* 点 dock 播放键 */
        break;
    case 'n':
        ctl_set_night(!rk_theme_is_night());
        break;
    case 'w':
        sdgoods_tap_synth(180, 124, 0, 0, 1);                       /* 设置页首行「WiFi 配网」 */
        break;
    case 'x':
        radio_pages_close_all();                                    /* 收尾回主屏，便于连续截屏 */
        break;
    case 'v':
        sdgoods_cc_debug_open(3);                                   /* 音量滑块页 */
        break;
    case 'g':
        /* 在音量页上合成一次「按下 → 横拖 → 松开」：用于复现/验收拖动卡顿。
         * 滑块 240×24 居中 ⇒ y=180、x=60..300；从 120 拖到 228。 */
        sdgoods_tap_synth(120, 180, 12, 0, 10);
        break;
    case 'm':
        /* 静音开关验收：开页 + 点按钮。
         * ⚠️ 不能在同一 tick 里紧接着 tap：页面刚创建，子对象 coords 还没算出来
         *    （LVGL 的 align 要等一次布局刷新），此时命中测试抓不到按钮。
         *    tap_synth 又必须回到 LVGL 线程 —— 用 150ms 的一次性 lv_timer 中转。
         * 连发两次 'm' 应看到 `tap: Mute 5% -> 0%` 后 `tap: Mute 0% -> 5%`。 */
        sdgoods_cc_debug_open(3);
        if (!lv_timer_create(mute_tap_later, 150, NULL)) {
            ESP_LOGW("radio-qa", "mute tap timer alloc failed");
        }
        break;
    case 'j':
        step_play(1);                                               /* 下一台：连切验收缓冲与失败率
                                                                     * （'s' 被平台串口截屏占了，别用） */
        break;
    case 'J':
        step_play(-1);                                              /* 上一台 */
        break;
    case 'y':
        /* 验收 30x 跟随：antenne.de 的入口 302 到 s4-webradio（200 audio/mpeg），
         * 旧代码会把 302 当失败，现在应看到 `http 302 -> ...` 后正常出声。 */
        radio_player_play("http://stream.antenne.de/antenne", "MP3");
        break;
    case 'V':
        /* 把音量恢复成 70% 并落盘（串口测试把滑块拖到 0 后救回来用）。
         * 必须先 flush：NVS 是跨 app 共用的，不落盘下次启动又会读回 0。 */
        sdgoods_audio_set_volume(70);
        sdgoods_cc_flush();
        break;
    default:
        if (c >= '1' && c <= '9') {
            play_index(c - '1');
        }
        break;
    }
}

void sdgoods_console_ext_cmd(char c)
{
    s_qa_cmd = c;
    lv_async_call(qa_apply, NULL);
}
