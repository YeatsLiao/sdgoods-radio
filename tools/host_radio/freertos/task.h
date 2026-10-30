/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 */
#pragma once
#include "FreeRTOS.h"
#include <string.h>
extern int radio_host_hold_catalog;
extern TaskFunction_t radio_host_pending_task;
extern void *radio_host_pending_arg;
static inline int xTaskCreateWithCaps(TaskFunction_t f, const char *n, uint32_t s,
                                    void *a, UBaseType_t p, void *h, unsigned c)
{ (void)s; (void)p; (void)h; (void)c;
  if(radio_host_hold_catalog && !strcmp(n,"radio_cat")){radio_host_pending_task=f;radio_host_pending_arg=a;}
  else f(a); return pdPASS; }
static inline void vTaskDeleteWithCaps(void *p) { (void)p; }
