#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
set -x

print_usage()
{
echo "usage: ./iq-reload-vspa.sh <eld file>"
echo "ex : ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld"
}

# check parameters
if [ $# -lt 1 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

(ls $1 >> /dev/null 2>&1)||echo $1 file not found


# NXP-LA9310-Driver 0000:01:00.0: subdrv DMA region:[4] offset 0
# NXP-LA9310-Driver 0000:01:00.0: Host virtual 0xffff800030000000, EP Phys 0xa0000000, size 2097152

ep_scratch=0xa0000000
scratch_addr=0x`hexdump -C /sys/firmware/devicetree/base/reserved-memory/la93@*/reg |cut -f 7-10 -d " " |sed 's/ //g'|cut -f 1 -d " " |head -1`

# load eld at beginning of scratch region
echo bin2mem -f $1 -a $scratch_addr -c 128
bin2mem -f $1 -a $scratch_addr -c 128

# load vpram
section=.vpram
ctrl=0x200
offset=0x`objdump  -h -j $section $1 | grep $section|cut -f 18 -d " "`
dst=0x`objdump  -h -j $section $1 | grep $section|cut -f 14 -d " "`
size=0x`objdump -h -j $section $1 | grep $section|cut -f 12 -d " "`
src=`printf "0x%X\n" $[$ep_scratch + $offset]`
memtool -32 0x190000B0=$dst
memtool -32 0x190000B4=$src
memtool -32 0x190000B8=$size
memtool -32 0x190000BC=$ctrl

stat=0
counter=1000
while [ $((stat)) -eq 0 ]
do
        stat=0x`memtool -32 0x190000C8 1 |grep "0x190000C8:"| cut -f 2 -d ":"|sed 's/ //g'`
        ((counter--))
        [ $counter -eq 0 ] && { echo "*** ERROR: DMA response timeout, command failed."; echo; exit; }
done
memtool -32 0x190000C8=1

# load vdram
section=.vdram
ctrl=0x000
offset=0x`objdump  -h -j $section $1 | grep $section|cut -f 18 -d " "`
dst=0x`objdump  -h -j $section $1 | grep $section|cut -f 14 -d " "`
size=0x`objdump -h -j $section $1 | grep $section|cut -f 12 -d " "`
src=`printf "0x%X\n" $[$ep_scratch + $offset]`
memtool -32 0x190000B0=$dst
memtool -32 0x190000B4=$src
memtool -32 0x190000B8=$size
memtool -32 0x190000BC=$ctrl

stat=0
counter=1000
while [ $((stat)) -eq 0 ]
do
        stat=0x`memtool -32 0x190000C8 1 |grep "0x190000C8:"| cut -f 2 -d ":"|sed 's/ //g'`
        ((counter--))
        [ $counter -eq 0 ] && { echo "*** ERROR: DMA response timeout, command failed."; echo; exit; }
done
memtool -32 0x190000C8=1

section=.idram
ctrl=0x000
offset=0x`objdump  -h -j $section $1 | grep $section|cut -f 18 -d " "`
dst=0x`objdump  -h -j $section $1 | grep $section|cut -f 14 -d " "`
size=0x`objdump -h -j $section $1 | grep $section|cut -f 12 -d " "`
src=`printf "0x%X\n" $[$ep_scratch + $offset]`
memtool -32 0x190000B0=0x4000
memtool -32 0x190000B4=$src
memtool -32 0x190000B8=$size
memtool -32 0x190000BC=$ctrl

stat=0
counter=1000
while [ $((stat)) -eq 0 ]
do
        stat=0x`memtool -32 0x190000C8 1 |grep "0x190000C8:"| cut -f 2 -d ":"|sed 's/ //g'`
        ((counter--))
        [ $counter -eq 0 ] && { echo "*** ERROR: DMA response timeout, command failed."; echo; exit; }
done
memtool -32 0x190000C8=1

