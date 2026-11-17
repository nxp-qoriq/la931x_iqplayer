#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2026 NXP
####################################################################
# rx-chan.sh - select the physical RX channel(s) streamed by the VSPA
#
#   ./rx-chan.sh <chanID>             contiguous: slot i = chanID + i
#   ./rx-chan.sh <ch0> <ch1> [...]    explicit: slot i = ch<i>
#
# Must be issued before the RX start (iq-app-rx.sh / iq-capture*.sh).
# The VSPA NACKs a chanID > 3, a duplicated chanID, or a contiguous
# range that does not fit (e.g. 2R: chanID must be 0..2).
####################################################################
#set -x

print_usage()
{
echo "usage: ./rx-chan.sh -r                 read back current mapping (proxy)"
echo "       ./rx-chan.sh <chanID>"
echo "       ./rx-chan.sh <chanID slot0> <chanID slot1> [...]"
echo ""
echo " chanID  LA9310   LimeSDR-Micro          RFNM"
echo "   0     RO1      connector (no RFIC)    RBA_RX1  secondary sma_b"
echo "   1     RX1      LMS7002M RX2 (ch B)    RBA_RX2  primary   sma_b"
echo "   2     RO0      connector (no RFIC)    RBB_RX1  secondary sma_a"
echo "   3     RX0      LMS7002M RX1 (ch A)    RBB_RX2  primary   sma_a"
echo ""
echo "ex : ./rx-chan.sh 3        # 1R: LimeSDR-Micro RX1"
echo "ex : ./rx-chan.sh 3 1      # 2R: LimeSDR-Micro RX1 + RX2"
echo "ex : ./rx-chan.sh 3 1 2 0  # LimeSDR-Micro default (load-limesdr-micro.sh):"
echo "                           # RX0 RX1 RO0 RO1, an N-RX firmware uses the first N"
}

if [ "$1" = "-r" ]; then
        SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
        . "$SCRIPT_DIR/iq-proxy-cfg.sh"
        if ! proxy_cfg_read; then
                echo "ERROR: cannot read VSPA proxy (VSPA not loaded?)"
                exit 1
        fi
        board=$(cat /run/iqplayer.board 2>/dev/null)
        echo "proxy cfg $PCFG_RAW: RX $PCFG_NRX  IQ swap $PCFG_IQSWAP  TX $PCFG_NTX  board ${board:-unknown}"
        if [ $PCFG_VALID -eq 0 ]; then
                echo "(firmware does not publish the channel map)"
                exit 0
        fi
        la=(RO1 RX1 RO0 RX0)
        lime=("connector (no RFIC)" "LMS7002M RX2 (ch B)" "connector (no RFIC)" "LMS7002M RX1 (ch A)")
        rfnm=("RBA_RX1 sma_b" "RBA_RX2 sma_b" "RBB_RX1 sma_a" "RBB_RX2 sma_a")
        for (( s = 0; s < PCFG_NRX; s++ )); do
                c=${PCFG_CHAN[$s]}
                case "$board" in
                limesdr-micro) b=${lime[$c]} ;;
                rfnm) b=${rfnm[$c]} ;;
                *) b="" ;;
                esac
                echo " slot $s -> chanID $c  ${la[$c]}  $b"
        done
        exit 0
fi

if [ $# -lt 1 ] || [ $# -gt 4 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

for c in "$@"; do
        if ! [[ "$c" =~ ^[0-3]$ ]]; then
                echo "invalid chanID '$c' (0-3)"
                print_usage
                exit 1
        fi
done

cmd=0x0d000000
if [ $# -eq 1 ];then
        # contiguous mode (legacy)
        lsb=$(printf "0x%X" $1)
else
        # explicit list: bit 16 set, 4 bits per slot in bits 15..0
        # (needs the matching firmware: older images used bit 8, which
        # overlaps slot 2, so a 4-slot list was NACKed)
        lsb=0x10000
        i=0
        for c in "$@"; do
                lsb=$(( lsb | (c << (4 * i)) ))
                i=$(( i + 1 ))
        done
        lsb=$(printf "0x%X" $lsb)
fi

vspa_mbox send 0 0 $cmd $lsb
if ! vspa_mbox recv 0 0; then
        echo "ERROR: VSPA NACKed RX channel select ($*): chanID out of range or duplicated for this firmware"
        exit 1
fi
