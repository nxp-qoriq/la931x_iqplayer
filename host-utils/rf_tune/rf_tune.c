/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2026 NXP
 *
 * rf_tune - interactive RF front-end tuning for the RFNM daughterboards
 * (Lime, Granita, Yucca) through the la9310rfnm sysfs interface:
 *   /sys/kernel/rfnm_primary/{rx0,rx1,tx0}/{enable,path,freq,gain,power,...}
 * Values are written, then "1" is written to <ch>/apply.
 *
 * Guards on top of the driver (the driver only clamps or rejects ranges):
 *  - per-board value ranges and allowed paths
 *  - confirmation when TX power crosses the board PA step
 *  - TX/RX loop budget: rx gain + tx power - external attenuation must
 *    stay under a limit while TX and RX are both on (-G, -x, -o)
 *  - Lime: RX apply with tx0 on is followed by a tx0 re-apply (shared
 *    antenna switch, see RFNM porting guide Known Trap 7); tx0 off is
 *    followed by an RX re-apply so the port goes back to RX
 *  - Granita loopback: tx power must be 0, TX loopback needs an RX
 *    channel on the loopback path
 *  - bias-tee on needs a confirmation
 * These guards are a convenience, not a hardware protection.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <termios.h>
#include <time.h>
#include <getopt.h>
#include <sys/select.h>
#include <sys/stat.h>

#define RF_TUNE_VERSION "1.1"

/* ----------------------------------------------------------------------
 * Board tables (ranges and paths from the la9310rfnm driver sources)
 * -------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int rg_min, rg_max;          /* rx gain range */
    int tp_min, tp_max;          /* tx power range used by rf_tune */
    const char *const *rx_paths;     /* rx0 (and rx1 if rx1_paths is NULL) */
    const char *const *tx_paths;
    int pa_thr;                  /* confirm when tx power goes above */
    int budget;                  /* default loop budget (gain + power) */
    int lime_port_rule;          /* shared ANT switch apply order */
    const char *const *rx1_paths;    /* rx1 when it differs from rx0 */
} board_t;

static const char *const lime_rx_p[] = { "sma_a", "sma_b", "embed_ant", NULL };
static const char *const lime_tx_p[] = { "sma_a", "sma_b", NULL };
static const char *const gran_rx_p[] = { "sma_a", "sma_b", "embed_ant", "loopback", NULL };
static const char *const gran_tx_p[] = { "sma_b", "sma_a", "loopback", NULL };
static const char *const yuc_rx_p[]  = { "sma_a", NULL };
static const char *const yuc_rx1_p[] = { "sma_b", NULL };
static const char *const yuc_tx_p[]  = { "sma_b", "sma_a", NULL };
static const char *const all_p[]     = { "sma_a", "sma_b", "sma_c", "sma_d", "sma_e",
                                         "sma_f", "sma_g", "sma_h", "embed_ant",
                                         "loopback", NULL };

static const board_t g_boards[] = {
    /* Lime: power -60..52 in the driver, 41..52 same as 40 (raw ladder) */
    { "lime",    -24, 30, -60, 40, lime_rx_p, lime_tx_p, 20, 40, 1, NULL },
    { "granita", -40, 60, -60, 30, gran_rx_p, gran_tx_p, 20, 40, 0, NULL },
    /* Yucca: power = PA step index - 10 (0..31), uncalibrated */
    { "yucca",     0, 77, -10, 21, yuc_rx_p,  yuc_tx_p,  15, 40, 0, yuc_rx1_p },
    { "unknown", -60, 80, -60, 52, all_p,     all_p,     20, 40, 0, NULL },
};
#define BOARD_UNKNOWN 3

/* ----------------------------------------------------------------------
 * Built-in defaults (reset). Lime values must stay in sync with the
 * echo fallback in host-utils/txrx_en.sh. Lines for a channel that the
 * board does not expose are skipped.
 * -------------------------------------------------------------------- */
static const char *const lime_def[] = {
    "tx0.enable=on", "tx0.path=sma_b", "tx0.freq=1850000000",
    "tx0.rfic_lpf_bw=20", "tx0.power=10",
    "rx0.enable=on", "rx0.path=sma_a", "rx0.freq=1850000000",
    "rx0.rfic_lpf_bw=100", "rx0.gain=20", NULL };
static const char *const gran_def[] = {
    "tx0.enable=on", "tx0.path=sma_b", "tx0.freq=1850000000",
    "tx0.rfic_lpf_bw=20", "tx0.power=10",
    "rx0.enable=on", "rx0.path=sma_a", "rx0.freq=1850000000",
    "rx0.rfic_lpf_bw=100", "rx0.gain=20", NULL };
static const char *const yuc_def[] = {
    "tx0.enable=on", "tx0.path=sma_b", "tx0.freq=2600000000",
    "tx0.rfic_lpf_bw=20", "tx0.power=0",
    "rx0.enable=on", "rx0.path=sma_a", "rx0.freq=2600000000",
    "rx0.rfic_lpf_bw=100", "rx0.gain=20", NULL };
static const char *const *const g_defs[] = { lime_def, gran_def, yuc_def, NULL };

/* ----------------------------------------------------------------------
 * Channel model
 * -------------------------------------------------------------------- */
enum { FI_EN, FI_PATH, FI_FREQ, FI_GAIN, FI_LPF, FI_AGC, FI_NOTCH, FI_BIAS, NF };
enum { T_ENUM, T_INT, T_FREQ };

static const char *const en_o[]  = { "off", "on", "tdd", NULL };
static const char *const oo_o[]  = { "off", "on", NULL };
static const char *const aoo_o[] = { "auto", "off", "on", NULL };

typedef struct {
    const char *label;       /* panel label */
    const char *rx_file;     /* sysfs name for rx, NULL = not present */
    const char *tx_file;
    int type;
    const char *const *opts; /* for T_ENUM (path uses the board list) */
} fdef_t;

static const fdef_t g_f[NF] = {
    { "enable",   "enable",      "enable",      T_ENUM, en_o  },
    { "path",     "path",        "path",        T_ENUM, NULL  },
    { "freq",     "freq",        "freq",        T_FREQ, NULL  },
    { "gain/pwr", "gain",        "power",       T_INT,  NULL  },
    { "lpf_bw",   "rfic_lpf_bw", "rfic_lpf_bw", T_INT,  NULL  },
    { "agc",      "agc",         NULL,          T_ENUM, aoo_o },
    { "fm_notch", "fm_notch",    NULL,          T_ENUM, aoo_o },
    { "bias_tee", "bias_tee",    "bias_tee",    T_ENUM, oo_o  },
};

#define MAX_CH 6
typedef struct {
    char name[8];
    int  is_tx;
    int  has[NF];
    long long cv[NF], hv[NF];        /* numeric: model / hardware */
    char cs[NF][24], hs[NF][24];     /* enum: model / hardware */
    int  dirty[NF];
    int  drv[NF];                    /* driver changed the written value */
    long long fmin, fmax;
} chan_t;

static chan_t   g_ch[MAX_CH];
static int      g_nch;
static const board_t *g_bd = &g_boards[BOARD_UNKNOWN];
static char     g_bd_how[32] = "default";
static char     g_base[256];
static int      g_slot = 0;
static int      g_budget = -1;       /* -G, -1 = board default */
static int      g_ext_att = 0;       /* -x dB */
static int      g_ota = 0;           /* -o: over the air, +30 dB budget */
static int      g_force = 0;         /* -f */
static int      g_yes = 0;           /* -y */
static long long g_fstep = 1000000;  /* interactive freq step, Hz */
static char     g_msg[200];
static char     g_profile[256];
static char     g_cfg[320];          /* saved state /etc/rf_tune/<board>.cfg */
static int      g_panel = 0;         /* interactive: off channels hold edits */
static int      g_noprep = 0;        /* -n: panel starts from sysfs only */
static int      g_color = 1;         /* -m / NO_COLOR: attributes only */
static int      g_ota_cli = 0;       /* -o given: cfg does not override */
static int      g_ext_cli = 0;       /* -x given: cfg does not override */
static int      g_stage_only_off = 0; /* set_kv: skip channels on in sysfs */
static int      g_stage_skip_en = 0;  /* set_kv: skip the enable field */

/* Error from set_kv: message line in the panel, stderr otherwise */
static void kv_err(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (g_panel) {
        vsnprintf(g_msg, sizeof(g_msg), fmt, ap);
    } else {
        fputs("rf_tune: ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

/* ----------------------------------------------------------------------
 * sysfs helpers
 * -------------------------------------------------------------------- */
static const char *ffile(const chan_t *c, int fi)
{
    return c->is_tx ? g_f[fi].tx_file : g_f[fi].rx_file;
}

static int sys_read(const char *ch, const char *f, char *out, size_t n)
{
    char p[384];
    FILE *fp;
    size_t l;

    snprintf(p, sizeof(p), "%s/%s/%s", g_base, ch, f);
    fp = fopen(p, "r");
    if (!fp)
        return -1;
    if (!fgets(out, (int)n, fp))
        out[0] = '\0';
    fclose(fp);
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == ' '))
        out[--l] = '\0';
    return 0;
}

static int sys_write(const char *ch, const char *f, const char *val)
{
    char p[384];
    FILE *fp;
    int ok;

    snprintf(p, sizeof(p), "%s/%s/%s", g_base, ch, f);
    fp = fopen(p, "w");
    if (!fp)
        return -errno;
    ok = fprintf(fp, "%s\n", val) > 0;
    if (fclose(fp) != 0 || !ok)
        return errno ? -errno : -EIO;
    return 0;
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* ----------------------------------------------------------------------
 * Board detection: -b, then dmesg, then sysfs shape
 * -------------------------------------------------------------------- */
static int board_by_name(const char *n)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_boards) / sizeof(g_boards[0])); i++)
        if (!strcasecmp(n, g_boards[i].name))
            return i;
    return -1;
}

static void detect_board(const char *forced)
{
    FILE *fp;
    char line[512];
    int found = -1, i;

    if (forced) {
        i = board_by_name(forced);
        if (i >= 0) {
            g_bd = &g_boards[i];
            snprintf(g_bd_how, sizeof(g_bd_how), "-b");
            return;
        }
        fprintf(stderr, "rf_tune: unknown board '%s'\n", forced);
    }
    /* dmesg describes the real board only: skipped for a test sysfs root */
    fp = getenv("RF_TUNE_SYS") ? NULL : popen("dmesg 2>/dev/null", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "RFNM: Loading ");
            char name[32];
            int slot;
            if (!p)
                continue;
            if (sscanf(p, "RFNM: Loading %31s driver for daughterboard at slot %d",
                       name, &slot) == 2 && slot == g_slot) {
                i = board_by_name(name);
                if (i >= 0)
                    found = i;
            }
        }
        pclose(fp);
    }
    if (found >= 0) {
        g_bd = &g_boards[found];
        snprintf(g_bd_how, sizeof(g_bd_how), "dmesg");
        return;
    }
    /* sysfs shape: rx1 present -> Granita or Yucca (2.5 GHz min) */
    {
        char p[320], v[32];
        snprintf(p, sizeof(p), "%s/rx1", g_base);
        if (path_exists(p)) {
            long long fmin = 0;
            if (sys_read("rx1", "freq_min", v, sizeof(v)) == 0)
                fmin = atoll(v);
            g_bd = &g_boards[fmin >= 2400000000LL ? 2 : 1];
        } else {
            g_bd = &g_boards[0];
        }
        snprintf(g_bd_how, sizeof(g_bd_how), "sysfs guess");
    }
}

static const char *const *chan_paths(const chan_t *c)
{
    if (c->is_tx)
        return g_bd->tx_paths;
    if (!strcmp(c->name, "rx1") && g_bd->rx1_paths)
        return g_bd->rx1_paths;
    return g_bd->rx_paths;
}

static int budget(void)
{
    return (g_budget >= 0 ? g_budget : g_bd->budget) + (g_ota ? 30 : 0);
}

/* ----------------------------------------------------------------------
 * Model <-> hardware
 * -------------------------------------------------------------------- */
static void chan_read(chan_t *c)
{
    char v[64];
    int fi;

    for (fi = 0; fi < NF; fi++) {
        const char *f = ffile(c, fi);
        c->has[fi] = 0;
        if (!f || sys_read(c->name, f, v, sizeof(v)) != 0)
            continue;
        c->has[fi] = 1;
        if (g_f[fi].type == T_ENUM) {
            snprintf(c->hs[fi], sizeof(c->hs[fi]), "%s", v);
            snprintf(c->cs[fi], sizeof(c->cs[fi]), "%s", v);
        } else {
            c->hv[fi] = atoll(v);
            c->cv[fi] = c->hv[fi];
        }
        c->dirty[fi] = 0;
    }
    c->fmin = 0;
    c->fmax = 8000000000LL;
    if (sys_read(c->name, "freq_min", v, sizeof(v)) == 0)
        c->fmin = atoll(v);
    if (sys_read(c->name, "freq_max", v, sizeof(v)) == 0)
        c->fmax = atoll(v);
}

static void scan_channels(void)
{
    static const char *const names[] = { "rx0", "rx1", "rx2", "rx3", "tx0", "tx1" };
    char p[320];
    int i;

    g_nch = 0;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        snprintf(p, sizeof(p), "%s/%s", g_base, names[i]);
        if (!path_exists(p))
            continue;
        memset(&g_ch[g_nch], 0, sizeof(g_ch[g_nch]));
        snprintf(g_ch[g_nch].name, sizeof(g_ch[g_nch].name), "%s", names[i]);
        g_ch[g_nch].is_tx = (names[i][0] == 't');
        chan_read(&g_ch[g_nch]);
        g_nch++;
    }
}

static chan_t *chan_by_name(const char *n)
{
    int i;
    for (i = 0; i < g_nch; i++)
        if (!strcmp(g_ch[i].name, n))
            return &g_ch[i];
    return NULL;
}

static int is_on_s(const char *s)
{
    return strcmp(s, "off") != 0;
}

static int chan_off_hw(const chan_t *c)
{
    return c->has[FI_EN] && !is_on_s(c->hs[FI_EN]);
}

static int chan_off_model(const chan_t *c)
{
    return c->has[FI_EN] && !is_on_s(c->cs[FI_EN]);
}

/* Panel only: channel off and staying off, its edits are held, not written */
static int chan_held(const chan_t *c)
{
    return g_panel && chan_off_hw(c) && chan_off_model(c);
}

static int chan_turning_on(const chan_t *c)
{
    return chan_off_hw(c) && !chan_off_model(c);
}

/* Pending changes that would be written (held edits excluded) */
static int any_dirty(void)
{
    int i, fi;
    for (i = 0; i < g_nch; i++) {
        if (chan_held(&g_ch[i]))
            continue;
        for (fi = 0; fi < NF; fi++)
            if (g_ch[i].dirty[fi])
                return 1;
    }
    return 0;
}

static void revert_field(chan_t *c, int fi)
{
    if (g_f[fi].type == T_ENUM)
        memcpy(c->cs[fi], c->hs[fi], sizeof(c->cs[fi]));
    else
        c->cv[fi] = c->hv[fi];
    c->dirty[fi] = 0;
}

static void revert_all(void)
{
    int i, fi;
    for (i = 0; i < g_nch; i++)
        for (fi = 0; fi < NF; fi++)
            if (g_ch[i].dirty[fi])
                revert_field(&g_ch[i], fi);
}

/*
 * Revert what was about to be written (refusal, cancel). Held edits are kept;
 * a channel that was turning on goes back to off and keeps its prepared values.
 */
static void revert_pending(void)
{
    int i, fi;
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        int on_turn = chan_turning_on(c);
        if (chan_held(c))
            continue;
        for (fi = 0; fi < NF; fi++) {
            if (!c->dirty[fi])
                continue;
            if (g_panel && on_turn && fi != FI_EN)
                continue;
            revert_field(c, fi);
        }
    }
}

static void mark(chan_t *c, int fi)
{
    if (g_f[fi].type == T_ENUM)
        c->dirty[fi] = strcmp(c->cs[fi], c->hs[fi]) != 0;
    else
        c->dirty[fi] = c->cv[fi] != c->hv[fi];
    c->drv[fi] = 0;
}

static long long clampll(long long v, long long lo, long long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void range_of(const chan_t *c, int fi, long long *lo, long long *hi)
{
    switch (fi) {
    case FI_FREQ: *lo = c->fmin; *hi = c->fmax; break;
    case FI_GAIN:
        *lo = c->is_tx ? g_bd->tp_min : g_bd->rg_min;
        *hi = c->is_tx ? g_bd->tp_max : g_bd->rg_max;
        break;
    case FI_LPF: *lo = 0; *hi = 160; break;
    default: *lo = 0; *hi = 0; break;
    }
}

/* ----------------------------------------------------------------------
 * Guards. Returns 0 ok, 1 confirmation needed, 2 refused.
 * -------------------------------------------------------------------- */
static void loop_state(int *tx_on, int *rx_on, int *gmax, int *pmax, int use_hw)
{
    int i;
    *tx_on = *rx_on = 0;
    *gmax = -1000;
    *pmax = -1000;
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        const char *en = use_hw ? c->hs[FI_EN] : c->cs[FI_EN];
        long long g = use_hw ? c->hv[FI_GAIN] : c->cv[FI_GAIN];
        if (!c->has[FI_EN] || !is_on_s(en))
            continue;
        if (c->is_tx) {
            (*tx_on)++;
            if (g > *pmax) *pmax = (int)g;
        } else {
            (*rx_on)++;
            if (g > *gmax) *gmax = (int)g;
        }
    }
}

static void guard_line(char *out, size_t n, int use_hw)
{
    int tx_on, rx_on, g, p;
    loop_state(&tx_on, &rx_on, &g, &p, use_hw);
    if (!tx_on || !rx_on) {
        snprintf(out, n, "guard: %s, loop budget not active",
                 tx_on ? "RX off" : "TX off");
        return;
    }
    snprintf(out, n, "guard: TX+RX on, gain %d + power %d - ext %d = %d %s %d (%s)  %s",
             g, p, g_ext_att, g + p - g_ext_att,
             (g + p - g_ext_att) > budget() ? ">" : "<=", budget(),
             g_ota ? "ota" : "cabled",
             (g + p - g_ext_att) > budget() ? "OVER" : "OK");
}

static int guard_check(char *why, size_t n)
{
    int i, lvl = 0, tx_on, rx_on, g, p, rx_lb = 0;
    why[0] = '\0';

    loop_state(&tx_on, &rx_on, &g, &p, 0);
    if (tx_on && rx_on && g + p - g_ext_att > budget()) {
        int g0, p0, t0, r0;
        loop_state(&t0, &r0, &g0, &p0, 1);
        /* refuse only changes that raise the loop level */
        if (!(t0 && r0) || g + p > g0 + p0) {
            snprintf(why, n, "loop budget: rx gain %d + tx power %d - ext %d dB > %d %s",
                     g, p, g_ext_att, budget(),
                     g_panel ? (g_ota ? "(over the air; x = ext att)" :
                                "(cabled; o = over the air, x = ext att)") :
                     "(-G/-x/-o to change)");
            return 2;
        }
    }
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        if (!c->is_tx && !chan_off_model(c) && c->has[FI_PATH] &&
            !strcmp(c->cs[FI_PATH], "loopback"))
            rx_lb = 1;
    }
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        if (chan_held(c))
            continue;
        if (c->is_tx && c->has[FI_PATH] && !strcmp(c->cs[FI_PATH], "loopback") &&
            (c->dirty[FI_PATH] || chan_turning_on(c)) && !rx_lb) {
            snprintf(why, n, "%s loopback needs an RX channel on the loopback path first",
                     c->name);
            return 2;
        }
    }
    for (i = 0; i < g_nch && lvl == 0; i++) {
        chan_t *c = &g_ch[i];
        int on_turn = chan_turning_on(c);
        if (chan_held(c))
            continue;
        if (c->is_tx && c->cv[FI_GAIN] > g_bd->pa_thr &&
            (on_turn || (c->dirty[FI_GAIN] && c->hv[FI_GAIN] <= g_bd->pa_thr))) {
            snprintf(why, n, "%s power %lld above %d: next PA stage on",
                     c->name, c->cv[FI_GAIN], g_bd->pa_thr);
            lvl = 1;
        } else if ((c->dirty[FI_BIAS] || on_turn) && c->has[FI_BIAS] &&
                   !strcmp(c->cs[FI_BIAS], "on")) {
            snprintf(why, n, "%s bias-tee on: DC on the %s SMA, no TX or test equipment"
                     " on that port", c->name, c->cs[FI_PATH]);
            lvl = 1;
        } else if (c->is_tx && c->has[FI_PATH] && !strcmp(c->cs[FI_PATH], "loopback") &&
                   c->cv[FI_GAIN] != 0 &&
                   (c->dirty[FI_GAIN] || c->dirty[FI_PATH] || on_turn)) {
            snprintf(why, n, "%s loopback with power %lld (0 = calibration mode)",
                     c->name, c->cv[FI_GAIN]);
            lvl = 1;
        }
    }
    return lvl;
}

/* ----------------------------------------------------------------------
 * Apply
 * -------------------------------------------------------------------- */
static const int g_worder[] = { FI_EN, FI_PATH, FI_FREQ, FI_GAIN, FI_LPF,
                                FI_AGC, FI_NOTCH, FI_BIAS };

/* mode: 2 main fields + dirty (channel turning on), 3 enable only, else dirty */
static int chan_write(chan_t *c, int mode)
{
    char v[40];
    int k, rc;

    for (k = 0; k < NF; k++) {
        int fi = g_worder[k];
        if (!c->has[fi])
            continue;
        if (mode == 3 ? fi != FI_EN :
            mode == 2 ? !(fi <= FI_LPF || c->dirty[fi]) : !c->dirty[fi])
            continue;
        if (g_f[fi].type == T_ENUM)
            snprintf(v, sizeof(v), "%s", c->cs[fi]);
        else
            snprintf(v, sizeof(v), "%lld", c->cv[fi]);
        rc = sys_write(c->name, ffile(c, fi), v);
        if (rc) {
            snprintf(g_msg, sizeof(g_msg), "write %s/%s=%s failed: %s",
                     c->name, ffile(c, fi), v, strerror(-rc));
            return rc;
        }
    }
    return 0;
}

static int chan_apply(chan_t *c)
{
    int rc = sys_write(c->name, "apply", "1");
    if (rc)
        snprintf(g_msg, sizeof(g_msg), "%s apply failed: %s", c->name, strerror(-rc));
    return rc;
}

static int chan_is_dirty(const chan_t *c)
{
    int fi;
    for (fi = 0; fi < NF; fi++)
        if (c->dirty[fi])
            return 1;
    return 0;
}

/*
 * Write the pending fields and apply, in an order safe for the front-end.
 * act per channel: 1 dirty fields, 2 turning on (main fields + dirty),
 * 3 turning off in the panel (enable only, other edits stay held).
 */
static int do_apply(void)
{
    char done[160] = "";
    int i, rc = 0, rx_chg = 0, tx_off = 0;
    int written[MAX_CH][NF], act[MAX_CH];

    memset(written, 0, sizeof(written));
    memset(act, 0, sizeof(act));
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        int fi;
        if (chan_held(c) || !chan_is_dirty(c))
            continue;
        act[i] = chan_turning_on(c) ? 2 :
                 (g_panel && c->dirty[FI_EN] && chan_off_model(c)) ? 3 : 1;
        for (fi = 0; fi < NF; fi++)
            written[i][fi] = !c->has[fi] ? 0 :
                             act[i] == 2 ? (fi <= FI_LPF || c->dirty[fi]) :
                             act[i] == 3 ? fi == FI_EN : c->dirty[fi];
        if (!c->is_tx)
            rx_chg = 1;
        else if (c->dirty[FI_EN] && chan_off_model(c))
            tx_off = 1;
    }

    /* 1. TX turning off goes first so the RX apply gets the port back */
    if (tx_off) {
        for (i = 0; i < g_nch && !rc; i++) {
            chan_t *c = &g_ch[i];
            if (!c->is_tx || !act[i])
                continue;
            rc = chan_write(c, act[i]);
            if (!rc) rc = chan_apply(c);
            if (!rc) { strcat(done, " "); strcat(done, c->name); }
        }
    }
    /* 2. RX channels */
    for (i = 0; i < g_nch && !rc; i++) {
        chan_t *c = &g_ch[i];
        int reapply = tx_off && g_bd->lime_port_rule && !chan_off_model(c);
        if (c->is_tx || (!act[i] && !reapply))
            continue;
        rc = chan_write(c, act[i]);
        if (!rc) rc = chan_apply(c);
        if (!rc) { strcat(done, " "); strcat(done, c->name); }
    }
    /* 3. TX channels (changed, or Lime re-apply after an RX apply) */
    for (i = 0; i < g_nch && !rc; i++) {
        chan_t *c = &g_ch[i];
        int reapply = rx_chg && g_bd->lime_port_rule && !chan_off_model(c);
        if (!c->is_tx || tx_off)
            continue;
        if (!act[i] && !reapply)
            continue;
        rc = chan_write(c, act[i]);
        if (!rc) rc = chan_apply(c);
        if (!rc) {
            strcat(done, " ");
            strcat(done, c->name);
            if (!act[i])
                strcat(done, "(re-apply)");
        }
    }

    /*
     * Read back: the model follows the driver (changed values flagged); in the
     * panel a channel that is off keeps its held values.
     */
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        char cs[NF][24];
        long long cv[NF];
        int fi, keep;
        memcpy(cs, c->cs, sizeof(cs));
        memcpy(cv, c->cv, sizeof(cv));
        chan_read(c);
        keep = g_panel && chan_off_hw(c);
        for (fi = 0; fi < NF; fi++) {
            if (keep && fi != FI_EN && c->has[fi]) {
                memcpy(c->cs[fi], cs[fi], sizeof(cs[fi]));
                c->cv[fi] = cv[fi];
                mark(c, fi);
            } else if (written[i][fi]) {
                if (g_f[fi].type == T_ENUM)
                    c->drv[fi] = strcmp(cs[fi], c->hs[fi]) != 0;
                else
                    c->drv[fi] = cv[fi] != c->hv[fi];
            }
        }
    }
    if (!rc)
        snprintf(g_msg, sizeof(g_msg), "applied:%s", done[0] ? done : " nothing");
    return rc;
}

/* Check guards then apply. confirmed = user said yes to a level-1 guard. */
static int g_pending_confirm;

static int try_apply(int confirmed, int interactive)
{
    char why[200];
    int lvl;

    if (!any_dirty())
        return 0;
    lvl = guard_check(why, sizeof(why));
    if (lvl == 2 && !g_force) {
        snprintf(g_msg, sizeof(g_msg), "REFUSED: %s", why);
        if (interactive)
            revert_pending();
        return 2;
    }
    if (lvl == 1 && !confirmed && !g_yes && !g_force) {
        if (interactive) {
            g_pending_confirm = 1;
            snprintf(g_msg, sizeof(g_msg), "CONFIRM: %s  -- y apply / n cancel", why);
        } else {
            snprintf(g_msg, sizeof(g_msg), "needs confirmation (-y): %s", why);
        }
        return 1;
    }
    g_pending_confirm = 0;
    return do_apply() ? 3 : 0;
}

/* ----------------------------------------------------------------------
 * Editing
 * -------------------------------------------------------------------- */
static const char *const *enum_opts(const chan_t *c, int fi)
{
    return fi == FI_PATH ? chan_paths(c) : g_f[fi].opts;
}

static void adjust(chan_t *c, int fi, int dir, int big)
{
    if (!c->has[fi])
        return;
    if (g_f[fi].type == T_ENUM) {
        const char *const *o = enum_opts(c, fi);
        int n = 0, cur = -1, k;
        for (k = 0; o[k]; k++) {
            if (!strcmp(o[k], c->cs[fi]))
                cur = k;
            n++;
        }
        if (n == 0)
            return;
        cur = (cur < 0) ? 0 : (cur + dir + n) % n;
        snprintf(c->cs[fi], sizeof(c->cs[fi]), "%s", o[cur]);
    } else {
        long long lo, hi, st;
        range_of(c, fi, &lo, &hi);
        if (fi == FI_FREQ)
            st = g_fstep * (big ? 10 : 1);
        else if (fi == FI_LPF)
            st = big ? 10 : 1;
        else
            st = big ? 5 : 1;
        c->cv[fi] = clampll(c->cv[fi] + dir * st, lo, hi);
    }
    mark(c, fi);
}

/* Parse "1850M", "1.85G", "1850000000", "-12" */
static int parse_num(const char *s, int is_freq_mhz_default, long long *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s)
        return -1;
    if (*end == 'k' || *end == 'K') v *= 1e3;
    else if (*end == 'm' || *end == 'M') v *= 1e6;
    else if (*end == 'g' || *end == 'G') v *= 1e9;
    else if (*end == '\0' && is_freq_mhz_default) v *= 1e6;
    else if (*end != '\0') return -1;
    *out = (long long)(v + (v >= 0 ? 0.5 : -0.5));
    return 0;
}

static int field_by_key(const chan_t *c, const char *k)
{
    int fi;
    if (!strcmp(k, "gain") || !strcmp(k, "power"))
        return FI_GAIN;
    if (!strcmp(k, "lpf") || !strcmp(k, "lpf_bw"))
        return FI_LPF;
    for (fi = 0; fi < NF; fi++) {
        const char *f = ffile(c, fi);
        if (f && !strcmp(f, k))
            return fi;
    }
    return -1;
}

/* key "rx0.gain", value string. Returns 0 ok. */
static int set_kv(const char *key, const char *val)
{
    char ch[8], fk[24];
    const char *dot = strchr(key, '.');
    chan_t *c;
    int fi;

    if (!dot || dot - key >= (long)sizeof(ch)) {
        kv_err("bad key '%s' (expected ch.field)", key);
        return -1;
    }
    memcpy(ch, key, (size_t)(dot - key));
    ch[dot - key] = '\0';
    snprintf(fk, sizeof(fk), "%s", dot + 1);
    c = chan_by_name(ch);
    if (!c) {
        kv_err("no channel '%s'", ch);
        return -1;
    }
    if (g_stage_only_off && !chan_off_hw(c))
        return 0;               /* panel start: channel on stays live */
    fi = field_by_key(c, fk);
    if (fi == FI_EN && g_stage_skip_en)
        return 0;               /* panel load: enable is not taken */
    if (fi < 0 || !c->has[fi]) {
        kv_err("%s has no field '%s'", ch, fk);
        return -1;
    }
    if (g_f[fi].type == T_ENUM) {
        const char *const *o = enum_opts(c, fi);
        int k, ok = 0;
        for (k = 0; o[k]; k++)
            if (!strcmp(o[k], val))
                ok = 1;
        if (!ok && !g_force) {
            kv_err("%s.%s: '%s' not allowed on %s (-f to force)",
                   ch, fk, val, g_bd->name);
            return -1;
        }
        snprintf(c->cs[fi], sizeof(c->cs[fi]), "%s", val);
    } else {
        long long v, lo, hi;
        if (parse_num(val, 0, &v)) {
            kv_err("%s.%s: bad number '%s'", ch, fk, val);
            return -1;
        }
        range_of(c, fi, &lo, &hi);
        if (fi != FI_LPF && (v < lo || v > hi)) {
            kv_err("%s.%s=%lld clamped to %lld..%lld", ch, fk, v, lo, hi);
            v = clampll(v, lo, hi);
        }
        c->cv[fi] = v;
    }
    mark(c, fi);
    return 0;
}

static void safe_state(void)
{
    int i;
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        if (!c->has[FI_GAIN])
            continue;
        if (c->is_tx)
            c->cv[FI_GAIN] = g_bd->tp_min;
        else
            c->cv[FI_GAIN] = clampll(0, g_bd->rg_min, g_bd->rg_max);
        mark(c, FI_GAIN);
    }
}

/* ----------------------------------------------------------------------
 * Profiles: key=value, same keys as -c
 * -------------------------------------------------------------------- */
static int profile_save(const char *path)
{
    char tmp[340], bak[340];
    FILE *fp;
    int i, fi;

    /* write a temp file, keep the old one as .bak, then rename */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    {
        char dir[320], *sl;
        snprintf(dir, sizeof(dir), "%s", path);
        sl = strrchr(dir, '/');
        if (sl && sl != dir) {
            *sl = '\0';
            mkdir(dir, 0755);
        }
    }
    fp = fopen(tmp, "w");
    if (!fp) {
        snprintf(g_msg, sizeof(g_msg), "save %s: %s", path, strerror(errno));
        return -1;
    }
    /* panel values: live channels as applied, off channels with held edits */
    fprintf(fp, "# rf_tune profile, board=%s, %s\n", g_bd->name, g_base);
    fprintf(fp, "#!ota=%d\n#!ext_att=%d\n", g_ota, g_ext_att);
    for (i = 0; i < g_nch; i++) {
        chan_t *c = &g_ch[i];
        for (fi = 0; fi < NF; fi++) {
            if (!c->has[fi])
                continue;
            if (g_f[fi].type == T_ENUM)
                fprintf(fp, "%s.%s=%s\n", c->name, ffile(c, fi), c->cs[fi]);
            else
                fprintf(fp, "%s.%s=%lld\n", c->name, ffile(c, fi), c->cv[fi]);
        }
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        snprintf(g_msg, sizeof(g_msg), "save %s: write error", path);
        return -1;
    }
    if (path_exists(path))
        rename(path, bak);
    if (rename(tmp, path) != 0) {
        snprintf(g_msg, sizeof(g_msg), "save %s: %s", path, strerror(errno));
        return -1;
    }
    snprintf(g_msg, sizeof(g_msg), "saved %s", path);
    return 0;
}

static const char *const *board_defaults(void)
{
    int i = (int)(g_bd - g_boards);
    return (i >= 0 && i < BOARD_UNKNOWN) ? g_defs[i] : NULL;
}

/*
 * Check the built-in table of every board against that board's ranges
 * and paths. Returns the number of bad lines (printed on stderr).
 */
static int defaults_selfcheck(void)
{
    int b, k, bad = 0;

    for (b = 0; b < BOARD_UNKNOWN; b++) {
        const board_t *bd = &g_boards[b];
        for (k = 0; g_defs[b][k]; k++) {
            const char *s = g_defs[b][k], *eq = strchr(s, '=');
            const char *f = strchr(s, '.');
            int tx = !strncmp(s, "tx", 2), ok = 1;

            if (!eq || !f) {
                ok = 0;
            } else if (!strncmp(f + 1, "gain=", 5)) {
                int v = atoi(eq + 1);
                ok = v >= bd->rg_min && v <= bd->rg_max;
            } else if (!strncmp(f + 1, "power=", 6)) {
                int v = atoi(eq + 1);
                ok = v >= bd->tp_min && v <= bd->tp_max && v <= bd->pa_thr;
            } else if (!strncmp(f + 1, "path=", 5)) {
                const char *const *p = tx ? bd->tx_paths :
                    (!strncmp(s, "rx1", 3) && bd->rx1_paths ? bd->rx1_paths : bd->rx_paths);
                ok = 0;
                for (; *p; p++)
                    if (!strcmp(*p, eq + 1))
                        ok = 1;
            } else if (!strncmp(f + 1, "freq=", 5) && b == 2) {
                long long v = atoll(eq + 1);
                ok = v >= 2496000000LL && v <= 5000000000LL;
            }
            if (!ok) {
                fprintf(stderr, "rf_tune: built-in default '%s' invalid for %s\n",
                        s, bd->name);
                bad++;
            }
        }
    }
    return bad;
}

/* Stage the built-in defaults of the current board (applied by caller) */
static int defaults_stage(void)
{
    const char *const *d = board_defaults();
    int k, err = 0;

    if (!d) {
        snprintf(g_msg, sizeof(g_msg), "no built-in defaults for board %s", g_bd->name);
        return -1;
    }
    for (k = 0; d[k]; k++) {
        char kv[64], *eq, ch[8];
        const char *dot;
        snprintf(kv, sizeof(kv), "%s", d[k]);
        dot = strchr(kv, '.');
        eq = strchr(kv, '=');
        if (!dot || !eq || dot - kv >= (long)sizeof(ch))
            continue;
        memcpy(ch, kv, (size_t)(dot - kv));
        ch[dot - kv] = '\0';
        if (!chan_by_name(ch))
            continue;           /* channel not on this board */
        *eq = '\0';
        if (set_kv(kv, eq + 1))
            err = 1;
    }
    if (err) {
        revert_all();
        snprintf(g_msg, sizeof(g_msg), "built-in defaults rejected, nothing applied");
        return -1;
    }
    snprintf(g_msg, sizeof(g_msg), "built-in defaults for %s staged", g_bd->name);
    return 0;
}

static int profile_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    int err = 0;

    if (!fp) {
        snprintf(g_msg, sizeof(g_msg), "load %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *eq, *e;
        /* "#!" lines: panel mode, ignored as comments by rf_tune 1.0 */
        if (!strncmp(line, "#!ota=", 6)) {
            if (!g_ota_cli)
                g_ota = atoi(line + 6) != 0;
            continue;
        }
        if (!strncmp(line, "#!ext_att=", 10)) {
            if (!g_ext_cli)
                g_ext_att = atoi(line + 10);
            continue;
        }
        if (line[0] == '#' || line[0] == '\n')
            continue;
        e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\r'))
            *--e = '\0';
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (set_kv(line, eq + 1))
            err = 1;
    }
    fclose(fp);
    if (err && !g_force) {
        revert_all();
        snprintf(g_msg, sizeof(g_msg), "load %s: errors, nothing applied (-f to force)", path);
        return -1;
    }
    snprintf(g_msg, sizeof(g_msg), "loaded %s", path);
    return 0;
}

/* ----------------------------------------------------------------------
 * Display
 * -------------------------------------------------------------------- */
static void fmt_val(const chan_t *c, int fi, char *out, size_t n)
{
    if (!c->has[fi]) {
        snprintf(out, n, "-");
    } else if (g_f[fi].type == T_ENUM) {
        snprintf(out, n, "%s", c->cs[fi]);
    } else if (fi == FI_FREQ) {
        snprintf(out, n, "%.6f MHz", (double)c->cv[fi] / 1e6);
    } else if (fi == FI_GAIN) {
        snprintf(out, n, "%lld %s", c->cv[fi], c->is_tx ? "pwr" : "dB");
    } else {
        snprintf(out, n, "%lld", c->cv[fi]);
    }
}

/* One-line hint for the selected field: range and board notes */
static void field_note(const chan_t *c, int fi, char *out, size_t n)
{
    long long lo, hi;
    int tx_on = 0, i;

    for (i = 0; i < g_nch; i++)
        if (g_ch[i].is_tx && is_on_s(g_ch[i].cs[FI_EN]))
            tx_on = 1;
    range_of(c, fi, &lo, &hi);
    if (fi == FI_FREQ) {
        snprintf(out, n, "%s freq %.3f..%.3f MHz, step %.3f MHz (f to change, [ ] x10)",
                 c->name, (double)lo / 1e6, (double)hi / 1e6, (double)g_fstep / 1e6);
    } else if (fi == FI_GAIN && c->is_tx) {
        long long p = c->cv[fi];
        const char *pa = "";
        char yuc[48] = "";
        if (!strcmp(g_bd->name, "lime"))
            pa = p <= 0 ? ", PA off" : (p <= 20 ? ", PA1 on" : ", PA1+PA2 on (40 = max)");
        if (!strcmp(g_bd->name, "yucca"))
            snprintf(yuc, sizeof(yuc), ", PA step %lld/31, uncalibrated", p + 10);
        snprintf(out, n, "%s power %lld..%lld (confirm above %d)%s%s",
                 c->name, lo, hi, g_bd->pa_thr, pa, yuc);
    } else if (fi == FI_GAIN) {
        const char *extra = "";
        if (!strcmp(g_bd->name, "lime"))
            extra = c->cv[fi] < 0 && tx_on ?
                    ", TX on: attenuators bypassed, effective 0 dB" :
                    ", <0 uses the 12/24 dB attenuators, PGA DC-guarded";
        snprintf(out, n, "%s gain %lld..%lld dB%s", c->name, lo, hi, extra);
    } else if (fi == FI_PATH) {
        const char *const *o = chan_paths(c);
        int k;
        size_t l = (size_t)snprintf(out, n, "%s path:", c->name);
        for (k = 0; o[k] && l < n; k++)
            l += (size_t)snprintf(out + l, n - l, " %s", o[k]);
    } else if (fi == FI_BIAS) {
        snprintf(out, n, "%s bias-tee: DC on the SMA, confirmation needed to turn on",
                 c->name);
    } else if (fi == FI_EN) {
        snprintf(out, n, "%s enable: off / on / tdd", c->name);
    } else {
        snprintf(out, n, "%s %s", c->name, g_f[fi].label);
    }
}

static int g_sel_ch, g_sel_f;
static char g_entry[24];
static int g_entry_on;
static int g_entry_tgt;              /* 1: entry is the external attenuation */

/* Message class: 2 refused, 1 confirmation / warning, 0 info */
static int msg_class(const char *m)
{
    if (!strncmp(m, "REFUSED", 7))
        return 2;
    if (!strncmp(m, "CONFIRM", 7) || !strncmp(m, "WARN", 4))
        return 1;
    return 0;
}

/* Background colours stay readable on dark and light terminal themes */
#define A_REFUSED (g_color ? "\033[1;37;41m" : "\033[1m")
#define A_CONFIRM (g_color ? "\033[30;43m" : "\033[1m")
#define A_DRIVER  (g_color ? "\033[31m" : "")
#define A_PENDING "\033[1;4m"
#define A_HELD    "\033[2m"

static void render(void)
{
    char v[48], note[200], gl[200];
    int i, fi;

    printf("\033[H");
    printf("rf_tune %s  %s  board: %s (%s)  ext att %d dB  %s\033[K\n",
           RF_TUNE_VERSION, g_base, g_bd->name, g_bd_how, g_ext_att,
           g_ota ? "over-the-air" : "cabled");
    printf("\033[K\n%-10s", "");
    for (i = 0; i < g_nch; i++)
        printf("  %-20s", g_ch[i].name);
    printf("\033[K\n");
    for (fi = 0; fi < NF; fi++) {
        int any = 0;
        for (i = 0; i < g_nch; i++)
            any |= g_ch[i].has[fi];
        if (!any)
            continue;
        printf("%-10s", g_f[fi].label);
        for (i = 0; i < g_nch; i++) {
            chan_t *c = &g_ch[i];
            int sel = (i == g_sel_ch && fi == g_sel_f), pad;
            const char *attr = "";
            char mk = ' ';
            fmt_val(c, fi, v, sizeof(v));
            if (c->drv[fi]) {
                mk = '!';
                attr = A_DRIVER;
            } else if (c->dirty[fi] && chan_held(c)) {
                mk = '~';
                attr = A_HELD;
            } else if (c->dirty[fi]) {
                mk = '*';
                attr = A_PENDING;
            }
            if (sel && g_entry_on && !g_entry_tgt) {
                snprintf(v, sizeof(v), "> %s_", g_entry);
                mk = ' ';
                attr = "";
            }
            pad = 19 - (int)strlen(v);
            if (pad < 0)
                pad = 0;
            printf("  %s%s%s%c\033[0m%*s", sel ? "\033[7m" : "", attr, v, mk, pad, "");
        }
        printf("\033[K\n");
    }
    printf("\033[K\n");
    field_note(&g_ch[g_sel_ch], g_sel_f, note, sizeof(note));
    if (chan_off_model(&g_ch[g_sel_ch])) {
        size_t l = strlen(note);
        snprintf(note + l, sizeof(note) - l, "  [off: edits held, applied on enable]");
    }
    printf("%s\033[K\n", note);
    guard_line(gl, sizeof(gl), 0);
    if (strstr(gl, "OVER"))
        printf("%s!! %s\033[0m\033[K\n", A_REFUSED, gl);
    else
        printf("%s\033[K\n", gl);
    if (g_entry_on && g_entry_tgt) {
        printf("external attenuation dB: > %s_   (Enter set, Esc cancel)\033[K\n", g_entry);
    } else {
        int k = msg_class(g_msg);
        printf("%s%s%s\033[0m\033[K\n", k == 2 ? A_REFUSED : k == 1 ? A_CONFIRM : "",
               k == 2 ? "!! " : k == 1 ? "?? " : "", g_msg);
    }
    printf("keys: up/dn field  tab , . column  -/+ lt/rt change  [ ] big step"
           "  f freq step  0-9 type value\033[K\n");
    printf("      z safe (TX min, RX 0)  s/l save/load profile  r re-read sysfs"
           "  o cabled/over the air  x ext att  q quit\033[K\n");
    printf("      S save next-run defaults  L load them  R reset to built-in"
           "  (%s)\033[K\n", g_cfg);
    printf("      * pending  ~ held (channel off, applied on enable)"
           "  ! changed by driver\033[K\n");
    printf("\033[J");
    fflush(stdout);
}

static void print_status(void)
{
    char v[48], gl[200];
    int i, fi;

    printf("rf_tune %s: %s, board %s (%s)\n", RF_TUNE_VERSION, g_base, g_bd->name, g_bd_how);
    printf("%-10s", "");
    for (i = 0; i < g_nch; i++)
        printf("  %-20s", g_ch[i].name);
    printf("\n");
    for (fi = 0; fi < NF; fi++) {
        int any = 0;
        for (i = 0; i < g_nch; i++)
            any |= g_ch[i].has[fi];
        if (!any)
            continue;
        printf("%-10s", g_f[fi].label);
        for (i = 0; i < g_nch; i++) {
            fmt_val(&g_ch[i], fi, v, sizeof(v));
            printf("  %-20s", v);
        }
        printf("\n");
    }
    printf("ranges: rx gain %d..%d dB, tx power %d..%d (PA confirm > %d), budget %d\n",
           g_bd->rg_min, g_bd->rg_max, g_bd->tp_min, g_bd->tp_max, g_bd->pa_thr, budget());
    guard_line(gl, sizeof(gl), 1);
    printf("%s\n", gl);
}

/* ----------------------------------------------------------------------
 * Terminal
 * -------------------------------------------------------------------- */
static struct termios g_tty;
static int g_tty_ok;

static void tty_restore(void)
{
    if (g_tty_ok) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty);
        printf("\033[0m\033[?25h\r\n");
        fflush(stdout);
        g_tty_ok = 0;
    }
}

static int tty_raw(void)
{
    struct termios raw;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_tty) != 0)
        return -1;
    g_tty_ok = 1;
    atexit(tty_restore);
    raw = g_tty;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    return 0;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_key(int timeout_ms)
{
    fd_set fds;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    unsigned char ch;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return -1;
    if (read(STDIN_FILENO, &ch, 1) != 1)
        return -1;
    if (ch == 27) {
        unsigned char s[2];
        tv.tv_sec = 0;
        tv.tv_usec = 30000;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0 &&
            read(STDIN_FILENO, &s[0], 1) == 1 && s[0] == '[' &&
            read(STDIN_FILENO, &s[1], 1) == 1)
            return 256 + s[1];   /* 256+'A' up, 'B' down, 'C' right, 'D' left */
        return 27;
    }
    return ch;
}

static void move_field(int dir)
{
    int k;
    for (k = 0; k < NF; k++) {
        g_sel_f = (g_sel_f + dir + NF) % NF;
        if (g_ch[g_sel_ch].has[g_sel_f])
            return;
    }
}

static void move_col(int dir)
{
    g_sel_ch = (g_sel_ch + dir + g_nch) % g_nch;
    if (!g_ch[g_sel_ch].has[g_sel_f])
        move_field(1);
}

static void entry_commit(void)
{
    chan_t *c = &g_ch[g_sel_ch];
    long long v, lo, hi;

    g_entry_on = 0;
    if (g_entry_tgt) {
        g_entry_tgt = 0;
        if (parse_num(g_entry, 0, &v) || v < 0 || v > 200) {
            snprintf(g_msg, sizeof(g_msg), "bad attenuation '%s' (0..200 dB)", g_entry);
            return;
        }
        g_ext_att = (int)v;
        snprintf(g_msg, sizeof(g_msg), "external attenuation %d dB, loop budget %d",
                 g_ext_att, budget());
        return;
    }
    if (g_f[g_sel_f].type == T_ENUM)
        return;
    if (parse_num(g_entry, g_sel_f == FI_FREQ, &v)) {
        snprintf(g_msg, sizeof(g_msg), "bad value '%s'", g_entry);
        return;
    }
    range_of(c, g_sel_f, &lo, &hi);
    c->cv[g_sel_f] = clampll(v, lo, hi);
    mark(c, g_sel_f);
}

/*
 * Panel start: channels on in sysfs stay live; channels off are prepared from
 * the saved defaults (or the built-in table) and applied when enabled.
 * Nothing is written. src gets a one-line summary.
 */
static void panel_prepare(char *src, size_t n)
{
    const char *from = "built-in defaults";
    size_t l = 0;
    int i, rc;

    if (g_noprep) {
        snprintf(src, n, "all channels from sysfs (-n), nothing applied");
        return;
    }
    g_stage_only_off = 1;
    g_stage_skip_en = 1;
    if (path_exists(g_cfg)) {
        const char *sl = strrchr(g_cfg, '/');
        from = sl ? sl + 1 : g_cfg;
        rc = profile_load(g_cfg);
    } else {
        rc = defaults_stage();
    }
    g_stage_only_off = 0;
    g_stage_skip_en = 0;
    if (rc) {
        snprintf(src, n, "%s", g_msg);
        return;
    }
    for (i = 0; i < g_nch && l < n; i++)
        l += (size_t)snprintf(src + l, n - l, "%s%s %s", i ? ", " : "", g_ch[i].name,
                              chan_off_hw(&g_ch[i]) ? "prepared" : "live");
    if (l < n)
        snprintf(src + l, n - l, " (from %s), nothing applied", from);
}

/*
 * Panel s/l, L, R: load into the panel without the enable field. Channels on
 * apply at once (guards checked), channels off hold the values.
 */
static void panel_load(const char *path)
{
    int rc;

    revert_all();
    g_stage_skip_en = 1;
    rc = path ? profile_load(path) : defaults_stage();
    g_stage_skip_en = 0;
    if (rc == 0)
        try_apply(0, 1);
}

static int interactive(void)
{
    static const long long steps[] = { 1000, 10000, 100000, 1000000, 10000000 };
    long long last_key = 0;
    int si = 3, tx_on, rx_on, g, p, pending_reset = 0;

    char src[200];

    if (tty_raw()) {
        fprintf(stderr, "rf_tune: stdin is not a terminal (use -s / -c / -l)\n");
        return 1;
    }
    g_panel = 1;
    panel_prepare(src, sizeof(src));
    loop_state(&tx_on, &rx_on, &g, &p, 1);
    if (tx_on && rx_on)
        snprintf(g_msg, sizeof(g_msg), "WARNING: RX and TX on at start, z = safe state; %s",
                 src);
    else
        snprintf(g_msg, sizeof(g_msg), "%s", src);
    g_sel_ch = 0;
    g_sel_f = FI_GAIN;
    if (!g_ch[0].has[g_sel_f])
        move_field(1);
    printf("\033[?25l\033[2J");
    render();

    for (;;) {
        int k = read_key(50);
        chan_t *c = &g_ch[g_sel_ch];

        if (k < 0) {
            if (!g_pending_confirm && !g_entry_on && any_dirty() &&
                now_ms() - last_key >= 150) {
                try_apply(0, 1);
                render();
            }
            continue;
        }
        last_key = now_ms();
        if (k == 'q' || k == 'Q' || k == 3)
            break;
        if (pending_reset) {
            pending_reset = 0;
            if (k == 'y' || k == 'Y') {
                panel_load(NULL);
            } else {
                snprintf(g_msg, sizeof(g_msg), "reset cancelled");
            }
            render();
            continue;
        }
        if (g_pending_confirm) {
            if (k == 'y' || k == 'Y') {
                try_apply(1, 1);
            } else {
                revert_pending();
                g_pending_confirm = 0;
                snprintf(g_msg, sizeof(g_msg), "cancelled");
            }
            render();
            continue;
        }
        if (g_entry_on) {
            size_t l = strlen(g_entry);
            if (k == '\r' || k == '\n')
                entry_commit();
            else if (k == 27)
                g_entry_on = 0, g_entry_tgt = 0;
            else if ((k == 127 || k == 8) && l)
                g_entry[l - 1] = '\0';
            else if (l + 1 < sizeof(g_entry) && k < 128 &&
                     (strchr("0123456789.-kKmMgG", k)))
                g_entry[l] = (char)k, g_entry[l + 1] = '\0';
            render();
            continue;
        }
        switch (k) {
        case 256 + 'A': move_field(-1); break;
        case 256 + 'B': move_field(1); break;
        case '\t': case '.': move_col(1); break;
        case ',': move_col(-1); break;
        case 256 + 'C': case '+': case '=': adjust(c, g_sel_f, 1, 0); break;
        case 256 + 'D': case '-': case '_': adjust(c, g_sel_f, -1, 0); break;
        case ']': adjust(c, g_sel_f, 1, 1); break;
        case '[': adjust(c, g_sel_f, -1, 1); break;
        case 'f':
            si = (si + 1) % 5;
            g_fstep = steps[si];
            break;
        case 'z':
            safe_state();
            break;
        case 'r':
            revert_all();
            scan_channels();
            snprintf(g_msg, sizeof(g_msg), "re-read from sysfs");
            break;
        case 's':
            profile_save(g_profile);
            break;
        case 'l':
            panel_load(g_profile);
            break;
        case 'S':
            profile_save(g_cfg);
            break;
        case 'L':
            panel_load(g_cfg);
            break;
        case 'o':
            g_ota = !g_ota;
            snprintf(g_msg, sizeof(g_msg), "%s, loop budget %d",
                     g_ota ? "over the air" : "cabled", budget());
            break;
        case 'x':
            g_entry_on = 1;
            g_entry_tgt = 1;
            g_entry[0] = '\0';
            break;
        case 'R':
            pending_reset = 1;
            snprintf(g_msg, sizeof(g_msg),
                     "CONFIRM: reset to built-in %s defaults (saved file kept)"
                     " -- y apply / n cancel", g_bd->name);
            break;
        default:
            if (k < 128 && strchr("0123456789", k) && g_f[g_sel_f].type != T_ENUM) {
                g_entry_on = 1;
                g_entry[0] = (char)k;
                g_entry[1] = '\0';
            }
            break;
        }
        render();
    }
    tty_restore();
    return 0;
}

/* ----------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------- */
static void usage(void)
{
    fprintf(stderr,
        "rf_tune %s - RFNM daughterboard RF tuning (Lime / Granita / Yucca)\n"
        "usage: rf_tune [options]                 interactive panel\n"
        "       rf_tune -s                        print status and exit\n"
        "       rf_tune -c rx0.gain=26 tx0.power=30 ...   set values and apply\n"
        "       rf_tune -l profile.cfg            load a profile and apply\n"
        "       rf_tune -d [ch.field=val ...]     apply the saved next-run defaults\n"
        "       rf_tune --reset [--save]          apply built-in defaults [and save them]\n"
        "       rf_tune --save                    save current state as next-run defaults\n"
        "options:\n"
        "  -b lime|granita|yucca  board type (default: dmesg, then sysfs guess)\n"
        "  -S primary|secondary   daughterboard slot (default primary)\n"
        "  -p file                profile file for s/l keys (default rf_profile_<board>.cfg)\n"
        "  -G dB                  loop budget: rx gain + tx power - ext att (default 40)\n"
        "  -x dB                  external attenuation between TX and RX\n"
        "  -o                     over the air (budget +30 dB)\n"
        "  -y                     accept confirmations (CLI)\n"
        "  -f                     force: ignore refusals and invalid profile lines\n"
        "  -n                     panel: start from sysfs only (no prepared channels)\n"
        "  -m                     panel: no colours (also NO_COLOR=1)\n"
        "panel start: channels on in sysfs are live; channels off are prepared from the\n"
        "saved defaults (or built-in), held while off and applied when enabled.\n"
        "keys: up/dn field, tab . , column, lt/rt -/+ change, [ ] big step, f freq step,\n"
        "      digits type a value (freq in MHz), z safe state, s save, l load,\n"
        "      S save next-run defaults, L load them, R reset to built-in defaults,\n"
        "      o cabled / over the air, x external attenuation,\n"
        "      r re-read, y/n confirm, q quit\n"
        "      marks: * pending, ~ held (channel off), ! changed by the driver\n"
        "Fields: freq Hz, rx gain dB, tx power (board units), lpf_bw MHz.\n"
        "Saved defaults: $RF_TUNE_CFG_DIR/<board>.cfg (default /etc/rf_tune),\n"
        "previous file kept as .bak; -d uses built-in values if the file is missing.\n"
        "RF_TUNE_SYS=<dir> uses another sysfs root (test).\n", RF_TUNE_VERSION);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "save",  no_argument, NULL, 1001 },
        { "reset", no_argument, NULL, 1002 },
        { "help",  no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *forced = NULL, *load = NULL, *prof = NULL;
    const char *root = getenv("RF_TUNE_SYS");
    const char *cdir = getenv("RF_TUNE_CFG_DIR");
    int status = 0, set = 0, c, rc = 0, dflt = 0, save = 0, reset = 0;

    if (defaults_selfcheck()) {
        fprintf(stderr, "rf_tune: built-in default table broken, fix rf_tune.c\n");
        return 1;
    }
    while ((c = getopt_long(argc, argv, "hb:S:scl:p:G:x:oyfvdnm", lopts, NULL)) != -1) {
        switch (c) {
        case 'd': dflt = 1; break;
        case 1001: save = 1; break;
        case 1002: reset = 1; break;
        case 'b': forced = optarg; break;
        case 'S': g_slot = (!strcmp(optarg, "secondary") || !strcmp(optarg, "1")) ? 1 : 0; break;
        case 's': status = 1; break;
        case 'c': set = 1; break;
        case 'l': load = optarg; break;
        case 'p': prof = optarg; break;
        case 'G': g_budget = atoi(optarg); break;
        case 'x': g_ext_att = atoi(optarg); g_ext_cli = 1; break;
        case 'o': g_ota = 1; g_ota_cli = 1; break;
        case 'n': g_noprep = 1; break;
        case 'm': g_color = 0; break;
        case 'y': g_yes = 1; break;
        case 'f': g_force = 1; break;
        case 'v': printf("rf_tune %s\n", RF_TUNE_VERSION); return 0;
        default: usage(); return c == 'h' ? 0 : 1;
        }
    }
    if (getenv("NO_COLOR"))
        g_color = 0;
    snprintf(g_base, sizeof(g_base), "%s/%s", root ? root : "/sys/kernel",
             g_slot ? "rfnm_secondary" : "rfnm_primary");
    if (!path_exists(g_base)) {
        fprintf(stderr, "rf_tune: %s not found (la9310rfnm not loaded?)\n", g_base);
        return 1;
    }
    scan_channels();
    if (g_nch == 0) {
        fprintf(stderr, "rf_tune: no rx/tx channel under %s\n", g_base);
        return 1;
    }
    detect_board(forced);
    if (prof)
        snprintf(g_profile, sizeof(g_profile), "%s", prof);
    else
        snprintf(g_profile, sizeof(g_profile), "rf_profile_%s.cfg", g_bd->name);
    snprintf(g_cfg, sizeof(g_cfg), "%s/%s.cfg", cdir ? cdir : "/etc/rf_tune", g_bd->name);

    if (set || load || dflt || reset || save) {
        int i;
        if (reset && defaults_stage()) {
            fprintf(stderr, "rf_tune: %s\n", g_msg);
            return 1;
        }
        if (dflt && !reset) {
            if (path_exists(g_cfg)) {
                if (profile_load(g_cfg)) {
                    fprintf(stderr, "rf_tune: %s\n", g_msg);
                    return 1;
                }
            } else {
                fprintf(stderr, "rf_tune: %s missing, using built-in defaults\n", g_cfg);
                if (defaults_stage()) {
                    fprintf(stderr, "rf_tune: %s\n", g_msg);
                    return 1;
                }
            }
        }
        if (load && profile_load(load)) {
            fprintf(stderr, "rf_tune: %s\n", g_msg);
            return 1;
        }
        for (i = optind; i < argc; i++) {
            char kv[96], *eq;
            snprintf(kv, sizeof(kv), "%s", argv[i]);
            eq = strchr(kv, '=');
            if (!eq) {
                fprintf(stderr, "rf_tune: expected ch.field=value, got '%s'\n", argv[i]);
                return 1;
            }
            *eq = '\0';
            if (set_kv(kv, eq + 1) && !g_force)
                return 1;
        }
        rc = try_apply(0, 0);
        printf("rf_tune: %s\n", any_dirty() || rc ? g_msg : (g_msg[0] ? g_msg : "no change"));
        if (save && rc == 0) {
            if (profile_save(g_cfg))
                rc = 1;
            printf("rf_tune: %s\n", g_msg);
        }
        if (status)
            print_status();
        return rc;
    }
    if (status) {
        print_status();
        return 0;
    }
    return interactive();
}
