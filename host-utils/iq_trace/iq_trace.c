/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
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
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>
#include <la9310_modinfo.h>
#include <la9310_host_if.h>
#include "l1-trace.h"
#include "imx8-host.h"
#include "vspa_trace_enum.h"
#include "vspa_exported_symbols.h"

uint32_t *v_ocram_addr;
uint32_t *BAR0_addr;
uint32_t *BAR1_addr;
uint32_t *BAR2_addr;
modinfo_t mi;

/* need following vspa symbols to be exported (vspa_exported_symbols.h)*/
// VSPA_EXPORT(l1_trace_data)
// VSPA_EXPORT(l1_trace_index)
// VSPA_EXPORT(l1_trace_disable)

#ifdef v_l1_trace_data
void print_vspa_trace(void) {
    uint32_t i, j, k;
    uint32_t TRACE_SIZE = s_l1_trace_data / sizeof(l1_trace_data_t);
    l1_trace_data_t *entry;

    *(v_l1_trace_disable) = 1;

    for (i = 0, j = *(v_l1_trace_index); i < TRACE_SIZE; i++, j++) {
        if (j >= TRACE_SIZE)
            j = 0;
        entry = (l1_trace_data_t *)v_l1_trace_data + j;
        k = 0;
        while (l1_trace_code[k].msg != 0xffff) {
            if (l1_trace_code[k].msg == entry->msg)
                break;
            k++;
        }
        printf("\n%4d, counter: %016ld, param: 0x%08x, (0x%03x)%s", i, entry->cnt, entry->param, entry->msg, l1_trace_code[k].text);
    }
    printf("\n");

    *(v_l1_trace_disable) = 0;

    return;
}
#else
void print_vspa_trace(void) { printf("\n VSPA trace disabled \n"); }
#endif

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
        printf("\n%4d, counter: %016ld, param: 0x%08x, (0x%03x)%s", i, entry->cnt, entry->param, entry->msg, l1_trace_code[k].text);
        entry++;
    }
    printf("\n");
    return;
}

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
    int devmem_fd, i;
    // void *scr_p;
    // uint32_t mapsize;
    // uint64_t k_page;

    /*
     * map memory regions
     */

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

void print_cmd_help(void) {
    fprintf(
        stderr,
        "\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++");
    fprintf(stderr, "\n|    iq_trace");
    fprintf(stderr, "\n|");
    fprintf(stderr, "\n| ./iq_streamer");
    fprintf(stderr, "\n|");
    fprintf(
        stderr,
        "\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++");
    return;
}

int main(int argc, char *argv[]) {
    int32_t c, i, ret;
    struct stat buffer;

    /* command line parser */
    while ((c = getopt(argc, argv, "hfrts:mdvxDMc")) != EOF) {
        switch (c) {
        case 'h': // help
            print_cmd_help();
            exit(1);
            break;
        case 'v':
            printf("%s", IQ_PLAYER_ELD_VERSION);
            exit(1);
            break;
        }
    }

    /*
     * get modem info
     */
    if (stat("/sys/shiva/shiva_status", &buffer) != 0) {
        perror("la9310 shiva driver not started");
        exit(EXIT_FAILURE);
    }

    ret = get_modem_info(0);
    if (ret != 0) {
        perror("Fail to get modem_info \r\n");
        exit(EXIT_FAILURE);
    }

    /*
     * map memory regions
     */
    if (map_physical_regions()) {
        perror("map_physical_regions failed:");
        exit(EXIT_FAILURE);
    }

    /*
     *   Print VSPA trace
     */
    print_vspa_trace();
    fflush(stdout);

    return 0;
}
