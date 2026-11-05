/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef VSPA_DMEM_PROXY_H_
#define VSPA_DMEM_PROXY_H_

#include "iq_replay.h"
#include "stats.h"

/* LA9310 VSPA DMEM AHB slave window: 4 MB at 0x20400000..0x207FFFFF */
#define LA9310_VSPA_DMEM_AHB_BASE  0x20400000u
#define LA9310_VSPA_DMEM_AHB_SIZE  0x00400000u

// IPC region in iqflood

#define IQFLOOD_OUTBOUND_ADDR 0xB0001000
#define VSPA_DMEM_PROXY_SIZE 1024
#define VSPA_DMEM_PROXY_ADDR (IQFLOOD_OUTBOUND_ADDR + g_iqflood_proxy_offset)

typedef struct s_tx_ch_host_proxy {
    uint32_t la9310_fifo_enqueued_size;
    uint32_t la9310_fifo_consumed_size;
    uint32_t DDR_rd_base_address;
    uint32_t DDR_rd_size;
    uint32_t host_produced_size;
    uint32_t rx_decim;
    uint32_t tx_upsmp;
    uint32_t rx_num_chan;
    uint32_t rx_fc_addr[RX_NUM_MAX_CHAN]; /* AHB address of rx_vspa_proxy[i].la9310_fifo_consumed_size
                                           * = BASE_VSPA_VCPU(0) + DMEM byte offset.
                                           * Written by VSPA at startup; 0 = VSPA not yet started. */
    uint32_t rx_ddr_step;
    uint32_t tx_ddr_step;
    uint32_t gbl_stats_fetch;
    uint32_t dmemProxyOffset;
} t_tx_ch_host_proxy;

typedef struct s_rx_ch_host_proxy {
    uint32_t la9310_fifo_produced_size;
    volatile uint32_t la9310_fifo_consumed_size; /* written asynchronously by host via AHB/BAR2 */
    uint32_t DDR_wr_base_address;
    uint32_t DDR_wr_size;
} t_rx_ch_host_proxy;

typedef struct s_vspa_dmem_proxy {
    t_tx_ch_host_proxy tx_state_readonly;
    t_rx_ch_host_proxy rx_state_readonly[RX_NUM_MAX_CHAN];
    t_stats vspa_stats;
    t_stats host_stats;
    t_stats app_stats;
} t_vspa_dmem_proxy;

extern t_tx_ch_host_proxy tx_vspa_proxy __attribute__((aligned(32)));
extern t_rx_ch_host_proxy rx_vspa_proxy[RX_NUM_MAX_CHAN] __attribute__((aligned(32)));

#endif /* IQMOD_TX_H_ */
