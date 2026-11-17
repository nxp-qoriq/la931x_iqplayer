#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./iq-replay.sh <iq sample file> <size num 4KB> [half duplex]"
echo "  default   : VSPA default DDR read config (1 DMA ch + mBurst, full-duplex safe)"
echo "  3rd arg 1 : 2 DDR read DMA ch + mBurst (cmd 0x055a0000), may overflow RX in full duplex"
echo "ex : ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200"
echo "ex : ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200 1"
}

# check parameters
if [ $# -lt 2 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

if [ $# -gt 2 ];then
	if [ $3 -eq 1 ];then
       		cmd=0x055a0000
	else 
       		cmd=0x05500000
	fi
else
        cmd=0x05500000
fi

(ls $1 >> /dev/null 2>&1)||echo $1 file not found

# TX guard: refuse to start if the loaded VSPA image has no TX channel
. "$(cd "$(dirname "$0")" && pwd)/iq-proxy-cfg.sh"
if proxy_cfg_read && [ "$PCFG_VALID" -eq 1 ] && [ "$PCFG_NTX" -eq 0 ]; then
        echo "ERROR: the loaded VSPA image has no TX channel ($PCFG_NRX RX, 0 TX)."
        echo "       Load 1T1R or 1T2R: ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld"
        exit 1
fi

# check la9310 shiva driver and retrieve iqsample info i.e. iqflood in scratch buffer (non cacheable)

ddrh=`la9310_modem_info | grep FLOOD |cut -f 2 -d "|" |sed 's/	//g'|sed 's/ //g'`
ddrep=`la9310_modem_info | grep FLOOD |cut -f 3 -d "|" |sed 's/	//g'|sed 's/ //g'`
maxsize=`la9310_modem_info | grep FLOOD |cut -f 4 -d "|" |sed 's/	//g'|sed 's/ //g'`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi
if [ $2 -gt $[$maxsize/4096] ];then
        echo $2 x4KB too large to fit in IQFLOOD region $maxsize bytes
        exit 1
fi

echo bin2mem -f $1 -a $ddrh
bin2mem -f $1 -a $ddrh
cmd=`printf "0x%X\n" $[$cmd + $2]`
#echo vspa_mbox send 0 0 $cmd $ddrep
vspa_mbox send 0 0 $cmd $ddrep
vspa_mbox recv 0 0

