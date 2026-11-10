/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef DFE_H_
#define DFE_H_

#include "txiqcomp.h"
#include "gpio.h"
#include "chip-la9310.h"
#include "axiq-la9310.h"

/**
 *  RX and TX kernel selection for QEC
 *  same kernel is used for Tx and Rx as far as no decimation is required.
 *  possible options :
 *   txiqcomp			 IQ phase and amplitude compensation
 *   txiqcomp_x32chf_5t  IQ phase and amplitude + IQ fractional delay compensation
 */
//#define TXIQCOMP2
//#define RXIQCOMP2
#define TXIQCOMP
#define RXIQCOMP

#ifdef TXIQCOMP2
extern structTXIQCompParams2 iq_comp_params2_tx _VSPA_VECTOR_ALIGN;
#else
extern structTXIQCompParams txiqcompcfg_struct _VSPA_VECTOR_ALIGN;
#endif

/* Number of RX QEC parameter sets: one per RX channel of the build. */
#if defined(IQMOD_RX_1T4R) || defined(IQMOD_RX_0T4R)
#define QEC_RX_NCH 4
#elif defined(IQMOD_RX_1T2R)
#define QEC_RX_NCH 2
#else
#define QEC_RX_NCH 1
#endif
#define QEC_TX_NCH 1

/* QEC mailbox channel field (MBOX_OPC_IQ_CORR, MBOX_OPC_RX/TX_DCO_CORR), in the
 * message MSB word: bits 17-16 (msg bits 49-48) channel, bit 18 (msg bit 50)
 * apply to all channels. */
#define QEC_MBOX_CH_MASK  0x00030000u
#define QEC_MBOX_CH_SHIFT 16
#define QEC_MBOX_ALL      0x00040000u

/* Apply an RX QEC mailbox message (dc=1: RX_DCO_CORR, 0: IQ_CORR) to the
 * selected channel(s). Returns 0 when the channel is out of range. */
uint32_t rx_qec_mbox_apply(uint32_t msb, uint32_t val, uint32_t dc);
/* TX has one QEC set: only channel 0 or "all" is accepted. */
#define QEC_TX_CH_OK(msb) (((msb) & QEC_MBOX_ALL) || !((msb) & QEC_MBOX_CH_MASK))

#ifdef RXIQCOMP2
/* txiqcomp_x32chf_5t keeps a single circular input buffer: one set only. */
extern structTXIQCompParams2 iq_comp_params2_rx _VSPA_VECTOR_ALIGN;
#else
/* txiqcomp needs its config inside one DMEM line: pad each set to a line. */
typedef struct {
    structTXIQCompParams p;
    float pad[10];
} _VSPA_VECTOR_ALIGN rx_qec_ch_t;
extern rx_qec_ch_t rxiqcompcfg_ch[QEC_RX_NCH];
#define RX_QEC_CFG(ch) (&rxiqcompcfg_ch[(ch)].p)
#endif

void rf_init(void);
void rf_update_iq_comp_params2(structTXIQCompParams2 *params_ptr, uint32_t rst, uint32_t idx, uint32_t val);
void rf_update_iq_comp_params(structTXIQCompParams *params_ptr, uint32_t rst, uint32_t idx, uint32_t val);
void stream_write_ptr_rst(uint32_t dma_channel_wr, uint32_t axi_wr);
void stream_read_ptr_rst(uint32_t dma_channel_rd, uint32_t axi_rd);
void stream_write(uint32_t dma_channel_wr, uint32_t axi_wr, uint32_t vsp);
void stream_read(uint32_t dma_channel_rd, uint32_t axi_rd, uint32_t vsp);

#endif /* DFE_H_ */
