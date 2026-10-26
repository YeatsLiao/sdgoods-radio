/*
 * sdgoods-radio · WiFi 配网模块（应用层，不修改平台组件）
 *
 * 方案（参考 ai-passport-bambu-monitor）：SoftAP + captive DNS + 手机网页配网 + NVS
 *   · 未配网（NVS 无 SSID）→ 开机进入配网模式：
 *       设备开热点 SDGOODS-RADIO-XXXX（无密码），手机连上后浏览器自动弹配置页
 *       （captive DNS 劫持 + 404→302），填 WiFi 名/密码（可扫描选择）提交 →
 *       保存 NVS → 自动切换 STA 连接。支持中文 SSID（POST + urldecode）。
 *   · 已配网 → 开机直接 STA 连接；连接失败若干次自动退回配网模式。
 *   · 预留：云端网关地址 host[:port] 一并存 NVS；电台应用暂不使用（拉流走电台目录自带的 URL）。
 *
 * ⚠️ 平台层 sdgoods_wifi_init 已完成基础 WiFi 初始化（STA 模式）；本模块在其之上
 *    用原生 ESP-IDF API 扩展 AP/HTTP/DNS/连接（不改 components/sdgoods_board）。
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_STATE_SETUP = 0,      /* 配网模式：AP 开放中，等待用户提交 */
    WIFI_STATE_CONNECTING,     /* STA 连接中 */
    WIFI_STATE_CONNECTED,      /* 已联网（已获得 IP） */
    WIFI_STATE_FAILED,         /* 连接失败（短暂状态，随后自动回 SETUP） */
} wifi_prov_state_t;

/* 启动配网/连接（幂等）：无配置进配网模式，有配置连 STA */
esp_err_t wifi_prov_start(void);

/* 清除已保存凭据并立即进入配网模式（长按电源键等重置入口用） */
bool wifi_prov_erase_credentials(void);

wifi_prov_state_t wifi_prov_get_state(void);
const char *wifi_prov_get_ip(void);
bool wifi_prov_is_setup_mode(void);

/* 当前配网热点名（含 MAC 后缀），供 UI 显示 */
void wifi_prov_get_ap_ssid(char *out, size_t outlen);

/* 已配置的云端网关地址 host[:port]；未配置返回空串（走固件默认） */
const char *wifi_prov_get_gateway(void);

/* ---- 机身内配网（不依赖手机热点）用：扫描 + 提交连接 ----
 * wifi_prov_scan：同步扫描周围网络，把去重后的 SSID 填进 ssids[i]（每项 33 字节），
 *   rssi[i] 给信号强度；返回实际条数（≤max）。需在 WiFi 已启动（STA/APSTA）时调用。
 * wifi_prov_submit：保存 ssid/pass 到 NVS 并切换 STA 连接（复用配网状态机，含重试）。
 *   连接结果通过 wifi_prov_get_state() 轮询（CONNECTED 即成功）。 */
int  wifi_prov_scan(char ssids[][33], int8_t rssi[], int max);
void wifi_prov_submit(const char *ssid, const char *pass);
