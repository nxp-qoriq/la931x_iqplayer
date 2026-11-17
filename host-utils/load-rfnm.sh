#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2025 NXP
####################################################################
# RFNM Blue board adaptation of load-limesdr-micro.sh.
# Differences from NXP BSP:
#   - kernel modules live in /rfnm/kernel/ (not /lib/modules/$(uname -r)/extra/)
#   - RF module KOs use rfnm_ prefix (not sdr_)
#set -x

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KMOD_DIR=/rfnm/kernel

# scratch_addr and scratch_size are deprecated parameters for la9310shiva,
# keeping them for backward compatibility
scratch_addr=0x`hexdump -C /sys/firmware/devicetree/base/reserved-memory/la93@*/reg |cut -f 7-10 -d " " |sed 's/ //g'|cut -f 1 -d " " |head -1`
scratch_size=0x`hexdump -C /sys/firmware/devicetree/base/reserved-memory/la93@*/reg |cut -f 16-19 -d " " |sed 's/ //g'|head -1`

[ -f /lib/firmware/la9310_dfe_122.bin ] || { echo "***ERROR: missing /lib/firmware/la9310_dfe_122.bin. Boot failed"; exit 1; }
[ -f /lib/firmware/apm.eld ] || { echo "***ERROR: missing /lib/firmware/apm.eld Boot failed"; exit 1; }


adcm=0xf   # adc_mask: bitmask of enabled ADC channels (0xf = all 4 channels)
adcr=0     # adc_rate_mask: per-channel half-rate flag (0=122.88 MHz, 0xf=61.44 MHz)
dacm=0x1   # dac_mask: bitmask of enabled DAC channels
dacr=0     # dac_rate_mask: per-channel half-rate flag (0=122.88 MHz, 1=61.44 MHz)
if [ $# -ge 1 ];then
        if [ $1 -eq 1 ];then
                adcm=0
                adcr=0
                dacm=0
                dacr=0
        fi
        if [ $1 -eq 2 ];then
                adcm=0
                adcr=0
                dacm=1
                dacr=0
        fi
fi

# load la9310
# Note: the rfnm build of la9310shiva.ko ignores alt_firmware_name/alt_vspa_fw_name.
# Firmware selection is done by symlinking the desired files to the names the driver expects.
#ln -sf /lib/firmware/la9310_dfe_122.bin /lib/firmware/la9310.bin
echo 7 > /proc/sys/kernel/printk
echo insmod $KMOD_DIR/la9310shiva.ko scratch_buf_size=$scratch_size scratch_buf_phys_addr=$scratch_addr adc_mask=$adcm adc_rate_mask=$adcr dac_mask=$dacm dac_rate_mask=$dacr
insmod $KMOD_DIR/la9310shiva.ko scratch_buf_size=$scratch_size scratch_buf_phys_addr=$scratch_addr adc_mask=$adcm adc_rate_mask=$adcr dac_mask=$dacm dac_rate_mask=$dacr

# la9310rfnm.ko exports rfnm_* symbols required by rfnm_lalib and RF daughterboard modules
[ -f $KMOD_DIR/la9310rfnm.ko ]        && insmod $KMOD_DIR/la9310rfnm.ko
# Silence "phytimer: fw parked an RX lane after overrun" during IQ Player captures.
# The parameter only exists with doc/patches/la9310-driver/optional/0001 applied;
# with the stock module this line does nothing.
[ -f /sys/module/la9310rfnm/parameters/rfnm_rx_park_fault ] && \
    echo 0 > /sys/module/la9310rfnm/parameters/rfnm_rx_park_fault

# insert RF module kernel modules (only if present)
[ -f $KMOD_DIR/rfnm_gpio.ko ]         && insmod $KMOD_DIR/rfnm_gpio.ko
[ -f $KMOD_DIR/rfnm_lalib.ko ]        && insmod $KMOD_DIR/rfnm_lalib.ko
[ -f $KMOD_DIR/rfnm_daughterboard.ko ] && insmod $KMOD_DIR/rfnm_daughterboard.ko
[ -f $KMOD_DIR/rfnm_lime.ko ]         && insmod $KMOD_DIR/rfnm_lime.ko
[ -f $KMOD_DIR/rfnm_granita.ko ]      && insmod $KMOD_DIR/rfnm_granita.ko

# enable ADC/DAC always on
sleep 1
$SCRIPT_DIR/fdd-start.sh

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
# Zero the entire proxy region (1024 bytes) via /dev/shiva0 which bypasses
# CONFIG_STRICT_DEVMEM. This ensures a clean slate before the VSPA firmware
# starts writing produced/consumed counters.
memtool -d /dev/shiva0 -a "$proxy_addr" -z 1024
$SCRIPT_DIR/set-proxy-offset.sh $proxy_offset
# RFNM defaults: swap I/Q at the AXIQ input (VSPA default), firmware RX
# order; re-applied by iq-reload-vspa.sh
$SCRIPT_DIR/iq-board-defaults.sh rfnm

# ADC/DAC sample rate is fixed by the M4 binary (la9310.bin -> la9310_dfe_61/122.bin,
# DCS_FULL_RATE cmake option).
