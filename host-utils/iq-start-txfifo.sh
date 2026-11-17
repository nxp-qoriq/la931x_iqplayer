#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-start-txfifo.sh <fifo size num 4KB> [half duplex]"
echo "ex : ./iq-start-txfifo.sh 8"
}

# check parameters
if [ $# -lt 1 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

if [ $# -gt 1 ];then
	if [ $2 -eq 1 ];then
       		cmd=0x051a0000
	else 
       		cmd=0x05100000
	fi
else
	cmd=0x05100000
fi

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)

ddrh=`la9310_modem_info | grep FLOOD |cut -f 2 -d "|" |sed 's/	//g'|sed 's/ //g'`
ddrep=`la9310_modem_info | grep FLOOD |cut -f 3 -d "|" |sed 's/	//g'|sed 's/ //g'`
maxsize=`la9310_modem_info | grep FLOOD |cut -f 4 -d "|" |sed 's/	//g'|sed 's/ //g'`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi
if [ $1 -gt $[$maxsize/4096] ];then
        echo $1 x4KB too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

# start tx iq_streamer on 32KB (8x4KB) fifo at bottom of iqflood
cmd=`printf "0x%X\n" $[$cmd + $1]`
vspa_mbox send 0 0 $cmd $ddrep
vspa_mbox recv 0 0
rc=$?
if [ $rc -eq 2 ];then
        echo "ERROR: VSPA NACKed TX start (cmd $cmd): TX not supported by the loaded firmware (0TxR build, e.g. 4R = 0T4R ?)"
        exit 1
elif [ $rc -ne 0 ];then
        echo "ERROR: no valid VSPA reply to TX start (cmd $cmd, rc $rc)"
        exit 1
fi

