## SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
## Copyright 2012 - 2025    NXP

#source the library
source "../../../vspa_common_la9310/vspa-lib/common/scrip/scripts/libsim.tcl"

config DebugTimeout 1000

debug LA9310_cal_Debug_core00_LA9310_Download

#initialize the setup
sim_init_config
		
dma_channel 11  [pwd]/test_vectors/stream_tx11

dma_channel 1  [pwd]/test_vectors/stream_rx1
dma_channel 2  [pwd]/test_vectors/stream_rx2
dma_channel 3  [pwd]/test_vectors/stream_rx3
dma_channel 4  [pwd]/test_vectors/stream_rx4


set f_vspa 700
set fs     160
set w_axi  256
set T_dma  [expr $f_vspa/$fs*16*$w_axi/32]
# DMA trigger for DAC channels
dma_trigger 11   1894  $T_dma  10000

# DMA trigger for ADC channels
dma_trigger 1   1894  $T_dma  10000
dma_trigger 2   1894  $T_dma  10000
dma_trigger 3   1894  $T_dma  10000
dma_trigger 4   1894  $T_dma  10000

# run the simulation
go
    
# save the output file
# sim_dram_save "test_vectors/time_samples_hf_DAC_vsp.hex" [evaluate #x output_buffer] 4096
# sim_dram_save "test_vectors/input_buffer_vsp.hex" [evaluate #x input_buffer] 4096



# kill
		
# verify simulation output with reference
# puts "Verifying DAC time samples"
# set testres [sim_file_match "test_vectors/time_samples_hf_DAC_vsp.hex" "test_vectors/time_samples_hf_DAC_matlab.hex" 512]
# puts "Verifying recived samples with transmitted samples"
# set testres [sim_file_match "test_vectors/time_samples_hf_DAC_vsp.hex" "test_vectors/input_buffer_vsp.hex" 512]
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
