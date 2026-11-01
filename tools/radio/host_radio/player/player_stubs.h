/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Deterministic host shims; no real networking, codec or audio.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_HTTP_EAGAIN 7007
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(ms) ((ms)/10)
#define pdPASS 1
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_METHOD_GET 0
typedef struct {int event_id; char *header_key,*header_value;} esp_http_client_event_t;
typedef void *esp_http_client_handle_t;
typedef struct {const char *url;int method,timeout_ms,buffer_size;void *crt_bundle_attach;bool keep_alive_enable;esp_err_t (*event_handler)(esp_http_client_event_t *);} esp_http_client_config_t;
#define esp_crt_bundle_attach NULL
typedef int esp_audio_err_t;
typedef void *esp_audio_simple_dec_handle_t;
#define ESP_AUDIO_ERR_OK 0
#define ESP_AUDIO_SIMPLE_DEC_TYPE_AAC 1
#define ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 2
typedef struct {int dec_type;void *dec_cfg;int cfg_size;bool use_frame_dec;} esp_audio_simple_dec_cfg_t;
typedef struct {uint8_t *buffer;uint32_t len,consumed;} esp_audio_simple_dec_raw_t;
typedef struct {uint8_t *buffer;uint32_t len,decoded_size;} esp_audio_simple_dec_out_t;
typedef struct {uint32_t sample_rate;uint8_t channel;} esp_audio_simple_dec_info_t;
size_t strlcpy(char *,const char *,size_t);
size_t strlcat(char *,const char *,size_t);
void *heap_caps_malloc(size_t,int);
void heap_caps_free(void *);
size_t heap_caps_get_free_size(int);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
void vTaskDelete(void *);
void vTaskDeleteWithCaps(void *);
int xTaskCreatePinnedToCore(void (*)(void *),const char *,int,void *,int,TaskHandle_t *,int);
int xTaskCreatePinnedToCoreWithCaps(void (*)(void *),const char *,int,void *,int,TaskHandle_t *,int,int);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_header(void *,const char *,const char *);
int esp_http_client_open(void *,int);
int esp_http_client_fetch_headers(void *);
int esp_http_client_get_status_code(void *);
int esp_http_client_close(void *);
int esp_http_client_cleanup(void *);
int esp_http_client_reset_redirect_counter(void *);
int esp_http_client_set_url(void *,const char *);
int esp_http_client_set_timeout_ms(void *,int);
int esp_http_client_read(void *,char *,int);
int esp_audio_simple_dec_open(const esp_audio_simple_dec_cfg_t *,void **);
int esp_audio_simple_dec_process(void *,esp_audio_simple_dec_raw_t *,esp_audio_simple_dec_out_t *);
int esp_audio_simple_dec_get_info(void *,esp_audio_simple_dec_info_t *);
int esp_audio_simple_dec_close(void *);
int esp_audio_dec_register_default(void);
int esp_audio_simple_dec_register_default(void);
int sdgoods_audio_stream_start(uint32_t,uint8_t);
int sdgoods_audio_stream_write(const int16_t *,size_t);
void sdgoods_audio_stream_stop(void);

