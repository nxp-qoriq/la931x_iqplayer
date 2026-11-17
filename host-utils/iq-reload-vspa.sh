#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2026 NXP
####################################################################
# Reload the VSPA with another IQ Player image at runtime, without reloading
# la9310shiva.ko:
#   1. VSPA software reset (MBOX_OPC_DONE_SWRESET, same as iq-swreset.sh)
#   2. copy the ELD to the scratch buffer, DMA .vpram, .vdram and .idram
#      into the VSPA through the VSPA debug DMA registers
#   3. copy the new image to /lib/firmware/apm.eld (iq_trace, iq_mon and the
#      next load-*.sh use it); a regular file, not a symlink
#   4. boot handshake (0x70000000, normally done by la9310shiva at load),
#      then send the IQFLOOD proxy offset again (the new .vdram resets it)
#      and check that the VSPA echoes it
#   5. board RX defaults again (iq-board-defaults.sh: IQ swap, RX order)
####################################################################

APM_ELD=/lib/firmware/apm.eld

print_usage()
{
echo "usage: ./iq-reload-vspa.sh [-v] [-n] <eld file>"
echo "  -v  trace commands (set -x)"
echo "  -n  do not update $APM_ELD"
echo "ex : ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld"
}

update_link=1
verbose=
while getopts "vnh" opt; do
        case $opt in
        v) verbose=1; set -x ;;
        n) update_link=0 ;;
        *) print_usage; exit 1 ;;
        esac
done
shift $((OPTIND - 1))

if [ $# -ne 1 ]; then
        echo Arguments wrong.
        print_usage
        exit 1
fi
eld="$(readlink -f "$1")"
[ -f "$eld" ] || { echo "*** ERROR: $1 not found"; exit 1; }

# LA9310 CCSR (BAR0) host physical address, from the driver. It depends on
# the host PCIe memory map (0x18000000 on i.MX8MP, 0x910000000 on i.MX95):
# never hardcode it, a write to a wrong host address can hang the host bus.
ccsr=0x$(la9310_modem_info | grep "CCSR phys" | cut -f 2 -d "x" | cut -f 1 -d " ")
if [ "$ccsr" = "0x" ] || [ $((ccsr)) -eq 0 ]; then
        echo "*** ERROR: cannot read the LA9310 CCSR address, is la9310shiva loaded ?"
        exit 1
fi
vspa=$((ccsr + 0x1000000))
# VSPA debug DMA registers (VSPA IP registers 0xB0..0xC8)
DMA_DST=$(printf "0x%X" $((vspa + 0xB0)))    # VSPA destination address
DMA_SRC=$(printf "0x%X" $((vspa + 0xB4)))    # source address, LA9310 (EP) view
DMA_SIZE=$(printf "0x%X" $((vspa + 0xB8)))   # byte count
DMA_CTRL=$(printf "0x%X" $((vspa + 0xBC)))   # 0x200 = program memory, 0x000 = data memory
DMA_STAT=$(printf "0x%X" $((vspa + 0xC8)))   # bit 0 = transfer complete, write 1 to clear
# scratch buffer as seen by the LA9310 (outbound window of la9310shiva):
# modem address of the first scratch region (VSPA OVERLAY)
ep_scratch=$(la9310_modem_info | grep "VSPA OVERLAY" | cut -f 3 -d "|" | tr -d ' \t')
if [ -z "$ep_scratch" ]; then
        echo "*** ERROR: cannot read the scratch buffer modem address"
        exit 1
fi
# memtool through /dev/shiva0 (works with CONFIG_STRICT_DEVMEM); falls back
# to /dev/mem with an unpatched la9310shiva
memtool_regs()
{
        memtool -d /dev/shiva0 "$@" 2>/dev/null || memtool "$@"
}
# .idram is loaded at the start of the IPPU data memory bank
IDRAM_DST=0x4000

mbox_send()
{
        # $1 = MSB, $2 = LSB; prints the VSPA reply, returns 1 if no reply
        local out
        vspa_mbox send 0 0 "$1" "$2" > /dev/null
        out="$(vspa_mbox recv 0 0)"
        echo "$out"
        echo "$out" | grep -q "Received from VSPA"
}

dma_stat()
{
        memtool_regs -32 $DMA_STAT 1 | grep -i "${DMA_STAT}:" | cut -f 2 -d ":" | sed 's/ //g'
}

dma_load()
{
        # $1 = section, $2 = ctrl, $3 = optional forced destination
        local section=$1 ctrl=$2 size dst offset src stat counter
        read -r size dst offset < <(objdump -h -j "$section" "$eld" 2>/dev/null |
                awk -v s="$section" '$2 == s { print $3, $4, $6 }')
        if [ -z "$size" ]; then
                echo "*** ERROR: section $section not found in $eld"
                exit 1
        fi
        [ -n "$3" ] && dst=$3 || dst=0x$dst
        src=$(printf "0x%X" $((ep_scratch + 0x$offset)))
        echo "  $section: dst $dst size 0x$size src $src"

        memtool_regs -32 $DMA_STAT=1 > /dev/null   # clear a stale completion
        memtool_regs -32 $DMA_DST=$dst > /dev/null
        memtool_regs -32 $DMA_SRC=$src > /dev/null
        memtool_regs -32 $DMA_SIZE=0x$size > /dev/null
        memtool_regs -32 $DMA_CTRL=$ctrl > /dev/null

        counter=1000
        stat=0
        while [ $((stat & 1)) -eq 0 ]; do
                stat=0x$(dma_stat)
                counter=$((counter - 1))
                if [ $counter -eq 0 ]; then
                        echo "*** ERROR: $section DMA timeout (status $stat)"
                        memtool_regs -32 $DMA_DST 8
                        exit 1
                fi
        done
        memtool_regs -32 $DMA_STAT=1 > /dev/null
}

. "$(cd "$(dirname "$0")" && pwd)/la9310-io.sh"
# fail before touching the VSPA if its registers cannot be mapped
la9310_check $DMA_STAT || exit 1

# 1. VSPA software reset: VSPA goes to done state before its memory is rewritten
echo "VSPA software reset"
# no reply is expected if the VSPA is already halted (e.g. previous reload failed)
mbox_send 0x10000000 0 || echo "*** WARNING: no reply to VSPA software reset, continuing"

# 2. ELD to the scratch buffer, then DMA each section into the VSPA
scratch_addr=0x$(la9310_modem_info | grep "Scratch buf phys" | cut -f 2 -d "x" | cut -f 1 -d " ")
if [ "$scratch_addr" = "0x" ]; then
        echo "*** ERROR: cannot read the scratch buffer address, is la9310shiva loaded ?"
        exit 1
fi
echo "load $eld at $scratch_addr"
# memtool -f through /dev/shiva0 (CONFIG_STRICT_DEVMEM refuses /dev/mem for
# the scratch RAM); bin2mem (/dev/mem) for drivers without the mmap patch
la9310_load "$eld" $scratch_addr > /dev/null ||
        { echo "*** ERROR: cannot write $eld to the scratch buffer (memtool /dev/shiva0 and bin2mem failed)"; exit 1; }
dma_load .vpram 0x200
dma_load .vdram 0x000
dma_load .idram 0x000 $IDRAM_DST

# 3. host tools follow the running image
# rm first: if apm.eld is still a symlink, cp would overwrite its target.
# $eld is already resolved, so reloading apm.eld itself (regular file) is a no-op.
if [ $update_link -eq 1 ] && [ "$eld" != "$APM_ELD" ]; then
        rm -f $APM_ELD
        cp "$eld" $APM_ELD && sync
        echo "$APM_ELD = copy of $eld"
fi

# 4. boot handshake, then IQFLOOD proxy offset (same value as load-*.sh)
maxsize=$(la9310_modem_info | grep FLOOD | cut -f 4 -d "|" | sed 's/\t//g; s/ //g')
if [ -z "$maxsize" ]; then
        echo "*** ERROR: cannot read the IQFLOOD region"
        exit 1
fi

# 4a. boot handshake normally done by la9310shiva: the new image posts
#     0xF1000000, waits for 0x70000000/0, answers 0xF0700000
echo "VSPA boot handshake"
vspa_mbox send 0 0 0x70000000 0 > /dev/null
booted=0
for i in 1 2; do
        out="$(vspa_mbox recv 0 0)"
        [ -n "$verbose" ] && echo "$out"
        if echo "$out" | grep -qi "f0700000"; then
                booted=1
                break
        fi
done
if [ $booted -eq 0 ]; then
        echo "*** ERROR: new VSPA image did not boot (no 0xF0700000 handshake reply)"
        exit 1
fi

# 4b. proxy offset, the VSPA echoes it in the reply MSB
proxy_offset=$(printf "0x%X" $((maxsize - 1024)))
echo "proxy offset $proxy_offset"
out="$(mbox_send 0x11000000 $proxy_offset)" || { echo "*** ERROR: no reply from the new VSPA image"; exit 1; }
ok=0
for v in $(echo "$out" | grep -oiE "0x[0-9a-f]+"); do
        [ $((v)) -eq $((proxy_offset)) ] && ok=1
done
if [ $ok -eq 0 ]; then
        echo "$out"
        echo "*** ERROR: proxy offset not acknowledged"
        exit 1
fi
echo "VSPA running $(basename "$eld")"

# 5. the new image boots with the firmware RX defaults (IQ swap 1, RX order
#    0 1 2 3): apply the board defaults recorded by load-*.sh again
"$(cd "$(dirname "$0")" && pwd)"/iq-board-defaults.sh || exit 1
