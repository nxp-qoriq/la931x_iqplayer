#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-capture.sh <DDR buff size nb 4KB> [half duplex]"
echo "ex : ./iq-capture.sh ./iqdata.bin 1200"
echo "ex : ./iq-capture.sh ./iqdata.bin 1200 1"
}

# check parameters
if [ $# -lt 2 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)

ddrh=`la9310_modem_info | grep FLOOD |cut -f 2 -d "|" |sed 's/	//g'|sed 's/ //g'`
ddrep=`la9310_modem_info | grep FLOOD |cut -f 3 -d "|" |sed 's/	//g'|sed 's/ //g'`
maxsize=`la9310_modem_info | grep FLOOD |cut -f 4 -d "|" |sed 's/	//g'|sed 's/ //g'`
buff=`printf "0x%X\n" $[$maxsize/2 + $ddrh]`
buffep=`printf "0x%X\n" $[$maxsize/2 + $ddrep]`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi
if [ $2 -gt $[$maxsize/2/4096] ];then
        echo $2 x4KB too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

if [ $# -gt 2 ];then
	if [ $3 -eq 1 ];then
		cmd=`printf "0x%X\n" $[0x06520000 + $2]`
	else 
		cmd=`printf "0x%X\n" $[0x06500000 + $2]`
	fi
else
	cmd=`printf "0x%X\n" $[0x06500000 + $2]`
fi

# IQFLOOD read through /dev/shiva0 first (CONFIG_STRICT_DEVMEM)
. "$(cd "$(dirname "$0")" && pwd)/la9310-io.sh"
vspa_mbox send 0 0 $cmd $buffep
vspa_mbox recv 0 0
echo la9310_dump $1 $buff $[4096 * $2]
la9310_dump $1 $buff $[4096 * $2] || { echo "*** ERROR: cannot read the capture"; exit 1; }

