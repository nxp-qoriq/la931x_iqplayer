/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2022-2024 NXP
 */

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
#include <pthread.h>
#include <sched.h>
#include <assert.h>
#include <semaphore.h>
#include <signal.h>
#include <argp.h>
#include "imx8-host.h"
#include "stats.h"
#include "vspa_dmem_proxy.h"
#include "la9310_regs.h"

#define pr_info printf
void la9310_hexdump(const void *ptr, size_t sz);
void print_vspa_stats(void);
void monitor_vspa_stats(void);
void print_vspa_trace(void);

extern uint32_t *v_vspa_dmem_proxy_ro;
extern uint32_t *v_tx_vspa_proxy_wo;
extern uint32_t *BAR0_addr;
extern uint32_t *BAR2_addr;
extern volatile uint32_t running;

#define TX_DDR_STEP (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.tx_ddr_step)
#define RX_DDR_STEP (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.rx_ddr_step)
#define RX_DECIM    (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.rx_decim)
#define TX_UPSMP    (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.tx_upsmp)
#define PROXY_CFG   (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.rx_num_chan)
#define RX_NUM_CHAN PROXY_CFG_NRX(PROXY_CFG)

#define m_gbl_stats_fetch (((t_tx_ch_host_proxy *)v_tx_vspa_proxy_wo)->gbl_stats_fetch)

static inline void __hexdump(unsigned long start, unsigned long end, unsigned long p, size_t sz, const unsigned char *c) {
    while (start < end) {
        unsigned int pos = 0;
        char buf[64];
        int nl = 0;

        pos += sprintf(buf + pos, "%08lx: ", start);
        do {
            if ((start < p) || (start >= (p + sz)))
                pos += sprintf(buf + pos, "..");
            else
                pos += sprintf(buf + pos, "%02x", *(c++));
            if (!(++start & 15)) {
                buf[pos++] = '\n';
                nl = 1;
            } else {
                nl = 0;
                if (!(start & 1))
                    buf[pos++] = ' ';
                if (!(start & 3))
                    buf[pos++] = ' ';
            }
        } while (start & 15);
        if (!nl)
            buf[pos++] = '\n';
        buf[pos] = '\0';
        pr_info("%s", buf);
    }
}
void la9310_hexdump(const void *ptr, size_t sz) {
    unsigned long p = (unsigned long)ptr;
    unsigned long start = p & ~15UL;
    unsigned long end = (p + sz + 15) & ~15UL;
    const unsigned char *c = ptr;

    __hexdump(start, end, p, sz, c);
}

/*
 * Ping-pong snapshot buffers.
 *  local_vspa_stats  -- VSPA firmware counters (full t_stats, all fields used)
 *  local_hostapp_stats -- host_stats + app_stats summed; only STAT_EXT_DMA_DDR_WR
 *                         (RX app consumption) and STAT_EXT_DMA_DDR_RD (TX app
 *                         production) are non-zero in practice.
 */
static t_stats local_vspa_stats[2];
/* RX-only firmware reuses tx_stats[] for the cycle breakdown and stall
 * counters. Firmware that publishes the channel map (proxy cfg [31:16] != 0)
 * also gives the TX channel count in [11:9]; older firmware is recognised by
 * TX stats staying zero together with rx_num_chan==4 (tx_stats[6] is reused
 * as elapsed-time counter, so only tx[7] is checked). */
#define PROXY_CFG_HAS_MAP (PROXY_CFG >> 16)
#define N_TX_ZERO_STATS(c) (PROXY_CFG_HAS_MAP ? (PROXY_CFG_NTX(PROXY_CFG) == 0) : \
        ((c)->tx_stats[7] == 0 && (c)->tx_stats[STAT_DMA_AXIQ_WRITE] == 0 && RX_NUM_CHAN == 4))
/* set per refresh: 1 when the loaded firmware has no TX channel */
static int g_no_tx;
/* VSPA cycles per 1 KB AXIQ buffer: 256 samples at 61.44 MSPS with the VSPA
 * at 491.52 MHz, as measured on RFNM (the clock is set by the RFNM FreeRTOS,
 * not by the insmod rate). */
#define VSPA_CY_PER_AXIQ_BUF 2048u
static double g_dt_s = 1.0;
static t_stats local_hostapp_stats[2];
uint32_t vspa_stats_tab;
volatile uint32_t *_VSPA_DMA_regs, *_GP_IN0;

/*
 * print_vspa_stats_table -- full RX / TX / Global display for the VSPA firmware
 * stats (all counters, with MB/s rates where applicable).
 */
static void print_vspa_stats_table(const t_stats *cur, const t_stats *prev,
                                   int rx_num_chan,
                                   uint32_t rx_ddr_step, uint32_t rx_decim,
                                   uint32_t tx_ddr_step, uint32_t tx_upsmp)
{
    int i, j;

    printf("\n[VSPA stats]\n");

    printf("RX stats :");
    for (i = 0; i < STATS_RX_MAX; i++) {
        printf("\n %s", VSPA_stat_rx_string[i]);
        for (j = 0; j < rx_num_chan; j++) {
            uint32_t cur_val  = *((const uint32_t *)(cur->rx_stats[j])  + i);
            uint32_t prev_val = *((const uint32_t *)(prev->rx_stats[j]) + i);

            printf("\t0x%08x", cur_val);
            if (i <= STAT_DMA_AXIQ_READ)
                printf("(%08u MB/s)", (uint32_t)((double)(cur_val - prev_val) * rx_ddr_step * rx_decim / 1e6 / g_dt_s));
            else if (i == STAT_DMA_DDR_WR)
                printf("(%08u MB/s)", (uint32_t)((double)(cur_val - prev_val) * rx_ddr_step / 1e6 / g_dt_s));
            else if (i == STAT_EXT_DMA_DDR_WR)
                printf("(%08u MB/s)", (uint32_t)((double)(cur_val - prev_val) * rx_ddr_step / 1e6 / g_dt_s));
            else
                printf("               ");
        }
    }

    if (g_no_tx) {
        printf("\nTX : none (RX-only firmware)");
        i = STATS_TX_MAX;
    } else {
        printf("\nTX stats :");
        i = 0;
    }
    for (; i < STATS_TX_MAX; i++) {
        uint32_t cur_val  = *((const uint32_t *)(cur->tx_stats)  + i);
        uint32_t prev_val = *((const uint32_t *)(prev->tx_stats) + i);

        printf("\n %s", VSPA_stat_tx_string[i]);
        printf("\t0x%08x", cur_val);
        if (i <= STAT_DMA_AXIQ_WRITE)
            printf("(%08u MB/s)", (uint32_t)((uint64_t)(cur_val - prev_val) * tx_ddr_step * tx_upsmp / 1000000));
        else if (i == STAT_DMA_DDR_RD)
            printf("(%08u MB/s)", (uint32_t)((uint64_t)(cur_val - prev_val) * tx_ddr_step / 1000000));
        else if (i == STAT_EXT_DMA_DDR_RD)
            printf("(%08u MB/s)", (uint32_t)((uint64_t)(cur_val - prev_val) * tx_ddr_step / 1000000));
        else
            printf("               ");
    }

    printf("\nGlobal stats :");
    for (i = 0; i < STATS_GBL_MAX; i++) {
        uint32_t cur_val = *((const uint32_t *)(cur->gbl_stats) + i);

        printf("\n %s", VSPA_stat_gbl_string[i]);
        printf("\t0x%08x", cur_val);
    }
    printf("\n");
}

/*
 * print_hostapp_stats_table -- compact display for the combined host+app stats.
 * Only the two counters actually written by libiqplayer are shown:
 *   RX: STAT_EXT_DMA_DDR_WR -- app RX bytes consumed (per channel)
 *   TX: STAT_EXT_DMA_DDR_RD -- app TX bytes produced
 */
static void print_hostapp_stats_table(const t_stats *cur, const t_stats *prev,
                                      int rx_num_chan,
                                      uint32_t rx_ddr_step,
                                      uint32_t tx_ddr_step)
{
    int j;

    printf("\n[Host/App stats]\n");

    /* RX app consumption -- one column per channel */
    printf("RX app consumed (%s) :", VSPA_stat_rx_string[STAT_EXT_DMA_DDR_WR]);
    for (j = 0; j < rx_num_chan; j++) {
        uint32_t cur_val  = cur->rx_stats[j][STAT_EXT_DMA_DDR_WR];
        uint32_t prev_val = prev->rx_stats[j][STAT_EXT_DMA_DDR_WR];

        printf("\t0x%08x(%08u MB/s)", cur_val,
               (uint32_t)((double)(cur_val - prev_val) * rx_ddr_step / 1e6 / g_dt_s));
    }

    /* TX app production -- single value (no per-channel split on TX side) */
    if (g_no_tx) {
        printf("\nTX app produced : none (RX-only firmware)\n");
    } else {
        uint32_t cur_val  = cur->tx_stats[STAT_EXT_DMA_DDR_RD];
        uint32_t prev_val = prev->tx_stats[STAT_EXT_DMA_DDR_RD];

        printf("\nTX app produced (%s) :\t0x%08x(%08u MB/s)\n",
               VSPA_stat_tx_string[STAT_EXT_DMA_DDR_RD],
               cur_val,
               (uint32_t)((uint64_t)(cur_val - prev_val) * tx_ddr_step / 1000000));
    }
}

void print_vspa_stats(void) {
    int i;
    t_stats *vspa_cur, *vspa_prev;
    t_stats *ha_cur,   *ha_prev;

    t_stats *hw_vspa = &(((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->vspa_stats);
    t_stats *hw_host = &(((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->host_stats);
    t_stats *hw_app  = &(((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->app_stats);

    /* Ping-pong index */
    if (vspa_stats_tab) {
        vspa_stats_tab = 0;
        vspa_cur = &local_vspa_stats[0];    vspa_prev = &local_vspa_stats[1];
        ha_cur   = &local_hostapp_stats[0]; ha_prev   = &local_hostapp_stats[1];
    } else {
        vspa_stats_tab = 1;
        vspa_cur = &local_vspa_stats[1];    vspa_prev = &local_vspa_stats[0];
        ha_cur   = &local_hostapp_stats[1]; ha_prev   = &local_hostapp_stats[0];
    }

    /* Snapshot VSPA stats and the combined host+app stats independently. */
    for (i = 0; i < (int)(sizeof(t_stats) / 4); i++) {
        if ((i % 16) == 0) {
            dccivac((uint32_t *)(hw_vspa) + i);
            dccivac((uint32_t *)(hw_host) + i);
            dccivac((uint32_t *)(hw_app)  + i);
        }
        ((uint32_t *)vspa_cur)[i] = *((uint32_t *)(hw_vspa) + i);

        /* Sum host_stats + app_stats; treat 0xFFFFFFFF (uninitialized) as 0. */
        {
            uint32_t h = *((uint32_t *)(hw_host) + i);
            uint32_t a = *((uint32_t *)(hw_app)  + i);
            if (h == 0xFFFFFFFFu) h = 0u;
            if (a == 0xFFFFFFFFu) a = 0u;
            ((uint32_t *)ha_cur)[i] = h + a;
        }
    }

    /* Guard against uninitialized VSPA proxy (all-0xFF when VSPA not started). */
    int rx_num_chan = (int)RX_NUM_CHAN;
    if (rx_num_chan < 1 || rx_num_chan > RX_NUM_MAX_CHAN) {
        printf("\n[iq_mon] VSPA proxy not initialized (rx_num_chan=0x%x), skipping stats.\n",
               (unsigned int)RX_NUM_CHAN);
        return;
    }

    uint32_t rx_ddr_step = RX_DDR_STEP;
    uint32_t rx_decim    = RX_DECIM;
    uint32_t tx_ddr_step = TX_DDR_STEP;
    uint32_t tx_upsmp    = TX_UPSMP;

    {
        static struct timespec t_prev;
        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (t_prev.tv_sec) {
            double dt = (double)(t_now.tv_sec - t_prev.tv_sec) + (double)(t_now.tv_nsec - t_prev.tv_nsec) * 1e-9;
            if (dt > 0.05)
                g_dt_s = dt;
        }
        t_prev = t_now;
        printf("\n[iq_mon] sample interval %.3f s (rates use real elapsed time)", g_dt_s);
    }

    {
        uint32_t cfg = PROXY_CFG;
        printf("\n[iq_mon] proxy cfg 0x%08x: RX %u  TX %u  IQ swap %u",
               cfg, PROXY_CFG_NRX(cfg), PROXY_CFG_NTX(cfg), PROXY_CFG_IQSWAP(cfg));
        if (cfg >> 16) {
            static const char *la[4] = { "RO1", "RX1", "RO0", "RX0" };
            printf("  map");
            for (i = 0; i < rx_num_chan; i++)
                printf(" slot%d=ch%u(%s)", i, PROXY_CFG_CHAN(cfg, i), la[PROXY_CFG_CHAN(cfg, i) & 3]);
        }
    }

    g_no_tx = N_TX_ZERO_STATS(vspa_cur);

    print_vspa_stats_table(vspa_cur, vspa_prev, rx_num_chan,
                           rx_ddr_step, rx_decim, tx_ddr_step, tx_upsmp);

    print_hostapp_stats_table(ha_cur, ha_prev, rx_num_chan,
                              rx_ddr_step, tx_ddr_step);

    /* RX cycle meter (VSPA uses spare unused[0..5] slots; per-stage sums
     * in tx_stats[0..4] for RX-only builds, all sums are cycles/16) */
    {
        const uint32_t *m = &vspa_cur->unused[0];
        uint32_t dn = m[1] - vspa_prev->unused[1];
        uint32_t ds = m[3] - vspa_prev->unused[3];
        uint32_t rb = vspa_cur->meter[METER_RX_BUDGET] ? vspa_cur->meter[METER_RX_BUDGET] : 2560u;
        printf("\n[VSPA RX cycle meter] pass max %u cy, avg %u cy (last period), passes %u (>%u: %u)"
               ", start-gap max %u cy\n",
               m[0], dn ? (uint32_t)((uint64_t)ds * 16 / dn) : 0, m[1], rb, m[2], m[4]);
        if (N_TX_ZERO_STATS(vspa_cur)) {
            static const char *nm[5] = { "axiq cmpl+rearm", "qec", "decim", "ddr push+tail", "period" };
            uint32_t k, v[5];
            for (k = 0; k < 5; k++) {
                uint32_t d = vspa_cur->tx_stats[k] - vspa_prev->tx_stats[k];
                v[k] = dn ? (uint32_t)((uint64_t)d * 16 / dn) : 0;
            }
            if (v[4]) {
                printf(" per pass avg (last period):");
                for (k = 0; k < 5; k++)
                    printf("  %s %u", nm[k], v[k]);
                printf("  outside %d cy\n", (int)v[4] - (int)(v[0] + v[1] + v[2] + v[3]));
            }
        }
    }

    /* TX pass meter (meter[0..5]) and main loop meter (meter[6..9]) */
    {
        const uint32_t *m = &vspa_cur->meter[METER_TX_BASE];
        const uint32_t *p = &vspa_prev->meter[METER_TX_BASE];
        if (m[1]) {
            uint32_t dn = m[1] - p[1], ds = m[3] - p[3];
            uint32_t b = vspa_cur->meter[METER_TX_BUDGET] ? vspa_cur->meter[METER_TX_BUDGET] : 2560u;
            printf("[VSPA TX cycle meter] pass max %u cy, avg %u cy (last period), passes %u (>%u: %u)"
                   ", start-gap max %u cy\n",
                   m[0], dn ? (uint32_t)((uint64_t)ds * 16 / dn) : 0, m[1], b, m[2], m[4]);
            printf(" TX in flight (last pass): DDR rd %u buf, AXIQ wr %u buf\n",
                   vspa_cur->meter[METER_TX_DDR_INFL], vspa_cur->meter[METER_TX_AXIQ_INFL]);
        }
        m = &vspa_cur->meter[METER_LOOP_BASE];
        p = &vspa_prev->meter[METER_LOOP_BASE];
        if (m[1]) {
            uint32_t dn = m[1] - p[1], ds = m[3] - p[3];
            uint32_t b = vspa_cur->meter[METER_LOOP_BUDGET] ? vspa_cur->meter[METER_LOOP_BUDGET] : 2560u;
            printf("[VSPA loop meter] pass max %u cy, avg %u cy (last period), passes %u (%u/s) (>%u: %u)\n",
                   m[0], dn ? (uint32_t)((uint64_t)ds * 16 / dn) : 0, m[1],
                   (uint32_t)(dn / g_dt_s), b, m[2]);
        }
    }

    /* 0T4R stall counters (only in RX_STALL_STATS_ON builds, recognised by
     * the elapsed-cycle counter tx_stats[6] being non-zero): reuse rx
     * columns APP_DDR_WR, EXT_DDR_WR and EXT_DDR_WR_OVR. */
    if (N_TX_ZERO_STATS(vspa_cur) && vspa_cur->tx_stats[6] != 0) {
        static const struct { int idx; const char *nm; } st[3] = {
            { STAT_APP_DDR_WR, "gate closed" },
            { STAT_EXT_DMA_DDR_WR, "ddr dma busy" },
            { ERROR_EXT_DMA_DDR_WR_OVERRUN, "dec backlog" },
        };
        int k, j;
        uint32_t pd = vspa_cur->tx_stats[5];
        printf("[VSPA RX stalls/s] prime depth ch0..3: %u %u %u %u\n",
               pd & 0xff, (pd >> 8) & 0xff, (pd >> 16) & 0xff, (pd >> 24) & 0xff);
        for (k = 0; k < 3; k++) {
            printf(" %-13s", st[k].nm);
            for (j = 0; j < rx_num_chan; j++)
                printf("\t%10u", vspa_cur->rx_stats[j][st[k].idx] - vspa_prev->rx_stats[j][st[k].idx]);
            printf("\n");
        }
        printf(" %-13s", "cmpl recover");
        for (j = 0; j < rx_num_chan && j < 4; j++)
            printf("\t%10u", vspa_cur->tx_stats[j]);
        printf("  (total)\n");

        /* Loss proof: buffers received vs buffers expected from VSPA elapsed
         * cycles (tx_stats[6] = cycles/1024 since stream start). */
        {
            uint64_t el_cy = (uint64_t)vspa_cur->tx_stats[6] * 1024u;
            uint64_t exp_buf = el_cy / VSPA_CY_PER_AXIQ_BUF;
            double dcy = (double)(vspa_cur->tx_stats[6] - vspa_prev->tx_stats[6]) * 1024.0;
            double fclk = dcy / g_dt_s;
            double bps = (double)(vspa_cur->rx_stats[0][STAT_DMA_AXIQ_READ] -
                                  vspa_prev->rx_stats[0][STAT_DMA_AXIQ_READ]) / g_dt_s;
            printf("[VSPA RX loss check] measured VSPA clk %.2f MHz, ch0 %.0f buf/s (nominal 240000, %.4f%%)"
                   ", cy/buf %.0f\n",
                   fclk / 1e6, bps, bps / 2400.0, bps > 0 ? fclk / bps : 0.0);
            printf(" (%u cy/buf, VSPA 491.52 MHz) expected %llu buf/ch\n",
                   VSPA_CY_PER_AXIQ_BUF, (unsigned long long)exp_buf);
            printf(" %-13s", "rcvd-expected");
            for (j = 0; j < rx_num_chan; j++)
                printf("\t%10lld", (long long)vspa_cur->rx_stats[j][STAT_DMA_AXIQ_READ] - (long long)exp_buf);
            printf("\n %-13s\t%10u  (checks with OVR still set after clear, total)\n",
                   "ovr stuck", vspa_cur->tx_stats[4]);
        }
    }

    _VSPA_DMA_regs = (volatile uint32_t *)((uint64_t)BAR0_addr + VSPA_CCSR + DMA_DMEM_PRAM_ADDR);
    printf("\nVSPA DMA regs (IP reg 0xB0):\n");
    la9310_hexdump((void *)_VSPA_DMA_regs, 0x30);
    _GP_IN0 = (volatile uint32_t *)((uint64_t)BAR0_addr + VSPA_CCSR + GP_IN0);
    printf("VSPA AXI regs (GP_IN0-9 IPREG 0x580):\n");
    la9310_hexdump((void *)_GP_IN0, 0x30);
    printf("\n");

    return;
}

void monitor_vspa_stats(void) {
    printf("\033[2J");
    while (running) {
        /* home + clear screen each refresh so shorter lines do not leave garbage */
        printf("\033[H\033[J");
        print_vspa_stats();
        fflush(stdout);
        m_gbl_stats_fetch = 1;
        sleep(1);
    }
    return;
}
