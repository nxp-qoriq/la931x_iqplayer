#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 NXP
####################################################################
#set -x

# scratch_addr and scratch_size are deprecated parameters for la9310shiva, keeping it for backward compatibility
scratch_addr=0x`hexdump -C /sys/firmware/devicetree/base/reserved-memory/la93@*/reg |cut -f 7-10 -d " " |sed 's/ //g'|cut -f 1 -d " " |head -1`
scratch_size=0x`hexdump -C /sys/firmware/devicetree/base/reserved-memory/la93@*/reg |cut -f 16-19 -d " " |sed 's/ //g'|head -1`

[ -f /lib/firmware/la9310_dfe.bin ] || { echo "***ERROR: missing /lib/firmware/la9310_dfe.bin. You are using old BSP, upgrade to BSP 0.4 or above, or rename la9310.bin to la9310_dfe.bin. Boot failed"; exit 1; }
[ -f /lib/firmware/apm-iqplayer-1T1R.eld ] || { echo "***ERROR: missing /lib/firmware/apm-iqplayer-1T1R.eld. You are using old BSP, upgrade to BSP 0.4.3 or above, or rename la9310.bin to la9310_dfe.bin. Boot failed"; exit 1; }

adcm=0xf
dacm=0x1
if [ $# -eq 1 ];then
        if [ $1 -eq 1 ];then
                adcm=0
                dacm=0
        fi
        if [ $1 -eq 2 ];then
                adcm=0
                dacm=1
        fi
fi

# load la9310
echo 7 > /proc/sys/kernel/printk
echo insmod /lib/modules/$(uname -r)/extra/la9310shiva.ko scratch_buf_size=$scratch_size scratch_buf_phys_addr=$scratch_addr adc_mask=0xf adc_rate_mask=$adcm dac_mask=0x1 dac_rate_mask=$dacm alt_firmware_name=la9310_dfe.bin alt_vspa_fw_name=apm.eld
insmod /lib/modules/$(uname -r)/extra/la9310shiva.ko scratch_buf_size=$scratch_size scratch_buf_phys_addr=$scratch_addr adc_mask=0xf adc_rate_mask=$adcm dac_mask=0x1 dac_rate_mask=$dacm alt_firmware_name=la9310_dfe.bin alt_vspa_fw_name=apm.eld

# insert rf modules kernel modules
[ -f /lib/modules/$(uname -r)/extra/sdr_gpio.ko ] && insmod /lib/modules/$(uname -r)/extra/sdr_gpio.ko
[ -f /lib/modules/$(uname -r)/extra/sdr_lalib.ko ] && insmod /lib/modules/$(uname -r)/extra/sdr_lalib.ko
[ -f /lib/modules/$(uname -r)/extra/sdr_daughterboard.ko ] && insmod /lib/modules/$(uname -r)/extra/sdr_daughterboard.ko
[ -f /lib/modules/$(uname -r)/extra/sdr_lime.ko ] && insmod /lib/modules/$(uname -r)/extra/sdr_lime.ko
[ -f /lib/modules/$(uname -r)/extra/sdr_granita.ko ] && insmod /lib/modules/$(uname -r)/extra/sdr_granita.ko

# enable ADC/DAC always on
sleep 1
./fdd-start.sh

#
#echo -n "rfnm_m7_v0.elf" > /sys/class/remoteproc/remoteproc0/firmware
#echo stop > /sys/class/remoteproc/remoteproc0/state
#echo start > /sys/class/remoteproc/remoteproc0/state

# check ADC/DAC clock
#  memtool -32 0x19040300 1
#  00010303 -> 61.44Mhz
#  00000000 -> 122.88Mhz

ddrh=`la9310_modem_info | grep FLOOD |cut -f 2 -d "|" |sed 's/	//g'|sed 's/ //g'`
maxsize=`la9310_modem_info | grep FLOOD |cut -f 4 -d "|" |sed 's/	//g'|sed 's/ //g'`
if [[ "$ddrh" -eq "" ]];then
        echo can not retrieve IQFLOOD region, is LA9310 shiva started ?
        exit 1
fi

proxy_addr=`printf "0x%X\n" $[$ddrh + $maxsize - 1024]`
proxy_offset=`printf "0x%X\n" $[$maxsize - 1024]`
iqflood_size=`printf "0x%X\n" $maxsize`
echo iqflood_addr $ddrh 
echo iqflood_size $iqflood_size
echo proxy_addr $proxy_addr
./set-proxy-offset.sh $proxy_offset

