## SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
## Copyright 2012 - 2025    NXP

#source the library
ssource "../../../vspa_common_la9310/vspa-lib/common/scrip/scripts/libsim.tcl"

config DebugTimeout 1000

debug LA9310_cal_Debug_core00_LA9310_Download_qds


# Enable AXIQ loopbacks in DBGGENCR
#mem 0xE00800EC -s =0x0000005E
#puts "Finished AXIQ Loopback Configuration"


# save the output file
# sim_dram_save "test_vectors/out_1.hex" [evaluate #x output_buffer] 1024
# sim_dram_save "test_vectors/in_1.hex" [evaluate #x input_buffer] 1024
# run the simulation
go
    




