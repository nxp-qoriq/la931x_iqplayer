#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################
#set -x

print_usage()
{
echo "usage: ./get_vspa_proxy_offset.sh"
}

# check parameters
if [ $# -ne 0 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

cmd=0x11100000
#echo vspa_mbox send 0 0 $cmd 0
vspa_mbox send 0 0 $cmd 0
proxy_offset=0x`vspa_mbox recv 0 0| grep MSB|cut -f 3 -d "x"|cut -f 1 -d " "|sed 's/,//g'`
echo $proxy_offset

