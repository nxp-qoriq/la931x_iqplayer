/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright 2026 NXP */
/*
 * rx_qec_decx2_8t: x2 decimation (8-tap FIR) and RX QEC in one call.
 *
 * Same call signature as decimator_2x_8_Taps_asm, but pTaps points to a
 * rx_qec_decx2_cfg_t prepared once with rx_qec_decx2_prepare():
 *   - taps are stored scaled by 1/2 so the filter output never saturates
 *     before QEC (raw output reaches 1.13 on full scale input)
 *   - QEC runs on the decimated samples with doubled gains and the DC
 *     offset scaled by sum(h):
 *        I' = dc_re*sum(h) + 2ci*I2
 *        Q' = dc_im*sum(h) + 2cq*Q2 + 2ci2q*I2
 * n_samples: input samples, multiple of 64. Output: n_samples/2 samples.
 */
#ifndef RX_QEC_DECX2_H
#define RX_QEC_DECX2_H

#include <stdint.h>

typedef struct {
    float taps_half[8];    /* line 0: FIR taps * 0.5 */
    float pad0[24];
    float qec[6];          /* line 1: txiqcomp layout {dc_re, dc_im, 0, ci2q, ci, cq} */
    float pad1[26];
} __attribute__((aligned(128))) rx_qec_decx2_cfg_t;

void rx_qec_decx2_8t(void *pOut, void *pIn, rx_qec_decx2_cfg_t *pCfg,
                     void *filtState, unsigned int n_samples);

/* Build the kernel config from FIR taps and a txiqcomp QEC config
 * {dc_re, dc_im, 0, ci2q, ci, cq}. Scalar, call at configuration time. */
static inline void rx_qec_decx2_prepare(rx_qec_decx2_cfg_t *c, const float *taps,
                                        const float *qec)
{
    int k;
    float sh = 0.0f;
    for (k = 0; k < 8; k++) {
        c->taps_half[k] = 0.5f * taps[k];
        sh += taps[k];
    }
    c->qec[0] = qec[0] * sh;
    c->qec[1] = qec[1] * sh;
    c->qec[2] = 0.0f;
    c->qec[3] = 2.0f * qec[3];
    c->qec[4] = 2.0f * qec[4];
    c->qec[5] = 2.0f * qec[5];
}

#endif
