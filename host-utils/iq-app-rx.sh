#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-app-rx.sh <output iq sample file> <File size> [FIFO Size]"
echo "ex : ./iq-app-rx.sh ./iqdata.bin 4915200"
}

# check parameters
if [ $# -lt 2 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

if [ $# -eq 2 ];then
        fifo=131072
else
        fifo=$3
fi

fifo4k=$[$fifo / 4096]
fifo=$[$fifo4k * 4096]

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)

ddrh=`la9310_modem_info | grep FLOOD |cut -f 2 -d "|" |sed 's/	//g'|sed 's/ //g'`
maxsize=`la9310_modem_info | grep FLOOD |cut -f 4 -d "|" |sed 's/	//g'|sed 's/ //g'`
buff=`printf "0x%X\n" $[$maxsize/2 + 0x00100000]`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi
if [ $fifo -gt $[$maxsize/2] ];then
        echo $fifo fifo too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

# Use second half of iqflood region for TX FIFO i.e. 128KB fifo at offset iqfloodSize/2 
taskset 0x4 iq_app -r -c 0 -f $1 -s $2 -F $[$maxsize/2] $fifo &
./iq-start-rxfifo.sh $fifo4k
 
 


