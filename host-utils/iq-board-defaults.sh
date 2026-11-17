#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 NXP
####################################################################
# iq-board-defaults.sh - apply the board RX defaults to the running VSPA
#
#   ./iq-board-defaults.sh [limesdr-micro|rfnm]
#
# Without argument the board is read from $IQP_BOARD_FILE, written by
# load-limesdr-micro.sh / load-rfnm.sh. Called by the load scripts and by
# iq-reload-vspa.sh (a new VSPA image resets IQ swap and channel order).
#
#   limesdr-micro : no I/Q swap, RX order RX0 RX1 RO0 RO1 (rx-chan.sh 3 1 2 0)
#   rfnm          : I/Q swap, firmware default RX order
# Only the first N entries of the RX order are used by an N-RX firmware
# (1T1R: RX0, 1T2R: RX0 RX1, 0T4R: all four).
####################################################################
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IQP_BOARD_FILE=/run/iqplayer.board

board=$1
if [ -z "$board" ]; then
        [ -f $IQP_BOARD_FILE ] || { echo "no board recorded in $IQP_BOARD_FILE, defaults not applied"; exit 0; }
        board=$(cat $IQP_BOARD_FILE)
fi

case $board in
limesdr-micro)
        $SCRIPT_DIR/rx-iqswap.sh 0 || exit 1
        $SCRIPT_DIR/rx-chan.sh 3 1 2 0 || exit 1
        ;;
rfnm)
        $SCRIPT_DIR/rx-iqswap.sh 1 || exit 1
        ;;
*)
        echo "unknown board '$board' (limesdr-micro|rfnm)"
        exit 1
        ;;
esac
echo "$board" > $IQP_BOARD_FILE
echo "$board RX defaults applied"
