[la931x_iqplayer v1.0]

The iq-player v1.0 delivers the following features:
 - Enable iq receive/transmit of time domain IQ samples to/from binary files
 - Support Tx/Rx QEC
 - Support x2 or x4 decimation, x2 used by default
 - Provide iq_mon utility to monitor statistics
 - Provide iq_app/lib_iqplayer to send and receive time domain IQ samples from linux user space application
 - Support full duplex 1T1R at 61.44MSPS
 - Support half duplex 1T or 1R at 122.88MSPS
 - Support full duplex 1T2R at 61.44MSPS
 - Experimental support for
   - External DMA agent Rx and/or Tx in linux user space "iq_streamer" aiming at (1) reach 160MSPS (2) achieve FD 122.88MSPS  
   - External DMA Rx agent offload in i.mx8mp M7 aiming to reach 160MSPS  

[la931x_iqplayer v1.1]

- Add linux Makefiles
- Add runtime VSPA image reload/swap
- Add sync start capability

Known limitations:
- Full Duplex 122.88 MSPS is working but suffers drop of samples due to RX/TX fifo overflow/underrun
- 4R firmware doesn't support all 4 channels enabled, user can enable only 2 channels at the same time

[la931x_iqplayer v1.2]

New features
1. IQ Player on the RFNM SDK (RFNM Blue, Lime / Granita / Yucca daughterboards)
   - builds from the RFNM SDK plus doc/patches
   - guide: doc/iqplayer_porting_guide_RFNM-SDK.rst
   - optional VSPA debugger (vdbg, profile, DAP) on RFNM: la9310_debug +
     doc/patches/la9310_debug (rfnm transport)
2. Event-driven VSPA and host flow control
   - the VSPA sleeps between passes and wakes on GO events (DMA completion or
     host mailbox) instead of polling DMEM: lower DMEM load, needed for 4R
   - host flow control: counter written directly in VSPA DMEM, then a mailbox-1
     GO wakes the VSPA after a flow-control stall 
3. 4R support
   - 0T4R firmware: lossless 4 x RX at 61.44 MSPS
   - 0T4R has no TX: TX start is NACKed by the VSPA. iq-app-tx.sh now stops
     with an error instead of starting iq_app (vspa_mbox recv returns 2 on
     NACK, 1 on timeout). Use 1T1R / 1T2R for a TX tone over the air.
   - per-channel RX QEC (fused QEC + x2 decimation), updated live without loss
   - RX channel select (rx-chan.sh / mbox 0x0D) fixed for 2R/4R: the AXIQ read
     address ignored the selected channel. rx-chan.sh now also accepts an
     explicit list per slot (e.g. "rx-chan.sh 3 1" = both LimeSDR-Micro
     LMS7002M RX channels in 1T2R). Invalid or duplicated chanIDs are NACKed.
   - LimeSDR-Micro M4: "lime rx 2" powers up LMS7002M RX channel B
4. Application examples
   - iq-app-rx.sh -X: live RX spectrum per channel, saturation indicator,
     time-domain I/Q view, IQ snapshot to file
   - iq-app-tx.sh -T: interactive tone generator with TX QEC
   - saved RX / TX QEC applied at start
5. RX / TX pipeline fixes
   - RX stall (1T1R, half rate with flow control)
   - TX collapse at 122.88 MSPS and TX underrun on restart
   - VCPU stack overflow
6. CONFIG_STRICT_DEVMEM support
   - iq_app, iq_mon, iq_trace map the scratch buffer, IQFLOOD and the PCIe BARs
     through /dev/shiva0 instead of /dev/mem, so the IQ Player works on kernels
     built with CONFIG_STRICT_DEVMEM=y
   - needs the la9310shiva modinfo mmap patch:
     doc/patches/la93xx_host_sw/0001-*.patch (NXP la93xx_host_sw, i.MX95 /
     LimeSDR-Micro), doc/patches/la9310-driver/0004-*.patch (RFNM)
   - with an unpatched driver, regions refused by /dev/shiva0 are mapped through
     /dev/mem (note printed once; needs CONFIG_STRICT_DEVMEM not set for RAM)
7. LimeSDR-Micro bring-up fixes
   - LA9310 clock: LA9310_SYS_CLK_FREQ must be 122880000 (former FreeRTOS default
     61.44 MHz gave x2 tone errors and a TX stall), see
     doc/patches/la93xx_freertos (also LMS channel B RX defaults, RX channel select)
   - runtime RX IQ swap: mbox 0x12, host-utils/rx-iqswap.sh
   - load-limesdr-micro.sh (replaces load-la9310.sh) sets IQ swap 0
     and RX order RX0 RX1 RO0 RO1 (rx-chan.sh 3 1 2 0); load-rfnm.sh sets
     IQ swap 1. iq-board-defaults.sh applies them, also after
     iq-reload-vspa.sh
   - DPDK dfe_app / bbdev la93xx fixes: doc/patches/dpdk
8. RX/TX configuration readback (no proxy size change)
   - proxy word 0x1C (rx_num_chan) now packs: [7:0] RX count, [8] RX IQ swap,
     [11:9] TX count, [31:16] RX channel map (4 bits/slot); old firmware
     leaves [31:16]=0 and only [7:0] is used
   - iq_app spectrum labels show the VSPA chanID, LA9310 port and board
     connector, e.g. "Ch0 [chan 3 RX0 | LMS7002M RX1 (A)]"
   - rx-chan.sh -r prints the current mapping; iq_mon prints it in its header
   - TX guard: iq_app / iq-app-tx.sh / iq-replay.sh refuse TX when the loaded
     firmware has no TX channel (0T4R)

Other changes
- Firmware variants: 1T1R, 1T2R, 0T4R (+ _DIAG / _PROF)
- iq-reload-vspa.sh: one command for a runtime VSPA image swap
- iq_mon: real-interval rates, VSPA cycle meters
- NXP BSP (LimeSDR-Micro): la93xx_host_sw needs one patch (doc/patches/la93xx_host_sw,
  CONFIG_STRICT_DEVMEM support), no other patches with LSDK 25.12 / SDK 4.0; iq_app
  uses the ARM generic timer
- Docs: reference manual simplified, design notes added, LimeSDR-Micro guide
  updated, RFNM quick start guide marked legacy
- SBOM.spdx.json (SPDX 2.3) replaces SCR.txt

Known limitations
- 0T4R: a few AXIQ overruns counted at stream start
- 1T1R at 122.88 MSPS: half duplex only
- LimeSDR-Micro with DCS_CLK 61.44 MHz: TX 1ch + mBurst stalls (open, see design
  notes); use 122.88 MHz
- LA9310 advertises PCIe ATS without invalidation support: use pci=noats or IOMMU
  passthrough on hosts with a translating IOMMU (see design notes)
