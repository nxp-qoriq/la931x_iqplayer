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

- Channel configurations: 1T1R, 1T2R, 1T4R, 0T4R (RX-only)

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

RX flow control is enabled by default in continuous mode. Setting bit 0x00400000 in the
RX start mailbox MSB (e.g. ``0x06D00200`` instead of ``0x06900200``) disables it; the VSPA
then overwrites the DDR ring without waiting for the host.

lib_iqplayer RX flow-control tuning (environment variables):

- ``IQ_RX_ACK_MIN``: minimum number of consumed bytes before the consumed pointer is
  written back to VSPA DMEM (default ``0x8000``). Smaller values reopen the VSPA gate
  sooner at the cost of more PCIe writes.
- ``IQ_RX_KICK``: when non-zero (default 1), each ACK is followed by a mailbox-1 GO
  event so the VSPA wakes up and re-evaluates the gate immediately.

::

 # Use first half of iqflood region for TX FIFO i.e. 32KB fifo at offset 0 
 ./iq-app-tx.sh <input iq sample file> [FIFO Size]
 ex : ./iq-app-tx.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin

::

 # Use second half of iqflood region for TX FIFO i.e. 128KB fifo at offset iqfloodSize/2  
 ./iq-app-rx.sh <output iq sample file> <File size> [FIFO Size]
 ex : ./iq-app-rx.sh ./iqdata.bin 4915200

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
  Rates use the real elapsed time between samples; the screen is cleared on each refresh.
- ``[VSPA RX cycle meter]`` line (all RX builds unless ``-DRX_CYCLE_METER_OFF``): longest
  and average streaming pass in VSPA cycles, pass count, passes above the 2560-cycle
  budget and longest gap between two pass starts (all reset at stream start).
- ``[VSPA RX stalls/s]`` and ``[VSPA RX loss check]`` blocks (``0T4R_NOQEC_DIAG`` only):
  per-channel stall counters (host gate closed, DDR DMA busy, decimation backlog), measured
  VSPA clock and buffers/s versus nominal.
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
- 0T4R (RX-only, no TX resources allocated)

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

0T4R RX-Only Configuration
***************************

The 0T4R firmware variant allocates all four RX channels and no TX resources, maximising
DMEM space available for RX buffering. This section describes the DMEM layout, the AXIQ
timing budget, and the measured per-phase pipeline breakdown obtained from live iq_trace
captures on RFNM Blue (i.MX8MP, LA9310 @ 614.4 MHz, ADC @ 61.44 MSPS).

DMEM Layout
-----------

The 0T4R build (``Debug_IQPLAYER_0T4R``) uses the following DMEM allocation
(RX_NUM_CHAN=4, RX_NUM_BUF=3, RX_DMA_TXR_size=256 samples, RX_DECIM=2):

 ============== ======================== =========== =============================================
  Memory bank    Symbol                   Size (B)    Notes
 ============== ======================== =========== =============================================
  VCPU DMEM      proxy (0x0000-0x007F)         128    VSPA mailbox / dmem proxy
  VCPU DMEM      input_dec_buffer              6144    4 ch x 3 buf x 128 smp x 4 B
  VCPU DMEM      (free)                        ~9856   up to stack at 0x3F00
  IPPU DRAM      rxiqcompcfg (0x4000)           512    QEC coefficient struct (4 channels)
  IPPU DRAM      input_buffer (0x4200)        12288    4 ch x 3 buf x 256 smp x 4 B
  IPPU DRAM      (free)                        3584    remaining to 0x8000
 ============== ======================== =========== =============================================

AXIQ Timing Budget
------------------

At 61.44 MSPS and RX_DMA_TXR_size = 256 samples, the AXIQ DMA transfer window is:

::

  window = 256 samples / 61.44 MSPS  = 4.167 us
         = 4.167 us  x  614.4 MHz    = 2560 VSPA cycles

The complete RX pipeline (AXIQ completion -> next AXIQ re-arm) must fit within 2560 cycles
to avoid AXIQ FIFO overrun (RXOVR).

Measurement Session 1 -- Pre-fix (live iq_trace on RFNM Blue)
--------------------------------------------------------------

Platform: RFNM Blue, i.MX8MP host, LA9310 @ 614.4 MHz, ADC @ 61.44 MSPS.
Build: ``Debug_IQPLAYER_0T4R_NOQEC`` (flags: ``-DIQMOD_RX_0T4R -DIQMOD_RX_NO_QEC``).
Measurement instrument: ``L1_TRACE_MSG_DMA_XFR`` events inserted at the top of the
PUSH_RX_DATA() hot path; inter-event spacing read from live ``iq_trace`` capture.

**Result: loop period ~4310 cy (budget 2560 cy; 68% over).**

The loop was 1750 cy over budget. The primary contributor was the AXIQ overrun recovery
block: on every iteration the firmware detected an AXIQ FIFO overrun
(``L1_TRACE_MSG_DMA_AXIQ_RX_OVER``) and issued four ``axiq_fifo_rx_sr`` reads plus eight
``axiq_fifo_rx_cr`` writes (2 per channel x 4 channels), adding ~886 cy to the hot path.
This created a self-sustaining cascade: the slow loop caused RXOVR, which slowed the loop
further, which caused more RXOVR.

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ COMP + re-arm (4 channels)            ~700      completion poll + stream_read re-arm
  AXIQ overrun check + clear (4 channels)    ~886      axiq_fifo_rx_sr + 2x axiq_fifo_rx_cr per ch
  Decimation kernel (4 channels)             ~900      decimator_2x_8_Taps_asm, 4 ch sequential
  DDR WR check + enqueue (4 channels)        ~550      slot check + stream_write
  Dispatch / control overhead               ~1274      inter-phase branches, trace calls, counters
 ======================================= ============ ================================================
  **Total measured (DMA_XFR spacing)**    **~4310**   **1750 cy over the 2560-cycle budget**
 ======================================= ============ ================================================

Root Cause
----------

The ``axiq_fifo_rx_sr`` / ``axiq_fifo_rx_cr`` pair (AXIQ overrun detection and clear) is
required only when QEC is active to protect the QEC coefficient pipeline. In the NOQEC
build (``IQMOD_RX_NO_QEC``) it provides no benefit but adds ~886 cy per loop iteration
across 4 channels, pushing the loop well above the 2560-cycle AXIQ window and triggering
a self-reinforcing RXOVR cascade.

Two additional fixes were identified independently:

- **Fix 4** -- the DMA completion-check loop and the re-arm loop were implemented as two
  separate ``for`` loops over 4 channels, adding ~250 cy of redundant loop overhead.
- **Fixes 5/6/7** -- measurement-only ``l1_trace`` calls (ENTRY_MAIN, DEC_START/DEC_COMP,
  and ``_main.c`` dispatch traces) remain in the NOQEC hot path and account for ~550 cy.

Applied Fixes
-------------

- **Fix 1** (``iqmod_rx_2R_decx2x4.c``): The entire AXIQ overrun check/clear block is
  wrapped in ``#ifndef IQMOD_RX_NO_QEC``, removing it from the NOQEC hot path.
  *Note: superseded by Fix A v2 (see below), which reinstates a lightweight throttled
  AXIQ check covering all 4 channels with a single register read.*
- **Fix 2** (same file): The ``axiq_fifo_rx_cr`` double-write (set then clear) is also
  guarded, preventing the ~886 cy cost from appearing even in a partial-guard scenario.
  *Note: also superseded by Fix A v2.*
- **Fix 4** (same file): The two separate ``for`` loops (COMP-check and re-arm) are fused
  into a single pass per channel, saving ~250 cy of loop overhead.
  *Note: superseded by the two-loop structure (Fix B resolution, see below), which is
  required by VSPA2 DMA FIFO propagation latency constraints.*

Measurement Session 2 -- Post-fix Fix 1+2+4 (live iq_trace on RFNM Blue)
--------------------------------------------------------------------------

Same platform and build as Session 1. ELD rebuilt, deployed, and measured after applying
Fixes 1, 2, and 4.

**Result: loop period ~3124 cy, RXOVR = 0, throughput 234 MB/s.**

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ COMP + re-arm (fused, 4 channels)     ~700      single-pass fused loop; Fix 4 applied
  AXIQ overrun check + clear                    0      removed by Fix 1+2 (#ifndef IQMOD_RX_NO_QEC)
  Decimation kernel (4 channels)             ~900      unchanged
  DDR WR check + enqueue (4 channels)        ~550      unchanged
  Dispatch / trace overhead (residual)      ~974      ENTRY_MAIN + DEC traces + dispatch traces
 ======================================= ============ ================================================
  **Total measured (DMA_XFR spacing)**    **~3124**   **564 cy over the 2560-cycle budget**
 ======================================= ============ ================================================

The 234 MB/s result (vs. 245 MB/s target) corresponds to the 564 cy residual overage.
This residual is entirely due to measurement/trace instrumentation left in the NOQEC hot
path. The breakdown is:

- ``L1_TRACE_MSG_ENTRY_MAIN`` in PUSH_RX_DATA: ~50 cy (Fix 5: remove unconditionally).
- ``L1_TRACE_L1APP_RX_DEC_START`` + ``L1_TRACE_L1APP_RX_DEC_COMP`` x 4 channels: ~200 cy
  (Fix 6: wrap in ``#ifndef IQMOD_RX_NO_QEC``).
- ``_main.c`` dispatch trace calls x ~5 sites: ~300 cy
  (Fix 7: wrap in ``#ifndef IQMOD_RX_NO_QEC``).

Projected loop after Fixes 5+6+7: ~3124 - 550 = **~2574 cy**, within the 2560-cycle
budget, yielding the **~245 MB/s** throughput target.

Measurement Session 3 -- Post-fix Fix 5+6+7 (live iq_trace on RFNM Blue)
--------------------------------------------------------------------------

Same platform and build as Session 2. ELD rebuilt and deployed after applying
Fixes 5, 6, and 7.

**Result: loop period ~2490 cy, RXOVR = 0, throughput 245 MB/s.**

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ COMP + re-arm (fused, 4 channels)     ~700      unchanged from Session 2
  AXIQ overrun check + clear                    0      removed by Fix 1+2 (#ifndef IQMOD_RX_NO_QEC)
  Decimation kernel (4 channels)             ~900      unchanged
  DDR WR check + enqueue (4 channels)        ~550      unchanged
  Dispatch / trace overhead (residual)       ~340      ENTRY_MAIN removed (Fix 5); DEC_START/DEC_COMP
                                                       guarded (Fix 6); ENTRY_MAIN_DMASTAT + ENTRY_IDLE
                                                       guarded in _main.c (Fix 7)
 ======================================= ============ ================================================
  **Total measured (DMA_XFR spacing)**    **~2490**   **70 cy within the 2560-cycle budget**
 ======================================= ============ ================================================

The 245 MB/s result confirms all fixes are effective. RXOVR counter = 0 across
all 4 channels in 300 steady-state trace entries. The firmware operates within
the 2560-cycle AXIQ window with ~70 cy margin.

Fix A v2 -- Throttled AXIQ Overrun Check
-----------------------------------------

Session 3 removed the AXIQ overrun/underrun check entirely via ``#ifndef IQMOD_RX_NO_QEC``
(Fix 1+2). While correct for NOQEC builds, this leaves AXIQ hardware faults silently
undetected. Fix A v2 reinstates a lightweight safety net compatible with the NOQEC hot path:

- A single ``iord(AXIQ_SR0)`` read covers all 4 RX channels simultaneously (bits for
  RX0..RX3 are packed in one register). The original per-channel ``axiq_fifo_rx_sr`` loop
  (4 reads) is replaced by this single read.
- The check is throttled via a ``rxovr_check_throttle`` counter: the SR+CR sequence runs
  once every 16 PUSH_RX_DATA iterations.
- Amortised hot-path cost: ~70 cy (1 iord + branch-not-taken, amortised over 16 calls).
  Un-throttled cost if errors present: ~170 cy (includes per-channel stat increments + 2
  iowr to clear). Both are well within the 2560-cycle AXIQ window.
- RXOVR is detected within ~16 x 4.167 us = ~67 us of occurrence.
- The counter is pre-loaded to 16 in the capture start path to suppress a false RXUDR on
  the first iteration (FIFO just enabled; any stale underrun flag is a false positive).

This replaces Fix 1+2. Fix 1+2's complete removal of the AXIQ check is no longer needed
because the throttled single-read path is fast enough even in the NOQEC hot path.

Fix B Attempt and DMA FIFO Propagation Latency
-----------------------------------------------

After confirming Session 3 results, an optimisation attempt (Fix B) tried to further
reduce hot-path cost by hoisting all three DMA status register reads before the
completion/re-arm loop and fusing completion processing and re-arm into a single
per-channel pass (one loop instead of two). This introduced a regression:

**Observed regression:** ch2 ``DMA_RX_CMD_OVR`` incrementing continuously; ch3 running at
~132 MB/s (half-rate); ch0/ch1 unaffected. Error appeared immediately on first iq_mon
sample after reboot.

**Root cause -- VSPA2 DMA FIFO propagation latency:**
The VSPA2 DMA operates in FIFO mode (``DMAC_FIFO``): each channel has 2 command slots.
``DMA_FIFO_STAT`` bit for a channel is cleared when both slots are occupied and is set
again when a slot becomes free. The hardware mechanism is:

1. ``dmac_clear_complete(ch)`` writes to ``DMA_COMP_STAT`` to acknowledge a completed
   transfer. This is what frees the FIFO slot.
2. ``dmac_is_available(ch)`` reads ``DMA_FIFO_STAT`` to check whether a slot is free.

There is a pipeline propagation delay between step 1 and step 2: the freed slot does not
appear in ``DMA_FIFO_STAT`` immediately. The VSPA2 memory-mapped register bus requires
several pipeline cycles before the clear in ``DMA_COMP_STAT`` propagates into the
``DMA_FIFO_STAT`` read. When ``dmac_clear_complete(ch)`` and ``dmac_is_available(ch)``
are back-to-back in the same loop iteration (the fused Fix B approach), the FIFO slot
appears still occupied, ``stream_read`` is not issued, and the AXIQ FIFO overflows on
the next window. This was not visible on ch0/ch1 because those channels had already been
processed earlier in the loop and the propagation had time to settle.

**Fix B resolution -- two-loop structure:**

The proven fix reverts to two separate loops:

- **Loop 1** iterates over all 4 channels and calls ``dmac_clear_complete()`` and
  ``dmac_clear_event()`` for every channel that has completed. No ``is_available`` calls
  are made in this loop.
- **Loop 2** iterates over all 4 channels and calls ``dmac_is_available()``. By the time
  Loop 2 reaches channel 0, the entire body of Loop 1 (4 iterations of completion check,
  stat increment, error check) has executed, providing sufficient elapsed cycles for the
  HW to propagate all freed FIFO slots into ``DMA_FIFO_STAT``.

The minimum safe separation between ``dmac_clear_complete(ch_N)`` and
``dmac_is_available(ch_N)`` is satisfied by the ~3-4 loop-1 iterations that execute
between them (one full pass of Loop 1 completes before Loop 2 starts for channel 0).

Measurement Session 4 -- Post Fix A v2 + Two-Loop (live iq_trace on RFNM Blue)
--------------------------------------------------------------------------------

Same platform and build as Session 3. ELD rebuilt and deployed after applying Fix A v2
(throttled AXIQ check) and the two-loop structure (Fix B resolution).

**Result: loop period ~1900 cy (typical), RXOVR = 0, all 4 channels at 245 MB/s.**

Loop period extracted from consecutive ``L1_TRACE_MSG_ENTRY_IDLE`` timestamps in a live
``iq_trace`` capture (300 entries, steady-state streaming):

 ======================================================= ============ =============================================
  Iteration                                               Period (cy)  Notes
 ======================================================= ============ =============================================
  Typical (DMA completion present every call)              ~1900-1960  steady state; all 4 channels active
  Short (no DMA completion in this pass)                   ~1518-1580  FIFO slot not yet ready; loop exits early
  Budget                                                     2560      AXIQ window at 61.44 MSPS
  Margin                                                    ~600-1000  comfortable headroom
 ======================================================= ============ =============================================

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ overrun check (throttled)             ~70        1 iord(AXIQ_SR0); amortised over 16 iterations
  Loop 1: completion check (4 channels)     ~200        dmac_is_complete + clear_complete + clear_event
  Loop 2: re-arm (4 channels)               ~200        dmac_is_available + stream_read
  Decimation kernel (4 channels)            ~900        decimator_2x_8_Taps_asm, 4 ch sequential
  DDR WR check + enqueue (4 channels)       ~550        slot check + stream_write
  Dispatch / control overhead               ~80         counters, proxy update, branch targets
 ======================================= ============ ================================================
  **Total (typical)**                     **~2000**    **~560 cy within the 2560-cycle budget**
 ======================================= ============ ================================================

No ``DMA_RX_CMD_OVR``, ``FIFO_RX_UDR``, ``FIFO_RX_OVR``, or ``DMA_XFER_ERROR`` counters
increment across the full 300-entry trace window. The two-loop structure resolves the Fix B
regression without incurring any measurable throughput penalty versus the fused approach.

Increasing RX_NUM_BUF to 4 provides one extra DMEM slot for in-flight DMA:

- **0T4R_NOQEC** (no QEC struct): ``input_buffer`` = 4 x 4 x 256 x 4 = 16384 B fits
  exactly in IPPU DRAM (16384 B total). This variant uses RX_NUM_BUF=4.
- **0T4R with QEC**: ``rxiqcompcfg`` occupies 512 B of IPPU DRAM, leaving only 15872 B
  for ``input_buffer``; RX_NUM_BUF=4 (16384 B) does not fit. Limited to RX_NUM_BUF=3
  unless the QEC struct is relocated.

Measurement Session 5 -- 4R Streaming with Host Flow Control
---------------------------------------------------------------

Platform: RFNM, i.MX8MP host, LA9310 VSPA @ 614.4 MHz, 4 x RX @ 61.44 MSPS
(245 MB/s AXIQ per channel, 122.88 MB/s DDR per channel after x2 decimation).

**Problem: half-rate stall with flow control on.** The DDR write completion flag
(``DMA_COMP_STAT``) is a single sticky bit per channel. With two writes queued in the
2-entry DMA command FIFO, two completions can merge into one; ``la9310_fifo_produced_size``
then falls behind for good, the host sees less data than was written and the flow-control
gate closes early, halving throughput.

**Fix: DDR accounting from DMA state** (``RX_DDR_STATE_CMPL``, 0T4R default). Each pass,
for each channel with writes outstanding, the number of writes in flight is derived from
``XRUN`` and ``DMA_FIFO_STAT``:

- not running, FIFO entry free -> 0
- running, FIFO entry free -> 1
- no FIFO entry free -> 2

and ``produced = enqueued - in_flight * RX_DDR_STEP``. The produced count never moves back
and advances to the minimum of this pass and the previous pass, so a single transient read
cannot overestimate it.

**Cycle budget.** A bisect with flow control off (all variants at the same commit) showed
the accounting must stay cheap at 4R:

 ======================================= ============================================
  Variant                                 Result (FC off)
 ======================================= ============================================
  Legacy completion bit                   lossless (but half-rate stall with FC on)
  State accounting, 1 write in flight,    ch3 (serviced last) starves: DMEM ring full,
  3 status reads + completion read        AXIQ FIFO overrun
  Same with 2 writes in flight            ~5x fewer events, still overruns
  2 writes in flight, 2 status reads      lossless (retained default)
 ======================================= ============================================

**Result (default 0T4R_NOQEC):** pass ~1780 cy average, 1866 cy max (2560 cy budget,
none above). 4 x 245 MB/s AXIQ, 4 x 122 MB/s DDR and app. Lossless in steady state with
flow control on and off (iq_app ``-X`` and ``-D``), no ``DDR_WR_OVR`` increase.

Build options (``iqmod_rx_2R_decx2x4.c``):

 ========================== ==============================================================
  Option                     Effect
 ========================== ==============================================================
  ``RX_DDR_INFLIGHT``        max DDR writes in flight per channel (1 or 2, default 2)
  ``-DRX_DDR_STATUS_FULL``   3 status reads + completion read (diagnostic, too slow at 4R)
  ``-DRX_DDR_LEGACY_CMPL``   legacy completion-bit accounting
  ``-DRX_STALL_STATS_ON``    stall and loss counters (used by ``0T4R_NOQEC_DIAG``)
  ``-DRX_CYCLE_METER_OFF``   remove the cycle meter
  ``-DRX_STAGE_METER``       per-stage cycle sums in tx_stats[0..4] (with cycle meter)
  ``-DRX_HOT_TRACE``         per-buffer l1 traces in the streaming loop
  ``-DRX_REVERSE_ORDER``     service channel 3 first (``0T4R_REV`` target)
 ========================== ==============================================================

Firmware variants built by ``make``: ``0T4R_NOQEC`` (default), ``0T4R_NOQEC_DIAG``
(stall/loss counters), ``0T4R_REV`` (reverse channel order, diagnostic).

A start command received while already streaming now restarts the stream instead of
returning a NACK; the DDR write channels are aborted and the RX stats reset at each start.

1T1R RX accounting fix
~~~~~~~~~~~~~~~~~~~~~~

``iqmod_rx.c`` (1T1R) used ``rx_vspa_proxy[0].la9310_fifo_consumed_size`` as its own
DDR-done counter. Since libiqplayer writes that field directly in VSPA DMEM
(``rx_fc_addr``), every host ACK moved the counter back, the QEC ring looked full and RX
stalled with ``DDR_WR_OVR`` and ``FIFO_RX_OVR`` counting on every pass (``-X`` and ``-D``).

- The firmware keeps its own counters; the host consumed field is only read, for flow control.
- ``la9310_fifo_produced_size`` is raised on DDR write completion (data in DDR), not on compress.
- DDR completion is counted from DMA state (writes in flight), as in 0T4R.
  ``-DRX_DDR_LEGACY_CMPL`` keeps the completion-bit method.
- Start and stop abort the DDR write DMA and clear produced.

Verified on RFNM: 1T1R iq-app TX and RX (``-X`` and ``-D``); 1T2R iq-app TX and RX
(``-X`` and ``-T``).

1T1R and 1T2R streaming options
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- 1T2R uses the same DMA-state DDR accounting as 0T4R (``-DRX_DDR_LEGACY_CMPL`` to disable).
- ``1T2R_DIAG`` target: 1T2R with ``-DRX_STALL_STATS_ON``. Only the rx_stats stall counters
  (``APP_DDR_WR``: host gate closed, ``EXT_DDR_WR``: DDR DMA busy, ``EXT_DDR_WR_OVR``:
  decimation blocked) are built; ``tx_stats`` keeps the real TX statistics.
- 1T1R has the RX cycle meter (``[VSPA RX cycle meter]`` line in iq_mon,
  ``-DRX_CYCLE_METER_OFF`` removes it).
- ``RX_PASS_BUDGET`` (default 2560) sets the cycle threshold of the "passes over budget"
  count: 2560 cy at 61.44 MSPS, 1280 cy at 122.88 MSPS.
- Per-buffer l1 traces in the 1T1R streaming loop and in ``_main.c`` are only built with
  ``-DRX_HOT_TRACE`` (all variants); error traces are kept.
- TX cycle meter (1T1R, 1T2R): times each streaming ``PUSH_TX_DATA()`` pass and prints
  ``[VSPA TX cycle meter]`` in iq_mon (max pass, average over the last period, pass count,
  passes over ``TX_PASS_BUDGET`` (default 2560), longest start-to-start gap).
  ``-DTX_CYCLE_METER_OFF`` removes it.
- Loop meter (all variants): times each main loop pass from GO wakeup to ``__idle()``
  (idle time is not counted). It prints ``[VSPA loop meter]`` with max, average, pass count,
  passes per second, and passes over ``LOOP_PASS_BUDGET`` (default 2560).
  ``-DLOOP_CYCLE_METER_OFF`` removes it.
- The meters are stored in ``t_stats.meter[16]``: [0..5] TX, [6..9] loop, [10..12] RX/TX/loop
  budgets. iq_mon uses the budgets in its labels. ``t_stats`` is shared with the host, so
  iq_mon, lib_iqplayer and iq_app must be rebuilt together with the firmware.
- Per-buffer TX l1 traces (AXIQ TX start/comp, DDR RD start/comp, QEC start/comp) are only
  built with ``-DTX_HOT_TRACE``.
- 1T2R memory headroom is small: the default l1 trace size is 40 entries (it was 50), and
  ``1T2R_DIAG`` is built without the TX and loop meters to fit in PRAM.

1T1R TX DMA completion accounting
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

At 122.88 MSPS TX only, with 2 DDR read channels and mBurst (``iq-replay.sh <file> <size> 1``),
the TX rate started at 491 MB/s and dropped to 327 MB/s after a few seconds, with
``DDR_RD_UDR`` and ``FIFO_TX_UDR`` counting. The cycle meters were well under budget.

Cause: each FIFO-mode DMA channel has 2 command slots but a single sticky completion bit.
When two transfers complete between two polls, only one completion is seen. TX counted
fetched DDR reads and consumed AXIQ writes from that bit, so each merge lost one buffer for
good and the TX rings looked fuller than they were.

Fix (``iqmod_tx.c``, default): the number of transfers still in flight is derived from the
DMA state, as for RX (``dma_done_from_state()`` in ``_main.c``): no FIFO slot free -> 2,
slot free and running -> 1, idle -> 0, and done = enqueued - in_flight. Done never moves
back and advances to the minimum of this pass and the previous pass.
``-DTX_DDR_LEGACY_CMPL`` restores the completion-bit counting.

- iq_mon prints ``TX in flight (last pass): DDR rd N buf, AXIQ wr N buf``
  (``t_stats.meter[13..14]``).
- 1T2R and ``1T2R_DIAG`` use the same TX accounting. To fit in PRAM the TX DDR read load
  test (start bit ``0x00200000``) is only built with ``-DTX_DDR_LOAD_TEST`` (otherwise the
  request is NACKed), and the ``dbg_gbl`` debug spin loops only with ``-DDBG_SPIN_ON``.

Verified on RFNM, 1T1R, TX only at 122.88 MSPS with 2 read channels + mBurst:
491 MB/s steady, no ``FIFO_TX_UDR``, TX pass max 501 cy, loop max 1204 cy.

1T1R at 122.88 MSPS
~~~~~~~~~~~~~~~~~~~

1T1R buffers are 512 samples (2 KB), so the time budget per buffer is 2560 VSPA cycles at
122.88 MSPS as well as at 61.44 MSPS (the default ``*_PASS_BUDGET`` values apply).

Measured on RFNM (i.MX8MP), 1T1R firmware, stats cleared after start:

 ================================== ========= ========= =========================================
  Mode / TX DDR read config          RX MB/s   TX MB/s   Result
 ================================== ========= ========= =========================================
  RX only (``iq-capture-ddr.sh``)     491       -         lossless, loop max 1252 cy
  TX only, 2 ch + mBurst              -         491       lossless, loop max 1204 cy
  TX only, 1 ch + mBurst (default)    -         < 491     DDR read bandwidth too low (481 MB/s)
  Full duplex, 2 ch + mBurst          447       482       FIFO_RX_OVR, FIFO_TX_UDR
  Full duplex, 4 ch, no mBurst        213       485       FIFO_RX_OVR (worse)
  Full duplex, 1 ch + mBurst          467       430       DDR_WR_OVR, FIFO_RX_OVR, FIFO_TX_UDR
 ================================== ========= ========= =========================================

In all full-duplex runs the VSPA cycle meters stay under budget (pass max < 610 cy,
start-gap max < 1810 cy, loop average ~1230 cy for a 2560 cy buffer time). PCIe is full
duplex and has the bandwidth for 491 MB/s in each direction. The limit is the VSPA DMA /
AXI path carrying both the TX DDR reads and the RX AXIQ reads plus DDR writes: more read
channels in flight starve the RX AXIQ reads, fewer starve TX and slow the DDR writes.

- With IQ Player (VSPA DMA only), 122.88 MSPS is supported in half duplex: RX only with the
  default configuration, TX only with 2 DDR read channels + mBurst
  (``iq-replay.sh <file> <size> 1``). Full duplex reaches about 90% of the rate.
- Full duplex 1T1R at 122.88 MSPS has been demonstrated with the I/Q data moved by the PCIe
  DMA driven from the M7 host, which takes the DDR traffic off the VSPA DMA/AXI path.
  This mode is outside the IQ Player scope and is not implemented here.

Known Issues & Workarounds
**************************

- 1T1R at 122.88 MSPS with IQ Player is half duplex only: full duplex reaches about 90% of
  the rate (VSPA DMA/AXI read and write contention, see "1T1R at 122.88 MSPS"). Full duplex
  requires the host PCIe DMA (demonstrated from the M7 host, not part of IQ Player).
  TX only needs 2 DDR read channels + mBurst (``iq-replay.sh <file> <size> 1``).
- 1T4R operation should enable no more than two RX channels to avoid FIFO overruns
- 0T4R with QEC enabled experiences AXIQ RXOVR at 61.44 MSPS: the 4-channel pipeline
  (3414 cy) exceeds the 2560-cycle AXIQ window; workaround is to use the 0T4R_NOQEC
  firmware variant which supports RX_NUM_BUF=4 early re-arm
- 0T4R_NOQEC: a few AXIQ FIFO overruns are counted at stream start, before the host drain
  threads run; steady state is lossless
- lib_iqplayer / iq_app must be started before traffic; stop/restart is not yet supported

Future Enhancements
*******************

- Add buffer metadata (timestamps)
- Add on-device signal generation and analysis
- Provide SoapySDR / GNU Radio APIs
- Port IQ Player to LA1224 (Layerscape Access family)

References
**********
 
- LA9310 documentation are available at <https://www.nxp.com/la9310> 
- LA9310 linux BSP documentation <https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/tree/la93xx>

