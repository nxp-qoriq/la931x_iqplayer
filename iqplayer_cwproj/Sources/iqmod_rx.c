/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <vspa/intrinsics.h>

#include "chip.h"
#include "vcpu.h"
#include "host.h"
#include "dmac.h"
#include "iohw.h"
#include "txiqcomp.h"
#include "dfe.h"
#include "main.h"
#include "l1-trace.h"
#include "iqmod_tx.h"
#include "cal_signal.h"
#include "iqmod_rx.h"
#include "stats.h"
#include "vspa_dmem_proxy.h"
#include "ccnt.h"

/* Cycle meter for the streaming pass (same layout as iqmod_rx_2R_decx2x4.c):
 * g_stats.unused[0..5] = max pass, pass count, passes > RX_PASS_BUDGET,
 * sum(pass)/16, max gap between pass starts, last pass start.
 * Build with -DRX_CYCLE_METER_OFF to remove it. */
#ifndef RX_CYCLE_METER_OFF
#define RX_CYCLE_METER 1
#endif
/* One AXIQ buffer time at VSPA 491.52 MHz: 2048 cy at 61.44 MSPS,
 * 1024 cy at 122.88 MSPS. */
#ifndef RX_PASS_BUDGET
#define RX_PASS_BUDGET 2048u
#endif

/* Per-buffer traces in the streaming loop are compiled out unless
 * -DRX_HOT_TRACE; error traces are kept. */
#ifdef RX_HOT_TRACE
#define RX_TRACE(m, p) l1_trace((m), (p))
#else
#define RX_TRACE(m, p) do { } while (0)
#endif

vspa_complex_fixed16 input_buffer[RX_NUM_BUF * RX_DMA_TXR_size] __attribute__((section(".vcpu_dmem")))
__attribute__((aligned(64))) = { 0x00000000, 0x00010001, 0x00020002, 0x00030003 };
vspa_complex_fixed16 input_qec_buffer[RX_NUM_QEC_BUF * RX_DMA_TXR_size] __attribute__((section(".ippu_dmem")))
__attribute__((aligned(64))) = { 0x00000000, 0x00010001, 0x00020002, 0x00030003 };

// uint32_t DDR_wr_QEC_enable= 0, DDR_wr_CMP_enable=0;
#define DDR_wr_QEC_enable 1
#define DDR_wr_CMP_enable 0
uint32_t DDR_wr_start_bit_update = 0, DDR_wr_load_start_bit_update = 0, DDR_wr_continuous = 0;
uint32_t ddr_wr_dma_xfr_size = RX_DDR_STEP;
uint32_t DDR_wr_buff_wrap_equeued = 0;
uint32_t DDR_wr_buff_loop_count = 0;
static uint32_t host_flow_control_disable = 1;

volatile uint32_t RX_ext_dma_enabled = 0;
uint32_t ddr_wr_dma_ch_nb = 0;
uint32_t ddr_wr_dma_ch_mask = 0;

static uint32_t RX_total_axiq_enqueued_size = 0;                                 /* 0: Axiq rx in fifo cmd	*/
static uint32_t RX_total_axiq_received_size = 0;                                 /* 1: Axiq rx dma completed	*/
volatile uint32_t RX_total_dmem_QECed_size = 0;                                  /* 2: Rx data QECed			*/
/* Private VSPA counters.  la9310_fifo_consumed_size is written by the host
 * (directly in DMEM through rx_fc_addr) and is only read here for flow
 * control.  la9310_fifo_produced_size is published to the host and follows
 * RX_total_dmem_consumed_size (data actually landed in DDR). */
static uint32_t RX_total_dmem_CMPed_size = 0;                                    /* 4: compressed            */
static uint32_t RX_total_ddr_enqueued_size = 0;                                  /* 5: DDR wr cmd fifo 		*/
static uint32_t RX_total_dmem_consumed_size = 0;                                 /* 6: xfer to DDR complete */
#define RX_total_ddr_consumed_size (rx_vspa_proxy[0].la9310_fifo_consumed_size)  /* 7: host consumed (RO) */
/* DDR write completion from DMA state: the completion flag is a single
 * sticky bit, two queued writes can complete between passes and merge.
 * in_flight = no free entry ? 2 : (running ? 1 : 0), done = enqueued -
 * in_flight * step, min() over two passes, never moving back. */
static uint32_t rx_ddr_cand = 0;

static vspa_complex_fixed16 *p_rx_axiq_enqueued = &input_buffer[0];
static vspa_complex_fixed16 *p_rx_axiq_received = &input_buffer[0];
static vspa_complex_fixed16 *p_rx_dmem_QECed_in = &input_buffer[0];
static vspa_complex_fixed16 *p_rx_dmem_QECed_out = &input_qec_buffer[0];
static vspa_complex_fixed16 *p_rx_dmem_CMPed = &input_qec_buffer[0];
static vspa_complex_fixed16 *p_rx_ddr_enqueued = &input_qec_buffer[0];
static vspa_complex_fixed16 *p_rx_consumed = &input_qec_buffer[0];

// uint32_t DDR_wr_base_address = 0xdeadbeef;
uint32_t DDR_wr_offset = 0;
// uint32_t DDR_wr_size;

#define DDR_wr_base_address (rx_vspa_proxy[0].DDR_wr_base_address)
#define DDR_wr_size (rx_vspa_proxy[0].DDR_wr_size)

uint32_t dma_chan_mask(uint32_t dma_channel, uint32_t nb_dma) {
    uint32_t i, mask = 0;

    for (i = 0; i < nb_dma; i++) {
#pragma loop_count(1, 16, 2, 0)
        mask |= 1 << (dma_channel + i);
    }
    return mask;
}

/* Forced inline: removes a 128 B call frame from the PUSH_RX_DATA path. */
static inline __attribute__((always_inline)) void DDR_write_multi_dma(uint32_t DDR_wr_dma_channel, uint32_t nb_dma,
                                                                      uint32_t DDR_address, uint32_t vsp_address,
                                                                      int32_t bytes_size) {
    uint32_t i;
    uint32_t size = bytes_size / nb_dma;

    // user should ensure bytes_size/nb_dma is enire and aligned on AXI bus width 16B.
    // 2048B is ok with 1,2,4 dmas only
    // dma channel should be contiguous

    for (i = 0; i < nb_dma; i++) {
#pragma loop_count(1, 16, 2, 0)
        uint32_t ctrl = DMAC_WRC | (DDR_wr_dma_channel + i);
        /* DMAC_TRIG_VCPU on last channel: fires a GO wakeup when the last
         * sub-transfer completes so main() re-enters without polling. */
        if (i == nb_dma - 1)
            ctrl |= DMAC_TRIG_VCPU;
        dmac_enable(ctrl, size, DDR_address + i * size, vsp_address + i * size);
    }
}

volatile cmp_cycle = 0;
#define CMP_CYCLES (24 + 2 * 512 / 32)
void rx_compress(vspa_complex_fixed16 *data) {
    if (!DDR_wr_CMP_enable)
        return;
    // compression place holder
    for (cmp_cycle = 0; cmp_cycle < CMP_CYCLES; cmp_cycle++) {
    };
}
/* No fused QEC+decimation kernel in the 1-channel RX path. */

void rx_qec_correction(vspa_complex_fixed16 *dataIn, vspa_complex_fixed16 *dataOut, uint32_t ch) {
    if (!DDR_wr_QEC_enable)
        return;

#ifdef RXIQCOMP2
    txiqcomp_x32chf_5t((vspa_complex_fixed16 *)dataIn, (vspa_complex_fixed16 *)dataOut, &iq_comp_params2_rx, MEM_LINE_SIZE);
#else
#ifdef RXIQCOMP
    txiqcomp((vspa_complex_fixed16 *)dataIn, (vspa_complex_fixed16 *)dataOut, RX_QEC_CFG(ch), MEM_LINE_SIZE);
#endif
#endif
}

//__attribute__(( section(".text.opcode_6") ))
void RX_IQ_DATA_TO_DDR(void) {
    /* Use the globals decoded by main() -- do NOT call host_mbox0_read() again
     * here; main() already consumed the mailbox before dispatching to this
     * function so a second read returns 0, cmd_start becomes 0, and the STOP
     * path fires instead of START. */
    uint32_t cmd_start = mailbox_in_msg_0_MSB & 0x00100000;

    if ((cmd_start) && (!DDR_wr_start_bit_update)) {
        DDR_wr_start_bit_update = 1;
        DDR_wr_load_start_bit_update = mailbox_in_msg_0_MSB & 0x00200000;
        DDR_wr_continuous = mailbox_in_msg_0_MSB & 0x00800000;
        // DDR_wr_QEC_enable= mailbox_in_msg_0_MSB & 0x00400000;
        // DDR_wr_CMP_enable= mailbox_in_msg_0_MSB & 0x00080000;
        ddr_wr_dma_ch_nb = (mailbox_in_msg_0_MSB & 0x00070000) >> 16;
        host_flow_control_disable = mailbox_in_msg_0_MSB & 0x00400000;

        if (!DDR_wr_continuous)
            host_flow_control_disable = 0;

        if (ddr_wr_dma_ch_nb > 2)
            goto fail_rx_iq_data;
        if (!ddr_wr_dma_ch_nb) {
            // default 1 DMA write 526MB/s half duplex, 490MB/s full duplex
            ddr_wr_dma_ch_nb = 1;
        }
        ddr_wr_dma_ch_mask = dma_chan_mask(DDR_WR_DMA_CHANNEL_1, ddr_wr_dma_ch_nb);
        ddr_wr_dma_xfr_size = (DDR_wr_CMP_enable ? RX_DDR_STEP * RX_COMPRESS_RATIO_PCT / 100 : RX_DDR_STEP);

        dmac_reset(0x1 << dma_channel_rd);

        p_rx_axiq_enqueued = &input_buffer[0];
        p_rx_axiq_received = &input_buffer[0];
        p_rx_dmem_QECed_in = &input_buffer[0];
        p_rx_dmem_QECed_out = &input_qec_buffer[0];
        p_rx_dmem_CMPed = &input_qec_buffer[0];
        p_rx_ddr_enqueued = &input_qec_buffer[0];
        p_rx_consumed = &input_qec_buffer[0];

        RX_total_axiq_enqueued_size = 0;
        RX_total_axiq_received_size = 0;
        RX_total_dmem_QECed_size = 0;
        RX_total_dmem_CMPed_size = 0;
        RX_total_ddr_enqueued_size = 0;
        RX_total_dmem_consumed_size = 0;

        rx_vspa_proxy[0].la9310_fifo_produced_size = 0;
        rx_vspa_proxy[0].la9310_fifo_consumed_size = 0;
        rx_ddr_cand = 0;
        /* abort any DDR write left from a previous run */
        dmac_abort(ddr_wr_dma_ch_mask);
        dmac_clear_complete(ddr_wr_dma_ch_mask);

        /* Flush zeroed proxy counters to DDR synchronously before the mbox ACK.
         * Host calls iq_player_init_rx() immediately on ACK; a deferred flush
         * via VSPA_PROXY_update() arrives after the ACK and leaves stale
         * produced_size in DDR, causing a false underrun on the first
         * iq_player_receive_data() call. */
        rx_proxy_updated = 0;
        DDR_write_VSPA_PROXY(DDR_WR_DMA_CHANNEL_5,
            VSPA_DMEM_PROXY_ADDR + offsetof(struct s_vspa_dmem_proxy, rx_state_readonly) * 2,
            2 * (uint32_t)&(rx_vspa_proxy[0]),
            sizeof(t_rx_ch_host_proxy) * 2 * RX_NUM_CHAN);
        while (!dmac_is_complete(0x1 << DDR_WR_DMA_CHANNEL_5)) {}
        dmac_clear_complete(0x1 << DDR_WR_DMA_CHANNEL_5);

        DDR_wr_size = ((mailbox_in_msg_0_MSB & 0x0000FFFF) * SIZE_4K); // send chunks of 4KB can be sent
        DDR_wr_base_address = mailbox_in_msg_0_LSB;
        DDR_wr_offset = 0;

        DDR_wr_buff_wrap_equeued = 0;
        DDR_wr_buff_loop_count = 0;

        // update host vspa_dmem_proxy
        rx_proxy_updated = 1;
        tx_proxy_updated = 1; /* tx proxy contains also some rx attributes  */

        /* LA9310 AXI bus supports 2 opened WR transactions
         * VSPA DMA DDR write reach 2x DMA 529MB/s, good enough for 122.88MSPS
         *
         */

        // if simple load test no axiq init
        if (DDR_wr_load_start_bit_update != 0) {
            DDR_wr_load_start_bit_update = 1;
            // Ack mailbox
            mailbox_out_msg_0_MSB = 0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            return;
        }

        // stop single tone
        RX_SingleT_start_bit_update = 0;

        // Disable Rx and reset fifo
        axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index]);
        stream_read_ptr_rst(dma_channel_rd, axi_rd);
        /* Wait for the 64-byte ptr_rst transfer to complete.
         * Original code used '|| !dmac_errxfr(...)' which requires BOTH complete
         * AND errxfr simultaneously -- errxfr never fires on a clean reset so the
         * loop spins forever.  Wait on complete only, then clear.
         * Plain wait, no nested PUSH_TX_DATA(): the nested call stacked a
         * second 256 B frame on top of this handler and set the VCPU stack
         * peak. The 64 B reset is short; a transient TX underrun at stream
         * start is accepted to save stack. Checked by tools/stack_check.py. */
        while (!dmac_is_complete(0x1 << dma_channel_rd)) {
        }
        dmac_clear_complete(0x1 << dma_channel_rd);

        if (DDR_wr_continuous) {
            // ack stream start
            mailbox_out_msg_0_MSB = 0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
        }

        RX_FIFO_ENABLE(Rx_Antenna2fifo_index[RX_index]);
        axiq_fifo_rx_cr(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index], AXIQ_CR_CLRERR, AXIQ_CR_CLRERR);
        axiq_fifo_rx_cr(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index], AXIQ_CR_CLRERR, 0);

        dmac_clear_errxfr(0x1 << dma_channel_rd);

        /* Prime the 2-deep AXIQ RX DMAC FIFO before returning to main().
         * Without initial stream_read calls the pipeline never starts:
         * PUSH_RX_DATA() waits on dmac_is_complete() which never fires
         * because no DMA was ever started. */
        {
            uint32_t prime_busy;
        while (dmac_is_available(0x1 << dma_channel_rd)) {
            prime_busy = RX_total_axiq_enqueued_size - RX_total_dmem_QECed_size;
            if ((RX_NUM_BUF * RX_DDR_STEP) - prime_busy < RX_DDR_STEP)
                break;
            stream_read(dma_channel_rd, axi_rd, 2 * (uint32_t)(p_rx_axiq_enqueued));
            INCR_RX_BUFF(p_rx_axiq_enqueued);
            RX_total_axiq_enqueued_size += RX_DDR_STEP;
            RX_TRACE(L1_TRACE_MSG_DMA_AXIQ_RX_START, (uint32_t)p_rx_axiq_enqueued);
        }
        }
#ifdef RX_CYCLE_METER
        /* restart cycle meter: 0 in unused[5] marks "no previous pass" */
        {
            uint32_t k;
            for (k = 0; k < 6; k++) g_stats.unused[k] = 0;
            g_stats.meter[METER_RX_BUDGET] = RX_PASS_BUDGET;
        }
#endif

        return;
    }

    if (cmd_start == 0) /* stop */
    {
        if (!RX_SingleT_start_bit_update) {
            dmac_abort(0x1 << dma_channel_rd);
            dmac_clear_complete(0x1 << dma_channel_rd);
            axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index]);
        }

        if (ddr_wr_dma_ch_mask) {
            dmac_abort(ddr_wr_dma_ch_mask);
            dmac_clear_complete(ddr_wr_dma_ch_mask);
        }
        rx_vspa_proxy[0].la9310_fifo_produced_size = 0;
        DDR_wr_base_address = 0xdeadbeef;
        RX_total_dmem_CMPed_size = 0;
        RX_total_dmem_consumed_size = 0;

        DDR_wr_start_bit_update = 0;
        DDR_wr_load_start_bit_update = 0;
        RX_SingleT_start_bit_update = 0;
        mailbox_out_msg_0_MSB = 0;
        mailbox_out_msg_0_LSB = 0x1;
        host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));

        // update host vspa_dmem_proxy
        rx_proxy_updated = 1;

        return;
    }

fail_rx_iq_data:

    /* failure send Nack */
    mailbox_out_msg_0_MSB = 0;
    mailbox_out_msg_0_LSB = 0x0;
    host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
    return;
}

//#pragma optimize_for_size off
volatile uint32_t rx_busy_size = 0;
void PUSH_RX_DATA(void) {
    uint32_t tmp_status;
    uint32_t tmp_dma_errors;
    uint32_t rx_empty_size;

    // Check AXIQ rx fifo is not full or overrun
    tmp_status = axiq_fifo_rx_sr(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index], AXIQ_SR_FIELD_ERROVER | AXIQ_SR_FIELD_ERRUNDER);
    if (tmp_status != 0) {
        if (tmp_status & (AXIQ_SR_FIELD_ERROVER << axiq_sr_shift(Rx_Antenna2fifo_index[RX_index]))) {
            g_stats.rx_stats[0][ERROR_AXIQ_FIFO_RX_OVERRUN]++;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_OVER, (uint32_t)g_stats.rx_stats[0][ERROR_AXIQ_FIFO_RX_OVERRUN]);
        }
        if (tmp_status & (AXIQ_SR_FIELD_ERRUNDER << axiq_sr_shift(Rx_Antenna2fifo_index[RX_index]))) {
            g_stats.rx_stats[0][ERROR_AXIQ_FIFO_RX_UNDERRUN]++;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_UNDER, (uint32_t)g_stats.rx_stats[0][ERROR_AXIQ_FIFO_RX_UNDERRUN]);
        }
        // l1_trace_disable = 1;
        axiq_fifo_rx_cr(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index], AXIQ_CR_CLRERR, AXIQ_CR_CLRERR);
        axiq_fifo_rx_cr(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index], AXIQ_CR_CLRERR, 0);
    }

    // DDR DMA load test , just fetch DDR as fast as possible
    if (DDR_wr_load_start_bit_update) {
        if (!RX_ext_dma_enabled) {
            // check DDR dma completion
            if (dmac_is_complete(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
                dmac_clear_complete(ddr_wr_dma_ch_mask);
                g_stats.rx_stats[0][STAT_DMA_DDR_WR]++;
                l1_trace(L1_TRACE_MSG_DMA_DDR_WR_COMP, (uint32_t)g_stats.rx_stats[0][STAT_DMA_DDR_WR]);
            }
            // restart DDR dma if possible
            p_rx_ddr_enqueued = input_qec_buffer;
            if (dmac_is_available(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
                DDR_write_multi_dma(DDR_WR_DMA_CHANNEL_1, ddr_wr_dma_ch_nb, DDR_wr_base_address + DDR_wr_offset,
                                    2 * (uint32_t)p_rx_ddr_enqueued, ddr_wr_dma_xfr_size);
                DDR_wr_offset = (DDR_wr_offset + ddr_wr_dma_xfr_size) % DDR_wr_size;
                l1_trace(L1_TRACE_MSG_DMA_DDR_WR_START, (uint32_t)p_rx_ddr_enqueued);
            }
        }
    }

#ifndef IQMOD_RX_0T1R
    // Single Tone
    if (RX_SingleT_start_bit_update) {
        // check axiq dma completion (received new samples) and QEC them
        if (dmac_is_complete(0x1 << dma_channel_rd)) {
            dmac_clear_complete(0x1 << dma_channel_rd);
            dmac_clear_event(0x1 << dma_channel_rd);
            g_stats.rx_stats[0][STAT_DMA_AXIQ_READ]++;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_COMP, (uint32_t)g_stats.rx_stats[0][STAT_DMA_AXIQ_READ]);
        }
        // restart axiq  dma if possible
        if (dmac_is_available(0x1 << dma_channel_rd)) {
            stream_read(dma_channel_rd, axi_rd, 2 * (uint32_t)(RX_SingleT_buffer));
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_START, (uint32_t)RX_SingleT_buffer);
        }
    }
#endif

    // if any of previous load test is on, skip DDR streaming
    if (DDR_wr_load_start_bit_update || RX_SingleT_start_bit_update)
        goto end_rx_push;

    // Stream waveform to DDR
    if (DDR_wr_start_bit_update) {
#ifdef RX_CYCLE_METER
        uint32_t cm_t0 = (uint32_t)ccnt_read();
        {
            uint32_t gap = cm_t0 - g_stats.unused[5];
            if (g_stats.unused[5] != 0 && gap < 0x00100000u && gap > g_stats.unused[4])
                g_stats.unused[4] = gap;
            g_stats.unused[5] = cm_t0 | 1u;
        }
#endif
        /* Use while not if: back-to-back AXIQ DMAs complete in ~79 cycles so
         * the second completion may already be pending when PUSH_RX_DATA runs.
         * A plain 'if' misses it: __builtin_done() finds no pending channel and
         * the VCPU sleeps until the next DDR WR DMAC_TRIG_VCPU wakeup (~several
         * thousand cycles at 122.88 MSPS) -- long enough for an AXIQ FIFO
         * overrun.  The while loop processes all back-to-back completions
         * immediately (at most 2 for the 2-deep FIFO). */
        while (dmac_is_complete(0x1 << dma_channel_rd)) {
            dmac_clear_complete(0x1 << dma_channel_rd);
            dmac_clear_event(0x1 << dma_channel_rd);
            RX_total_axiq_received_size += RX_DDR_STEP;
            g_stats.rx_stats[0][STAT_DMA_AXIQ_READ]++;
            RX_TRACE(L1_TRACE_MSG_DMA_AXIQ_RX_COMP, (uint32_t)g_stats.rx_stats[0][STAT_DMA_AXIQ_READ]);
            // check axiq dma error
            if (dmac_errxfr(0x1 << dma_channel_rd)) {
                dmac_clear_errxfr(0x1 << dma_channel_rd);
                g_stats.gbl_stats[ERROR_DMA_XFER_ERROR]++;
                l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_XFER_ERROR, (uint32_t)g_stats.gbl_stats[ERROR_DMA_XFER_ERROR]);
            }
        }
        // restart axiq  dma if possible
        if (dmac_is_available(0x1 << dma_channel_rd)) {
            rx_busy_size = RX_total_axiq_enqueued_size - RX_total_dmem_QECed_size;
            rx_empty_size = (RX_NUM_BUF * RX_DDR_STEP) - rx_busy_size;
            if (rx_empty_size >= RX_DDR_STEP) {
                stream_read(dma_channel_rd, axi_rd, 2 * (uint32_t)(p_rx_axiq_enqueued));
                INCR_RX_BUFF(p_rx_axiq_enqueued);
                RX_total_axiq_enqueued_size += RX_DDR_STEP;
                RX_TRACE(L1_TRACE_MSG_DMA_AXIQ_RX_START, (uint32_t)p_rx_axiq_enqueued);
            } else {
                // overflow no more dmem buffer to arm new axiq DMA
                g_stats.rx_stats[0][ERROR_DMA_DDR_WR_OVERRUN]++;
                l1_trace(L1_TRACE_MSG_DMA_DDR_WR_OVERRUN, (uint32_t)g_stats.rx_stats[0][ERROR_DMA_DDR_WR_OVERRUN]);
                // l1_trace_disable = 1;
            }
        }
        if ((RX_total_axiq_received_size - RX_total_dmem_QECed_size) >= RX_DDR_STEP) {
            rx_busy_size = RX_total_dmem_QECed_size - RX_total_dmem_consumed_size;
            rx_empty_size = (RX_NUM_QEC_BUF * RX_DDR_STEP) - rx_busy_size;
            if (rx_empty_size >= RX_DDR_STEP) {
                // QEC buffer just received
                RX_TRACE(L1_TRACE_L1APP_RX_QEC_START, (uint32_t)p_rx_dmem_QECed_in);
                rx_qec_correction((vspa_complex_fixed16 *)p_rx_dmem_QECed_in, (vspa_complex_fixed16 *)p_rx_dmem_QECed_out, 0);
                INCR_RX_BUFF(p_rx_dmem_QECed_in);
                INCR_RX_QEC_BUFF(p_rx_dmem_QECed_out);
                RX_total_dmem_QECed_size += RX_DDR_STEP;
                RX_TRACE(L1_TRACE_L1APP_RX_QEC_COMP, (uint32_t)RX_total_dmem_QECed_size);
            }
        }
        if ((RX_total_dmem_QECed_size - RX_total_dmem_CMPed_size) >= RX_DDR_STEP) {
            // Compress buffer
            RX_TRACE(L1_TRACE_L1APP_RX_CMP_START, (uint32_t)p_rx_dmem_CMPed);
            // rx_compress((vspa_complex_fixed16 *)p_rx_dmem_CMPed);
            INCR_RX_QEC_BUFF(p_rx_dmem_CMPed);
            RX_total_dmem_CMPed_size += RX_DDR_STEP;
            RX_TRACE(L1_TRACE_L1APP_RX_CMP_COMP, (uint32_t)RX_total_dmem_QECed_size);
        }
        // Push data to DDR if standalone mode
        if (!RX_ext_dma_enabled) {
            // check DDR dma completion
            if (RX_total_ddr_enqueued_size != RX_total_dmem_consumed_size) {
                uint32_t r, f, n, cand, prev;
                dmac_clear_complete(ddr_wr_dma_ch_mask);
                r = dmac_is_running(ddr_wr_dma_ch_mask);
                f = (dmac_is_available(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask);
                n = !f ? 2u : (r ? 1u : 0u);
                cand = RX_total_ddr_enqueued_size - n * RX_DDR_STEP;
                /* glitch filter: use min(this pass, previous pass) */
                prev = rx_ddr_cand;
                rx_ddr_cand = cand;
                if ((int32_t)(prev - cand) < 0)
                    cand = prev;
                if ((int32_t)(cand - RX_total_dmem_consumed_size) > 0) {
                    g_stats.rx_stats[0][STAT_DMA_DDR_WR] += (cand - RX_total_dmem_consumed_size) / RX_DDR_STEP;
                    RX_total_dmem_consumed_size = cand;
                    rx_vspa_proxy[0].la9310_fifo_produced_size = cand;
                    rx_proxy_updated = 1;
                    if (DDR_wr_buff_wrap_equeued && cand == RX_total_ddr_enqueued_size) {
                        DDR_wr_buff_wrap_equeued = 0;
                        DDR_wr_buff_loop_count++;
                    }
                }
            }

            // host flow control (host-written consumed counter, read only)
            if (host_flow_control_disable ||
                (RX_total_ddr_enqueued_size - RX_total_ddr_consumed_size + RX_DDR_STEP <= DDR_wr_size)) {
                if ((RX_total_dmem_CMPed_size - RX_total_ddr_enqueued_size) >= RX_DDR_STEP) {
                    if (dmac_is_available(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
                        DDR_write_multi_dma(DDR_WR_DMA_CHANNEL_1, ddr_wr_dma_ch_nb, DDR_wr_base_address + DDR_wr_offset,
                                            2 * (uint32_t)p_rx_ddr_enqueued, ddr_wr_dma_xfr_size);
                        INCR_RX_QEC_BUFF(p_rx_ddr_enqueued);
                        RX_total_ddr_enqueued_size += RX_DDR_STEP;
                        DDR_wr_offset += ddr_wr_dma_xfr_size;
                        if (DDR_wr_offset >= DDR_wr_size) {
                            DDR_wr_buff_wrap_equeued = 1;
                            DDR_wr_offset = 0;
                        }
                        RX_TRACE(L1_TRACE_MSG_DMA_DDR_WR_START, (uint32_t)p_rx_ddr_enqueued);
                    }
                }
            }
        }
        if ((DDR_wr_buff_loop_count) && (!DDR_wr_continuous)) {

            dmac_abort(0x1 << dma_channel_rd);
            dmac_clear_complete(0x1 << dma_channel_rd);
            axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[RX_index]);

            DDR_wr_base_address = 0xdeadbeef;
            DDR_wr_start_bit_update = 0;
            mailbox_out_msg_0_MSB = 0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));

            // update host vspa_dmem_proxy
            rx_proxy_updated = 1;
        }
#ifdef RX_CYCLE_METER
        {
            meter_end(&g_stats.unused[0], cm_t0, RX_PASS_BUDGET);
        }
#endif
    }

end_rx_push:
    /* Check DMA errors*/
    tmp_dma_errors = (uint32_t)iord(DMA_CFGERR_STAT);
    if (tmp_dma_errors != 0) {
        l1_trace(L1_TRACE_MSG_DMA_CFGERR, (uint32_t)(g_stats.gbl_stats[ERROR_DMA_CONFIG_ERROR]));
        g_stats.gbl_stats[ERROR_DMA_CONFIG_ERROR]++;
        dmac_clear_errcfg();
        while (dbg_gbl == 6) {
        };
    };

    // update host proxy if needed
    VSPA_PROXY_update();
}
#pragma optimize_for_size reset
