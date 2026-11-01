/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Actual application/page/settings code with deterministic SDK/service mocks.
 * Tests command routing and lifecycle, not audio, network or physical touch.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_radio.h"
#include "ui_radio_pages.h"
#include "ui_radio_skin.h"
#include "radio_settings.h"
#include "radio_player.h"
#include "radio_catalog.h"
#include "wifi_prov.h"
#include "nvs.h"
#include "sdgoods_app_shell.h"
#include "sdgoods_i18n.h"

static bool exists, fail_commit;
static char skin[24], pending[24];
static struct { char key[16]; int32_t value; } ints[8];
static int int_count;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ (void)ns; if (!exists && mode == NVS_READONLY) return ESP_FAIL; exists = true; *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; pending[0] = 0; }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *value)
{ (void)h; for (int i = 0; i < int_count; ++i) if (!strcmp(key, ints[i].key)) { *value = ints[i].value; return ESP_OK; } return ESP_FAIL; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t value)
{ (void)h; int i; for (i = 0; i < int_count; ++i) if (!strcmp(key, ints[i].key)) break; assert(i < 8); if (i == int_count) ++int_count; snprintf(ints[i].key, 16, "%s", key); ints[i].value = value; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size)
{ (void)h; if (strcmp(key, "skin_id") || !skin[0] || *size < strlen(skin) + 1) return ESP_FAIL; strcpy(out, skin); *size = strlen(skin) + 1; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *s)
{ (void)h; if (!strcmp(key, "skin_id")) snprintf(pending, sizeof(pending), "%s", s); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { (void)h; (void)key; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{ (void)h; if (fail_commit) return ESP_FAIL; if (pending[0]) strcpy(skin, pending); pending[0] = 0; return ESP_OK; }

static radio_player_state_t state;
static int plays, stops;
static bool fail_play;
static char last_play_url[200];
esp_err_t radio_player_init(void) { return ESP_OK; }
esp_err_t radio_player_play(const char *url, const char *codec)
{ snprintf(last_play_url,sizeof(last_play_url),"%s",url); (void)codec; ++plays; if(fail_play)return ESP_FAIL; state = RP_PLAYING; return ESP_OK; }
void radio_player_stop(void) { ++stops; state = RP_IDLE; }
void radio_player_set_paused(bool paused) { state = paused ? RP_PAUSED : RP_PLAYING; }
radio_player_state_t radio_player_state(void) { return state; }
bool radio_player_is_busy(void) { return state == RP_PLAYING || state == RP_CONNECTING || state == RP_BUFFERING; }
int radio_player_levels(uint8_t *out, int n) { memset(out, 80, (size_t)n); return n; }
static wifi_prov_state_t wifi_state=WIFI_STATE_CONNECTED;
static int wifi_submits;
wifi_prov_state_t wifi_prov_get_state(void) { return wifi_state; }
bool wifi_prov_is_setup_mode(void) { return false; }
int wifi_prov_scan(char ssids[][33], int8_t rssi[], int max) { assert(max>0);strcpy(ssids[0],"Test WiFi");rssi[0]=-40;return 1; }
esp_err_t wifi_prov_submit(const char *ssid, const char *pass) { (void)ssid; (void)pass;++wifi_submits;wifi_state=WIFI_STATE_CONNECTING;return ESP_OK; }
static radio_station_t stations[64];
static int catalog_generation;
static char loaded_city[24];
int radio_host_hold_catalog;
void (*radio_host_pending_task)(void *);
void *radio_host_pending_arg;
int radio_catalog_load(const char *city)
{
    snprintf(loaded_city,sizeof(loaded_city),"%s",city?city:"");
    ++catalog_generation;
    for (int i = 0; i < 64; ++i) {
        snprintf(stations[i].name, sizeof(stations[i].name), SDG_T("测试电台 %02d","Test station %02d"), i);
        snprintf(stations[i].url, sizeof(stations[i].url), "https://example.invalid/%d/%d", catalog_generation, i);
        stations[i].freq_mhz = 103.7f;
    }
    return 64;
}
int radio_catalog_count(void) { return 64; }
const radio_station_t *radio_catalog_at(int idx) { return idx >= 0 && idx < 64 ? &stations[idx] : NULL; }
static void (*exit_cb)(void);
void sdgoods_cc_bind(lv_obj_t *scr)
{
    lv_obj_t *capture=lv_obj_create(scr);lv_obj_remove_style_all(capture);
    lv_obj_set_pos(capture,0,0);lv_obj_set_size(capture,360,70);
    lv_obj_clear_flag(capture,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(capture,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_PRESS_LOCK);
}
void sdgoods_app_shell_bind(lv_obj_t *scr) { sdgoods_cc_bind(scr); }
void sdgoods_app_shell_set_exit_cb(void (*cb)(void)) { exit_cb = cb; }
static void (*pause_cb)(void), (*resume_cb)(void);
void sdgoods_app_shell_set_pause_cb(void (*cb)(void)) { pause_cb = cb; }
void sdgoods_app_shell_set_resume_cb(void (*cb)(void)) { resume_cb = cb; }
void sdgoods_app_shell_leave(bool home)
{ (void)home; lv_scr_load(lv_obj_create(NULL)); if (exit_cb) exit_cb(); }
uint32_t sdg_i18n_seq(void) { return 0; }

static lv_obj_t *at(lv_obj_t *o, int x, int y)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return NULL;
    lv_area_t a; lv_obj_get_coords(o, &a);
    if (x < a.x1 || x > a.x2 || y < a.y1 || y > a.y2) return NULL;
    for (int i = (int)lv_obj_get_child_cnt(o) - 1; i >= 0; --i) {
        lv_obj_t *hit = at(lv_obj_get_child(o, i), x, y);
        if (hit) return hit;
    }
    return lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) ? o : NULL;
}

static void tick(void) { lv_tick_inc(130); lv_timer_handler(); lv_refr_now(NULL); }
static void tap(int x, int y)
{
    lv_obj_update_layout(lv_scr_act());
    lv_obj_t *o = at(lv_scr_act(), x, y);
    assert(o && !lv_obj_has_state(o, LV_STATE_DISABLED));
    lv_event_send(o,LV_EVENT_PRESSED,NULL);
    lv_event_send(o,LV_EVENT_RELEASED,NULL);
    lv_event_send(o, LV_EVENT_CLICKED, NULL);
    tick();
}

static bool has_text(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_cnt(o); ++i)
        if (has_text(lv_obj_get_child(o, (int)i), text)) return true;
    return false;
}

static void open_settings(void) { tap(180,332); tap(274,94); }
void radio_host_snapshot(const char *name);

static void gesture(lv_dir_t direction)
{
    lv_indev_t mock = {0};
    mock.proc.types.pointer.gesture_dir = direction;
    lv_event_send(lv_scr_act(),LV_EVENT_PRESSED,NULL);
    lv_event_send(lv_scr_act(),LV_EVENT_GESTURE,&mock);
    lv_event_send(lv_scr_act(),LV_EVENT_RELEASED,NULL);
    tick();
}

static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void pointer_read(lv_indev_drv_t *driver,lv_indev_data_t *data)
{(void)driver;data->point=pointer_point;data->state=pointer_state;}
static void pointer_sample(int x,int y,lv_indev_state_t state)
{
    pointer_point=(lv_point_t){x,y};pointer_state=state;
    lv_tick_inc(40);lv_timer_handler();lv_refr_now(NULL);
}
static void drag(int x,int y,int end_x,int end_y)
{
    pointer_sample(x,y,LV_INDEV_STATE_PRESSED);
    for(int i=1;i<=8;++i)pointer_sample(x+(end_x-x)*i/8,y+(end_y-y)*i/8,LV_INDEV_STATE_PRESSED);
    pointer_sample(end_x,end_y,LV_INDEV_STATE_RELEASED);tick();
}
static lv_obj_t *find_text(lv_obj_t *o,const char *text)
{
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return NULL;
    if(lv_obj_check_type(o,&lv_label_class) && !strcmp(lv_label_get_text(o),text))return o;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(o);++i){lv_obj_t *found=find_text(lv_obj_get_child(o,(int)i),text);if(found)return found;}
    return NULL;
}
static lv_obj_t *find_type(lv_obj_t *o,const lv_obj_class_t *type)
{
    if(lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN))return NULL;
    if(lv_obj_check_type(o,type))return o;
    for(uint32_t i=0;i<lv_obj_get_child_cnt(o);++i){lv_obj_t *found=find_type(lv_obj_get_child(o,(int)i),type);if(found)return found;}
    return NULL;
}
static void check_password_layout(void)
{
    lv_obj_update_layout(lv_scr_act());
    lv_obj_t *title=find_text(lv_scr_act(),SDG_T("输入密码","Password"));
    lv_obj_t *ssid=find_text(lv_scr_act(),"Test WiFi");
    lv_obj_t *field=find_type(lv_scr_act(),&lv_textarea_class);
    lv_obj_t *key=find_text(lv_scr_act(),"q");
    assert(title&&ssid&&field&&key);
    lv_area_t a,b,c,d;lv_obj_get_coords(title,&a);lv_obj_get_coords(ssid,&b);
    lv_obj_get_coords(field,&c);lv_obj_get_coords(lv_obj_get_parent(key),&d);
    assert(a.y2<b.y1 && b.y2<c.y1 && c.y2<d.y1);
    assert(d.x1>=60 && d.x2<300 && d.x2-d.x1+1==44 && d.y2-d.y1+1==34);
}

static void tap_text(const char *text)
{
    lv_obj_t *label=find_text(lv_scr_act(),text);assert(label);
    lv_obj_t *button=lv_obj_get_parent(label);
    lv_area_t area;lv_obj_get_coords(button,&area);
    tap((area.x1+area.x2)/2,(area.y1+area.y2)/2);
}

static void check_password_paging(void)
{
    lv_obj_t *field=find_type(lv_scr_act(),&lv_textarea_class);assert(field);
    tap_text("q");tap_text("1/2>");
    assert(!strcmp(lv_textarea_get_text(field),"q"));
    assert(find_text(lv_scr_act(),"f"));
    tap_text("Aa");assert(find_text(lv_scr_act(),"F"));
    tap_text("F");tap_text("123");tap_text("0");
    assert(!strcmp(lv_textarea_get_text(field),"qF0"));
    tap_text("#+=");tap_text("\"");
    tap_text("1/3>");assert(find_text(lv_scr_act(),"\\"));
    tap_text("\\");tap_text("2/3>");tap_text("~");
    assert(!strcmp(lv_textarea_get_text(field),"qF0\"\\~"));
    tap_text("DEL");tap_text(SDG_T("空格","Space"));
    assert(!strcmp(lv_textarea_get_text(field),"qF0\"\\ "));
    tap_text(SDG_T("显示","Show"));assert(!lv_textarea_get_password_mode(field));
    tap_text(SDG_T("隐藏","Hide"));assert(lv_textarea_get_password_mode(field));
    tap_text("ABC");
    assert(!strcmp(lv_textarea_get_text(field),"qF0\"\\ "));
    radio_host_snapshot("wifi-password-paged");
}

static void navigation_and_network(void)
{
    static lv_indev_drv_t driver;
    lv_indev_drv_init(&driver);driver.type=LV_INDEV_TYPE_POINTER;driver.read_cb=pointer_read;
    lv_indev_drv_register(&driver);
    int old_plays=plays,old_stops=stops;
    drag(180,332,180,212);
    assert(radio_pages_any_open());
    drag(270,148,90,148);
    assert(has_text(lv_scr_act(),SDG_T("测试电台 03","Test station 03")));
    drag(90,148,270,148);
    assert(has_text(lv_scr_act(),SDG_T("测试电台 00","Test station 00")));
    drag(90,148,270,148);
    assert(has_text(lv_scr_act(),SDG_T("测试电台 00","Test station 00")));
    for(int i=0;i<21;++i)drag(270,148,90,148);
    assert(has_text(lv_scr_act(),SDG_T("测试电台 63","Test station 63")));
    drag(180,240,180,120);
    assert(!radio_pages_any_open() && plays==old_plays && stops==old_stops);
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    puts("PASS: real LVGL pointer input: main up opens list, row swipes page both ways without playback, all 64 stations reachable, up closes list.");
    open_settings();
    assert(has_text(lv_scr_act(),SDG_T("已连接","Connected")));
    wifi_state=WIFI_STATE_FAILED;tick();assert(has_text(lv_scr_act(),SDG_T("连接失败","Failed")));
    wifi_state=WIFI_STATE_CONNECTED;tick();assert(has_text(lv_scr_act(),SDG_T("已连接","Connected")));
    tap(180,178);radio_host_snapshot("city-picker");tap(180,196);tick();tick();
    assert(!strcmp(loaded_city,"北京") && !radio_pages_any_open());
    assert(plays==old_plays && stops==old_stops && wifi_submits==0 && state==RP_PLAYING);
    int generation=catalog_generation;
    fail_commit=true;open_settings();tap(180,178);tap(180,244);tick();
    assert(catalog_generation==generation && !strcmp(loaded_city,"北京"));
    fail_commit=false;
    radio_host_hold_catalog=1;open_settings();tap(180,178);tap(180,244);tick();
    assert(radio_host_pending_task && state==RP_PLAYING);
    open_settings();tap(180,178);tap(180,196);tick();
    radio_host_hold_catalog=0;
    void (*pending_fn)(void *)=radio_host_pending_task;radio_host_pending_task=NULL;
    pending_fn(radio_host_pending_arg);tick();tick();
    assert(!strcmp(loaded_city,"北京") && plays==old_plays && stops==old_stops && wifi_submits==0);
    puts("PASS: live network status refresh and city switch preserve audio and never submit Wi-Fi credentials.");
    open_settings();tap(180,142);tick();tap(180,148);check_password_layout();check_password_paging();radio_host_snapshot("wifi-password");
    tap(80,94);assert(has_text(lv_scr_act(),"Test WiFi"));
    tap(180,148);
    lv_obj_t *connect=find_text(lv_scr_act(),SDG_T("连接","Join"));assert(connect);
    lv_area_t a;lv_obj_get_coords(connect,&a);tap((a.x1+a.x2)/2,(a.y1+a.y2)/2);
    assert(wifi_submits==1 && radio_pages_any_open());radio_host_snapshot("wifi-connecting");
    wifi_state=WIFI_STATE_CONNECTED;tick();assert(!radio_pages_any_open());
    assert(plays==old_plays && stops==old_stops);
    puts("PASS: Wi-Fi submit retains connection waiting state; success closes waiting page without restarting current audio.");
}

void radio_host_integration(void)
{
    assert(!strcmp(radio_settings_get_skin(), "dot-matrix"));
    radio_settings_set_night(false);
    assert(!strcmp(radio_settings_get_skin(), "paper-radio"));
    radio_settings_set_night(true);
    assert(!strcmp(radio_settings_get_skin(), "dot-matrix"));
    assert(radio_skin_find("editorial")==0);
    assert(radio_settings_set_skin("vinyl-club"));
    assert(!strcmp(radio_settings_get_skin(),"vinyl-pixel"));
    fail_commit=true;
    assert(!radio_settings_set_skin("editorial"));
    assert(!strcmp(radio_settings_get_skin(),"vinyl-pixel"));
    fail_commit=false;
    assert(radio_settings_set_skin("dot-matrix"));
    radio_settings_set_sleep_min(30);
    radio_settings_set_fav(5, true);
    assert(!radio_settings_set_skin(""));
    assert(!radio_settings_set_skin("this-id-is-too-long-to-be-a-valid-skin-id"));
    lv_obj_t *previous = lv_scr_act();
    ui_radio_start();
    lv_obj_del(previous);
    tick(); tick();
    assert(plays == 1 && state == RP_PLAYING);
    open_settings();radio_host_snapshot("settings"); tap(180, 214);
    tap(262, 298); tap(262, 298); tap(262, 298);
    tap(180, 298);
    assert(!strcmp(radio_settings_get_skin(), "cat-dj"));
    assert(plays == 1 && stops == 0 && state == RP_PLAYING);
    tap(180, 294);
    assert(state == RP_PAUSED);
    open_settings(); tap(180, 214); tap(262, 298); tap(180, 298);
    assert(!strcmp(radio_settings_get_skin(), "space-signal"));
    assert(plays == 1 && stops == 0 && state == RP_PAUSED);
    open_settings(); tap(180, 214); tap(262, 298); tap(80, 94);
    assert(!strcmp(radio_settings_get_skin(), "space-signal"));
    fail_commit = true;
    radio_pages_detach();open_settings(); tap(180, 214); tap(262, 298); tap(180, 298);
    assert(!strcmp(radio_settings_get_skin(), "space-signal"));
    assert(plays == 1 && stops == 0 && state == RP_PAUSED);
    fail_commit = false;
    radio_pages_detach();
    gesture(LV_DIR_LEFT);
    assert(!strcmp(radio_settings_get_skin(),"seaside-sunset"));
    assert(state==RP_PAUSED && plays==1 && stops==0);
    gesture(LV_DIR_RIGHT);
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    gesture(LV_DIR_TOP);
    assert(radio_pages_any_open());radio_pages_detach();gesture(LV_DIR_BOTTOM);
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    for(int i=0;i<RADIO_SKIN_COUNT;i++)gesture(LV_DIR_LEFT);
    for(int i=0;i<RADIO_SKIN_COUNT;i++)gesture(LV_DIR_RIGHT);
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    assert(state==RP_PAUSED && plays==1 && stops==0);
    int original_skin=radio_skin_find(radio_settings_get_skin());
    open_settings();tap(180,214);
    for(int i=original_skin;i<RADIO_SKIN_COUNT-1;++i)tap(262,298);
    assert(has_text(lv_scr_act(),SDG_T("紫夜唱片","Night Vinyl")));
    assert(has_text(lv_scr_act(),SDG_T("外观 13 / 13","Style 13 / 13")));
    radio_host_snapshot("vinyl-picker");tap(180,298);
    assert(!strcmp(radio_settings_get_skin(),"vinyl-night"));
    assert(state==RP_PAUSED && plays==1 && stops==0);
    gesture(LV_DIR_LEFT);
    assert(!strcmp(radio_settings_get_skin(),"paper-radio"));
    gesture(LV_DIR_RIGHT);
    assert(!strcmp(radio_settings_get_skin(),"vinyl-night"));
    for(int i=0;i<=original_skin;++i)gesture(LV_DIR_LEFT);
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    assert(state==RP_PAUSED && plays==1 && stops==0);
    pause_cb(); gesture(LV_DIR_LEFT); resume_cb();
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    tap(180,332);
    gesture(LV_DIR_LEFT);
    assert(!strcmp(radio_settings_get_skin(),"space-signal"));
    radio_host_snapshot("station-list");
    for (int i = 0; i < 21; ++i) tap(262, 298);
    assert(has_text(lv_scr_act(), SDG_T("测试电台 63","Test station 63")));
    tap(180, 145);
    assert(plays == 2 && state == RP_PLAYING);
    assert(radio_settings_get_last_index() == 63);
    assert(radio_settings_get_sleep_min() == 30 && radio_settings_is_fav(5));
    char previous_url[200];
    snprintf(previous_url,sizeof(previous_url),"%s",last_play_url);
    open_settings(); tap(180,286); tick();
    state=RP_ERROR;
    tap(180,294);
    assert(plays==3 && !strcmp(last_play_url,previous_url));
    fail_play=true;
    tap(262,294);
    assert(plays==4 && state==RP_IDLE && has_text(lv_scr_act(),SDG_T("连接失败，点播放重试","Failed - press play to retry")));
    fail_play=false;
    tap(180,294);
    assert(plays==5 && state==RP_PLAYING);
    navigation_and_network();
    ui_radio_close();
    assert(!ui_radio_is_open() && stops == 2);
    puts("PASS: actual application/page flow: legacy migration, playing/paused skin switches without audio restart, cancel, failed-save rollback, all 64 stations reachable, preferences preserved, horizontal gesture cycles, vertical/overlay/page isolation, exit cleanup. SDK/services mocked.");
}
