#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./set-proxy-offset.sh <proxy-offset> "
}

# check parameters
if [ $# -lt 1 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

cmd=0x11000000

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)
# [] NXP-LA9310-Driver 0000:01:00.0: RFNM IQFLOOD Buff:0xc0000000[H]-0x96400000[M],size 20971520
ddrh=`dmesg |grep "IQ Flood Buffer"|cut -f 7 -d ":"|cut -f 1 -d " "| head -1`
maxsize=`dmesg |grep IQFLOOD |cut -f 2 -d "z"|cut -f 2 -d " "| head -1`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi
if [[ $1 -gt $maxsize ]];then
        echo $1 too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

#echo vspa_mbox send 0 0 $cmd $ddrep
vspa_mbox send 0 0 $cmd $1
vspa_mbox recv 0 0
