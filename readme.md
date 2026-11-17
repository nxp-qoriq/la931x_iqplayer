# LA9310 iqplayer repository

la931x_iqplayer is a basic application to stream time domain IQ samples with the LA9310 SoC.
It includes the VSPA firmware source code, host Linux scripts to control the VSPA firmware,
and a host Linux library and application example.

# Documentation

- `doc/iqplayer_reference_manual.rst`: features, firmware variants, usage, QEC, performance, debug
- `doc/iqplayer_porting_guide_RFNM-SDK.rst`: RFNM board with the RFNM SDK (recommended for RFNM, recent Linux kernel)
- `doc/iqplayer_porting_guide_LimeSDR-Micro.rst`: LimeSDR-Micro on FRDM-IMX8MPLUS / FRDM-IMX95 / Layerscape
- `doc/iqplayer_quick_start_guide_RFNM.rst`: legacy, discontinued NXP LA9310 BSP for RFNM (reference only)
- `doc/iqplayer_design_notes.rst`: development history and measurements
- `doc/release_notes.md`: release notes
- `doc/patches/`: temporary patches for other repositories, until the upstream pull requests
  are merged. See `doc/patches/README.md` for which folders to use:
  - NXP BSP (LimeSDR-Micro): `la93xx_host_sw`, `la93xx_freertos`, `dpdk`
  - RFNM SDK: `la9310-driver`, `la9310-freertos`
  - both, optional VSPA debugger (6G for All `la9310_debug`): `la9310_debug`

Release v1.2 was prepared with the help of Vero Studio (AI coding assistant); changes
were reviewed by the maintainer and the firmware was tested on the boards.

# Requirements

- This firmware uses VSPA kernels from https://github.com/nxp-qoriq/la931x_vspa_common
- The host-utils need two header trees, passed with `LA9310_COMMON_HEADERS` and `UAPI_DIR`
  (defaults: `../la93xx_freertos/common_headers` and `../la93xx_host_sw/uapi`):
  - NXP platforms (LimeSDR-Micro): https://github.com/nxp-qoriq/la93xx_freertos and
    https://github.com/nxp-qoriq/la93xx_host_sw
  - RFNM: https://github.com/rfnm/la9310-freertos and https://github.com/rfnm/la9310-driver
    with `doc/patches/` applied (`uapi/la9310_modinfo.h` comes from the patches),
    see `doc/iqplayer_porting_guide_RFNM-SDK.rst`
- Optional: Arm RAN Acceleration Library (https://gitlab.arm.com/networking/ral), used by
  `iq_app` for the live spectrum FFT. Without it (`RAL_DIR`, default `../ral`) the build
  prints a warning and `iq_app` uses a slower DFT. Cross build:
  `cmake -B build_aarch64 -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DARMRAL_ARCH=NEON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release && make -C build_aarch64 armral`
- The Linux VSPA tool chain is freely available with the eval version of CodeWarrior for VSPA 10.3.0
  or at https://www.nxp.com/lgfiles/sdk/la1224/imx-la9310-sdk-10/cwvspa.vbeta_14_00_781_vspa.linux.tgz

# Contents

- `iqplayer_cwproj`: source code, Makefile and CodeWarrior project for the VSPA firmware variants
  1T1R, 1T2R, 0T4R (CW build configurations `Debug_IQPLAYER_<variant>`, same ELDs as `make`)
- `host-utils`: Linux user space utilities
  - shell scripts to load, start and stop transfers: `load-limesdr-micro.sh` / `load-rfnm.sh`, `iq-replay.sh`,
    `iq-capture.sh`, `iq-capture-ddr.sh`, `iq-app-tx.sh`, `iq-app-rx.sh`, `iq-stop.sh`,
    `iq-reload-vspa.sh`
  - RX configuration: `rx-chan.sh` (channel order, `-r` read back), `rx-iqswap.sh`,
    `iq-board-defaults.sh` (board defaults), `iq-proxy-cfg.sh` (proxy config word helper)
  - `iq_mon`: statistics monitor for the DMA channels and FIFOs, with VSPA cycle meters
  - `iq_trace`: dumps VSPA trace information to debug or profile the firmware
  - `iq_app` / `lib_iqplayer`: application example streaming to/from the upper stack,
    with live spectrum, QEC tuning and IQ snapshot
  - `imb2qec.py` / `imb2qec-2.py`: compute and apply QEC coefficients from measured imbalance

## Compile iqplayer firmware and utilities

Set the compiler install path (default `VSPA_TOOL ?= /opt/VSPA_Tools_vbeta_14_00_781`)
```sh
export LA9310_COMMON_HEADERS=<freertos tree>/common_headers
export UAPI_DIR=<driver tree>/uapi
make
make install
scp -r install/* root@<targetIP>:/
```

## Usage - Play/Capture waveforms from/to binary files

Load and start the VSPA firmware
```sh
 ./load-limesdr-micro.sh      # RFNM: ./load-rfnm.sh
```

Switch firmware variant at runtime (VSPA reset, DMA load, `apm.eld` copy and proxy
offset are done by the script)
```sh
 ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld
```

Play (repeat) a waveform from a file
```sh
 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200
```

Capture (one time) IQ samples into a file
```sh
 ./iq-capture.sh ./iqdata.bin 1200
```

Capture (repeat) into a DDR buffer
```sh
 ./iq-capture-ddr.sh 1200
 ./iq-stop.sh
 bin2mem -f iqdata.bin -a 0x9CC00000 -r 4915200
```

## RF configuration and AXIQ loopback

NXP platforms (LimeSDR-Micro): the RF chain and the AXIQ digital loopback are controlled
by the M4 firmware through `dpdk-dfe_app`
```sh
 mount -t hugetlbfs none /dev/hugepages
 echo 24 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 dpdk-dfe_app -c "lime tx 1 freq 1200000000"
 dpdk-dfe_app -c "lime rx 1 freq 1210000000"
 dpdk-dfe_app -c "axiq_lb enable"
```

RFNM: the RF chain is configured by Linux through the `rfnm_daughterboard` sysfs
(`/sys/kernel/rfnm_primary/{tx0,rx0,rx1}`), directly or with `txrx_en.sh`
```sh
 ./txrx_en.sh limerx        # limetx, limerx, granitatx, granitatxrx, granita2R
 echo 1200000000 > /sys/kernel/rfnm_primary/rx0/freq
 echo 1 > /sys/kernel/rfnm_primary/rx0/apply
```
`dpdk-dfe_app` is not used on RFNM: AXIQ loopback needs an M4 firmware built with
`-DAXIQ_LOOPBACK=ON`, see `doc/iqplayer_porting_guide_RFNM-SDK.rst`.

## Usage - iq_app TX

Default 256 KB TX FIFO in DDR, `-f` = DAC at 122.88 MSPS
```sh
 ./iq-app-tx.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin   # file replay (background)
 ./iq-app-tx.sh -T                                        # interactive tone and TX QEC (foreground)
 ./iq-stop.sh
```

The panel shows the tone bin and its two neighbours; the bar length follows the linear
amplitude (`-l` for a dB bar). Example, tone at 6.72 MHz, amplitude 0.2:
```sh
+-----------------------------------------------------------------+
| iq_app  Tone TX (LA9310)                                        |
+-----------------------------------------------------------------+
|  Waveform  : tone                                               |
|  Frequency :     +6720.000 kHz  (up/dn:  960.000 kHz/step)      |
|  Amplitude :  0.200              (lt/rt: 0.050 /step)           |
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
|     7.680 MHz |                                                                                | -103.98 dBFS
|     6.720 MHz |###############                                                                 |  -13.98 dBFS
|     5.760 MHz |                                                                                | -104.18 dBFS
| ...
+-----------------------------------------------------------------+
```

The TX start is refused when the loaded firmware has no TX channel (0T4R):
`iq-app-tx.sh` and `iq-replay.sh` stop with an error, load 1T1R or 1T2R first.

## RX channel mapping

`rx-chan.sh` selects the physical RX channel (mbx chanID) of each slot; the load scripts
set the board default (LimeSDR-Micro: `rx-chan.sh 3 1 2 0`). `-r` reads back the
configuration published by the VSPA:
```sh
 ./rx-chan.sh -r
 proxy cfg 0x02130202: RX 2  IQ swap 0  TX 1  board limesdr-micro
  slot 0 -> chanID 3  RX0  LMS7002M RX1 (ch A)
  slot 1 -> chanID 1  RX1  LMS7002M RX2 (ch B)
```

## Usage - iq_app RX

Default 512 KB RX FIFO per channel in DDR, channel count read from the loaded firmware
```sh
 ./iq-app-rx.sh -D ./rx.bin 4915200      # capture 4915200 bytes per channel -> rx_ch<N>.bin
 ./iq-app-rx.sh -X                       # live spectrum with RX QEC keys
 ./iq-app-rx.sh -X -W /tmp/rx 10ms       # key w writes a 10 ms IQ snapshot per channel
 kill -INT <iq_app PID>                  # stop a background capture (file is saved)
 ./iq-stop.sh
```

Each channel header is `Ch<slot> [chan <mbx chanID> <LA9310 input> | <board path>]  IQ swap <n>`.
Example: 1T2R on LimeSDR-Micro, one antenna on LMS7002M RX1 (channel A); RX2 (channel B)
has no antenna, so it is about 20 dB lower.

Spectrum view:
```sh
RX QEC ch 0/2  gain  +0.00 dB  phase  +0.00 deg  DC I +0.000  DC Q +0.000   keys: c=chan g/G p/P i/I j/J r=rst s=save w=snap t=time q=quit

Ch0 [chan 3 RX0 | LMS7002M RX1 (A)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
  RSSI -7.1  floor -78.2  DC -28.3 dBFS
  spurs: -9.0@-3.28 -23.9@-10.00 -48.3@-3.40 -49.9@-3.42 MHz
  peak I -5.4  Q -5.0 dBFS  clip 0 (0.000%)  crest 5.1 dB
  -6.2 |                                                                                |
 -12.5 |                               |                                                |
 -18.8 |                               |                                                |
 -25.0 |             |                 |                                                |
 -31.2 |             |                 #        |                                       |
 -37.5 |             |                 #        |                                       |
 -43.8 |             #                 #        |                                       |
 -50.0 |        |    #|                #        #                                       |
 -56.2 |        |    #|               |#|       #                                       |
 -62.5 |        |    ##              |###|      #                                   |   |
 -68.8 |        #    ##            |#######|    #       |                           |   |
 -75.0 |        #|  |###|||||||################|#|||||| || ||| | |        |         |   |
 -81.2 |||||||||##############################################################||||||#|| |
 -87.5 |################################################################################|
 -93.8 |################################################################################|
-100.0 |################################################################################|
       ++-------------------+-------------------+-------------------+-------------------+
     -15.36               -7.68               +0.00               +7.68              +15.36       MHz
Ch1 [chan 1 RX1 | LMS7002M RX2 (B)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
  RSSI -27.9  floor -83.9  DC -28.5 dBFS
  spurs: -39.3@-3.28 -55.5@-10.00 -60.9@-11.92 -67.7@+3.28 MHz
  peak I -24.8  Q -24.8 dBFS  clip 0 (0.000%)  crest 6.1 dB
  -6.2 |                                                                                |
 -12.5 |                                                                                |
 -18.8 |                                                                                |
 -25.0 |                                                                                |
 -31.2 |                                        |                                       |
 -37.5 |                                        |                                       |
 -43.8 |                               |        |                                       |
 -50.0 |                               |        #                                       |
 -56.2 |             |                 |        #                                       |
 -62.5 |        |    |                 #        #                                       |
 -68.8 |        |    |                 #        #       |                               |
 -75.0 |        |    #|                #        #       |                               |
 -81.2 |        #| ||#|||||||||||||||||#||||||||#|||||||#||||||||||||||||||||           |
 -87.5 ||||||######################################################################||||||
 -93.8 |################################################################################|
-100.0 |################################################################################|
       ++-------------------+-------------------+-------------------+-------------------+
     -15.36               -7.68               +0.00               +7.68              +15.36       MHz
```

Time view (key `t`; `*` = I, `o` = Q, `+` = both, `.` = zero line):
```sh
Ch0 [chan 3 RX0 | LMS7002M RX1 (A)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
  RSSI -7.1  floor -78.2  DC -28.3 dBFS
  spurs: -9.0@-3.28 -24.0@-10.00 -49.5@-3.42 -50.1@-11.92 MHz
  peak I -5.5  Q -5.0 dBFS  clip 0 (0.000%)  crest 5.1 dB
+1.000 |                                                                                |
+0.875 |                                                                                |
+0.750 |                                                                                |
+0.625 |                                                                                |
+0.500 |       o*       o**      o**      oo*       o*       o        o  *     oo **    |
+0.375 |*     o* *     o   *    o o *      * **    o  **    o +***     +* *      *      |
+0.250 | *   o    *   o *o  *        *   o          *o  *            o          *o  *   |
+0.125 |....o...o..*.o.........o........o...o..*..o........o.*..........o.....o.........|
+0.000 |  *   *               o  * o      *      o    o        o    o *  o * o    o    o|
-0.125 |     *   o     *  o  *  *   o *o     o     *      o     o *o         *         *|
-0.250 |    +**    o +* *      **       +*     o o* *      +        o      o o+ *   o  o|
-0.375 |  o            *      o  *    o   *        *     oo **    oo **     o  *     oo |
-0.500 |   o        o        o         o        o                                       |
-0.625 |                                                                                |
-0.750 |                                                                                |
-0.875 |                                                                                |
       ++-------------------+-------------------+-------------------+-------------------+
        0 .. 2.604 us, 80 samples, 1/col  scale +-1.00 FS (auto)  keys: +/- zoom, a=auto/fixed, t=spectrum
Ch1 [chan 1 RX1 | LMS7002M RX2 (B)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
  RSSI -27.9  floor -83.9  DC -28.5 dBFS
  spurs: -39.3@-3.28 -55.7@-10.00 -61.1@-11.92 -67.4@+3.28 MHz
  peak I -24.9  Q -24.8 dBFS  clip 0 (0.000%)  crest 6.1 dB
+0.100 |                                                                                |
+0.087 |                                                                                |
+0.075 |                                                                                |
+0.062 |                                                     o                          |
+0.050 |      o                 ooo                oo       o         oo       oo       |
+0.038 |    oo oo    ooooo    oo   oo  ooooo     oo  o    oo  oo    oo        o  oo    o|
+0.025 |oo o     o  o     oo         oo     ooooo     oooo      oo o    oo  oo     oooo |
+0.013 |  o       oo        oo                                    o       oo            |
+0.000 |        **      **         *       *         **                            *    |
-0.013 | *     *  *       ***    ** **      ***        **    ****     *****     *** *   |
-0.025 |* * ***        *     *  *     * ***    *  ***     **     **  *     * * *     * *|
-0.038 |   *       ****       **       *        **       *  *      **       * *       * |
-0.050 |                                                                                |
-0.063 |                                                                                |
-0.075 |                                                                                |
-0.087 |                                                                                |
       ++-------------------+-------------------+-------------------+-------------------+
        0 .. 2.604 us, 80 samples, 1/col  scale +-0.10 FS (auto)  keys: +/- zoom, a=auto/fixed, t=spectrum
```


## QEC

- Press `s` in `iq-app-rx.sh -X` / `iq-app-tx.sh -T` to save the QEC to `<script dir>/qec`.
  Saved files are applied at the next start, for every firmware variant
  (`iq-app-rx.sh -Q <dir>` / `-q`, `iq-app-tx.sh` `QEC_DIR` / `QEC_NOLOAD=1`).
- `imb2qec.py <tx|rx> <gain_dB> <phase_deg> <dc_re> <dc_im> [--ch N | --all]`
- Since v1.2 the AXIQ RX FIFOs swap I/Q (RX_IQ_SWAP=1, RFNM/LMS7002 mirrored spectrum fix).
  Re-run the RX QEC calibration after updating: previous phase/DC values change sign.
