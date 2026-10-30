/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Runs the real player with deterministic task, HTTP, decoder and I2S shims.
 * Verifies control flow and ownership; does not decode audio or test Wi-Fi.
 */
#include "player_stubs.h"
#include <setjmp.h>
#include "../../../main/radio_player.c"

enum { NORMAL, SLOW, SUPERSEDE, BAD_CODEC, NO_PROGRESS, START_FAIL, PAUSE_FIRST, EMPTY_STREAM, WRONG_HINT, WRITE_FAIL, SHORT_GAP };
static int mode, reads, decoded, starts, writes, allocated, opens, codec_type;
static TickType_t ticks;
static void (*pending_task)(void *);
static bool resumed;
static bool run_commands;
static int wake_reads;
static jmp_buf task_done;

size_t strlcpy(char *out,const char *in,size_t cap) { size_t n=strlen(in); if(cap){size_t k=n<cap-1?n:cap-1;memcpy(out,in,k);out[k]=0;}return n; }
size_t strlcat(char *out,const char *in,size_t cap) { size_t n=strlen(out);return n+strlcpy(out+n,in,cap-n); }
void *heap_caps_malloc(size_t size,int caps) { (void)caps;void *p=malloc(size);if(p)++allocated;return p; }
void heap_caps_free(void *p) { if(p)--allocated;free(p); }
size_t heap_caps_get_free_size(int caps) { (void)caps;return 100000; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { static uintptr_t next=2;return (void *)next++; }
int xSemaphoreTake(SemaphoreHandle_t s,unsigned timeout) {
    (void)timeout;
    if(run_commands && s==s_wake){
        if(++wake_reads==1)assert(radio_player_play("https://test.invalid/live","MP3")==ESP_OK);
        if(wake_reads==3)longjmp(task_done,1);
    }
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s;return 1; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay)
{
    assert(delay>0);ticks+=delay;
    if(pending_task){void (*fn)(void *)=pending_task;pending_task=NULL;fn(NULL);}
    if(mode==PAUSE_FIRST && s_state==RP_PAUSED && !resumed){
        resumed=true;radio_player_set_paused(false);assert(s_state!=RP_PLAYING);
    }
}
void vTaskDelete(void *t) { (void)t; }
void vTaskDeleteWithCaps(void *t) { (void)t; }
int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,int stack,void *arg,int prio,TaskHandle_t *handle,int core)
{ (void)fn;(void)name;(void)stack;(void)arg;(void)prio;(void)core;*handle=(void *)1;return pdPASS; }
int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *),const char *name,int stack,void *arg,int prio,TaskHandle_t *handle,int core,int caps)
{ (void)name;(void)stack;(void)arg;(void)prio;(void)handle;(void)core;(void)caps;assert(!pending_task);pending_task=fn;return pdPASS; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) { (void)cfg;reads=0;++opens;return (void *)1; }
int esp_http_client_set_header(void *h,const char *k,const char *v) { (void)h;(void)k;(void)v;return 0; }
int esp_http_client_open(void *h,int len)
{
    (void)h;(void)len;
    if(mode==SLOW)ticks+=pdMS_TO_TICKS(8000);
    if(mode==SUPERSEDE)assert(radio_player_play("https://new.invalid/live","MP3")==ESP_OK);
    return ESP_OK;
}
int esp_http_client_fetch_headers(void *h) { (void)h;assert(s_state==RP_CONNECTING);return 0; }
int esp_http_client_get_status_code(void *h) { (void)h;return 200; }
int esp_http_client_close(void *h) { (void)h;return 0; }
int esp_http_client_cleanup(void *h) { (void)h;return 0; }
int esp_http_client_reset_redirect_counter(void *h) { (void)h;return 0; }
int esp_http_client_set_url(void *h,const char *u) { (void)h;(void)u;return 0; }
int esp_http_client_set_timeout_ms(void *h,int ms) { (void)h;(void)ms;return 0; }
int esp_http_client_read(void *h,char *out,int cap)
{
    (void)h;
    if(mode==EMPTY_STREAM){ticks+=pdMS_TO_TICKS(2500);return -ESP_ERR_HTTP_EAGAIN;}
    int chunks=mode==BAD_CODEC||mode==WRONG_HINT?4:1;
    if(reads++>=chunks)return 0;
    memset(out,0x55,(size_t)cap);return cap;
}
int esp_audio_simple_dec_open(const esp_audio_simple_dec_cfg_t *cfg,void **dec) { codec_type=cfg->dec_type;*dec=(void *)1;return 0; }
int esp_audio_simple_dec_process(void *d,esp_audio_simple_dec_raw_t *in,esp_audio_simple_dec_out_t *out)
{
    (void)d;assert(++decoded<1000);
    in->consumed=mode==NO_PROGRESS?0:in->len;
    bool bad=mode==BAD_CODEC||(mode==WRONG_HINT&&codec_type!=ESP_AUDIO_SIMPLE_DEC_TYPE_AAC);
    out->decoded_size=bad?0:512;memset(out->buffer,0,out->decoded_size);return 0;
}
int esp_audio_simple_dec_get_info(void *d,esp_audio_simple_dec_info_t *info) { (void)d;info->sample_rate=44100;info->channel=2;return 0; }
int esp_audio_simple_dec_close(void *d) { (void)d;return 0; }
int esp_audio_dec_register_default(void) { return 0; }
int esp_audio_simple_dec_register_default(void) { return 0; }
int sdgoods_audio_stream_start(uint32_t rate,uint8_t ch) { assert(rate==44100&&ch==2);++starts;return mode==START_FAIL?ESP_FAIL:ESP_OK; }
int sdgoods_audio_stream_write(const int16_t *pcm,size_t bytes) {
    (void)pcm;assert(bytes>0);++writes;
    if(mode==SHORT_GAP)ticks+=pdMS_TO_TICKS(15000);
    return mode==WRITE_FAIL?ESP_FAIL:ESP_OK;
}
void sdgoods_audio_stream_stop(void) {}
static uint8_t raw[RP_RAW_CAP], pcm[RP_OUT_CAP];
static void reset(int next)
{
    mode=next;reads=decoded=starts=writes=opens=0;resumed=false;pending_task=NULL;ticks=0;
    s_seq++;s_state=RP_CONNECTING;s_paused=next==PAUSE_FIRST;s_rate=0;s_ch=0;
}
static int attempt(void) { bool played=false;return rp_attempt("https://test.invalid/live",false,s_seq,raw,pcm,&played); }
int main(void)
{
    assert(radio_player_init()==ESP_OK);
    int baseline=allocated;
    for(int i=0;i<100;++i){reset(NORMAL);assert(attempt()==RP_END_DISCONNECT);assert(writes>0);assert(allocated==baseline);}
    puts("PASS: 100 real player attempts release every HTTP read buffer.");
    reset(SLOW);attempt();assert(writes>0);
    puts("PASS: 8-second connection does not consume first-frame timeout.");
    reset(SUPERSEDE);assert(attempt()==RP_END_SUPERSEDED);assert(s_state==RP_CONNECTING);assert(starts==0);
    puts("PASS: superseded connection cannot overwrite the new command state or start audio.");
    reset(PAUSE_FIRST);attempt();assert(resumed&&writes>0);
    puts("PASS: resume before first frame does not claim PLAYING prematurely.");
    reset(NO_PROGRESS);attempt();assert(decoded==1);
    reset(START_FAIL);attempt();assert(decoded==1&&writes==0);
    reset(WRITE_FAIL);attempt();assert(decoded==1&&writes==1);
    reset(SHORT_GAP);bool stable=true;rp_attempt("https://test.invalid/live",false,s_seq,raw,pcm,&stable);assert(!stable);
    puts("PASS: zero-consumption decoder and audio-start failure leave the decode loop.");
    reset(BAD_CODEC);rp_session("https://test.invalid/live","MP3",s_seq,raw,pcm);assert(s_state==RP_ERROR&&opens==3);
    reset(EMPTY_STREAM);rp_session("https://test.invalid/live","MP3",s_seq,raw,pcm);assert(s_state==RP_ERROR&&ticks<=pdMS_TO_TICKS(32000));
    reset(WRONG_HINT);rp_session("https://test.invalid/live","UNKNOWN",s_seq,raw,pcm);assert(writes>0&&s_state==RP_ERROR);
    puts("PASS: bad/empty streams terminate; UNKNOWN codec can fall back to AAC; brief frames cannot reset retry budget forever.");
    assert(allocated==baseline);
    reset(BAD_CODEC);run_commands=true;
    if(setjmp(task_done)==0)rp_task(NULL);
    run_commands=false;
    assert(opens==3);
    puts("PASS: duplicate wake tokens do not restart an already handled command.");
    return 0;
}

