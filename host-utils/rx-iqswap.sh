#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 NXP
####################################################################
# rx-iqswap.sh - set the RX AXIQ I/Q swap of the VSPA iqplayer
#
#   ./rx-iqswap.sh 0    no swap (LimeSDR-Micro)
#   ./rx-iqswap.sh 1    swap I/Q (RFNM, VSPA boot default)
#
# Applied on the next RX start (iq-app-rx.sh / iq-capture*.sh) or
# rx-chan.sh. Called by load-limesdr-micro.sh and load-rfnm.sh.
####################################################################

if [ $# -ne 1 ] || { [ "$1" != "0" ] && [ "$1" != "1" ]; }; then
    echo "usage: ./rx-iqswap.sh <0|1>   0 = no swap (LimeSDR-Micro), 1 = swap (RFNM)"
    exit 1
fi

vspa_mbox send 0 0 0x12000000 $1
if ! vspa_mbox recv 0 0; then
    echo "***ERROR: VSPA did not acknowledge RX IQ swap $1 (old VSPA image ?)"
    exit 1
fi
echo "RX IQ swap set to $1"
