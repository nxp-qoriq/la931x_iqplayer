.. sectnum::

**IQ Player Software Reference Manual**
+++++++++++++++++++++++++++++++++++++++

Overview
********

IQ Player is a lightweight reference application that exercises the LA9310 baseband TX/RX
paths with raw time-domain I/Q samples. It targets SDR applications, RF testing and
calibration, provides TX/RX QEC, DC offset correction and decimation, and aims at the
maximum hardware throughput (sampling rate, PCIe bandwidth, channel count).

Key features:

- Continuous raw RX/TX streaming, file replay and capture, host streaming library
- Per-channel RX QEC and DC offset correction, TX QEC, saved and reapplied at start
- Live spectrum display with QEC tuning and IQ snapshot capture (``iq_app -X``)
- Statistics, cycle meters and traces for performance debugging

Supported configurations (VSPA DMA only, i.MX8MP RFNM):

 ============ ============================= =================================================
  Config       Rate                          Status
 ============ ============================= =================================================
  1T1R         61.44 MSPS                    full duplex
  1T1R         122.88 MSPS                   half duplex (RX only or TX only)
  1T2R         61.44 MSPS, x2 decimation     full duplex
  0T4R         4 x 61.44 MSPS, x2 decim.     RX only, fused QEC + decimation, lossless
  TX only      up to 160 MSPS                2 DDR read channels + mBurst
 ============ ============================= =================================================

Hardware Requirements
*********************

The RFNM board (LA9310 + i.MX8M Plus) is the NXP reference evaluation platform for LA9310.
For a recent Linux kernel and updates, use the RFNM SDK and run IQ Player on top of it,
see ``iqplayer_porting_guide_RFNM-SDK.rst``.
The discontinued NXP LA9310 Linux BSP for RFNM is described in the legacy
``iqplayer_quick_start_guide_RFNM.rst`` (reference only, no longer updated).

IQ Player runs on any LA9310-based setup and has been validated with the LimeSDR-Micro M.2
module (https://www.crowdsupply.com/lime-micro/limesdr-micro) on FRDM-IMX8MPLUS and
FRDM-IMX95
(https://www.nxp.com/design/design-center/development-boards-and-designs/frdm-development-boards:FRDM).
See ``iqplayer_porting_guide_LimeSDR-Micro.rst``.

Software Components
*******************

The data path is implemented in the VSPA firmware: ADC/DAC data flows on one side, DMA of
I/Q samples to and from host DDR on the other. Linux controls and operates IQ Player.

- **la93xx_host_sw (Shiva) driver**: loads and starts the VSPA firmware over PCIe. Included
  in the NXP Linux BSPs (https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/tree/la93xx).
- **LA9310 M4 FreeRTOS**: not required by IQ Player. The only constraint is that it must not
  use the VSPA mailbox registers, which are reserved for IQ Player. On NXP platforms
  (LimeSDR-Micro) it does the RF configuration and the AXIQ loopback, controlled with
  ``dpdk-dfe_app`` (https://github.com/nxp-qoriq/la93xx_freertos). On RFNM the RF
  configuration is done by Linux (``rfnm_daughterboard`` sysfs, ``txrx_en.sh``) and the AXIQ
  loopback is an M4 build option; see ``iqplayer_porting_guide_RFNM-SDK.rst``.
- **host-utils scripts**: shell scripts that send VSPA mailbox commands (start, stop, QEC,
  stats) (``host-utils/``).
- **lib_iqplayer / iq_app**: host library and sample application that stream samples
  between VSPA and host buffers, using DDR FIFOs with flow control (``host-utils/lib_iqplayer``,
  ``host-utils/iq_app``).
- **iq_mon**: statistics monitor (``host-utils/iq_monitor``).

Firmware Variants
*****************

Variants built by ``make``:

 ===================== ==================================================================
  Variant               Description
 ===================== ==================================================================
  ``1T1R``              1 TX + 1 RX
  ``1T2R``              1 TX + 2 RX, x2 decimation
  ``1T2R_DIAG``         1T2R + RX stall counters
  ``0T4R``              4 RX, fused QEC + x2 decimation, lossless at 4 x 61.44 MSPS
  ``0T4R_DIAG``         0T4R + stall/loss counters
  ``0T4R_PROF``         0T4R + per-stage cycle meter
 ===================== ==================================================================

``make install CONFIG=<variant>`` copies these ELDs and copies the selected one to ``apm.eld`` (a regular file, not a symlink).

Changing the VSPA image at runtime
----------------------------------

The LA9310 VSPA has 32 KB of program memory and 32 KB of data memory (16 KB VCPU bank +
16 KB IPPU bank). Use cases that do not fit in one image can switch firmware at runtime
without reloading the driver:

::

 ./load-la9310.sh                                        # or load-rfnm.sh: default image
 ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld # switch to 1T2R

``iq-reload-vspa.sh``:

1. sends the VSPA software reset (same mailbox as ``iq-swreset.sh``);
2. copies the ELD to the scratch buffer and DMAs ``.vpram``, ``.vdram`` and ``.idram`` into
   the VSPA; a DMA timeout stops the script with the section name and exit code 1;
3. copies the new image to ``/lib/firmware/apm.eld``, used by ``iq_trace``, ``iq_mon`` and
   the next ``load-*.sh`` (``-n`` leaves it unchanged);
4. performs the VSPA boot handshake (``0x70000000``, answered ``0xF0700000``; done by
   la9310shiva at driver load, otherwise it would consume the next mailbox), then sends the
   IQFLOOD proxy offset again (the new ``.vdram`` resets it) and checks that the VSPA echoes
   it. A missing handshake or echo stops the script with exit code 1.

``-v`` traces the commands. After the switch, use the ``iq-app-*.sh`` options of the new
variant (for example 2 RX channels with 1T2R).

Program and data memory overlay
-------------------------------

An overlay changes only part of the code or data at runtime: common sections stay resident
and overlay sections are loaded on demand (for example RX or TX code at each TDD
transition). The Shiva driver manages overlays through sysfs:

::

 echo "Overlay_section_name" > /sys/devices/platform/soc@0/33800000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0/la9310sysfs/vspa_do_overlay

Usage
*****

IQFLOOD region
--------------

All transfers use a host DDR buffer, the IQFLOOD region. It is declared in the device tree
and mapped over PCIe by la9310shiva through endpoint outbound windows. Its host physical
address and LA9310 (EP) address are given by ``la9310_modem_info``:

::

 root@imx95frdm:~/host_utils# la9310_modem_info
 ...
 Region           |      Host Phy Addr    |      Modem Phy Addr   |      Size
 IQ FLOOD phys    |      0x148000000      |      0xb0001000       |      0x2000000
 ...
 DAC Mask: 0x1  Rate: 61.44 MHz
 ADC-0: ON  Rate : 61.44 MHz

Layout used by the streaming scripts (``iq-app-tx.sh``, ``iq-app-rx.sh``):

- the last 1 KB holds the VSPA DMEM proxy (control and statistics, see VSPA design notes)
- the rest is split into ``N_TX + N_RX`` slots of ``(size - 1 KB) / (N_TX + N_RX)``
  bytes, 4 KB aligned
- slot 0 is the TX FIFO (when N_TX = 1), followed by one slot per RX channel

N_RX is read from the proxy (``rx_num_chan``), so the scripts adapt to the loaded firmware.

File replay and capture scripts
-------------------------------

These scripts use the IQFLOOD buffer directly, without a host application. Sizes are in
4 KB units.

::

 # TX: load a waveform at the start of IQFLOOD and replay it until stop
 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200
 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200 1   # 2 DDR read ch + mBurst

 # RX: one-shot or continuous capture into IQFLOOD
 ./iq-capture.sh ./iqdata.bin 1200
 ./iq-capture-ddr.sh 1200
 ./iq-stop.sh
 bin2mem -f iqdata.bin -a 0x9CC00000 -r 4915200

In multi-channel RX the buffer is split across channels and all captures start together.

Streaming with iq_app
---------------------

``iq_app`` streams between host memory and VSPA through DDR FIFOs in IQFLOOD, with
host flow control. It pins its threads (TX feeder, RX drain, UI) to separate cores so TX
and RX can run in parallel. Best results are obtained with a real-time host setup (CPU
isolation, reduced architectural timer interrupts).

TX (``iq-app-tx.sh``, default FIFO 256 KB, ``-f`` = DAC at 122.88 MSPS):

::

 ./iq-app-tx.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin   # file replay (background)
 ./iq-app-tx.sh -T                                        # interactive tone (foreground)

Tone keys: up/down frequency, left/right amplitude, ``w`` waveform, ``g/G p/P i/I j/J``
TX QEC gain/phase/DC, ``r`` reset QEC, ``s`` save QEC, ``q`` quit.

Example screen (1T1R, 61.44 MSPS, tone at Fs/8 = 7.68 MHz):

::

 +-----------------------------------------------------------------+
 | iq_app  Tone TX (LA9310)                                        |
 +-----------------------------------------------------------------+
 |  Waveform  : tone                                               |
 |  Frequency :     +7680.000 kHz  (up/dn:  960.000 kHz/step)      |
 |  Amplitude :  1.000              (lt/rt: 0.050 /step)           |
 +-----------------------------------------------------------------+
 |  TX QEC                                                         |
 +-----------------------------------------------------------------+
 |  Gain      :    +0.00 dB           ( g/G : step 0.1 dB  )       |
 |  Phase     :    +0.00 deg          ( p/P : step 0.5 deg )       |
 |  DC I      :   +0.000              ( i/I : step 0.01    )       |
 |  DC Q      :   +0.000              ( j/J : step 0.01    )       |
 +-----------------------------------------------------------------+
 |  Keys: up/dn=freq  lt/rt=amp  w=wave  r=rst  s=save  q=quit     |
 +-----------------------------------------------------------------+
 | TX spectrum  FFT=512   SR=61.440 MHz
 | ...
 |     8.640 MHz |                                                                                |  -90.11 dBFS
 |     7.680 MHz |############################################################################### |   -0.00 dBFS
 |     6.720 MHz |                                                                                |  -90.17 dBFS
 | ...
 +-----------------------------------------------------------------+

RX (``iq-app-rx.sh``, default FIFO 512 KB per channel, ``-f`` = ADC at 122.88 MSPS,
``-n`` overrides the auto-detected channel count):

::

 ./iq-app-rx.sh -D ./rx.bin 4915200      # capture 4915200 bytes per channel (background)
                                         # -> rx_ch0.bin, rx_ch1.bin ...
 ./iq-app-rx.sh -X                       # live spectrum (foreground)
 ./iq-app-rx.sh -X -W /tmp/rx 10ms       # spectrum, key w writes 10 ms per channel

RF tuning (RFNM): ``rf_tune`` in a second terminal sets the daughterboard gain, power,
frequency and path with range checks and TX/RX loop guards, see the RFNM porting guide
section "RF tuning with rf_tune".

Spectrum options: ``-N`` FFT size (8192), ``-M`` FFTs averaged (8), ``-H`` Hann window,
``-K/-B`` chart top/bottom dBFS, ``-L`` rows per channel, ``-C`` initial QEC channel.

Spectrum keys: ``c`` next QEC channel, ``g/G`` gain +-0.1 dB, ``p/P`` phase +-0.5 deg,
``i/I j/J`` DC I/Q +-0.01, ``r`` reset gain/phase, ``s`` save QEC, ``w`` snapshot,
``t`` spectrum / time-domain chart, ``+/-`` time zoom, ``a`` time scale auto / fixed,
``q`` quit.

Saturation: each channel has a line ``peak I <dB> Q <dB> dBFS  clip <n> (<%>)  crest <dB>``.
``clip`` counts samples with \|I\| or \|Q\| >= 32700. ``[CLIP]`` (red) means at least one
clipped sample in the refresh, ``[HIGH]`` (yellow) a peak above -3 dBFS. Crest factor is
about 3 dB for a clean tone and goes toward 0 dB when the signal is clipped. Lower the RX
gain (or TX power) until ``[CLIP]`` stays off; a peak around -6 dBFS is a good target.

Time-domain chart (key ``t``, like the I/Q plots of ``compute_tone_params.py``): newest
FFT block of each channel, ``*`` = I, ``o`` = Q, ``+`` = both, ``.`` = zero line.
The Y scale is automatic by default (smallest 1-2-5 step above the peak, e.g. +-0.05 FS
for a -29 dBFS signal); key ``a`` switches to a fixed +-1 FS scale where the top and
bottom rows (``=``) are the clip rails. The chart starts at 1 sample per column (80
samples, the actual I/Q points); ``-`` doubles the samples per column up to the whole
block, ``+`` goes back. Above 1 sample per column each column is the min..max of its
samples, so a single clipped sample is still visible.

Example screen (1T2R, 2 channels at 30.72 MSPS after x2 decimation):

::

 RX QEC ch 0/2  gain  -0.20 dB  phase  +0.50 deg  DC I +0.000  DC Q -0.030   keys: c=chan g/G p/P i/I j/J r=rst s=save w=snap t=time q=quit

 Ch0 (RxChan 0)  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
   RSSI -23.6  floor -92.6  DC -23.6 dBFS
   spurs: -81.7@+0.01 -82.2@-15.36 -84.8@+1.26 -85.5@+0.04 MHz
   peak I -17.9  Q -18.1 dBFS  clip 0 (0.000%)  crest 5.7 dB
   -6.2 |                                                                                |
  -12.5 |                                                                                |
  -18.8 |                                                                                |
  -25.0 |                                        |                                       |
  -31.2 |                                        |                                       |
  -37.5 |                                        |                                       |
  -43.8 |                                        #                                       |
  -50.0 |                                        #                                       |
  -56.2 |                                        #                                       |
  -62.5 |                                        #                                       |
  -68.8 |                                        #                                       |
  -75.0 |                                        #                                       |
  -81.2 |                                       |#                                       |
  -87.5 ||                   ||| | |           ||#  |          |  | |||                  |
  -93.8 ||| ||||||||###########################################################|||||||  ||
 -100.0 |################################################################################|
        ++-------------------+-------------------+-------------------+-------------------+
      -15.36               -7.68               +0.00               +7.68              +15.36       MHz
 Ch1 (RxChan 1)  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
   RSSI -24.8  floor -98.9  DC -24.8 dBFS
   spurs: -65.9@+7.68 -66.0@-7.68 -80.5@-15.36 -91.4@+0.16 MHz
   -6.2 |                                                                                |
  -12.5 |                                                                                |
  -18.8 |                                                                                |
  -25.0 |                                        |                                       |
  -31.2 |                                        |                                       |
  -37.5 |                                        |                                       |
  -43.8 |                                        |                                       |
  -50.0 |                                        #                                       |
  -56.2 |                                        #                                       |
  -62.5 |                                        #                                       |
  -68.8 |                    |                   #                   |                   |
  -75.0 |                    |                   #                   |                   |
  -81.2 ||                   |                   #                   |                   |
  -87.5 ||                   #                   #                   #                   |
  -93.8 ||                   #                  |# |        |    ||  #                   |
 -100.0 |#|||||||||############################################################|||||||||||
        ++-------------------+-------------------+-------------------+-------------------+
      -15.36               -7.68               +0.00               +7.68              +15.36       MHz

IQ snapshot (key ``w``): writes the next samples of all channels, contiguous from the key
press, to ``<base>_<rate>Msps_ch<N>_<YYYYmmdd_HHMMSS>.bin`` (cs16, rate e.g. ``30p72``)
while the spectrum keeps running; the status line reports the result. ``-W <base> [size]``
sets the base (default ``<script dir>/capture/rx``) and the size per channel in bytes or
``<N>ms`` (default ``1ms``).

Stop a background run with ``kill -INT <pid>`` (the capture is saved), then ``./iq-stop.sh``.
``kill -USR1 <pid>`` dumps the iq_app trace.

Flow control
------------

``iq-start-txfifo.sh`` and ``iq-start-rxfifo.sh`` start VSPA with the DDR buffers used as
FIFOs (the iq-app scripts call them). RX flow control is on by default in continuous
mode; setting bit ``0x00400000`` in the RX start mailbox MSB (for example ``0x06D00200``
instead of ``0x06900200``) disables it and VSPA overwrites the ring without waiting for the
host.

lib_iqplayer RX tuning (environment variables):

- ``IQ_RX_ACK_MIN``: consumed bytes before the consumed pointer is written back to VSPA
  DMEM (default ``0x8000``). Smaller values reopen the VSPA gate sooner at the cost of more
  PCIe writes.
- ``IQ_RX_KICK``: when non-zero (default 1), each ACK is followed by a mailbox-1 GO event so
  VSPA re-evaluates the gate immediately.

QEC and DC Offset
*****************

Kernels
-------

- 1T1R, 1T2R RX and all TX: ``txiqcomp()`` (IQ imbalance and DC offset); alternate
  ``txiqcomp_x32chf_5t()`` adds a 5-tap fractional delay.
- 0T4R RX: ``rx_qec_decx2_8t`` (la931x_vspa_common ``vspa-lib/rx_qec_decx2``), QEC fused
  with the x2 decimation, one call per channel and buffer. QEC is applied after decimation
  on the n/2 outputs, DC terms scaled by the tap sum. Functional (not bit-exact) versus
  the two-stage chain: SNR > 75 dB on test vectors, 99 cy vs 107 cy per 256-sample block.
  The config is rebuilt on stream start and refreshed incrementally on each QEC or DCO
  update, without sample loss.

Per-channel mailbox commands
----------------------------

Each firmware keeps one RX QEC set per RX channel (1T1R: 1, 1T2R: 2, 0T4R: 4) and one TX
set. ``MBOX_OPC_IQ_CORR`` (0x08) and ``MBOX_OPC_RX_DCO_CORR`` (0x0E) select the channel in
the message MSB word; the LSB carries the float32 value:

 ============= ============ ===========================================================
  MSB bits      msg bits     Meaning
 ============= ============ ===========================================================
  31-24         63-56        opcode (0x08 IQ_CORR, 0x0E RX_DCO_CORR)
  21            53           IQ_CORR: 1 = TX, 0 = RX
  20            52           IQ_CORR: reset the selected set(s) to passthrough
  18            50           apply to all channels (channel field ignored)
  17-16         49-48        channel 0-3
  15-0          47-32        IQ_CORR: coefficient index (FTAP1-3, 0xE DC_I, 0xF DC_Q)
 ============= ============ ===========================================================

Without bit 50 only the selected channel changes. A channel not present in the build is
rejected (reply LSB 0, NACK). TX accepts channel 0 or bit 50 only. Tools that leave bits
50-48 at 0 update channel 0 only.

Calibration, save and load
--------------------------

- Tune interactively with ``iq-app-rx.sh -X`` (RX) or ``iq-app-tx.sh -T`` (TX) and press
  ``s`` to save ``<qecdir>/qec_coeff_rx_ant<ch>.cfg`` or ``qec_coeff_tx_ant0.cfg``.
- ``imb2qec.py`` and ``imb2qec-2.py`` compute coefficients from measured imbalance
  (``--ch N`` or ``--all`` for RX).
- Saved files are applied at start: ``iq_app -r/-X`` loads every RX channel file,
  ``iq_app -t/-T`` the TX file, and the keys continue from the loaded values.
- The files hold coefficient values (f1, f4, f2, dcI, dcQ), not tap indexes, so one file is
  valid for every firmware.
- ``qecdir`` is ``<script dir>/qec`` for the scripts, whatever the current directory
  (``iq_app -Q <dir>``, default ``./qec``). ``iq-app-rx.sh -Q <dir>`` changes it and ``-q``
  skips loading; for ``iq-app-tx.sh`` use ``QEC_DIR`` / ``QEC_NOLOAD=1``.

Decimation and Data Format
**************************

Decimation kernels: ``decimator_2x_8_Taps_asm`` and ``decimator_4x_8_Taps_asm``. The default
is x2 with an 8-tap FIR, a balance between multi-RX support and VSPA cycles. 0T4R uses the
fused ``rx_qec_decx2_8t`` kernel.

The firmware works in 16-bit signed-magnitude (half-fixed); VSPA DMA converts to the 2's
complement int16 used by the host. Files are interleaved cs16 (I then Q, 4 bytes per
sample). MATLAB code is provided to generate and display binary waveforms; ``iq_app -P
<file>`` shows the spectrum of a capture file offline.

Performance
***********

Throughput
----------

 ====================== ========================================
  Rate (MSPS)            Bandwidth per channel (MB/s)
 ====================== ========================================
  61.44                  245.76
  122.88                 491.52
  160                    640
 ====================== ========================================

The maximum PCIe throughput measured on i.MX8MP RFNM is ~836 MB/s; actual performance
depends on the DMA engine and its configuration. IQ Player uses VSPA DMA.

VSPA DMA write to host DDR (RX)
-------------------------------

Inbound writes are limited to ~528 MB/s on i.MX8MP RFNM (hardware limitation, AN13164),
enough for 122.88 MSPS on one VSPA DMA channel; 160 MSPS needs a faster host such as
i.MX95. IQ Player uses one DMA write channel.

VSPA DMA read from host DDR (TX)
--------------------------------

The LA9310 AXI bus allows 4 outstanding reads, so several DMA channels increase throughput.
Measured on i.MX8MP RFNM:

 ========== ======================= ========= ===============================
  Config     Parallel DMA channels   mBurst    Bandwidth (MB/s)
 ========== ======================= ========= ===============================
  A          1                       off       200
  B          2                       off       393
  C          4                       off       590
  D          1                       on        481
  E          2                       on        836 (max)
  F          4                       on        836 (max)
 ========== ======================= ========= ===============================

C, E and F give the best TX throughput but starve the RX writes in full duplex (FIFO
overflow). The default (1 channel + mBurst) allows full duplex at 61.44 MSPS; 2 channels +
mBurst (``iq-replay.sh <file> <size> 1``) gives the TX-only maximum, up to 160 MSPS.

Host eDMA on RFNM i.MX8MP reaches ~700 MB/s with large transfers, but needs host-side DMA
agents synchronised with VSPA. IQ Player does not use it.

Cycle budget
------------

Each buffer must be processed before the next one arrives:

::

  budget (VSPA cycles per buffer) = N_samples x VSPA_clk / Fs

The VSPA runs at 4 x DCS_CLK. On RFNM, DCS_CLK comes from the Si5510 programmed by the
i.MX kernel (``i2c-rfnm-si5510.c``, 122.88 MHz; ``la9310rfnm`` may reclock it and prints
``reclocking dcs frequency``). ADC/DAC rates are DCS_CLK divided by a power of two, so
61.44 MSPS needs DCS = 122.88 MHz and VSPA = 491.52 MHz.

- 0T4R: 256 samples at 61.44 MSPS -> 2048 cycles
- 1T1R: 512-sample buffers, 2048 cycles at 122.88 MSPS and at 61.44 MSPS

The cycle meters count passes over ``RX_PASS_BUDGET``, ``TX_PASS_BUDGET`` and
``LOOP_PASS_BUDGET`` (default 2048 each; use 1024 for 256-sample buffers at 122.88 MSPS).

Measured results
----------------

0T4R (RFNM, 4 x 61.44 MSPS, flow control on and off): 4 x 245 MB/s AXIQ, 4 x 122 MB/s DDR
and application, lossless in steady state, live QEC updates without loss.

0T4R DMEM layout (RX_NUM_CHAN=4, RX_DMA_TXR_size=256, RX_DECIM=2; 4-deep AXIQ input ring,
3-deep decimated ring, 0x480 VCPU stack):

 ============== ======================== =============================================
  Memory bank    Symbol                   Notes
 ============== ======================== =============================================
  VCPU DMEM      proxy (0x0000-0x007F)    VSPA mailbox / dmem proxy
  VCPU DMEM      input_dec_buffer         4 ch x decimated ring x 128 smp x 4 B
  IPPU DRAM      rxiqcompcfg (0x4000)     QEC coefficient struct (4 channels)
  IPPU DRAM      input_buffer (0x4200)    4 ch x AXIQ ring x 256 smp x 4 B
 ============== ======================== =============================================

1T1R at 122.88 MSPS (RFNM i.MX8MP, stats cleared after start):

 ================================== ========= ========= =========================================
  Mode / TX DDR read config          RX MB/s   TX MB/s   Result
 ================================== ========= ========= =========================================
  RX only (``iq-capture-ddr.sh``)    491       -         lossless, loop max 1252 cy
  TX only, 2 ch + mBurst             -         491       lossless, loop max 1204 cy
  TX only, 1 ch + mBurst (default)   -         < 491     DDR read bandwidth too low (481 MB/s)
  Full duplex, 2 ch + mBurst         447       482       FIFO_RX_OVR, FIFO_TX_UDR
  Full duplex, 4 ch, no mBurst       213       485       FIFO_RX_OVR (worse)
  Full duplex, 1 ch + mBurst         467       430       DDR_WR_OVR, FIFO_RX_OVR, FIFO_TX_UDR
 ================================== ========= ========= =========================================

The cycle meters stay under budget in all full-duplex runs and PCIe has the bandwidth; the
limit is the VSPA DMA / AXI path carrying TX DDR reads, RX AXIQ reads and DDR writes
together. More read channels starve the RX AXIQ reads, fewer starve TX. Full duplex at
122.88 MSPS has been demonstrated with the PCIe DMA driven from the M7 host, outside the
IQ Player scope.

Debug and Monitoring
********************

iq_mon and statistics
---------------------

``iq_mon`` polls the statistics in the DDR proxy every second, adapts to the channel count
and computes bandwidth from the real elapsed time.

::

 RX stats :
  DMA_AXIQ_RD    0x000e7592(00000491 MB/s)       0x000e7593(00000491 MB/s)
  DMA_DDR_WR     0x000e7592(00000245 MB/s)       0x000e7592(00000245 MB/s)
  DDR_WR_OVR     0x00000000                      0x00000000
  FIFO_RX_UDR    0x00000000                      0x00000000
  FIFO_RX_OVR    0x00000000                      0x00000000
 TX stats :
  DMA_AXIQ_WR    0x00000000(00000000 MB/s)
  DMA_DDR_RD     0x00000000(00000000 MB/s)
  DDR_RD_UDR     0x00000000
  FIFO_TX_UDR    0x00000000
 Global stats :
  DMA_CFG_ERROR  0x00901003
  DMA_XFER_ERROR 0x00000000

::

 ./stats.sh -1        # clear VSPA stats
 ./iq_mon -c          # clear host stats

Cycle meters
------------

Shown by iq_mon, all reset at stream start:

- ``[VSPA RX cycle meter]``: longest and average streaming pass, pass count, passes over
  ``RX_PASS_BUDGET``, longest gap between two pass starts.
- ``[VSPA TX cycle meter]`` (1T1R, 1T2R): same for ``PUSH_TX_DATA()``, budget
  ``TX_PASS_BUDGET``.
- ``[VSPA loop meter]``: main loop pass from GO wakeup to ``__idle()`` (idle not counted),
  passes per second, budget ``LOOP_PASS_BUDGET``.
- ``TX in flight (last pass): DDR rd N buf, AXIQ wr N buf``.
- ``[VSPA RX stalls/s]`` and ``[VSPA RX loss check]`` (``_DIAG`` variants): per-channel
  stalls (host gate closed, DDR DMA busy, decimation backlog), measured VSPA clock and
  buffers/s versus nominal.

The meters live in ``t_stats.meter[16]`` ([0..5] TX, [6..9] loop, [10..12] budgets,
[13..14] TX in flight). ``t_stats`` is shared with the host: rebuild iq_mon, lib_iqplayer
and iq_app together with the firmware.

To fit in PRAM, 1T2R has no loop or TX meter, and 1T2R_DIAG has no RX pass meter either.

Build options
-------------

 =========================== ==============================================================
  Option                      Effect
 =========================== ==============================================================
  ``-DRX_STALL_STATS_ON``     stall and loss counters (``0T4R_DIAG``, ``1T2R_DIAG``)
  ``-DRX_CYCLE_METER_OFF``    remove the RX cycle meter
  ``-DTX_CYCLE_METER_OFF``    remove the TX cycle meter
  ``-DLOOP_CYCLE_METER_OFF``  remove the loop meter
  ``-DRX_STAGE_METER``        per-stage cycle sums in tx_stats[0..4] (``0T4R_PROF``)
  ``-DRX_HOT_TRACE``          per-buffer RX l1 traces in the streaming loop
  ``-DTX_HOT_TRACE``          per-buffer TX l1 traces
  ``-DRX_AXIQ_CHK_LIGHT``     AXIQ error check counts only, no l1 traces (0T4R)
  ``-DRX_IQ_SWAP=0``          disable the AXIQ RX I/Q swap (default 1, RFNM/LMS7002 fix)
 =========================== ==============================================================

Traces
------

- iq_app keeps a heap trace buffer; ``kill -USR1 <pid>`` dumps it.
- The VSPA firmware keeps an l1 trace in DMEM (40 entries by default), read with
  ``iq_trace``. Error traces are always built; per-buffer traces need ``*_HOT_TRACE``.

VSPA design notes
-----------------

- VSPA DMA: 128-bit AXI3, 3 engines, 16 channels, single burst per transaction, optional
  multi-burst (up to 4 x 256 B). Lower channel number = higher priority, channel 0 strict.
  One write engine, two or more read engines: spread reads and writes over channels.
- DMEM: spread load over VDMEM and IDMEM, align transfers to the bus width, align AXI
  origins to 4 KB. Access priority: DMA write, DMA read, AXI slave, IPPU, debug, VCPU.
- DMA completion: a FIFO-mode channel has 2 command slots but one sticky completion bit,
  so two completions can merge. RX and TX count transfers in flight from the DMA state
  (``dma_done_from_state()``) instead of the completion bit.
- PCIe: posted writes are preferred over reads to avoid initiator stalls. VSPA DMA-copies
  its read-only state and statistics (``s_vspa_dmem_proxy``) to the last 1 KB of IQFLOOD;
  writable fields (host flow control) are written by the host directly in DMEM at the
  offsets given by ``dmemProxyOffset``.
- A start command received while streaming restarts the stream (DDR write channels aborted,
  RX stats reset).

The development history (measurement sessions, root causes, removed variants) is kept in
``iqplayer_design_notes.rst``.

Known Issues and Limitations
****************************

- 1T1R at 122.88 MSPS is half duplex only; full duplex reaches about 90% of the rate (see
  Performance, Measured results). TX only needs 2 DDR read channels + mBurst (``iq-replay.sh <file> <size> 1``).
- 0T4R: a few AXIQ FIFO overruns are counted at stream start, before the host drain threads
  run; steady state is lossless.
- lib_iqplayer / iq_app must be started before traffic; stop/restart is not yet supported.

Future Enhancements
*******************

- Buffer metadata (timestamps)
- On-device signal generation and analysis
- SoapySDR / GNU Radio APIs
- Port to LA1224 (Layerscape Access family)

References
**********

- LA9310 documentation: https://www.nxp.com/la9310
- LA9310 Linux BSP: https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/tree/la93xx
- ``iqplayer_design_notes.rst``: development history and measurements
