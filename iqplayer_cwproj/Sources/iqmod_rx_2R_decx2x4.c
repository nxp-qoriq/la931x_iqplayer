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
#include "main.h"
#include "stats.h"
#include "ddc2x4x.h"
#include "vspa_dmem_proxy.h"
#include "ccnt.h"

/* Cycle meter for the streaming pass, about 20-30 cycles per pass.
 * Uses the spare g_stats.unused[0..5] slots: max pass, pass count,
 * passes > 2560 cy, sum(pass)/16, max gap between pass starts, last pass start.
 * Build with -DRX_CYCLE_METER_OFF to remove it completely. */
#ifndef RX_CYCLE_METER_OFF
#define RX_CYCLE_METER 1
#endif
/* Pass budget in VSPA cycles for the "over budget" count: one AXIQ buffer
 * time (2560 cy at 61.44 MSPS, 1280 cy at 122.88 MSPS). */
#ifndef RX_PASS_BUDGET
#define RX_PASS_BUDGET 2560u
#endif

/* Per-stage cycle sums (>>4) in tx_stats[0..4], only in RX-only builds where
 * the TX stats are unused: [0] completions+re-arm, [1] QEC, [2] decimation,
 * [3] DDR push + tail, [4] start-to-start period.  Needs -DRX_STAGE_METER. */
#if defined(RX_CYCLE_METER) && defined(RX_STAGE_METER) && defined(IQMOD_RX_0T4R)
#define RX_STAGE(k) do { uint32_t _t = (uint32_t)ccnt_read(); \
        g_stats.tx_stats[k] += (_t - cm_ts) >> 4; cm_ts = _t; } while (0)
#else
#define RX_STAGE(k) do { } while (0)
#endif

/* Per-buffer traces in the streaming loop cost tens of cycles each (about
 * 600 cycles per 4-channel round with QEC).  They are compiled out unless
 * -DRX_HOT_TRACE; error traces are kept. */
#ifdef RX_HOT_TRACE
#define RX_TRACE(m, p) l1_trace((m), (p))
#else
#define RX_TRACE(m, p) do { } while (0)
#endif

/* Stall counters (0T4R only, opt-in with -DRX_STALL_STATS_ON; they cost
 * cycles in the 4R pass and are diagnostic only).  They reuse
 * rx_stats columns that VSPA never writes in the streaming path:
 *   [STAT_APP_DDR_WR]              data pending, host FC gate closed
 *   [STAT_EXT_DMA_DDR_WR]          data pending, gate open, DDR DMA busy
 *   [ERROR_EXT_DMA_DDR_WR_OVERRUN] decimation blocked by dec-ring backlog
 * tx_stats[5] holds the AXIQ prime depth per channel at stream start,
 * one byte per channel (ch0 in bits 7:0).
 * Increments sit only on stall branches, the normal path is unchanged. */
/* tx_stats[] is only free for RX diagnostics in RX-only builds (0T4R).  In
 * 1T2R it holds the real TX stats, so only the rx_stats stall counters are
 * built there. */
#ifdef IQMOD_RX_0T4R
#define RX_TXSTATS_FREE 1
#endif
#if (defined(IQMOD_RX_0T4R) || defined(IQMOD_RX_1T2R)) && defined(RX_STALL_STATS_ON)
#define RX_STALL_STATS 1
#define RX_STALL(ch, idx) (g_stats.rx_stats[ch][idx]++)
#ifdef RX_TXSTATS_FREE
/* Loss proof (updated in the 1-in-16 AXIQ check only):
 *   tx_stats[6] elapsed VSPA cycles since stream start, in units of 1024 cy
 *   tx_stats[4] checks where an overrun bit was still set after the clear
 * Host compares DMA_AXIQ_RD with elapsed cycles / 2560 (one 1 KB buffer). */
static uint32_t rx_el_last, rx_el_frac;
#endif
#else
#define RX_STALL(ch, idx) do { } while (0)
#endif

/* DDR write accounting from DMA state (0T4R, disable with
 * -DRX_DDR_LEGACY_CMPL).  The completion flag is a single sticky bit, so with
 * two writes queued in the 2-entry command FIFO two completions can merge
 * into one and produced_size falls behind for good.  Instead, the number of
 * writes in flight is derived each pass from XRUN and FIFO_STAT:
 *   not running, entry free -> 0;  running, entry free -> 1;  no entry -> 2
 * and produced = enqueued - in_flight * step.  produced never moves back,
 * and it advances to min(value this pass, value previous pass), so a single
 * transient status read cannot overestimate it.
 * RX_DDR_INFLIGHT (1 or 2, default 2) is the max number of DDR writes queued
 * per channel, gated on the in-flight count read in the same pass.
 * Cycle budget at 4R (measured, FC off): with 1 write in flight or with the
 * full 3-read status sequence (-DRX_DDR_STATUS_FULL) the last channel
 * starves and the AXIQ FIFO overruns.  The default (2 in flight, XRUN +
 * FIFO_STAT reads only) is lossless with and without host flow control.
 * tx_stats[0..3] count advances of more than 2 chunks at once (only with
 * RX_STALL_STATS); shared with the optional RX_STAGE_METER. */
#if (defined(IQMOD_RX_0T4R) || defined(IQMOD_RX_1T2R)) && !defined(RX_DDR_LEGACY_CMPL)
#define RX_DDR_STATE_CMPL 1
#ifndef RX_DDR_INFLIGHT
#define RX_DDR_INFLIGHT 2
#endif
static uint32_t rx_ddr_cand[RX_NUM_CHAN];
#endif

/* Channel service order in the streaming loops.  -DRX_REVERSE_ORDER serves
 * channel 3 first and channel 0 last (diagnostic: tells loop-order starvation
 * apart from a hardware/DMA-channel limit). */
#ifdef RX_REVERSE_ORDER
#define RX_FOR_EACH_CHAN(i) for ((i) = RX_NUM_CHAN; (i)-- > 0;)
#else
#define RX_FOR_EACH_CHAN(i) for ((i) = 0; (i) < RX_NUM_CHAN; (i)++)
#endif

vspa_complex_fixed16 input_buffer[RX_NUM_CHAN][RX_NUM_BUF * RX_DMA_TXR_size] __attribute__((section(".ippu_dmem")))
__attribute__((aligned(64)));
vspa_complex_fixed16 input_dec_buffer[RX_NUM_CHAN][RX_NUM_DEC_BUF * (RX_DMA_TXR_size / RX_DECIM)]
    __attribute__((section(".vcpu_dmem"))) __attribute__((aligned(64)));

cfixed16_t filtState[RX_NUM_CHAN][32] __attribute__((section(".ippu_dmem"))) __attribute__((aligned(64)));
int filter_taps_downsampling[8] __attribute__((aligned(64))) = {
#include "para_files\2xdown_coeff.txt"
};

t_rx_ch_context rx_ch_context[RX_NUM_CHAN];

// uint32_t DDR_wr_QEC_enable= 0, DDR_wr_CMP_enable=0;
#define DDR_wr_QEC_enable 1
#define DDR_wr_CMP_enable 0
uint32_t DDR_wr_start_bit_update = 0, DDR_wr_load_start_bit_update = 0, DDR_wr_continuous = 0;
volatile uint32_t RX_ext_dma_enabled = 0;
uint32_t ddr_wr_dma_ch_nb = 0;
uint32_t ddr_wr_dma_ch_mask = 0;
static uint32_t host_flow_control_disable = 1;
/* Throttle counter for the AXIQ overrun/underrun check: the full SR+CR
 * sequence across 4 channels costs ~886 cy, which exceeds the 2560-cycle
 * AXIQ window at 61.44 MSPS.  Checking every 16 iterations amortises the
 * cost to ~55 cy while still detecting overruns within ~67 us. */
static uint32_t rxovr_check_throttle = 1;

uint32_t ddr_wr_dma_xfr_size = RX_DDR_STEP;

uint32_t dma_chan_mask(uint32_t dma_channel, uint32_t nb_dma) {
    uint32_t i, mask = 0;

    for (i = 0; i < nb_dma; i++) {
#pragma loop_count(1, 16, 2, 0)
        mask |= 1 << (dma_channel + i);
    }
    return mask;
}

void DDR_write_multi_dma(uint32_t DDR_wr_dma_channel, uint32_t nb_dma, uint32_t DDR_address, uint32_t vsp_address,
                         int32_t bytes_size) {
    uint32_t i;
    uint32_t size = bytes_size / nb_dma;

    // user should ensure bytes_size/nb_dma is enire and aligned on AXI bus width 16B.
    // 2048B is ok with 1,2,4 dmas only
    // dma channel should be contiguous

    for (i = 0; i < nb_dma; i++) {
#pragma loop_count(1, 16, 2, 0)
        uint32_t ctrl = DMAC_WRC | (DDR_wr_dma_channel + i);
        dmac_enable(ctrl, size, DDR_address + i * size, vsp_address + i * size);
    }
}

void rx_qec_correction(vspa_complex_fixed16 *dataIn, vspa_complex_fixed16 *dataOut) {
    //	if(!DDR_wr_QEC_enable)
    //		return;

#ifdef RXIQCOMP2
    txiqcomp_x32chf_5t((vspa_complex_fixed16 *)dataIn, (vspa_complex_fixed16 *)dataOut, &iq_comp_params2_rx, MEM_LINE_SIZE);
#else
#if defined(RXIQCOMP) && !defined(IQMOD_RX_NO_QEC)
    txiqcomp((vspa_complex_fixed16 *)dataIn, (vspa_complex_fixed16 *)dataOut, &rxiqcompcfg_struct, MEM_LINE_SIZE);
#endif
#endif
}

//__attribute__(( section(".text.opcode_6") ))
void RX_IQ_DATA_TO_DDR(void) {
    /* Use the globals decoded by main() -- do NOT call host_mbox0_read() again
     * here; main() already consumed the mailbox before dispatching to this
     * function so a second read would return 0 and cmd_start would be 0,
     * causing the STOP path to fire instead of START. */
    uint32_t cmd_start = mailbox_in_msg_0_MSB & 0x00100000;
    uint32_t DDR_wr_size;
    int i;

    /* Start while already streaming (for example after the host app was
     * killed without a stop): restart instead of NACKing. */
    if (cmd_start && DDR_wr_start_bit_update && !DDR_wr_load_start_bit_update) {
        DDR_wr_start_bit_update = 0;
        for (i = 0; i < RX_NUM_CHAN; i++) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            dmac_abort(0x1 << dma_channel_rd);
            dmac_clear_complete(0x1 << dma_channel_rd);
            axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index]);
        }
    }

    if ((cmd_start) && (!DDR_wr_start_bit_update)) {
        DDR_wr_start_bit_update = 1;
        DDR_wr_load_start_bit_update = mailbox_in_msg_0_MSB & 0x00200000;
        DDR_wr_continuous = mailbox_in_msg_0_MSB & 0x00800000;
        //	    DDR_wr_QEC_enable= mailbox_in_msg_0_MSB & 0x00400000;
        //	    DDR_wr_CMP_enable =  			mailbox_in_msg_0_MSB & 0x00080000;
        ddr_wr_dma_ch_nb = (mailbox_in_msg_0_MSB & 0x00070000) >> 16;
        host_flow_control_disable = mailbox_in_msg_0_MSB & 0x00400000;

        if (!DDR_wr_continuous)
            host_flow_control_disable = 0;

        if (ddr_wr_dma_ch_nb > 1)
            goto fail_rx_iq_data;
        if (!ddr_wr_dma_ch_nb) {
            // default 1 DMA write 526MB/s half duplex, 490MB/s full duplex
            ddr_wr_dma_ch_nb = 1;
        }
        DDR_wr_size = ((mailbox_in_msg_0_MSB & 0x0000FFFF) * SIZE_4K); // send chunks of 4KB can be sent
        ddr_wr_dma_xfr_size = RX_DDR_STEP;

        /* Drop any DDR write in flight or completed from a previous stream
         * so it is not counted as produced in the new one. */
        for (i = 0; i < RX_NUM_CHAN; i++) {
            uint32_t m = 0x1u << (DDR_WR_DMA_CHANNEL_1 + i);
            dmac_abort(m);
            dmac_clear_complete(m);
        }

        for (i = 0; i < RX_NUM_CHAN; i++) {
            rx_ch_context[i].RX_index = RX_index + i;
            rx_ch_context[i].p_rx_axiq_enqueued = &input_buffer[i][0];
            rx_ch_context[i].p_rx_axiq_received = &input_buffer[i][0];
            rx_ch_context[i].p_rx_dmem_QECed = &input_buffer[i][0];
            rx_ch_context[i].p_rx_dmem_input_decimated = &input_buffer[i][0];
            rx_ch_context[i].p_rx_dmem_output_decimated = &input_dec_buffer[i][0];
            rx_ch_context[i].p_rx_ddr_enqueued = &input_dec_buffer[i][0];
            rx_ch_context[i].p_rx_consumed = &input_dec_buffer[i][0];
            rx_ch_context[i].RX_total_axiq_enqueued_size = 0;
            rx_ch_context[i].RX_total_axiq_received_size = 0;
            rx_ch_context[i].RX_total_dmem_QECed_size = 0;
            rx_ch_context[i].RX_total_dmem_input_Decimated_size = 0;
            rx_ch_context[i].RX_total_decimated_size = 0;
            rx_vspa_proxy[i].la9310_fifo_produced_size = 0; 
            rx_ch_context[i].RX_total_ddr_enqueued_size = 0;
            rx_vspa_proxy[i].la9310_fifo_consumed_size = 0;
            rx_ch_context[i].DDR_wr_offset = 0;
            rx_vspa_proxy[i].DDR_wr_base_address = mailbox_in_msg_0_LSB + i * DDR_wr_size / RX_NUM_CHAN;
            rx_vspa_proxy[i].DDR_wr_size = DDR_wr_size / RX_NUM_CHAN;
        }

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

        // update host vspa_dmem_proxy
        rx_proxy_updated = 1;

        // clear decimation history buffers
        memclr((void *)filtState, sizeof(filtState));

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
        for (i = 0; i < RX_NUM_CHAN; i++) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            axi_rd = axi_ADC_FIFO_addr[i];
            dmac_reset(0x1 << dma_channel_rd);
            axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index]);
            stream_read_ptr_rst(dma_channel_rd, axi_rd);
            do {
#ifndef IQMOD_RX_0T1R
                PUSH_TX_DATA();
#endif
            } while (!dmac_is_complete(0x1 << dma_channel_rd));
            dmac_clear_complete(0x1 << dma_channel_rd);
        }

        if (DDR_wr_continuous) {
            // ack stream start
            mailbox_out_msg_0_MSB = 0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
        }

        for (i = 0; i < RX_NUM_CHAN; i++) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            axiq_fifo_rx_enable(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index]);
            axiq_fifo_rx_cr(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index], AXIQ_CR_CLRERR, AXIQ_CR_CLRERR);
            axiq_fifo_rx_cr(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index], AXIQ_CR_CLRERR, 0);
            dmac_clear_errxfr(0x1 << dma_channel_rd);
        }
        /* Prime the 2-deep AXIQ RX DMAC FIFO for all channels before returning.
         * Without initial stream_read() calls the pipeline never starts:
         * PUSH_RX_DATA() Loop 2 waits on dmac_is_available() which never fires
         * because no DMA was ever armed. */
        {
            uint32_t prime_busy;
        for (i = 0; i < RX_NUM_CHAN; i++) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            axi_rd = axi_ADC_FIFO_addr[i];
            while (dmac_is_available(0x1 << dma_channel_rd)) {
                prime_busy = rx_ch_context[i].RX_total_axiq_enqueued_size -
                             rx_ch_context[i].RX_total_dmem_input_Decimated_size;
                if ((RX_NUM_BUF * RX_DMA_TXR_STEP) - prime_busy < RX_DMA_TXR_STEP)
                    break;
                stream_read(dma_channel_rd, axi_rd,
                            2 * (uint32_t)(rx_ch_context[i].p_rx_axiq_enqueued));
                INCR_RX_BUFF(rx_ch_context[i].p_rx_axiq_enqueued, i);
                rx_ch_context[i].RX_total_axiq_enqueued_size += RX_DMA_TXR_STEP;
            }
        }
        }
#ifdef RX_STALL_STATS
        {
            uint32_t pd = 0;
            for (i = 0; i < RX_NUM_CHAN; i++) {
                uint32_t k;
                pd |= ((rx_ch_context[i].RX_total_axiq_enqueued_size / RX_DMA_TXR_STEP) & 0xffu) << (8 * i);
                /* per-run RX stats: clear all columns of this channel */
                for (k = 0; k < STATS_RX_MAX; k++)
                    g_stats.rx_stats[i][k] = 0;
            }
            (void)pd;
#ifdef RX_TXSTATS_FREE
            g_stats.tx_stats[5] = pd;
            g_stats.tx_stats[6] = 0;
#ifndef RX_STAGE_METER
            g_stats.tx_stats[4] = 0;
#endif
            rx_el_frac = 0;
            rx_el_last = (uint32_t)ccnt_read();
#if defined(RX_DDR_STATE_CMPL) && !defined(RX_STAGE_METER)
            for (i = 0; i < RX_NUM_CHAN; i++)
                g_stats.tx_stats[i] = 0;
#endif
#endif /* RX_TXSTATS_FREE */
        }
#endif
#ifdef RX_DDR_STATE_CMPL
        for (i = 0; i < RX_NUM_CHAN; i++)
            rx_ddr_cand[i] = 0;
#endif
        /* Avoid false RXUDR on the first PUSH_RX_DATA call: FIFO has just
         * been enabled so any stale underrun flag would be a false positive. */
        rxovr_check_throttle = 16;
#ifdef RX_CYCLE_METER
        /* restart cycle meter: 0 in unused[5] marks "no previous pass" */
        for (i = 0; i < 6; i++) g_stats.unused[i] = 0;
        g_stats.meter[METER_RX_BUDGET] = RX_PASS_BUDGET;
#if defined(IQMOD_RX_0T4R) && defined(RX_STAGE_METER)
        for (i = 0; i < 5; i++) g_stats.tx_stats[i] = 0;
#endif
#endif
        return;
    }

    if (cmd_start == 0) /* stop */
    {
        DDR_wr_start_bit_update = 0;
        DDR_wr_load_start_bit_update = 0;
        RX_SingleT_start_bit_update = 0;
        for (i = 0; i < RX_NUM_CHAN; i++) {
            rx_vspa_proxy[i].DDR_wr_base_address = 0xdeadbeef;
            rx_vspa_proxy[i].la9310_fifo_produced_size = 0;
            rx_vspa_proxy[i].la9310_fifo_consumed_size = 0;
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            dmac_abort(0x1 << dma_channel_rd);
            dmac_clear_complete(0x1 << dma_channel_rd);
            axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index]);
        }

        rx_proxy_updated = 1;
        mailbox_out_msg_0_MSB = 0;
        mailbox_out_msg_0_LSB = 0x1;
        host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));

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
#ifdef RX_FC_POLL
/* Set when some channel still has decimated data waiting for the FC gate or
 * a DDR DMA slot; main() then re-polls instead of entering __idle(). */
uint32_t rx_work_pending = 0;
#endif
void PUSH_RX_DATA(void) {
    uint32_t tmp_status, i;
    uint32_t tmp_dma_errors;
    uint32_t rx_empty_size;
    uint32_t all_chan_ddr_consumed_size = 0;
#ifdef RX_FC_POLL
    rx_work_pending = 0;
#endif

    /* Check all 4 AXIQ RX FIFOs for overrun/underrun every 16 iterations.
     * A single iord(AXIQ_SR0) covers all 4 channels; compile-time constant
     * masks avoid all runtime shift computation.
     * Normal-path cost: ~70 cy (1 iord + branch not taken).
     * Error-path cost: ~170 cy (+ per-channel increments + traces + 2 iowr). */
    if (--rxovr_check_throttle == 0) {
        rxovr_check_throttle = 16;
#if defined(RX_STALL_STATS) && defined(RX_TXSTATS_FREE)
        if (DDR_wr_start_bit_update) {
            uint32_t now = (uint32_t)ccnt_read();
            rx_el_frac += now - rx_el_last;
            rx_el_last = now;
            g_stats.tx_stats[6] += rx_el_frac >> 10;
            rx_el_frac &= 1023u;
        }
#endif
        tmp_status = iord(AXIQ_SR0);
        /* Build active-channel masks at compile time: 4-channel configs use the
         * existing _RX_ALL macros; 2-channel (1T2R) uses only RX0+RX1 bits. */
#if RX_NUM_CHAN > 2
#define AXIQ_SR_ERROVER_RX_ACTIVE   AXIQ_SR_FIELD_ERROVER_RX_ALL
#define AXIQ_SR_ERRUNDER_RX_ACTIVE  AXIQ_SR_FIELD_ERRUNDER_RX_ALL
#define AXIQ_CR_FIELD_CLRERR_RX_ACTIVE AXIQ_CR_FIELD_CLRERR_RX_ALL
#define AXIQ_CR_CLRERR_RX_ACTIVE    AXIQ_CR_CLRERR_RX_ALL
#else
#define AXIQ_SR_ERROVER_RX_ACTIVE  ((uint32_t)((AXIQ_SR_FIELD_ERROVER  << AXIQ_SR_SHIFT_RX0) | \
                                               (AXIQ_SR_FIELD_ERROVER  << AXIQ_SR_SHIFT_RX1)))
#define AXIQ_SR_ERRUNDER_RX_ACTIVE ((uint32_t)((AXIQ_SR_FIELD_ERRUNDER << AXIQ_SR_SHIFT_RX0) | \
                                               (AXIQ_SR_FIELD_ERRUNDER << AXIQ_SR_SHIFT_RX1)))
#define AXIQ_CR_FIELD_CLRERR_RX_ACTIVE ((uint32_t)((AXIQ_CR_FIELD_CLRERR << AXIQ_CR_SHIFT_RX0) | \
                                                   (AXIQ_CR_FIELD_CLRERR << AXIQ_CR_SHIFT_RX1)))
#define AXIQ_CR_CLRERR_RX_ACTIVE   ((uint32_t)((AXIQ_CR_CLRERR << AXIQ_CR_SHIFT_RX0) | \
                                               (AXIQ_CR_CLRERR << AXIQ_CR_SHIFT_RX1)))
#endif
        if (tmp_status & (AXIQ_SR_ERROVER_RX_ACTIVE | AXIQ_SR_ERRUNDER_RX_ACTIVE)) {
#define AXIQ_SR_OVR_RX0 ((uint32_t)(AXIQ_SR_FIELD_ERROVER  << AXIQ_SR_SHIFT_RX0))
#define AXIQ_SR_UDR_RX0 ((uint32_t)(AXIQ_SR_FIELD_ERRUNDER << AXIQ_SR_SHIFT_RX0))
#define AXIQ_SR_OVR_RX1 ((uint32_t)(AXIQ_SR_FIELD_ERROVER  << AXIQ_SR_SHIFT_RX1))
#define AXIQ_SR_UDR_RX1 ((uint32_t)(AXIQ_SR_FIELD_ERRUNDER << AXIQ_SR_SHIFT_RX1))
#define AXIQ_SR_OVR_RX2 ((uint32_t)(AXIQ_SR_FIELD_ERROVER  << AXIQ_SR_SHIFT_RX2))
#define AXIQ_SR_UDR_RX2 ((uint32_t)(AXIQ_SR_FIELD_ERRUNDER << AXIQ_SR_SHIFT_RX2))
#define AXIQ_SR_OVR_RX3 ((uint32_t)(AXIQ_SR_FIELD_ERROVER  << AXIQ_SR_SHIFT_RX3))
#define AXIQ_SR_UDR_RX3 ((uint32_t)(AXIQ_SR_FIELD_ERRUNDER << AXIQ_SR_SHIFT_RX3))
#define CHK_CH(ch, OVR, UDR)                                                        \
            if (tmp_status & (OVR)) {                                                \
                g_stats.rx_stats[ch][ERROR_AXIQ_FIFO_RX_OVERRUN]++;                 \
                l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_OVER,                             \
                         (uint32_t)g_stats.rx_stats[ch][ERROR_AXIQ_FIFO_RX_OVERRUN]); \
            }                                                                        \
            if (tmp_status & (UDR)) {                                                \
                g_stats.rx_stats[ch][ERROR_AXIQ_FIFO_RX_UNDERRUN]++;                \
                l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_UNDER,                            \
                         (uint32_t)g_stats.rx_stats[ch][ERROR_AXIQ_FIFO_RX_UNDERRUN]); \
            }
#if RX_NUM_CHAN > 2
            /* Report per antenna (stat column), not per hardware FIFO:
             * Rx_Antenna2fifo_index = {RX1, RX3, RX0, RX2}. */
            CHK_CH(2, AXIQ_SR_OVR_RX0, AXIQ_SR_UDR_RX0)
            CHK_CH(0, AXIQ_SR_OVR_RX1, AXIQ_SR_UDR_RX1)
            CHK_CH(3, AXIQ_SR_OVR_RX2, AXIQ_SR_UDR_RX2)
            CHK_CH(1, AXIQ_SR_OVR_RX3, AXIQ_SR_UDR_RX3)
#else
            CHK_CH(0, AXIQ_SR_OVR_RX0, AXIQ_SR_UDR_RX0)
            CHK_CH(1, AXIQ_SR_OVR_RX1, AXIQ_SR_UDR_RX1)
#endif
#undef CHK_CH
#undef AXIQ_SR_OVR_RX0
#undef AXIQ_SR_UDR_RX0
#undef AXIQ_SR_OVR_RX1
#undef AXIQ_SR_UDR_RX1
#undef AXIQ_SR_OVR_RX2
#undef AXIQ_SR_UDR_RX2
#undef AXIQ_SR_OVR_RX3
#undef AXIQ_SR_UDR_RX3
            /* Clear active RX FIFO errors with 2 register writes. */
            iowr(AXIQ_CR0, AXIQ_CR_FIELD_CLRERR_RX_ACTIVE, AXIQ_CR_CLRERR_RX_ACTIVE);
            iowr(AXIQ_CR0, AXIQ_CR_FIELD_CLRERR_RX_ACTIVE, 0);
#if defined(RX_STALL_STATS) && defined(RX_TXSTATS_FREE) && !defined(RX_STAGE_METER)
            /* still set right after the clear -> clear sequence not effective */
            if (iord(AXIQ_SR0) & AXIQ_SR_ERROVER_RX_ACTIVE)
                g_stats.tx_stats[4]++;
#endif
        }
#undef AXIQ_SR_ERROVER_RX_ACTIVE
#undef AXIQ_SR_ERRUNDER_RX_ACTIVE
#undef AXIQ_CR_FIELD_CLRERR_RX_ACTIVE
#undef AXIQ_CR_CLRERR_RX_ACTIVE
    }

    // DDR DMA load test , just fetch DDR as fast as possible
    if (DDR_wr_load_start_bit_update) {
        if (!RX_ext_dma_enabled) {
            for (i = 0; i < RX_NUM_CHAN; i++) {
                ddr_wr_dma_ch_mask = dma_chan_mask(DDR_WR_DMA_CHANNEL_1 + i, 1);
                // check DDR dma completion
                if (dmac_is_complete(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
                    dmac_clear_complete(ddr_wr_dma_ch_mask);
                    g_stats.rx_stats[i][STAT_DMA_DDR_WR]++;
                    l1_trace(L1_TRACE_MSG_DMA_DDR_WR_COMP, (uint32_t)g_stats.rx_stats[i][STAT_DMA_DDR_WR]);
                }
                // restart DDR dma if possible
                rx_ch_context[i].p_rx_ddr_enqueued = input_buffer[i];
                if (dmac_is_available(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {

                    DDR_write_multi_dma(DDR_WR_DMA_CHANNEL_1 + i, 1,
                                        rx_vspa_proxy[i].DDR_wr_base_address + rx_ch_context[i].DDR_wr_offset,
                                        2 * (uint32_t)rx_ch_context[i].p_rx_ddr_enqueued, ddr_wr_dma_xfr_size);
                    rx_ch_context[i].DDR_wr_offset =
                        (rx_ch_context[i].DDR_wr_offset + ddr_wr_dma_xfr_size) % (rx_vspa_proxy[i].DDR_wr_size);
                    l1_trace(L1_TRACE_MSG_DMA_DDR_WR_START, (uint32_t)rx_ch_context[i].p_rx_ddr_enqueued);
                }
            }
        }
    }

    // Single Tone
    if (RX_SingleT_start_bit_update) {
        for (i = 0; i < RX_NUM_CHAN; i++) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            axi_rd = axi_ADC_FIFO_addr[i];
            // check axiq dma completion (received new samples) and QEC them
            if (dmac_is_complete(0x1 << dma_channel_rd)) {
                dmac_clear_complete(0x1 << dma_channel_rd);
                dmac_clear_event(0x1 << dma_channel_rd);
                g_stats.rx_stats[i][STAT_DMA_AXIQ_READ]++;
                l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_COMP, (uint32_t)g_stats.rx_stats[i][STAT_DMA_AXIQ_READ]);
            }
            // restart axiq  dma if possible
            if (dmac_is_available(0x1 << dma_channel_rd)) {
                stream_read(dma_channel_rd, axi_rd, 2 * (uint32_t)(RX_SingleT_buffer + i * RX_DMA_TXR_size));
                l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_START, (uint32_t)RX_SingleT_buffer + i * RX_DMA_TXR_size);
            }
        }
    }

    // if any of previous load test is on, skip DDR streaming
    if (DDR_wr_load_start_bit_update || RX_SingleT_start_bit_update)
        goto end_rx_push;

    // Stream waveform to DDR
    if (DDR_wr_start_bit_update) {
#ifdef RX_CYCLE_METER
        uint32_t cm_t0 = (uint32_t)ccnt_read();
        uint32_t cm_ts = cm_t0;
        {
            /* unused[5] holds the previous pass start (no extra .data);
             * 0 means first pass after start, skip the gap. */
            uint32_t gap = cm_t0 - g_stats.unused[5];
            if (g_stats.unused[5] != 0 && gap < 0x00100000u) {
                if (gap > g_stats.unused[4]) g_stats.unused[4] = gap;
#if defined(IQMOD_RX_0T4R) && defined(RX_STAGE_METER)
                g_stats.tx_stats[4] += gap >> 4;
#endif
            }
            g_stats.unused[5] = cm_t0 | 1u;
        }
#endif

        /* Loop 1: process completions and errors for all channels first.
         * All dmac_clear_complete() calls complete here, giving the hardware
         * time to propagate freed FIFO slots into DMA_FIFO_STAT before the
         * re-arm loop reads dmac_is_available().  The fused single-loop
         * approach caused DMA_RX_CMD_OVR on ch2/ch3 because clear_complete
         * and is_available were back-to-back with no intervening latency. */
        RX_FOR_EACH_CHAN(i) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            uint32_t ch_mask = 0x1u << dma_channel_rd;
            if (dmac_is_complete(ch_mask)) {
                dmac_clear_complete(ch_mask);
                dmac_clear_event(ch_mask);
                rx_ch_context[i].RX_total_axiq_received_size += RX_DMA_TXR_STEP;
                g_stats.rx_stats[i][STAT_DMA_AXIQ_READ]++;
#ifndef IQMOD_RX_NO_QEC
                RX_TRACE(L1_TRACE_MSG_DMA_AXIQ_RX_COMP, (uint32_t)g_stats.rx_stats[i][STAT_DMA_AXIQ_READ]);
#endif
            }
            if (dmac_errxfr(ch_mask)) {
                dmac_clear_errxfr(ch_mask);
                g_stats.rx_stats[i][ERROR_DMA_XFER_ERROR]++;
                l1_trace(L1_TRACE_MSG_DMA_AXIQ_RX_XFER_ERROR, (uint32_t)g_stats.rx_stats[i][ERROR_DMA_XFER_ERROR]);
            }
        }
        /* Loop 2: re-arm channels.  By the time we reach ch2/ch3 here, the
         * hardware has had sufficient cycles (loop 1 body for ch0/ch1/ch2/ch3
         * plus loop overhead) to reflect freed FIFO slots in DMA_FIFO_STAT. */
        RX_FOR_EACH_CHAN(i) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            axi_rd = axi_ADC_FIFO_addr[i];
            uint32_t ch_mask = 0x1u << dma_channel_rd;
            if (dmac_is_available(ch_mask)) {
                rx_busy_size = rx_ch_context[i].RX_total_axiq_enqueued_size - rx_ch_context[i].RX_total_dmem_input_Decimated_size;
                rx_empty_size = (RX_NUM_BUF * RX_DMA_TXR_STEP) - rx_busy_size;
                if (rx_empty_size >= RX_DMA_TXR_STEP) {
                    stream_read(dma_channel_rd, axi_rd, 2 * (uint32_t)(rx_ch_context[i].p_rx_axiq_enqueued));
                    INCR_RX_BUFF(rx_ch_context[i].p_rx_axiq_enqueued, i);
                    rx_ch_context[i].RX_total_axiq_enqueued_size += RX_DMA_TXR_STEP;
#ifndef IQMOD_RX_NO_QEC
                    RX_TRACE(L1_TRACE_MSG_DMA_AXIQ_RX_START, (uint32_t)rx_ch_context[i].p_rx_axiq_enqueued);
#endif
                } else {
                    /* No DMEM buffer free: FIFO will overrun. */
                    g_stats.rx_stats[i][ERROR_DMA_DDR_WR_OVERRUN]++;
                    RX_TRACE(L1_TRACE_MSG_DMA_DDR_WR_OVERRUN, (uint32_t)rx_empty_size);
                    //l1_trace_disable=1;
                }
            }
        }

        RX_STAGE(0);
#ifndef IQMOD_RX_NO_QEC
        /* QEC stage: correct IQ imbalance per channel before decimation. */
        RX_FOR_EACH_CHAN(i) {
            if ((rx_ch_context[i].RX_total_axiq_received_size - rx_ch_context[i].RX_total_dmem_QECed_size) >= RX_DMA_TXR_STEP) {
                RX_TRACE(L1_TRACE_L1APP_RX_QEC_START, (uint32_t)rx_ch_context[i].p_rx_dmem_QECed);
                rx_qec_correction((vspa_complex_fixed16 *)rx_ch_context[i].p_rx_dmem_QECed,
                                  (vspa_complex_fixed16 *)rx_ch_context[i].p_rx_dmem_QECed);
                INCR_RX_BUFF(rx_ch_context[i].p_rx_dmem_QECed, i);
                rx_ch_context[i].RX_total_dmem_QECed_size += RX_DMA_TXR_STEP;
                RX_TRACE(L1_TRACE_L1APP_RX_QEC_COMP, (uint32_t)rx_ch_context[i].RX_total_dmem_QECed_size);
            }
        }
#endif /* !IQMOD_RX_NO_QEC */

        RX_STAGE(1);
        /* Decimation: when IQMOD_RX_NO_QEC the QEC stage is absent so gate
         * directly on AXIQ received count; otherwise gate on QEC output. */
        RX_FOR_EACH_CHAN(i) {
#ifdef IQMOD_RX_NO_QEC
            if ((rx_ch_context[i].RX_total_axiq_received_size - rx_ch_context[i].RX_total_dmem_input_Decimated_size) >=
#else
            if ((rx_ch_context[i].RX_total_dmem_QECed_size - rx_ch_context[i].RX_total_dmem_input_Decimated_size) >=
#endif
                RX_DMA_TXR_STEP) {
                /* Back-pressure: do not overwrite dec-ring slots that the DDR
                 * DMA has not yet consumed.  When the DDR FC gate is closed
                 * (host drain too slow) dec_backlog grows; once it reaches
                 * (RX_NUM_DEC_BUF-1)*RX_DDR_STEP stall decimation so the
                 * DMEM AXIQ ring fills up and the AXIQ re-arm stops firing.
                 * This propagates back-pressure all the way to the AXIQ FIFO
                 * instead of silently losing samples in the dec ring. */
                {
                    uint32_t dec_backlog = rx_ch_context[i].RX_total_decimated_size -
                                          rx_ch_context[i].RX_total_ddr_enqueued_size;
                    if (dec_backlog >= (uint32_t)((RX_NUM_DEC_BUF - 1u) * RX_DDR_STEP)) {
                        RX_STALL(i, ERROR_EXT_DMA_DDR_WR_OVERRUN);
                        continue;
                    }
                }
                /* Gate decimation only on local DMEM dec-ring capacity (dec_backlog
                 * guard above already ensures a free slot exists).  Do NOT gate on
                 * the host-managed la9310_fifo_consumed_size here: if the host is
                 * briefly slow to call rx_fc_notify(), the DDR ring fills and this
                 * gate would stall decimation, which then stalls the AXIQ re-arm
                 * and causes AXIQ HW FIFO overrun (ERROR_AXIQ_FIFO_RX_OVERRUN).
                 * The DDR ring fullness check belongs in the DDR DMA enqueue path
                 * below, not in the decimation path. */
#ifndef IQMOD_RX_NO_QEC
                RX_TRACE(L1_TRACE_L1APP_RX_DEC_START, (uint32_t)rx_ch_context[i].p_rx_dmem_input_decimated);
#endif
#if defined(IQMOD_RX_1T2R) || defined(IQMOD_RX_1T4R) || defined(IQMOD_RX_0T4R)
                decimator_2x_8_Taps_asm((cfixed16_t *)rx_ch_context[i].p_rx_dmem_output_decimated,
                                        (cfixed16_t *)rx_ch_context[i].p_rx_dmem_input_decimated,
                                        (float32_t *)filter_taps_downsampling, (cfixed16_t *)filtState[i], RX_DMA_TXR_size);
#endif
                INCR_RX_BUFF(rx_ch_context[i].p_rx_dmem_input_decimated, i);
                INCR_RX_DEC_BUFF(rx_ch_context[i].p_rx_dmem_output_decimated, i);
                rx_ch_context[i].RX_total_dmem_input_Decimated_size += RX_DMA_TXR_STEP;
                // rx_ch_context[i].RX_total_dmem_output_Decimated_size += RX_DDR_STEP;
                rx_ch_context[i].RX_total_decimated_size += RX_DDR_STEP;
                rx_proxy_updated = 1;
#ifndef IQMOD_RX_NO_QEC
                RX_TRACE(L1_TRACE_L1APP_RX_DEC_COMP, (uint32_t)rx_ch_context[i].RX_total_dmem_input_Decimated_size);
#endif
            }
        }

        RX_STAGE(2);
        // Push data to DDR if standalone mode
        if (!RX_ext_dma_enabled) {
            RX_FOR_EACH_CHAN(i) {
                // check DDR dma completion
                ddr_wr_dma_ch_mask = dma_chan_mask(DDR_WR_DMA_CHANNEL_1 + i, 1);
#ifdef RX_DDR_STATE_CMPL
                uint32_t ddr_n = 0;
                if (rx_ch_context[i].RX_total_ddr_enqueued_size != rx_vspa_proxy[i].la9310_fifo_produced_size) {
                    uint32_t r1, f, n, cand, prev;
#ifndef RX_DDR_STATUS_FULL
                    /* Default: 2 status reads, no completion read.
                     * XRUN is read before FIFO_STAT; VSPA is the only one
                     * that starts writes, so a transition between the two
                     * reads can only make n too high (conservative). */
                    dmac_clear_complete(ddr_wr_dma_ch_mask);
                    r1 = dmac_is_running(ddr_wr_dma_ch_mask);
                    f  = dmac_is_available(ddr_wr_dma_ch_mask);
                    n = !f ? 2u : (r1 ? 1u : 0u);
#else
                    uint32_t r2;
                    if (dmac_is_complete(ddr_wr_dma_ch_mask))
                        dmac_clear_complete(ddr_wr_dma_ch_mask);
                    r1 = dmac_is_running(ddr_wr_dma_ch_mask);
                    f  = dmac_is_available(ddr_wr_dma_ch_mask);
                    r2 = dmac_is_running(ddr_wr_dma_ch_mask);
                    n = !f ? 2u : ((r1 | r2) ? 1u : 0u);
#endif
                    ddr_n = n;
                    cand = rx_ch_context[i].RX_total_ddr_enqueued_size - n * RX_DDR_STEP;
                    /* glitch filter: use min(this pass, previous pass) */
                    prev = rx_ddr_cand[i];
                    rx_ddr_cand[i] = cand;
                    if ((int32_t)(prev - cand) < 0)
                        cand = prev;
                    if ((int32_t)(cand - rx_vspa_proxy[i].la9310_fifo_produced_size) > 0) {
                        uint32_t d = (cand - rx_vspa_proxy[i].la9310_fifo_produced_size) / RX_DDR_STEP;
                        rx_vspa_proxy[i].la9310_fifo_produced_size = cand;
                        g_stats.rx_stats[i][STAT_DMA_DDR_WR] += d;
                        rx_proxy_updated = 1;
#if defined(RX_STALL_STATS) && defined(RX_TXSTATS_FREE) && !defined(RX_STAGE_METER)
                        if (d > 2)
                            g_stats.tx_stats[i]++;
#endif
                    }
                }
#else
                if (dmac_is_complete(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
                    dmac_clear_complete(ddr_wr_dma_ch_mask);
                    rx_vspa_proxy[i].la9310_fifo_produced_size += RX_DDR_STEP;
#ifdef RX_FC_POLL
                    /* refresh the host mirror even if decimation is stalled */
                    rx_proxy_updated = 1;
#endif
                    g_stats.rx_stats[i][STAT_DMA_DDR_WR]++;
#ifndef IQMOD_RX_NO_QEC
                    RX_TRACE(L1_TRACE_MSG_DMA_DDR_WR_COMP, (uint32_t)g_stats.rx_stats[i][STAT_DMA_DDR_WR]);
#endif
                }
#endif /* RX_DDR_STATE_CMPL */

#ifdef RX_FC_POLL
                if ((rx_ch_context[i].RX_total_decimated_size - rx_ch_context[i].RX_total_ddr_enqueued_size) >= RX_DDR_STEP)
                    rx_work_pending = 1;
#endif
                // host flow control: block DDR DMA if host is less than RX_DDR_STEP behind ring boundary
                if (host_flow_control_disable ||
                    (rx_ch_context[i].RX_total_ddr_enqueued_size - rx_vspa_proxy[i].la9310_fifo_consumed_size + RX_DDR_STEP <=
                     rx_vspa_proxy[i].DDR_wr_size)) {
                    // if ((rx_ch_context[i].RX_total_dmem_output_Decimated_size-rx_ch_context[i].RX_total_ddr_enqueued_size) >=
                    // RX_DDR_STEP)
                    if ((rx_ch_context[i].RX_total_decimated_size - rx_ch_context[i].RX_total_ddr_enqueued_size) >= RX_DDR_STEP) {
#ifdef RX_DDR_STATE_CMPL
                        if (ddr_n < RX_DDR_INFLIGHT &&
                            dmac_is_available(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
#else
                        if (dmac_is_available(ddr_wr_dma_ch_mask) == ddr_wr_dma_ch_mask) {
#endif
                            DDR_write_multi_dma(DDR_WR_DMA_CHANNEL_1 + i, 1,
                                                rx_vspa_proxy[i].DDR_wr_base_address + rx_ch_context[i].DDR_wr_offset,
                                                2 * (uint32_t)rx_ch_context[i].p_rx_ddr_enqueued, ddr_wr_dma_xfr_size);
                            INCR_RX_DEC_BUFF(rx_ch_context[i].p_rx_ddr_enqueued, i);
                            rx_ch_context[i].RX_total_ddr_enqueued_size += RX_DDR_STEP;
                            rx_ch_context[i].DDR_wr_offset += ddr_wr_dma_xfr_size;
                            if (rx_ch_context[i].DDR_wr_offset >= rx_vspa_proxy[i].DDR_wr_size)
                                rx_ch_context[i].DDR_wr_offset = 0;
#ifndef IQMOD_RX_NO_QEC
                            RX_TRACE(L1_TRACE_MSG_DMA_DDR_WR_START, (uint32_t)rx_ch_context[i].p_rx_ddr_enqueued);
#endif
                        } else {
                            RX_STALL(i, STAT_EXT_DMA_DDR_WR);
                        }
                    }
                }
#ifdef RX_STALL_STATS
                else if ((rx_ch_context[i].RX_total_decimated_size - rx_ch_context[i].RX_total_ddr_enqueued_size) >= RX_DDR_STEP) {
                    RX_STALL(i, STAT_APP_DDR_WR);
                }
#endif
            }
        }

#ifndef IQMOD_RX_NO_QEC
        /* Late re-arm: with QEC the correction loop (~1500 cy) may allow a
         * second DMA slot to open up after the early re-arm.  Not needed in
         * the NOQEC path where the early re-arm above is sufficient, and
         * enabling it there causes a double-arm race that overflows the
         * input_buffer ring and triggers DDR_WR_OVERRUN -> RXOVR. */
        RX_FOR_EACH_CHAN(i) {
            dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
            if (dmac_is_available(0x1 << dma_channel_rd)) {
                axi_rd = axi_ADC_FIFO_addr[i];
                rx_busy_size = rx_ch_context[i].RX_total_axiq_enqueued_size - rx_ch_context[i].RX_total_dmem_input_Decimated_size;
                rx_empty_size = (RX_NUM_BUF * RX_DMA_TXR_STEP) - rx_busy_size;
                if (rx_empty_size >= RX_DMA_TXR_STEP) {
                    stream_read(dma_channel_rd, axi_rd, 2 * (uint32_t)(rx_ch_context[i].p_rx_axiq_enqueued));
                    INCR_RX_BUFF(rx_ch_context[i].p_rx_axiq_enqueued, i);
                    rx_ch_context[i].RX_total_axiq_enqueued_size += RX_DMA_TXR_STEP;
                    RX_TRACE(L1_TRACE_MSG_DMA_AXIQ_RX_START, (uint32_t)rx_ch_context[i].p_rx_axiq_enqueued);
                } else {
                    /* No DMEM buffer free after QEC+decimation: FIFO will overrun. */
                    g_stats.rx_stats[i][ERROR_DMA_DDR_WR_OVERRUN]++;
                    RX_TRACE(L1_TRACE_MSG_DMA_DDR_WR_OVERRUN, (uint32_t)rx_empty_size);
                    //l1_trace_disable=1;
                }
            }
        }
#endif /* !IQMOD_RX_NO_QEC */

        // end reception
        all_chan_ddr_consumed_size = 0;
        for (i = 0; i < RX_NUM_CHAN; i++) {
            if (rx_vspa_proxy[i].la9310_fifo_consumed_size >= rx_vspa_proxy[i].DDR_wr_size)
                all_chan_ddr_consumed_size++;
        }
        if ((all_chan_ddr_consumed_size >= RX_NUM_CHAN) && (!DDR_wr_continuous)) {
            DDR_wr_start_bit_update = 0;
            for (i = 0; i < RX_NUM_CHAN; i++) {
                rx_vspa_proxy[i].DDR_wr_base_address = 0xdeadbeef;
                dma_channel_rd = Rx_Antenna2axiq_dma_chan[i + RX_index];
                axi_rd = axi_ADC_FIFO_addr[i];
                dmac_abort(0x1 << dma_channel_rd);
                dmac_clear_complete(0x1 << dma_channel_rd);
                axiq_fifo_rx_disable(AXIQ_BANK_0, Rx_Antenna2fifo_index[rx_ch_context[i].RX_index]);
            }
            rx_proxy_updated = 1;
            mailbox_out_msg_0_MSB = 0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
        }
#ifdef RX_CYCLE_METER
        {
            RX_STAGE(3);
            meter_end(&g_stats.unused[0], cm_t0, RX_PASS_BUDGET);
        }
#endif
    }

end_rx_push:
    /* Check DMA errors*/
    tmp_dma_errors = (uint32_t)iord(DMA_CFGERR_STAT);
    if (tmp_dma_errors != 0) {
        {
            uint32_t err_ch = (i < RX_NUM_CHAN) ? i : (RX_NUM_CHAN - 1u);
            l1_trace(L1_TRACE_MSG_DMA_CFGERR, (uint32_t)(g_stats.rx_stats[err_ch][ERROR_DMA_CONFIG_ERROR]));
            g_stats.rx_stats[err_ch][ERROR_DMA_CONFIG_ERROR]++;
        }
        dmac_clear_errcfg();
        #ifdef DBG_SPIN_ON
        while (dbg_gbl == 6) {
        };
        #endif
    };

    // update host proxy if needed
    VSPA_PROXY_update();
}
#pragma optimize_for_size reset
