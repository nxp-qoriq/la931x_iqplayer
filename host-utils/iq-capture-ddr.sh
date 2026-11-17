#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-capture-ddr.sh <DDR buff size nb 4KB> [half duplex]"
echo "ex : ./iq-capture-ddr.sh 1200"
echo "ex : ./iq-capture-ddr.sh 1200 1"
}

# check parameters
if [ $# -lt 1 ];then
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
if [ $1 -gt $[$maxsize/2/4096] ];then
        echo $1 x4KB too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

if [ $# -gt 1 ];then
	if [ $2 -eq 1 ];then
		cmd=`printf "0x%X\n" $[0x06d20000 + $1]`
	else 
		cmd=`printf "0x%X\n" $[0x06d00000 + $1]`
	fi
else
	cmd=`printf "0x%X\n" $[0x06d00000 + $1]`
	# with compression 0x06d80000
	# withotu QEC      0x06900000
fi

vspa_mbox send 0 0 $cmd $buffep
vspa_mbox recv 0 0
echo running until ./iq-stop.sh and 
echo "memtool -d /dev/shiva0 -a $buff -o iqdata.bin $[4096 * $1]"
echo "(bin2mem -f iqdata.bin -a $buff -r $[4096 * $1] on kernels without CONFIG_STRICT_DEVMEM)"

