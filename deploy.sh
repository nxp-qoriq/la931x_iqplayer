#!/bin/sh
# Copyright 2024 NXP
# SPDX-License-Identifier: BSD-3-Clause
####################################################################
#set -x

print_usage()
{
echo "usage: ./deploy.sh <target ipaddr>"
echo "ex : ./deploy.sh 172.16.0.156"
}

# check parameters
if [ $# -ne 1 ];then
        echo Arguments wrong.
        print_usage
        exit 1
fi

scp -r ./install/* root@$1:/

