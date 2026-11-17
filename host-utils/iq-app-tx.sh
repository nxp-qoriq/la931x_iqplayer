#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2026 NXP
####################################################################
# iq-app-tx.sh - stream IQ to LA9310 TX through iq_app
#
# File replay (background):
#   ./iq-app-tx.sh [-f] <input iq sample file> [FIFO Size]
# Interactive tone TX (foreground, keyboard control, no file):
#   ./iq-app-tx.sh [-f] -T [FIFO Size]
#
# -f : full speed, DAC at 122.88 MSPS (default half speed 61.44 MSPS).
#      Only used by iq_app for the tone frequency and the spectrum axis.
#     keys: up/dn freq, lt/rt amp, w waveform, g/G p/P i/I j/J TX QEC,
#           r reset QEC, s save QEC (<script dir>/qec/qec_coeff_tx_ant0.cfg), q quit
#
# Saved TX QEC (<script dir>/qec/qec_coeff_tx_ant0.cfg) is applied at start
# in both modes. Set QEC_DIR to use another directory, QEC_NOLOAD=1 to skip.
#
# Host flow control is enabled: iq_app feeds a DDR ring FIFO at the
# start of iqflood, and VSPA fetches from it as data is produced.
#
# A large FIFO (default 256 KB) is needed to absorb host scheduling
# jitter. With 32 KB (~130 us at 245 MB/s) the AXIQ TX FIFO underruns.
#
# The TX FIFO must fit in the TX slot so it does not overlap the RX
# FIFOs used by iq-app-rx.sh (slot = (iqflood - 1KB) / (N_TX + N_RX)).
####################################################################
#set -x

DEFAULT_FIFO_SIZE=262144
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEC_DIR=${QEC_DIR:-$SCRIPT_DIR/qec}
QEC_OPT="-Q $QEC_DIR"
[ -n "$QEC_NOLOAD" ] && [ "$QEC_NOLOAD" != "0" ] && QEC_OPT="$QEC_OPT -U"
PROXY_RESERVED=1024

print_usage()
{
echo "usage: ./iq-app-tx.sh [-f] <input iq sample file> [FIFO Size]"
echo "       ./iq-app-tx.sh [-f] -T [FIFO Size]"
echo "  -f        : full speed, DAC at 122.88 MSPS (default half speed 61.44 MSPS)"
echo "  -T        : interactive tone TX (foreground, keyboard control, no file)"
echo "  FIFO Size : TX DDR ring bytes, 4KB aligned (default $DEFAULT_FIFO_SIZE)"
echo "ex : ./iq-app-tx.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin"
echo "ex : ./iq-app-tx.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 524288"
echo "ex : ./iq-app-tx.sh -T"
echo "ex : ./iq-app-tx.sh -f -T"
}

CONV_MHZ=61.44   # DAC converter rate: 61.44 (half, default) or 122.88 (full, -f)
if [ "$1" = "-f" ]; then
        CONV_MHZ=122.88
        shift
fi

tone_mode=0
if [ "$1" = "-T" ]; then
        tone_mode=1
        shift
        fifo=${1:-$DEFAULT_FIFO_SIZE}
else
        if [ $# -lt 1 ];then
                echo Arguments wrong.
                print_usage
                exit 1
        fi
        if [ ! -f "$1" ];then
                echo "$1 file not found"
                exit 1
        fi
        iq_file=$1
        fifo=${2:-$DEFAULT_FIFO_SIZE}
fi

fifo=$(( fifo ))
fifo4k=$(( fifo / 4096 ))
fifo=$(( fifo4k * 4096 ))
if [ $fifo4k -eq 0 ];then
        echo "fifo size must be at least 4096 bytes"
        exit 1
fi

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)
ddrh=$(la9310_modem_info | grep FLOOD | cut -f 2 -d "|" | tr -d ' \t')
maxsize=$(la9310_modem_info | grep FLOOD | cut -f 4 -d "|" | tr -d ' \t')
if [ -z "$ddrh" ];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi

# Auto-detect N_RX from VSPA proxy (rx_num_chan at proxy + 0x1C) to size the TX slot
proxy_rx_num_chan_addr=$(printf "0x%X" $(( ddrh + maxsize - PROXY_RESERVED + 0x1C )))
raw_hex=$(memtool -d /dev/shiva0 -32 "$proxy_rx_num_chan_addr" 1 2>/dev/null | awk 'NR==1{print $2}')
cfg_word=$(( 0x${raw_hex:-0} ))
N_RX=$(( cfg_word & 0xFF ))
if [ "$N_RX" -lt 1 ] || [ "$N_RX" -gt 4 ]; then
        # fallback: assume 2 RX channels (conservative slot size)
        N_RX=2
fi
# TX guard: TX count in [11:9], valid when the channel map [31:16] is set
if [ $(( cfg_word >> 16 )) -ne 0 ] && [ $(( (cfg_word >> 9) & 7 )) -eq 0 ]; then
        echo "ERROR: the loaded VSPA image has no TX channel ($N_RX RX, 0 TX)."
        echo "       Load 1T1R or 1T2R: ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld"
        exit 1
fi
slot_size=$(( ((maxsize - PROXY_RESERVED) / (1 + N_RX) / 4096) * 4096 ))
if [ $fifo -gt $slot_size ];then
        echo "fifo $fifo too large for TX slot $slot_size bytes (N_RX=$N_RX)"
        exit 1
fi

echo "TX fifo: $fifo bytes ($fifo4k x 4KB), TX slot $slot_size bytes, N_RX=$N_RX"

# Stop any previous TX iq_app (file replay or tone); a leftover feeder
# would race with the new one on the same ring and cause underruns.
if pgrep -f 'iq_app -[tT]' >/dev/null; then
        echo "Stopping previous TX iq_app ..."
        pkill -TERM -f 'iq_app -[tT]'
        for i in 1 2 3 4 5 6 7 8 9 10; do
                pgrep -f 'iq_app -[tT]' >/dev/null || break
                sleep 0.2
        done
        pkill -KILL -f 'iq_app -[tT]' 2>/dev/null
fi

# Start VSPA TX streamer first (flow control enabled), so that iq_app
# init_tx() syncs its produced/consumed counters on the live VSPA
# la9310_fifo_enqueued_size instead of a stale 0.
# Abort if VSPA NACKs the TX start (e.g. 0T4R firmware has no TX).
if ! ./iq-start-txfifo.sh $fifo4k; then
        echo "TX start failed, iq_app not started"
        exit 1
fi

if [ $tone_mode -eq 1 ]; then
        # Foreground: keyboard control needs the terminal. Stop VSPA TX on exit.
        # iq_app pins its own threads: feeder on IQ_CPU_TX (3), UI on the
        # cores not used by TX/RX, so it can run in parallel with iq-app-rx.sh
        # Ctrl-C reaches iq_app as a key (ISIG off in raw mode); ignore
        # SIGINT here anyway so the script never exits before iq_app has
        # restored the terminal, and always stops VSPA TX. stty is the
        # backstop if iq_app is killed.
        tty_saved=$(stty -g 2>/dev/null)
        trap '' INT
        iq_app -T -Y $CONV_MHZ $QEC_OPT -F 0x00000000 $fifo
        trap - INT
        [ -n "$tty_saved" ] && stty "$tty_saved" 2>/dev/null
        printf '\033[0m\033[?25h\r'
        echo "Tone TX stopped. Stopping VSPA TX ..."
        vspa_mbox send 0 0 0x05000000 0x0
        vspa_mbox recv 0 0 || echo "warning: VSPA TX stop not acknowledged"
else
        # TX FIFO at offset 0 of iqflood region
        iq_app -t -Y $CONV_MHZ $QEC_OPT -f "$iq_file" -F 0x00000000 $fifo &
fi
