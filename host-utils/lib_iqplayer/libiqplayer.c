/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <assert.h>
#include <semaphore.h>
#include <signal.h>
#include <argp.h>

#include "vspa_dmem_proxy.h"
#include "imx8-host.h"
#include "l1-trace-host.h"
#include "lib_iqplayer_api.h"

uint32_t *v_iqflood_ddr_addr = NULL;
uint32_t *BAR2_addr = NULL;
uint32_t *v_vspa_dmem_proxy_ro = NULL;
t_tx_ch_host_proxy *tx_vspa_proxy_ro = NULL;
t_rx_ch_host_proxy *rx_vspa_proxy_ro = NULL;
uint32_t *v_tx_vspa_proxy_wo;
uint32_t *v_rx_vspa_proxy_wo;
t_stats *app_stats = NULL;

#define TX_DDR_STEP (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.tx_ddr_step)
#define RX_DDR_STEP (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.rx_ddr_step)
#define RX_DECIM    (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.rx_decim)
#define TX_UPSMP    (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.tx_upsmp)
#define PROXY_CFG   (((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly.rx_num_chan)
#define RX_NUM_CHAN PROXY_CFG_NRX(PROXY_CFG)

/* -----------------------------------------------------------------------
 * VSPA mbox1 GO-event support
 *
 * The VSPA VCPU sleeps waiting for a GO event.  A direct PCIe AHB write to
 * VSPA DMEM does NOT wake the VCPU.  Posting a message on mbox1 (host->VSPA)
 * is required in addition to the BAR2 proxy flow-control write.
 *
 * Mbox1 host->VSPA register layout (from vspa_mbox.c MAILBOX_ADDR macro):
 *   base  = ccsr_phys + 0x1000000          (VSPA register space)
 *   mbox1 host->VSPA MSB word = base + 0x688 + core_id * 0x4000
 *   mbox1 host->VSPA LSB word = base + 0x68C + core_id * 0x4000
 *
 * Posting: write MSB first, then LSB.  Writing LSB triggers the interrupt.
 * TX opcode: 0xF2000000 | 0 (MSB), produced_size (LSB).
 * RX opcode: 0xF3000000 | 0 (MSB), consumed_size (LSB).
 *
 * If ccsr_phys == 0 at init time the mbox mapping is skipped and the library
 * falls back to BAR2-write-only (backward-compatible).
 * ----------------------------------------------------------------------- */

#define VSPA_REG_OFFSET    0x1000000ULL
#define MBOX1_HOST2VSPA_MSB_OFF  0x688ULL
#define MBOX1_CORE_STRIDE        0x4000ULL

#define MBOX_TX_OPCODE  0xF2000000U
#define MBOX_RX_OPCODE  0xF3000000U

/* Per-channel cached volatile pointer to la9310_fifo_consumed_size in VSPA DMEM.
 * Set once at iq_player_init_rx() from the AHB address VSPA exports in
 * tx_vspa_proxy_ro->rx_fc_addr[chan].  rx_fc_notify() is then a single write. */
static volatile uint32_t *v_rx_fc_ptr[RX_NUM_MAX_CHAN];

static uint64_t  g_ccsr_phys   = 0;
static uint32_t  g_core_id     = 0;
static void     *g_mbox1_tx_map = NULL;   /* mmap base for the 8-byte mbox window */
static uint32_t *v_mbox1_tx_msb = NULL;  /* MSB register virtual address */
static uint32_t *v_mbox1_tx_lsb = NULL;  /* LSB register virtual address */

/*
 * map_mbox1_tx - map the 8-byte mbox1 host->VSPA register window.
 * Returns 0 on success, -1 on failure (library will fall back to BAR2-only).
 */
static int map_mbox1_tx(void)
{
    uint64_t mbox_phys;
    long     page_size;
    uint64_t page_base;
    uint32_t page_offset;
    int      fd;

    if (g_ccsr_phys == 0)
        return 0; /* no mbox requested */

    mbox_phys = g_ccsr_phys + VSPA_REG_OFFSET +
                MBOX1_HOST2VSPA_MSB_OFF +
                (uint64_t)g_core_id * MBOX1_CORE_STRIDE;

    page_size   = sysconf(_SC_PAGESIZE);
    page_base   = mbox_phys & ~((uint64_t)(page_size - 1));
    page_offset = (uint32_t)(mbox_phys - page_base);

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("libiqplayer: open /dev/mem for mbox1");
        return -1;
    }

    g_mbox1_tx_map = mmap(NULL, (size_t)(page_offset + 8),
                          PROT_READ | PROT_WRITE, MAP_SHARED,
                          fd, (off_t)page_base);
    close(fd);

    if (g_mbox1_tx_map == MAP_FAILED) {
        perror("libiqplayer: mmap mbox1");
        g_mbox1_tx_map = NULL;
        return -1;
    }

    v_mbox1_tx_msb = (uint32_t *)((uint8_t *)g_mbox1_tx_map + page_offset);
    v_mbox1_tx_lsb = v_mbox1_tx_msb + 1;

    printf("libiqplayer: mbox1 mapped at phys=0x%016" PRIx64
           " core=%u msb_virt=%p\n",
           mbox_phys, g_core_id, (void *)v_mbox1_tx_msb);
    fflush(stdout);
    return 0;
}

/*
 * tx_fc_notify - update TX flow-control and kick VSPA via mbox1.
 * produced_size : new total bytes produced by the host into the TX FIFO.
 */
static inline void tx_fc_notify(uint32_t produced_size)
{
    /* BAR2 proxy write (existing path) */
    ((t_tx_ch_host_proxy *)v_tx_vspa_proxy_wo)->host_produced_size = produced_size;

    /* mbox1 GO-event kick */
    if (v_mbox1_tx_msb) {
        *v_mbox1_tx_msb = MBOX_TX_OPCODE;
        __asm__ volatile("" ::: "memory");
        *v_mbox1_tx_lsb = produced_size;
    }
}

/*
 * rx_fc_notify - update RX flow-control and kick VSPA via mbox1.
 * chan          : RX channel index.
 * consumed_size : new total bytes consumed by the host from the RX FIFO.
 *
 * v_rx_fc_ptr[chan] is cached once at iq_player_init_rx() so this path
 * is a single volatile store at runtime with no address arithmetic.
 * If the pointer is NULL (VSPA not ready at init time) the write is skipped.
 */
/* Serialises the MSB/LSB mbox1 register pair: RX may be drained by two
 * threads (split drain) and an interleaved pair would be corrupt. */
static pthread_mutex_t g_mbox1_lock = PTHREAD_MUTEX_INITIALIZER;

/* RX flow-control diagnostics, one slot per channel. Each channel is drained
 * by a single thread, so the counters need no locking.
 *   IQ_RX_ACK_MIN (bytes, default 0): skip an ack until at least this much
 *                 was consumed since the last one
 *   IQ_RX_KICK    (default 1): 0 = do not post the mbox1 GO kick on acks */
typedef struct {
    uint64_t acks;          /* acks written to VSPA */
    uint64_t skipped;       /* acks skipped by IQ_RX_ACK_MIN */
    uint64_t notify_ns;     /* time spent in rx_fc_notify */
    uint32_t last_acked;    /* consumed value last written */
    uint32_t max_lag;       /* max produced - consumed seen by host */
    uint32_t max_mirror;    /* max host consumed - VSPA view of consumed */
    uint32_t pad;
} rx_fc_diag_t;
static rx_fc_diag_t g_rx_diag[RX_NUM_MAX_CHAN];
static int g_rx_ack_min = -1;
static int g_rx_kick    = -1;

static inline uint64_t fc_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void rx_fc_load_env(void)
{
    const char *v;

    if (g_rx_ack_min >= 0)
        return;
    v = getenv("IQ_RX_ACK_MIN");
    g_rx_ack_min = v ? (int)strtol(v, NULL, 0) : 0x8000;
    v = getenv("IQ_RX_KICK");
    g_rx_kick = v ? (int)strtol(v, NULL, 0) : 1;
    fprintf(stderr, "libiqplayer: RX ack_min %d bytes, mbox kick %d\n",
            g_rx_ack_min, g_rx_kick);
}

static inline void rx_fc_notify(uint32_t chan, uint32_t consumed_size)
{
    uint64_t t0 = fc_now_ns();

    g_rx_diag[chan].acks++;
    g_rx_diag[chan].last_acked = consumed_size;
    if (v_rx_fc_ptr[chan])
        *v_rx_fc_ptr[chan] = consumed_size;

    /* mbox1 GO-event kick */
    if (v_mbox1_tx_msb && g_rx_kick != 0) {
        pthread_mutex_lock(&g_mbox1_lock);
        *v_mbox1_tx_msb = MBOX_RX_OPCODE;
        __asm__ volatile("" ::: "memory");
        *v_mbox1_tx_lsb = consumed_size;
        pthread_mutex_unlock(&g_mbox1_lock);
    }
    g_rx_diag[chan].notify_ns += fc_now_ns() - t0;
}

/* Ack only if IQ_RX_ACK_MIN bytes were consumed since the last ack. */
static inline void rx_fc_notify_batched(uint32_t chan, uint32_t consumed_size)
{
    if (g_rx_ack_min > 0 &&
        consumed_size - g_rx_diag[chan].last_acked < (uint32_t)g_rx_ack_min) {
        g_rx_diag[chan].skipped++;
        return;
    }
    rx_fc_notify(chan, consumed_size);
}

void iq_player_print_rx_diag(uint32_t first_chan, uint32_t n, double run_s)
{
    uint32_t c;

    for (c = first_chan; c < first_chan + n && c < RX_NUM_MAX_CHAN; c++) {
        rx_fc_diag_t *d = &g_rx_diag[c];
        fprintf(stderr, "libiqplayer: ch%u acks %llu (%.0f/s) skipped %llu,"
                " notify %.1f%% of run, max lag 0x%x, max VSPA consumed lag 0x%x\n",
                c, (unsigned long long)d->acks,
                run_s > 0 ? (double)d->acks / run_s : 0.0,
                (unsigned long long)d->skipped,
                run_s > 0 ? 100.0 * (double)d->notify_ns / 1e9 / run_s : 0.0,
                d->max_lag, d->max_mirror);
    }
}

int iq_player_init(uint32_t *v_iqflood, uint32_t iqflood_size,
                   uint32_t *v_la9310_pci_bar2,
                   uint64_t ccsr_phys, uint32_t core_id)
{
    v_iqflood_ddr_addr = v_iqflood;

    /* use last 256 bytes of iqflood as shared vspa dmem proxy,
     * vspa will write mirrored dmem value to avoid PCI read from host */
    v_vspa_dmem_proxy_ro = (uint32_t *)(v_iqflood_ddr_addr + (iqflood_size - VSPA_DMEM_PROXY_SIZE) / 4);
    rx_vspa_proxy_ro = &(((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->rx_state_readonly[0]);
    tx_vspa_proxy_ro = &(((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->tx_state_readonly);
    app_stats = &(((t_vspa_dmem_proxy *)v_vspa_dmem_proxy_ro)->app_stats);
    BAR2_addr = v_la9310_pci_bar2;

    /* use dmem structure at hardcoded address to write host status/request */
    dccivac((uint32_t *)tx_vspa_proxy_ro);
    /* VSPA DMEM AHB slave window is always BAR2+0x400000 regardless of channel count. */
    v_tx_vspa_proxy_wo = (uint32_t *)((uint64_t)BAR2_addr + 0x400000 + 0x00000000);
    v_rx_vspa_proxy_wo = (uint32_t *)((uint64_t)BAR2_addr + 0x400000 + 0x00000040);

    /* mbox1 GO-event mapping (optional; falls back gracefully if ccsr_phys==0) */
    g_ccsr_phys = ccsr_phys;
    g_core_id   = core_id;
    map_mbox1_tx();

    return 1;
}

uint32_t app_TX_total_produced_size = 0; /* bytes copied into modem TX FIFO */
uint32_t app_TX_total_consumed_size = 0; /* bytes sent out of modem TX FIFO */
uint32_t tx_modem_ddr_fifo_start;
uint32_t tx_modem_ddr_fifo_size;
uint32_t tx_modem_fifo_offset;

int iq_player_init_tx(uint32_t fifo_start, uint32_t fifo_size)
{
    (void)VSPA_stat_gbl_string; /* suppress unused-variable warnings */
    (void)VSPA_stat_tx_string;
    (void)VSPA_stat_rx_string;

    if (v_iqflood_ddr_addr == NULL)
        return 0;

    dccivac((uint32_t *)(tx_vspa_proxy_ro));

    /* init fifo pointers */
    tx_modem_ddr_fifo_start = fifo_start;
    tx_modem_ddr_fifo_size  = fifo_size;
    tx_modem_fifo_offset    = tx_vspa_proxy_ro->la9310_fifo_enqueued_size % fifo_size;

    /* init flow control */
    app_TX_total_consumed_size = tx_vspa_proxy_ro->la9310_fifo_enqueued_size;
    app_TX_total_produced_size = tx_vspa_proxy_ro->la9310_fifo_enqueued_size;
    tx_fc_notify(app_TX_total_produced_size);

    return 1;
}

int iq_player_send_data(uint32_t *v_buffer, uint32_t size)
{
    uint32_t fifoWaterMark = 0;
    uint32_t busy_size = 0;
    uint32_t empty_size = 0;
    void *ddr_dst;

    dccivac((uint32_t *)(tx_vspa_proxy_ro));

    /* check stop/restart */
    if (tx_vspa_proxy_ro->DDR_rd_base_address == 0xdeadbeef) {
        tx_modem_fifo_offset       = 0;
        app_TX_total_consumed_size = 0;
        app_TX_total_produced_size = 0;
        tx_fc_notify(0);
        return 0;
    }

    /* check new transfer opportunity */
    app_TX_total_consumed_size = tx_vspa_proxy_ro->la9310_fifo_enqueued_size;
    busy_size = app_TX_total_produced_size - app_TX_total_consumed_size;
    if ((int32_t)busy_size < 0) {
        /* VSPA fetched past what the host produced: VSPA flow control was
         * not synchronised with this host session (stale host_produced_size
         * from a previous run). Data sent to the DAC is garbage. */
        printf("\n TX VSPA read ahead of host (stale flow control), exit (busy=0x%08x app_TX_total_produced_size=0x%08x app_TX_total_consumed_size=0x%08x)\n",
               busy_size, app_TX_total_produced_size, app_TX_total_consumed_size);
        fflush(stdout);
        exit(0);
    }
    if (busy_size > tx_modem_ddr_fifo_size) {
        printf("\n TX underrun , exit (busy=0x%08x app_TX_total_produced_size=0x%08x app_TX_total_consumed_size=0x%08x)\n",
               busy_size, app_TX_total_produced_size, app_TX_total_consumed_size);
        fflush(stdout);
        exit(0);
    }
    fifoWaterMark = tx_modem_ddr_fifo_size - tx_modem_fifo_offset;
    empty_size    = tx_modem_ddr_fifo_size - busy_size;
    if (empty_size >= TX_DDR_STEP) {
        if (empty_size > fifoWaterMark)
            empty_size = fifoWaterMark;
        if (empty_size > size)
            empty_size = size;
        /* room available: advance produced pointer */
        app_TX_total_produced_size += empty_size;
        app_stats->tx_stats[STAT_EXT_DMA_DDR_RD] += empty_size / TX_DDR_STEP;
    } else {
        /* no room in TX FIFO */
        return 0;
    }

    /* copy data into DDR FIFO */
    ddr_dst = (void *)((uint64_t)v_iqflood_ddr_addr + tx_modem_ddr_fifo_start + tx_modem_fifo_offset);
    l1_trace(L1_TRACE_MSG_DMA_DDR_RD_START, tx_modem_ddr_fifo_start + tx_modem_fifo_offset);
    memcpy(ddr_dst, v_buffer, empty_size);
    flush_region(ddr_dst, empty_size);
    l1_trace(L1_TRACE_MSG_DMA_DDR_RD_COMP, empty_size);

    /* update modem flow control + kick VSPA GO event */
    tx_fc_notify(app_TX_total_produced_size);

    tx_modem_fifo_offset += empty_size;
    if (tx_modem_fifo_offset >= tx_modem_ddr_fifo_size)
        tx_modem_fifo_offset = 0;

    return empty_size;
}

uint32_t rx_modem_ddr_fifo_start[RX_NUM_MAX_CHAN] = { 0, 0, 0, 0 };
uint32_t rx_modem_ddr_fifo_size[RX_NUM_MAX_CHAN]  = { 0, 0, 0, 0 };
uint32_t rx_modem_fifo_offset[RX_NUM_MAX_CHAN]    = { 0, 0, 0, 0 };
uint32_t app_RX_total_consumed_size[RX_NUM_MAX_CHAN] = { 0, 0, 0, 0 };
uint32_t app_RX_total_produced_size[RX_NUM_MAX_CHAN] = { 0, 0, 0, 0 };

/* -----------------------------------------------------------------------
 * RX copy rate limiter (g_rx_copy_rate): 0-100 percent of steps do memcpy
 * to the caller buffer.  Skipped steps still advance consumed_size so the
 * VSPA FC gate stays open; only the host memcpy is omitted.
 * Useful to measure DDR throughput without the CPU cost of a full memcpy.
 * Default 100 = copy everything.
 * ----------------------------------------------------------------------- */
static uint32_t g_rx_copy_rate    = 100u;
static __thread uint32_t g_rx_step_counter = 0u; /* per drain thread */

/* Per drain thread copy timing (ns) and copy mode.
 * IQ_RX_COPY=1 selects the overlapped invalidate+copy path. */
static __thread uint64_t g_rx_inval_ns = 0u;
static __thread uint64_t g_rx_copy_ns  = 0u;
static int g_rx_copy_mode = -1;

static inline uint64_t lib_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void iq_player_get_rx_copy_times(uint64_t *inval_ns, uint64_t *copy_ns)
{
    if (inval_ns) *inval_ns = g_rx_inval_ns;
    if (copy_ns)  *copy_ns  = g_rx_copy_ns;
}

void iq_player_set_rx_copy_rate(uint32_t percent)
{
    g_rx_copy_rate    = (percent > 100u) ? 100u : percent;
    g_rx_step_counter = 0u;
    fprintf(stderr, "libiqplayer: RX copy rate set to %u%%\n", g_rx_copy_rate);
}

/* -----------------------------------------------------------------------
 * RX throttle: back-pressure the VSPA DDR pipeline by sleeping after each
 * consume step.
 *   g_rx_throttle_percent  : 0-100; 100 = full speed (no sleep).
 *   g_rx_throttle_ref_mbps : nominal full-rate in MB/s (e.g. 500 for LA9310).
 * Sleep formula per step: us = bytes * (100 - pct) / (pct * ref_mbps)
 * A sleep accumulator is used to avoid tiny usleep() calls that get rounded
 * to the Linux tick period (~4 ms at CONFIG_HZ=250).
 * ----------------------------------------------------------------------- */
static uint32_t g_rx_throttle_percent      = 100u;
static uint32_t g_rx_throttle_ref_mbps     = 500u;
static uint32_t g_rx_throttle_sleep_acc_us = 0u;

void iq_player_set_rx_throttle_rate(uint32_t percent, uint32_t ref_mbps)
{
    g_rx_throttle_percent      = (percent > 100u) ? 100u : percent;
    g_rx_throttle_ref_mbps     = (ref_mbps == 0u) ? 1u : ref_mbps;
    g_rx_throttle_sleep_acc_us = 0u;
    fprintf(stderr, "libiqplayer: RX throttle rate set to %u%% (ref %u MB/s)\n",
            g_rx_throttle_percent, g_rx_throttle_ref_mbps);
}

int iq_player_init_rx(uint32_t chan, uint32_t fifo_start, uint32_t fifo_size)
{
    if (v_iqflood_ddr_addr == NULL)
        return -1;

    dccivac((uint32_t *)(rx_vspa_proxy_ro));

    /* init fifo pointers */
    rx_modem_ddr_fifo_start[chan] = fifo_start;
    rx_modem_ddr_fifo_size[chan]  = fifo_size;
    rx_modem_fifo_offset[chan]    = 0;

    /* Cache the volatile pointer to la9310_fifo_consumed_size in VSPA DMEM.
     * VSPA populates rx_fc_addr[chan] with the AHB address of that field
     * during its own init (_main.c).  Spin briefly for the DDR proxy mirror
     * to reflect the written value (VSPA flushes the proxy before posting the
     * IQ_MOD_RX mailbox ACK, so this loop rarely iterates more than once). */
    {
        uint32_t fc_ahb;
        int retries = 1000;

        do {
            dccivac((uint32_t *)tx_vspa_proxy_ro);
            fc_ahb = tx_vspa_proxy_ro->rx_fc_addr[chan];
        } while (!(fc_ahb >= LA9310_VSPA_DMEM_AHB_BASE &&
                   fc_ahb <  LA9310_VSPA_DMEM_AHB_BASE + LA9310_VSPA_DMEM_AHB_SIZE) &&
                 --retries > 0);

        if (fc_ahb >= LA9310_VSPA_DMEM_AHB_BASE &&
            fc_ahb <  LA9310_VSPA_DMEM_AHB_BASE + LA9310_VSPA_DMEM_AHB_SIZE) {
            v_rx_fc_ptr[chan] = (volatile uint32_t *)
                ((uint8_t *)BAR2_addr + 0x400000u +
                 (fc_ahb - LA9310_VSPA_DMEM_AHB_BASE));
            printf("libiqplayer: rx_fc_ptr[%u] -> BAR2+0x%x (ahb=0x%08x)\n",
                   chan,
                   (unsigned)((fc_ahb - LA9310_VSPA_DMEM_AHB_BASE) + 0x400000u),
                   fc_ahb);
        } else {
            fprintf(stderr,
                    "libiqplayer: WARNING rx_fc_addr[%u]=0x%08x not in VSPA DMEM range"
                    " -- FC writes disabled for this channel\n",
                    chan, fc_ahb);
            v_rx_fc_ptr[chan] = NULL;
        }
    }

    /* Reset flow counters to 0 unconditionally.
     * The proxy DDR mirror may hold stale values from a previous run; the
     * VSPA always restarts its own counter from 0 on a new IQ_MOD_RX command,
     * so reading the stale proxy here would cause a false underrun on the
     * first call to iq_player_receive_data(). */
    app_RX_total_consumed_size[chan] = 0;
    app_RX_total_produced_size[chan] = 0;
    rx_fc_notify(chan, 0);

    return 1;
}

/*
 * iq_player_receive_data_multi - drain up to N channels in one optimised call.
 *
 * Phase 0: one dccivac invalidates all 4 x t_rx_ch_host_proxy (64 bytes = 1
 *          A53 cache line) regardless of how many channels are active.
 *
 * Phase 1: for every channel -- read produced size, compute available data,
 *          handle overflow skip-ahead, advance consumed counter, and send
 *          rx_fc_notify() BEFORE any memcpy.  This opens the VSPA FC gate on
 *          all channels before the (relatively slow) cache invalidation and
 *          data copy begin, giving VSPA maximum head-start.
 *
 * Phase 2: for every channel that has data -- invalidate the DDR region and
 *          memcpy into the caller-supplied destination.  Done after all
 *          FC-acks so the VSPA pipeline is already running on the next chunk.
 *
 * Returns 1 if at least one channel produced data, 0 if all were idle.
 */
int iq_player_receive_data_multi(rx_drain_req_t *reqs, uint32_t n)
{
    uint32_t i;
    int any = 0;

    /* Phase 0: one cache-line invalidation covers all channel proxies. */
    dccivac((uint32_t *)(rx_vspa_proxy_ro));
    rx_fc_load_env();

    /* Phase 1: compute data sizes, advance consumed, send ahead FC-acks. */
    for (i = 0; i < n; i++) {
        rx_drain_req_t *r    = &reqs[i];
        uint32_t        chan = r->chan_id;
        uint32_t        data_size;
        uint32_t        fifoWaterMark;

        r->got = 0;

        /* stop/restart sentinel from VSPA */
        if (rx_vspa_proxy_ro[chan].DDR_wr_base_address == 0xdeadbeef) {
            app_RX_total_produced_size[chan] = 0;
            app_RX_total_consumed_size[chan] = 0;
            rx_modem_fifo_offset[chan]       = 0;
            rx_fc_notify(chan, 0);
            continue;
        }

        app_RX_total_produced_size[chan] = rx_vspa_proxy_ro[chan].la9310_fifo_produced_size;
        data_size = app_RX_total_produced_size[chan] - app_RX_total_consumed_size[chan];
        {
            /* the VSPA mirror also carries its own view of consumed */
            uint32_t mlag = app_RX_total_consumed_size[chan] -
                            rx_vspa_proxy_ro[chan].la9310_fifo_consumed_size;
            if (data_size > g_rx_diag[chan].max_lag &&
                data_size <= rx_modem_ddr_fifo_size[chan])
                g_rx_diag[chan].max_lag = data_size;
            if (mlag > g_rx_diag[chan].max_mirror &&
                mlag <= rx_modem_ddr_fifo_size[chan])
                g_rx_diag[chan].max_mirror = mlag;
        }

        /* overflow skip-ahead: host fell more than ring_size behind */
        if (data_size > rx_modem_ddr_fifo_size[chan]) {
            fprintf(stderr,
                    "libiqplayer: ch%u VSPA DDR ring overflow"
                    " (produced=0x%08x consumed=0x%08x diff=0x%08x ring=0x%08x)"
                    " -- skipping ahead\n",
                    chan,
                    app_RX_total_produced_size[chan],
                    app_RX_total_consumed_size[chan],
                    data_size,
                    rx_modem_ddr_fifo_size[chan]);
            app_RX_total_consumed_size[chan] =
                app_RX_total_produced_size[chan] - rx_modem_ddr_fifo_size[chan];
            rx_modem_fifo_offset[chan] =
                app_RX_total_consumed_size[chan] % rx_modem_ddr_fifo_size[chan];
            rx_fc_notify(chan, app_RX_total_consumed_size[chan]);
            data_size = rx_modem_ddr_fifo_size[chan];
        }

        fifoWaterMark = rx_modem_ddr_fifo_size[chan] - rx_modem_fifo_offset[chan];
        if (data_size < RX_DDR_STEP)
            continue;

        if (data_size > fifoWaterMark) data_size = fifoWaterMark;
        if (data_size > r->max_size)   data_size = r->max_size;

        /* Advance consumed and send FC-ack AHEAD of memcpy.
         * VSPA can immediately begin writing the next DDR chunk while the
         * host is still invalidating cache and copying this one. */
        app_RX_total_consumed_size[chan] += data_size;
        app_stats->rx_stats[chan][STAT_EXT_DMA_DDR_WR] += data_size / RX_DDR_STEP;
        rx_fc_notify_batched(chan, app_RX_total_consumed_size[chan]);

        r->got = (int)data_size;
        any    = 1;
    }

    /* Phase 2: invalidate D-cache and copy data -- done after all FC-acks. */
    for (i = 0; i < n; i++) {
        rx_drain_req_t *r    = &reqs[i];
        uint32_t        chan = r->chan_id;
        if (r->got <= 0)
            continue;

        void *ddr_src = (void *)((uint64_t)v_iqflood_ddr_addr +
                                 rx_modem_ddr_fifo_start[chan] +
                                 rx_modem_fifo_offset[chan]);

        g_rx_step_counter++;
        if (g_rx_copy_rate == 100u ||
            (g_rx_step_counter * g_rx_copy_rate / 100u) >
            ((g_rx_step_counter - 1u) * g_rx_copy_rate / 100u)) {
            uint64_t t0, t1, t2;

            if (g_rx_copy_mode < 0) {
                const char *v = getenv("IQ_RX_COPY");
                g_rx_copy_mode = v ? atoi(v) : 0;
            }
            l1_trace(L1_TRACE_MSG_DMA_DDR_RD_START,
                     rx_modem_ddr_fifo_start[chan] + rx_modem_fifo_offset[chan]);
            t0 = lib_now_ns();
            if (g_rx_copy_mode == 1 && r->dst) {
                invalidate_copy_region(r->dst, ddr_src, (uint32_t)r->got);
                t1 = t0;
            } else {
                invalidate_region(ddr_src, (uint32_t)r->got);
                t1 = lib_now_ns();
                if (r->dst)
                    memcpy(r->dst, ddr_src, (uint32_t)r->got);
            }
            t2 = lib_now_ns();
            g_rx_inval_ns += t1 - t0;
            g_rx_copy_ns  += t2 - t1;
            l1_trace(L1_TRACE_MSG_DMA_DDR_RD_COMP, (uint32_t)r->got);
        }

        rx_modem_fifo_offset[chan] += (uint32_t)r->got;
        if (rx_modem_fifo_offset[chan] >= rx_modem_ddr_fifo_size[chan])
            rx_modem_fifo_offset[chan] = 0;
    }

    return any;
}

int iq_player_receive_data(uint32_t chan, uint32_t *v_buffer, uint32_t max_size)
{
    uint32_t fifoWaterMark = 0;
    uint32_t data_size = 0;
    void *ddr_src;

    /* All 4 x t_rx_ch_host_proxy (4 x 16 = 64 bytes) fit in one A53 cache line.
     * A single DC CIVAC on the array base invalidates all channels at once;
     * this is cheaper and correct regardless of which channel is being drained. */
    dccivac((uint32_t *)(rx_vspa_proxy_ro));

    /* check stop/restart */
    if (rx_vspa_proxy_ro[chan].DDR_wr_base_address == 0xdeadbeef) {
        app_RX_total_produced_size[chan] = 0;
        app_RX_total_consumed_size[chan] = 0;
        rx_modem_fifo_offset[chan]       = 0;
        rx_fc_notify(chan, 0);
        return 0;
    }

    /* check new data: VSPA increments la9310_fifo_produced_size on each DDR DMA
     * completion.  la9310_fifo_consumed_size is the host's own write; reading it
     * back here would always return what we wrote and never advance. */
    app_RX_total_produced_size[chan] = rx_vspa_proxy_ro[chan].la9310_fifo_produced_size;
    data_size = app_RX_total_produced_size[chan] - app_RX_total_consumed_size[chan];
    if (data_size > rx_modem_ddr_fifo_size[chan]) {
        /* VSPA DDR ring overflow: host drain fell more than ring_size behind.
         * The overwritten region is unrecoverable.  Skip consumed ahead to
         * the oldest valid byte (produced - ring_size) so the VSPA FC gate
         * can reopen and streaming can continue.  This is safe in spectrum
         * and sink modes where data loss is acceptable; in capture mode it
         * means a gap in the recorded waveform but avoids a full deadlock. */
        fprintf(stderr,
                "libiqplayer: ch%u VSPA DDR ring overflow"
                " (produced=0x%08x consumed=0x%08x diff=0x%08x ring=0x%08x)"
                " -- skipping ahead\n",
                chan,
                app_RX_total_produced_size[chan],
                app_RX_total_consumed_size[chan],
                data_size,
                rx_modem_ddr_fifo_size[chan]);
        app_RX_total_consumed_size[chan] =
            app_RX_total_produced_size[chan] - rx_modem_ddr_fifo_size[chan];
        rx_modem_fifo_offset[chan] =
            app_RX_total_consumed_size[chan] % rx_modem_ddr_fifo_size[chan];
        rx_fc_notify(chan, app_RX_total_consumed_size[chan]);
        data_size = rx_modem_ddr_fifo_size[chan];
    }

    fifoWaterMark = rx_modem_ddr_fifo_size[chan] - rx_modem_fifo_offset[chan];
    if (data_size >= RX_DDR_STEP) {
        if (data_size > fifoWaterMark)
            data_size = fifoWaterMark;
        if (data_size > max_size)
            data_size = max_size;
        /* advance consumed pointer */
        app_RX_total_consumed_size[chan] += data_size;
        app_stats->rx_stats[chan][STAT_EXT_DMA_DDR_WR] += data_size / RX_DDR_STEP;
    } else {
        /* no data available */
        return 0;
    }

    /* copy data from DDR FIFO (optionally skipped by copy rate limiter) */
    ddr_src = (void *)((uint64_t)v_iqflood_ddr_addr + rx_modem_ddr_fifo_start[chan] + rx_modem_fifo_offset[chan]);
    g_rx_step_counter++;
    /* perform memcpy if: copy rate == 100, or (step_counter * copy_rate / 100) has advanced */
    if (g_rx_copy_rate == 100u ||
        (g_rx_step_counter * g_rx_copy_rate / 100u) > ((g_rx_step_counter - 1u) * g_rx_copy_rate / 100u)) {
        l1_trace(L1_TRACE_MSG_DMA_DDR_RD_START, rx_modem_ddr_fifo_start[chan] + rx_modem_fifo_offset[chan]);
        invalidate_region(ddr_src, data_size);
        memcpy(v_buffer, ddr_src, data_size);
        l1_trace(L1_TRACE_MSG_DMA_DDR_RD_COMP, data_size);
    }

    /* update modem flow control + kick VSPA GO event */
    rx_fc_notify(chan, app_RX_total_consumed_size[chan]);

    /* throttle: accumulate sleep debt and sleep when >= 1000 us */
    if (g_rx_throttle_percent < 100u && g_rx_throttle_percent > 0u) {
        /* us_per_step = bytes * (100 - pct) / (pct * ref_mbps) */
        uint32_t add_us = (data_size * (100u - g_rx_throttle_percent)) /
                          (g_rx_throttle_percent * g_rx_throttle_ref_mbps);
        g_rx_throttle_sleep_acc_us += add_us;
        if (g_rx_throttle_sleep_acc_us >= 1000u) {
            usleep(g_rx_throttle_sleep_acc_us);
            g_rx_throttle_sleep_acc_us = 0u;
        }
    }

    rx_modem_fifo_offset[chan] += data_size;
    if (rx_modem_fifo_offset[chan] >= rx_modem_ddr_fifo_size[chan])
        rx_modem_fifo_offset[chan] = 0;

    return data_size;
}
