#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 NXP
####################################################################
#set -x

la9310_ccsr_base=0x`dmesg | grep BAR:0|cut -f 2 -d "x"|cut -f 1 -d " "| head -1`
phytimer_base=$[$la9310_ccsr_base + 0x1020000]

# configure C21 to capture counter
devmem 0x190200ac w 0xa0

# read current time 
phytimer_counter=`devmem 0x190200b0`

# configure all channels to start in 1s
# 1s = 61440000 incr
phytimer_counter_1S=$[$phytimer_counter + 61440000]
devmem $[$phytimer_base + 0x10] w $phytimer_counter_1S
devmem $[$phytimer_base + 0x0c] w 0x00000002
devmem $[$phytimer_base + 0x18] w $phytimer_counter_1S
devmem $[$phytimer_base + 0x14] w 0x00000002
devmem $[$phytimer_base + 0x20] w $phytimer_counter_1S
devmem $[$phytimer_base + 0x1c] w 0x00000002
devmem $[$phytimer_base + 0x28] w $phytimer_counter_1S
devmem $[$phytimer_base + 0x24] w 0x00000002
devmem $[$phytimer_base + 0x60] w $phytimer_counter_1S
devmem $[$phytimer_base + 0x5c] w 0x00000002

