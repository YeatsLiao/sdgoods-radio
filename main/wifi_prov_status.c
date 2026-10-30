/*
 * 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程
 * 应用层示例
 * https://github.com/SDGOODS/SDGOODS-ESP32S3
 *
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * 「谷仓共创计划」与「谷仓 SDGOODS 开放平台」项目、谷仓次元屏（谷仓电子徽章）设备，
 *   以及本基础代码的著作权与相关权利，均归深圳希德创新网络有限公司所有。
 * SPDX-License-Identifier: Apache-2.0
 *
 * 本文件属于应用层，以 Apache-2.0 发布：可自由商用、可闭源分发，
 * 只需保留本声明并携带 NOTICE 文件。详见 LICENSING.md。
 */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Observe association AND an IPv4 lease; neither a cached event nor IP alone is enough.
 */
#include "wifi_prov_status.h"
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"

wifi_prov_state_t wifi_prov_observed_state(wifi_prov_state_t cached, bool pending,
                                           char *ip, size_t capacity)
{
    if(ip && capacity)snprintf(ip,capacity,"0.0.0.0");
    if(pending)return WIFI_STATE_CONNECTING;
    wifi_ap_record_t ap;
    esp_netif_ip_info_t info;
    esp_netif_t *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif && esp_wifi_sta_get_ap_info(&ap)==ESP_OK &&
       esp_netif_get_ip_info(netif,&info)==ESP_OK && info.ip.addr){
        if(ip && capacity)snprintf(ip,capacity,IPSTR,IP2STR(&info.ip));
        return WIFI_STATE_CONNECTED;
    }
    return cached==WIFI_STATE_CONNECTED?WIFI_STATE_CONNECTING:cached;
}
