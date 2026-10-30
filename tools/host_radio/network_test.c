/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Actual Wi-Fi provider/state observer with deterministic SDK shims.
 */
#include "network_stubs.h"
#include "../../main/wifi_prov.c"
#include "../../main/wifi_prov_status.c"

static bool associated,netif_ready=true,lease_ready=true,fail_nvs,fail_timer;
static uint32_t ip_address;
static int stops,connects,mode,registrations,timer_count;
static char configured_ssid[33],values[3][80],staged[3][80];
static struct timer_mock timers[4];
static int key_index(const char *key){return !strcmp(key,"ssid")?0:!strcmp(key,"pass")?1:2;}
int nvs_open(const char *ns,int rw,int *h){(void)ns;(void)rw;*h=1;memcpy(staged,values,sizeof(values));return 0;}
int nvs_get_str(int h,const char *key,char *out,size_t *n){(void)h;const char *v=values[key_index(key)];if(strlen(v)+1>*n)return -1;strcpy(out,v);*n=strlen(v)+1;return 0;}
int nvs_set_str(int h,const char *key,const char *v){(void)h;if(fail_nvs)return -1;snprintf(staged[key_index(key)],80,"%s",v);return 0;}
int nvs_erase_key(int h,const char *key){(void)h;staged[key_index(key)][0]=0;return 0;}
int nvs_commit(int h){(void)h;if(fail_nvs)return -1;memcpy(values,staged,sizeof(values));return 0;}
void nvs_close(int h){(void)h;}
void *esp_netif_get_handle_from_ifkey(const char *key){assert(!strcmp(key,"WIFI_STA_DEF"));return netif_ready?(void *)1:NULL;}
void *esp_netif_create_default_wifi_ap(void){return (void *)2;}
int esp_netif_get_ip_info(void *n,esp_netif_ip_info_t *info){(void)n;info->ip.addr=ip_address;return lease_ready?0:-1;}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap){memset(ap,0,sizeof(*ap));return associated?0:-1;}
int esp_event_handler_instance_register(int base,int id,void (*f)(void *,int,int,void *),void *arg,void **h){(void)base;(void)id;(void)f;(void)arg;*h=(void *)1;++registrations;return 0;}
int esp_wifi_stop(void){++stops;associated=false;ip_address=0;return 0;}
int esp_wifi_start(void){return 0;}
int esp_wifi_set_mode(int m){mode=m;return 0;}
int esp_wifi_set_config(int which,const wifi_config_t *cfg){if(which==WIFI_IF_STA)snprintf(configured_ssid,33,"%s",cfg->sta.ssid);return 0;}
int esp_wifi_connect(void){++connects;return 0;}
int esp_wifi_set_ps(int ps){assert(ps==WIFI_PS_NONE);return 0;}
int esp_wifi_get_mac(int which,uint8_t *mac){(void)which;memset(mac,0,6);return 0;}
int esp_wifi_scan_start(const wifi_scan_config_t *cfg,bool wait){(void)cfg;(void)wait;return 0;}
int esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *records){(void)records;*n=0;return 0;}
int esp_timer_create(const esp_timer_create_args_t *cfg,esp_timer_handle_t *t){assert(timer_count<4);*t=&timers[timer_count++];(*t)->callback=cfg->callback;(*t)->arg=cfg->arg;return 0;}
int esp_timer_start_once(esp_timer_handle_t t,uint64_t delay){if(fail_timer)return -1;t->delay=delay;t->active=true;return 0;}
int esp_timer_stop(esp_timer_handle_t t){t->active=false;return 0;}
int httpd_req_recv(httpd_req_t *r,char *b,size_t n){(void)r;(void)b;(void)n;return -1;}
int httpd_resp_send(httpd_req_t *r,const char *b,int n){(void)r;(void)b;(void)n;return 0;}
int httpd_resp_send_err(httpd_req_t *r,int c,const char *b){(void)r;(void)c;(void)b;return 0;}
int httpd_resp_set_type(httpd_req_t *r,const char *b){(void)r;(void)b;return 0;}
int httpd_resp_set_hdr(httpd_req_t *r,const char *a,const char *b){(void)r;(void)a;(void)b;return 0;}
int httpd_resp_set_status(httpd_req_t *r,const char *b){(void)r;(void)b;return 0;}
int httpd_start(void **h,const httpd_config_t *cfg){(void)cfg;*h=(void *)1;return 0;}
int httpd_stop(void *h){(void)h;return 0;}
int httpd_register_uri_handler(void *h,const httpd_uri_t *u){(void)h;(void)u;return 0;}
int httpd_register_err_handler(void *h,int code,int (*fn)(httpd_req_t *,int)){(void)h;(void)code;(void)fn;return 0;}
void *start_dns_server(const dns_server_config_t *cfg){(void)cfg;return (void *)1;}
void stop_dns_server(void *d){(void)d;}
static void got_ip(uint32_t address)
{
    associated=true;ip_address=address;
    ip_event_got_ip_t event={.ip_info.ip.addr=address};
    on_ip_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&event);
}
int main(void)
{
    associated=true;ip_address=0x6401a8c0u;
    assert(wifi_prov_start()==ESP_OK && wifi_prov_get_state()==WIFI_STATE_CONNECTED && stops==0);
    assert(wifi_prov_start()==ESP_OK && registrations==2 && timer_count==1);
    assert(!strcmp(wifi_prov_get_ip(),"192.168.1.100"));
    ip_address=0;assert(wifi_prov_get_state()==WIFI_STATE_CONNECTING);
    associated=false;ip_address=0x6401a8c0u;assert(wifi_prov_get_state()==WIFI_STATE_CONNECTING);
    associated=true;netif_ready=false;assert(wifi_prov_get_state()==WIFI_STATE_CONNECTING);
    netif_ready=true;lease_ready=false;assert(wifi_prov_get_state()==WIFI_STATE_CONNECTING);
    lease_ready=true;assert(wifi_prov_get_state()==WIFI_STATE_CONNECTED);
    puts("PASS: actual provider adopts an existing connection, starts once, and requires both association and valid IPv4.");
    associated=false;ip_address=0;
    wifi_event_sta_disconnected_t lost={.reason=200};
    for(int i=0;i<12;++i)on_wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&lost);
    assert(stops==0 && s_state==WIFI_STATE_FAILED && s_retry_timer->active && s_retry_timer->delay==5000000);
    s_retry_timer->callback(NULL);assert(connects==1);
    got_ip(0x6501a8c0u);assert(!s_retry_timer->active && wifi_prov_get_state()==WIFI_STATE_CONNECTED);
    puts("PASS: repeated signal loss uses deferred backoff without stopping Wi-Fi or switching into a setup hotspot; recovery cancels retry.");
    assert(wifi_prov_submit("new-network","password")==ESP_OK);
    assert(wifi_prov_get_state()==WIFI_STATE_CONNECTING && s_connect_timer->active);
    assert(wifi_prov_submit("other","second")==ESP_ERR_INVALID_STATE && !strcmp(values[0],"new-network"));
    s_connect_timer->active=false;s_connect_timer->callback(NULL);
    assert(stops==1 && connects==2 && !strcmp(configured_ssid,"new-network"));
    wifi_event_sta_disconnected_t expected={.reason=WIFI_REASON_ASSOC_LEAVE};
    on_wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&expected);
    assert(!s_retry_timer->active);
    got_ip(0x6601a8c0u);assert(wifi_prov_get_state()==WIFI_STATE_CONNECTED);
    puts("PASS: pending credential change cannot report old Wi-Fi as success, duplicate submissions do not overwrite it, intentional disconnect is ignored.");
    fail_nvs=true;assert(wifi_prov_submit("fail","bad")==ESP_FAIL);fail_nvs=false;
    assert(stops==1 && !s_connect_timer->active && !strcmp(values[0],"new-network"));
    fail_timer=true;assert(wifi_prov_submit("timer-fail","bad")==ESP_FAIL);fail_timer=false;
    assert(!atomic_load(&s_connect_pending) && stops==1);
    assert(wifi_prov_submit(NULL,"bad")==ESP_ERR_INVALID_ARG);
    assert(timer_count==2);
    puts("PASS: persistence/timer failures are returned, pending state clears, and connection timers are reused.");
    return 0;
}

