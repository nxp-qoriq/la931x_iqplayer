/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef __L1_TRACE_H__
#define __L1_TRACE_H__

#ifdef IQMOD_RX_1T1R
#define L1_TRACE 1
#define L1_TRACE_SIZE 100
#endif
#ifdef IQMOD_RX_1T2R
#define L1_TRACE 1
/* 40 (was 50): frees .vdram for the t_stats cycle meters */
#ifndef L1_TRACE_SIZE
#define L1_TRACE_SIZE 40
#endif
#endif
#ifdef IQMOD_RX_1T4R
#define L1_TRACE 1
#ifndef L1_TRACE_SIZE
#define L1_TRACE_SIZE 100
#endif
#endif
#ifdef IQMOD_RX_0T4R
#define L1_TRACE 1
/* reduced from 100: tight .vdram (16 KB) */
#ifndef L1_TRACE_SIZE
#define L1_TRACE_SIZE 300
#endif
#endif
#ifdef IQMOD_RX_0T1R
#define L1_TRACE 1
/* reduced from 100: tight .vdram (16 KB) */
#ifndef L1_TRACE_SIZE
#define L1_TRACE_SIZE 50
#endif
#endif
#ifdef IQMOD_RX_1T0R
#define L1_TRACE 1
/* reduced from 100: tight .vdram (16 KB) */
#ifndef L1_TRACE_SIZE
#define L1_TRACE_SIZE 50
#endif
#endif

/* No-wrap trace mode: when defined as 1, tracing stops when the buffer
 * is full instead of wrapping back to index 0.  Default ON so the
 * startup sequence is preserved in the trace buffer for debugging.
 * Override with -DL1_TRACE_NOWRAP=0 at compile time to re-enable wrap
 * for long-running steady-state capture. */
#ifndef L1_TRACE_NOWRAP
#define L1_TRACE_NOWRAP 1
#endif

#define L1_TRACE_HOST_SIZE 100
#define L1_TRACE_M7_SIZE 100

enum l1_trace_msg_type {
    L1_TRACE_MSG_DMA = 0x100,
    L1_TRACE_MSG_L1C = 0x200,
    L1_TRACE_MSG_L1APP = 0x300,
    L1_TRACE_MSG_OVLY = 0xA00,
    L1_TRACE_MSG_IPPU = 0xB00,
    L1_TRACE_MSG_ENTRY = 0xC00,
    L1_TRACE_MSG_COMMON = 0xD00,
};

/**
 * L1 trace DMA:
 */
enum l1_trace_msg_axiq {
    /* 0x100 */ L1_TRACE_MSG_DMA_XFR = L1_TRACE_MSG_DMA,
    /* 0x101 */ L1_TRACE_MSG_DMA_CFGERR,
    /* 0x102 */ L1_TRACE_MSG_DMA_AXIQ_RX_START,
    /* 0x103 */ L1_TRACE_MSG_DMA_AXIQ_TX_START,
    /* 0x104 */ L1_TRACE_MSG_DMA_AXIQ_RX_OVER,
    /* 0x105 */ L1_TRACE_MSG_DMA_AXIQ_RX_UNDER,
    /* 0x106 */ L1_TRACE_MSG_DMA_AXIQ_TX_OVER,
    /* 0x107 */ L1_TRACE_MSG_DMA_AXIQ_TX_UNDER,
    /* 0x108 */ L1_TRACE_MSG_DMA_AXIQ_RX_COMP,
    /* 0x109 */ L1_TRACE_MSG_DMA_AXIQ_TX_COMP,
    /* 0x10A */ L1_TRACE_MSG_DMA_AXIQ_RX_XFER_ERROR,
    /* 0x10B */ L1_TRACE_MSG_DMA_AXIQ_TX_XFER_ERROR,
    /* 0x10C */ L1_TRACE_MSG_DMA_XFR_SIZE,
    /* 0x10D */ L1_TRACE_MSG_DMA_DDR_RD_START,
    /* 0x10E */ L1_TRACE_MSG_DMA_DDR_RD_COMP,
    /* 0x10F */ L1_TRACE_MSG_DMA_DDR_RD_UNDERRUN,
    /* 0x110 */ L1_TRACE_MSG_DMA_DDR_WR_START,
    /* 0x111 */ L1_TRACE_MSG_DMA_DDR_WR_COMP,
    /* 0x112 */ L1_TRACE_MSG_DMA_DDR_WR_OVERRUN,
    /* 0x113 */ L1_TRACE_MSG_DMA_AXIQ_RX_AVAIL,    /* param = dmac_is_available() after stream_read enqueue */
    /* 0x114 */ L1_TRACE_MSG_DMA_STRESS_START,     /* param = (dir<<8)|nb_dma: stress test dir/chan count */
    /* 0x115 */ L1_TRACE_MSG_DMA_DDR_WR_COMP_STAT, /* param = dmac_is_complete(mask) raw: 0 = not done */
    /* 0x116 */ L1_TRACE_MSG_DMA_DDR_WR_ERRXFR,    /* param = dmac_errxfr(mask): nonzero = transfer error */
    /* 0x117 */ L1_TRACE_MSG_EXT_BUFF_BUSY,        /* external flow control */
    /* 0x118 */ L1_TRACE_MSG_EXT_BUFF_FC_UPDATE,   /* external flow control update */
    /* 0x119 */ L1_TRACE_MSG_AXIQ_BUFF,            /* AXIQ available RX buffer */
};

/**
 * L1 trace L1C:
 */
enum l1_trace_msg_l1c {
    /* 0x200 */ L1_TRACE_MSG_L1C_SSB_SCAN = L1_TRACE_MSG_L1C,
    /* 0x201 */ L1_TRACE_MSG_L1C_SSB_STOP,
    /* 0x202 */ L1_TRACE_MSG_L1C_DL_MSG,
    /* 0x203 */ L1_TRACE_MSG_L1C_DL_SLOTCONFIG,
    /* 0x204 */ L1_TRACE_MSG_L1C_DL_SEMISTATIC,
    /* 0x205 */ L1_TRACE_MSG_L1C_DL_SLOTUPDATE,
    /* 0x206 */ L1_TRACE_MSG_L1C_UL_START,
    /* 0x207 */ L1_TRACE_MSG_L1C_UL_MSG,
    /* 0x208 */ L1_TRACE_MSG_L1C_DL_CONSUME,
    /* 0x209 */ L1_TRACE_MSG_L1C_UL_CONSUME,
    /* 0x20A */ L1_TRACE_MSG_L1C_FREE,
};

/**
 * L1 trace L1APP:
 */
enum l1_trace_msg_l1app {
    /* 0x300 */ L1_TRACE_L1APP_FETCH_NEXT_TC = L1_TRACE_MSG_L1APP,
    /* 0x301 */ L1_TRACE_L1APP_TX_QEC_START,
    /* 0x302 */ L1_TRACE_L1APP_TX_QEC_COMP,
    /* 0x303 */ L1_TRACE_L1APP_RX_QEC_START,
    /* 0x304 */ L1_TRACE_L1APP_RX_QEC_COMP,
    /* 0x305 */ L1_TRACE_L1APP_BUFF_ID_HS,
    /* 0x306 */ L1_TRACE_L1APP_SLOT_ID,
    /* 0x307 */ L1_TRACE_L1APP_SYM_ID,
    /* 0x308 */ L1_TRACE_L1APP_LAST_SYM,
    /* 0x309 */ L1_TRACE_L1APP_BUFF_ID_OUT,
    /* 0x30a */ L1_TRACE_L1APP_RX_DEC_START,
    /* 0x30b */ L1_TRACE_L1APP_RX_DEC_COMP,
    /* 0x30c */ L1_TRACE_L1APP_RX_CMP_START,
    /* 0x30d */ L1_TRACE_L1APP_RX_CMP_COMP,
    /* 0x30e */ L1_TRACE_L1APP_TX_INTERP_START,
    /* 0x30f */ L1_TRACE_L1APP_TX_INTERP_COMP,
};

/**
 * L1 trace OVLY:
 */
enum l1_trace_msg_ovly {
    /* 0xA00 */ L1_TRACE_MSG_OVLY_SSB = L1_TRACE_MSG_OVLY,
    /* 0xA01 */ L1_TRACE_MSG_OVLY_DL,
    /* 0xA02 */ L1_TRACE_MSG_OVLY_UL,
    /* 0xA03 */ L1_TRACE_MSG_OVLY_ERR,
    /* 0xA04 */ L1_TRACE_MSG_OVLY_DL_DYNAMIC
};

/**
 * L1 trace IPPU:
 */
enum l1_trace_msg_ippu {
    /* 0xB00 */ L1_TRACE_MSG_IPPU_ERROR = L1_TRACE_MSG_IPPU
};

/**
 * L1 trace entry:
 */
enum l1_trace_msg_entry {
    /* 0xC00 */ L1_TRACE_MSG_ENTRY_IDLE = L1_TRACE_MSG_ENTRY,
    /* 0xC01 */ L1_TRACE_MSG_ENTRY_MAIN,
    /* 0xC02 */ L1_TRACE_MSG_ENTRY_SSB,
    /* 0xC03 */ L1_TRACE_MSG_ENTRY_DL_A,
    /* 0xC04 */ L1_TRACE_MSG_ENTRY_DL_B,
    /* 0xC05 */ L1_TRACE_MSG_ENTRY_DL_C,
    /* 0xC06 */ L1_TRACE_MSG_ENTRY_UL_A,
    /* 0xC07 */ L1_TRACE_MSG_ENTRY_UL_B,
    /* 0xC08 */ L1_TRACE_MSG_ENTRY_UL_C,
    /* 0xC09 */ L1_TRACE_MSG_ENTRY_MAIN_DMASTAT = 0xC09, /* param = DMA_GO_STAT channel completion bitmask */
                                                         /* 0xC0A - 0xC0F: reserved */
    /* 0xC10 */ L1_TRACE_MSG_SPM_MBOX0_RECV = 0xC10,    /* param = mbox0 MSB */
    /* 0xC11 */ L1_TRACE_MSG_SPM_MBOX0_ACK,             /* param = ack MSB posted */
    /* 0xC12 */ L1_TRACE_MSG_SPM_MBOX1_RECV,            /* param = mbox1 MSB */
    /* 0xC13 */ L1_TRACE_MSG_SPM_MBOX1_ACK,             /* param = ack MSB posted */
};

/**
 * L1 error code:
 */
enum l1_trace_msg_common {
    /* 0xD00 */ L1_TRACE_MSG_EXCEPTION = L1_TRACE_MSG_COMMON, //!< Generic exception with code.
    /* 0xD01 */ L1_TRACE_MSG_AXIS_DLA2DLB,
    /* 0xD02 */ L1_TRACE_MSG_AXIS_DLB2DLC_DLC,
    /* 0xD03 */ L1_TRACE_MSG_AXIS_DLA2DLB_SSB240,
    /* 0xD04 */ L1_TRACE_MSG_DL_B2A_UL_MSG,
    /* 0xD05 */ L1_TRACE_MSG_BUILD_VER,      /* param = MMDDHHmm decimal build timestamp */
    /* 0xD06 */ L1_TRACE_MSG_TX_CMD,         /* TX_IQ_DATA_FROM_DDR entry: param = mailbox_in_msg_0_MSB */
    /* 0xD07 */ L1_TRACE_MSG_RX_CMD,         /* RX_IQ_DATA_TO_DDR entry:  param = mailbox_in_msg_0_MSB */
    /* 0xD08 */ L1_TRACE_MSG_MBOX0_RECV_MSB, /* mbox0 command received: param = mailbox_in_msg_0_MSB */
    /* 0xD09 */ L1_TRACE_MSG_MBOX0_RECV_LSB, /* mbox0 command received: param = mailbox_in_msg_0_LSB */
    /* 0xD0A */ L1_TRACE_MSG_MBOX0_ACK,      /* mbox0 ACK posted:  param = mailbox_out_msg_0_MSB */
    /* 0xD0B */ L1_TRACE_MSG_MBOX0_NACK,     /* mbox0 NACK posted: param = mailbox_in_msg_0_MSB */
    /* 0xD0C */ L1_TRACE_MSG_MBOX1_RECV_MSB, /* mbox1 command received: param = mailbox_in_msg_1_MSB */
    /* 0xD0D */ L1_TRACE_MSG_MBOX1_RECV_LSB, /* mbox1 command received: param = mailbox_in_msg_1_LSB */
    /* 0xD0E */ L1_TRACE_MSG_MBOX1_ACK,      /* mbox1 ACK posted:  param = mailbox_out_msg_1_MSB */
    /* 0xD0F */ L1_TRACE_MSG_MBOX1_NACK,     /* mbox1 NACK posted: param = mailbox_in_msg_1_MSB */
};

#ifndef __VSPA__

typedef struct l1_trace_data_s {
    uint64_t cnt;
    uint32_t msg;
    uint32_t param;
} l1_trace_data_t;

typedef struct l1_trace_code_s {
    uint32_t msg;
    char text[100];
} l1_trace_code_t;

void l1_trace(uint32_t msg, uint32_t param);
void l1_trace_clear(void);

#else
#include "ccnt.h"

typedef struct l1_trace_data_s {
    ccnt_t cnt;
    uint32_t msg;
    uint32_t param;
} l1_trace_data_t;

#pragma cplusplus on
#if L1_TRACE
void l1_trace(uint32_t msg, uint32_t param);
void l1_trace(uint32_t msg);
void l1_trace_nr(uint32_t msg);
void l1_trace_nr(uint32_t msg, uint32_t param);
void l1_trace_clear(void);

#else  // L1_TRACE
inline void l1_trace_clear(void) {}

inline void l1_trace(uint32_t msg) {}
inline void l1_trace(uint32_t msg, uint32_t param) {}
inline void l1_trace_nr(uint32_t msg) {}
inline void l1_trace_nr(uint32_t msg, uint32_t param) {}
#endif // L1_TRACE

#if defined(__DEBUG__)
inline void l1_trace_dbg(uint32_t msg, uint32_t param) { l1_trace(msg, param); }

inline void l1_trace_dbg(uint32_t msg) { l1_trace(msg); }
#else  // __DEBUG__
inline void l1_trace_dbg(uint32_t, uint32_t) {}

inline void l1_trace_dbg(uint32_t) {}
#endif //__DEBUG__
#pragma cplusplus reset
#endif

#ifndef __M7__
extern volatile l1_trace_data_t l1_trace_data[] __attribute__((aligned(64)));
#else
extern volatile l1_trace_data_t *l1_trace_data;
#endif
extern volatile uint32_t l1_trace_index;
extern volatile uint32_t l1_trace_disable;

#endif // __L1_TRACE_H__
