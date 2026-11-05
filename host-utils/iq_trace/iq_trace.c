/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024-2025 NXP
 *
 * iq_trace - Dump VSPA l1_trace ring buffer from LA9310 IQ player.
 *
 * DMEM offsets for the trace symbols are extracted at runtime from the
 * ELD deployed in /lib/firmware/apm.eld (or specified with -e).
 * This means a single binary works for any firmware variant (1T1R, 0T1R,
 * 1T0R, ...) without a rebuild.
 *
 * Symbol extraction uses readelf (always available on Linux target).
 * If VSPA_TOOL is set in the environment, vspa-elfdump is tried first.
 *
 * VSPA2 DMEM host address mapping (from vspa_symbols_extract.sh):
 *   VSPA byte addr < 0x4000  -> host = BAR2_addr + 0x400000 + vspa_addr (TCML)
 *   VSPA byte addr >= 0x4000 -> host = BAR2_addr + 0x500000 + vspa_addr (TCMU)
 *
 * Usage:
 *   iq_trace [-e <eld>] [-p] [-r] [-R] [-v] [-h]
 *
 *   -e E   ELD file for symbol extraction.
 *          Default: /lib/firmware/apm.eld
 *   -p     Pause tracing during snapshot (writes l1_trace_disable=1, clears after).
 *   -r     (no-op, kept for compatibility) Re-arm is now the default.
 *   -R     Disable automatic re-arm after dump (do NOT clear l1_trace_disable).
 *   -v     Print version string and exit.
 *
 * Re-arm behaviour:
 *   By default iq_trace clears l1_trace_disable=0 after every dump so the
 *   VSPA resumes writing trace entries immediately.  Use -R to suppress this
 *   if you need tracing to stay paused after a snapshot.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <la9310_modinfo.h>
#include <la9310_host_if.h>
/* LA9310_DEV_NAME_PREFIX is defined in the NXP-tree la9310_host_if.h.
 * On RFNM builds neither header tree defines it; supply the RFNM driver
 * device name prefix so host-utils can open /dev/shiva<n>. */
#ifndef LA9310_DEV_NAME_PREFIX
#define LA9310_DEV_NAME_PREFIX  "shiva"
#endif
#include "l1-trace.h"
#include "imx8-host.h"
#include "vspa_trace_enum.h"

/* Default ELD path -- override with -e */
#define DEFAULT_ELD_PATH "/lib/firmware/apm.eld"

/*
 * VSPA2 DMEM host address offsets from BAR2 base (mi.tcmu.host_phy_addr).
 * Derived from vspa_symbols_extract.sh:
 *   addr < 0x4000  (TCML / VCPU DMEM bank 0) -> BAR2 + 0x400000 + addr
 *   addr >= 0x4000 (TCMU / VCPU DMEM bank 1) -> BAR2 + 0x500000 + addr
 */
#define VSPA2_TCML_BAR2_OFFSET 0x400000UL
#define VSPA2_TCMU_BAR2_OFFSET 0x500000UL
#define VSPA2_TCML_THRESH      0x4000UL
#define VSPA2_MAP_SIZE         0x200000UL /* 2 MB covers both banks */

uint32_t *v_ocram_addr;
uint32_t *BAR0_addr;
uint32_t *BAR1_addr;
uint32_t *BAR2_addr;
modinfo_t mi;

/* --------------------------------------------------------------------------
 * Trace param decoder (ported from LA1224 iq_trace_la1224.c)
 * -------------------------------------------------------------------------- */
static const char *vspa_trace_param_info(uint32_t msg, uint32_t param) {
    static char buf[64];

    switch (msg) {
    case 0xD05: {
        /* BUILD_VER: param = MMDDHHmm as a plain decimal integer.
         * ELD_BUILD_VERSION is set by: $(shell date +%-m%d%H%M)
         * e.g. Oct 29 16:00 -> param=10291600 (0x009D0990).
         * Decode with decimal arithmetic, not byte-field shifts. */
        unsigned int mn =  param % 100u;
        unsigned int hh = (param / 100u) % 100u;
        unsigned int dd = (param / 10000u) % 100u;
        unsigned int mm =  param / 1000000u;
        snprintf(buf, sizeof(buf), "built %02u/%02u %02u:%02u", mm, dd, hh, mn);
        return buf;
    }
    case 0xC01: {
        /* ENTRY_MAIN: param = CONTROL register at boot.
         * Bit 21 = MSG1_GO, bit 20 = MSG0_GO, bit 2 = DMA_GO */
        if (param == 0) {
            snprintf(buf, sizeof(buf), "BOOT");
        } else {
            int n = 0;
            buf[0] = '\0';
            if (param & (1u << 21))
                n += snprintf(buf + n, sizeof(buf) - n, "%sMSG1", n ? "|" : "");
            if (param & (1u << 20))
                n += snprintf(buf + n, sizeof(buf) - n, "%sMSG0", n ? "|" : "");
            if (param & (1u << 2))
                snprintf(buf + n, sizeof(buf) - n, "%sDMA", n ? "|" : "");
        }
        return buf;
    }
    case 0xC09: {
        /* ENTRY_MAIN_DMASTAT: param = DMA_GO_STAT bitmask on wakeup */
        int n = 0, ch;
        buf[0] = '\0';
        for (ch = 0; ch < 32; ch++) {
            if (param & (1u << ch))
                n += snprintf(buf + n, sizeof(buf) - n, "%s%d", n ? "," : "ch=", ch);
        }
        if (!n)
            snprintf(buf, sizeof(buf), "no-dma-ch");
        return buf;
    }
    case 0xD06:
    case 0xD07:
        snprintf(buf, sizeof(buf), "cmd_msb=0x%08x", param);
        return buf;
    case 0xD08:
        snprintf(buf, sizeof(buf), "opc=0x%02x", (param >> 24) & 0xFFu);
        return buf;
    case 0xD09:
        snprintf(buf, sizeof(buf), "lsb=0x%08x", param);
        return buf;
    case 0xD0A:
        snprintf(buf, sizeof(buf), "ack_msb=0x%08x", param);
        return buf;
    case 0xD0B:
        snprintf(buf, sizeof(buf), "unknown_opc=0x%02x", (param >> 24) & 0xFFu);
        return buf;
    case 0xD0C:
        snprintf(buf, sizeof(buf), "opc=0x%02x", (param >> 24) & 0xFFu);
        return buf;
    case 0xD0D:
        snprintf(buf, sizeof(buf), "lsb=0x%08x", param);
        return buf;
    case 0xD0F:
        snprintf(buf, sizeof(buf), "bad_opc=0x%02x", (param >> 24) & 0xFFu);
        return buf;
    default:
        return "";
    }
}

/* --------------------------------------------------------------------------
 * ELD symbol extraction
 * -------------------------------------------------------------------------- */

/*
 * eld_get_trace_offsets - extract l1_trace symbol VSPA addresses from an ELD.
 *
 * Tries vspa-elfdump ($VSPA_TOOL/bin/vspa-elfdump) first (output gives the
 * raw VSPA address directly).  Falls back to "readelf -sW" which is always
 * available on the Linux target; readelf st_value for VSPA2 ELDs is the
 * absolute VSPA byte address.
 *
 * Returns 0 on success, -1 on failure.
 * On success fills: *data_addr, *idx_addr, *dis_addr, *data_size (bytes).
 */
static int eld_get_trace_offsets(const char *eld_path,
                                 uint32_t *data_addr, uint32_t *idx_addr,
                                 uint32_t *dis_addr, uint32_t *data_size)
{
    char cmd[640];
    FILE *fp;
    char line[512];
    int found_data = 0, found_idx = 0, found_dis = 0;

    /* --- Pass 1: try vspa-elfdump -y (VSPA toolchain) --- */
    {
        const char *vspa_tool = getenv("VSPA_TOOL");
        char dump_path[512];
        const char *dump_bin = NULL;

        if (vspa_tool) {
            snprintf(dump_path, sizeof(dump_path), "%s/bin/vspa-elfdump", vspa_tool);
            if (access(dump_path, X_OK) == 0)
                dump_bin = dump_path;
        }
        if (!dump_bin && access("/opt/VSPA_Tools_vbeta_14_00_781/bin/vspa-elfdump", X_OK) == 0)
            dump_bin = "/opt/VSPA_Tools_vbeta_14_00_781/bin/vspa-elfdump";

        if (dump_bin) {
            snprintf(cmd, sizeof(cmd), "%s -y '%s' 2>/dev/null", dump_bin, eld_path);
            fp = popen(cmd, "r");
            if (fp) {
                /* vspa-elfdump -y format:
                 *   0xADDR  0xSIZE  SCOPE  TYPE  CLASS  SEC  _symbol */
                while (fgets(line, sizeof(line), fp)) {
                    unsigned long val, sz;
                    char scope[16], type[16], cls[16];
                    char ndx[8], sym[128];
                    if (sscanf(line, " %lx %lx %15s %15s %15s %7s %127s",
                               &val, &sz, scope, type, cls, ndx, sym) == 7) {
                        if (strcmp(scope, "GLOBAL") != 0 || strcmp(type, "OBJECT") != 0)
                            continue;
                        if (strcmp(sym, "_l1_trace_data") == 0) {
                            *data_addr = (uint32_t)val;
                            *data_size = (uint32_t)sz;
                            found_data = 1;
                        } else if (strcmp(sym, "_l1_trace_index") == 0) {
                            *idx_addr = (uint32_t)val;
                            found_idx = 1;
                        } else if (strcmp(sym, "_l1_trace_disable") == 0) {
                            *dis_addr = (uint32_t)val;
                            found_dis = 1;
                        }
                    }
                }
                pclose(fp);
                if (found_data && found_idx && found_dis)
                    return 0;
                found_data = found_idx = found_dis = 0;
            }
        }
    }

    /* --- Pass 2: readelf -sW (always available on Linux target) ---
     * For VSPA2 ELDs the st_value field is the absolute VSPA byte address.
     * The host BAR2 mapping is: vspa_addr < 0x4000 -> BAR2+0x400000+addr
     *                            vspa_addr >= 0x4000 -> BAR2+0x500000+addr
     */
    snprintf(cmd, sizeof(cmd), "readelf -sW '%s' 2>/dev/null", eld_path);
    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "iq_trace: cannot run readelf on %s\n", eld_path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        unsigned long num, val, sz;
        char type[16], bind[16], vis[16], ndx_str[16], sym[128];
        if (sscanf(line, " %lu: %lx %lu %15s %15s %15s %15s %127s",
                   &num, &val, &sz, type, bind, vis, ndx_str, sym) != 8)
            continue;
        if (strcmp(type, "OBJECT") != 0)
            continue;

        if (strcmp(sym, "_l1_trace_data") == 0) {
            *data_addr = (uint32_t)val;
            *data_size = (uint32_t)sz;
            found_data = 1;
        } else if (strcmp(sym, "_l1_trace_index") == 0) {
            *idx_addr = (uint32_t)val;
            found_idx = 1;
        } else if (strcmp(sym, "_l1_trace_disable") == 0) {
            *dis_addr = (uint32_t)val;
            found_dis = 1;
        }
    }
    pclose(fp);

    if (!found_data || !found_idx || !found_dis) {
        fprintf(stderr,
                "iq_trace: missing trace symbols in %s"
                " (data=%d idx=%d dis=%d)\n",
                eld_path, found_data, found_idx, found_dis);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * DMEM access helper
 * -------------------------------------------------------------------------- */

/*
 * vspa_dmem_ptr - convert a VSPA2 byte address to a host pointer via BAR2.
 *
 * BAR2_addr is already mapped to mi.tcmu.host_phy_addr (the larger of the
 * two VSPA DMEM windows).  The per-symbol host pointer is computed as:
 *   vspa_addr < 0x4000  -> BAR2_addr + 0x400000 + vspa_addr  (TCML bank)
 *   vspa_addr >= 0x4000 -> BAR2_addr + 0x500000 + vspa_addr  (TCMU bank)
 */
static inline volatile uint32_t *vspa_dmem_ptr(uint32_t vspa_addr)
{
    uint32_t bar2_off = (vspa_addr < VSPA2_TCML_THRESH)
                        ? (uint32_t)VSPA2_TCML_BAR2_OFFSET + vspa_addr
                        : (uint32_t)VSPA2_TCMU_BAR2_OFFSET + vspa_addr;
    return (volatile uint32_t *)((uint8_t *)BAR2_addr + bar2_off);
}

/* --------------------------------------------------------------------------
 * Trace ring dump
 * -------------------------------------------------------------------------- */

static void print_vspa_trace(const char *eld_path, int pause_tracing, int rearm_tracing)
{
    uint32_t data_addr = 0, idx_addr = 0, dis_addr = 0, data_size = 0;

    if (eld_get_trace_offsets(eld_path, &data_addr, &idx_addr, &dis_addr, &data_size) < 0) {
        fprintf(stderr, "iq_trace: cannot extract trace symbols from %s\n", eld_path);
        printf("\n VSPA trace: symbol extraction failed\n");
        return;
    }

    uint32_t TRACE_SIZE = data_size / sizeof(l1_trace_data_t);
    fprintf(stderr, "iq_trace: ELD=%s  data=0x%x idx=0x%x dis=0x%x entries=%u\n",
            eld_path, data_addr, idx_addr, dis_addr, TRACE_SIZE);

    volatile uint32_t *v_dis  = vspa_dmem_ptr(dis_addr);
    volatile uint32_t *v_idx  = vspa_dmem_ptr(idx_addr);
    volatile uint32_t *v_data = vspa_dmem_ptr(data_addr);

    /* Diagnostic: print the host mapping info for l1_trace_disable so any
     * out-of-range access (mmap window too small) is immediately visible. */
    {
        uint32_t bar2_off_dis = (dis_addr < VSPA2_TCML_THRESH)
                                ? (uint32_t)VSPA2_TCML_BAR2_OFFSET + dis_addr
                                : (uint32_t)VSPA2_TCMU_BAR2_OFFSET + dis_addr;
        const char *warn = (bar2_off_dis >= mi.tcmu.size) ? "  <-- WARNING: outside mmap window!" : "";
        fprintf(stderr,
                "iq_trace: l1_trace_disable: vspa=0x%04x  bar2_off=0x%06x"
                "  mmap_size=0x%06x  host_ptr=%p%s\n",
                dis_addr, bar2_off_dis, (uint32_t)mi.tcmu.size,
                (void *)v_dis, warn);
    }

    if (pause_tracing) {
        *v_dis = 1;
        __sync_synchronize();
    }

    uint32_t snap_idx = *v_idx;

    /* snapshot the ring into a local buffer */
    l1_trace_data_t *snap = calloc(data_size, 1);
    if (!snap) {
        fprintf(stderr, "iq_trace: out of memory\n");
        return;
    }
    {
        uint32_t *dst = (uint32_t *)snap;
        volatile uint32_t *src = v_data;
        size_t nwords = data_size / sizeof(uint32_t);
        size_t w;
        for (w = 0; w < nwords; w++)
            dst[w] = src[w];
    }

    if (pause_tracing) {
        __sync_synchronize();
        *v_dis = 0;
    }

    printf("=== VSPA trace (write_head=%u) ===\n", snap_idx);

    uint32_t i, j, k;
    for (i = 0; i < TRACE_SIZE; i++) {
        j = (snap_idx + 1 + i) % TRACE_SIZE;
        if (snap[j].cnt == 0 && snap[j].msg == 0)
            continue;
        k = 0;
        while (l1_trace_code[k].msg != 0xffff) {
            if (l1_trace_code[k].msg == snap[j].msg)
                break;
            k++;
        }
        {
            const char *info = vspa_trace_param_info(snap[j].msg, snap[j].param);
            printf("  [%3u] cnt=%20" PRIu64 "  msg=0x%03x %-29s  param=0x%08x%s%s\n",
                   j, snap[j].cnt, snap[j].msg, l1_trace_code[k].text,
                   snap[j].param, info[0] ? "  " : "", info);
        }
    }
    printf("\n");

    free(snap);

    if (rearm_tracing && !pause_tracing) {
        __sync_synchronize();
        *v_dis = 0;
        __sync_synchronize();
        fprintf(stderr, "iq_trace: l1_trace_disable cleared (rearmed)\n");
    }
}

/* --------------------------------------------------------------------------
 * M7 trace (unchanged)
 * -------------------------------------------------------------------------- */
extern uint32_t *v_ocram_addr;
void print_m7_trace(void) {
    uint32_t i, k;
    uint32_t TRACE_SIZE = L1_TRACE_M7_SIZE;
    l1_trace_data_t *entry = (l1_trace_data_t *)(v_ocram_addr);
    for (i = 0; i < TRACE_SIZE; i++) {
        k = 0;
        while (l1_trace_code[k].msg != 0xffff) {
            if (l1_trace_code[k].msg == entry->msg)
                break;
            k++;
        }
        printf("\n%4d, counter: %016" PRIu64 ", param: 0x%08x, (0x%03x)%s",
               i, entry->cnt, entry->param, entry->msg, l1_trace_code[k].text);
        entry++;
    }
    printf("\n");
}

/* --------------------------------------------------------------------------
 * Modem info / memory mapping
 * -------------------------------------------------------------------------- */
static int get_modem_info(int modem_id) {
    int fd;
    int ret;
    char dev_name[32];
    sprintf(dev_name, "/dev/%s%d", LA9310_DEV_NAME_PREFIX, modem_id);

    fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        printf("File %s open error\n", dev_name);
        return -1;
    }

    ret = ioctl(fd, IOCTL_LA93XX_MODINFO_GET, &mi);
    if (ret < 0) {
        printf("IOCTL_LA9310_MODINFO_GET failed.\n");
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

int map_physical_regions(void) {
    int devmem_fd;

    devmem_fd = open("/dev/mem", O_RDWR);
    if (-1 == devmem_fd) {
        perror("/dev/mem open failed");
        return -1;
    }

    BAR0_addr = mmap(NULL, mi.ccsr.size, PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, mi.ccsr.host_phy_addr);
    if (BAR0_addr == MAP_FAILED) {
        perror("Mapping BAR0_addr buffer failed\n");
        return -1;
    }

    BAR1_addr = mmap(NULL, mi.tcml.size, PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, mi.tcml.host_phy_addr);
    if (BAR1_addr == MAP_FAILED) {
        perror("Mapping BAR1_addr buffer failed\n");
        return -1;
    }

    BAR2_addr = mmap(NULL, mi.tcmu.size, PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, mi.tcmu.host_phy_addr);
    if (BAR2_addr == MAP_FAILED) {
        perror("Mapping BAR2_addr buffer failed\n");
        return -1;
    }

    close(devmem_fd);

    return 0;
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    int32_t c, ret;
    struct stat buffer;
    const char *eld_path = DEFAULT_ELD_PATH;
    int pause_tracing = 0;
    int rearm_tracing = 1; /* re-arm is ON by default; use -R to suppress */

    while ((c = getopt(argc, argv, "hvrRpe:")) != EOF) {
        switch (c) {
        case 'h':
            fprintf(stderr, "Usage: %s [-e <eld>] [-p] [-r] [-R] [-v]\n", argv[0]);
            fprintf(stderr, "  -e E  ELD file for symbol extraction (default: %s)\n", DEFAULT_ELD_PATH);
            fprintf(stderr, "  -p    Pause tracing during snapshot\n");
            fprintf(stderr, "  -r    (no-op) Re-arm after dump is now the default\n");
            fprintf(stderr, "  -R    Disable auto re-arm (keep l1_trace_disable set)\n");
            fprintf(stderr, "  -v    Print version and exit\n");
            exit(0);
        case 'v':
            printf("iq_trace  (runtime ELD symbol extraction via readelf)\n");
            printf("  Default ELD: %s\n", DEFAULT_ELD_PATH);
            exit(0);
        case 'e':
            eld_path = optarg;
            break;
        case 'p':
            pause_tracing = 1;
            break;
        case 'r':
            /* no-op: rearm is now default; kept for script compatibility */
            break;
        case 'R':
            rearm_tracing = 0;
            break;
        default:
            break;
        }
    }

    if (stat("/sys/shiva/shiva_status", &buffer) != 0) {
        perror("la9310 shiva driver not started");
        exit(EXIT_FAILURE);
    }

    ret = get_modem_info(0);
    if (ret != 0) {
        perror("Fail to get modem_info \r\n");
        exit(EXIT_FAILURE);
    }

    if (map_physical_regions()) {
        perror("map_physical_regions failed:");
        exit(EXIT_FAILURE);
    }

    print_vspa_trace(eld_path, pause_tracing, rearm_tracing);
    fflush(stdout);

    return 0;
}
