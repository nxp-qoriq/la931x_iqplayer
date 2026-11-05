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
#include "iqmod_tx.h"
#include "main.h"
#include "dfe.h"
#include "l1-trace.h"
#include "iqmod_rx.h"
#include "cal_signal.h"
#include "stats.h"
#include "vspa_dmem_proxy.h"
#include "ccnt.h"

/* TX cycle meter: measures each streaming PUSH_TX_DATA() pass with ccnt.
 * g_stats.meter[0..5] = max pass, pass count, passes > TX_PASS_BUDGET,
 * sum of pass cycles >> 4, max start-to-start gap, last start (|1).
 * Build with -DTX_CYCLE_METER_OFF to remove it. */
#ifndef TX_CYCLE_METER_OFF
#define TX_CYCLE_METER 1
#endif

#ifndef TX_PASS_BUDGET
#define TX_PASS_BUDGET 2560u
#endif

/* TX DMA completion accounting: default derives completed DDR reads and
 * AXIQ writes from the DMA state (see dma_done_from_state() in _main.c).
 * -DTX_DDR_LEGACY_CMPL restores the completion-bit counting, which loses
 * buffers when two transfers complete between two polls. */
#ifndef TX_DDR_LEGACY_CMPL
#define TX_DDR_STATE_CMPL 1
static uint32_t tx_ddr_cand = 0, tx_axiq_cand = 0;
#endif

/* Per-buffer TX traces are only kept with -DTX_HOT_TRACE. */
#ifdef TX_HOT_TRACE
#define TX_TRACE(m, p) l1_trace((m), (p))
#else
#define TX_TRACE(m, p) do { } while (0)
#endif

vspa_complex_fixed16 output_buffer[TX_NUM_BUF * TX_DMA_TXR_size] __attribute__((section(".vcpu_dmem")))
__attribute__((aligned(64))) = { 0x00000000, 0x00010001, 0x00020002, 0x00030003 };

/* DDR read load test (start bit 0x00200000) is only built with
 * -DTX_DDR_LOAD_TEST, to save PRAM. Without it the request is NACKed. */
uint32_t DDR_rd_load_start_bit_update = 0; /* stays 0, read by signal.c */
#ifndef TX_DDR_LOAD_TEST
#define DDR_rd_load_start_bit_update 0
#endif
uint32_t DDR_rd_start_bit_update = 0;
#define DDR_rd_QEC_enable 1
uint32_t ddr_rd_dma_xfr_size = TX_DDR_STEP;

volatile uint32_t TX_ext_dma_enabled = 0;
static uint32_t ddr_rd_dma_ch_nb = 0;
static uint32_t ddr_rd_dma_ch_mask = 0;
static uint32_t ddr_rd_dma_mBurst = 1;
static uint32_t host_flow_control_disable = 1;

#define DDR_rd_base_address (tx_vspa_proxy.DDR_rd_base_address)
#define DDR_rd_size (tx_vspa_proxy.DDR_rd_size)

#define TX_total_ddr_ready_size (tx_vspa_proxy.host_produced_size)            /* -1: DDR data ready */
static uint32_t TX_total_ddr_enqueued_size = 0;                               /* 0: DDR dma fifo cmd 		*/
#define TX_total_ddr_fetched_size (tx_vspa_proxy.la9310_fifo_enqueued_size)   /* 1: DDR fetched	   		*/
static uint32_t TX_total_dmem_QECced_size = 0;                                /* 2: Tx  data QECed	   	*/
static uint32_t TX_total_axiq_enqueued_size = 0;                              /* 3: Axiq Tx in fifo cmd	*/
#define TX_total_axiq_consumed_size (tx_vspa_proxy.la9310_fifo_consumed_size) /* 4: Axiq Tx completed		*/

vspa_complex_fixed16 *p_tx_ddr_enqueued = &output_buffer[0];
vspa_complex_fixed16 *p_tx_ddr_fetched = &output_buffer[0];
vspa_complex_fixed16 *p_tx_dmem_QECed = &output_buffer[0];
vspa_complex_fixed16 *p_tx_axiq_enqueued = &output_buffer[0];
vspa_complex_fixed16 *p_tx_axiq_consumed = &output_buffer[0];

// uint32_t  DDR_rd_base_address = 0xdeadbeef; // ( export to external host )
// uint32_t  DDR_rd_size;

uint32_t rd_ddr_src_ptr = 0xcafedeca;
uint32_t DDR_rd_counter;

void DDR_read_multi_dma(uint32_t DDR_rd_dma_channel, uint32_t nb_dma, uint32_t DDR_address, uint32_t vsp_address,
                        int32_t bytes_size) {
    uint32_t i;
    uint32_t size = bytes_size / nb_dma;

    // user should ensure bytes_size/nb_dma is enire and aligned on AXI bus width 16B.
    // 2048B is ok with 1,2,4 dmas only
    // dma channel should be contiguous

    for (i = 0; i < nb_dma; i++) {
#pragma loop_count(1, 16, 2, 0)
        uint32_t ctrl = DMAC_RDC | (DDR_rd_dma_channel + i);
        if (ddr_rd_dma_mBurst) {
            ctrl |= DMAC_MBRE;
        }
        /* Wake VCPU on completion of the last channel so TX AXIQ is refilled in time */
        if (i == nb_dma - 1) {
            ctrl |= DMAC_TRIG_VCPU;
        }
        dmac_enable(ctrl, size, DDR_address + i * size, vsp_address + i * size);
    }
}

static uint32_t dma_chan_mask(uint32_t dma_channel, uint32_t nb_dma) {
    uint32_t i, mask = 0;

    for (i = 0; i < nb_dma; i++) {
#pragma loop_count(1, 16, 2, 0)
        mask |= 1 << (dma_channel + i);
    }
    return mask;
}

void tx_qec_correction(vspa_complex_fixed16 *dataIn, vspa_complex_fixed16 *dataOut) {
    if (!DDR_rd_QEC_enable)
        return;

#ifdef TXIQCOMP2
    txiqcomp_x32chf_5t((vspa_complex_fixed16 *)dataIn, (vspa_complex_fixed16 *)dataOut, &iq_comp_params2_tx, MEM_LINE_SIZE);
#else
#ifdef TXIQCOMP
    txiqcomp((vspa_complex_fixed16 *)dataIn, (vspa_complex_fixed16 *)dataOut, &txiqcompcfg_struct, MEM_LINE_SIZE);
#endif
#endif
}

uint32_t rd_dmem_dst_byte_ptr;

//__attribute__(( section(".text.opcode_5") ))
void TX_IQ_DATA_FROM_DDR(void) {
    uint64_t msg64 = host_mbox0_read();
    uint32_t cmd_start = (HIWORD(msg64)) & 0x00100000;

    if ((cmd_start) && (!DDR_rd_start_bit_update)) {
        DDR_rd_start_bit_update = 1;
#ifdef TX_CYCLE_METER
        {
            uint32_t k;
            for (k = 0; k < 6; k++) g_stats.meter[METER_TX_BASE + k] = 0;
            g_stats.meter[METER_TX_BUDGET] = TX_PASS_BUDGET;
        }
#endif
#ifdef TX_DDR_LOAD_TEST
        DDR_rd_load_start_bit_update = (HIWORD(msg64)) & 0x00200000;
#else
        if ((HIWORD(msg64)) & 0x00200000) {
            DDR_rd_start_bit_update = 0;
            goto fail_tx_iq_data;
        }
#endif
        // DDR_rd_QEC_enable= (HIWORD(msg64)) & 0x00400000;
        ddr_rd_dma_ch_nb = ((HIWORD(msg64)) & 0x00070000) >> 16;
        ddr_rd_dma_mBurst = ((HIWORD(msg64)) & 0x00080000 ? 1 : 0);
        host_flow_control_disable = (HIWORD(msg64)) & 0x00400000;
        if (ddr_rd_dma_ch_nb > 4)
            goto fail_tx_iq_data;
        if (!ddr_rd_dma_ch_nb) {
            /* LA9310 AXI bus supports 4 opened RD transactions
             * Read measurements ( wo/ multi-burst):
             *  2x VSPA DMA read	DDR	393MB/s       <-- default
             *  4x VSPA DMA read	DDR	590MB/s
             * Read measurements ( w/ multi-burst):
             *  1x VSPA DMA read	DDR	480MB/s
             *  2x VSPA DMA read	DDR	??MB/s
             *  4x VSPA DMA read	DDR	825MB/s
             * 4 channels will cause AXIQ FIFO read starvation if rx is also running
             */
            /* Full-duplex safe default: 1 channel + multi-burst.
             * Measured on HW: 2ch causes FIFO_RX_OVR in 1T2R when
             * iq-capture runs; 1ch/mBurst avoids AXI read contention.
             */
            ddr_rd_dma_mBurst = 1;
            ddr_rd_dma_ch_nb = 1;
        }

        ddr_rd_dma_ch_mask = dma_chan_mask(DDR_RD_DMA_CHANNEL_1, ddr_rd_dma_ch_nb);

        DDR_rd_counter = 0;
        ddr_rd_dma_xfr_size = TX_DDR_STEP;

        if (0 == TX_total_ddr_ready_size) {
            TX_total_ddr_ready_size = TX_NUM_BUF * TX_DDR_STEP;
        }
        TX_total_ddr_enqueued_size = 0;
        TX_total_ddr_fetched_size = 0;
        TX_total_dmem_QECced_size = 0;
        TX_total_axiq_enqueued_size = 0;
        TX_total_axiq_consumed_size = 0;
#ifdef TX_DDR_STATE_CMPL
        tx_ddr_cand = 0;
        tx_axiq_cand = 0;
#endif

        p_tx_ddr_enqueued = &output_buffer[0];
        p_tx_ddr_fetched = &output_buffer[0];
        p_tx_dmem_QECed = &output_buffer[0];
        p_tx_axiq_enqueued = &output_buffer[0];
        p_tx_axiq_consumed = &output_buffer[0];

        DDR_rd_size = ((mailbox_in_msg_0_MSB & 0x0000FFFF) * SIZE_4K);
        DDR_rd_base_address = mailbox_in_msg_0_LSB;

        /* Push the reset TX proxy (enqueued/consumed = 0) to the host DDR
         * mirror synchronously, before the mailbox ACK. Otherwise the host
         * iq_player_init_tx() may read stale counters from a previous run
         * and report a false TX underrun on restart. */
        tx_proxy_updated = 0;
        DDR_write_VSPA_PROXY(DDR_WR_DMA_CHANNEL_5,
            VSPA_DMEM_PROXY_ADDR + offsetof(struct s_vspa_dmem_proxy, tx_state_readonly) * 2,
            2 * (uint32_t)&tx_vspa_proxy,
            sizeof(t_tx_ch_host_proxy) * 2);
        while (!dmac_is_complete(0x1 << DDR_WR_DMA_CHANNEL_5)) {}
        dmac_clear_complete(0x1 << DDR_WR_DMA_CHANNEL_5);

#ifdef TX_DDR_LOAD_TEST
        // if simple load test no axiq init
        if (DDR_rd_load_start_bit_update != 0) {
            DDR_rd_load_start_bit_update = 1;

            // Ack mailbox
            mailbox_out_msg_0_MSB = 0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));

            return;
        }
#endif

        // stop single tone
        TX_SingleT_start_bit_update = 0;

        // Disable Tx and reset fifo
        axiq_tx_disable();

        // Disable Tx and reset fifo
        stream_write_ptr_rst(DMA_CHANNEL_WR, axi_wr);
        do {
            PUSH_RX_DATA();
        } while (!dmac_is_complete(0x1 << DMA_CHANNEL_WR));
        dmac_clear_complete(0x1 << DMA_CHANNEL_WR);

        // Ack mailbox
        mailbox_out_msg_0_MSB = 0;
        mailbox_out_msg_0_LSB = 0x1;
        host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));

        // Enable Tx
        axiq_fifo_tx_enable(AXIQ_BANK_0, AXIQ_FIFO_TX0);

        // clear error
        axiq_fifo_tx_cr(AXIQ_BANK_0, AXIQ_FIFO_TX0, AXIQ_CR_CLRERR, AXIQ_CR_CLRERR);
        axiq_fifo_tx_cr(AXIQ_BANK_0, AXIQ_FIFO_TX0, AXIQ_CR_CLRERR, 0);

        return;
    }

    if (cmd_start == 0) /* stop */
    {
        if (!TX_SingleT_start_bit_update) {
            axiq_tx_disable();
            dmac_abort(0x1 << DMA_CHANNEL_WR);
            dmac_clear_complete(0x1 << DMA_CHANNEL_WR);
            dmac_clear_event(0x1 << DMA_CHANNEL_WR);
        }
        mailbox_out_msg_0_MSB = 0;
        mailbox_out_msg_0_LSB = 0x1;
        host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));

        DDR_rd_base_address = 0xdeadbeef;
        TX_total_ddr_fetched_size = 0;
        TX_total_axiq_consumed_size = 0;

        DDR_rd_start_bit_update = 0;
#ifdef TX_DDR_LOAD_TEST
        DDR_rd_load_start_bit_update = 0;
#endif
        TX_SingleT_start_bit_update = 0;

        // update host vspa_dmem_proxy
        tx_proxy_updated = 1;

        return;
    }

fail_tx_iq_data:

    /* failure send Nack */
    mailbox_out_msg_0_MSB = 0;
    mailbox_out_msg_0_LSB = 0x0;
    host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
    return;
}

//#pragma optimize_for_size off
volatile uint32_t tx_busy_size = 0;
void PUSH_TX_DATA(void) {
    uint32_t tmp_status;
    uint32_t tmp_dma_errors;
    uint32_t tx_empty_size;

    // Check AXIQ tx fifo is not empty or underrun
    tmp_status = axiq_fifo_tx_sr(AXIQ_BANK_0, AXIQ_FIFO_TX0, AXIQ_SR_FIELD_ERRUNDER | AXIQ_SR_FIELD_ERROVER);
    if (tmp_status != 0) {
        if (tmp_status & (AXIQ_SR_FIELD_ERROVER << axiq_sr_shift(AXIQ_FIFO_TX0))) {
            g_stats.tx_stats[ERROR_AXIQ_FIFO_TX_OVERRUN]++;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_TX_OVER, tmp_status);
        }
        if (tmp_status & (AXIQ_SR_FIELD_ERRUNDER << axiq_sr_shift(AXIQ_FIFO_TX0))) {
            g_stats.tx_stats[ERROR_AXIQ_FIFO_TX_UNDERRUN]++;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_TX_UNDER, tmp_status);
        }
        axiq_fifo_tx_cr(AXIQ_BANK_0, AXIQ_FIFO_TX0, AXIQ_CR_CLRERR, AXIQ_CR_CLRERR);
        axiq_fifo_tx_cr(AXIQ_BANK_0, AXIQ_FIFO_TX0, AXIQ_CR_CLRERR, 0);
        // l1_trace_disable = 1;
    }

#ifdef TX_DDR_LOAD_TEST
    // DDR DMA load test , just fetch DDR as fast as possible
    if (DDR_rd_load_start_bit_update) {
        if (!TX_ext_dma_enabled) {
            // Check transfer from DDR completed
            if (dmac_is_complete(ddr_rd_dma_ch_mask) == ddr_rd_dma_ch_mask) {
                dmac_clear_complete(ddr_rd_dma_ch_mask);
                g_stats.tx_stats[STAT_DMA_DDR_RD]++;
                l1_trace(L1_TRACE_MSG_DMA_DDR_RD_COMP, (uint32_t)g_stats.tx_stats[STAT_DMA_DDR_RD]);
            }
            // start new transfer from DDR if possible
            if (dmac_is_available(ddr_rd_dma_ch_mask) == ddr_rd_dma_ch_mask) {
                // start new transfer from ddr ( using 4 DMA to take advantage of possible 4 outstanding Read on AXIQ bus )
                rd_ddr_src_ptr = DDR_rd_base_address + DDR_rd_counter;
                p_tx_ddr_enqueued = &output_buffer[1 * TX_DMA_TXR_size];
                rd_dmem_dst_byte_ptr = 2 * (uint32_t)(p_tx_ddr_enqueued);
                DDR_read_multi_dma(DDR_RD_DMA_CHANNEL_1, ddr_rd_dma_ch_nb, rd_ddr_src_ptr, rd_dmem_dst_byte_ptr,
                                   ddr_rd_dma_xfr_size);
                DDR_rd_counter = (DDR_rd_counter + ddr_rd_dma_xfr_size) % DDR_rd_size;
                l1_trace(L1_TRACE_MSG_DMA_DDR_RD_START, (uint32_t)p_tx_ddr_enqueued);
            }
        }
    }
#endif

    // Single Tone
    if (TX_SingleT_start_bit_update) {
        if (dmac_is_complete(0x1 << DMA_CHANNEL_WR)) {
            dmac_clear_complete(0x1 << DMA_CHANNEL_WR);
            g_stats.tx_stats[STAT_DMA_AXIQ_WRITE]++;
            TX_total_axiq_consumed_size += TX_DDR_STEP;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_TX_COMP, (uint32_t)g_stats.tx_stats[STAT_DMA_AXIQ_WRITE]);
        }

        // Check if there is room to post new tx cmd
        if (dmac_is_available(0x1 << DMA_CHANNEL_WR)) {
            // tx_qec_correction((vspa_complex_fixed16 *)qec_ptr);
            // !! tbd would need extra copy and pingpong for runtime QeC
            // or trigger new nco gen + qec on QEC param update
            stream_write(DMA_CHANNEL_WR, axi_wr, 2 * (uint32_t)(TX_SingleT_buffer));
            TX_total_axiq_enqueued_size += TX_DDR_STEP;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_TX_START, (uint32_t)TX_SingleT_buffer);
        }
    }

    // if any of previous load test is on, skip DDR streaming
    if (DDR_rd_load_start_bit_update || TX_SingleT_start_bit_update)
        goto end_tx_push;

    // Stream waveform from DDR buffer
    if (DDR_rd_start_bit_update) {
#ifdef TX_CYCLE_METER
        uint32_t cm_t0 = ccnt_read();
        meter_gap(&g_stats.meter[METER_TX_BASE], cm_t0);
#endif
        // Check transmit complete (write to axiq DAC)
#ifdef TX_DDR_STATE_CMPL
        {
            uint32_t d = dma_done_from_state(0x1 << DMA_CHANNEL_WR, TX_total_axiq_enqueued_size,
                                             TX_total_axiq_consumed_size, &tx_axiq_cand, TX_DDR_STEP);
            while (TX_total_axiq_consumed_size != d) {
                INCR_TX_BUFF(p_tx_axiq_consumed);
                TX_total_axiq_consumed_size += TX_DDR_STEP;
                g_stats.tx_stats[STAT_DMA_AXIQ_WRITE]++;
                tx_proxy_updated = 1;
            }
        }
#else
        if (dmac_is_complete(0x1 << DMA_CHANNEL_WR)) {
            dmac_clear_complete(0x1 << DMA_CHANNEL_WR);
            // dmac_clear_event(0x1<<DMA_CHANNEL_WR);
            // Update consumed pointer (i.e. buffer is ready for reuse)
            INCR_TX_BUFF(p_tx_axiq_consumed);
            TX_total_axiq_consumed_size += TX_DDR_STEP;
            g_stats.tx_stats[STAT_DMA_AXIQ_WRITE]++;
            TX_TRACE(L1_TRACE_MSG_DMA_AXIQ_TX_COMP, (uint32_t)g_stats.tx_stats[STAT_DMA_AXIQ_WRITE]);
            // update host vspa_dmem_proxy
            tx_proxy_updated = 1;
        }
#endif
        if (dmac_errxfr(0x1 << DMA_CHANNEL_WR)) {
            dmac_clear_errxfr(0x1 << DMA_CHANNEL_WR);
            g_stats.gbl_stats[ERROR_DMA_XFER_ERROR]++;
            l1_trace(L1_TRACE_MSG_DMA_AXIQ_TX_XFER_ERROR, (uint32_t)g_stats.gbl_stats[ERROR_DMA_XFER_ERROR]);
        }

        if (!TX_ext_dma_enabled) {
            // Check transfer from DDR completed
#ifdef TX_DDR_STATE_CMPL
            {
                uint32_t d = dma_done_from_state(ddr_rd_dma_ch_mask, TX_total_ddr_enqueued_size,
                                                 TX_total_ddr_fetched_size, &tx_ddr_cand, TX_DDR_STEP);
                if (TX_total_ddr_fetched_size != d) {
                    while (TX_total_ddr_fetched_size != d) {
                        INCR_TX_BUFF(p_tx_ddr_fetched);
                        TX_total_ddr_fetched_size += TX_DDR_STEP;
                        g_stats.tx_stats[STAT_DMA_DDR_RD]++;
                    }
                    TX_TRACE(L1_TRACE_MSG_DMA_DDR_RD_COMP, (uint32_t)TX_total_ddr_fetched_size);
                    tx_proxy_updated = 1;
                }
            }
#else
            if (dmac_is_complete(ddr_rd_dma_ch_mask) == ddr_rd_dma_ch_mask) {
                dmac_clear_complete(ddr_rd_dma_ch_mask);
                INCR_TX_BUFF(p_tx_ddr_fetched);
                TX_total_ddr_fetched_size += TX_DDR_STEP;
                g_stats.tx_stats[STAT_DMA_DDR_RD]++;
                TX_TRACE(L1_TRACE_MSG_DMA_DDR_RD_COMP, (uint32_t)g_stats.tx_stats[STAT_DMA_DDR_RD]);
            }
#endif

            // host flow control
            if ((TX_total_ddr_ready_size - TX_total_ddr_enqueued_size >= TX_DDR_STEP) || host_flow_control_disable) {
                // start new transfer from DDR if possible
                if (dmac_is_available(ddr_rd_dma_ch_mask) == ddr_rd_dma_ch_mask) {
                    tx_busy_size = TX_total_ddr_enqueued_size - TX_total_axiq_consumed_size;
                    tx_empty_size = (TX_NUM_BUF * TX_DDR_STEP) - tx_busy_size;
                    if (tx_empty_size >= TX_DDR_STEP) {
                        // start new transfer from ddr ( using 4 DMA to take advantage of possible 4 outstanding Read on AXIQ bus )
                        rd_ddr_src_ptr = DDR_rd_base_address + DDR_rd_counter;
                        rd_dmem_dst_byte_ptr = 2 * (uint32_t)(p_tx_ddr_enqueued);
                        DDR_read_multi_dma(DDR_RD_DMA_CHANNEL_1, ddr_rd_dma_ch_nb, rd_ddr_src_ptr, rd_dmem_dst_byte_ptr,
                                           ddr_rd_dma_xfr_size);
                        DDR_rd_counter = (DDR_rd_counter + ddr_rd_dma_xfr_size) % DDR_rd_size;
                        TX_TRACE(L1_TRACE_MSG_DMA_DDR_RD_START, (uint32_t)p_tx_ddr_enqueued);
                        INCR_TX_BUFF(p_tx_ddr_enqueued);
                        TX_total_ddr_enqueued_size += TX_DDR_STEP;
                    }
                }
            }
        }

        // QEC data before transmission
        if ((TX_total_ddr_fetched_size - TX_total_dmem_QECced_size) >= TX_DDR_STEP) {
            TX_TRACE(L1_TRACE_L1APP_TX_QEC_START, (uint32_t)p_tx_dmem_QECed);
            tx_qec_correction(p_tx_dmem_QECed, p_tx_dmem_QECed);
            INCR_TX_BUFF(p_tx_dmem_QECed);
            TX_total_dmem_QECced_size += TX_DDR_STEP;
            TX_TRACE(L1_TRACE_L1APP_TX_QEC_COMP, (uint32_t)TX_total_dmem_QECced_size);
        }

        /* Start new transfers to DAC: use while so all free DMAC queue
         * slots are filled, keeping the AXIQ TX FIFO continuously fed. */
        while (dmac_is_available(0x1 << DMA_CHANNEL_WR)) {
            if ((TX_total_dmem_QECced_size - TX_total_axiq_enqueued_size) >= TX_DDR_STEP) {
                stream_write(DMA_CHANNEL_WR, axi_wr, 2 * (uint32_t)(p_tx_axiq_enqueued));
                TX_TRACE(L1_TRACE_MSG_DMA_AXIQ_TX_START, (uint32_t)p_tx_axiq_enqueued);
                INCR_TX_BUFF(p_tx_axiq_enqueued);
                TX_total_axiq_enqueued_size += TX_DDR_STEP;
            } else {
                if (TX_total_axiq_enqueued_size == TX_total_axiq_consumed_size) {
                    /* nothing in flight and no data ready: real underrun */
                    g_stats.tx_stats[ERROR_DMA_DDR_RD_UNDERRUN]++;
                    l1_trace(L1_TRACE_MSG_DMA_DDR_RD_UNDERRUN, (uint32_t)g_stats.tx_stats[ERROR_DMA_DDR_RD_UNDERRUN]);
                }
                break;
            }
        }
#ifdef TX_CYCLE_METER
        g_stats.meter[METER_TX_DDR_INFL] = (TX_total_ddr_enqueued_size - TX_total_ddr_fetched_size) / TX_DDR_STEP;
        g_stats.meter[METER_TX_AXIQ_INFL] = (TX_total_axiq_enqueued_size - TX_total_axiq_consumed_size) / TX_DDR_STEP;
        meter_end(&g_stats.meter[METER_TX_BASE], cm_t0, TX_PASS_BUDGET);
#endif
    }

end_tx_push:

    /* Check DMA errors*/
    tmp_dma_errors = (uint32_t)iord(DMA_CFGERR_STAT);
    if (tmp_dma_errors != 0) {
        g_stats.gbl_stats[ERROR_DMA_CONFIG_ERROR]++;
        l1_trace(L1_TRACE_MSG_DMA_CFGERR, (uint32_t)(g_stats.gbl_stats[ERROR_DMA_CONFIG_ERROR]));
        dmac_clear_errcfg();
    };

    // update host proxy if needed
    VSPA_PROXY_update();
}
