#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 NXP
####################################################################
# la9310-io.sh - LA9310 host memory access helpers, sourced by the
# host-utils scripts:
#   . "$(cd "$(dirname "$0")" && pwd)/la9310-io.sh"
#
# Everything goes through /dev/shiva0 first (la9310shiva with the modinfo
# mmap patch), which works with CONFIG_STRICT_DEVMEM=y and
# CONFIG_IO_STRICT_DEVMEM=y. /dev/mem (devmem, bin2mem) is only a fallback
# for drivers without the patch on kernels without STRICT_DEVMEM:
# STRICT_DEVMEM refuses RAM (scratch, IQFLOOD) and IO_STRICT_DEVMEM refuses
# the PCIe BARs claimed by la9310shiva.
#
#   la9310_wr <addr> <value>         write one 32-bit word
#   la9310_rd <addr>                 print one 32-bit word as 0xHHHHHHHH
#   la9310_load <file> <addr>        copy a file to memory
#   la9310_dump <file> <addr> <size> copy <size> bytes of memory to a file
####################################################################

LA9310_DEV=${LA9310_DEV:-/dev/shiva0}

# la9310_check <addr>: fail early, with the cause, when <addr> (a BAR0
# register) can be reached neither through /dev/shiva0 nor /dev/mem
la9310_check()
{
        local a
        a=$(printf "0x%X" $(($1)))
        memtool -d $LA9310_DEV -32 $a 1 > /dev/null 2>&1 && return 0
        memtool -32 $a 1 > /dev/null 2>&1 && return 0
        echo "*** ERROR: cannot map LA9310 registers ($a) through $LA9310_DEV or /dev/mem."
        echo "    la9310shiva needs the modinfo mmap patch: doc/patches/la9310-driver/0004"
        echo "    (RFNM SDK) or doc/patches/la93xx_host_sw/0001 (NXP BSP),"
        echo "    when the kernel has CONFIG_STRICT_DEVMEM / CONFIG_IO_STRICT_DEVMEM."
        return 1
}

la9310_wr()
{
        local a
        a=$(printf "0x%X" $(($1)))
        memtool -d $LA9310_DEV -32 $a=$2 > /dev/null 2>&1 && return 0
        devmem $a w $2
}

la9310_rd()
{
        local a v
        a=$(printf "0x%X" $(($1)))
        # memtool prints "0xADDR:  HHHHHHHH"
        v=$(memtool -d $LA9310_DEV -32 $a 1 2>/dev/null | awk 'NR==1{print $2}')
        if [ -n "$v" ]; then
                echo 0x$v
                return 0
        fi
        devmem $a
}

la9310_load()
{
        memtool -d $LA9310_DEV -a $2 -f "$1" > /dev/null 2>&1 && return 0
        bin2mem -f "$1" -a $2
}

la9310_dump()
{
        memtool -d $LA9310_DEV -a $2 -o "$1" $3 > /dev/null 2>&1 && return 0
        bin2mem -f "$1" -a $2 -r $3
}
