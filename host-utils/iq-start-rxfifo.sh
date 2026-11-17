#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-start-rxfifo.sh <fifo size num 4KB>"
echo "ex : ./iq-start-rxfifo.sh 8"
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

cmd=`printf "0x%X\n" $[0x06900000 + $1]`
vspa_mbox send 0 0 $cmd $buffep
if ! vspa_mbox recv 0 0; then
        echo "ERROR: VSPA NACKed or did not answer RX start (cmd $cmd)"
        exit 1
fi
echo running until ./iq-stop.sh and 

