#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 NXP
####################################################################
#set -x

la9310_ccsr_base=0x`la9310_modem_info | grep CCSR |cut -f 2 -d "x"|cut -f 1 -d " "`
if [ "$la9310_ccsr_base" = "0x" ] || [ $(($la9310_ccsr_base)) -eq 0 ]; then
	echo "*** ERROR: cannot read the LA9310 CCSR address, is la9310shiva loaded ?"
	exit 1
fi
# BAR0 access through /dev/shiva0 first (CONFIG_IO_STRICT_DEVMEM)
. "$(cd "$(dirname "$0")" && pwd)/la9310-io.sh"
phytimer_base=$[$la9310_ccsr_base + 0x1020000]

# configure C21 to capture counter
la9310_wr $[$phytimer_base + 0xac] 0xa0

# read current time 
phytimer_counter=`la9310_rd $[$phytimer_base + 0xb0]`

# configure all channels to start in 1s
# 1s = 61440000 incr
phytimer_counter_1S=$[$phytimer_counter + 61440000]
la9310_wr $[$phytimer_base + 0x10] $phytimer_counter_1S
la9310_wr $[$phytimer_base + 0x0c] 0x00000002
la9310_wr $[$phytimer_base + 0x18] $phytimer_counter_1S
la9310_wr $[$phytimer_base + 0x14] 0x00000002
la9310_wr $[$phytimer_base + 0x20] $phytimer_counter_1S
la9310_wr $[$phytimer_base + 0x1c] 0x00000002
la9310_wr $[$phytimer_base + 0x28] $phytimer_counter_1S
la9310_wr $[$phytimer_base + 0x24] 0x00000002
la9310_wr $[$phytimer_base + 0x60] $phytimer_counter_1S
la9310_wr $[$phytimer_base + 0x5c] 0x00000002

