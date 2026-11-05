/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef __STATS_H__
#define __STATS_H__

#include "iq_replay.h"

typedef enum {
    STAT_DMA_AXIQ_READ,
    STAT_DMA_DDR_WR,
    STAT_APP_DDR_WR,
    STAT_EXT_DMA_DDR_WR,
    ERROR_DMA_DDR_WR_OVERRUN,
    ERROR_AXIQ_FIFO_RX_UNDERRUN,
    ERROR_AXIQ_FIFO_RX_OVERRUN,
    ERROR_EXT_DMA_DDR_WR_OVERRUN,
    STATS_RX_MAX
} stats_rx_e;

typedef enum {
    STAT_DMA_AXIQ_WRITE = 0,
    STAT_DMA_DDR_RD,
    STAT_APP_DDR_RD,
    STAT_EXT_DMA_DDR_RD,
    ERROR_DMA_DDR_RD_UNDERRUN,
    ERROR_AXIQ_FIFO_TX_UNDERRUN,
    ERROR_AXIQ_FIFO_TX_OVERRUN,
    ERROR_EXT_DMA_DDR_RD_UNDERRUN,
    STATS_TX_MAX
} stats_tx_e;

typedef enum { ERROR_DMA_CONFIG_ERROR, ERROR_DMA_XFER_ERROR, STATS_GBL_MAX } stats_gbl_e;

typedef struct s_stats {
    uint32_t gbl_stats[STATS_GBL_MAX];
    uint32_t unused[8 - STATS_GBL_MAX];
    uint32_t tx_stats[STATS_TX_MAX];
    uint32_t rx_stats[RX_NUM_MAX_CHAN][STATS_RX_MAX];
    /* Cycle meters (VSPA ccnt cycles):
     *  [0..5]  TX pass meter: max, count, passes > budget, sum>>4,
     *          max start-gap, last start (|1, 0 = no previous pass)
     *  [6..9]  main loop meter (wakeup to idle): max, count, > budget, sum>>4
     *  [10]    RX pass budget, [11] TX pass budget, [12] loop budget
     *  [13..15] reserved */
    uint32_t meter[16];
} t_stats;

#define METER_TX_BASE   0
#define METER_LOOP_BASE 6
#define METER_RX_BUDGET 10
#define METER_TX_BUDGET 11
#define METER_LOOP_BUDGET 12
#define METER_TX_DDR_INFL 13
#define METER_TX_AXIQ_INFL 14

#if !defined(__VSPA__) && !defined(__M7__)

static char *VSPA_stat_rx_string[STATS_RX_MAX + 1] = { "DMA_AXIQ_RD",    "DMA_DDR_WR",     "APP_DDR_WR",
                                                       "EXT_DDR_WR",     "DDR_WR_OVR",     "FIFO_RX_UDR",
                                                       "FIFO_RX_OVR",    "EXT_DDR_WR_OVR", "STATS_RX_MAX" };

/* Must match stats_tx_e order exactly. */
static char *VSPA_stat_tx_string[STATS_TX_MAX + 1] = { "DMA_AXIQ_WR",    "DMA_DDR_RD",     "APP_DDR_RD",
                                                       "EXT_DDR_RD",     "DDR_RD_UDR",     "FIFO_TX_UDR",
                                                       "FIFO_TX_OVR",    "EXT_DDR_RD_UDR", "STATS_TX_MAX" };

static char *VSPA_stat_gbl_string[STATS_GBL_MAX + 1] = { "DMA_CFG_ERROR", "DMA_XFER_ERROR", "STATS_GBL_MAX" };

#endif

#ifdef __VSPA__
extern volatile t_stats g_stats __attribute__((aligned(64)));
/* Shared cycle meter helpers (_main.c). m points to a 6-word (pass) or
 * 4-word (loop, no gap) meter block laid out as described above. */
void meter_gap(volatile uint32_t *m, uint32_t t0);
void meter_end(volatile uint32_t *m, uint32_t t0, uint32_t budget);
/* DMA-state completion accounting (_main.c): returns the new "done" byte
 * count for a FIFO-mode DMA channel group, see dma_done_from_state(). */
uint32_t dma_done_from_state(uint32_t mask, uint32_t enq, uint32_t done, uint32_t *cand, uint32_t step);
#endif

#endif // __STATS_H__
