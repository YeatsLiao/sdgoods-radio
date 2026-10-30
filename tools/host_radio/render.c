/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Native LVGL renderer; hardware and touch input are not simulated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ui_radio_skin.h"
#include "sdgoods_i18n.h"
#include "radio_station_identity.h"

static lv_color_t pixels[360 * 360], drawbuf[360 * 12];
static const char *s_output;
static bool english;
void radio_host_integration(void);
static void clicked(lv_event_t *e)
{
    sdgoods_tap_ctx_t *ctx = lv_event_get_user_data(e);
    if(ctx->valid && ctx->max_disp>12)return;
    if (ctx->cb) ctx->cb(ctx->ud);
}
static void tap_pressed(lv_event_t *e)
{
    sdgoods_tap_ctx_t *ctx=lv_event_get_user_data(e);
    lv_indev_t *input=lv_indev_get_act();
    ctx->valid=input!=NULL;ctx->max_disp=0;
    if(input)lv_indev_get_point(input,&ctx->start);
}
static void tap_pressing(lv_event_t *e)
{
    sdgoods_tap_ctx_t *ctx=lv_event_get_user_data(e);
    lv_indev_t *input=lv_indev_get_act();
    if(!ctx->valid || !input)return;
    lv_point_t point;lv_indev_get_point(input,&point);
    int dx=LV_ABS(point.x-ctx->start.x),dy=LV_ABS(point.y-ctx->start.y);
    if(dx>ctx->max_disp)ctx->max_disp=dx;
    if(dy>ctx->max_disp)ctx->max_disp=dy;
}
const char *sdg_i18n_pick(const char *zh, const char *en) { return english ? en : zh; }
void sdg_i18n_set(sdg_lang_t lang){english=lang==SDG_LANG_EN;}
void sdgoods_tap_bind(lv_obj_t *obj, sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud)
{ ctx->cb=cb;ctx->ud=ud;ctx->valid=false;lv_obj_add_flag(obj,LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_add_event_cb(obj,tap_pressed,LV_EVENT_PRESSED,ctx);
  lv_obj_add_event_cb(obj,tap_pressing,LV_EVENT_PRESSING,ctx);
  lv_obj_add_event_cb(obj,clicked,LV_EVENT_CLICKED,ctx); }

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *colors)
{
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x) {
            if (x >= 0 && x < 360 && y >= 0 && y < 360) pixels[y * 360 + x] = *colors;
            ++colors;
        }
    lv_disp_flush_ready(drv);
}

static void save(const char *dir, const char *id, const char *state)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%s.ppm", dir, id, state);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n360 360\n255\n");
    for (int i = 0; i < 360 * 360; ++i) {
        lv_color32_t c = {.full = lv_color_to32(pixels[i])};
        fputc(c.ch.red, f); fputc(c.ch.green, f); fputc(c.ch.blue, f);
    }
    fclose(f);
}

static void noop(void *arg) { (void)arg; }
void radio_host_snapshot(const char *name){lv_refr_now(NULL);save(s_output,name,"page");}

static uint32_t frame_hash(bool outside_art)
{
    uint32_t hash=2166136261u;
    for(int y=0;y<360;++y)for(int x=0;x<360;++x){
        if(outside_art && x>=RADIO_SKIN_ART_ROOT_X && x<RADIO_SKIN_ART_ROOT_X+RADIO_SKIN_ART_ROOT_W &&
           y>=RADIO_SKIN_ART_ROOT_Y && y<RADIO_SKIN_ART_ROOT_Y+RADIO_SKIN_ART_ROOT_H)continue;
        hash=(hash^pixels[y*360+x].full)*16777619u;
    }
    return hash;
}

static void assert_circle_bounds(const lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj,&area);
    const int xs[]={area.x1,area.x2},ys[]={area.y1,area.y2};
    for(int x=0;x<2;++x)for(int y=0;y<2;++y){
        int dx=xs[x]-180,dy=ys[y]-180;
        assert(dx*dx+dy*dy<=176*176);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_output=argv[1];
    lv_init();
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, drawbuf, NULL, 360 * 12);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = drv.ver_res = 360;
    drv.draw_buf = &buffer;
    drv.flush_cb = flush;
    lv_disp_drv_register(&drv);
    const radio_skin_actions_t actions = {noop, noop, noop, noop, noop};
    assert(radio_skin_find("unknown-id") == RADIO_SKIN_DEFAULT);
    assert(RADIO_SKIN_COUNT==13);
    assert(radio_skin_find("editorial")==0);
    assert(radio_skin_get(0)->layout==RADIO_LAYOUT_PAPER);
    assert(!strcmp(radio_skin_get(0)->zh,"纸感收音机"));
    assert(radio_skin_find("vinyl-club")==10);
    unsigned animated=0;
    char brand[128],tiny[5];
    radio_station_short_name("杭州市广播电视台西湖之声",brand,sizeof(brand));
    assert(!strcmp(brand,"西湖之声"));
    assert(radio_station_known_frequency(brand)>105.39f && radio_station_known_frequency(brand)<105.41f);
    assert(radio_station_known_frequency("北京交通广播")==0.0f);
    assert(radio_station_known_frequency("中国之声")==0.0f);
    radio_station_copy_utf8("西湖之声",tiny,sizeof(tiny));
    assert(!strcmp(tiny,"西"));
    uint8_t levels[18];
    for (int i = 0; i < 18; ++i) levels[i] = (uint8_t)(i * 37 % 256);
    for (int round = 0; round < 12; ++round) {
        for (int i = 0; i < RADIO_SKIN_COUNT; ++i) {
            assert(radio_skin_find(radio_skin_get(i)->id) == i);
            radio_skin_apply_palette(i);
            lv_obj_t *old = lv_scr_act();
            lv_obj_t *scr = lv_obj_create(NULL);
            lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
            radio_skin_view_t view;
            english = false;
            radio_skin_view_create(&view, scr, i, &actions);
            lv_scr_load(scr);
            lv_obj_del(old);
            english = false;
            radio_skin_view_update(&view, "城市音乐台", "FM 103.7", "直播中", "11:21", true, levels, 18);
            lv_refr_now(NULL);
            assert(lv_obj_get_y(view.clock)==RADIO_SKIN_CLOCK_Y);
            assert(lv_obj_get_y(view.status)==RADIO_SKIN_STATUS_Y);
            assert(lv_obj_get_y(view.live_dot)==RADIO_SKIN_STATUS_Y+5);
            assert(RADIO_SKIN_CLOCK_Y+RADIO_SKIN_CLOCK_H<=RADIO_SKIN_STATUS_Y);
            assert(RADIO_SKIN_STATUS_Y+RADIO_SKIN_STATUS_H<=RADIO_SKIN_ART_ROOT_Y);
            assert_circle_bounds(view.clock);
            assert_circle_bounds(view.status);
            assert(!lv_obj_has_flag(view.clock,LV_OBJ_FLAG_CLICKABLE));
            assert(!lv_obj_has_flag(view.status,LV_OBJ_FLAG_CLICKABLE));
            if(view.art_root){
                assert_circle_bounds(view.art_root);
                if(view.image){
                    assert(radio_skin_get(i)->art->header.w==RADIO_SKIN_ART_W);
                    assert(radio_skin_get(i)->art->header.h==RADIO_SKIN_ART_H);
                }else assert(radio_skin_get(i)->layout==RADIO_LAYOUT_VINYL);
                assert(RADIO_SKIN_ART_ROOT_Y+RADIO_SKIN_ART_ROOT_H<=lv_obj_get_y(view.name));
                assert(lv_obj_get_height(view.art_fx)==RADIO_SKIN_ART_H+2);
            }
            if(view.paper_dial){
                assert_circle_bounds(view.paper_dial);
                assert(lv_obj_get_style_clip_corner(view.paper_dial,0));
                assert(lv_obj_get_style_border_post(view.paper_dial,0));
                lv_obj_t *window=lv_obj_get_child(view.paper_dial,0);
                assert(lv_obj_get_style_clip_corner(window,0));
                assert(lv_obj_get_x(window)>=RADIO_PAPER_SCENE_INSET);
                assert(lv_obj_get_y(window)>=RADIO_PAPER_SCENE_INSET);
                assert(lv_obj_get_x(window)+lv_obj_get_width(window)<=lv_obj_get_width(view.paper_dial)-RADIO_PAPER_SCENE_INSET);
                lv_obj_t *scene=lv_obj_get_child(window,0);
                assert(lv_obj_get_width(scene)==RADIO_PAPER_SCENE_W);
                assert(lv_obj_get_height(scene)==RADIO_PAPER_SCENE_H);
                assert(lv_obj_get_y(view.name)+lv_obj_get_height(view.name)<=lv_obj_get_y(view.meta));
                assert(lv_obj_get_y(view.meta)+lv_obj_get_height(view.meta)<lv_obj_get_y(view.paper_dial));
                assert(!lv_obj_has_flag(view.paper_needle,LV_OBJ_FLAG_HIDDEN));
            }
            if (round == 0) save(argv[1], radio_skin_get(i)->id, "playing");
            if(round==0 && view.art_fx){
                ++animated;
                assert(!lv_obj_has_flag(view.art_root,LV_OBJ_FLAG_CLICKABLE));
                uint32_t outside=frame_hash(true),first=0;
                bool changed=false;
                for(int f=0;f<60;++f){
                    lv_tick_inc(120);
                    radio_skin_view_animate(&view,lv_tick_get(),true);
                    lv_refr_now(NULL);
                    assert(frame_hash(true)==outside);
                    if(view.image){
                        assert(lv_obj_get_x(view.image)>=0 && lv_obj_get_y(view.image)>=0);
                        assert(lv_obj_get_x(view.image)+RADIO_SKIN_ART_W<=RADIO_SKIN_ART_ROOT_W);
                        assert(lv_obj_get_y(view.image)+RADIO_SKIN_ART_H<=RADIO_SKIN_ART_ROOT_H);
                    }
                    uint32_t hash=frame_hash(false);
                    if(f==0)first=hash;else if(hash!=first)changed=true;
                    char state[24];snprintf(state,sizeof(state),"anim-%02d",f);
                    save(argv[1],radio_skin_get(i)->id,state);
                }
                assert(changed);
                uint32_t phase=view.motion_ms;
                lv_tick_inc(2000);
                radio_skin_view_animate(&view,lv_tick_get(),false);
                assert(view.motion_ms==phase);
                lv_tick_inc(120);
                radio_skin_view_animate(&view,lv_tick_get(),true);
                assert(view.motion_ms==(phase+120)%7200);
                radio_skin_view_update(&view,"城市音乐台","FM 103.7","已暂停","11:21",false,NULL,0);
                radio_skin_view_animate(&view,lv_tick_get(),true);
                lv_refr_now(NULL);
                uint32_t paused_phase=view.motion_ms,paused_pixels=frame_hash(false);
                lv_tick_inc(1000);
                radio_skin_view_animate(&view,lv_tick_get(),true);
                lv_refr_now(NULL);
                assert(view.motion_ms!=paused_phase && view.motion_running);
                if(radio_skin_get(i)->layout==RADIO_LAYOUT_VINYL)assert(frame_hash(false)!=paused_pixels);
                assert(!strcmp(lv_label_get_text(view.status),"已暂停"));
                radio_skin_view_update(&view,"城市音乐台","FM 103.7","连接失败，点播放重试","11:21",false,NULL,0);
                uint32_t error_phase=view.motion_ms;
                lv_tick_inc(120);
                radio_skin_view_animate(&view,lv_tick_get(),true);
                lv_refr_now(NULL);
                assert(view.motion_ms!=error_phase && view.motion_running);
            }
            radio_skin_view_update(&view, "杭州华语之声网络广播", "网络电台", "直播中", "11:21", true, levels, 18);
            lv_refr_now(NULL);
            if (i == 0) {
                assert(lv_obj_get_style_text_font(view.name,0)->line_height==40);
                assert(!strcmp(lv_label_get_text(view.meta),"网络电台"));
                assert(lv_label_get_long_mode(view.name)==LV_LABEL_LONG_SCROLL_CIRCULAR);
            }
            if (i == 1) assert(!strcmp(view.dots.text, "WEB") && view.dot_wave);
            if (round == 0 && i < 2) save(argv[1], radio_skin_get(i)->id, "network-station");
            radio_skin_view_update(&view,"杭州市广播电视台西湖之声","FM 105.4","直播中","11:21",true,levels,18);
            if(i==0){
                assert(!strcmp(lv_label_get_text(view.name),"西湖之声"));
                assert(!strcmp(lv_label_get_text(view.meta),"FM 105.4"));
                assert(view.paper_dial && !view.art_fx);
                /* lv_obj_set_x 只改样式 x，坐标要等布局刷新后才能读到，不能直接 get_x 断言 */
                lv_obj_update_layout(view.paper_needle);
                assert(lv_obj_get_x(view.paper_needle)>=232 && lv_obj_get_x(view.paper_needle)<=233);
            }
            if(i==1)assert(!strcmp(view.dots.text,"105.4"));
            lv_refr_now(NULL);
            if(round==0 && i<2)save(argv[1],radio_skin_get(i)->id,"xihu");
            radio_skin_view_update(&view,"Radio Paradise Main","CH 03","直播中","11:21",true,levels,18);
            if(i==0){assert(!strcmp(lv_label_get_text(view.meta),"CH 03"));assert(lv_obj_has_flag(view.paper_needle,LV_OBJ_FLAG_HIDDEN));}
            if(i==1){
                assert(!strcmp(view.dots.text,"03"));
                assert(!strcmp(lv_label_get_text(view.meta),"网络频道"));
            }
            radio_skin_view_update(&view, "杭州交通经济广播超长台名测试", "网络电台", "已暂停", "11:21", false, NULL, 0);
            lv_refr_now(NULL);
            assert(strcmp(lv_label_get_text(view.status), "已暂停") == 0);
            assert((rk_icon_t)(uintptr_t)lv_obj_get_user_data(view.play_icon) == RK_ICON_PLAY);
            if (round == 0) save(argv[1], radio_skin_get(i)->id, "long-paused");
            english = true;
            radio_skin_view_update(&view, "A very long international radio station name", "Internet radio", "Failed - press play to retry", "11:21", false, NULL, 0);
            lv_refr_now(NULL);
            if (round == 0) save(argv[1], radio_skin_get(i)->id, "english-error");
            lv_tick_inc(120);
            lv_timer_handler();
        }
    }
    assert(animated==10);
    for(int i=0;i<RADIO_SKIN_COUNT;++i){
        lv_obj_t *old=lv_scr_act(),*scr=lv_obj_create(NULL);
        lv_scr_load(scr);lv_obj_del(old);
        radio_skin_apply_palette(i);
        lv_obj_set_style_bg_color(scr,lv_color_hex(radio_skin_get(i)->bg),0);
        radio_skin_preview(scr,i);
        lv_refr_now(NULL);
        save(argv[1],radio_skin_get(i)->id,"preview");
    }
    printf("PASS: %d actual LVGL view rebuilds, registry fallback, label layout, live/paused/error/long text rendering; all previews created.\n",12*RADIO_SKIN_COUNT);
    printf("PASS: %u animated skins, %u frames, art bounds preserved, paused motion continues, hidden clocks frozen without catch-up.\n",animated,animated*60);
    english = true;
    radio_host_integration();
    return 0;
}
