#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-app-tx.sh <input iq sample file> [FIFO Size]"
echo "ex : ./iq-app-tx.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin "
}

# check parameters
if [ $# -lt 1 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

fifo=$2
if [ $# -eq 1 ];then
	fifo=32768
fi

fifo4k=$[$fifo / 4096]
fifo=$[$fifo4k * 4096]

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)

ddrh=`la9310_modem_info | grep FLOOD |cut -f 2 -d "|" |sed 's/	//g'|sed 's/ //g'`
maxsize=`la9310_modem_info | grep FLOOD |cut -f 4 -d "|" |sed 's/	//g'|sed 's/ //g'`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi
if [ $fifo -gt $[$maxsize/2] ];then
        echo fifo too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

# Use first half of iqflood region for TX FIFO i.e. 32KB fifo at offset 0 
taskset 0x8 iq_app -t -f $1 -F 0x00000000 $fifo &
./iq-start-txfifo.sh $fifo4k
