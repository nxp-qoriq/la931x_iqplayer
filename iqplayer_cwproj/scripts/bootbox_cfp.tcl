## SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
## Copyright 2012 - 2025    NXP

#source the library
source "../../../vspa_common_la9310/vspa-lib/common/scrip/scripts/libsim.tcl"

config DebugTimeout 1000

debug LA9310_cal_Debug_core00_LA9310_Download_CFP

# Enable the PHY Timer
mem 0x41020000 -s =0x1

# Clear Comparator Trigger 1
mem 0x4102000C -s =0x84
# Generate Trigger
mem 0x4102000C -s =0x8

# Clear Comparator Trigger 2
mem 0x41020014 -s =0x84
# Generate Trigger
mem 0x41020014 -s =0x8

# Clear Comparator Trigger 3
mem 0x4102001C -s =0x84
# Generate Trigger
mem 0x4102001C -s =0x8

# Clear Comparator Trigger 4
mem 0x41020024 -s =0x84
# Generate Trigger
mem 0x41020024 -s =0x8

# Generate Trigger
mem 0x4102005C -s =0x8

# Enable AXIQ loopbacks in DBGGENCR
mem 0xE00800EC -s =0x0000005E

puts "Finished AXIQ Loopback Configuration"

# Write mailbox
reg HOST_OUT_0_MSB = 0x01100000
reg HOST_OUT_0_LSB = 0x00000020

reg DMA_COMP_STAT = 0xFFFFFFFF
reg DMA_GO_STAT = 0xFFFFFFFF
reg DMA_STAT_ABORT = 0xFFFFFFFF


# run the simulation
go
    
# save the output file
# sim_dram_save "test_vectors/time_samples_hf_DAC_vsp.hex" [evaluate #x output_buffer] 4096
# sim_dram_save "test_vectors/input_buffer_vsp.hex" [evaluate #x input_buffer] 4096
#sim_dram_save "test_vectors/time_samples_hf_DAC_vsp.hex" [evaluate #x output_buffer] 1024
#sim_dram_save "test_vectors/input_buffer_vsp.hex" [evaluate #x input_buffer] 1024



#kill
		
# verify simulation output with reference
# puts "Verifying DAC time samples"
# set testres [sim_file_match "test_vectors/time_samples_hf_DAC_vsp.hex" "test_vectors/time_samples_hf_DAC_matlab.hex" 512]
#puts "Verifying recived samples with transmitted samples"
#set testres [sim_file_match "test_vectors/time_samples_hf_DAC_vsp.hex" "test_vectors/input_buffer_vsp.hex" 512]
# puts "Verifying fft"
# set testres [sim_file_match "test_vectors/fftout_vsp.hex" "test_vectors/fftout_matlab.hex" 512]
# puts "Verifying power"
# set testres [sim_file_match "test_vectors/power_vsp.hex" "test_vectors/power_matlab.hex" 512]
# puts "Verifying SINAD"
# set testres [sim_file_match "test_vectors/SINAD_vsp.hex" "test_vectors/SINAD_matlab.hex" 1]

# puts "Verifying DMA transfer from DMEM to AXIQ"
# set testres [sim_file_match "test_vectors/time_samples_hf_DAC_vsp.hex" "test_vectors/stream_tx4_WR.dma" 512]
# puts "Verifying DMA transfer from AXIQ to DMEM"
# set testres [sim_file_match "test_vectors/input_buffer_vsp.hex" "test_vectors/stream_rx1_RD.dma" 512]

# return $testres
