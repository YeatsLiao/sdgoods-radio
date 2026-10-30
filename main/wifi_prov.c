/*
 * sdgoods-radio · WiFi 配网模块实现
 *
 * 方案（参考 ai-passport-bambu-monitor 的成熟配网，适配本平台）：
 *   1. SoftAP 热点 "SDGOODS-RADIO-XXXX"（开放网络，XXXX 取 MAC 后两字节避免撞名）
 *   2. captive DNS：劫持所有 A 查询到 192.168.4.1（手机连上后自动弹配网页）
 *   3. HTTP：GET / 配网页（含 /api/scan 扫描列表）+ POST /save 保存并连接
 *   4. NVS 保存 ssid/pass/gw；开机已配网则直接 STA 连接，失败若干次自动回配网模式
 *
 * ⚠️ 平台层 sdgoods_wifi_init 已完成 nvs/netif/event loop/wifi 基础初始化（STA 模式），
 *    本模块在其之上扩展 AP + HTTP + DNS + 连接逻辑，不改 components/sdgoods_board。
 * ⚠️ httpd 任务优先级低于 LVGL(5)，避免单核 CPU 争抢触发看门狗。
 */

#include "wifi_prov.h"
#include "wifi_prov_status.h"
#include <stdatomic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dns_server.h"

static const char *TAG = "wifi_prov";

#define NVS_NS         "radio"           /* NVS 命名空间 */
#define KEY_SSID       "ssid"
#define KEY_PASS       "pass"
#define KEY_GW         "gw"              /* 云端网关地址 host[:port]，可空走默认 */
#define AP_SSID_PREFIX "SDGOODS-RADIO-"
#define AP_MAX_CONN    4
#define STA_RETRY_MAX  6                 /* 失败重试次数，超出回配网模式 */

static _Atomic(wifi_prov_state_t) s_state = WIFI_STATE_SETUP;
static atomic_bool s_connect_pending;
static atomic_bool s_expected_disconnect;
static bool s_started;
static esp_timer_handle_t s_connect_timer, s_retry_timer;
static char s_ip[16] = "0.0.0.0";
static char s_ap_ssid[33] = AP_SSID_PREFIX;
static atomic_int s_retries;
static httpd_handle_t s_httpd = NULL;
static dns_server_handle_t s_dns = NULL;
static bool s_ap_netif_created = false;
static bool s_handlers_registered = false;

/* ---------------------------------------------------------------------------
 * NVS 读写
 * ------------------------------------------------------------------------- */
static bool nvs_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len,
                           char *gw, size_t gw_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l = ssid_len;
    esp_err_t e1 = nvs_get_str(h, KEY_SSID, ssid, &l);
    l = pass_len;
    esp_err_t e2 = nvs_get_str(h, KEY_PASS, pass, &l);
    if (gw && gw_len) {
        l = gw_len;
        if (nvs_get_str(h, KEY_GW, gw, &l) != ESP_OK) {
            gw[0] = '\0';
        }
    }
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0');
}

static esp_err_t nvs_save_creds(const char *ssid, const char *pass, const char *gw)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err=nvs_set_str(h, KEY_SSID, ssid);
    if(err==ESP_OK)err=nvs_set_str(h, KEY_PASS, pass);
    if(err==ESP_OK)err=nvs_set_str(h, KEY_GW, gw ? gw : "");
    if(err==ESP_OK)err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------------------------------------------------------------------------
 * 表单 / URL 解码辅助（参考 bambu：POST body，'+'→空格，%XX 解码，支持中文）
 * ------------------------------------------------------------------------- */
static bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *end = strchr(p, '&');
            size_t raw = end ? (size_t)(end - p) : strlen(p);
            size_t o = 0;
            for (size_t i = 0; i < raw && o + 1 < outlen; i++) {
                if (p[i] == '+') {
                    out[o++] = ' ';
                } else if (p[i] == '%' && i + 2 < raw) {
                    char hex[3] = { p[i + 1], p[i + 2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    out[o++] = p[i];
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = '\0';
    return false;
}

static void html_escape(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < outlen; p++) {
        if (*p == '&')      o += snprintf(out + o, outlen - o, "&amp;");
        else if (*p == '"') o += snprintf(out + o, outlen - o, "&quot;");
        else if (*p == '<') o += snprintf(out + o, outlen - o, "&lt;");
        else if (*p == '>') o += snprintf(out + o, outlen - o, "&gt;");
        else out[o++] = *p;
    }
    out[o] = '\0';
}

/* ---------------------------------------------------------------------------
 * HTTP 配网页
 * ------------------------------------------------------------------------- */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>谷仓电台 · 配网</title>"
    "<style>:root{--bg:#1a1a2e;--card:#23233f;--ink:#ffd1a6;--sub:#8fa0c0;"
    "--line:#3a3a5c;--accent:#5a5aa0}"
    "*{box-sizing:border-box;margin:0}"
    "body{font-family:-apple-system,'Segoe UI',system-ui,sans-serif;background:var(--bg);"
    "color:var(--ink);min-height:100vh;padding:28px 16px}"
    ".card{max-width:430px;margin:0 auto;background:var(--card);border:1px solid var(--line);"
    "border-radius:20px;padding:26px 22px}"
    "h1{font-size:20px;margin-bottom:6px}"
    ".tip{font-size:13px;color:var(--sub);line-height:1.6;margin-bottom:18px}"
    "label{display:block;font-size:13px;margin:14px 0 6px;font-weight:600}"
    "input,select{width:100%;padding:12px 14px;border:1.5px solid var(--line);border-radius:12px;"
    "font-size:16px;background:#181830;color:var(--ink)}"
    ".row{display:flex;gap:8px}"
    ".scanbtn{flex-shrink:0;width:auto;padding:0 16px;background:var(--accent);color:#fff;"
    "border:0;border-radius:12px;font-size:15px;font-weight:600}"
    "button.submit{width:100%;margin-top:22px;padding:14px;border:0;border-radius:14px;"
    "background:var(--accent);color:#fff;font-size:16px;font-weight:650}"
    ".foot{max-width:430px;margin:16px auto 0;text-align:center;font-size:12.5px;color:var(--sub);line-height:1.6}"
    "</style></head><body><div class=\"card\">";

static const char PAGE_FOOT[] =
    "</div><div class=\"foot\">配置仅保存在设备本地，不会上传任何服务器<br>"
    "此热点无互联网属正常现象，直接填写表单即可</div>"
    "<script>"
    "function setScan(t){var b=document.getElementById('scanbtn');b.textContent=t;b.disabled=(t!=='扫描');}"
    "async function scanWifi(){setScan('扫描中…');"
    "try{var r=await fetch('/api/scan');var ns=await r.json();"
    "var s=document.getElementById('netsel');"
    "s.innerHTML='<option value=\"\">— 选择网络 —</option>';"
    "ns.forEach(function(n){var o=document.createElement('option');o.value=n.s;"
    "o.textContent=n.s+'  '+n.r+'dBm'+(n.e?' 加密':'');s.appendChild(o);});"
    "s.style.display='block';}catch(e){alert('扫描失败，请手动输入 WiFi 名称');}"
    "setScan('扫描');}"
    "function pickNet(){var v=document.getElementById('netsel').value;if(v)document.getElementById('ssid').value=v;}"
    "</script></body></html>";

static char s_page[4096];

static esp_err_t handle_index(httpd_req_t *req)
{
    char esc_ssid[96] = {0};
    char cur_ssid[33] = {0}, cur_pass[65] = {0}, cur_gw[64] = {0};
    bool have = nvs_load_creds(cur_ssid, sizeof(cur_ssid), cur_pass, sizeof(cur_pass),
                               cur_gw, sizeof(cur_gw));
    html_escape(have ? cur_ssid : "", esc_ssid, sizeof(esc_ssid));

    int off = snprintf(s_page, sizeof(s_page), "%s", PAGE_HEAD);
    off += snprintf(s_page + off, sizeof(s_page) - off,
        "<h1>谷仓电台 配网</h1>"
        "<p class=\"tip\">连接热点 <b>%s</b> 后，在此填写家里 WiFi，保存后设备自动连接。</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<label>WiFi 名称</label>"
        "<div class=\"row\">"
        "<input id=\"ssid\" name=\"ssid\" required maxlength=\"32\" value=\"%s\" "
        "placeholder=\"输入或点击扫描\" autocomplete=\"off\" style=\"flex:1\">"
        "<button type=\"button\" class=\"scanbtn\" id=\"scanbtn\" onclick=\"scanWifi()\">扫描</button>"
        "</div>"
        "<select id=\"netsel\" onchange=\"pickNet()\" style=\"display:none;margin-top:8px\"></select>"
        "<label>WiFi 密码%s</label>"
        "<input name=\"pass\" type=\"password\" maxlength=\"64\" placeholder=\"%s\">"
        "<button type=\"submit\" class=\"submit\">保存并连接</button>"
        "</form>",
        s_ap_ssid, esc_ssid,
        (have && cur_pass[0]) ? "（已配置，不改留空）" : "",
        (have && cur_pass[0]) ? "不改请留空" : "必填");
    snprintf(s_page + off, sizeof(s_page) - off, "%s", PAGE_FOOT);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

static const char PAGE_OK[] =
    "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
    "<title>已保存</title></head><body style=\"background:#1a1a2e;color:#7ef0a0;"
    "font-family:sans-serif;max-width:420px;margin:48px auto;padding:0 16px\">"
    "<h2>配置已保存，正在连接 WiFi…</h2>"
    "<p style=\"color:#8fa0c0\">可以关闭本页。连接失败时，请在设备设置中重新配网。</p>"
    "</body></html>";

/* 前向声明 */
static esp_err_t schedule_connect_sta(const char *ssid, const char *pass, const char *gw);

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[512] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    int received=0;
    while(received<len){
        int r=httpd_req_recv(req,body+received,(size_t)(len-received));
        if(r<=0){
            httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"incomplete request body");
            return ESP_OK;
        }
        received+=r;
    }
    body[received]='\0';

    char ssid[33] = {0}, pass[65] = {0}, gw[64] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "gw",   gw,   sizeof(gw));

    /* A blank password only reuses credentials for the same SSID. */
    if (pass[0] == '\0') {
        char old_ssid[33] = {0}, old_pass[65] = {0}, old_gw[64] = {0};
        if (nvs_load_creds(old_ssid, sizeof(old_ssid), old_pass, sizeof(old_pass),
                           old_gw, sizeof(old_gw)) && strcmp(ssid,old_ssid)==0) {
            strncpy(pass, old_pass, sizeof(pass) - 1);
        }
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid empty");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "save: ssid=%s pass_len=%d gw=%s", ssid, (int)strlen(pass), gw);

    if (atomic_load(&s_connect_pending) || nvs_save_creds(ssid, pass, gw) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_OK;
    }

    if(schedule_connect_sta(ssid,pass,gw)!=ESP_OK){
        httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"connection could not be scheduled");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_OK, HTTPD_RESP_USE_STRLEN);
    /* 延迟切换 STA：不能在 httpd handler 内 stop 自己（会崩），1.5s 后由定时器执行 */
    return ESP_OK;
}

/* WiFi 扫描（GET /api/scan）：配网模式下同步扫描，返回 JSON 供前端下拉 */
static esp_err_t api_scan(httpd_req_t *req)
{
    static wifi_ap_record_t recs[12];
    static char json[1024];

    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    uint16_t n = 12;
    esp_wifi_scan_get_ap_records(&n, recs);

    size_t pos = 0;
    json[pos++] = '[';
    for (int i = 0; i < (int)n && pos < sizeof(json) - 80; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        if (strncmp((const char *)recs[i].ssid, AP_SSID_PREFIX, sizeof(AP_SSID_PREFIX) - 1) == 0) continue;
        if (pos > 1) json[pos++] = ',';
        char esc[40] = {0};
        size_t ei = 0;
        for (const uint8_t *p = recs[i].ssid; *p && ei < 38; p++) {
            if (*p < 0x20) continue;
            if (*p == '"' || *p == '\\') esc[ei++] = '\\';
            esc[ei++] = (char)*p;
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "{\"s\":\"%s\",\"r\":%d,\"e\":%s}",
                        esc, (int)recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN ? "true" : "false");
    }
    json[pos++] = ']';
    json[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 404 → 302 重定向到配页（iOS/Android captive portal 识别；响应体须有内容） */
static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_httpd) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.task_priority = 4;         /* 低于 LVGL(5)，避免单核抢 CPU 触发看门狗 */
    cfg.stack_size = 8192;         /* 容纳 index handler 的页面缓冲 */
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t u_index = { .uri = "/",         .method = HTTP_GET,  .handler = handle_index };
    httpd_uri_t u_save  = { .uri = "/save",     .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t u_scan  = { .uri = "/api/scan", .method = HTTP_GET,  .handler = api_scan };
    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_scan);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_handler);

    /* captive DNS：所有 A 查询 → SoftAP IP（192.168.4.1） */
    if (!s_dns) {
        dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&dns_cfg);
    }
    ESP_LOGI(TAG, "http server up: http://192.168.4.1/  (ap: %s)", s_ap_ssid);
    return ESP_OK;
}

static void stop_http_server(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * WiFi 模式切换
 * ------------------------------------------------------------------------- */
static void build_ap_config(wifi_config_t *ap_cfg)
{
    memset(ap_cfg, 0, sizeof(*ap_cfg));
    strncpy((char *)ap_cfg->ap.ssid, s_ap_ssid, sizeof(ap_cfg->ap.ssid) - 1);
    ap_cfg->ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg->ap.channel = 6;
    ap_cfg->ap.max_connection = AP_MAX_CONN;
    ap_cfg->ap.authmode = WIFI_AUTH_OPEN;
}

/* 配网模式：SoftAP(APSTA) 开放 + HTTP + captive DNS（APSTA 让 STA 接口可扫描） */
static void enter_setup_mode(void)
{
    s_state=WIFI_STATE_SETUP;
    if(s_retry_timer)esp_timer_stop(s_retry_timer);
    ESP_LOGI(TAG, "enter setup mode (AP: %s)", s_ap_ssid);
    stop_http_server();               /* 幂等 */
    esp_wifi_stop();

    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }
    wifi_config_t ap_cfg;
    build_ap_config(&ap_cfg);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    start_http_server();
    s_retries = 0;
    s_state = WIFI_STATE_SETUP;
}

/* STA 连接（开机已配 / 配网保存后） */
static void connect_sta(const char *ssid, const char *pass, const char *gw)
{
    ESP_LOGI(TAG, "connecting STA: %s (gw=%s)", ssid, gw && gw[0] ? gw : "default");
    stop_http_server();
    s_state=WIFI_STATE_CONNECTING;
    atomic_store(&s_expected_disconnect,true);
    if(s_retry_timer)esp_timer_stop(s_retry_timer);
    {
        /* 从配网模式切回：需要 stop/start，且此时可能是 APSTA 模式 */
        esp_wifi_stop();
    }
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = strlen(pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    esp_err_t err=esp_wifi_set_mode(WIFI_MODE_STA);
    if(err==ESP_OK)err=esp_wifi_set_config(WIFI_IF_STA,&sta_cfg);
    if(err==ESP_OK)err=esp_wifi_start();
    s_retries = 0;
    atomic_store(&s_connect_pending,false);
    if(err==ESP_OK)err=esp_wifi_connect();
    if(err!=ESP_OK){s_state=WIFI_STATE_FAILED;ESP_LOGW(TAG,"connect request failed: %s",esp_err_to_name(err));}
}

/* 保存的凭据快照 + 延迟连接（定时器回调执行，避开 httpd handler 内切换） */
static char s_pending_ssid[33];
static char s_pending_pass[65];
static char s_pending_gw[64];

static void prov_timer_cb(void *arg)
{
    (void)arg;
    connect_sta(s_pending_ssid, s_pending_pass, s_pending_gw);
}

static esp_err_t schedule_connect_sta(const char *ssid, const char *pass, const char *gw)
{
    if(atomic_exchange(&s_connect_pending,true))return ESP_ERR_INVALID_STATE;
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    strncpy(s_pending_pass, pass, sizeof(s_pending_pass) - 1);
    strncpy(s_pending_gw, gw ? gw : "", sizeof(s_pending_gw) - 1);
    esp_timer_create_args_t args = {
        .callback = prov_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "prov_connect",
    };
    s_state=WIFI_STATE_CONNECTING;
    esp_err_t err=ESP_OK;
    if(!s_connect_timer)err=esp_timer_create(&args,&s_connect_timer);
    if(err==ESP_OK)err=esp_timer_start_once(s_connect_timer,1500*1000);
    if(err!=ESP_OK){atomic_store(&s_connect_pending,false);s_state=WIFI_STATE_FAILED;}
    return err;
}

/* ---------------------------------------------------------------------------
 * 机身内配网公开接口（供电台 UI 直接调用，不走手机热点）
 * ------------------------------------------------------------------------- */
int wifi_prov_scan(char ssids[][33], int8_t rssi[], int max)
{
    static wifi_ap_record_t recs[16];
    wifi_scan_config_t sc = { .show_hidden = false };
    /* block=true 同步扫描；STA/APSTA 模式下都可用。约 2~4s，调用方需避开 LVGL 线程或用短任务。 */
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = 16;
    esp_wifi_scan_get_ap_records(&n, recs);
    int cnt = 0;
    for (int i = 0; i < (int)n && cnt < max; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        if (strncmp((const char *)recs[i].ssid, AP_SSID_PREFIX, sizeof(AP_SSID_PREFIX) - 1) == 0) continue;
        bool dup = false;
        for (int j = 0; j < cnt; j++) {
            if (strncmp(ssids[j], (const char *)recs[i].ssid, 32) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(ssids[cnt], (const char *)recs[i].ssid, 32);
        ssids[cnt][32] = '\0';
        if (rssi) rssi[cnt] = recs[i].rssi;
        cnt++;
    }
    return cnt;
}

esp_err_t wifi_prov_submit(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid)>32 || (pass && strlen(pass)>64)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(atomic_load(&s_connect_pending))return ESP_ERR_INVALID_STATE;
    esp_err_t err=nvs_save_creds(ssid,pass?pass:"","");
    return err==ESP_OK?schedule_connect_sta(ssid,pass?pass:"",""):err;
}

/* ---------------------------------------------------------------------------
 * 事件处理
 * ------------------------------------------------------------------------- */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    if(s_state!=WIFI_STATE_SETUP && !atomic_load(&s_connect_pending)){
        s_state=WIFI_STATE_CONNECTING;
        esp_err_t err=esp_wifi_connect();
        if(err!=ESP_OK){
            s_state=WIFI_STATE_FAILED;
            if(s_retry_timer)esp_timer_start_once(s_retry_timer,5000*1000);
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_START) {
        /* Connection requests have a single owner (connect/retry timer). */
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event=data;
        if(event && event->reason==WIFI_REASON_ASSOC_LEAVE && atomic_exchange(&s_expected_disconnect,false))return;
        if (s_state != WIFI_STATE_SETUP && !atomic_load(&s_connect_pending)) {
            if(s_retries<STA_RETRY_MAX)++s_retries;
            s_state=s_retries<STA_RETRY_MAX?WIFI_STATE_CONNECTING:WIFI_STATE_FAILED;
            unsigned delay=s_retries<STA_RETRY_MAX?500:5000;
            ESP_LOGW(TAG,"link lost, reconnect in %ums",delay);
            if(s_retry_timer){esp_timer_stop(s_retry_timer);esp_timer_start_once(s_retry_timer,delay*1000u);}
        }
    } else if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP started (192.168.4.1)");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_state = WIFI_STATE_CONNECTED;
        atomic_store(&s_expected_disconnect,false);
        if(s_retry_timer)esp_timer_stop(s_retry_timer);
        /* 关闭 WiFi 省电：默认 WIFI_PS_MIN_MODEM 会让射频在信标间隔间休眠，
         * 收音频流时产生周期性 ~100-200ms 延迟尖峰 → 声音一卡一卡。电台靠持续下行，关省电。 */
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "connected, ip=%s (wifi ps-mode off)", s_ip);
    }
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */
esp_err_t wifi_prov_start(void)
{
    if(s_started)return ESP_OK;
    esp_timer_create_args_t retry={.callback=retry_timer_cb,.name="wifi_retry"};
    esp_err_t timer_err=esp_timer_create(&retry,&s_retry_timer);
    if(timer_err!=ESP_OK)return timer_err;
    if (!s_handlers_registered) {
        esp_event_handler_instance_t h1, h2;
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, NULL, &h1);
        esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                            on_ip_event, NULL, &h2);
        s_handlers_registered = true;
    }

    /* 热点名带 MAC 后缀，避免多台设备撞名 */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    s_started=true;
    if(wifi_prov_observed_state(WIFI_STATE_CONNECTING,false,s_ip,sizeof(s_ip))==WIFI_STATE_CONNECTED){
        s_state=WIFI_STATE_CONNECTED;
        esp_wifi_set_ps(WIFI_PS_NONE);
        return ESP_OK;
    }
    char ssid[33] = {0}, pass[65] = {0}, gw[64] = {0};
    if (nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass), gw, sizeof(gw))) {
        connect_sta(ssid, pass, gw);
    } else {
        enter_setup_mode();
    }
    return ESP_OK;
}

bool wifi_prov_erase_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, KEY_SSID);
        nvs_erase_key(h, KEY_PASS);
        nvs_erase_key(h, KEY_GW);
        nvs_commit(h);
        nvs_close(h);
    }
    enter_setup_mode();
    return true;
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    wifi_prov_state_t state=wifi_prov_observed_state(s_state,atomic_load(&s_connect_pending),NULL,0);
    if(state==WIFI_STATE_CONNECTED)s_state=state;
    return state;
}
const char *wifi_prov_get_ip(void)
{
    wifi_prov_observed_state(s_state,atomic_load(&s_connect_pending),s_ip,sizeof(s_ip));
    return s_ip;
}
bool wifi_prov_is_setup_mode(void)          { return s_state == WIFI_STATE_SETUP; }

void wifi_prov_get_ap_ssid(char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    strncpy(out, s_ap_ssid, outlen - 1);
    out[outlen - 1] = '\0';
}

const char *wifi_prov_get_gateway(void)
{
    static char gw[64] = {0};
    char ssid[33] = {0}, pass[65] = {0};
    if (nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass), gw, sizeof(gw)) && gw[0]) {
        return gw;
    }
    return NULL;   /* 无自定义网关，由 cloud_client 使用固件默认 */
}
