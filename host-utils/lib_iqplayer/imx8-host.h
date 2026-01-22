/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef __IMX8_HOST_H__
#define __IMX8_HOST_H__

#define CACHE_LINE_SIZE 64

// Data Cache Flush/Clean
#define dcbf(p) \
    { asm volatile("dc cvac, %0" : : "r"(p) : "memory"); }
// Clean and Invalidate data cache
#define dccivac(p) \
    { asm volatile("dc civac, %0" : : "r"(p) : "memory"); }

void invalidate_region(void *region, uint32_t size);
void flush_region(void *region, uint32_t size);

#endif
