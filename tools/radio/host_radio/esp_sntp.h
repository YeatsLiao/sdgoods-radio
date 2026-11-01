/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 */
#pragma once
#include <time.h>
#define ESP_SNTP_OPMODE_POLL 0
static inline void esp_sntp_setoperatingmode(int mode) { (void)mode; }
static inline void esp_sntp_setservername(int slot, const char *name) { (void)slot; (void)name; }
static inline void esp_sntp_init(void) { }
static inline int radio_host_setenv(const char *k, const char *v, int overwrite) { (void)k; (void)v; (void)overwrite; return 0; }
static inline struct tm *radio_host_localtime_r(const time_t *t, struct tm *out) { *out = *localtime(t); return out; }
#define setenv radio_host_setenv
#define localtime_r radio_host_localtime_r
#define tzset _tzset
