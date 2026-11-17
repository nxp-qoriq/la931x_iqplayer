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
#include "phy-timer.h"
#include "ippu.h"
#include "ovly.h"
#include "ovly_init.h"
#include "main.h"
#include "dfe.h"
#include "iqmod_tx.h"
#include "iqmod_rx.h"
#include "cal_signal.h"
#include "dc_cal.h"
#include "ccnt.h"
#include "l1-trace.h"
#include "stats.h"
#include "vspa_dmem_proxy.h"

/* Main loop meter: cycles from GO wakeup (top of loop body) to __idle(),
 * so idle time is excluded. g_stats.meter[6..9] = max, count,
 * passes > LOOP_PASS_BUDGET, sum >> 4. Build with -DLOOP_CYCLE_METER_OFF
 * to remove it. */
#ifndef LOOP_CYCLE_METER_OFF
#define LOOP_CYCLE_METER 1
#endif
#ifndef LOOP_PASS_BUDGET
#define LOOP_PASS_BUDGET 2048u
#endif

volatile int dbg_gbl = 0xdeadbeef;

volatile t_stats g_stats __attribute__((aligned(64)));
t_tx_ch_host_proxy tx_vspa_proxy __attribute__((section(".dmem_proxy_tx"))) __attribute__((aligned(32)));
t_rx_ch_host_proxy rx_vspa_proxy[RX_NUM_MAX_CHAN] __attribute__((section(".dmem_proxy_rx"))) __attribute__((aligned(32)));
uint32_t tx_proxy_updated = 0;
uint32_t rx_proxy_updated = 0;
uint32_t g_iqflood_proxy_offset = 0;

uint32_t TX_SingleT_start_bit_update = 0, RX_SingleT_start_bit_update = 0, RX_SingleT_continue = 0;

/* DCS AXIQ DMA mapping
 * as per LA9310 RM Table 13. AXIQ FIFO Address Map and Intended VSPA DMA Association
 *
 *                      >default<
 * chanID                  0           1           2           3            >  mbx API chanID
 * RFNM slot_chan          RBA_RX1     RBA_RX2     RBB_RX1     RBB_RX2      >  RFNM slot mapping
 * LA9310 chan             RO1         RX1         RO0         RX0
 * VSPA DMA chanID         2           4           1           3
 * axi_ADC_FIFI_addr[4] = {0x44002000, 0x44004000, 0x44001000, 0x44003000};
 */

uint32_t Rx_Antenna2axiq_dma_chan[4] = { 0x2, 0x4, 0x1, 0x3 };
enum axiq_fifo_e Rx_Antenna2fifo_index[4] = { AXIQ_FIFO_RX1, AXIQ_FIFO_RX3, AXIQ_FIFO_RX0, AXIQ_FIFO_RX2 };
uint32_t axi_ADC_FIFO_addr[4] = { 0x44002000, 0x44004000, 0x44001000, 0x44003000 };
/* Per-slot physical RX channel (mbx chanID). Slot i streams chanID
 * rx_chan_map[i]. Default is contiguous from RX_index. */
uint32_t rx_chan_map[4] = { 0, 1, 2, 3 };
/* Tables indexed by slot, kept in sync with rx_chan_map (saves PRAM in the
 * RX hot path versus a double lookup). */
uint32_t rx_slot_dma[4] = { 0x2, 0x4, 0x1, 0x3 };
uint32_t rx_slot_axi[4] = { 0x44002000, 0x44004000, 0x44001000, 0x44003000 };
enum axiq_fifo_e rx_slot_fifo[4] = { AXIQ_FIFO_RX1, AXIQ_FIFO_RX3, AXIQ_FIFO_RX0, AXIQ_FIFO_RX2 };
uint32_t axi_DAC_FIFO_addr = 0x4400B000;
/* RX AXIQ IQ swap, applied by RX_FIFO_ENABLE(); set by MBOX_OPC_RX_IQ_SWAP. */
uint32_t rx_iq_swap_cr = RX_IQ_SWAP ? AXIQ_CR_IQ_SWAP_ON : AXIQ_CR_IQ_SWAP_OFF;

/* Number of TX channels published to the host (0 for RX-only builds). */
#ifdef IQMOD_RX_0T1R
#define PROXY_TX_NUM_CHAN 0
#else
#define PROXY_TX_NUM_CHAN 1
#endif

/* Rebuild the proxy configuration word (RX/TX channel count, IQ swap, RX
 * channel map, see PROXY_CFG_* in vspa_dmem_proxy.h) and schedule the proxy
 * copy to the host. Not in the streaming path: boot and mailbox only. */
static void proxy_cfg_update(void)
{
    uint32_t map = rx_chan_map[0] | (rx_chan_map[1] << 4) |
                   (rx_chan_map[2] << 8) | (rx_chan_map[3] << 12);

    tx_vspa_proxy.rx_num_chan = PROXY_CFG_MAKE(RX_NUM_CHAN,
                                               rx_iq_swap_cr == AXIQ_CR_IQ_SWAP_ON,
                                               PROXY_TX_NUM_CHAN, map);
    tx_proxy_updated = 1;
}

/* Global variables */

volatile uint32_t mailbox_out_msg_0_MSB = 0; // (VCPU_OUT_0_MSB)
volatile uint32_t mailbox_out_msg_0_LSB = 0; // (VCPU_OUT_0_LSB)
volatile uint32_t mailbox_out_msg_1_MSB = 0; // (VCPU_OUT_1_MSB)
volatile uint32_t mailbox_out_msg_1_LSB = 0; // (VCPU_OUT_1_LSB)
uint32_t mailbox_in_0_status, mailbox_in_msg_0_MSB, mailbox_in_msg_0_LSB;
uint32_t mailbox_in_msg_1_MSB, mailbox_in_msg_1_LSB;

mbox_opc_e op_mode;

uint32_t axi_wr, dma_channel_rd, axi_rd;

uint32_t fft_bin, RX_index, BW_index, DCOC_direction, tone_freq_DAC, tone_phase_DAC;

vspa_complex_float32 gain;
vspa_complex_float32 *pgain;

int i, tone_index, image_tone_index;

bool first_run_done = false;

//======================================================================================================
// Main local variables and types  (variables should be declared static)
//======================================================================================================

//======================================================================================================
// Public (externally visible) functions
//======================================================================================================

/* Cycle meter helpers shared by the TX pass meter and the loop meter.
 * Kept out of line to save PRAM (1T2R has little headroom). */
__attribute__((noinline)) void meter_gap(volatile uint32_t *m, uint32_t t0) {
    uint32_t gap = t0 - m[5];
    if (m[5] != 0 && gap < 0x00100000u && gap > m[4])
        m[4] = gap;
    m[5] = t0 | 1u;
}

__attribute__((noinline)) void meter_end(volatile uint32_t *m, uint32_t t0, uint32_t budget) {
    uint32_t dt = ccnt_read() - t0;
    if (dt >= 0x00100000u)
        return;
    if (dt > m[0])
        m[0] = dt;
    m[1]++;
    if (dt > budget)
        m[2]++;
    m[3] += dt >> 4;
}

/* DMA-state completion accounting for FIFO-mode channels (2 command slots,
 * one sticky completion bit). Two completions between two polls merge into
 * one bit, so counting completion bits loses buffers for good. Instead the
 * number of transfers still in flight is derived from the channel state:
 *   no FIFO slot free -> 2, slot free and running -> 1, idle -> 0
 * and done = enq - in_flight * step. done never moves back and advances to
 * min(this pass, previous pass) so a single transient read cannot
 * overestimate it. Same method as the RX DDR accounting. */
__attribute__((noinline)) uint32_t dma_done_from_state(uint32_t mask, uint32_t enq, uint32_t done, uint32_t *cand,
                                                       uint32_t step) {
    uint32_t n, c, prev;

    if (enq == done)
        return done;
    dmac_clear_complete(mask);
    if (dmac_is_available(mask) != mask)
        n = 2;
    else
        n = dmac_is_running(mask) ? 1 : 0;
    c = enq - n * step;
    prev = *cand;
    *cand = c;
    if ((int32_t)(prev - c) < 0)
        c = prev;
    if ((int32_t)(c - done) > 0)
        return c;
    return done;
}

// update host proxy if needed

void VSPA_PROXY_update(void) {
    /* Proxy DMAs on channel 5 are fire-and-forget: clear any stale
     * completion bit so later synchronous waits on channel 5 do not
     * exit early on an old completion. */
    if (dmac_is_complete(0x1 << DDR_WR_DMA_CHANNEL_5)) {
        dmac_clear_complete(0x1 << DDR_WR_DMA_CHANNEL_5);
    }
    /* Clear the dirty flag only once the proxy DMA is actually launched.
     * If channel 5 is busy (e.g. RX and TX both dirty in the same call),
     * keep the flag set and retry on the next iteration. Dropping the
     * update leaves a stale TX enqueued counter in the host mirror, which
     * stalls host flow control and causes periodic AXIQ TX UDR/OVR bursts. */
    if (rx_proxy_updated) {
        if (dmac_is_available(0x1 << DDR_WR_DMA_CHANNEL_5)) {
            rx_proxy_updated = 0;
            DDR_write_VSPA_PROXY(DDR_WR_DMA_CHANNEL_5,
                                 VSPA_DMEM_PROXY_ADDR + offsetof(struct s_vspa_dmem_proxy, rx_state_readonly) * 2,
                                 2 * (uint32_t) & (rx_vspa_proxy[0]), sizeof(t_rx_ch_host_proxy) * 2 * RX_NUM_CHAN);
        }
    }
    if (tx_proxy_updated) {
        if (dmac_is_available(0x1 << DDR_WR_DMA_CHANNEL_5)) {
            tx_proxy_updated = 0;
            DDR_write_VSPA_PROXY(DDR_WR_DMA_CHANNEL_5,
                                 VSPA_DMEM_PROXY_ADDR + offsetof(struct s_vspa_dmem_proxy, tx_state_readonly) * 2,
                                 2 * (uint32_t)&tx_vspa_proxy, sizeof(t_tx_ch_host_proxy) * 2);
        }
    }
}

void DDR_write_VSPA_PROXY(uint32_t DDR_wr_dma_channel, uint32_t DDR_address, uint32_t vsp_address, uint32_t size) {
    uint32_t ctrl = DMAC_WR | DDR_wr_dma_channel;
    dmac_enable(ctrl, size, DDR_address, vsp_address);
#ifdef RX_HOT_TRACE
    l1_trace(L1_TRACE_MSG_DMA_XFR, (uint32_t)DDR_address);
#endif
}

/* ELD_BUILD_VERSION: MMDDHHmm decimal, set by Sources/Makefile.
 * IDE builds (CodeWarrior) do not define it: use 0. A value derived from
 * __DATE__/__TIME__ is decoded at run time by this compiler and does not
 * fit in the 1T2R PRAM. Add ELD_BUILD_VERSION=<n> to the CW defines to
 * stamp an IDE build. */
#ifndef ELD_BUILD_VERSION
#define ELD_BUILD_VERSION 0
#endif

void setup(void) {
    uint64_t msg64 = 0, i = 0, j = 0;

    /* First trace entry: build version for post-mortem identification.
     * ELD_BUILD_VERSION is MMDDHHmm decimal (Makefile), 0 in IDE builds. */
    l1_trace(L1_TRACE_MSG_BUILD_VER, ELD_BUILD_VERSION);

    vcpu_swver(0x00020002);
    ccnt_enable();
    host_mbox0_post(MAKEDWORD(0xF1000000, 0));
#ifndef IS_SIMULATOR
    do {
        mailbox_in_0_status = (vspa_mbox0_is_valid() | vspa_mbox1_is_valid());
    } while (mailbox_in_0_status == 0);
#endif
    if (mailbox_in_0_status & 0x4) {
        msg64 = host_mbox0_read();
        mailbox_in_msg_0_MSB = HIWORD(msg64);
        mailbox_in_msg_0_LSB = LOWORD(msg64);
        if (mailbox_in_msg_0_MSB == 0x70000000 && mailbox_in_msg_0_LSB == 0x0) {
            host_mbox0_post(MAKEDWORD(0xF0700000, 0x0));
        }
    }

    if (mailbox_in_0_status & 0x8) {
        msg64 = host_mbox1_read();
        mailbox_in_msg_1_MSB = HIWORD(msg64);
        mailbox_in_msg_1_LSB = LOWORD(msg64);
        if (mailbox_in_msg_1_MSB == 0x70000000 && mailbox_in_msg_1_LSB == 0x0) {
            host_mbox1_post(MAKEDWORD(0xF0700000, 0));
        }
    }

    rf_init();

    axi_wr = 0x4400B000;

#if !defined(IQMOD_RX_0T1R) && !defined(IQMOD_RX_0T4R)
    stream_write_ptr_rst(DMA_CHANNEL_WR, axi_wr);
    WAIT(dmac_is_complete(0x1 << DMA_CHANNEL_WR));
    dmac_clear_complete(0x1 << DMA_CHANNEL_WR);
#endif

    host_clear();
    host_mbox0_enable();
    host_mbox1_enable();

    ippu_arg_base(0x1F80); // need a check!!!

    /* Redirect GO wakeup entry point to main() directly so subsequent
     * GO events (mailbox, DMA) skip _start/_init_bss and do not zero
     * .bss again (which would reset first_run_done to false). */
    entry(main);

    first_run_done = true;

    memclr((void *)&g_stats, sizeof(g_stats));
}

#pragma optimization_level 0
void wait_for_pending_transfers(uint32_t ch) {
    // clear done transfers if there are any
    while (dmac_is_complete(0x1 << ch)) {
#if !defined(IQMOD_RX_0T1R) && !defined(IQMOD_RX_0T4R)
        PUSH_TX_DATA();
#endif
        dmac_clear_complete(0x1 << ch);
        for (int i = 0; i < 100; i++) {
            __asm volatile("fnop .asmvol");
            __asm volatile("fnop .asmvol");
            __asm volatile("fnop .asmvol");
        }
    }

    // check if transfer is still pending
    while (dmac_is_running(0x1 << ch)) {
        // wait for transfer to finish
        do {
#if !defined(IQMOD_RX_0T1R) && !defined(IQMOD_RX_0T4R)
            PUSH_TX_DATA();
#endif
        } while (!dmac_is_complete(0x1 << ch));

        dmac_clear_complete(0x1 << ch);
        for (int i = 0; i < 100; i++) {
            __asm volatile("fnop .asmvol");
            __asm volatile("fnop .asmvol");
            __asm volatile("fnop .asmvol");
        }
    }

    // stop channel
    dmac_clear_event(0x1 << ch);
    dmac_clear_complete(0x1 << ch);
}
#pragma optimization_level reset

void DDR_write_MSI(uint32_t DDR_wr_dma_channel, uint32_t DDR_address, uint32_t vsp_address) {
    uint32_t ctrl = DMAC_WR | DDR_wr_dma_channel;
    uint32_t size = 4;

    dmac_enable(ctrl, size, DDR_address, vsp_address);
}

//----------------------------------------------------------------------------------------------------
__attribute__((noreturn)) void main(void) {
    uint64_t msg64 = 0, i = 0;

    l1_trace(L1_TRACE_MSG_ENTRY_MAIN, (uint32_t)iord(CONTROL));
    //l1_trace(L1_TRACE_MSG_ENTRY_MAIN, (uint32_t)iord(DMA_GO_STAT));
    //l1_trace(L1_TRACE_MSG_ENTRY_MAIN, (uint32_t)iord(DMA_CFGERR_STAT));
    l1_trace(L1_TRACE_MSG_ENTRY_MAIN, (uint32_t)iord(DMA_COMP_STAT));

    if (!first_run_done) {
// !! this workaround is needed otherwise VSPA gets stuck (dmac_is_available()/dmac_is_complete() doesn't work)
// !! unless issue "ccs::config_chain {la9310 dap}" on ccs
#define HOST_VCPU_A011455 (0x024 >> 2)
        iowr(HOST_VCPU_A011455, 0x10, 0x10);

        initOverlays();
        // redcap_load_ovly(REDCAP_OVLY_CODE_IQ_DATA);

        setup();

        RX_index = 0;
        axi_rd = axi_ADC_FIFO_addr[RX_index];
        dma_channel_rd = Rx_Antenna2axiq_dma_chan[RX_index];

        // update host vspa_dmem_proxy
        tx_vspa_proxy.DDR_rd_base_address = 0xdeadbeef;
        tx_vspa_proxy.dmemProxyOffset = (uint32_t)&tx_vspa_proxy;
        tx_vspa_proxy.rx_decim = RX_DECIM;
        tx_vspa_proxy.tx_upsmp = TX_UPSMP;
        tx_vspa_proxy.rx_ddr_step = RX_DDR_STEP;
        tx_vspa_proxy.tx_ddr_step = TX_DDR_STEP;
        proxy_cfg_update();
        for (i = 0; i < RX_NUM_CHAN; i++)
            /* VSPA addresses are in 2-byte units; multiply by 2 to get AHB byte offset */
            tx_vspa_proxy.rx_fc_addr[i] = BASE_VSPA_VCPU(0) +
                2u * (uint32_t)&rx_vspa_proxy[i].la9310_fifo_consumed_size;
        tx_proxy_updated = 1;
        for (i = 0; i < RX_NUM_MAX_CHAN; i++) {
            rx_vspa_proxy[i].DDR_wr_base_address = 0xdeadbeef;
            rx_vspa_proxy[i].DDR_wr_size = i;
        }
        rx_proxy_updated = 1;
        tx_vspa_proxy.gbl_stats_fetch = 1;

    }

    while (1) {
    /* On each entry — including re-entry after __idle() GO wakeup — read the
     * wakeup cause and pending mbox status first.  __idle() = __builtin_done()
     * is __noreturn: the VSPA re-enters main() from the top on every GO event,
     * so any code placed after __idle() in the loop body is dead code. */
#ifdef LOOP_CYCLE_METER
    uint32_t lm_t0 = ccnt_read();
#endif
#ifdef RX_HOT_TRACE
    l1_trace(L1_TRACE_MSG_ENTRY_MAIN_DMASTAT, (uint32_t)iord(DMA_GO_STAT));
#endif
#ifndef IS_SIMULATOR
    mailbox_in_0_status = (vspa_mbox0_is_valid() | vspa_mbox1_is_valid());
#else
    mailbox_in_0_status = 0x4;
#endif

    if (tx_vspa_proxy.gbl_stats_fetch) {
        if (dmac_is_available(0x1 << DDR_WR_DMA_CHANNEL_5)) {
            tx_vspa_proxy.gbl_stats_fetch = 0;
            DDR_write_VSPA_PROXY(DDR_WR_DMA_CHANNEL_5,
                                 VSPA_DMEM_PROXY_ADDR + offsetof(struct s_vspa_dmem_proxy, vspa_stats) * 2,
                                 2 * (uint32_t)&g_stats, sizeof(t_stats) * 2);
        } else {
            g_stats.gbl_stats[ERROR_DMA_XFER_ERROR]++;
        }
    }

    if (mailbox_in_0_status & 0x4) {
#ifndef IS_SIMULATOR
        msg64 = host_mbox0_read();
        mailbox_in_msg_0_MSB = HIWORD(msg64);
        mailbox_in_msg_0_LSB = LOWORD(msg64);

        host_mbox0_clear();
        op_mode = (mbox_opc_e)((mailbox_in_msg_0_MSB & 0xFF000000) >> 24);
        fft_bin = (mailbox_in_msg_0_LSB & 0x000000FF);
        BW_index = ((mailbox_in_msg_0_MSB & 0x000C0000) >> 18);
        DCOC_direction = ((mailbox_in_msg_0_MSB & 0x00200000) >> 21);
#else
        // No mailbox but debugger/tcl script init
        mailbox_in_msg_0_MSB = 0;
        mailbox_in_msg_0_LSB = 0;
        op_mode = MBOX_OPC_SINGLE_TONE_TX;
        fft_bin = 0x21;
        BW_index = 0;
        DCOC_direction = 0;
        __swbreak();
#endif // #ifndef IS_SIMULATOR

        tone_index = FFT_SIZE / 2 + fft_bin;
        image_tone_index = FFT_SIZE / 2 - fft_bin;

        axi_wr = axi_DAC_FIFO_addr;

        //   __swbreak();

        switch (op_mode) {
#ifndef IQMOD_RX_0T1R
        case MBOX_OPC_SINGLE_TONE_TX:
            TX_single_tone();
            break;

        case MBOX_OPC_SINGLE_TONE_RX:
            RX_single_tone_measurement();
            break;

        case MBOX_OPC_DCOC:
            if (DCOC_direction == 0) {
                RX_DCOC_CAL();
            } else {
                TX_DCOC_CAL();
            }
            break;

        case MBOX_OPC_BW_CAL:
            BW_CAL();
            break;

        case MBOX_OPC_IQ_MOD_TX:
            TX_IQ_DATA_FROM_DDR();
            break;
#endif

#ifndef IQMOD_RX_1T0R
        case MBOX_OPC_IQ_MOD_RX:
            RX_IQ_DATA_TO_DDR();
            break;

        case MBOX_OPC_MSI: {
            uint32_t msi_reg = mailbox_in_msg_0_LSB;

            ((vspa_complex_fixed16 *)input_buffer)->real = 0x0b0b;
            ((vspa_complex_fixed16 *)input_buffer)->imag = 0x0b0b;
            dmac_abort(0x1 << DDR_WR_DMA_CHANNEL_1);
            dmac_clear_complete(0x1 << DDR_WR_DMA_CHANNEL_1);
            DDR_write_MSI(DDR_WR_DMA_CHANNEL_1, 0xB0000000 + msi_reg * 0x100, 2 * (uint32_t)(input_buffer));
            WAIT(dmac_is_complete(0x1 << DDR_WR_DMA_CHANNEL_1));

            //*(uint32_t *)(&input_buffer[1])=0xB0000008 + msi_reg*0x100;
            dmac_abort(0x1 << DDR_WR_DMA_CHANNEL_1);
            dmac_clear_complete(0x1 << DDR_WR_DMA_CHANNEL_1);
            DDR_write_MSI(DDR_WR_DMA_CHANNEL_1, 0xA0000000, 2 * (uint32_t)(input_buffer));
            WAIT(dmac_is_complete(0x1 << DDR_WR_DMA_CHANNEL_1));

        } break;
#endif

        case MBOX_OPC_IQ_CORR: {
            // QEC param update
            /* bit 50 all channels, bit49-48 channel (see QEC_MBOX_* in dfe.h) */
            uint32_t iq_tx_rx = ((mailbox_in_msg_0_MSB & 0x00200000) >> 21);      /* bit 53 */
            uint32_t iq_rst = ((mailbox_in_msg_0_MSB & 0x00100000) >> 20);        /* bit 52 */
            uint32_t iq_idx = mailbox_in_msg_0_MSB & 0x0000FFFF;                  /* bit 47-32*/
            uint32_t iq_val = mailbox_in_msg_0_LSB;                               /* bit 31-0 */
            uint32_t iq_ok;

            if (iq_tx_rx) {
                iq_ok = QEC_TX_CH_OK(mailbox_in_msg_0_MSB);
                if (iq_ok) {
#ifdef TXIQCOMP2
                    rf_update_iq_comp_params2(&iq_comp_params2_tx, iq_rst, iq_idx, iq_val);
#else
                    rf_update_iq_comp_params(&txiqcompcfg_struct, iq_rst, iq_idx, iq_val);
#endif
                }
#ifndef IQMOD_RX_0T1R
                // update single tone pattern
                if (TX_SingleT_start_bit_update) {
                    gen_nco_single_tone(TX_SingleT_buffer);
                }
#endif
            } else {
                iq_ok = rx_qec_mbox_apply(mailbox_in_msg_0_MSB, iq_val, 0);
            }
            (void)iq_rst; (void)iq_idx;

            mailbox_out_msg_0_MSB = mailbox_in_msg_0_LSB;
            mailbox_out_msg_0_LSB = iq_ok;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;
        }

#ifndef IQMOD_RX_0T1R
        case MBOX_OPC_TX_DCO_CORR:
#ifdef TXIQCOMP2
            iq_comp_params2_tx.dcOffset.real = (mailbox_in_msg_0_LSB & 0x0000FFFF);
            iq_comp_params2_tx.dcOffset.imag = ((mailbox_in_msg_0_LSB & 0xFFFF0000) >> 16);
#else
            txiqcompcfg_struct.dcOffset.real = (mailbox_in_msg_0_LSB & 0x0000FFFF);
            txiqcompcfg_struct.dcOffset.imag = ((mailbox_in_msg_0_LSB & 0xFFFF0000) >> 16);
#endif

            // update single tone pattern
            if (TX_SingleT_start_bit_update) {
                gen_nco_single_tone(TX_SingleT_buffer);
            }

            mailbox_out_msg_0_MSB = mailbox_in_msg_0_LSB;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;
#endif

#ifndef IQMOD_RX_1T0R
        case MBOX_OPC_RX_DCO_CORR: {
            /* bit 50 all channels, bit49-48 channel */
            uint32_t dc_ok = rx_qec_mbox_apply(mailbox_in_msg_0_MSB, mailbox_in_msg_0_LSB, 1);
            mailbox_out_msg_0_MSB = mailbox_in_msg_0_LSB;
            mailbox_out_msg_0_LSB = dc_ok;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;
        }

        case MBOX_OPC_RX_CHAN_SELECT: {
            /* LSB bit 16 = 0: contiguous, slot i = (LSB & 0xf) + i.
             * LSB bit 16 = 1: list, slot i = (LSB >> 4*i) & 0xf (bits 15..0,
             * the flag sits above the 4 slots so slot 2 does not overlap it).
             * NACK (LSB 0) if a chanID is > 3 or used twice. */
            /* No local array here: main() stack is at its limit. */
#define RX_SEL_CH(sel, k) (((sel) & 0x10000) ? (((sel) >> (4 * (k))) & 0xf) : (((sel) & 0xf) + (k)))
            uint32_t sel = mailbox_in_msg_0_LSB;
            uint32_t used = 0, ok = 1, k, c;

            for (k = 0; k < RX_NUM_CHAN; k++) {
                c = RX_SEL_CH(sel, k);
                if (c > 3 || (used & (1u << c)))
                    ok = 0;
                else
                    used |= 1u << c;
            }

            mailbox_out_msg_0_MSB = mailbox_in_msg_0_LSB;
            mailbox_out_msg_0_LSB = 0x0;
            if (ok) {
                dmac_abort(0x1 << dma_channel_rd);
                dmac_clear_complete(0x1 << dma_channel_rd);
                DDR_wr_start_bit_update = 0;
                axiq_rx_disable();
                for (k = 0; k < RX_NUM_CHAN; k++) {
                    c = RX_SEL_CH(sel, k);
                    rx_chan_map[k] = c;
                    rx_slot_dma[k] = Rx_Antenna2axiq_dma_chan[c];
                    rx_slot_axi[k] = axi_ADC_FIFO_addr[c];
                    rx_slot_fifo[k] = Rx_Antenna2fifo_index[c];
                }
                RX_index = rx_chan_map[0];
                axi_rd = axi_ADC_FIFO_addr[RX_index];
                dma_channel_rd = Rx_Antenna2axiq_dma_chan[RX_index];
                proxy_cfg_update();
                mailbox_out_msg_0_LSB = 0x1;
            }
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;
        }
#endif

#ifndef IQMOD_RX_1T0R
        case MBOX_OPC_RX_IQ_SWAP: {
            /* LSB 0 = no swap, 1 = swap, else NACK. Applied on the next RX
             * FIFO enable (next RX start / channel select). */
            uint32_t v = mailbox_in_msg_0_LSB;

            mailbox_out_msg_0_MSB = v;
            mailbox_out_msg_0_LSB = (v <= 1);
            if (v <= 1) {
                rx_iq_swap_cr = v ? AXIQ_CR_IQ_SWAP_ON : AXIQ_CR_IQ_SWAP_OFF;
                proxy_cfg_update();
            }
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;
        }
#endif

        case MBOX_OPC_EMPTY_2:
            // does nothing but ACK, mailbox, AVI test
            mailbox_out_msg_0_MSB = 0x0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;

        case MBOX_OPC_GET_STATS_COUNT:
            uint32_t counter_rst = ((mailbox_in_msg_0_MSB & 0x00100000) >> 20); /* bit 52 */
            uint32_t counter_idx = mailbox_in_msg_0_MSB & 0x0000FFFF;           /* bit 47-32*/
            if (counter_idx >= sizeof(g_stats) * 4)
                break;
            mailbox_out_msg_0_MSB = *((uint32_t *)&g_stats + counter_idx);
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            if (counter_rst) {
                memclr((void *)&g_stats, sizeof(g_stats));
            }
            break;

        case MBOX_OPC_DONE_SWRESET:
            extern void start(void);
            /* Quiesce every DMA channel before the ACK: a late DMA
             * completion or GO event after entry(start) would restart the
             * old image (0xF1000000 posted while the host is reloading
             * the VSPA memories). Only the host mailbox can wake it now. */
            dmac_abort();
            dmac_clear();
            entry(start);
            host_clear();
            mailbox_out_msg_0_MSB = 0x0;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            __idle();

            break;

        case MBOX_OPC_PROXY_OFFSET:
            uint32_t proxy_offset_read_only = ((mailbox_in_msg_0_MSB & 0x00100000) >> 20); /* bit 52 */
            if (!proxy_offset_read_only) {
                g_iqflood_proxy_offset = mailbox_in_msg_0_LSB;
                tx_proxy_updated = 1;
                rx_proxy_updated = 1;
                tx_vspa_proxy.gbl_stats_fetch = 1;
            }
            mailbox_out_msg_0_MSB = g_iqflood_proxy_offset;
            mailbox_out_msg_0_LSB = 0x1;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;

        case 0x70:
            /* modem_info init ping (same handshake as setup()): ACK with 0xF0700000 */
            host_mbox0_post(MAKEDWORD(0xF0700000, 0x0));
            break;

        default:
            // not a valid command, NACK
            mailbox_out_msg_0_MSB = 0x0;
            mailbox_out_msg_0_LSB = 0x0;
            host_mbox0_post(MAKEDWORD(mailbox_out_msg_0_MSB, mailbox_out_msg_0_LSB));
            break;
        }
    }

    if (mailbox_in_0_status & 0x8) {
        msg64 = host_mbox1_read();
        mailbox_in_msg_1_MSB = HIWORD(msg64);
        mailbox_in_msg_1_LSB = LOWORD(msg64);
        host_mbox1_clear(); //(host_sent_msg1_go)
        op_mode = (mbox_opc_e)((mailbox_in_msg_1_MSB & 0xFF000000) >> 24);
        switch (op_mode) {
        case MBOX_OPC_EMPTY_2:
            /* does nothing but ACK, mailbox/AVI test */
            mailbox_out_msg_1_MSB = 0x0;
            mailbox_out_msg_1_LSB = 0x1;
            host_mbox1_post(MAKEDWORD(mailbox_out_msg_1_MSB, mailbox_out_msg_1_LSB));
            break;
        case MBOX_OPC_TX_GO:
        case MBOX_OPC_RX_GO:
            /* Flow-control wakeup from libiqplayer.  The host has already
             * written the updated produced/consumed counter into the BAR2
             * proxy before posting this mbox1 message.  No ACK is needed;
             * fall through to PUSH_TX/RX_DATA() at the bottom of the loop. */
            break;
        default:
            /* not a valid command, NACK */
            mailbox_out_msg_1_MSB = 0x0;
            mailbox_out_msg_1_LSB = 0x0;
            host_mbox1_post(MAKEDWORD(mailbox_out_msg_1_MSB, mailbox_out_msg_1_LSB));
            break;
        }
    }

#if !defined(IQMOD_RX_0T1R) && !defined(IQMOD_RX_0T4R)
    PUSH_TX_DATA();
#endif
#ifndef IQMOD_RX_1T0R
    PUSH_RX_DATA();
#endif

#ifdef LOOP_CYCLE_METER
    meter_end(&g_stats.meter[METER_LOOP_BASE], lm_t0, LOOP_PASS_BUDGET);
    g_stats.meter[METER_LOOP_BUDGET] = LOOP_PASS_BUDGET;
#endif

    /* Trace idle entry then halt VCPU until a GO event (mailbox or DMA).
     * Without __idle() the loop spins and floods the trace buffer with
     * ENTRY_IDLE every ~300 cycles even with no traffic. */
#ifdef RX_HOT_TRACE
    l1_trace(L1_TRACE_MSG_ENTRY_IDLE, (uint32_t)iord(DMA_FIFO_STAT));
#endif
#ifndef IS_SIMULATOR
    __idle();
#endif

    //	__swbreak();
    } /* while(1) */
}
