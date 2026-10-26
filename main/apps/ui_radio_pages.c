/*
 * sdgoods-radio · 页面层实现
 *
 * 一条硬规则：**任何会让当前页面被销毁的动作都走 lv_async_call**。
 * 直接在点按回调里删掉自己所在的整棵树，LVGL 的事件派发还在用这些对象，
 * 属于「能跑但迟早出鬼」的写法（旧版设置浮层只是隐藏，所以没暴露这个问题）。
 */

#include "ui_radio_pages.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "radio_catalog.h"
#include "ui_radio_kit.h"
#include "wifi_prov.h"

static const char *TAG = "UI_RADIO_PG";

#define LIST_SHOW_MAX   40
#define WIFI_MAX_NETS   12

static const radio_pages_ctl_t *s_ctl;

static rk_page_t s_list;
static rk_page_t s_set;
static rk_page_t s_pick;
static rk_page_t s_wifi;
static rk_page_t s_wifi_pw;

/* 位移守卫上下文：由调用方静态存储，生命周期覆盖对应对象 */
static sdgoods_tap_ctx_t s_list_ctx[LIST_SHOW_MAX];
static sdgoods_tap_ctx_t s_set_ctx[8];
static sdgoods_tap_ctx_t s_pick_ctx[20];
static sdgoods_tap_ctx_t s_wifi_ctx[WIFI_MAX_NETS];
static sdgoods_tap_ctx_t s_head_ctx[4];
static sdgoods_tap_ctx_t s_pw_ctx[2];

static lv_obj_t *s_pw_ta;
static char      s_pw_ssid[33];

/* WiFi 扫描交接（任务 → tick） */
static char          s_nets[WIFI_MAX_NETS][33];
static int8_t        s_rssi[WIFI_MAX_NETS];
static volatile int  s_scan_cnt = -1;
static volatile bool s_scan_running;
static bool          s_scan_listed;
static bool          s_wait_connect;

/* 选择器 */
static void (*s_pick_cb)(int idx);

static void radio_pages_open_wifi(lv_obj_t *parent);
static void radio_pages_open_wifi_pw(lv_obj_t *parent);

void radio_pages_init(const radio_pages_ctl_t *ctl)
{
    s_ctl = ctl;
}

void radio_pages_detach(void)
{
    rk_page_close(&s_pick);
    rk_page_close(&s_wifi_pw);
    rk_page_close(&s_wifi);
    rk_page_close(&s_set);
    rk_page_close(&s_list);
    s_pick_cb = NULL;
    s_wait_connect = false;
}

void radio_pages_close_all(void)
{
    radio_pages_detach();
}

bool radio_pages_any_open(void)
{
    return rk_page_is_open(&s_list) || rk_page_is_open(&s_set) ||
           rk_page_is_open(&s_pick) || rk_page_is_open(&s_wifi) ||
           rk_page_is_open(&s_wifi_pw);
}

/* ------------------------------------------------------------------ 异步 */
static void list_apply(void *arg)
{
    int idx = (int)(intptr_t)arg;
    rk_page_close(&s_list);
    if (s_ctl && s_ctl->play_index) {
        s_ctl->play_index(idx);
    }
}

static void list_row_cb(void *ud)
{
    lv_async_call(list_apply, ud);
}

/* ------------------------------------------------------------------ 列表 */
static void on_list_settings(void *ud)
{
    (void)ud;
    radio_pages_close_all();
    if (s_ctl) {
        lv_obj_t *parent = lv_scr_act();
        radio_pages_open_settings(parent);
    }
}

void radio_pages_open_list(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    radio_pages_detach();
    rk_page_open(&s_list, parent, "电台列表");
    rk_page_action(&s_list, "设置", &s_head_ctx[0], on_list_settings, NULL);

    int n = s_ctl && s_ctl->count ? s_ctl->count() : 0;
    if (n <= 0) {
        rk_page_hint(&s_list, "还没有电台\n连上 WiFi 后会自动获取");
        return;
    }
    if (n > LIST_SHOW_MAX) {
        n = LIST_SHOW_MAX;
    }

    static char names[LIST_SHOW_MAX][RADIO_NAME_MAX + 4];
    static char metas[LIST_SHOW_MAX][16];
    for (int i = 0; i < n; i++) {
        const radio_station_t *st = radio_catalog_at(i);
        if (!st) {
            break;
        }
        rk_text_safe(st->name, names[i], sizeof(names[i]), RK_FONT_TITLE);
        if (names[i][0] == '\0') {
            snprintf(names[i], sizeof(names[i]), "电台 %d", i + 1);
        }
        if (st->bitrate > 0) {
            snprintf(metas[i], sizeof(metas[i]), "%uk", (unsigned)st->bitrate);
        } else {
            metas[i][0] = '\0';
        }
        rk_page_row_net(&s_list, names[i], metas[i], &s_list_ctx[i], list_row_cb,
                        (void *)(intptr_t)i);
    }
}

/* ------------------------------------------------------------------ 选择器 */
static void pick_apply(void *arg)
{
    int idx = (int)(intptr_t)arg;
    void (*cb)(int) = s_pick_cb;
    s_pick_cb = NULL;
    rk_page_close(&s_pick);
    if (cb) {
        cb(idx);
    }
}

static void pick_row_cb(void *ud)
{
    lv_async_call(pick_apply, ud);
}

static void picker_open(lv_obj_t *parent, const char *title, const char *const *labels,
                        int n, int selected, void (*cb)(int))
{
    if (!parent || n <= 0 || n > 20) {
        return;
    }
    rk_page_open(&s_pick, parent, title);
    s_pick_cb = cb;
    for (int i = 0; i < n; i++) {
        static char marks[20][8];
        if (i == selected) {
            snprintf(marks[i], sizeof(marks[i]), "当前");
        } else {
            marks[i][0] = '\0';
        }
        rk_page_row(&s_pick, labels[i], marks[i], &s_pick_ctx[i], pick_row_cb,
                    (void *)(intptr_t)i);
    }
}

/* 城市 */
static const char *const CITY_LABELS[] = {
    "自动定位", "北京", "上海", "广州", "深圳", "杭州", "成都", "武汉", "南京",
    "西安", "重庆", "天津", "香港", "台北", "长沙", "苏州",
};
#define CITY_N ((int)(sizeof(CITY_LABELS) / sizeof(CITY_LABELS[0])))

static int city_index(void)
{
    const char *cur = (s_ctl && s_ctl->city) ? s_ctl->city() : "";
    for (int i = 1; i < CITY_N; i++) {
        if (strcmp(cur, CITY_LABELS[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static void city_picked(int idx)
{
    if (!s_ctl || !s_ctl->set_city || idx < 0 || idx >= CITY_N) {
        return;
    }
    s_ctl->set_city(idx == 0 ? "" : CITY_LABELS[idx]);
}

static void open_city_picker(lv_obj_t *parent)
{
    picker_open(parent, "选择城市", CITY_LABELS, CITY_N, city_index(), city_picked);
}

/* 定时 */
static const char *const SLEEP_LABELS[] = {
    "关闭", "15 分钟", "30 分钟", "45 分钟", "60 分钟", "90 分钟",
};
static const int SLEEP_STEPS[] = {0, 15, 30, 45, 60, 90};
#define SLEEP_N ((int)(sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0])))

static int sleep_index(void)
{
    int cur = (s_ctl && s_ctl->sleep_min) ? s_ctl->sleep_min() : 0;
    for (int i = 0; i < SLEEP_N; i++) {
        if (SLEEP_STEPS[i] == cur) {
            return i;
        }
    }
    return 0;
}

static void sleep_picked(int idx)
{
    if (!s_ctl || !s_ctl->set_sleep || idx < 0 || idx >= SLEEP_N) {
        return;
    }
    s_ctl->set_sleep(SLEEP_STEPS[idx]);
}

static void open_sleep_picker(lv_obj_t *parent)
{
    picker_open(parent, "定时休眠", SLEEP_LABELS, SLEEP_N, sleep_index(), sleep_picked);
}

/* ------------------------------------------------------------------ 设置 */
static void set_async_open_city(void *a)
{
    (void)a;
    open_city_picker(lv_scr_act());
}

static void set_async_open_sleep(void *a)
{
    (void)a;
    open_sleep_picker(lv_scr_act());
}

static void set_async_theme(void *a)
{
    (void)a;
    bool night = !rk_theme_is_night();
    radio_pages_detach();
    if (s_ctl && s_ctl->set_night) {
        s_ctl->set_night(night);
    }
}

static void set_async_refresh(void *a)
{
    (void)a;
    radio_pages_detach();
    if (s_ctl && s_ctl->refresh) {
        s_ctl->refresh();
    }
}

static void set_async_wifi(void *a)
{
    (void)a;
    radio_pages_detach();
    radio_pages_open_wifi(lv_scr_act());
}

static void set_city_cb(void *ud)    { (void)ud; lv_async_call(set_async_open_city, NULL); }
static void set_sleep_cb(void *ud)   { (void)ud; lv_async_call(set_async_open_sleep, NULL); }
static void set_theme_cb(void *ud)   { (void)ud; lv_async_call(set_async_theme, NULL); }
static void set_refresh_cb(void *ud) { (void)ud; lv_async_call(set_async_refresh, NULL); }
static void set_wifi_cb(void *ud)    { (void)ud; lv_async_call(set_async_wifi, NULL); }

void radio_pages_open_settings(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    radio_pages_detach();
    rk_page_open(&s_set, parent, "设置");

    char sleep_buf[24];
    int sm = (s_ctl && s_ctl->sleep_min) ? s_ctl->sleep_min() : 0;
    if (sm <= 0) {
        snprintf(sleep_buf, sizeof(sleep_buf), "关闭");
    } else {
        snprintf(sleep_buf, sizeof(sleep_buf), "%d 分钟", sm);
    }

    const char *wifi = "未连接";
    if (wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        wifi = "已连接";
    } else if (wifi_prov_is_setup_mode()) {
        wifi = "待配网";
    }

    /* 顺序按「多久用一次」排：配网 / 换城市是配置期低频但**必须在首屏**（不然要滚动），
     * 主题 / 休眠 / 刷新次之。用 40px 紧凑行：5 行 × 40 = 200px 正好落在圆屏可视区内，
     * 不需要滚动 —— 圆屏上没有「这页能滚」的提示，藏到折行外等于用户找不到。
     * 也没有「返回」行：页头 ✕ 就能关。 */
    rk_page_row_compact(&s_set, "WiFi 配网", wifi, &s_set_ctx[4], set_wifi_cb, NULL);
    rk_page_row_compact(&s_set, "城市", s_ctl && s_ctl->city ? s_ctl->city() : "自动定位",
                        &s_set_ctx[0], set_city_cb, NULL);
    rk_page_row_compact(&s_set, "主题", rk_theme_is_night() ? "夜色" : "纸张",
                        &s_set_ctx[1], set_theme_cb, NULL);
    rk_page_row_compact(&s_set, "定时休眠", sleep_buf, &s_set_ctx[2], set_sleep_cb, NULL);
    rk_page_row_compact(&s_set, "刷新电台", NULL, &s_set_ctx[3], set_refresh_cb, NULL);
}

/* ------------------------------------------------------------------ WiFi */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    int c = wifi_prov_scan(s_nets, s_rssi, WIFI_MAX_NETS);
    s_scan_cnt = c;
    s_scan_running = false;
    ESP_LOGI(TAG, "wifi scan done: %d", c);
    rk_task_exit();
}

static void wifi_start_scan(void)
{
    if (s_scan_running) {
        return;
    }
    s_scan_running = true;
    s_scan_listed = false;
    s_scan_cnt = -1;
    if (!rk_task_spawn(wifi_scan_task, "wifi_scan", 8192, NULL, 4)) {
        s_scan_running = false;
        ESP_LOGW(TAG, "wifi scan task create failed");
    }
}

static void wifi_net_cb(void *ud)
{
    int i = (int)(intptr_t)ud;
    if (i < 0 || i >= WIFI_MAX_NETS) {
        return;
    }
    snprintf(s_pw_ssid, sizeof(s_pw_ssid), "%s", s_nets[i]);
    /* 密码页叠在 WiFi 页上：不销毁父页，只把父页藏起来，避免重建列表 */
    if (rk_page_is_open(&s_wifi)) {
        lv_obj_add_flag(s_wifi.root, LV_OBJ_FLAG_HIDDEN);
    }
    radio_pages_open_wifi_pw(lv_scr_act());
}

static void wifi_fill_list(void)
{
    if (!rk_page_is_open(&s_wifi)) {
        return;
    }
    lv_obj_t *body = s_wifi.body;
    lv_obj_clean(body);
    if (s_scan_cnt <= 0) {
        rk_page_hint(&s_wifi, "没有扫描到网络\n换个位置再试，或复用手机热点");
        return;
    }
    for (int i = 0; i < s_scan_cnt && i < WIFI_MAX_NETS; i++) {
        rk_page_row(&s_wifi, s_nets[i], s_rssi[i] < -75 ? "弱" : NULL,
                    &s_wifi_ctx[i], wifi_net_cb, (void *)(intptr_t)i);
    }
}

static void wifi_rescan_cb(void *ud)
{
    (void)ud;
    if (!rk_page_is_open(&s_wifi)) {
        return;
    }
    lv_obj_clean(s_wifi.body);
    rk_page_hint(&s_wifi, "扫描中…");
    wifi_start_scan();
}

static void wifi_connect_cb(void *ud)
{
    (void)ud;
    const char *pass = s_pw_ta ? lv_textarea_get_text(s_pw_ta) : "";
    wifi_prov_submit(s_pw_ssid, pass);
    s_wait_connect = true;
    radio_pages_detach();
}

static void wifi_pw_back_cb(void *ud)
{
    (void)ud;
    rk_page_close(&s_wifi_pw);
    if (rk_page_is_open(&s_wifi)) {
        lv_obj_clear_flag(s_wifi.root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void radio_pages_open_wifi_pw(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    rk_page_close(&s_wifi_pw);
    rk_page_open(&s_wifi_pw, parent, "输入密码");

    const rk_theme_t *th = rk_theme();
    lv_obj_t *ssid = lv_label_create(s_wifi_pw.root);
    lv_label_set_text(ssid, s_pw_ssid);
    lv_obj_set_style_text_font(ssid, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(ssid, th->muted, 0);
    lv_obj_set_pos(ssid, 58, 96);
    lv_obj_set_width(ssid, 244);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(ssid, LV_OBJ_FLAG_CLICKABLE);

    s_pw_ta = lv_textarea_create(s_wifi_pw.root);
    lv_obj_set_size(s_pw_ta, 244, 42);
    lv_obj_set_pos(s_pw_ta, 58, 118);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_max_length(s_pw_ta, 64);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "WiFi 密码");
    lv_obj_set_style_text_font(s_pw_ta, RK_FONT_BODY, 0);
    lv_obj_set_style_bg_color(s_pw_ta, th->card, 0);
    lv_obj_set_style_bg_opa(s_pw_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_pw_ta, th->ink, 0);
    lv_obj_set_style_border_width(s_pw_ta, 1, 0);
    lv_obj_set_style_border_color(s_pw_ta, th->hair, 0);
    lv_obj_set_style_radius(s_pw_ta, 10, 0);

    lv_obj_t *ok = rk_pill(s_wifi_pw.root, 58, 172, 112, 36, "连接", th->ink, th->bg);
    sdgoods_tap_bind(ok, &s_pw_ctx[0], wifi_connect_cb, NULL);
    lv_obj_t *back = rk_pill(s_wifi_pw.root, 190, 172, 112, 36, "返回", th->card, th->ink);
    sdgoods_tap_bind(back, &s_pw_ctx[1], wifi_pw_back_cb, NULL);

    lv_obj_t *kb = lv_keyboard_create(s_wifi_pw.root);
    lv_obj_set_size(kb, 360, 150);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pw_ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_bg_color(kb, th->card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(kb, th->bg, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, th->ink, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_move_foreground(kb);
}

static void radio_pages_open_wifi(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    rk_page_close(&s_wifi);
    rk_page_close(&s_wifi_pw);
    rk_page_open(&s_wifi, parent, "WiFi 配网");
    rk_page_action(&s_wifi, "重扫", &s_head_ctx[1], wifi_rescan_cb, NULL);
    rk_page_hint(&s_wifi, "扫描中…");

    /* 上一次已经扫过就直接复用，避免每次进来都等 3 秒 */
    if (s_scan_cnt >= 0 && !s_scan_running) {
        s_scan_listed = false;
        wifi_fill_list();
        s_scan_listed = true;
    } else {
        wifi_start_scan();
    }
}

/* ------------------------------------------------------------------ tick */
void radio_pages_open_wifi_page(lv_obj_t *parent)
{
    radio_pages_open_wifi(parent);
}

void radio_pages_tick(void)
{
    if (rk_page_is_open(&s_wifi) && s_scan_cnt >= 0 && !s_scan_listed) {
        s_scan_listed = true;
        wifi_fill_list();
    }

    if (s_wait_connect && wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        s_wait_connect = false;
        radio_pages_detach();
        if (s_ctl && s_ctl->refresh) {
            s_ctl->refresh();
        }
    }
}
