/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef __LIB_IQ_API_H__
#define __LIB_IQ_API_H__

#include <stdint.h>

/* Maximum number of RX channels supported by the VSPA firmware. */
#ifndef RX_NUM_MAX_CHAN
#define RX_NUM_MAX_CHAN  4u
#endif

/*
 * iq_player_init - initialise the IQ player library.
 *
 * v_iqflood      : virtual address of the iqflood DDR carveout.
 * iqflood_size   : size of the carveout in bytes.
 * v_la9310_pci_bar2 : virtual address of the LA9310 BAR2 (TCMU) window.
 * ccsr_phys      : physical base address of LA9310 CCSR (from modinfo).
 *                  Pass 0 to skip VSPA mbox1 mapping (BAR2-write-only fallback).
 * core_id        : VSPA core index (0-7).  Selects the mbox1 register bank.
 */
int iq_player_init(uint32_t *v_iqflood, uint32_t iqflood_size,
                   uint32_t *v_la9310_pci_bar2,
                   uint64_t ccsr_phys, uint32_t core_id);

int iq_player_init_tx(uint32_t fifo_start, uint32_t fifo_size);
int iq_player_send_data(uint32_t *v_buffer, uint32_t size);

int iq_player_init_rx(uint32_t chan, uint32_t fifo_start, uint32_t fifo_size);
int iq_player_receive_data(uint32_t chan, uint32_t *v_buffer, uint32_t max_size);

/*
 * Drain chunk size used by iq_player_receive_data_multi().
 * Capping each round-robin step to 16 KB distributes drain bandwidth evenly
 * across all channels and prevents fast-producing channels from starving
 * slower ones, which is the primary cause of DDR_WR_OVR on ch2/ch3.
 */
#define IQ_PLAYER_DRAIN_CHUNK_SIZE  16384u

/*
 * Per-channel request descriptor for iq_player_receive_data_multi().
 *   chan_id  : channel index (0..RX_NUM_MAX_CHAN-1).
 *   dst      : destination buffer (may be NULL to advance FC without copying).
 *   max_size : maximum bytes to drain for this channel this iteration.
 *              Caller should set this to min(free_ring_space, IQ_PLAYER_DRAIN_CHUNK_SIZE).
 *   got      : output -- actual bytes drained (0 if no data available).
 */
typedef struct {
    uint32_t  chan_id;
    uint32_t *dst;
    uint32_t  max_size;
    int       got;
} rx_drain_req_t;

/*
 * iq_player_receive_data_multi - drain up to N channels in one call.
 *
 * Optimisations vs N individual iq_player_receive_data() calls:
 *   1. Single DC CIVAC: one dccivac covers all 4 x t_rx_ch_host_proxy entries
 *      (64 bytes = 1 A53 cache line) regardless of N.
 *   2. Ahead FC-ack: rx_fc_notify() is called for every channel BEFORE any
 *      invalidate_region()/memcpy(), so VSPA can start writing the next DDR
 *      chunk on all channels while the host is still copying the current one.
 *   3. Batched invalidate+copy: invalidate_region()+memcpy() is done in a
 *      second pass after all FC-acks have been sent.
 *
 * Returns 1 if at least one channel had data, 0 if all channels were idle.
 */
int iq_player_receive_data_multi(rx_drain_req_t *reqs, uint32_t n);

/* Per calling thread: total ns spent in cache invalidate and in memcpy for
 * RX drains (with IQ_RX_COPY=1 both are combined into copy_ns). */
void iq_player_get_rx_copy_times(uint64_t *inval_ns, uint64_t *copy_ns);

/* Print per-channel RX flow-control diagnostics (acks, notify time, lags). */
void iq_player_print_rx_diag(uint32_t first_chan, uint32_t n, double run_s);

/*
 * iq_player_set_rx_copy_rate - limit the fraction of RX steps that do memcpy.
 * percent : 0-100.  100 = copy every step (default).
 *           0 = never copy (FC gate stays open; useful for DDR throughput tests).
 * Consumed pointer still advances on every step regardless of copy_rate.
 */
void iq_player_set_rx_copy_rate(uint32_t percent);

/*
 * iq_player_set_rx_throttle_rate - insert sleep-based back-pressure.
 * percent     : 0-100; 100 = full speed (default).  Lower values slow VSPA DDR writes.
 * ref_mbps    : nominal full-rate in MB/s for this platform (e.g. 500 for LA9310).
 */
void iq_player_set_rx_throttle_rate(uint32_t percent, uint32_t ref_mbps);

#endif
