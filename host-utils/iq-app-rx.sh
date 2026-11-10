#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2026 NXP
####################################################################
# iq-app-rx.sh - start iq_app on LA9310 (spectrum or capture)
#
# N_RX is auto-detected from the VSPA proxy (rx_num_chan field).
# Use -n to override if auto-detect fails.
#
# Usage:
#   ./iq-app-rx.sh -X [-f] [-n N_RX] [-C QEC_CH] [-W base [size]] [-Q dir] [-q] [fifo_size]
#       Spectrum mode (foreground, live FFT). Press q to quit.
#       RX QEC keys act on the current QEC channel (one QEC set per channel):
#       c next channel, g/G gain +-0.1 dB, p/P phase +-0.5 deg,
#       i/I j/J DC I/Q +-0.01, r reset gain/phase,
#       s save <qecdir>/qec_coeff_rx_ant<ch>.cfg,
#       w write an IQ snapshot of all channels to
#         <base>_<rate>Msps_ch<N>_<YYYYmmdd_HHMMSS>.bin
#
#   Saved RX QEC (<qecdir>/qec_coeff_rx_ant<ch>.cfg) is applied at start in
#   both modes. The files hold coefficient values, not firmware tap indexes,
#   so the same config is used with every firmware. qecdir defaults to
#   <script dir>/qec, independent of the current directory.
#
#   ./iq-app-rx.sh -D <outfile> <filesize> [-f] [-n N_RX] [fifo_size]
#       Capture mode: record <filesize> bytes per channel.
#       <outfile> is a base name: every channel N writes <base>_chN.<ext>
#       (e.g. ./rx.bin -> ./rx_ch0.bin, ./rx_ch1.bin ...).
#       Runs in background; prints PID.
#
# Options:
#   -X             spectrum mode (foreground)
#   -D file size   dump/capture mode (background)
#   -f             full speed: ADC at 122.88 MSPS (default half speed 61.44 MSPS)
#   -n N_RX        number of RX channels 1-4 (default: auto-detect from proxy)
#   -K top         -X only: chart top dBFS (default 0)
#   -B bottom      -X only: chart bottom dBFS (default -100)
#   -L rows        -X only: chart rows per channel (default 40 for 1T1R, 16 for 1T2R, 10 for 0T4R)
#   -M navg        -X only: FFTs averaged per refresh (default 8)
#   -N fft         -X only: FFT size (default 8192, as compute_tone_params.py)
#   -H             -X only: Hann window + power average (default rect, mean |X|)
#   -W base [size] -X only: snapshot base name and per-channel size for key w
#                  (default <script dir>/capture/rx, 1ms; size in bytes or <N>ms)
#   -Q dir         QEC config directory (default <script dir>/qec)
#   -q             do not apply saved QEC at start
#   fifo_size      per-channel DDR ring FIFO bytes, hex ok (default 0x80000)
#
# Examples:
#   ./iq-app-rx.sh -X                          # spectrum, auto-detect N_RX
#   ./iq-app-rx.sh -X -n 2 0x40000             # spectrum, 2 ch, small fifo
#   ./iq-app-rx.sh -D ./rx.bin 4915200         # capture, auto-detect N_RX
#   ./iq-app-rx.sh -D ./rx.bin 4915200 -n 2    # capture, 2 ch
#   ./iq-app-rx.sh -X -f                       # spectrum, ADC at full speed 122.88 MSPS
#   ./iq-app-rx.sh -X -W /tmp/rx 10ms          # key w writes 10 ms per channel
#
####################################################################
#set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROXY_RESERVED=1024
DEFAULT_FIFO_SIZE=524288
QEC_DIR="$SCRIPT_DIR/qec"
SNAP_BASE="$SCRIPT_DIR/capture/rx"
SNAP_SIZE="1ms"
QEC_NOLOAD=""

usage() {
    echo "Usage:"
    echo "  $0 -X [-f] [-n N_RX] [-C QEC_CH] [-W base [size]] [-Q dir] [-q] [fifo_size]"
    echo "  $0 -D <outfile> <filesize> [-f] [-n N_RX] [-Q dir] [-q] [fifo_size]"
    echo ""
    echo "  -X             spectrum mode (foreground, live FFT)"
    echo "  -D file size   dump/capture mode (background); file is a base name:"
    echo "                 rx.bin -> rx_ch0.bin, rx_ch1.bin ..."
    echo "  -f             full speed: ADC at 122.88 MSPS (default half speed 61.44 MSPS)"
    echo "  -n N_RX        RX channel count 1-4 (default: auto-detect from proxy)"
    echo "  -C QEC_CH      -X only: RX QEC channel selected at start (default 0, key c cycles)"
    echo "  -K top         -X only: chart top dBFS (default 0)"
    echo "  -B bottom      -X only: chart bottom dBFS (default -100)"
    echo "  -L rows        -X only: chart rows per channel (default 40 for 1T1R, 16 for 1T2R, 10 for 0T4R)"
    echo "  -M navg        -X only: FFTs averaged per refresh (default 8)"
    echo "  -N fft         -X only: FFT size (default 8192, as compute_tone_params.py)"
    echo "  -H             -X only: Hann window + power average (default rect, mean |X|)"
    echo "  -W base [size] -X only: key w snapshot base and per-channel size"
    echo "                 (default $SCRIPT_DIR/capture/rx, 1ms; bytes or <N>ms)"
    echo "                 files: <base>_<rate>Msps_ch<N>_<YYYYmmdd_HHMMSS>.bin"
    echo "  -Q dir         QEC config directory (default $SCRIPT_DIR/qec)"
    echo "  -q             do not apply saved QEC at start"
    echo "  fifo_size      per-channel DDR FIFO bytes, hex ok (default $(printf '0x%X' $DEFAULT_FIFO_SIZE))"
    echo "ex:"
    echo "  ./iq-app-rx.sh -X "
    echo "  ./iq-app-rx.sh -D rx.bin 0x100000"
    exit 1
}

# --- Require at least one argument ---
if [ $# -eq 0 ]; then
    usage
fi

# --- Parse mode and options ---
MODE=""
OUT_FILE=""
FILE_SIZE=0
N_RX_OVERRIDE=""
QEC_CH=0
DB_TOP=0
NAVG=8
FFT_N=8192
HANN=""
DB_BOT=-100
ROWS=""
CONV_MHZ=61.44   # ADC converter rate: 61.44 (half, default) or 122.88 (full, -f)
FIFO_SIZE_ARG=$DEFAULT_FIFO_SIZE

while [ $# -gt 0 ]; do
    case "$1" in
        -X)
            MODE="spectrum"
            shift
            ;;
        -D)
            MODE="capture"
            if [ $# -lt 3 ]; then
                echo "***ERROR: -D requires <outfile> and <filesize>"
                usage
            fi
            OUT_FILE=$2
            FILE_SIZE=$3
            shift 3
            ;;
        -C)
            if [ $# -lt 2 ]; then
                echo "***ERROR: -C requires a value"
                usage
            fi
            QEC_CH=$2
            shift 2
            ;;
        -H)
            HANN="-H"
            shift
            ;;
        -W)
            if [ $# -lt 2 ]; then
                echo "***ERROR: -W requires a base name"
                usage
            fi
            SNAP_BASE=$2
            shift 2
            # optional size: number (bytes, hex ok) or <N>ms
            if [ $# -gt 0 ] && [[ "$1" =~ ^(0x[0-9a-fA-F]+|[0-9]+(\.[0-9]+)?ms|[0-9]+)$ ]]; then
                # a bare number right after -W is the size, not fifo_size
                SNAP_SIZE=$1
                shift
            fi
            ;;
        -Q)
            if [ $# -lt 2 ]; then
                echo "***ERROR: -Q requires a directory"
                usage
            fi
            QEC_DIR=$2
            shift 2
            ;;
        -q)
            QEC_NOLOAD="-U"
            shift
            ;;
        -K|-B|-M|-N|-L)
            if [ $# -lt 2 ]; then
                echo "***ERROR: $1 requires a value"
                usage
            fi
            case "$1" in
                -K) DB_TOP=$2 ;;
                -B) DB_BOT=$2 ;;
                -M) NAVG=$2 ;;
                -N) FFT_N=$2 ;;
                -L) ROWS=$2 ;;
            esac
            shift 2
            ;;
        -f)
            CONV_MHZ=122.88
            shift
            ;;
        -n)
            if [ $# -lt 2 ]; then
                echo "***ERROR: -n requires a value"
                usage
            fi
            N_RX_OVERRIDE=$2
            shift 2
            ;;
        -*)
            echo "***ERROR: unknown option '$1'"
            usage
            ;;
        *)
            # positional: fifo_size
            FIFO_SIZE_ARG=$1
            shift
            ;;
    esac
done

if [ -z "$MODE" ]; then
    echo "***ERROR: must specify -X (spectrum) or -D (dump)"
    usage
fi

# --- FIFO size: per-channel, aligned down to 4 KB ---
fifo_bytes=$(( FIFO_SIZE_ARG ))
fifo_align=$(( fifo_bytes / 4096 ))
fifo_bytes=$(( fifo_align * 4096 ))
if [ $fifo_align -eq 0 ]; then
    echo "***ERROR: fifo_size $FIFO_SIZE_ARG is smaller than one 4 KB page"
    exit 1
fi

# --- Stop any previous RX iq_app (SIGINT so a capture is saved) ---
if pgrep -f 'iq_app -[rX]' >/dev/null; then
    echo "Stopping previous RX iq_app (SIGINT) ..."
    pkill -INT -f 'iq_app -[rX]'
    for i in $(seq 1 25); do
        pgrep -f 'iq_app -[rX]' >/dev/null || break
        sleep 0.2
    done
    if pgrep -f 'iq_app -[rX]' >/dev/null; then
        echo "***ERROR: previous RX iq_app still running (pid $(pgrep -f 'iq_app -[rX]' | tr '\n' ' '))"
        exit 1
    fi
fi

# --- Retrieve iqflood region from driver ---
ddrh=$(la9310_modem_info | grep FLOOD | cut -f 2 -d "|" | tr -d ' \t')
ddrep=$(la9310_modem_info | grep FLOOD | cut -f 3 -d "|" | tr -d ' \t')
maxsize=$(la9310_modem_info | grep FLOOD | cut -f 4 -d "|" | tr -d ' \t')

if [ -z "$ddrh" ] || [ "$ddrh" -eq 0 ] 2>/dev/null; then
    echo "***ERROR: cannot retrieve IQFLOOD region - is la9310shiva loaded?"
    exit 1
fi

# --- Auto-detect N_RX from VSPA proxy (unless -n given) ---
# tx_vspa_proxy sits at the last 1024 bytes of the iqflood region.
# rx_num_chan is at byte offset +0x1C within the proxy struct.
proxy_base=$(( ddrh + maxsize - PROXY_RESERVED ))
proxy_rx_num_chan_addr=$(printf "0x%X" $(( proxy_base + 0x1C )))

if [ -n "$N_RX_OVERRIDE" ]; then
    N_RX=$N_RX_OVERRIDE
    echo "N_RX=$N_RX  (manual override via -n)"
else
    # memtool -32 addr 1 prints one line: "0xADDR:  HHHHHHHH"
    raw_hex=$(memtool -d /dev/shiva0 -32 "$proxy_rx_num_chan_addr" 1 2>/dev/null \
              | awk 'NR==1{print $2}')
    N_RX=$(printf "%d" "0x${raw_hex}" 2>/dev/null)

    if [ -z "$N_RX" ] || [ "$N_RX" -lt 1 ] || [ "$N_RX" -gt 4 ] 2>/dev/null; then
        echo "***ERROR: could not read rx_num_chan from proxy at $proxy_rx_num_chan_addr (got '$raw_hex')"
        echo "  Use -n <1-4> to specify N_RX manually."
        exit 1
    fi
    echo "N_RX=$N_RX  (auto-detected from proxy at $proxy_rx_num_chan_addr)"
fi

# Derive N_TX from N_RX (1T1R: N_RX=1, 1T2R: N_RX=2, 0T4R: N_RX=4)
if [ "$N_RX" -eq 4 ]; then
    N_TX=0
else
    N_TX=1
fi
total_slots=$(( N_TX + N_RX ))

# Spectrum chart rows per channel: 40 for 1T1R, 16 for 1T2R, 10 for 0T4R (-L overrides)
if [ -z "$ROWS" ]; then
    if [ "$N_RX" -gt 2 ]; then ROWS=10; elif [ "$N_RX" -gt 1 ]; then ROWS=16; else ROWS=40; fi
fi

# --- Compute slot layout ---
usable=$(( maxsize - PROXY_RESERVED ))
raw_slot=$(( usable / total_slots ))
slot_size=$(( (raw_slot / 4096) * 4096 ))

if [ $slot_size -eq 0 ]; then
    echo "***ERROR: iqflood region ($maxsize bytes) too small for $total_slots slots"
    exit 1
fi

# Validate fifo_size fits within RX slot allocation
total_rx_bytes=$(( N_RX * fifo_bytes ))
rx_slot_total=$(( N_RX * slot_size ))
if [ $total_rx_bytes -gt $rx_slot_total ]; then
    echo "***ERROR: N_RX*fifo_size $(printf '0x%X' $total_rx_bytes) exceeds total RX slot $(printf '0x%X' $rx_slot_total)"
    exit 1
fi

# --- ch0 FIFO offset within iqflood EP address space ---
ch0_fifo_offset=$(( N_TX * slot_size ))
ch0_fifo_offset_hex=$(printf "0x%X" $ch0_fifo_offset)

# --- Total mbox parameters ---
total_num_4KB=$(( N_RX * fifo_align ))
rx_cmd=$(printf "0x%X" $(( 0x06900000 + total_num_4KB )))
rx_base_phys=$(( ddrep + N_TX * slot_size ))
rx_buf_phys=$(printf "0x%X" "$rx_base_phys")

echo "N_TX=$N_TX"
echo "iqflood     : host=$ddrh  ep=$ddrep  size=$(printf '0x%X' $maxsize)"
echo "slot_size   : $(printf '0x%X' $slot_size)"
echo "rx_base     : $rx_buf_phys"
echo "fifo/chan    : $(printf '0x%X' $fifo_bytes)  total: $(printf '0x%X' $total_rx_bytes)"
echo "rx_cmd      : MSB=$rx_cmd  LSB=$rx_buf_phys  (num_4KB=$total_num_4KB)"
echo ""

# --- Clear RX proxy produced/consumed before mbox send ---
proxy_rx_base=$(( ddrh + maxsize - PROXY_RESERVED + 0x40 ))
echo "Clearing RX proxy produced/consumed for $N_RX channel(s)..."
for (( ch=0; ch<N_RX; ch++ )); do
    ch_proxy=$(printf "0x%X" $(( proxy_rx_base + ch * 0x10 )))
    memtool -d /dev/shiva0 -a "$ch_proxy" -z 8
    echo "  ch$ch proxy $ch_proxy: cleared"
done
echo ""

# --- Kick VSPA to start RX ---
echo "vspa_mbox send 0 0 $rx_cmd $rx_buf_phys"
vspa_mbox send 0 0 "$rx_cmd" "$rx_buf_phys"
vspa_mbox recv 0 0
echo ""

# --- Launch iq_app ---
# iq_app pins its RX drain thread to IQ_CPU_RX (default 2) and the UI to
# the cores not used by TX/RX, so it can run in parallel with iq-app-tx.sh
if [ "$MODE" = "spectrum" ]; then
    echo "Starting spectrum mode (foreground): iq_app -X -n $N_RX -c 0 -F $ch0_fifo_offset_hex $fifo_bytes -N $FFT_N -Y $CONV_MHZ -K $DB_TOP -B $DB_BOT -M $NAVG -L $ROWS $HANN -Q $QEC_DIR $QEC_NOLOAD -w $SNAP_BASE -d $SNAP_SIZE"
    iq_app -X -n "$N_RX" -c 0 -C "$QEC_CH" -Y "$CONV_MHZ" \
        -K "$DB_TOP" -B "$DB_BOT" -M "$NAVG" -L "$ROWS" \
        -F "$ch0_fifo_offset_hex" "$fifo_bytes" \
        -N "$FFT_N" $HANN -Q "$QEC_DIR" $QEC_NOLOAD \
        -w "$SNAP_BASE" -d "$SNAP_SIZE"
else
    echo "Starting capture mode (background): iq_app -r -n $N_RX -c 0 -f $OUT_FILE -s $FILE_SIZE -F $ch0_fifo_offset_hex $fifo_bytes -Y $CONV_MHZ -Q $QEC_DIR $QEC_NOLOAD"
    iq_app -r -n "$N_RX" -c 0 -Y "$CONV_MHZ" -Q "$QEC_DIR" $QEC_NOLOAD \
        -f "$OUT_FILE" -s "$FILE_SIZE" \
        -F "$ch0_fifo_offset_hex" "$fifo_bytes" &
    IQ_APP_PID=$!
    echo "  iq_app PID: $IQ_APP_PID"
    echo ""
    echo "RX capture running (${N_RX} channel(s), PID: $IQ_APP_PID)."
    echo "Stop with: kill -INT $IQ_APP_PID"
    echo "Or stop VSPA stream: vspa_mbox send 0 0 0x06000000 0x0 && vspa_mbox recv 0 0"
fi
