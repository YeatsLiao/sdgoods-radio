/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 */
#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 0; }
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
