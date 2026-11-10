#!/bin/bash
# Copyright 2024-2026 NXP
# SPDX-License-Identifier: BSD-3-Clause
####################################################################
# RFNM RF configuration through the rfnm_primary sysfs.
# Lime daughterboard: an RX apply done while tx0 is on keeps the front-end
# port owned by TX (TX to RX coupling about 35 dB higher, raised floor).
# The TX apply restores the switches, so RX is always applied before TX:
# limerx re-applies tx0 when it is on, limetxrx applies RX then TX.
####################################################################

SYS=/sys/kernel/rfnm_primary
# TX LO: limetx default 1850 MHz (same LO as RX), limetxrx default 1860 MHz
# (10 MHz offset keeps the TX line away from the RX DC). Override with TX_FREQ.

print_usage()
{
  echo "usage ./txrx_en.sh <granitatx/granitatxrx/granita2R/limetx/limerx/limetxrx/off/status/profile>"
  echo "  limetxrx  Lime RX then TX (TX_FREQ, default 1860000000)"
  echo "  off       Lime tx0 off then rx0 re-apply (port back to RX)"
  echo "  status    print rx0/tx0 settings"
  echo "  profile <file>  apply an rf_tune profile (guards checked, see rf_tune -h)"
  echo "  defaults  apply saved next-run defaults (/etc/rf_tune/<board>.cfg, rf_tune -d)"
  echo "  reset     apply built-in rf_tune defaults (saved file kept, rf_tune --reset)"
  echo "  interactive tuning: rf_tune (second terminal next to iq-app-rx.sh -X)"
}

tx_is_on()
{
  grep -qi "on" $SYS/tx0/enable 2>/dev/null
}

# lime_tx / lime_rx values must stay in sync with lime_def[] in
# rf_tune/rf_tune.c (built-in defaults used by rf_tune --reset)
lime_tx()
{
  # $1 = TX LO in Hz
  echo on > $SYS/tx0/enable
  echo sma_b > $SYS/tx0/path
  echo $1 > $SYS/tx0/freq
  echo 20 > $SYS/tx0/rfic_lpf_bw
  echo 10 > $SYS/tx0/power
  echo 1 > $SYS/tx0/apply
}

lime_rx()
{
  echo on > $SYS/rx0/enable
  echo sma_a > $SYS/rx0/path
  echo 1850000000 > $SYS/rx0/freq
  echo 100 > $SYS/rx0/rfic_lpf_bw
  echo 20 > $SYS/rx0/gain
  echo 1 > $SYS/rx0/apply
}

ch_status()
{
  local d f
  for d in rx0 tx0; do
    echo "== $d"
    for f in enable path freq rfic_lpf_bw gain power; do
      [ -r $SYS/$d/$f ] && printf "  %-12s %s\n" $f "$(cat $SYS/$d/$f 2>/dev/null)"
    done
  done
}

if [ $# -eq 0 ];then
  print_usage
  exit 1
else
  if [ $1 = granitatx ];then
    echo "Setting up FDD sma_b Tx "

    echo on > /sys/kernel/rfnm_primary/tx0/enable
    echo sma_b > /sys/kernel/rfnm_primary/tx0/path
    echo 1850000000 > /sys/kernel/rfnm_primary/tx0/freq
    echo 0 > /sys/kernel/rfnm_primary/tx0/rfic_lpf_bw
    echo 1 > /sys/kernel/rfnm_primary/tx0/power
    echo 1 > /sys/kernel/rfnm_primary/tx0/apply

  elif [ $1 = granitatxrx ];then
    echo "Setting up FDD sma_b Tx sma_a Rx"

    echo on > /sys/kernel/rfnm_primary/tx0/enable
    echo sma_b > /sys/kernel/rfnm_primary/tx0/path
    echo 1850000000 > /sys/kernel/rfnm_primary/tx0/freq
    echo 0 > /sys/kernel/rfnm_primary/tx0/rfic_lpf_bw
    echo 1 > /sys/kernel/rfnm_primary/tx0/power
    echo 1 > /sys/kernel/rfnm_primary/tx0/apply

    echo on > /sys/kernel/rfnm_primary/rx1/enable
    echo sma_a > /sys/kernel/rfnm_primary/rx1/path
    echo 1850000000 > /sys/kernel/rfnm_primary/rx1/freq
    echo 100 > /sys/kernel/rfnm_primary/rx1/rfic_lpf_bw
    echo 20 > /sys/kernel/rfnm_primary/rx1/gain
    echo 1 > /sys/kernel/rfnm_primary/rx1/apply

  elif [ $1 = granita2R ];then
    echo "Setting up FDD 2R sma_a and sma_b Rx"

    echo on > /sys/kernel/rfnm_primary/rx1/enable
    echo sma_a > /sys/kernel/rfnm_primary/rx1/path
    echo 1850000000 > /sys/kernel/rfnm_primary/rx1/freq
    echo 100 > /sys/kernel/rfnm_primary/rx1/rfic_lpf_bw
    echo 20 > /sys/kernel/rfnm_primary/rx1/gain
    echo 1 > /sys/kernel/rfnm_primary/rx1/apply
    echo on > /sys/kernel/rfnm_primary/rx0/enable
    echo sma_b > /sys/kernel/rfnm_primary/rx0/path
    echo 1850000000 > /sys/kernel/rfnm_primary/rx0/freq
    echo 100 > /sys/kernel/rfnm_primary/rx0/rfic_lpf_bw
    echo 20 > /sys/kernel/rfnm_primary/rx0/gain
    echo 1 > /sys/kernel/rfnm_primary/rx0/apply

  elif [ $1 = limetx ];then
    echo "Setting up LIME Tx FDD "

    lime_tx ${TX_FREQ:-1850000000}

  elif [ $1 = limerx ];then
    echo "Setting up LIME Rx FDD "
    lime_rx
    if tx_is_on; then
      # RX apply with TX on leaves the port owned by TX: re-apply TX to
      # restore the front-end switches
      echo "tx0 is on: re-applying tx0 to restore the front-end switches"
      echo 1 > $SYS/tx0/apply
    fi

  elif [ $1 = limetxrx ];then
    echo "Setting up LIME Rx then Tx FDD (TX ${TX_FREQ:-1860000000} Hz)"
    lime_rx
    lime_tx ${TX_FREQ:-1860000000}

  elif [ $1 = off ];then
    echo "LIME tx0 off, rx0 re-applied"
    echo off > $SYS/tx0/enable
    echo 1 > $SYS/tx0/apply
    tx_is_on || echo 1 > $SYS/rx0/apply

  elif [ $1 = status ];then
    ch_status

  elif [ $1 = profile ];then
    command -v rf_tune >/dev/null || { echo "rf_tune not found in PATH (make install + deploy.sh)"; exit 1; }
    rf_tune -l "${2:?profile file}"

  elif [ $1 = defaults ] || [ $1 = reset ];then
    if command -v rf_tune >/dev/null; then
      if [ $1 = defaults ]; then rf_tune -y -d; else rf_tune -y --reset; fi
    else
      echo "rf_tune not found: Lime built-in values (RX then TX)"
      lime_rx
      lime_tx 1850000000
    fi

  else
    print_usage
  fi
fi
