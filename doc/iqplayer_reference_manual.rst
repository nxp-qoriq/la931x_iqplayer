.. sectnum::

**IQ Player  Software Reference Manual** 
+++++++++++++++++++++++++++++++++++++++++


Overview
********
  
IQ Player is a lightweight reference application designed to exercise the LA9310 baseband TX/RX paths using raw time-domain I/Q samples. 
It targets SDR applications, RF testing, and calibration scenarios, providing signal conditioning features such as TX/RX QEC and decimation, 
while maximizing hardware throughput (sampling rate, PCI bandwidth, and channel count).

Key Features:

- Continuous raw RX/TX streaming

- Supported sampling rates: 61.44 MSPS, 122.88 MSPS, and optionally 160 MSPS

- Channel configurations: 1T1R, 1T2R, 1T4R

- QEC and decimation support


Hardware Requirements
*********************

The RFNM board (LA9310 + i.MX8M Plus) is the reference evaluation platform used by NXP for LA9310. This board is supported by default in the NXP LA9310 Linux BSP.
Users may generate images using the NXP Linux BSP or directly use prebuilt images available on www.nxp.com.

Refer to 'iqplayer_quick_start_guide_rfnm-imx8mplus.rst'

IQ Player can operate on any LA9310-based setup and has been validated with the LimeSDR‑Micro M.2 module:
https://www.crowdsupply.com/lime-micro/limesdr-micro

This module can be used with low-cost reference platforms such as:

- FRDM‑IMX8M Plus

- FRDM‑IMX95

See: https://www.nxp.com/design/design-center/development-boards-and-designs/frdm-development-boards:FRDM

Refer to 'iqplayer_porting_guide_LimeSDR-Micro_imx8mp_imx95.rst'


Software Components
*******************

The IQ Player data path is fully implemented in the VSPA firmware, handling ADC/DAC data flows on one side and DMA transfers of I/Q samples to and from host DDR on the other.

The Linux host environment is used to control and operate IQ Player:

Linux and la93xx_host_sw (Shiva) driver
---------------------------------------

The IQ Player VSPA firmware is loaded and started using the la93xx_host_sw (Shiva) PCI driver.

The host Linux system must provide support for this driver. NXP Linux BSPs include this support for multiple platforms.

- https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/tree/la93xx

LA9310 M4 FreeRTOS
------------------

IQ Player has no dependency on LA9310 M4 FreeRTOS control software.

The only constraint is that the FreeRTOS firmware must not use VSPA mailbox registers, which are exclusively used by IQ Player scripts.

The M4 firmware may still be used for other purposes such as:

- AXIQ loopback control
- RF configuration
- Clock configuration

On RFNM platforms, all RF and clock configuration is handled by Linux; therefore, the default BSP FreeRTOS image can be used.

- https://github.com/nxp-qoriq/la93xx_freertos

host-utils linux scripts
------------------------

IQ Player firmware operation is controlled via memory-mapped LA9310 VSPA mailbox registers.

Linux shell scripts are provided to control operating modes such as RX/TX start and stop:

- https://github.com/nxp-qoriq/la931x_iqplayer/host-utils

 
iq_app/lib_iqplayer
-------------------

IQ Player supports:

- Playing and capturing I/Q sample files
- Streaming samples between VSPA and host software

Host DDR buffers used during DMA have option to act as RX and TX FIFOs, with built-in backpressure handling.

A Linux library and sample application are provided for this mode:

- https://github.com/nxp-qoriq/la931x_iqplayer/host-utils/lib_iqplayer
- https://github.com/nxp-qoriq/la931x_iqplayer/host-utils/iq_app

Usage
*****

Initial configuration
----------------------
 
The basic principle consists of sending VSPA mailbox commands to stream data from a buffer in host DDR (the IQFLOOD region).

This region is:
- Declared in the device tree
- Mapped over PCI by the LA9310 driver la9310shiva using endpoint outbound windows

The configuration includes:
- Host physical address (e.g., 0x148000000 )
- Associated LA9310 physical address (e.g., 0xb0001000)

These values can be retrieved using la9310_modem_info :
::
 
 root@imx95frdm:~/host_utils# la9310_modem_info
 Board Name - NA
 LA93xx ID:0
 Dev name - shiva0
 PCI addr - 0000:01:00.0
 PCI WIN start     0x0 Size:0x40000000
 HIF start         0x92101b000 Size:0x328
 CCSR phys         0x910000000 Size:0x10000000
 TCML phys         0x921000000 Size:0x20000
 TCMU phys         0x928000000 Size:0x800000
 Scratch buf phys 0x144000000 Size:0x1e19240
 Scrach Buf Regions
 Region           |      Host Phy Addr    |      Modem Phy Addr   |      Size
 VSPA OVERLAY phys|      0xfe000000       |      0xa0000000       |      0x200000
 VSPA start       |      0xfe200040       |      0xa0200040       |      0x0
 FW start         |      0xfe200080       |      0xa0200080       |      0x18000
 DBG LOG phys     |      0xfe2180c0       |      0xa02180c0       |      0x1000
 IQ SAMPLES phys  |      0xfe219100       |      0xa0219100       |      0x1400000
 IQ FLOOD phys    |      0x148000000      |      0xb0001000       |      0x2000000
 NLM OPS start    |      0xff619140       |      0xa1619140       |      0x800000
 STD FW phys      |      0xffe19180       |      0xa1e19180       |      0x20000
 RF CAL phys      |      0xffe391c0       |      0xa1e391c0       |      0x32000
 DAC Mask: 0x1  Rate: 61.44 MHz
 ADC-0: ON  Rate : 61.44 MHz
 ADC-1: ON  Rate : 61.44 MHz
 ADC-2: ON  Rate : 61.44 MHz
 ADC-3: ON  Rate : 61.44 MHz
 

Single-Buffer Replay (TX)
-------------------------
 
IQ replay loads a binary waveform at the start of iqflood and issues a VSPA mailbox command that streams from the EP address (e.g., 0xb0001000) to the DAC through VSPA TX DMEM FIFO. DAC pacing drives FIFO progress; the DDR buffer is replayed until stop.
::
 
 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200
 
Single-Shot and Continuous Capture (RX)
---------------------------------------
 
RX capture uses the second half of iqflood buffer :
- One-shot capture: iq-capture.sh
- Continuous capture: iq-capture-ddr.sh

In multi-channel RX (2R/4R), the DDR buffer is split across channels and all captures start simultaneously.
::
 
 ./iq-capture.sh ./iqdata.bin 1200
 ./iq-capture-ddr.sh 1200
 ./iq-stop.sh
 bin2mem -f iqdata.bin -a 0x9CC00000 -r 4915200
 
Streaming via lib_iqplayer / iq_app
-----------------------------------
 
The lib_iqplayer library demonstrates host-driven streaming using DDR as TX/RX FIFOs, with VSPA DMA transferring data to and from DMEM FIFOs.
iq_app will stream data from/to standard malloc() buffer using intermediate FIFOs located in iqflood region.

Default examples use 32KB TX FIFO ans 128KB RX FIFO in DDR 
iq_app application will perform best when host is configured for real time operation :
- CPU isolation is enabled
- Architectural timer interrupts are reduced

To enable host-side flow control and operate DDR buffers as FIFO :
- iq-start-rxfifo.sh
- iq-start-txfifo.sh

::

 # Use first half of iqflood region for TX FIFO i.e. 32KB fifo at offset 0 
 ./iq-app-tx.sh <input iq sample file> [FIFO Size]
 
::

 # Use second half of iqflood region for TX FIFO i.e. 128KB fifo at offset iqfloodSize/2  
 ./iq-app-rx.sh <output iq sample file> <File size> [FIFO Size]

::

 get iq_app trace
  kill -USR1 <iq_streamer PID>

 to stop app
  kill <iq_streamer PID>
  ./iq-stop.sh

Performance 
***********

Throughput Requirements
-----------------------

Each I/Q sample consists of:
- 16-bit I + 16-bit Q = 4 bytes per sample
 
 ====================== ========================================
         Rate (MSPS)       Bandwidth (MB/s)
 ====================== ========================================
  61.44                     122.88
  122.88                    491.52
  160                       640
 ====================== ========================================
 
Experiments show a maximum PCI throughput of ~836 MB/s on i.MX8MP RFNM.
Actual transfer performance depends on the DMA engine and configuration.
IQ Player uses VSPA DMA by default.
 
VSPA DMA : Write to Host DDR ( RX Path )
-----------------------------------------
 
Inbound memory writes are limited to ~528 MB/s on i.MX8MP RFNM due to a known hardware limitation documented in AN13164.
This throughput is sufficient for 122.88 MSPS operation using a single VSPA DMA channel.

Operation at 160 MSPS requires a higher-performance host such as i.MX95.   
 
 .. note::
        IQ Player default configuration is using one VSPA DMA write channel.


VSPA DMA : Read from Host DDR ( Tx Path )
-----------------------------------------
 
LA9310 AXI bus supports 4 opened Rd transactions in parallel, so best throughput may be achieved by using 4x DMA channels.  
Here perf measured on i.MX8MP RFNM with different VSPA DMA configurations
 
 ========== ========================== ========= ===============================
 Config N°	Nb parallel DMA chan	mBurst        Bandwidth (MB/s)
 ========== ========================== ========= ===============================
    A                  1                  off        200 
    B                  2                  off        393 
    C                  4                  off        590 
    D                  1                  on	    481 
    E                  2                  on	    836  (max out)
    F                  4                  on	    836  (max out)
 ========== ========================== ========= ===============================
 
Config C, E, F are good for performance but are causing problem for full duplex operation 
( Tx reads starving Rx writes and causing fifo overflow ).  
 
 .. note::
        IQ Player default configuration is 2 channels without mBurst allowing Full Duplex operation at 61.44MSPS.
 .. note::
        IQ Player has an option to enable mBurst to achieve max performance in half duplex, tx only up to 160MSPS.  

Host eDMA : Read and Write
--------------------------
 
Host eDMA on RFNM i.MX8MP can reach ~700 MB/s with large transfers. 
While this enables higher bandwidth operation, it adds software complexity and requires host-side DMA agents synchronized with VSPA code.
 
 .. note::
        IQ Player does not use host eDMA by default.
 
VSPA DMA Considerations
***********************

- LA9310 VSPA DMA: 128-bit AXI3; 3 engines; 16 channels; single-burst per transaction; optional multi-burst (up to 4×256B).
- Priority: lower channel number = higher priority; channel 0 has strict priority.
- DMEM bandwidth: spread load across VDMEM and IDMEM; align transfers to bus widths; align AXI origins to 4 KB boundaries.
- Write engine: single; Read engines: ≥2; parallelize reads/writes on different channels.
- DMEM access priority (highest→lowest): DMA write, DMA read, AXI slave, IPPU, Debug, VCPU.
 
PCI Access Strategy (VSPA dmem proxy)
*************************************

Prefer posted PCI writes over reads to avoid initiator stalls. 
IQ Player mirrors control/statistics from VSPA DMEM to host DDR using a dmem proxy structure (s_vspa_dmem_proxy) located at the last 1 KB of IQFLOOD. 
Read-only fields (tx_state_readonly, rx_state_readonly, vspa_stats) are DMA-copied to DDR
writable fields (e.g., host flow control) expose DMEM offsets via dmemProxyOffset

Debugging & Monitoring
**********************

- iq_mon: polls t_stats in the DDR proxy region every second, adapts to channel count, and computes bandwidth.
- Tracing: iq_app keeps a local heap trace buffer; send SIGUSR1 to dump. VSPA firmware keeps DMEM trace readable via iq_trace.

::

 RX stats :
  DMA_AXIQ_RD    0x000e7592(00000491 MB/s)       0x000e7593(00000491 MB/s)
  DMA_DDR_WR     0x000e7592(00000245 MB/s)       0x000e7592(00000245 MB/s)
  EXT_DDR_WR     0x00000000(00000000 MB/s)       0x00000000(00000000 MB/s)
  DDR_WR_OVR     0x00000000                      0x00000000
  FIFO_RX_UDR    0x00000000                      0x00000000
  FIFO_RX_OVR    0x00000000                      0x00000000
  DMA_RX_CMD_OVR 0x00000000                      0x00000000
  EXT_DDR_WR_OVR 0x00000000                      0x00000000
 TX stats :
  DMA_AXIQ_WR    0x00000000(00000000 MB/s)
  DMA_DDR_RD     0x00000000(00000000 MB/s)
  EXT_DDR_RD     0x00000000(00000000 MB/s)
  DDR_RD_UDR     0x00000000
  FIFO_TX_UDR    0x00000000
  FIFO_TX_OVR    0x00000000
  DMA_TX_CMD_UDR 0x00000000
  EXT_DDR_RD_UDR 0x00000000
 Global stats :
  DMA_CFG_ERROR  0x00901003
  DMA_XFER_ERROR 0x00000000

::

 #clear vspa stats
 ./stats.sh -1

 #clear host stats
 ./iq_mon -c

 # get vspa trace
  iq_streamer -d

IQ Data Format
**************
 
Firmware uses 16-bit signed-magnitude (half-fixed). VSPA DMA can convert to 2’s complement expected by host software (int16). 
IQ Player scripts use 2’s complement files; MATLAB code is provided to generate/display binary waveforms.

QEC and DC Offset
*****************

Integrated VSPA kernels provide IQ imbalance (phase & amplitude) compensation and optional fractional delay, 
along with DC offset correction. Default kernel: txiqcomp(); 
alternate: txiqcomp_x32chf_5t() (adds 5-tap fractional delay). 
Python tooling (imb2qec-rfnm.py) assists parameter computation per channel.

Decimation
**********

Decimation kernels: decimator_2x_8_Taps_asm and decimator_4x_8_Taps_asm. 
Default uses 2× with an 8-tap FIR as a balanced choice for multi-RX and VSPA cycles.

Reload VSPA images at runtime
*****************************

The LA9310 provides 32 KB of program memory and 32 KB of data memory (16 KB VCPU bank + 16 KB IPPU bank). Two mechanisms are available to extend the effective memory usage:

- Change the VSPA image at runtime (software reset)
- Use program and data memory overlays


Changing the VSPA Image at Runtime (software reset)
---------------------------------------------------

For applications with multiple use cases, it can be useful to implement different VSPA firmwares with distinct memory layouts and code.
For example, IQ Player supports multiple configurations: 
- RX only
- TX only
- 1T1R
- 1T2R
- 1T4R

::

./load-la9310.sh   -> Start vspa on default /lib/firmware/apm-iqplayer-1T1R.eld   
./iq-swreset.sh -> Issue opcode to put VSPA in done state with all event cleared, config start as go handler
./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld -> Configure VSPA DMA to load vpram, vdram, idram . 
./stats.sh 0   -> Issue any command to wakeup vspa and restart on new image

Program and Data Memory Overlay
-------------------------------
An overlay mechanism is avaible to change only a portion of code or data at runtime.
In this approach, the user defines common program and data sections and manages dynamically overlaid sections.
Depending on the use case, the user loads the appropriate firmware code and data into the overlay regions.
For example, in a TDD application, the firmware may reload RX or TX code at each TDD transition.
la93xx_host_sw (Shiva) driver has support for overlay management 
Here how to load an overlay section using sysfs

::

  echo "Overlay_section_name" > /sys/devices/platform/soc@0/33800000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0/la9310sysfs/vspa_do_overlay

Known Issues & Workarounds
**************************

- Full-duplex 1T1R at 122.88 MSPS is currently limited to half-duplex
- 1T4R operation should enable no more than two RX channels to avoid FIFO overruns
- lib_iqplayer / iq_app must be started before traffic; stop/restart is not yet supported

Future Enhancements
*******************

- Improve full-duplex stability at 122.88 MSPS
- Add buffer metadata (timestamps)
- Add on-device signal generation and analysis
- Provide SoapySDR / GNU Radio APIs
- Port IQ Player to LA1224 (Layerscape Access family)

References
**********
 
- LA9310 documentation are available at <https://www.nxp.com/la9310> 
- LA9310 linux BSP documentation <https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/tree/la93xx>

