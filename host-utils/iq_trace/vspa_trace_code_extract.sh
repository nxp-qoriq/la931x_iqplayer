#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024 NXP
####################################################################

#set -x

echo "### exporting vspa trace code from l1-trace.h"
md5sum=$(md5sum  $1)

echo "/* SPDX-License-Identifier: BSD-3-Clause" > ./vspa_trace_enum.h
echo "* Copyright 2024 NXP">> ./vspa_trace_enum.h
echo " */">> ./vspa_trace_enum.h


echo "/* File generated based on l1-trace.h md5sum" $md5sum "*/" >> ./vspa_trace_enum.h
echo "l1_trace_code_t l1_trace_code[] ={" >> ./vspa_trace_enum.h

dos2unix  $1
grep " L1_TRACE_"  $1  | grep "\*" > ./vspa_trace_enum_0.txt
sed 's/\*//g' vspa_trace_enum_0.txt > ./vspa_trace_enum_1.txt
sed 's/\///g' vspa_trace_enum_1.txt > ./vspa_trace_enum_2.txt
sed 's/,/ /g' vspa_trace_enum_2.txt > ./vspa_trace_enum.txt

while read in; do
	code=$(echo $in | cut -f 2  -d "*" | cut -f 1 -d " ")
	text=$(echo $in | cut -f 3  -d "*"| cut -f 2 -d " ")
	echo "{ "$code", \""$text "\"}," >> ./vspa_trace_enum.h
done < ./vspa_trace_enum.txt
	echo "{ 0xFFFF, \"MAX_CODE_TRACE\"}" >> ./vspa_trace_enum.h

echo "};" >> ./vspa_trace_enum.h

rm vspa_trace_enum*.txt

