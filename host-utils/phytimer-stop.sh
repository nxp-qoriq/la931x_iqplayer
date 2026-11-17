#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 NXP
####################################################################
#set -x

la9310_ccsr_base=0x`la9310_modem_info | grep CCSR |cut -f 2 -d "x"|cut -f 1 -d " "`
# BAR0 access through /dev/shiva0 first (CONFIG_IO_STRICT_DEVMEM)
. "$(cd "$(dirname "$0")" && pwd)/la9310-io.sh"
phytimer_base=$[$la9310_ccsr_base + 0x1020000]

la9310_wr $[$phytimer_base + 0x0c] 0x00000005
la9310_wr $[$phytimer_base + 0x14] 0x00000005
la9310_wr $[$phytimer_base + 0x1c] 0x00000005
la9310_wr $[$phytimer_base + 0x24] 0x00000005
la9310_wr $[$phytimer_base + 0x5c] 0x00000005