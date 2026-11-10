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
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <assert.h>
#include <semaphore.h>
#include <signal.h>
#include <argp.h>
#include <termios.h>
#include <time.h>
#include <la9310_modinfo.h>
#include <la9310_host_if.h>
/* LA9310_DEV_NAME_PREFIX is defined in the NXP-tree la9310_host_if.h.
 * On RFNM builds neither header tree defines it; supply the RFNM driver
 * device name prefix so host-utils can open /dev/shiva<n>. */
#ifndef LA9310_DEV_NAME_PREFIX
#define LA9310_DEV_NAME_PREFIX  "shiva"
#endif

#include "iq_app.h"
#include "imx8-host.h"
#include "l1-trace-host.h"
#include "lib_iqplayer_api.h"

/* -----------------------------------------------------------------------
 * Armral / DFT abstraction
 *
 * If the Arm RAL library is available at build time (-DHAVE_ARMRAL), use the
 * hardware-accelerated cs16 FFT.  Otherwise fall back to a simple libm DFT
 * that is correct but slow -- fine for interactive spectrum display.
 * ----------------------------------------------------------------------- */
#ifdef HAVE_ARMRAL
#include <armral.h>
typedef armral_cmplx_int16_t iq_cs16_t;
typedef armral_cmplx_f32_t   iq_cf32_t;
#else
typedef struct { int16_t re; int16_t im; } iq_cs16_t;
typedef struct { float re; float im; } iq_cf32_t;
#endif

/* -----------------------------------------------------------------------
 * Globals shared between main() and helper functions
 * ----------------------------------------------------------------------- */

static const char *FilePath;
uint32_t           file_size = 0;

volatile uint32_t running;

static uint32_t *v_iqflood_ddr_addr;
static uint32_t *v_scratch_ddr_addr;
static uint32_t *v_la9310_bar2;

uint32_t RxChanID;

uint32_t modem_ddr_fifo_start;
uint32_t modem_ddr_fifo_size;

/* number of RX channels handled by this process (1 = legacy single-channel) */
static uint32_t g_n_rx     = 1;
static bool     g_n_rx_set = false; /* true when -n was given explicitly */

/* spectrum / tone parameters */
static uint32_t g_fft_size    = 512;
static uint32_t g_bar_cols    = 80;
static float    g_db_floor    = -100.0f;
static float    g_db_top      = 0.0f;   /* -K: top of the bar chart, dBFS */
static uint32_t g_spec_navg   = 8;      /* -M: FFTs averaged per refresh */
static bool     g_fft_set     = false;  /* true when -N was given */
static uint32_t g_spec_rows   = 0;      /* -L: chart rows, 0 = auto (40/16/10 for 1/2/4 ch) */
static bool     g_tone_dbbar  = false;  /* -l: -T bar in dB (default linear amplitude) */
static bool     g_spec_hann   = false;  /* -H: Hann window + power average */
static const char *g_spec_file = NULL;  /* -P: offline spectrum of a file */
static volatile int g_spec_tview = 0;   /* -X 't' key: time-domain chart */
static volatile uint32_t g_spec_tspc = 1;  /* -X '+'/'-': samples per column */
static volatile int g_spec_tfix = 0;    /* -X 'a' key: 1 = fixed +-1 FS scale */
/* -X/-P saturation stats; |sample| >= SPEC_CLIP_LSB counts as clipped */
#define SPEC_CLIP_LSB 32700
typedef struct {
    int32_t  pk_i, pk_q;    /* max |I|, max |Q| in LSB */
    uint32_t clip;          /* samples with |I| or |Q| >= SPEC_CLIP_LSB */
} spec_tstat_t;
static spec_tstat_t *g_spec_ts;          /* updated by spec_accum_block() */
#define SPEC_FFT_DEFAULT 8192u          /* -X/-P default, as compute_tone_params.py */
static float    g_srate_mhz   = 122.88f;
static bool     g_srate_set   = false; /* true when -Z was given explicitly */
static float    g_conv_mhz    = 61.44f; /* ADC/DAC converter rate (-Y), default half rate */

/* QEC config directory (-Q) and load-at-start control (-U disables) */
static const char *g_qec_dir  = "./qec";
static bool     g_qec_load    = true;

/* -X snapshot capture ('w' key): base name (-w) and per-channel bytes (-d,
 * 0 = 1 ms at the RX sample rate) */
static const char *g_snap_base = "./capture/rx";
static uint32_t g_snap_bytes   = 0;
static float    g_snap_ms      = 1.0f; /* used when g_snap_bytes == 0 */
static volatile int g_snap_req = 0;
static char     g_snap_msg[160] = "";

/* RX flow-control parameters */
static uint32_t g_rx_copy_rate     = 100u; /* percent of chunks to memcpy (100 = all) */
static uint32_t g_rx_throttle_pct  = 0u;   /* 0 = no throttle */

/* tone generator parameters */
static float    g_tone_freq_khz  = 0.0f;  /* 0 = default to Fs/8 */
static float    g_tone_amplitude = 1.0f;
static int      g_tone_type      = 0;     /* 0=tone, 1=two_tone, 2=noise */

void print_host_trace(void);

modinfo_t mi;

/* -----------------------------------------------------------------------
 * Device / memory helpers
 * ----------------------------------------------------------------------- */

static int get_modem_info(int modem_id)
{
    int fd;
    int ret;
    char dev_name[32];

    sprintf(dev_name, "/dev/%s%d", LA9310_DEV_NAME_PREFIX, modem_id);

    fd = open(dev_name, O_RDWR);
    if (fd < 0) {
        printf("File %s open error\n", dev_name);
        return -1;
    }

    ret = ioctl(fd, IOCTL_LA93XX_MODINFO_GET, &mi);
    if (ret < 0) {
        printf("IOCTL_LA9310_MODINFO_GET failed.\n");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int map_physical_regions(void)
{
    int devmem_fd, shiva_fd;
    char dev_name[32];

    /*
     * Open /dev/mem for MMIO regions (BAR2/tcmu).
     * The iqflood and scratch carveouts are normal RAM pages; use /dev/shiva<N>
     * to bypass CONFIG_STRICT_DEVMEM restrictions.
     */
    sprintf(dev_name, "/dev/%s%d", LA9310_DEV_NAME_PREFIX, 0);
    shiva_fd = open(dev_name, O_RDWR);
    if (shiva_fd < 0) {
        perror("shiva device open failed");
        return -1;
    }

    v_scratch_ddr_addr = mmap(NULL, mi.scratchbuf.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, shiva_fd, mi.scratchbuf.host_phy_addr);
    if (v_scratch_ddr_addr == MAP_FAILED) {
        perror("Mapping v_scratch_ddr_addr buffer failed");
        close(shiva_fd);
        return -1;
    }

    v_iqflood_ddr_addr = mmap(NULL, mi.iqflood.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, shiva_fd, mi.iqflood.host_phy_addr);
    if (v_iqflood_ddr_addr == MAP_FAILED) {
        perror("Mapping v_iqflood_ddr_addr buffer failed");
        close(shiva_fd);
        return -1;
    }
    close(shiva_fd);

    devmem_fd = open("/dev/mem", O_RDWR);
    if (devmem_fd < 0) {
        perror("/dev/mem open failed");
        return -1;
    }

    v_la9310_bar2 = mmap(NULL, mi.tcmu.size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, devmem_fd, mi.tcmu.host_phy_addr);
    if (v_la9310_bar2 == MAP_FAILED) {
        perror("Mapping v_la9310_bar2 buffer failed");
        close(devmem_fd);
        return -1;
    }
    close(devmem_fd);
    return 0;
}

/* -----------------------------------------------------------------------
 * Signal handlers
 * ----------------------------------------------------------------------- */
void terminate_process(int sig) { (void)sig; running = 0; }

/* -----------------------------------------------------------------------
 * Terminal state: saved once before the first raw-mode switch, restored
 * at exit (atexit) whatever the exit path, so the shell prompt and the
 * readline column are always consistent after -T / -X.
 * ----------------------------------------------------------------------- */
static struct termios g_tty_saved;
static volatile int g_tty_saved_ok;

static void tty_restore(void)
{
    if (g_tty_saved_ok) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
        /* reset attributes, show cursor, start the prompt on a fresh line */
        printf("\033[0m\033[?25h\r\n");
        fflush(stdout);
        g_tty_saved_ok = 0;
    }
}

/* Switch stdin to raw keys. ISIG is cleared so Ctrl-C arrives as key 3
 * and goes through the normal quit path instead of a SIGINT race with the
 * calling script. vtime is the read timeout in 1/10 s (0 = blocking). */
static int tty_raw(int vtime)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO))
        return 0;
    if (!g_tty_saved_ok) {
        if (tcgetattr(STDIN_FILENO, &g_tty_saved) != 0)
            return 0;
        g_tty_saved_ok = 1;
        atexit(tty_restore);
    }
    raw = g_tty_saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = (cc_t)vtime;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    return 1;
}

void sigusr1_process(int sig) { (void)sig; fflush(stdout); print_host_trace(); }

/* -----------------------------------------------------------------------
 * Help
 * ----------------------------------------------------------------------- */
void print_cmd_help(void)
{
    fprintf(stderr,
        "+--------------------------------------------------------------+\n"
        "| iq_app -- LA9310 IQ player host utility\n"
        "+--------------------------------------------------------------+\n"
        "| Basic modes:\n"
        "|\t-h\t\tHelp\n"
        "|\t-t\t\tTX mode (stream file to VSPA)\n"
        "|\t-r\t\tRX mode (capture VSPA output to file)\n"
        "|\t-X\t\tSpectrum mode (live FFT display; keys: g/G p/P i/I j/J RX QEC,\n"
        "|\t\t\t  c next QEC channel, r reset gain/phase,\n"
        "|\t\t\t  s save <qecdir>/qec_coeff_rx_ant<ch>.cfg,\n"
        "|\t\t\t  w write IQ snapshot of all channels,\n"
        "|\t\t\t  t spectrum/time-domain chart, +/- time zoom,\n"
        "|\t\t\t  a time scale auto/fixed +-1 FS, q quit)\n"
        "|\t\t\t  Status line: peak I/Q dBFS, clip count, crest factor\n"
        "|\t-C <ch>\t\tRX QEC channel selected at start in -X mode (default 0)\n"
        "|\t-T\t\tInteractive tone TX (keys: up/dn freq, lt/rt amp, w wave,\n"
        "|\t\t\t  g/G p/P i/I j/J TX QEC, r reset, s save, q quit)\n"
        "|\t-v\t\tVersion\n"
        "|\n"
        "| QEC options:\n"
        "|\t-Q <dir>\tQEC config directory (default ./qec). Saved\n"
        "|\t\t\t  qec_coeff_rx_ant<ch>.cfg / qec_coeff_tx_ant0.cfg are\n"
        "|\t\t\t  applied at start (-r -X -t -T)\n"
        "|\t-U\t\tDo not apply saved QEC at start\n"
        "|\n"
        "| Snapshot options (used with -X, key w):\n"
        "|\t-w <base>\tSnapshot base name (default ./capture/rx), writes\n"
        "|\t\t\t  <base>_<rate>Msps_ch<N>_<YYYYmmdd_HHMMSS>.bin\n"
        "|\t-d <size>\tSnapshot size per channel: bytes, or <N>ms (default 1ms)\n"
        "|\n"
        "| Common options:\n"
        "|\t-c <chanID>\tFirst RX channel index (default 0)\n"
        "|\t-n <N>\t\tNumber of RX channels to handle (default 1)\n"
        "|\t-f <path>\tInput (TX) or RX output base name (rx.bin -> rx_ch0.bin, rx_ch1.bin ...)\n"
        "|\t-s <size>\tPer-channel RX capture size in bytes\n"
        "|\t-F <offset> <size>\tCh0 FIFO: byte offset in iqflood and per-ch size\n"
        "|\t\t\t  ch_i FIFO base = offset + i * size\n"
        "|\t-R <pct>\tRX copy rate: percent of chunks to copy (default 100)\n"
        "|\t-S <pct>\tRX throttle: slow down drain to pct%% of 500 MB/s ref\n"
        "|\n"
        "| Spectrum options (used with -X):\n"
        "|\t-N <fft_size>\tFFT size, power-of-2 (default 8192 for -X/-P, 512 for -T)\n"
        "|\t-H\t\tHann window + power average (default: rectangular,\n"
        "|\t\t\t  mean |X| as host-utils/compute_tone_params.py)\n"
        "|\t-P <file>\tOffline spectrum of a cs16 capture file, no modem needed\n"
        "|\t-W <cols>\tBar width in terminal columns (default 80)\n"
        "|\t-B <dbfs>\tdBFS bottom of the chart, negative (default -100)\n"
        "|\t-L <rows>\tChart rows per channel, 3-60 (default 40 for 1 ch, 16 for 2, 10 for 3-4)\n"
        "|\t-K <dbfs>\tdBFS top of the chart (default 0)\n"
        "|\t-M <navg>\tFFTs averaged per refresh (default 8)\n"
        "|\t-Y <mhz>\tADC/DAC converter rate MHz (default 61.44 half rate, 122.88 full rate)\n"
        "|\t-Z <mhz>\tSample rate MHz override (default: -Y rate / VSPA decim or upsmp)\n"
        "|\n"
        "| Tone options (used with -T):\n"
        "|\t-q <freq_khz>\tTone frequency in kHz (default Fs/8)\n"
        "|\t-l\t\tTone panel bar in dB (-B floor to 0 dBFS); default linear amplitude\n"
        "|\t-A <amp>\tAmplitude 0.0-1.0 (default 1.0)\n"
        "|\t-V <tone|two_tone|noise>\tWaveform type (default tone)\n"
        "+--------------------------------------------------------------+\n"
        "| Examples:\n"
        "|\t iq_app -t -f tone.bin -F 0x00000000 32768\n"
        "|\t iq_app -r -f out.bin -s 4915200 -c 0 -F 0x04000000 524288\n"
        "|\t iq_app -r -n 2 -f out.bin -s 4915200 -c 0 -F 0x03000000 524288\n"
        "|\t iq_app -r -R 50 -f out.bin -s 4915200 -c 0 -F 0x04000000 524288\n"
        "|\t iq_app -r -S 80 -f out.bin -s 4915200 -c 0 -F 0x04000000 524288\n"
        "|\t iq_app -X -c 0 -F 0x04000000 524288 -N 512 -Z 122.88\n"
        "|\t iq_app -T -Z 30.72 -F 0x00000000 262144 -A 0.9\n"
        "+--------------------------------------------------------------+\n\n");
}

/* -----------------------------------------------------------------------
 * TX streaming
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * CPU affinity
 *
 * TX (iq-app-tx.sh) and RX (iq-app-rx.sh) may run as two iq_app processes
 * at the same time, so the time-critical loops get dedicated cores:
 *   IQ_CPU_TX (default 3) : TX feeder (file replay and -T tone)
 *   IQ_CPU_RX (default 2) : RX drain thread
 *   all other CPUs        : keyboard/UI, FFT, tone regen, RX main thread
 * Override with the IQ_CPU_TX / IQ_CPU_RX environment variables.
 * ----------------------------------------------------------------------- */
static int env_cpu(const char *name, int def)
{
    const char *v = getenv(name);
    int ncpu = get_nprocs();
    int c = v ? atoi(v) : def;

    if (c < 0 || c >= ncpu)
        c = ncpu - 1;
    return c;
}

static void pin_self_mask(cpu_set_t *set, const char *who)
{
    if (pthread_setaffinity_np(pthread_self(), sizeof(*set), set) != 0)
        fprintf(stderr, "%s: pthread_setaffinity_np failed\n", who);
}

/* Pin calling thread to the TX (is_tx=1) or RX (is_tx=0) dedicated core. */
static void pin_rt_core(int is_tx)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(is_tx ? env_cpu("IQ_CPU_TX", 3) : env_cpu("IQ_CPU_RX", 2), &set);
    pin_self_mask(&set, is_tx ? "TX feeder" : "RX drain");
}

/* Second RX drain core, used only when the drain is split (N_RX > 2 and
 * IQ_RX_SPLIT != 0). -1 means not in use. */
static int g_rx2_cpu = -1;

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);

    return v ? (int)strtol(v, NULL, 0) : def;
}

static void pin_cpu(int cpu, const char *who)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pin_self_mask(&set, who);
}

/* Pin calling thread to every CPU except the TX and RX dedicated cores. */
static void pin_ui_cores(void)
{
    cpu_set_t set;
    int i, ncpu = get_nprocs();
    int tx = env_cpu("IQ_CPU_TX", 3), rx = env_cpu("IQ_CPU_RX", 2);

    CPU_ZERO(&set);
    for (i = 0; i < ncpu; i++)
        if (i != tx && i != rx && i != g_rx2_cpu)
            CPU_SET(i, &set);
    if (CPU_COUNT(&set) == 0)
        CPU_SET(0, &set);
    pin_self_mask(&set, "UI");
}

int process_ant_tx_streaming_app(void *arg)
{
    (void)arg;
    uint32_t ddr_rd_offset = 0;
    uint32_t size_sent = 0;
    void *buffer = NULL;
    int ret = 0;

    FILE *fp = fopen(FilePath, "rb");
    if (!fp) {
        fprintf(stderr, "Error opening '%s': %s\n", FilePath, strerror(errno));
        return EXIT_FAILURE;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "fseek(SEEK_END) failed: %s\n", strerror(errno));
        ret = EXIT_FAILURE;
        goto out1;
    }
    file_size = (uint32_t)ftell(fp);
    if (file_size == 0) {
        fprintf(stderr, "TX: empty file\n");
        ret = EXIT_FAILURE;
        goto out1;
    }
    rewind(fp);

    buffer = malloc(file_size);
    if (!buffer) {
        fprintf(stderr, "malloc(%u) failed\n", file_size);
        ret = EXIT_FAILURE;
        goto out1;
    }

    if (fread(buffer, 1, file_size, fp) != file_size) {
        fprintf(stderr, "fread failed: %s\n", strerror(errno));
        ret = EXIT_FAILURE;
        goto out2;
    }

    ret = iq_player_init_tx(modem_ddr_fifo_start, modem_ddr_fifo_size);
    if (!ret) {
        fprintf(stderr, "TX: iq_player_init_tx failed\n");
        ret = EXIT_FAILURE;
        goto out2;
    }

    pin_rt_core(1);
    ddr_rd_offset = 0;
    while (running) {
        void *ddr_src = (void *)((uint8_t *)buffer + ddr_rd_offset);
        uint32_t chunk = (file_size - ddr_rd_offset > modem_ddr_fifo_size)
                         ? modem_ddr_fifo_size : (file_size - ddr_rd_offset);
        size_sent = iq_player_send_data(ddr_src, chunk);
        ddr_rd_offset += size_sent;
        if (ddr_rd_offset >= file_size)
            ddr_rd_offset = 0;
    }

    ret = 0;
out2:
    free(buffer);
out1:
    fclose(fp);
    return ret;
}

/* -----------------------------------------------------------------------
 * Multi-channel RX streaming
 *
 * Architecture (per RX channel):
 *   drain_thread   -- tight loop; calls iq_player_receive_data() directly
 *                     into a power-of-2 ring buffer.  Time-critical: must
 *                     drain the VSPA DDR FIFO fast enough to prevent
 *                     DDR_WR_OVERRUN.  No blocking calls.
 *   process_thread -- reads from the ring and writes to the output file.
 *                     May block on fwrite(); isolated from drain latency.
 *
 * Ring is a flat heap allocation; wpos/rpos are monotonic byte counters
 * accessed by one writer (drain) and one reader (process) each -- no locks
 * needed, only compiler barriers to order the counter updates.
 *
 * File naming:
 *   every channel : base_chN.ext  (e.g. rx.bin -> rx_ch0.bin, rx_ch1.bin)
 * ----------------------------------------------------------------------- */

#define RX_RING_MULT   8u    /* ring = RX_RING_MULT * fifo_size, power-of-2 */
#define RX_RING_MIN    (128u * 1024u)  /* minimum ring size in bytes */

typedef struct {
    uint32_t         chan_id;
    uint32_t         fifo_start;    /* byte offset of this ch's FIFO in iqflood */
    uint32_t         fifo_size;     /* per-channel FIFO size in bytes */
    uint32_t         cap_size;      /* per-channel capture size in bytes */
    char             file_path[512];

    /* lock-free SPSC ring (drain -> display/capture consumer) */
    uint8_t         *ring_buf;
    uint32_t         ring_mask;     /* size - 1 (power-of-2) */
    volatile uint64_t ring_wpos;    /* written by drain thread only */
    volatile uint64_t ring_rpos;    /* written by consumer only */

    volatile int     stop;          /* set by main to request thread exit */
    volatile int     overrun_count; /* incremented by drain on ring full */
    uint8_t          sink_mode;     /* 1 = spectrum: evict oldest data when ring full */
} rx_chan_ctx_t;

/* Derive output filename for channel ch: base_chN.ext for every channel. */
static void make_chan_filename(const char *base_path, uint32_t ch, char *out, size_t out_sz)
{
    /* find last '.' that is not a directory separator */
    const char *dot = strrchr(base_path, '.');
    const char *slash = strrchr(base_path, '/');
    if (dot && (!slash || dot > slash)) {
        /* has extension */
        size_t base_len = (size_t)(dot - base_path);
        snprintf(out, out_sz, "%.*s_ch%u%s", (int)base_len, base_path, ch, dot);
    } else {
        snprintf(out, out_sz, "%s_ch%u", base_path, ch);
    }
}

/* Wrapper passed to the single all-channels drain thread. */
typedef struct {
    rx_chan_ctx_t *ctxs;      /* first channel of this drain group */
    uint32_t      n;          /* channels in this group */
    int           cpu;        /* -1: IQ_CPU_RX core, otherwise explicit core */
    uint32_t      chunk;      /* bytes per channel per iteration */
    int           rt;         /* 1: SCHED_FIFO */
    volatile int *stop;       /* shared stop flag (ctxs[0].stop of the full set) */
    /* per-thread counters, printed at exit */
    uint64_t      calls;
    uint64_t      idle_calls;
    uint64_t      bytes;
    uint64_t      busy_ns;    /* time spent inside iq_player_receive_data_multi */
    uint64_t      inval_ns;   /* of which cache invalidate */
    uint64_t      copy_ns;    /* of which memcpy */
    uint64_t      run_ns;     /* thread lifetime */
} rx_drain_all_ctx_t;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Single drain thread: services all N channels in one batched call per loop.
 *
 * Uses iq_player_receive_data_multi() for three optimisations:
 *   1. One DC CIVAC covers all 4 channel proxies (64 bytes = 1 cache line).
 *   2. Ahead FC-ack: rx_fc_notify() is sent for all channels BEFORE any
 *      memcpy so VSPA starts writing the next DDR chunk immediately.
 *   3. Small fixed chunks (IQ_PLAYER_DRAIN_CHUNK_SIZE = 16 KB) per channel
 *      per iteration spread drain bandwidth evenly across all channels,
 *      preventing fast-producing channels from starving ch2/ch3.
 *
 * Per-channel ring logic (sliding window, sink eviction, overrun counting)
 * is unchanged from the original single-channel drain thread design. */
static void *rx_drain_all_thread_fn(void *arg)
{
    rx_drain_all_ctx_t *a    = (rx_drain_all_ctx_t *)arg;
    rx_chan_ctx_t      *ctxs = a->ctxs;
    uint32_t            n    = a->n;
    uint32_t            chunk = a->chunk;
    uint32_t            i, k, rot = 0;
    rx_drain_req_t      reqs[RX_NUM_MAX_CHAN];
    rx_chan_ctx_t      *rctx[RX_NUM_MAX_CHAN];
    uint64_t            t_start;

    if (a->cpu < 0)
        pin_rt_core(0);
    else
        pin_cpu(a->cpu, "RX drain2");
    if (a->rt) {
        struct sched_param sp = { .sched_priority = 50 };
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            fprintf(stderr, "RX drain: SCHED_FIFO not allowed, running SCHED_OTHER\n");
    }

    t_start = now_ns();
    while (!*a->stop) {
        /* Build requests in rotated channel order so no channel is always
         * copied last.  Each request is capped to the drain chunk. */
        for (k = 0; k < n; k++) {
            rx_chan_ctx_t *ctx       = &ctxs[(k + rot) % n];
            uint32_t       ring_size = ctx->ring_mask + 1u;

            rctx[k] = ctx;
            __asm__ volatile("" ::: "memory");
            uint64_t r = ctx->ring_rpos;
            uint64_t w = ctx->ring_wpos;

            uint32_t used       = (uint32_t)(w - r);
            uint32_t free_space = ring_size - used;

            reqs[k].chan_id  = ctx->chan_id;
            reqs[k].got      = 0;
            reqs[k].dst      = NULL;
            reqs[k].max_size = 0;

            if (ctx->cap_size > 0u && !ctx->sink_mode) {
                if (used > ctx->cap_size) {
                    __asm__ volatile("" ::: "memory");
                    ctx->ring_rpos = w - ctx->cap_size;
                    r          = ctx->ring_rpos;
                    used       = ctx->cap_size;
                    free_space = ring_size - used;
                }
            }

            if (free_space == 0) {
                if (ctx->sink_mode) {
                    __asm__ volatile("" ::: "memory");
                    ctx->ring_rpos += chunk;
                    __asm__ volatile("" ::: "memory");
                    r          = ctx->ring_rpos;
                    free_space = ring_size - (uint32_t)(w - r);
                } else {
                    ctx->overrun_count++;
                    continue;
                }
            }

            uint32_t off    = (uint32_t)(w & (uint64_t)ctx->ring_mask);
            uint32_t linear = ring_size - off;
            uint32_t req    = chunk;
            if (req > free_space) req = free_space;
            if (req > linear)     req = linear;

            reqs[k].dst      = (uint32_t *)(ctx->ring_buf + off);
            reqs[k].max_size = req;
        }
        if (n > 1 && ++rot >= n)
            rot = 0;

        /* One library call: single dccivac + ahead FC-acks + batched memcpy. */
        {
            uint64_t t0 = now_ns();
            int any = iq_player_receive_data_multi(reqs, n);
            a->busy_ns += now_ns() - t0;
            a->calls++;
            if (!any)
                a->idle_calls++;
        }

        /* Advance ring write pointers for channels that returned data. */
        for (i = 0; i < n; i++) {
            if (reqs[i].got > 0) {
                __asm__ volatile("" ::: "memory");
                rctx[i]->ring_wpos += (uint32_t)reqs[i].got;
                a->bytes += (uint32_t)reqs[i].got;
            }
        }
    }
    a->run_ns = now_ns() - t_start;
    iq_player_get_rx_copy_times(&a->inval_ns, &a->copy_ns);
    return NULL;
}

/* Start the RX drain thread(s).
 * Run-time switches (environment):
 *   IQ_RX_SPLIT  (default 1)    : with N_RX > 2, ch0..h-1 on IQ_CPU_RX and
 *                                 ch h..n-1 on IQ_CPU_RX2 (0 = one thread)
 *   IQ_CPU_RX2   (default 1)    : core of the second drain thread
 *   IQ_RX_CHUNK  (default 16384): bytes per channel per drain iteration
 *   IQ_RX_RT     (default 0)    : 1 = SCHED_FIFO priority 50 for drains
 *   IQ_RX_COPY   (default 0)    : 1 = overlapped invalidate+copy (library)
 * dctx must hold 2 entries and stay in scope until the threads are joined.
 * Returns 0 on success. */
static int rx_start_drains(rx_chan_ctx_t *ctxs, uint32_t n, rx_drain_all_ctx_t *dctx,
                           pthread_t *tids)
{
    uint32_t h = n;
    uint32_t g, ngrp;
    int chunk = env_int("IQ_RX_CHUNK", (int)IQ_PLAYER_DRAIN_CHUNK_SIZE);
    int rt    = env_int("IQ_RX_RT", 0);

    if (chunk < 4096)
        chunk = 4096;
    g_rx2_cpu = -1;
    if (n > 2 && env_int("IQ_RX_SPLIT", 1)) {
        g_rx2_cpu = env_cpu("IQ_CPU_RX2", 1);
        if (g_rx2_cpu == env_cpu("IQ_CPU_RX", 2) || g_rx2_cpu == env_cpu("IQ_CPU_TX", 3))
            g_rx2_cpu = -1;
        else
            h = n / 2;
    }
    /* re-pin the caller so threads it creates later avoid the drain cores */
    pin_ui_cores();

    memset(dctx, 0, 2 * sizeof(*dctx));
    ngrp = (h < n) ? 2u : 1u;
    for (g = 0; g < ngrp; g++) {
        dctx[g].ctxs  = ctxs + (g ? h : 0u);
        dctx[g].n     = g ? (n - h) : h;
        dctx[g].cpu   = g ? g_rx2_cpu : -1;
        dctx[g].chunk = (uint32_t)chunk;
        dctx[g].rt    = rt;
        dctx[g].stop  = &ctxs[0].stop;
        if (pthread_create(&tids[g], NULL, rx_drain_all_thread_fn, &dctx[g]) != 0) {
            tids[g] = 0;
            return -1;
        }
    }
    if (ngrp == 2)
        fprintf(stderr, "iq_app: RX drain split: ch0-%u on CPU %d, ch%u-%u on CPU %d,"
                " chunk %d, rt %d\n",
                h - 1, env_cpu("IQ_CPU_RX", 2), h, n - 1, g_rx2_cpu, chunk, rt);
    else
        fprintf(stderr, "iq_app: RX drain: 1 thread ch0-%u on CPU %d, chunk %d, rt %d\n",
                n - 1, env_cpu("IQ_CPU_RX", 2), chunk, rt);
    return 0;
}

/* Stop and join all drain threads, then print per-thread counters. */
static void rx_stop_drains(rx_chan_ctx_t *ctxs, uint32_t n, rx_drain_all_ctx_t *dctx,
                           pthread_t *tids)
{
    uint32_t i, g;

    /* set every stop flag before joining: a thread may watch any of them */
    for (i = 0; i < n; i++)
        ctxs[i].stop = 1;
    for (g = 0; g < 2; g++) {
        if (!tids[g])
            continue;
        pthread_join(tids[g], NULL);
        tids[g] = 0;
        if (dctx[g].run_ns) {
            double run_s = (double)dctx[g].run_ns / 1e9;
            fprintf(stderr, "iq_app: drain%u ch%u-%u: %.1f MB/s, busy %.0f%%"
                    " (inval %.0f%%, copy %.0f%%), calls %llu, idle %.0f%%\n",
                    g, dctx[g].ctxs[0].chan_id, dctx[g].ctxs[dctx[g].n - 1].chan_id,
                    (double)dctx[g].bytes / 1e6 / run_s,
                    100.0 * (double)dctx[g].busy_ns / (double)dctx[g].run_ns,
                    100.0 * (double)dctx[g].inval_ns / (double)dctx[g].run_ns,
                    100.0 * (double)dctx[g].copy_ns / (double)dctx[g].run_ns,
                    (unsigned long long)dctx[g].calls,
                    dctx[g].calls ? 100.0 * (double)dctx[g].idle_calls /
                                    (double)dctx[g].calls : 0.0);
            iq_player_print_rx_diag(dctx[g].ctxs[0].chan_id, dctx[g].n, run_s);
        }
    }
}

/* rx_dump_ring_to_file -- called from main after drain threads are stopped.
 * Writes the last min(ring_available, cap_size) bytes from the ring to disk.
 * Handles ring wrap by copying in up to two parts.
 * Returns 0 on success, EXIT_FAILURE on error. */
static int rx_dump_ring_to_file(rx_chan_ctx_t *ctx)
{
    uint32_t ring_size = ctx->ring_mask + 1u;
    uint64_t w = ctx->ring_wpos;
    uint64_t r = ctx->ring_rpos;
    uint32_t avail = (uint32_t)(w - r);

    /* clamp to requested capture size */
    uint32_t dump_sz = avail;
    if (dump_sz > ctx->cap_size)
        dump_sz = ctx->cap_size;

    if (dump_sz == 0) {
        fprintf(stderr, "rx_dump ch%u: no data captured\n", ctx->chan_id);
        return 0;
    }

    /* read the LAST dump_sz bytes so we get the most recent samples */
    uint64_t read_pos = w - dump_sz;
    uint32_t off      = (uint32_t)(read_pos & (uint64_t)ctx->ring_mask);
    uint32_t linear   = ring_size - off;

    FILE *fp = fopen(ctx->file_path, "wb");
    if (!fp) {
        fprintf(stderr, "rx_dump ch%u: cannot open '%s': %s\n",
                ctx->chan_id, ctx->file_path, strerror(errno));
        return EXIT_FAILURE;
    }

    int ret = 0;
    if (linear >= dump_sz) {
        /* contiguous */
        if (fwrite(ctx->ring_buf + off, 1, dump_sz, fp) != dump_sz)
            ret = EXIT_FAILURE;
    } else {
        /* wraps: two parts */
        if (fwrite(ctx->ring_buf + off, 1, linear, fp) != linear)
            ret = EXIT_FAILURE;
        else if (fwrite(ctx->ring_buf, 1, dump_sz - linear, fp) != dump_sz - linear)
            ret = EXIT_FAILURE;
    }

    if (ret != 0)
        fprintf(stderr, "rx_dump ch%u: fwrite failed: %s\n",
                ctx->chan_id, strerror(errno));
    else
        printf("rx_dump ch%u: wrote %u bytes to %s\n",
               ctx->chan_id, dump_sz, ctx->file_path);

    fclose(fp);
    return ret;
}

/* Multi-channel RX capture (-r mode).
 *
 * One drain thread per channel runs continuously, filling a ring buffer that
 * is large enough to hold the full requested capture (file_size bytes).  The
 * drain thread never blocks on disk I/O -- it just calls
 * iq_player_receive_data() in a tight loop.  When the ring fills up the VSPA
 * decimation back-pressure mechanism (dec_backlog gate in firmware) naturally
 * stalls AXIQ so no samples are lost.
 *
 * On SIGINT the main thread signals all drain threads to stop, joins them,
 * then dumps the last min(ring_available, file_size) bytes from each channel
 * ring to disk via rx_dump_ring_to_file(). */
static int process_ant_rx_multichan_app(void)
{
    pin_ui_cores();
    uint32_t n = g_n_rx;
    uint32_t i;
    int ret = 0;

    rx_chan_ctx_t  *ctxs       = NULL;
    pthread_t      *drain_tids = NULL;
    rx_drain_all_ctx_t drain_all_ctx[2];

    memset(drain_all_ctx, 0, sizeof(drain_all_ctx));

    ctxs       = calloc(n, sizeof(rx_chan_ctx_t));
    drain_tids = calloc(RX_NUM_MAX_CHAN, sizeof(pthread_t));
    if (!ctxs || !drain_tids) {
        fprintf(stderr, "rx multichan: alloc failed\n");
        ret = EXIT_FAILURE;
        goto out_free_meta;
    }

    /* Ring must be large enough to hold the full capture without eviction.
     * Round up file_size to a power of 2, then take the larger of that and
     * RX_RING_MULT * fifo_size. */
    uint32_t ring_sz = modem_ddr_fifo_size * RX_RING_MULT;
    if (ring_sz < RX_RING_MIN)
        ring_sz = RX_RING_MIN;
    if (ring_sz < file_size)
        ring_sz = file_size;
    {
        uint32_t p = 1;
        while (p < ring_sz)
            p <<= 1;
        ring_sz = p;
    }

    /* initialise per-channel contexts */
    for (i = 0; i < n; i++) {
        rx_chan_ctx_t *ctx = &ctxs[i];

        ctx->chan_id    = RxChanID + i;
        ctx->fifo_start = modem_ddr_fifo_start + i * modem_ddr_fifo_size;
        ctx->fifo_size  = modem_ddr_fifo_size;
        ctx->cap_size   = file_size;
        /* sink_mode = 0: do not evict old data; let ring fill and back-pressure VSPA */
        ctx->sink_mode  = 0u;
        ctx->ring_mask  = ring_sz - 1u;

        make_chan_filename(FilePath, i, ctx->file_path, sizeof(ctx->file_path));

        ctx->ring_buf = malloc(ring_sz);
        if (!ctx->ring_buf) {
            fprintf(stderr, "rx multichan ch%u: ring malloc(%u) failed\n", i, ring_sz);
            ret = EXIT_FAILURE;
            for (uint32_t j = 0; j < i; j++)
                free(ctxs[j].ring_buf);
            goto out_free_meta;
        }

        /* init FIFO for this channel in the library */
        if (!iq_player_init_rx(ctx->chan_id, ctx->fifo_start, ctx->fifo_size)) {
            fprintf(stderr, "rx multichan ch%u: iq_player_init_rx failed\n", i);
            ret = EXIT_FAILURE;
            for (uint32_t j = 0; j <= i; j++)
                free(ctxs[j].ring_buf);
            goto out_free_meta;
        }
    }

    /* Single drain thread services all N channels in round-robin order.
     * All 4 x t_rx_ch_host_proxy entries fit in one 64-byte A53 cache line;
     * using one thread avoids cross-core DC CIVAC bouncing caused by multiple
     * A53 cores each invalidating and re-fetching the same cache line.
     * Prints are deferred until after pthread_create so the thread starts
     * with minimum latency after the last iq_player_init_rx(). */
    if (rx_start_drains(ctxs, n, drain_all_ctx, drain_tids) != 0) {
        perror("rx multichan: pthread_create drain");
        ret = EXIT_FAILURE;
        goto stop_all;
    }

    /* Print channel info after thread is already running. */
    for (i = 0; i < n; i++) {
        rx_chan_ctx_t *ctx = &ctxs[i];
        printf("iq_app rx ch%u: fifo_start=0x%08X size=0x%X ring=0x%X file=%s\n",
               ctx->chan_id, ctx->fifo_start, ctx->fifo_size, ring_sz, ctx->file_path);
    }
    printf("iq_app: capturing -- press Ctrl-C to stop and save\n");
    fflush(stdout);

    /* wait for SIGINT / SIGTERM */
    while (running)
        pause();

stop_all:
    /* signal all drain threads to stop, join, print per-thread counters */
    rx_stop_drains(ctxs, n, drain_all_ctx, drain_tids);

    /* dump the ring contents to disk for each channel */
    for (i = 0; i < n; i++) {
        if (rx_dump_ring_to_file(&ctxs[i]) != 0)
            ret = EXIT_FAILURE;
    }

    /* print overrun summary (should be zero with back-pressure working) */
    for (i = 0; i < n; i++) {
        if (ctxs[i].overrun_count > 0)
            fprintf(stderr, "rx multichan ch%u: ring overrun count = %d\n",
                    ctxs[i].chan_id, ctxs[i].overrun_count);
    }

    for (i = 0; i < n; i++)
        free(ctxs[i].ring_buf);

out_free_meta:
    free(ctxs);
    free(drain_tids);
    return ret;
}

/* -----------------------------------------------------------------------
 * Spectrum mode -- live FFT display, N channels
 *
 * Architecture:
 *   1 drain thread   -- round-robin over all N channels using rx_drain_all_thread_fn +
 *                       rx_chan_ctx_t.  Drain chunk = full fifo_size for
 *                       maximum throughput (prevents DDR_WR_OVERRUN on all
 *                       channels including non-displayed ones).
 *   1 keyboard thread
 *   main thread      -- for each channel: consume best-effort one FFT-worth
 *                       of samples from the ring, run FFT, update EMA.
 *                       Then render N stacked spectrum panels.
 *
 * When HAVE_ARMRAL is defined the Arm RAL cs16 FFT is used.
 * Otherwise a simple O(N^2) DFT via libm provides the same interface.
 * ----------------------------------------------------------------------- */

typedef struct {
    volatile int stop;
    volatile int kbd_stop;
} spec_kbd_ctx_t;

static int rx_qec_handle_key(char ch);
static void rx_qec_status_line(void);

/* Keyboard thread: 'q' or Ctrl-C to stop, g/G p/P i/I j/J r s for RX QEC */
static void *spec_kbd_thread(void *arg)
{
    spec_kbd_ctx_t *ctx = (spec_kbd_ctx_t *)arg;

    (void)tty_raw(0);    /* select() below gives the 50 ms poll */

    while (!ctx->stop) {
        fd_set fds;
        struct timeval tv = { 0, 50000 };
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            char ch = 0;
            if (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == 'q' || ch == 'Q' || ch == 3 /* Ctrl-C */) {
                    ctx->kbd_stop = 1;
                    running = 0;
                    break;
                }
                (void)rx_qec_handle_key(ch);
            }
        }
    }

    /* terminal restored by tty_restore() once the display is done */
    return NULL;
}

/* Radix-2 FFT fallback, used when HAVE_ARMRAL is not defined (for example
 * an x86 build for -P). Unscaled, same convention as armral_fft_execute_cf32.
 * n must be a power of 2. */
#ifndef HAVE_ARMRAL
static void fft_radix2_cf32(const iq_cf32_t *in, iq_cf32_t *out, uint32_t n)
{
    uint32_t bits = 0, i, j, len;

    while ((1u << bits) < n)
        bits++;
    for (i = 0; i < n; i++) {
        uint32_t r = 0, x = i;
        for (j = 0; j < bits; j++) {
            r = (r << 1) | (x & 1u);
            x >>= 1;
        }
        out[r] = in[i];
    }
    for (len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        for (j = 0; j < len / 2u; j++) {
            float c = (float)cos(ang * (double)j);
            float sn = (float)sin(ang * (double)j);
            for (i = 0; i < n; i += len) {
                iq_cf32_t u = out[i + j];
                iq_cf32_t v = out[i + j + len / 2u];
                float vr = v.re * c - v.im * sn;
                float vi = v.re * sn + v.im * c;
                out[i + j].re = u.re + vr;
                out[i + j].im = u.im + vi;
                out[i + j + len / 2u].re = u.re - vr;
                out[i + j + len / 2u].im = u.im - vi;
            }
        }
    }
}
#endif /* !HAVE_ARMRAL */

/* -----------------------------------------------------------------------
 * process_ant_rx_spectrum_app
 *
 * Uses a single rx_drain_all_thread_fn to drain all N channels in round-robin
 * order so every channel's VSPA DDR FIFO is drained at full fifo_size
 * granularity with no cross-core cache-line bouncing.  The main thread then
 * consumes one FFT-worth of
 * samples from each channel's ring on a best-effort basis, runs the FFT,
 * updates per-channel EMA display state, and renders N stacked spectrum
 * panels.
 * ----------------------------------------------------------------------- */
/* Frequency labels (MHz) under the RX bar chart at -Fs/2, -Fs/4, 0, +Fs/4,
 * +Fs/2. Column col maps to (col/bar_w - 0.5)*Fs, matching the FFT shift. */
static void spec_print_freq_axis(uint32_t bar_w)
{
    char line[512];
    uint32_t len = 8u + bar_w + 8u;
    int t;

    if (len >= sizeof(line))
        len = sizeof(line) - 1u;
    memset(line, ' ', len);
    line[len] = '\0';
    for (t = 0; t <= 4; t++) {
        uint32_t col = (uint32_t)t * bar_w / 4u;
        float fmhz = ((float)t / 4.0f - 0.5f) * g_srate_mhz;
        char lab[16];
        int n = snprintf(lab, sizeof(lab), "%+.2f", (double)fmhz);
        int pos = 8 + (int)col - n / 2;

        if (pos < 0)
            pos = 0;
        if (pos + n > (int)len)
            pos = (int)len - n;
        memcpy(&line[pos], lab, (size_t)n);
    }
    printf("%s  MHz\n", line);
}

#ifdef HAVE_ARMRAL
static armral_fft_plan_t *g_spec_plan;
#endif

static void spec_fft(iq_cf32_t *in, iq_cf32_t *out, uint32_t n)
{
#ifdef HAVE_ARMRAL
    (void)n;
    armral_fft_execute_cf32(g_spec_plan, in, out);
#else
    fft_radix2_cf32(in, out, n);
#endif
}

/* Line end: clear to end of line on a live screen, plain newline for -P. */
static const char *spec_eol(void)
{
    return g_spec_file ? "\n" : "\033[K\n";
}

/* Add one FFT block to acc[] (natural bin order).
 * win == NULL (default): rectangular window, acc += |X| / (N * 32768), the
 *   same scaling as compute_tone_params.py (mean |X| over blocks).
 * win != NULL (-H): Hann window, acc += |X|^2 / (sum(w)^2 * 32768^2), power
 *   average normalised so a full-scale on-bin tone reads 0 dBFS.
 * Also accumulates sum(I), sum(Q) and sum(I^2 + Q^2) for RSSI and DC. */
static void spec_accum_block(const iq_cs16_t *raw, uint32_t n,
                             const float *win, float win_sum,
                             iq_cf32_t *in, iq_cf32_t *out, float *acc,
                             double *sum_i, double *sum_q, double *sum_p)
{
    uint32_t k;
    double si = 0.0, sq = 0.0, sp = 0.0;

    if (g_spec_ts) {
        spec_tstat_t *ts = g_spec_ts;
        for (k = 0; k < n; k++) {
            int32_t ai = raw[k].re < 0 ? -(int32_t)raw[k].re : raw[k].re;
            int32_t aq = raw[k].im < 0 ? -(int32_t)raw[k].im : raw[k].im;
            if (ai > ts->pk_i)
                ts->pk_i = ai;
            if (aq > ts->pk_q)
                ts->pk_q = aq;
            if (ai >= SPEC_CLIP_LSB || aq >= SPEC_CLIP_LSB)
                ts->clip++;
        }
    }

    for (k = 0; k < n; k++) {
        double re = (double)raw[k].re;
        double im = (double)raw[k].im;
        si += re;
        sq += im;
        sp += re * re + im * im;
        if (win) {
            in[k].re = (float)raw[k].re * win[k];
            in[k].im = (float)raw[k].im * win[k];
        } else {
            in[k].re = (float)raw[k].re;
            in[k].im = (float)raw[k].im;
        }
    }
    *sum_i += si;
    *sum_q += sq;
    *sum_p += sp;

    spec_fft(in, out, n);

    if (win) {
        float sc = 1.0f / (win_sum * win_sum * 32768.0f * 32768.0f);
        for (k = 0; k < n; k++)
            acc[k] += (out[k].re * out[k].re + out[k].im * out[k].im) * sc;
    } else {
        float sc = 1.0f / ((float)n * 32768.0f);
        for (k = 0; k < n; k++)
            acc[k] += sqrtf(out[k].re * out[k].re + out[k].im * out[k].im) * sc;
    }
}

static int spec_cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Row of value v (in FS) on a chart of 'rows' rows covering +-scale;
 * row 0 is the top. Values outside the scale go to the edge rows. */
static int spec_trow(float v, float scale, uint32_t rows)
{
    int r = (int)floorf((scale - v) * (float)rows / (2.0f * scale));

    if (r < 0)
        r = 0;
    if (r >= (int)rows)
        r = (int)rows - 1;
    return r;
}

/* Time-domain chart of one block (like the I/Q plots of
 * compute_tone_params.py). '*' = I, 'o' = Q, '+' = both, '.' = zero line.
 * Y scale: auto (smallest 1-2-5 step above the peak) or fixed +-1 FS
 * ('a' key), where the edge rows are the clip rails ('=').
 * X: g_spec_tspc samples per column. At 1 sample/column each sample is
 * one point; above, each column is the min..max envelope of its samples,
 * so a short clip is never averaged away. */
static void spec_render_time(const iq_cs16_t *raw, uint32_t n, uint32_t rows)
{
    static const float steps[] = { 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f };
    uint32_t cols = g_bar_cols, spc = g_spec_tspc, span, row, col, s, k;
    float scale = 1.0f, pk = 0.0f;
    int fixed = g_spec_tfix;

    if (spc < 1u)
        spc = 1u;
    if (cols < 8u)
        cols = 8u;
    if ((uint64_t)cols * spc > n)
        spc = n / cols ? n / cols : 1u;
    span = cols * spc;
    if (span > n) {
        span = n;
        cols = n;
    }
    if (rows < 4u)
        rows = 4u;

    if (!fixed) {
        for (s = 0; s < span; s++) {
            float vi = fabsf((float)raw[s].re) / 32768.0f;
            float vq = fabsf((float)raw[s].im) / 32768.0f;
            if (vi > pk) pk = vi;
            if (vq > pk) pk = vq;
        }
        for (k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
            scale = steps[k];
            if (pk <= scale)
                break;
        }
    }

    for (row = 0; row < rows; row++) {
        float top = scale - (float)row * 2.0f * scale / (float)rows;
        float bot = top - 2.0f * scale / (float)rows;
        int zero = (bot <= 0.0f && top > 0.0f);
        int rail = fixed && (row == 0u || row == rows - 1u);
        printf("%+6.3f |", (double)top);
        for (col = 0; col < cols; col++) {
            uint32_t s0 = col * spc, s1 = s0 + spc;
            float imn = 2.0f, imx = -2.0f, qmn = 2.0f, qmx = -2.0f;
            for (s = s0; s < s1 && s < n; s++) {
                float vi = (float)raw[s].re / 32768.0f;
                float vq = (float)raw[s].im / 32768.0f;
                if (vi < imn) imn = vi;
                if (vi > imx) imx = vi;
                if (vq < qmn) qmn = vq;
                if (vq > qmx) qmx = vq;
            }
            int r = (int)row;
            int hi = r >= spec_trow(imx, scale, rows) && r <= spec_trow(imn, scale, rows);
            int hq = r >= spec_trow(qmx, scale, rows) && r <= spec_trow(qmn, scale, rows);
            putchar(hi && hq ? '+' : hi ? '*' : hq ? 'o' :
                    rail ? '=' : zero ? '.' : ' ');
        }
        printf("|%s", spec_eol());
    }
    printf("       +");
    for (col = 0; col < cols; col++)
        putchar((col % (cols / 4u ? cols / 4u : 1u)) == 0u ? '+' : '-');
    printf("+%s", spec_eol());
    printf("        0 .. %.3f us, %u samples, %u/col  scale +-%.2f FS (%s)"
           "  keys: +/- zoom, a=auto/fixed, t=spectrum%s",
           (double)((float)span / g_srate_mhz), span, spc, (double)scale,
           fixed ? "fixed, = clip rails" : "auto", spec_eol());
}

/* Saturation line: peak |I|/|Q| in dBFS, clip count, crest factor of the
 * busier component (about 3 dB for a clean tone, near 0 dB when clipped). */
static void spec_print_sat(const spec_tstat_t *ts, double sum_p, double ns)
{
    int32_t pk = ts->pk_i > ts->pk_q ? ts->pk_i : ts->pk_q;
    float pk_db = 20.0f * log10f((float)pk / 32768.0f + 1e-15f);
    float rms_db = 10.0f * log10f((float)(sum_p / (2.0 * ns)) / (32768.0f * 32768.0f) + 1e-30f);
    const char *on = "", *off = "", *tag = "";

    if (ts->clip) {
        tag = "  [CLIP]";
        if (!g_spec_file) { on = "\033[1;31m"; off = "\033[0m"; }
    } else if (pk_db > -3.0f) {
        tag = "  [HIGH]";
        if (!g_spec_file) { on = "\033[1;33m"; off = "\033[0m"; }
    }
    printf("  peak I %.1f  Q %.1f dBFS  clip %u (%.3f%%)  crest %.1f dB%s%s%s%s",
           (double)(20.0f * log10f((float)ts->pk_i / 32768.0f + 1e-15f)),
           (double)(20.0f * log10f((float)ts->pk_q / 32768.0f + 1e-15f)),
           ts->clip, ns > 0.0 ? 100.0 * (double)ts->clip / ns : 0.0,
           (double)(pk_db - rms_db), on, tag, off, spec_eol());
}

/* Print one channel: text lines, then the bar chart and frequency axis, or
 * the time-domain chart of 'last' when the 't' view is on.
 * acc/nblk is the accumulator from spec_accum_block(); pwr and tmp are
 * scratch arrays of n floats. ts/last may be NULL. */
static void spec_render_channel(uint32_t idx, uint32_t chan_id,
                                const float *acc, uint32_t nblk,
                                double sum_i, double sum_q, double sum_p,
                                uint32_t n, uint32_t bar_rows,
                                float *pwr, float *tmp,
                                const spec_tstat_t *ts, const iq_cs16_t *last)
{
    const float fs2 = 32768.0f * 32768.0f;
    float bin_hz = (g_srate_mhz * 1e6f) / (float)n;
    double ns = (double)nblk * (double)n;
    uint32_t k, row, col;

    /* linear power per bin, then dBFS */
    for (k = 0; k < n; k++) {
        float v = acc[k] / (float)nblk;
        if (!g_spec_hann)
            v = v * v;
        pwr[k] = 10.0f * log10f(v + 1e-30f);
    }

    /* floor: median bin level, matches the flat part of the python plot */
    memcpy(tmp, pwr, n * sizeof(float));
    qsort(tmp, n, sizeof(float), spec_cmp_float);
    float floor_db = tmp[n / 2u];

    double mi = sum_i / ns, mq = sum_q / ns;
    float rssi_db = 10.0f * log10f((float)(sum_p / ns) / fs2 + 1e-30f);
    float dc_db   = 10.0f * log10f((float)(mi * mi + mq * mq) / fs2 + 1e-30f);

    /* spurs: up to 4 local maxima, 6 dB above the floor, at least 5 bins
     * apart, DC +-2 bins excluded (LO leakage is reported as DC) */
    uint32_t sp_bin[4];
    uint32_t nsp = 0, t;
    for (t = 0; t < 4u; t++) {
        float best = floor_db + 6.0f;
        uint32_t bb = 0;
        int found = 0;
        for (k = 3u; k + 3u < n; k++) {
            float v = pwr[k];
            uint32_t q;
            int near = 0;
            if (v <= best || v < pwr[k - 1u] || v < pwr[k + 1u])
                continue;
            for (q = 0; q < nsp; q++) {
                int32_t d = (int32_t)k - (int32_t)sp_bin[q];
                if (d > -5 && d < 5)
                    near = 1;
            }
            if (near)
                continue;
            best = v;
            bb = k;
            found = 1;
        }
        if (!found)
            break;
        sp_bin[nsp++] = bb;
    }

    printf("Ch%u (RxChan %u)  Fs %.2f MHz  FFT %u x%u  RBW %.1f kHz  %s%s",
           idx, chan_id, (double)g_srate_mhz, n, nblk,
           (double)(bin_hz / 1e3f), g_spec_hann ? "hann/pwr" : "rect/mag",
           spec_eol());
    printf("  RSSI %.1f  floor %.1f  DC %.1f dBFS%s",
           (double)rssi_db, (double)floor_db, (double)dc_db, spec_eol());
    printf("  spurs:");
    if (nsp == 0u)
        printf(" none");
    for (t = 0; t < nsp; t++) {
        int32_t sb = (sp_bin[t] >= n / 2u) ?
                     (int32_t)sp_bin[t] - (int32_t)n : (int32_t)sp_bin[t];
        printf(" %.1f@%+.2f", (double)pwr[sp_bin[t]],
               (double)((float)sb * bin_hz / 1e6f));
    }
    printf("%s%s", nsp ? " MHz" : "", spec_eol());
    if (ts)
        spec_print_sat(ts, sum_p, ns);

    if (g_spec_tview && last) {
        spec_render_time(last, n, bar_rows);
        return;
    }

    /* chart, DC in the middle: '#' up to the column mean, '|' up to the
     * column max, so tones and spurs show as spikes over the floor */
    uint32_t bar_w = g_bar_cols;
    float db_top = g_db_top, db_bot = g_db_floor;
    if (bar_w > n)
        bar_w = n;
    if (db_top <= db_bot)
        db_top = db_bot + 10.0f;
    float db_range = db_top - db_bot;

    for (row = 0; row < bar_rows; row++) {
        float row_db = db_top - (float)(row + 1u) * db_range / (float)bar_rows;
        printf("%6.1f |", (double)row_db);
        for (col = 0; col < bar_w; col++) {
            uint32_t b0 = col * n / bar_w;
            uint32_t b1 = (col + 1u) * n / bar_w;
            float mx = -300.0f;
            double sum = 0.0;
            uint32_t b;
            for (b = b0; b < b1; b++) {
                float v = pwr[(b + n / 2u) % n];
                if (v > mx)
                    mx = v;
                sum += pow(10.0, (double)v / 10.0);
            }
            float mean_db = 10.0f * log10f((float)(sum / (double)(b1 - b0)) + 1e-30f);
            putchar(mean_db >= row_db ? '#' : (mx >= row_db ? '|' : ' '));
        }
        printf("|%s", spec_eol());
    }
    printf("       +");
    for (col = 0; col < bar_w; col++)
        putchar((col % (bar_w / 4u ? bar_w / 4u : 1u)) == 0u ? '+' : '-');
    printf("+%s", spec_eol());
    spec_print_freq_axis(bar_w);
}

/* -P <file>: offline spectrum of a cs16 capture (same format as iq_app -r /
 * iq-capture.sh), using all full blocks like compute_tone_params.py. Runs
 * without the modem, so the same code can be checked on a PC. */
static int spec_file_app(const char *path)
{
    uint32_t n = g_fft_set ? g_fft_size : SPEC_FFT_DEFAULT;
    FILE *f = fopen(path, "rb");
    int ret = EXIT_FAILURE;

    if (!g_srate_set)
        g_srate_mhz = g_conv_mhz;
    if (!f) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (n < 64u || (n & (n - 1u)) != 0u) {
        fprintf(stderr, "-N must be a power of 2 >= 64\n");
        fclose(f);
        return EXIT_FAILURE;
    }

    iq_cs16_t *raw = malloc(n * sizeof(iq_cs16_t));
    iq_cf32_t *in  = malloc(n * sizeof(iq_cf32_t));
    iq_cf32_t *out = malloc(n * sizeof(iq_cf32_t));
    float *win = malloc(n * sizeof(float));
    float *acc = calloc(n, sizeof(float));
    float *pwr = malloc(n * sizeof(float));
    float *tmp = malloc(n * sizeof(float));
    float win_sum = 0.0f;
    double si = 0.0, sq = 0.0, sp = 0.0;
    uint32_t nblk = 0, k;
    spec_tstat_t fts = { 0, 0, 0u };

    if (!raw || !in || !out || !win || !acc || !pwr || !tmp)
        goto out;
    for (k = 0; k < n; k++) {
        win[k] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)k / (float)(n - 1u)));
        win_sum += win[k];
    }
#ifdef HAVE_ARMRAL
    if (armral_fft_create_plan_cf32(&g_spec_plan, (int)n, ARMRAL_FFT_FORWARDS) != ARMRAL_SUCCESS)
        goto out;
#endif
    g_spec_ts = &fts;
    while (fread(raw, sizeof(iq_cs16_t), n, f) == n) {
        spec_accum_block(raw, n, g_spec_hann ? win : NULL, win_sum,
                         in, out, acc, &si, &sq, &sp);
        nblk++;
    }
    g_spec_ts = NULL;
    if (nblk == 0u) {
        fprintf(stderr, "%s: less than one FFT block\n", path);
    } else {
        printf("%s: %u samples (%.2f ms)\n", path, nblk * n,
               (double)((float)(nblk * n) / (g_srate_mhz * 1e3f)));
        /* IQ_APP_TVIEW=1 [IQ_APP_TSPC=<n>]: time chart of the last block */
        if (getenv("IQ_APP_TVIEW"))
            g_spec_tview = 1;
        if (getenv("IQ_APP_TSPC"))
            g_spec_tspc = (uint32_t)strtoul(getenv("IQ_APP_TSPC"), NULL, 0);
        spec_render_channel(0, 0, acc, nblk, si, sq, sp, n,
                            g_spec_rows ? g_spec_rows : 40u, pwr, tmp,
                            &fts, raw);
        ret = EXIT_SUCCESS;
    }
#ifdef HAVE_ARMRAL
    if (g_spec_plan)
        armral_fft_destroy_plan(&g_spec_plan);
#endif
out:
    fclose(f);
    free(raw); free(in); free(out); free(win);
    free(acc); free(pwr); free(tmp);
    return ret;
}

/* Snapshot ('w' key) helpers. The snapshot copies every new byte of each
 * channel ring from the moment of the request, so the capture is contiguous
 * even though the spectrum view itself drops old blocks. */
#define SNAP_MAX_BYTES (64u * 1024u * 1024u)

typedef struct {
    int      active;
    uint32_t bytes;            /* per-channel target size */
    uint32_t got[RX_NUM_MAX_CHAN];
    uint64_t pos[RX_NUM_MAX_CHAN];  /* next absolute ring position to copy */
    uint8_t *buf[RX_NUM_MAX_CHAN];
    char     stamp[32];
} spec_snap_t;

/* mkdir -p for the directory part of path */
static void snap_mkdirs(const char *path)
{
    char tmp[256];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    p = strrchr(tmp, '/');
    if (!p)
        return;
    *p = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Sample rate tag for file names, e.g. 30.72 -> "30p72", 61.44 -> "61p44" */
static void snap_rate_tag(char *out, size_t len)
{
    char *p;

    snprintf(out, len, "%.3f", (double)g_srate_mhz);
    p = out + strlen(out) - 1;
    while (p > out && *p == '0')
        *p-- = '\0';
    if (*p == '.')
        *p = '\0';
    p = strchr(out, '.');
    if (p)
        *p = 'p';
}

static void snap_start(spec_snap_t *sn, rx_chan_ctx_t *ctxs, uint32_t n)
{
    uint32_t i, bytes = g_snap_bytes;
    time_t t = time(NULL);
    struct tm tmv;

    if (bytes == 0u)  /* default: g_snap_ms (1 ms) at the RX sample rate, cs16 */
        bytes = (uint32_t)(g_srate_mhz * 1000.0f * g_snap_ms + 0.5f) * (uint32_t)sizeof(iq_cs16_t);
    if (bytes > SNAP_MAX_BYTES)
        bytes = SNAP_MAX_BYTES;
    bytes &= ~3u;
    memset(sn, 0, sizeof(*sn));
    sn->bytes = bytes;
    for (i = 0; i < n && i < RX_NUM_MAX_CHAN; i++) {
        sn->buf[i] = malloc(bytes);
        if (!sn->buf[i]) {
            snprintf(g_snap_msg, sizeof(g_snap_msg), "snapshot: malloc %u failed", bytes);
            while (i--)
                free(sn->buf[i]);
            return;
        }
        __asm__ volatile("" ::: "memory");
        sn->pos[i] = ctxs[i].ring_rpos;
    }
    localtime_r(&t, &tmv);
    strftime(sn->stamp, sizeof(sn->stamp), "%Y%m%d_%H%M%S", &tmv);
    sn->active = 1;
    snprintf(g_snap_msg, sizeof(g_snap_msg), "snapshot: capturing %u bytes/ch ...", bytes);
}

/* Copy new ring bytes [pos, w) of channel i; must run before ring_rpos
 * is advanced past w. */
static void snap_feed(spec_snap_t *sn, uint32_t i, rx_chan_ctx_t *ctx, uint64_t w)
{
    uint32_t ring_size = ctx->ring_mask + 1u;

    /* drain thread may have overwritten unread data (sink_mode eviction) */
    if (w - sn->pos[i] > (uint64_t)ring_size) {
        sn->pos[i] = w - ring_size;
        snprintf(g_snap_msg, sizeof(g_snap_msg), "snapshot: ch%u ring overrun, data gap", i);
    }
    while (sn->got[i] < sn->bytes && sn->pos[i] < w) {
        uint32_t off   = (uint32_t)(sn->pos[i] & (uint64_t)ctx->ring_mask);
        uint32_t chunk = ring_size - off;
        uint64_t avail = w - sn->pos[i];
        uint32_t left  = sn->bytes - sn->got[i];

        if ((uint64_t)chunk > avail)
            chunk = (uint32_t)avail;
        if (chunk > left)
            chunk = left;
        memcpy(sn->buf[i] + sn->got[i], ctx->ring_buf + off, chunk);
        sn->got[i] += chunk;
        sn->pos[i] += chunk;
    }
}

/* Write the files once every channel is complete. */
static void snap_finish(spec_snap_t *sn, rx_chan_ctx_t *ctxs, uint32_t n)
{
    char rate[24], path[320];
    uint32_t i, nok = 0;

    for (i = 0; i < n && i < RX_NUM_MAX_CHAN; i++)
        if (sn->got[i] < sn->bytes)
            return;
    snap_rate_tag(rate, sizeof(rate));
    snap_mkdirs(g_snap_base);
    for (i = 0; i < n && i < RX_NUM_MAX_CHAN; i++) {
        FILE *fp;
        snprintf(path, sizeof(path), "%s_%sMsps_ch%u_%s.bin",
                 g_snap_base, rate, ctxs[i].chan_id, sn->stamp);
        fp = fopen(path, "wb");
        if (fp) {
            if (fwrite(sn->buf[i], 1, sn->bytes, fp) == sn->bytes)
                nok++;
            fclose(fp);
        }
        free(sn->buf[i]);
        sn->buf[i] = NULL;
    }
    snprintf(g_snap_msg, sizeof(g_snap_msg), "snapshot: %u/%u file(s) %s_%sMsps_ch*_%s.bin (%u bytes/ch)",
             nok, n, g_snap_base, rate, sn->stamp, sn->bytes);
    sn->active = 0;
}

static int process_ant_rx_spectrum_app(void)
{
    spec_snap_t snap;
    pin_ui_cores();
    if (!g_fft_set)
        g_fft_size = SPEC_FFT_DEFAULT;
    int ret = 0;
    uint32_t n   = g_n_rx;
    uint32_t fft_n = g_fft_size;
    uint32_t i;

    /* per-channel drain ring: same sizing as multichan RX */
    uint32_t ring_sz = modem_ddr_fifo_size * RX_RING_MULT;
    if (ring_sz < RX_RING_MIN)
        ring_sz = RX_RING_MIN;
    /* must also hold at least 4 * fft_n samples */
    if (ring_sz < fft_n * 4u * (uint32_t)sizeof(iq_cs16_t))
        ring_sz = fft_n * 4u * (uint32_t)sizeof(iq_cs16_t);
    /* and g_spec_navg FFT blocks plus slack */
    if (ring_sz < fft_n * (g_spec_navg + 2u) * (uint32_t)sizeof(iq_cs16_t))
        ring_sz = fft_n * (g_spec_navg + 2u) * (uint32_t)sizeof(iq_cs16_t);
    {
        uint32_t p = 1;
        while (p < ring_sz)
            p <<= 1;
        ring_sz = p;
    }

    /* allocate per-channel contexts */
    rx_chan_ctx_t *ctxs       = calloc(n, sizeof(rx_chan_ctx_t));
    pthread_t     *drain_tids = calloc(RX_NUM_MAX_CHAN, sizeof(pthread_t));
    rx_drain_all_ctx_t drain_all_ctx[2];

    memset(drain_all_ctx, 0, sizeof(drain_all_ctx));
    if (!ctxs || !drain_tids) {
        fprintf(stderr, "spectrum: alloc failed\n");
        free(ctxs); free(drain_tids);
        return EXIT_FAILURE;
    }

    /* FFT scratch buffers (shared across channels -- one FFT at a time) */
    /* Raw cs16 samples are copied from the ring, then windowed and
     * transformed in float. A cs16 FFT loses ~50 dB of dynamic range
     * (internal 1/N scaling plus int16 rounding), which hides the noise
     * floor and any weak wideband signal. */
    iq_cs16_t *fft_raw = malloc(fft_n * sizeof(iq_cs16_t));
    iq_cf32_t *fft_in  = malloc(fft_n * sizeof(iq_cf32_t));
    iq_cf32_t *fft_out = malloc(fft_n * sizeof(iq_cf32_t));
    /* hann window: w[k] = 0.5*(1 - cos(2*pi*k/(N-1))), pre-computed once */
    float     *hann_win = malloc(fft_n * sizeof(float));
    float      win_sum  = 0.0f;
    if (!fft_raw || !fft_in || !fft_out || !hann_win) {
        fprintf(stderr, "spectrum: FFT buffer malloc failed\n");
        ret = EXIT_FAILURE;
        goto out_fft_bufs;
    }
    {
        float n1 = (fft_n > 1u) ? (float)(fft_n - 1u) : 1.0f;
        for (uint32_t k = 0; k < fft_n; k++) {
            hann_win[k] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)k / n1));
            win_sum += hann_win[k];
        }
    }

#ifdef HAVE_ARMRAL
    armral_status ral_ret = armral_fft_create_plan_cf32(&g_spec_plan, (int)fft_n, ARMRAL_FFT_FORWARDS);
    if (ral_ret != ARMRAL_SUCCESS) {
        fprintf(stderr, "spectrum: armral_fft_create_plan_cf32 failed (%d)\n", (int)ral_ret);
        ret = EXIT_FAILURE;
        goto out_plan;
    }
#endif

    /* per-channel EMA display state: disp[ch * fft_n .. (ch+1)*fft_n - 1] */
    float *disp  = malloc(n * fft_n * sizeof(float));
    float *power = malloc(fft_n * sizeof(float));  /* scratch for one channel */
    float *sortb = malloc(fft_n * sizeof(float));  /* scratch for the median */
    /* newest block per channel, for the 't' time-domain view */
    iq_cs16_t *tlast = calloc((size_t)n * fft_n, sizeof(iq_cs16_t));
    spec_tstat_t ch_ts[RX_NUM_MAX_CHAN];
    memset(ch_ts, 0, sizeof(ch_ts));
    if (!disp || !power || !sortb || !tlast) {
        fprintf(stderr, "spectrum: disp/power malloc failed\n");
        ret = EXIT_FAILURE;
#ifdef HAVE_ARMRAL
        goto out_disp;
#else
        goto out_disp;
#endif
    }
    /* disp holds linear power (averaged); converted to dB at render time */
    for (i = 0; i < n * fft_n; i++)
        disp[i] = 0.0f;
    /* averaged time-domain levels per channel: AC power (DC removed) and
     * DC power, both in LSB^2; -1 = not yet initialised */
    double   ch_si[RX_NUM_MAX_CHAN], ch_sq[RX_NUM_MAX_CHAN], ch_sp[RX_NUM_MAX_CHAN];
    uint32_t ch_nb[RX_NUM_MAX_CHAN];
    for (i = 0; i < RX_NUM_MAX_CHAN; i++) {
        ch_si[i] = ch_sq[i] = ch_sp[i] = 0.0;
        ch_nb[i] = 0u;
    }

    /* initialise channels, alloc rings (drain thread spawned after the loop) */
    for (i = 0; i < n; i++) {
        rx_chan_ctx_t *ctx = &ctxs[i];

        ctx->chan_id    = RxChanID + i;
        ctx->fifo_start = modem_ddr_fifo_start + i * modem_ddr_fifo_size;
        ctx->fifo_size  = modem_ddr_fifo_size;
        /* cap_size not used in spectrum drain -- set to max to avoid wrap */
        ctx->cap_size   = (uint32_t)0xFFFFFFFFu;
        ctx->sink_mode  = 1u; /* evict oldest ring data when full to prevent DDR_WR_OVR */
        ctx->ring_mask  = ring_sz - 1u;

        ctx->ring_buf = malloc(ring_sz);
        if (!ctx->ring_buf) {
            fprintf(stderr, "spectrum ch%u: ring malloc(%u) failed\n", i, ring_sz);
            ret = EXIT_FAILURE;
            for (uint32_t j = 0; j < i; j++)
                free(ctxs[j].ring_buf);
            goto out_disp;
        }

        if (!iq_player_init_rx(ctx->chan_id, ctx->fifo_start, ctx->fifo_size)) {
            fprintf(stderr, "spectrum ch%u: iq_player_init_rx failed\n", i);
            ret = EXIT_FAILURE;
            for (uint32_t j = 0; j <= i; j++)
                free(ctxs[j].ring_buf);
            goto out_disp;
        }

        printf("spectrum ch%u: fifo_start=0x%08X size=0x%X ring=0x%X\n",
               ctx->chan_id, ctx->fifo_start, ctx->fifo_size, ring_sz);
    }

    /* Single drain thread services all N channels in round-robin order.
     * All channel proxy entries share one 64-byte A53 cache line -- one thread
     * avoids cross-core DC CIVAC bouncing.
     * drain_all_ctx must remain in scope until the thread is joined at stop_drains. */
    if (rx_start_drains(ctxs, n, drain_all_ctx, drain_tids) != 0) {
        perror("spectrum: pthread_create drain");
        ret = EXIT_FAILURE;
        goto stop_drains;
    }
    fflush(stdout);

    /* keyboard thread */
    spec_kbd_ctx_t kbd_ctx;
    memset(&kbd_ctx, 0, sizeof(kbd_ctx));
    pthread_t kbd_tid;
    if (pthread_create(&kbd_tid, NULL, spec_kbd_thread, &kbd_ctx) != 0) {
        perror("spectrum: pthread_create kbd");
        ret = EXIT_FAILURE;
        goto stop_drains;
    }

    /* hide cursor, clear screen */
    printf("\033[?25l\033[2J");
    fflush(stdout);
    memset(&snap, 0, sizeof(snap));

    while (running) {
        if (g_snap_req && !snap.active) {
            g_snap_req = 0;
            snap_start(&snap, ctxs, n);
        }
        /* wait until at least one channel has fft_n samples available */
        int any_ready = 0;
        while (running && !any_ready) {
            usleep(1000);
            __asm__ volatile("" ::: "memory");
            for (i = 0; i < n; i++) {
                uint64_t avail = ctxs[i].ring_wpos - ctxs[i].ring_rpos;
                if (avail >= (uint64_t)(fft_n * (uint32_t)sizeof(iq_cs16_t))) {
                    any_ready = 1;
                    break;
                }
            }
        }
        if (!running)
            break;

        /* render: cursor to top-left */
        printf("\033[H");
        rx_qec_status_line();

        /* Each refresh: FFT of up to g_spec_navg newest blocks per channel,
         * no history kept between refreshes (see spec_accum_block). */
        uint32_t need_bytes = fft_n * (uint32_t)sizeof(iq_cs16_t);
        uint32_t bar_rows   = g_spec_rows ? g_spec_rows :
                              ((n > 2u) ? 10u : ((n > 1u) ? 16u : 40u));

        for (i = 0; i < n && i < RX_NUM_MAX_CHAN; i++) {
            rx_chan_ctx_t *ctx = &ctxs[i];
            float *acc = disp + i * fft_n;

            __asm__ volatile("" ::: "memory");
            uint64_t w = ctx->ring_wpos;
            uint64_t r = ctx->ring_rpos;
            uint64_t nb64 = (w - r) / (uint64_t)need_bytes;

            if (snap.active)
                snap_feed(&snap, i, ctx, w);
            uint32_t nblk = (nb64 > (uint64_t)g_spec_navg) ?
                            g_spec_navg : (uint32_t)nb64;

            if (nblk > 0u) {
                /* newest nblk blocks; older data is dropped so the drain
                 * thread never stalls (avoids VSPA DDR_WR_OVR) */
                uint32_t ring_size_loc = ctx->ring_mask + 1u;
                r = w - (uint64_t)nblk * need_bytes;
                memset(acc, 0, fft_n * sizeof(float));
                ch_si[i] = ch_sq[i] = ch_sp[i] = 0.0;
                memset(&ch_ts[i], 0, sizeof(ch_ts[i]));
                g_spec_ts = &ch_ts[i];
                for (uint32_t blk = 0; blk < nblk; blk++) {
                    uint64_t rb = r + (uint64_t)blk * need_bytes;
                    uint32_t src_off = (uint32_t)(rb & (uint64_t)ctx->ring_mask);
                    uint32_t linear  = ring_size_loc - src_off;
                    if (linear >= need_bytes) {
                        memcpy(fft_raw, ctx->ring_buf + src_off, need_bytes);
                    } else {
                        memcpy(fft_raw, ctx->ring_buf + src_off, linear);
                        memcpy((uint8_t *)fft_raw + linear, ctx->ring_buf,
                               need_bytes - linear);
                    }
                    spec_accum_block(fft_raw, fft_n,
                                     g_spec_hann ? hann_win : NULL, win_sum,
                                     fft_in, fft_out, acc,
                                     &ch_si[i], &ch_sq[i], &ch_sp[i]);
                }
                g_spec_ts = NULL;
                memcpy(tlast + (size_t)i * fft_n, fft_raw, need_bytes);
                /* release the consumed ring space */
                __asm__ volatile("" ::: "memory");
                ctx->ring_rpos = w;
                ch_nb[i] = nblk;
            }
            /* if no new data, the previous frame is shown again */
            if (ch_nb[i] > 0u)
                spec_render_channel(i, ctx->chan_id, acc, ch_nb[i],
                                    ch_si[i], ch_sq[i], ch_sp[i],
                                    fft_n, bar_rows, power, sortb,
                                    &ch_ts[i], tlast + (size_t)i * fft_n);
        }
        printf("\033[J");
        if (snap.active)
            snap_finish(&snap, ctxs, n);

        fflush(stdout);
    }
    for (i = 0; i < RX_NUM_MAX_CHAN; i++)
        free(snap.buf[i]);

    /* stop keyboard thread */
    kbd_ctx.stop = 1;
    pthread_join(kbd_tid, NULL);

    /* restore terminal: settings, cursor, prompt on a fresh line */
    tty_restore();

stop_drains:
    rx_stop_drains(ctxs, n, drain_all_ctx, drain_tids);
    for (i = 0; i < n; i++)
        free(ctxs[i].ring_buf);

out_disp:
    free(tlast);
    free(sortb);
    free(power);
    free(disp);

#ifdef HAVE_ARMRAL
out_plan:
    if (g_spec_plan)
        armral_fft_destroy_plan(&g_spec_plan);
#endif

out_fft_bufs:
    free(hann_win);
    free(fft_raw);
    free(fft_in);
    free(fft_out);
    free(ctxs);
    free(drain_tids);
    return ret;
}

/* -----------------------------------------------------------------------
 * Interactive tone TX mode (-T), ported from la12xx_iqplayer iq_app.
 *
 * Two threads:
 *   feeder (main): tight loop calling iq_player_send_data() on buf[active].
 *                  Never sleeps so the TX DDR ring stays full.
 *   keyboard:      reads single keystrokes (termios raw mode), updates
 *                  parameters, regenerates the waveform into the inactive
 *                  buffer, then swaps active behind a compiler barrier.
 *
 * Ping-pong buffer: buf[0] and buf[1] each hold modem_ddr_fifo_size bytes
 * of int16 I/Q pairs (4 bytes per sample).
 *
 * Waveform types (-V or 'w' key):
 *   tone      - single complex exponential at freq F (default)
 *   two_tone  - sum of +F and -F tones (real cosine on I)
 *   noise     - white complex Gaussian noise
 *
 * TX QEC coefficients are pushed with MBOX_OPC_IQ_CORR (opcode 0x08) through
 * the vspa_mbox CLI whenever a QEC parameter changes:
 *   MSB = 0x08000000 | (txrx << 21) | idx,  LSB = float32 bits
 *
 * Default tap index map matches the la931x iqplayer txiqcomp kernel (used
 * for both TX and RX QEC). Firmware stores IQImb_ftaps[idx-1] in the order
 * [0 ci2q ci cq] and applies:
 *   i' = dc_I + ci * i
 *   q' = dc_Q + cq * q + ci2q * i       (DC is added, not subtracted)
 * so ci = f1, ci2q = f2, cq = f4:
 *   f2 -> 0x2, f1 -> 0x3, f4 -> 0x4, DC_I -> 0xE, DC_Q -> 0xF
 * Build with -DQEC_TAP_MAP_5G_DFE for the la12xx / la931x_vspa_5g_dfe
 * layout (f1 -> 0x2, f4 -> 0x3, f2 -> 0x4).
 * ----------------------------------------------------------------------- */

#ifdef QEC_TAP_MAP_5G_DFE
#define QEC_IDX_F1 0x2u
#define QEC_IDX_F4 0x3u
#define QEC_IDX_F2 0x4u
#else
#define QEC_IDX_F1 0x3u
#define QEC_IDX_F4 0x4u
#define QEC_IDX_F2 0x2u
#endif
#define QEC_IDX_DC_I 0xEu
#define QEC_IDX_DC_Q 0xFu

#define TONE_WAVEFORM_MAX 3

/* TX QEC state */
static float g_qec_gain_db   = 0.0f; /* gain imbalance in dB */
static float g_qec_phase_deg = 0.0f; /* phase imbalance in degrees */
static float g_qec_dc_i      = 0.0f; /* DC offset I */
static float g_qec_dc_q      = 0.0f; /* DC offset Q */

typedef struct {
    iq_cs16_t   *buf[2];  /* ping-pong pair */
    volatile int active;  /* index currently streamed by feeder */
    volatile int stop;    /* set to quit */
} tone_ctx_t;

#define TONE_BLK 1024u

/* Fill buf with n_samples I/Q pairs of the selected waveform. */
static void tone_gen_waveform(iq_cs16_t *buf, uint32_t n_samples, float freq_hz,
                              float srate_hz, float amplitude, int wtype)
{
    float scale = amplitude * 32767.0f;
    float pi2   = 2.0f * (float)M_PI;
    uint32_t i;
    uint32_t blk = n_samples;

    /* Tone and two-tone are periodic when freq is a multiple of Fs/TONE_BLK
     * (always true for the Fs/64 key steps). Compute one block and replicate
     * it, so a key press costs ~1K sin/cos instead of the whole buffer. */
    if ((wtype == 0 || wtype == 1) && n_samples > TONE_BLK &&
        (n_samples % TONE_BLK) == 0) {
        float cyc = freq_hz * (float)TONE_BLK / srate_hz;
        if (fabsf(cyc - roundf(cyc)) < 1e-3f)
            blk = TONE_BLK;
    }

    switch (wtype) {
    case 0: { /* single tone */
        float inc = pi2 * freq_hz / srate_hz;
        for (i = 0; i < blk; i++) {
            float ph = inc * (float)i;
            buf[i].re = (int16_t)(scale * cosf(ph));
            buf[i].im = (int16_t)(scale * sinf(ph));
        }
        break;
    }
    case 1: { /* two tone: +F and -F sum to a real cosine on I */
        float inc = pi2 * freq_hz / srate_hz;
        for (i = 0; i < blk; i++) {
            float ph = inc * (float)i;
            buf[i].re = (int16_t)(scale * cosf(ph));
            buf[i].im = 0;
        }
        break;
    }
    case 2: { /* Box-Muller Gaussian noise, ~3-sigma clip */
        for (i = 0; i < n_samples; i++) {
            float u1 = ((float)(rand() & 0x7FFF) + 0.5f) / 32768.0f;
            float u2 = ((float)(rand() & 0x7FFF) + 0.5f) / 32768.0f;
            float r  = sqrtf(-2.0f * logf(u1)) * scale * 0.35f;
            float t  = pi2 * u2;
            float si = r * cosf(t);
            float sq = r * sinf(t);
            if (si > 32767.0f)  si = 32767.0f;
            if (si < -32767.0f) si = -32767.0f;
            if (sq > 32767.0f)  sq = 32767.0f;
            if (sq < -32767.0f) sq = -32767.0f;
            buf[i].re = (int16_t)si;
            buf[i].im = (int16_t)sq;
        }
        break;
    }
    default:
        memset(buf, 0, n_samples * sizeof(*buf));
        blk = n_samples;
        break;
    }

    for (i = blk; i < n_samples; i += blk)
        memcpy(&buf[i], buf, blk * sizeof(*buf));
}

/* Write QEC coefficients to <qecdir>/qec_coeff_<txrx>_ant<N>.cfg (la12xx format). */
static void qec_save_file(const char *txrx_str, uint32_t ant_id, float f1, float f4,
                          float f2, float dcI, float dcQ)
{
    char path[256];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/qec_coeff_%s_ant%u.cfg", g_qec_dir, txrx_str, ant_id);
    mkdir(g_qec_dir, 0755);
    fp = fopen(path, "w");
    if (!fp) {
        perror("qec_save_file");
        return;
    }
    fprintf(fp, "# QEC coeff state: %s antenna %u\n", txrx_str, ant_id);
    fprintf(fp, "f1=%.16f\nf4=%.16f\nf2=%.16f\ndcI=%.16f\ndcQ=%.16f\n",
            f1, f4, f2, dcI, dcQ);
    fclose(fp);
    printf("\nQEC saved to %s\n", path);
}

/* Read <qecdir>/qec_coeff_<txrx>_ant<N>.cfg written by qec_save_file().
 * The file holds coefficient values (not tap indexes), so the same file is
 * valid for every firmware tap map. Returns 0 on success, -1 if no file. */
static int qec_load_file(const char *txrx_str, uint32_t ant_id, float *f1, float *f4,
                         float *f2, float *dcI, float *dcQ)
{
    char path[256], line[128];
    FILE *fp;
    int got = 0;

    snprintf(path, sizeof(path), "%s/qec_coeff_%s_ant%u.cfg", g_qec_dir, txrx_str, ant_id);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    *f1 = 1.0f; *f4 = 1.0f; *f2 = 0.0f; *dcI = 0.0f; *dcQ = 0.0f;
    while (fgets(line, sizeof(line), fp)) {
        char key[16];
        float v;
        if (line[0] == '#')
            continue;
        if (sscanf(line, " %15[^= ] = %f", key, &v) != 2)
            continue;
        if (!strcmp(key, "f1"))       { *f1 = v;  got |= 1; }
        else if (!strcmp(key, "f4"))  { *f4 = v;  got |= 2; }
        else if (!strcmp(key, "f2"))  { *f2 = v;  got |= 4; }
        else if (!strcmp(key, "dcI")) { *dcI = v; got |= 8; }
        else if (!strcmp(key, "dcQ")) { *dcQ = v; got |= 16; }
    }
    fclose(fp);
    if (got != 31)
        fprintf(stderr, "iq_app: %s incomplete, missing fields use passthrough\n", path);
    printf("iq_app: QEC loaded from %s\n", path);
    return 0;
}

/* Inverse of qec_coeffs_from_imb(): f4 = 1/alpha, f2 = -tan(phi)/alpha. */
static void qec_imb_from_coeffs(float f4, float f2, float *gain_db, float *phase_deg)
{
    float alpha;

    if (fabsf(f4) < 1e-6f)
        f4 = 1e-6f;
    alpha      = 1.0f / f4;
    *gain_db   = 20.0f * log10f(fabsf(alpha));
    *phase_deg = atanf(-f2 * alpha) * 180.0f / (float)M_PI;
}

/* Push one QEC coefficient to VSPA via the vspa_mbox CLI (send + recv ack). */
/* Channel field of MBOX_OPC_IQ_CORR: MSB bits 17-16 (msg bits 49-48). */
#define QEC_MBOX_CH(ch) (((uint32_t)(ch) & 0x3u) << 16)

static void tone_send_qec_ch(uint32_t txrx, uint32_t ch, uint32_t idx, float val)
{
    uint32_t lsb;
    uint32_t msb = 0x08000000u | (txrx << 21) | QEC_MBOX_CH(ch) | (idx & 0xFFFFu);
    char cmd[160];
    int rc;

    memcpy(&lsb, &val, sizeof(lsb));
    snprintf(cmd, sizeof(cmd),
             "vspa_mbox send 0 0 0x%08x 0x%08x >/dev/null 2>&1; "
             "vspa_mbox recv 0 0 >/dev/null 2>&1", msb, lsb);
    rc = system(cmd);
    (void)rc;
}

static void tone_send_qec(uint32_t txrx, uint32_t idx, float val)
{
    tone_send_qec_ch(txrx, 0, idx, val);
}

/* Compute TX f1/f4/f2 from gain/phase imbalance.
 *   alpha = 10^(gain_db/20), phi = phase_deg * pi/180
 *   f1 = 1/cos(phi), f2 = -tan(phi)/alpha, f4 = 1/alpha */
static void qec_coeffs_from_imb(float gain_db, float phase_deg,
                                float *f1, float *f4, float *f2)
{
    float alpha = powf(10.0f, gain_db / 20.0f);
    float phi   = phase_deg * (float)M_PI / 180.0f;
    float c     = cosf(phi);
    float s     = sinf(phi);

    if (fabsf(c) < 1e-6f)
        c = 1e-6f;
    if (fabsf(alpha) < 1e-6f)
        alpha = 1e-6f;
    *f1 = 1.0f / c;
    *f2 = -s / c / alpha;
    *f4 = 1.0f / alpha;
}

static void tone_qec_coeffs(float *f1, float *f4, float *f2)
{
    qec_coeffs_from_imb(g_qec_gain_db, g_qec_phase_deg, f1, f4, f2);
}

/* -----------------------------------------------------------------------
 * RX QEC (spectrum mode -X): same txiqcomp kernel and tap map, txrx = 0.
 * The firmware keeps one RX QEC set per channel; the keys act on the
 * current channel g_rxq_ch ('c' cycles it, -C sets it at start).
 * ----------------------------------------------------------------------- */
#define RXQ_MAX_CH 4
typedef struct {
    float gain_db, phase_deg, dc_i, dc_q;
} rxq_state_t;
static rxq_state_t g_rxq[RXQ_MAX_CH];
static uint32_t g_rxq_ch = 0;
#define g_rxq_gain_db   (g_rxq[g_rxq_ch].gain_db)
#define g_rxq_phase_deg (g_rxq[g_rxq_ch].phase_deg)
#define g_rxq_dc_i      (g_rxq[g_rxq_ch].dc_i)
#define g_rxq_dc_q      (g_rxq[g_rxq_ch].dc_q)

static void rx_qec_push(int with_dc)
{
    float f1, f4, f2;

    qec_coeffs_from_imb(g_rxq_gain_db, g_rxq_phase_deg, &f1, &f4, &f2);
    tone_send_qec_ch(0, g_rxq_ch, QEC_IDX_F1, f1);
    tone_send_qec_ch(0, g_rxq_ch, QEC_IDX_F4, f4);
    tone_send_qec_ch(0, g_rxq_ch, QEC_IDX_F2, f2);
    if (with_dc) {
        tone_send_qec_ch(0, g_rxq_ch, QEC_IDX_DC_I, g_rxq_dc_i);
        tone_send_qec_ch(0, g_rxq_ch, QEC_IDX_DC_Q, g_rxq_dc_q);
    }
}

/* Apply saved RX QEC of every channel at start (unless -U). The UI state is
 * rebuilt from the file so the keys continue from the loaded values. */
static void rx_qec_load_apply(void)
{
    uint32_t ch, save_ch = g_rxq_ch;

    if (!g_qec_load)
        return;
    for (ch = 0; ch < g_n_rx && ch < RXQ_MAX_CH; ch++) {
        float f1, f4, f2, dcI, dcQ;
        if (qec_load_file("rx", ch, &f1, &f4, &f2, &dcI, &dcQ) != 0)
            continue;
        g_rxq_ch = ch;
        qec_imb_from_coeffs(f4, f2, &g_rxq_gain_db, &g_rxq_phase_deg);
        g_rxq_dc_i = dcI;
        g_rxq_dc_q = dcQ;
        rx_qec_push(1);
    }
    g_rxq_ch = save_ch;
}

/* Handle one RX QEC key; returns 1 if the key was a QEC key. */
static int rx_qec_handle_key(char ch)
{
    switch (ch) {
    case 'g': g_rxq_gain_db   += 0.1f;  rx_qec_push(0); return 1;
    case 'G': g_rxq_gain_db   -= 0.1f;  rx_qec_push(0); return 1;
    case 'p': g_rxq_phase_deg += 0.5f;  rx_qec_push(0); return 1;
    case 'P': g_rxq_phase_deg -= 0.5f;  rx_qec_push(0); return 1;
    case 'i': g_rxq_dc_i      += 0.01f; rx_qec_push(1); return 1;
    case 'I': g_rxq_dc_i      -= 0.01f; rx_qec_push(1); return 1;
    case 'j': g_rxq_dc_q      += 0.01f; rx_qec_push(1); return 1;
    case 'J': g_rxq_dc_q      -= 0.01f; rx_qec_push(1); return 1;
    case 'r':
        /* Reset gain/phase to passthrough; DC (possibly from DC cal) kept. */
        g_rxq_gain_db = g_rxq_phase_deg = 0.0f;
        rx_qec_push(0);
        return 1;
    case 'c':
        /* next channel; its own state is kept, nothing is sent */
        g_rxq_ch = (g_rxq_ch + 1u) % (g_n_rx ? g_n_rx : 1u);
        return 1;
    case 's': {
        float f1, f4, f2;
        qec_coeffs_from_imb(g_rxq_gain_db, g_rxq_phase_deg, &f1, &f4, &f2);
        qec_save_file("rx", g_rxq_ch, f1, f4, f2, g_rxq_dc_i, g_rxq_dc_q);
        return 1;
    }
    case 'w':
        g_snap_req = 1;
        return 1;
    case 't':
        /* spectrum <-> time-domain chart */
        g_spec_tview = !g_spec_tview;
        return 1;
    case '+':
    case '=':
        /* zoom in: half the samples per column, down to 1 */
        if (g_spec_tspc > 1u)
            g_spec_tspc /= 2u;
        return 1;
    case '-': {
        /* zoom out: double samples per column, up to the whole block */
        uint32_t nmax = g_fft_set ? g_fft_size : SPEC_FFT_DEFAULT;
        uint32_t cols = g_bar_cols ? g_bar_cols : 1u;
        if (g_spec_tspc * 2u * cols <= nmax)
            g_spec_tspc *= 2u;
        return 1;
    }
    case 'a':
        /* time chart Y scale: auto <-> fixed +-1 FS */
        g_spec_tfix = !g_spec_tfix;
        return 1;
    default:
        return 0;
    }
}

static void rx_qec_status_line(void)
{
    printf("RX QEC ch %u/%u  gain %+6.2f dB  phase %+6.2f deg  DC I %+6.3f  DC Q %+6.3f"
           "   keys: c=chan g/G p/P i/I j/J r=rst s=save w=snap t=time q=quit\033[K\n",
           g_rxq_ch, g_n_rx ? g_n_rx : 1u, (double)g_rxq_gain_db, (double)g_rxq_phase_deg,
           (double)g_rxq_dc_i, (double)g_rxq_dc_q);
    printf("%s\033[K\n", g_snap_msg);
}

static void tone_push_all_qec(void);

/* Apply saved TX QEC (qec_coeff_tx_ant0.cfg) at start (unless -U). */
static void tx_qec_load_apply(void)
{
    float f1, f4, f2, dcI, dcQ;

    if (!g_qec_load || qec_load_file("tx", 0, &f1, &f4, &f2, &dcI, &dcQ) != 0)
        return;
    qec_imb_from_coeffs(f4, f2, &g_qec_gain_db, &g_qec_phase_deg);
    g_qec_dc_i = dcI;
    g_qec_dc_q = dcQ;
    tone_push_all_qec();
}

/* Send all five TX QEC coefficients (txrx = 1). */
static void tone_push_all_qec(void)
{
    float f1, f4, f2;

    tone_qec_coeffs(&f1, &f4, &f2);
    tone_send_qec(1, QEC_IDX_F1, f1);
    tone_send_qec(1, QEC_IDX_F4, f4);
    tone_send_qec(1, QEC_IDX_F2, f2);
    tone_send_qec(1, QEC_IDX_DC_I, g_qec_dc_i);
    tone_send_qec(1, QEC_IDX_DC_Q, g_qec_dc_q);
}

/* One frequency step = one display slot = SR/N * (N/64) kHz. */
static float tone_freq_step_khz(void)
{
    uint32_t fn = g_fft_size ? g_fft_size : 512u;
    uint32_t st = (fn >= 64u) ? (fn / 64u) : 1u;
    return g_srate_mhz * 1000.0f * (float)st / (float)fn;
}

/* Move frequency by delta steps, wrapping within [-Fs/2, Fs/2). */
static void tone_freq_bump(int delta)
{
    float span = g_srate_mhz * 1000.0f;
    float half = span / 2.0f;

    g_tone_freq_khz += (float)delta * tone_freq_step_khz();
    if (g_tone_freq_khz >= half)
        g_tone_freq_khz -= span;
    if (g_tone_freq_khz < -half)
        g_tone_freq_khz += span;
}

static void tone_amp_bump(float delta)
{
    g_tone_amplitude += delta;
    if (g_tone_amplitude > 1.0f)
        g_tone_amplitude = 1.0f;
    if (g_tone_amplitude < 0.0f)
        g_tone_amplitude = 0.0f;
}

/* Redraw the fixed control panel and a 3-row TX spectrum around the tone. */
/* Power of one FFT bin in dBFS, computed in float (Hann window, direct DFT).
 * The panel shows only 3 bins, so a per-bin DFT is cheap. A cs16 FFT was
 * used before, but its output saturates at int16 for any strong tone
 * (peak ~A*N/2), which pinned the reading at -54 dBFS whatever the
 * amplitude. Normalised by the Hann window sum: full-scale tone = 0 dBFS. */
static float tone_bin_dbfs(const iq_cs16_t *src, uint32_t n, uint32_t bin)
{
    double acc_re = 0.0, acc_im = 0.0, p, wsum = 0.0;
    double w0 = 2.0 * M_PI * (double)bin / (double)n;
    uint32_t k;

    for (k = 0; k < n; k++) {
        double w  = 0.5 * (1.0 - cos(2.0 * M_PI * (double)k / (double)(n - 1u)));
        double xr = (double)src[k].re * w, xi = (double)src[k].im * w;
        wsum += w;
        double c  = cos(w0 * (double)k), sn = sin(w0 * (double)k);
        /* X[bin] = sum x[k] * exp(-j w0 k) */
        acc_re += xr * c + xi * sn;
        acc_im += xi * c - xr * sn;
    }
    /* normalise by the window sum (coherent gain), so a tone of amplitude
     * A reads 20*log10(A) dBFS: full scale 0 dBFS, A = 0.05 reads -26 */
    p = (acc_re * acc_re + acc_im * acc_im) /
        (wsum * wsum * 32767.0 * 32767.0);
    return (float)(10.0 * log10(p + 1e-30));
}

static void tone_print_status(tone_ctx_t *ctx)
{
    static const char *wname[TONE_WAVEFORM_MAX] = { "tone", "two_tone", "noise" };
    static const char *sep = "+-----------------------------------------------------------------+";
    uint32_t n_samp = modem_ddr_fifo_size / (uint32_t)sizeof(iq_cs16_t);
    uint32_t fft_n  = g_fft_size;
    uint32_t k;

    /* no redraw once stopping: output after tty_restore() would land on
     * the shell prompt line */
    if (ctx->stop || !running)
        return;
    printf("\033[H");
    printf("%s\n", sep);
    printf("| iq_app  Tone TX (LA9310)                                        |\n");
    printf("%s\n", sep);
    printf("|  Waveform  : %-8s                                           |\n",
           wname[g_tone_type % TONE_WAVEFORM_MAX]);
    printf("|  Frequency : %+13.3f kHz  (up/dn: %8.3f kHz/step)      |\n",
           (double)g_tone_freq_khz, (double)tone_freq_step_khz());
    printf("|  Amplitude :  %-5.3f              (lt/rt: 0.050 /step)           |\n",
           (double)g_tone_amplitude);
    printf("%s\n", sep);
    printf("|  TX QEC                                                         |\n");
    printf("%s\n", sep);
    printf("|  Gain      : %+8.2f dB           ( g/G : step 0.1 dB  )       |\n", (double)g_qec_gain_db);
    printf("|  Phase     : %+8.2f deg          ( p/P : step 0.5 deg )       |\n", (double)g_qec_phase_deg);
    printf("|  DC I      : %+8.3f              ( i/I : step 0.01    )       |\n", (double)g_qec_dc_i);
    printf("|  DC Q      : %+8.3f              ( j/J : step 0.01    )       |\n", (double)g_qec_dc_q);
    printf("%s\n", sep);
    printf("|  Keys: up/dn=freq  lt/rt=amp  w=wave  r=rst  s=save  q=quit     |\n");
    printf("%s\n", sep);

    /* FFT size must be a power of 2 that fits in the buffer. */
    if (fft_n > n_samp)
        fft_n = n_samp;
    if (fft_n < 64u || (fft_n & (fft_n - 1u)) != 0u) {
        fflush(stdout);
        return;
    }

    {
        iq_cs16_t *src = ctx->buf[ctx->active];
        uint32_t step = fft_n / 64u;
        uint32_t i_tone;
        int delta;

        /* display index (FFT-shifted, DC in the middle) nearest the tone */
        i_tone = (uint32_t)((g_tone_freq_khz / 1000.0f / g_srate_mhz + 0.5f) * (float)fft_n + 0.5f);
        i_tone = (i_tone / step) * step;
        if (i_tone >= fft_n)
            i_tone = fft_n - step;

        printf("| TX spectrum  FFT=%-4u  SR=%.3f MHz\n", fft_n, (double)g_srate_mhz);
        printf("| ...\n");
        for (delta = 1; delta >= -1; delta--) {
            int idx = (int)i_tone + delta * (int)step;
            uint32_t i = (idx < 0) ? (fft_n - step) : ((uint32_t)idx % fft_n);
            uint32_t bin = (i + fft_n / 2u) & (fft_n - 1u);
            float fmhz = (((float)i / (float)fft_n) - 0.5f) * g_srate_mhz;
            float db   = tone_bin_dbfs(src, fft_n, bin);
            /* default: bar length follows the linear amplitude (|X| / full
             * scale), so 0.05 -> 5% and 0.5 -> 50% of the bar. -l: dB bar
             * from the -B floor up to 0 dBFS. */
            float norm = g_tone_dbbar ?
                         (db - g_db_floor) / (0.0f - g_db_floor) :
                         powf(10.0f, db / 20.0f);
            uint32_t bw, b;

            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
            bw = (uint32_t)(norm * (float)g_bar_cols);
            printf("| %9.3f MHz |", (double)fmhz);
            for (b = 0; b < g_bar_cols; b++)
                putchar(b < bw ? '#' : ' ');
            printf("| %7.2f dBFS\n", (double)db);
        }
        printf("| ...\n");
        printf("%s\n", sep);
    }
    fflush(stdout);
}

/* Keyboard thread: read keys in raw mode, update tone and QEC parameters. */
static void *tone_kbd_thread(void *arg)
{
    tone_ctx_t *ctx = (tone_ctx_t *)arg;
    uint32_t n_samp = modem_ddr_fifo_size / (uint32_t)sizeof(iq_cs16_t);

    pin_ui_cores();
    (void)tty_raw(1);    /* 100 ms read timeout so stop is seen */

    tone_print_status(ctx);

    while (!ctx->stop && running) {
        char ch = 0;
        int regen = 0, qec_chg = 0;

        if (read(STDIN_FILENO, &ch, 1) != 1)
            continue;

        switch (ch) {
        case '\033': { /* arrow keys: ESC [ A/B/C/D */
            char seq[2] = { 0, 0 };
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
                read(STDIN_FILENO, &seq[1], 1) == 1) {
                switch (seq[1]) {
                case 'A': tone_freq_bump(+1);  regen = 1; break;
                case 'B': tone_freq_bump(-1);  regen = 1; break;
                case 'C': tone_amp_bump(+0.05f); regen = 1; break;
                case 'D': tone_amp_bump(-0.05f); regen = 1; break;
                default: break;
                }
            }
            break;
        }
        case '+': tone_freq_bump(+1);    regen = 1; break;
        case '-': tone_freq_bump(-1);    regen = 1; break;
        case 'u': tone_amp_bump(+0.05f); regen = 1; break;
        case 'd': tone_amp_bump(-0.05f); regen = 1; break;
        case 'w': g_tone_type = (g_tone_type + 1) % TONE_WAVEFORM_MAX; regen = 1; break;
        case 'g': g_qec_gain_db   += 0.1f;  qec_chg = 1; break;
        case 'G': g_qec_gain_db   -= 0.1f;  qec_chg = 1; break;
        case 'p': g_qec_phase_deg += 0.5f;  qec_chg = 1; break;
        case 'P': g_qec_phase_deg -= 0.5f;  qec_chg = 1; break;
        case 'i': g_qec_dc_i      += 0.01f; qec_chg = 1; break;
        case 'I': g_qec_dc_i      -= 0.01f; qec_chg = 1; break;
        case 'j': g_qec_dc_q      += 0.01f; qec_chg = 1; break;
        case 'J': g_qec_dc_q      -= 0.01f; qec_chg = 1; break;
        case 'r':
            g_qec_gain_db = g_qec_phase_deg = g_qec_dc_i = g_qec_dc_q = 0.0f;
            qec_chg = 1;
            break;
        case 's': {
            float f1, f4, f2;
            tone_qec_coeffs(&f1, &f4, &f2);
            qec_save_file("tx", 0, f1, f4, f2, g_qec_dc_i, g_qec_dc_q);
            break;
        }
        case 'q':
        case 'Q':
        case 3: /* Ctrl-C in raw mode */
            ctx->stop = 1;
            running   = 0;
            break;
        default:
            break;
        }

        if (qec_chg)
            tone_push_all_qec();

        if (regen && !ctx->stop) {
            int inactive = 1 - ctx->active;
            tone_gen_waveform(ctx->buf[inactive], n_samp, g_tone_freq_khz * 1e3f,
                              g_srate_mhz * 1e6f, g_tone_amplitude, g_tone_type);
            __asm__ volatile("" ::: "memory");
            ctx->active = inactive;
        }

        if (regen || qec_chg)
            tone_print_status(ctx);
    }

    /* terminal restored by tty_restore() in process_tone_tx_app() */
    return NULL;
}

/* Entry point for -T mode. */
static int process_tone_tx_app(void)
{
    uint32_t n_samp;
    tone_ctx_t ctx;
    pthread_t kbd_tid;
    uint32_t send_off = 0;
    int last_active = 0;

    if (modem_ddr_fifo_size == 0) {
        fprintf(stderr, "tone TX: FIFO size not set (-F)\n");
        return EXIT_FAILURE;
    }
    if (g_fft_size == 0 || (g_fft_size & (g_fft_size - 1u)) != 0) {
        fprintf(stderr, "tone TX: -N fft size must be a power of 2\n");
        return EXIT_FAILURE;
    }
    n_samp = modem_ddr_fifo_size / (uint32_t)sizeof(iq_cs16_t);

    /* Default frequency Fs/8, snapped to the nearest display slot so the
     * generated tone lands on an FFT bin shown in the panel. */
    if (g_tone_freq_khz == 0.0f)
        g_tone_freq_khz = g_srate_mhz * 1e3f / 8.0f;
    {
        float step = tone_freq_step_khz();
        g_tone_freq_khz = roundf(g_tone_freq_khz / step) * step;
    }
    if (g_tone_amplitude < 0.0f) g_tone_amplitude = 0.0f;
    if (g_tone_amplitude > 1.0f) g_tone_amplitude = 1.0f;

    memset(&ctx, 0, sizeof(ctx));
    ctx.buf[0] = malloc(modem_ddr_fifo_size);
    ctx.buf[1] = malloc(modem_ddr_fifo_size);
    if (!ctx.buf[0] || !ctx.buf[1]) {
        fprintf(stderr, "tone TX: malloc ping-pong buffers failed\n");
        free(ctx.buf[0]);
        free(ctx.buf[1]);
        return EXIT_FAILURE;
    }
    tone_gen_waveform(ctx.buf[0], n_samp, g_tone_freq_khz * 1e3f, g_srate_mhz * 1e6f,
                      g_tone_amplitude, g_tone_type);
    memcpy(ctx.buf[1], ctx.buf[0], modem_ddr_fifo_size);

    if (!iq_player_init_tx(modem_ddr_fifo_start, modem_ddr_fifo_size)) {
        fprintf(stderr, "tone TX: iq_player_init_tx failed\n");
        free(ctx.buf[0]);
        free(ctx.buf[1]);
        return EXIT_FAILURE;
    }

    /* clear screen + hide cursor */
    printf("\033[?25l\033[2J");
    fflush(stdout);

    if (pthread_create(&kbd_tid, NULL, tone_kbd_thread, &ctx) != 0) {
        perror("tone TX: pthread_create keyboard");
        printf("\033[?25h\r\n");
        free(ctx.buf[0]);
        free(ctx.buf[1]);
        return EXIT_FAILURE;
    }

    pin_rt_core(1);

    /* Feeder: stream the active buffer continuously. send_off keeps phase
     * continuity across partial sends; on a buffer swap restart at 0. */
    while (!ctx.stop && running) {
        int active;
        int sent;

        __asm__ volatile("" ::: "memory");
        active = ctx.active;
        if (active != last_active) {
            send_off    = 0;
            last_active = active;
        }
        sent = iq_player_send_data((uint32_t *)((uint8_t *)ctx.buf[active] + send_off),
                                   modem_ddr_fifo_size - send_off);
        if (sent > 0) {
            send_off += (uint32_t)sent;
            if (send_off >= modem_ddr_fifo_size)
                send_off = 0;
        }
    }

    ctx.stop = 1;
    running  = 0;
    pthread_join(kbd_tid, NULL);

    tty_restore();
    free(ctx.buf[0]);
    free(ctx.buf[1]);
    return 0;
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int32_t  c, ret;
    command_e command = OP_TX_ONLY;
    struct stat status;

    if (argc <= 1) {
        print_cmd_help();
        exit(1);
    }
    signal(SIGINT,  terminate_process);
    signal(SIGTERM, terminate_process);
    signal(SIGUSR1, sigusr1_process);

    while ((c = getopt(argc, argv, "htrXTF:f:c:C:n:s:N:W:B:K:M:HP:L:lZ:Y:q:A:V:vR:S:Q:Uw:d:")) != EOF) {
        switch (c) {
        case 'h':
            print_cmd_help();
            exit(0);
        case 't':
            command = OP_TX_ONLY;
            break;
        case 'r':
            command = OP_RX_ONLY;
            break;
        case 'C':
            g_rxq_ch = (uint32_t)strtoul(optarg, NULL, 0) % RXQ_MAX_CH;
            break;
        case 'X':
            command = OP_SPECTRUM;
            break;
        case 'T':
            command = OP_TONE_TX;
            break;
        case 'F':
            modem_ddr_fifo_start = (uint32_t)strtoull(argv[optind - 1], NULL, 0);
            modem_ddr_fifo_size  = (uint32_t)strtoul(argv[optind], NULL, 0);
            optind++;
            break;
        case 's':
            file_size = (uint32_t)strtoull(argv[optind - 1], NULL, 0);
            break;
        case 'c':
            RxChanID = (uint32_t)strtoull(argv[optind - 1], NULL, 0);
            break;
        case 'n':
            g_n_rx = (uint32_t)strtoul(argv[optind - 1], NULL, 0);
            if (g_n_rx == 0) {
                fprintf(stderr, "-n: channel count must be >= 1\n");
                exit(EXIT_FAILURE);
            }
            g_n_rx_set = true;
            break;
        case 'f':
            FilePath = argv[optind - 1];
            break;
        /* spectrum options */
        case 'N':
            g_fft_size = (uint32_t)strtoul(argv[optind - 1], NULL, 0);
            g_fft_set  = true;
            break;
        case 'H':
            g_spec_hann = true;
            break;
        case 'l':
            g_tone_dbbar = true;
            break;
        case 'L':
            g_spec_rows = (uint32_t)strtoul(optarg, NULL, 0);
            if (g_spec_rows < 3u)
                g_spec_rows = 3u;
            if (g_spec_rows > 60u)
                g_spec_rows = 60u;
            break;
        case 'P':
            g_spec_file = optarg;
            break;
        case 'Q':
            g_qec_dir = optarg;
            break;
        case 'U':
            g_qec_load = false;
            break;
        case 'w':
            g_snap_base = optarg;
            break;
        case 'd':
            {
                char *end = NULL;
                double v = strtod(optarg, &end);
                if (end && !strcmp(end, "ms")) {
                    g_snap_ms    = (float)v;
                    g_snap_bytes = 0;
                } else {
                    g_snap_bytes = (uint32_t)strtoul(optarg, NULL, 0) & ~3u;
                }
            }
            break;
        case 'W':
            g_bar_cols = (uint32_t)strtoul(argv[optind - 1], NULL, 0);
            break;
        case 'B':
            g_db_floor = strtof(argv[optind - 1], NULL);
            break;
        case 'K':
            g_db_top = strtof(optarg, NULL);
            break;
        case 'M':
            g_spec_navg = (uint32_t)strtoul(optarg, NULL, 0);
            if (g_spec_navg < 1u)
                g_spec_navg = 1u;
            break;
        case 'Y':
            g_conv_mhz = strtof(optarg, NULL);
            if (g_conv_mhz <= 0.0f) {
                fprintf(stderr, "-Y: converter rate must be > 0 MHz\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'Z':
            g_srate_mhz = strtof(argv[optind - 1], NULL);
            g_srate_set = true;
            break;
        /* tone options */
        case 'q':
            g_tone_freq_khz = strtof(argv[optind - 1], NULL);
            break;
        case 'A':
            g_tone_amplitude = strtof(argv[optind - 1], NULL);
            break;
        case 'V':
            {
                const char *wtype = argv[optind - 1];
                if (strcmp(wtype, "two_tone") == 0)
                    g_tone_type = 1;
                else if (strcmp(wtype, "noise") == 0)
                    g_tone_type = 2;
                else
                    g_tone_type = 0; /* "tone" or unknown */
            }
            break;
        case 'R':
            g_rx_copy_rate = (uint32_t)strtoul(argv[optind - 1], NULL, 0);
            if (g_rx_copy_rate > 100u) {
                fprintf(stderr, "-R: copy rate must be 0-100\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'S':
            g_rx_throttle_pct = (uint32_t)strtoul(argv[optind - 1], NULL, 0);
            if (g_rx_throttle_pct > 100u) {
                fprintf(stderr, "-S: throttle percent must be 0-100\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'v':
            printf("iq_app version 1.2\n");
            exit(0);
        default:
            print_cmd_help();
            exit(1);
        }
    }

    /* -P: offline file spectrum, no modem needed */
    if (g_spec_file)
        return spec_file_app(g_spec_file);

    /* Check modem driver is loaded */
    if (stat("/sys/shiva/shiva_status", &status) != 0) {
        perror("la9310 shiva driver not started");
        exit(EXIT_FAILURE);
    }

#if defined(IQMOD_RX_1T1R) || defined(IQMOD_RX_0T1R)
    if (RxChanID != 0) {
        fprintf(stderr, "Rx chan not supported: app compiled for 1T1R\n");
        exit(EXIT_FAILURE);
    }
#endif

    running = 1;

    ret = get_modem_info(0);
    if (ret != 0) {
        perror("Failed to get modem_info");
        exit(EXIT_FAILURE);
    }

    if (map_physical_regions()) {
        perror("map_physical_regions failed");
        exit(EXIT_FAILURE);
    }

    /*
     * Auto-detect g_n_rx from the VSPA proxy if -n was not given on the CLI.
     * The tx_vspa_proxy struct sits at the last 1024 bytes of the iqflood region.
     * Field rx_num_chan is at byte offset +0x1C within t_tx_ch_host_proxy.
     * VSPA writes this field at boot before any mbox command, so it is readable
     * immediately after the firmware is loaded.
     */
    if (!g_n_rx_set) {
        const uint8_t *proxy_base =
            (const uint8_t *)v_iqflood_ddr_addr + mi.iqflood.size - 1024u;
        uint32_t rx_num_chan = *(const volatile uint32_t *)(proxy_base + 0x1Cu);
        if (rx_num_chan >= 1u && rx_num_chan <= 4u) {
            g_n_rx = rx_num_chan;
            printf("iq_app: auto-detected rx_num_chan=%u from VSPA proxy\n", g_n_rx);
        } else {
            fprintf(stderr,
                "iq_app: proxy rx_num_chan=%u out of range [1..4]; defaulting to 1\n",
                rx_num_chan);
            g_n_rx = 1u;
        }
    }

    if (g_rxq_ch >= g_n_rx)
        g_rxq_ch = 0;

    /*
     * Sample rate seen by the host (unless -Z given):
     *   RX: converter rate (-Y, default 61.44 MHz) / proxy rx_decim
     *   TX: converter rate (-Y, default 61.44 MHz) / proxy tx_upsmp
     * Proxy fields: rx_decim at +0x14, tx_upsmp at +0x18.
     *
     * The la9310_modem_info adc_rate_mask/dac_rate_mask are NOT used: they
     * only echo the la9310shiva insmod parameters and do not reflect the real
     * converter clock on every platform (e.g. RFNM sets it from la9310rfnm).
     */
    if (!g_srate_set) {
        const uint8_t *proxy_base =
            (const uint8_t *)v_iqflood_ddr_addr + mi.iqflood.size - 1024u;
        uint32_t rx_decim = *(const volatile uint32_t *)(proxy_base + 0x14u);
        uint32_t tx_upsmp = *(const volatile uint32_t *)(proxy_base + 0x18u);
        int is_tx = (command == OP_TX_ONLY || command == OP_TONE_TX);
        uint32_t div;

        /*
        uint32_t adc_bit = (RxChanID < 4u) ? RxChanID : 0u;
        conv = is_tx ? ((mi.dac_rate_mask & 1) ? 61.44f : 122.88f)
                     : (((mi.adc_rate_mask >> adc_bit) & 1) ? 61.44f : 122.88f);
        */
        if (is_tx)
            div = (tx_upsmp >= 1u && tx_upsmp <= 16u) ? tx_upsmp : 1u;
        else
            div = (rx_decim >= 1u && rx_decim <= 16u) ? rx_decim : 1u;
        g_srate_mhz = g_conv_mhz / (float)div;
        printf("iq_app: Fs %.3f MHz / %s %u = %.3f MHz (-Y to change converter rate, -Z to override)\n",
               (double)g_conv_mhz, is_tx ? "upsmp" : "decim", div, (double)g_srate_mhz);
    }

    /*
     * Initialise library:
     *   - pass mi.ccsr.host_phy_addr so the library can map mbox1 for GO events.
     *   - core_id = 0 (single-core default; add -C option if multi-core needed).
     */
    ret = iq_player_init(v_iqflood_ddr_addr, mi.iqflood.size, v_la9310_bar2,
                         mi.ccsr.host_phy_addr, 0);
    if (!ret) {
        fprintf(stderr, "iq_player_init failed\n");
        return EXIT_FAILURE;
    }

    /* apply optional RX flow-control settings */
    if (g_rx_copy_rate != 100u)
        iq_player_set_rx_copy_rate(g_rx_copy_rate);
    if (g_rx_throttle_pct != 0u)
        iq_player_set_rx_throttle_rate(g_rx_throttle_pct, 500u);

    /* Apply saved QEC so the config is the same for every firmware/run */
    if (command == OP_RX_ONLY || command == OP_SPECTRUM)
        rx_qec_load_apply();
    else if (command == OP_TX_ONLY || command == OP_TONE_TX)
        tx_qec_load_apply();

    switch (command) {
    case OP_TX_ONLY:
        return process_ant_tx_streaming_app(NULL);
    case OP_RX_ONLY:
        return process_ant_rx_multichan_app();
    case OP_SPECTRUM:
        return process_ant_rx_spectrum_app();
    case OP_TONE_TX:
        return process_tone_tx_app();
    default:
        fprintf(stderr, "Unknown command\n");
        return EXIT_FAILURE;
    }
}
