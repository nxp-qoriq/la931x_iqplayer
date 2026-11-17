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
    uint32_t rx_num_chan; /* RX/TX configuration word, see PROXY_CFG_* below */
    uint32_t rx_fc_addr[RX_NUM_MAX_CHAN]; /* AHB address of rx_vspa_proxy[i].la9310_fifo_consumed_size
                                           * = BASE_VSPA_VCPU(0) + DMEM byte offset.
                                           * Written by VSPA at startup; 0 = VSPA not yet started. */
    uint32_t rx_ddr_step;
    uint32_t tx_ddr_step;
    uint32_t gbl_stats_fetch;
    uint32_t dmemProxyOffset;
} t_tx_ch_host_proxy;

/* Layout of t_tx_ch_host_proxy.rx_num_chan (proxy byte offset 0x1C).
 * The struct stays 64 bytes (one cache line), the fields are packed:
 *   [7:0]   number of RX channels (1..4)
 *   [8]     RX AXIQ I/Q swap (0 = off, 1 = on)
 *   [11:9]  number of TX channels (0 = no TX, e.g. 0T4R)
 *   [15:12] reserved, 0
 *   [31:16] RX channel map, 4 bits per slot: slot i streams mbx chanID
 *           ((v >> (16 + 4 * i)) & 0xF)
 * Written by the VSPA at boot, on RX channel select and on RX IQ swap. */
#define PROXY_CFG_NRX(v)        ((v) & 0xFFu)
#define PROXY_CFG_IQSWAP(v)     (((v) >> 8) & 0x1u)
#define PROXY_CFG_NTX(v)        (((v) >> 9) & 0x7u)
#define PROXY_CFG_CHAN(v, slot) (((v) >> (16 + 4 * (slot))) & 0xFu)
#define PROXY_CFG_MAKE(nrx, iqswap, ntx, map16) \
    (((nrx) & 0xFFu) | (((iqswap) & 0x1u) << 8) | (((ntx) & 0x7u) << 9) | \
     (((map16) & 0xFFFFu) << 16))

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
