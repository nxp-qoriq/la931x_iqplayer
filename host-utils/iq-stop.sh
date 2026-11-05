#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2026 NXP
####################################################################
# iq-stop.sh - stop host iq_app (RX and TX) then VSPA RX/TX streams
####################################################################
#set -x

# RX: SIGINT so a running capture flushes its files
pkill -INT -f 'iq_app -[rX]' 2>/dev/null
# TX: SIGTERM (file replay / tone)
pkill -TERM -f 'iq_app -[tT]' 2>/dev/null
for i in $(seq 1 25); do
        pgrep -f 'iq_app -[rXtT]' >/dev/null || break
        sleep 0.2
done
pgrep -f 'iq_app -[rXtT]' >/dev/null && echo "warning: iq_app still running: $(pgrep -f 'iq_app -[rXtT]' | tr '\n' ' ')"

# VSPA RX stop
vspa_mbox send 0 0 0x06000000 0
vspa_mbox recv 0 0

# VSPA TX stop
vspa_mbox send 0 0 0x05000000 0
vspa_mbox recv 0 0
