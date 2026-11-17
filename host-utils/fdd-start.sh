#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 NXP
####################################################################
#set -x

# enable ADC/DAC always on

#echo dpdk-dfe_app -c "fdd start"
#mount -t hugetlbfs none /dev/hugepages
#echo 24 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
#dpdk-dfe_app -c "fdd start"
# workaround.. until dpdk-dfe_app enables all channels

la9310_ccsr_base=0x`la9310_modem_info | grep CCSR |cut -f 2 -d "x"|cut -f 1 -d " "`
phytimer_base=$[$la9310_ccsr_base + 0x1020000]
# BAR0 access through /dev/shiva0 first (CONFIG_IO_STRICT_DEVMEM)
. "$(cd "$(dirname "$0")" && pwd)/la9310-io.sh"
la9310_check $phytimer_base || exit 1
for off in 0x0c 0x14 0x1c 0x24 0x5c; do
        la9310_wr $[$phytimer_base + $off] 0x0000000a
done
