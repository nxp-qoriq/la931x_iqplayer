#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 NXP
####################################################################
# iq-proxy-cfg.sh - read the VSPA proxy configuration word (proxy + 0x1C)
#
# Sourced by the host scripts:  . "$SCRIPT_DIR/iq-proxy-cfg.sh"; proxy_cfg_read
# Layout (PROXY_CFG_* in iqplayer_cwproj/include/vspa_dmem_proxy.h):
#   [7:0]   number of RX channels      -> PCFG_NRX
#   [8]     RX IQ swap                  -> PCFG_IQSWAP
#   [11:9]  number of TX channels       -> PCFG_NTX
#   [31:16] RX channel map, 4 bits/slot -> PCFG_CHAN[0..3]
# PCFG_VALID=1 when the firmware publishes the map (older firmware leaves
# [31:16] at 0; then only PCFG_NRX is meaningful). Returns 1 if the proxy
# cannot be read or the RX count is out of range.
####################################################################

PROXY_RESERVED=${PROXY_RESERVED:-1024}

proxy_cfg_read()
{
        local ddrh maxsize addr raw v i
        ddrh=$(la9310_modem_info | grep FLOOD | cut -f 2 -d "|" | tr -d ' \t')
        maxsize=$(la9310_modem_info | grep FLOOD | cut -f 4 -d "|" | tr -d ' \t')
        [ -n "$ddrh" ] && [ -n "$maxsize" ] || return 1
        addr=$(printf "0x%X" $(( ddrh + maxsize - PROXY_RESERVED + 0x1C )))
        raw=$(memtool -d /dev/shiva0 -32 "$addr" 1 2>/dev/null | awk 'NR==1{print $2}')
        [ -n "$raw" ] || return 1
        v=$(( 0x$raw ))
        PCFG_RAW=$(printf "0x%08X" $v)
        PCFG_NRX=$(( v & 0xFF ))
        PCFG_IQSWAP=$(( (v >> 8) & 1 ))
        PCFG_NTX=$(( (v >> 9) & 7 ))
        PCFG_VALID=0
        [ $(( v >> 16 )) -ne 0 ] && PCFG_VALID=1
        for i in 0 1 2 3; do
                PCFG_CHAN[$i]=$(( (v >> (16 + 4 * i)) & 0xF ))
        done
        [ $PCFG_NRX -ge 1 ] && [ $PCFG_NRX -le 4 ] || return 1
        return 0
}
