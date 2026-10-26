/*
 * sdgoods-radio · 城市电台主屏（应用层）
 *
 * 设计见 docs/UI_REDESIGN.md。这里只做三件事：
 *   ① 搭主屏（表盘环 / 点阵英雄 / 台名元信息 / dock）；
 *   ② 把触摸翻译成意图（点按 = 播放暂停，横向拖拽 = 调台，长按 = 设置）；
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
#include "sdgoods_lcd.h"
#include "ui_radio_kit.h"
#include "ui_radio_pages.h"
#include "wifi_prov.h"

static const char *TAG = "UI_RADIO";

/* ---- 布局（360×360 圆屏，圆心 180,180；顶部 50px 留给控制中心手势）---- */
#define HERO_X       30
#define HERO_Y       98
#define HERO_W       300
#define HERO_H       66
#define NAME_Y       166
#define META_Y       196
#define BARS_BASE    234
#define BARS_N       RADIO_PLAYER_LEVELS
#define BARS_W       5
#define BARS_GAP     5
#define RING_SIZE    360
#define RING_R       168
#define CHIP_Y       240
#define CHIP_H       24
#define DOCK_Y       276
#define DOCK_PLAY_X  152
#define DOCK_PLAY_Y  268
#define DOCK_PLAY_S  56

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
static lv_obj_t   *s_wifi_dot;
static lv_obj_t   *s_chip;
static lv_obj_t   *s_play_btn;
static lv_obj_t   *s_bars[BARS_N];

static char s_last_hero[12];
static char s_last_name[RADIO_NAME_MAX + 4];
static char s_last_meta[80];
static char s_last_clock[8];

static int  s_cur_idx = -1;
static int  s_preview_idx = -1;
static bool s_paused;
static bool s_dragging;
static bool s_long_fired;
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

/* 点按上下文（静态存储，生命周期覆盖对应对象） */
static sdgoods_tap_ctx_t s_ctx_prev, s_ctx_next, s_ctx_play, s_ctx_chip, s_ctx_brand;

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

static const char *city_label(void)
{
    return s_city[0] ? s_city : "城市电台";
}

static const char *state_word(radio_player_state_t st, bool paused)
{
    if (paused || st == RP_PAUSED) return "已暂停";
    switch (st) {
    case RP_CONNECTING: return "连接中";
    case RP_BUFFERING:  return "缓冲中";
    case RP_ERROR:      return "信号中断";
    case RP_PLAYING:    return NULL;
    default:            return NULL;
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
    lv_obj_set_user_data(ic, (void *)(uintptr_t)(playing ? RK_ICON_PAUSE : RK_ICON_PLAY));
    lv_obj_invalidate(ic);
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
 * 触摸：点按 = 播放暂停；横向拖拽 = 调台；长按 = 设置
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
    s_long_fired = false;
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

    if (s_long_fired) {
        s_long_fired = false;
        s_dragging = false;
        return;
    }
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

static void drag_long_pressed(lv_event_t *e)
{
    (void)e;
    if (radio_pages_any_open()) {
        return;
    }
    s_long_fired = true;
    open_settings();
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

static void on_open_list(void *ud)
{
    (void)ud;
    radio_pages_open_list(s_scr);
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

    /* 元信息：状态优先，其次才是「城市 · 码率 · 序号」 */
    char meta[80];
    if (!s_catalog_ready && n == 0) {
        snprintf(meta, sizeof(meta), "正在获取城市电台…");
    } else if (n == 0) {
        snprintf(meta, sizeof(meta), "请先连接 WiFi");
    } else {
        const char *w = state_word(state, s_paused);
        if (w) {
            snprintf(meta, sizeof(meta), "%s · %d/%d", w, idx + 1, n);
        } else if (st && st->bitrate > 0) {
            snprintf(meta, sizeof(meta), "%s · %uk · %d/%d", city_label(),
                     (unsigned)st->bitrate, idx + 1, n);
        } else {
            snprintf(meta, sizeof(meta), "%s · %d/%d", city_label(), idx + 1, n);
        }
    }
    set_text(s_meta, s_last_meta, sizeof(s_last_meta), meta);

    /* dock 播放图标 */
    set_play_icon(state == RP_PLAYING || state == RP_CONNECTING || state == RP_BUFFERING);

    /* 胶囊：台数 */
    if (s_chip) {
        char buf[32];
        if (n > 0) {
            snprintf(buf, sizeof(buf), "电台列表 · %d", n);
        } else {
            snprintf(buf, sizeof(buf), "电台列表");
        }
        rk_pill_set_text(s_chip, buf);
    }

    /* WiFi 点 */
    if (s_wifi_dot) {
        bool on = (wifi_prov_get_state() == WIFI_STATE_CONNECTED);
        lv_obj_set_style_bg_color(s_wifi_dot, on ? rk_theme()->accent : rk_theme()->muted, 0);
    }
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
        lv_coord_t h = 2 + 16 * v / 255;
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
    lv_obj_add_event_cb(s_drag, drag_long_pressed, LV_EVENT_LONG_PRESSED, NULL);

    /* 页头：品牌（点按进设置）/ WiFi 点 / 时钟 */
    lv_obj_t *brand = rk_box(s_scr, 52, 56, 108, 28);
    lv_obj_add_flag(brand, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *brand_lbl = lv_label_create(brand);
    lv_label_set_text(brand_lbl, "城市电台");
    lv_obj_set_style_text_font(brand_lbl, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(brand_lbl, th->ink, 0);
    lv_obj_set_style_text_letter_space(brand_lbl, 1, 0);
    lv_obj_set_pos(brand_lbl, 6, 6);
    lv_obj_clear_flag(brand_lbl, LV_OBJ_FLAG_CLICKABLE);
    sdgoods_tap_bind(brand, &s_ctx_brand, (sdgoods_tap_cb_t)on_brand, NULL);

    s_wifi_dot = rk_box(s_scr, 302, 69, 8, 8);
    lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_dot, th->muted, 0);
    lv_obj_set_style_bg_opa(s_wifi_dot, LV_OPA_COVER, 0);

    s_clock = lv_label_create(s_scr);
    lv_label_set_text(s_clock, "--:--");
    lv_obj_set_style_text_font(s_clock, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_clock, th->muted, 0);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_clock, 244, 64);
    lv_obj_set_width(s_clock, 50);
    lv_obj_clear_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);

    rk_hairline(s_scr, 58, 90, 244, th->hair);

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

    /* 胶囊：电台列表 */
    s_chip = rk_pill(s_scr, 110, CHIP_Y, 140, CHIP_H, "电台列表", th->card, th->ink);
    sdgoods_tap_bind(s_chip, &s_ctx_chip, (sdgoods_tap_cb_t)on_open_list, NULL);

    /* dock */
    lv_obj_t *prev = rk_round_btn(s_scr, 96, DOCK_Y, 40, RK_ICON_PREV, th->card, th->ink);
    sdgoods_tap_bind(prev, &s_ctx_prev, (sdgoods_tap_cb_t)on_prev, NULL);
    s_play_btn = rk_round_btn(s_scr, DOCK_PLAY_X, DOCK_PLAY_Y, DOCK_PLAY_S,
                              RK_ICON_PLAY, th->ink, th->on_accent);
    sdgoods_tap_bind(s_play_btn, &s_ctx_play, (sdgoods_tap_cb_t)on_play, NULL);
    lv_obj_t *next = rk_round_btn(s_scr, 224, DOCK_Y, 40, RK_ICON_NEXT, th->card, th->ink);
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
    s_wifi_dot = NULL;
    s_chip = NULL;
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
    s_wifi_dot = NULL;
    s_chip = NULL;
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
 * ========================================================================== */
static char s_qa_cmd;

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
        sdgoods_tap_synth(110 + 70, CHIP_Y + CHIP_H / 2, 0, 0, 1);   /* 「电台列表」胶囊 */
        break;
    case 'k':
        sdgoods_tap_synth(106, 70, 0, 0, 1);                        /* 左上品牌字 */
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
