/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "imx8-host.h"

void invalidate_region(void *region, uint32_t size) {
    uint32_t *ptr;
    uint32_t index;

    for (ptr = (uint32_t *)region, index = 0; index < size / CACHE_LINE_SIZE; ptr += 16, index++) {
        dccivac((uint32_t *)(ptr));
    }
    /* make sure the invalidates are complete before the caller reads */
    asm volatile("dsb ish" : : : "memory");
}

/* Invalidate and copy a DDR region in blocks: the invalidates for block
 * k+1 are issued while block k is copied, so the cache maintenance
 * overlaps the DDR reads instead of running as a separate pass. */
void invalidate_copy_region(void *dst, void *src, uint32_t size) {
    const uint32_t blk = 4096u;
    uint8_t *s = (uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    uint32_t off, n, l;

    n = (size < blk) ? size : blk;
    for (l = 0; l < n; l += CACHE_LINE_SIZE)
        dccivac(s + l);
    asm volatile("dsb ish" : : : "memory");

    for (off = 0; off < size; off += blk) {
        uint32_t cur  = (size - off < blk) ? (size - off) : blk;
        uint32_t next = off + blk;

        if (next < size) {
            uint32_t nn = (size - next < blk) ? (size - next) : blk;
            for (l = 0; l < nn; l += CACHE_LINE_SIZE)
                dccivac(s + next + l);
        }
        memcpy(d + off, s + off, cur);
        if (next < size)
            asm volatile("dsb ish" : : : "memory");
    }
}

void flush_region(void *region, uint32_t size) {
    uint32_t *ptr;
    uint32_t index;

    for (ptr = (uint32_t *)region, index = 0; index < size / CACHE_LINE_SIZE; ptr += 16, index++) {
        dcbf((uint32_t *)(ptr));
    }
}
