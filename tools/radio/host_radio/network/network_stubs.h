/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Host SDK shims; no real Wi-Fi or network traffic.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_NVS_NOT_FOUND -4
static inline const char *esp_err_to_name(int e){(void)e;return "mock error";}
static inline void mock_log(const char *tag,const char *format,...){(void)tag;(void)format;}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
int nvs_open(const char *,int,nvs_handle_t *);
int nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
int nvs_set_str(nvs_handle_t,const char *,const char *);
int nvs_erase_key(nvs_handle_t,const char *);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef int esp_event_base_t;
typedef void *esp_event_handler_instance_t;
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define WIFI_EVENT_AP_START 3
#define IP_EVENT_STA_GOT_IP 4
#define WIFI_REASON_ASSOC_LEAVE 8
typedef struct{int reason;}wifi_event_sta_disconnected_t;
typedef struct{uint32_t addr;}esp_ip4_addr_t;
typedef struct{esp_ip4_addr_t ip;}esp_netif_ip_info_t;
typedef struct{esp_netif_ip_info_t ip_info;}ip_event_got_ip_t;
typedef void esp_netif_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) ((p)->addr&255),(((p)->addr>>8)&255),(((p)->addr>>16)&255),(((p)->addr>>24)&255)
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
int esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *);
int esp_event_handler_instance_register(int,int,void (*)(void *,int,int,void *),void *,void **);
#define WIFI_IF_AP 1
#define WIFI_IF_STA 0
#define WIFI_MODE_APSTA 2
#define WIFI_MODE_STA 1
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_PS_NONE 0
typedef struct{uint8_t ssid[33];int8_t rssi;int authmode;}wifi_ap_record_t;
typedef struct{bool show_hidden;}wifi_scan_config_t;
typedef struct{
 struct{uint8_t ssid[33],password[65];struct{int authmode;}threshold;struct{bool capable,required;}pmf_cfg;}sta;
 struct{uint8_t ssid[33];int ssid_len,channel,max_connection,authmode;}ap;
}wifi_config_t;
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
int esp_wifi_stop(void);
int esp_wifi_start(void);
int esp_wifi_set_mode(int);
int esp_wifi_set_config(int,const wifi_config_t *);
int esp_wifi_connect(void);
int esp_wifi_set_ps(int);
int esp_wifi_get_mac(int,uint8_t *);
int esp_wifi_scan_start(const wifi_scan_config_t *,bool);
int esp_wifi_scan_get_ap_records(uint16_t *,wifi_ap_record_t *);
typedef struct timer_mock{void (*callback)(void *);void *arg;uint64_t delay;bool active;}*esp_timer_handle_t;
typedef struct{void (*callback)(void *);void *arg;int dispatch_method;const char *name;}esp_timer_create_args_t;
#define ESP_TIMER_TASK 0
int esp_timer_create(const esp_timer_create_args_t *,esp_timer_handle_t *);
int esp_timer_start_once(esp_timer_handle_t,uint64_t);
int esp_timer_stop(esp_timer_handle_t);
typedef void *httpd_handle_t;
typedef struct{int content_len;}httpd_req_t;
typedef int httpd_err_code_t;
typedef struct{int server_port,task_priority,stack_size,max_open_sockets;bool lru_purge_enable;}httpd_config_t;
typedef struct{const char *uri;int method;int (*handler)(httpd_req_t *);}httpd_uri_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_404_NOT_FOUND 404
#define HTTP_GET 0
#define HTTP_POST 1
int httpd_req_recv(httpd_req_t *,char *,size_t);
int httpd_resp_send(httpd_req_t *,const char *,int);
int httpd_resp_send_err(httpd_req_t *,int,const char *);
int httpd_resp_set_type(httpd_req_t *,const char *);
int httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
int httpd_resp_set_status(httpd_req_t *,const char *);
int httpd_start(void **,const httpd_config_t *);
int httpd_stop(void *);
int httpd_register_uri_handler(void *,const httpd_uri_t *);
int httpd_register_err_handler(void *,int,int (*)(httpd_req_t *,int));
typedef void *dns_server_handle_t;
typedef struct{int dummy;}dns_server_config_t;
#define DNS_SERVER_CONFIG_SINGLE(a,b) ((dns_server_config_t){0})
void *start_dns_server(const dns_server_config_t *);
void stop_dns_server(void *);

