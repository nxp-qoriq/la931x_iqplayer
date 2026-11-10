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
- `doc/patches/`: patches for the RFNM la9310-driver and la9310-freertos repositories

# Requirements

- This firmware uses VSPA kernels from https://github.com/nxp-qoriq/la931x_vspa_common
- The host-utils need two header trees, passed with `LA9310_COMMON_HEADERS` and `UAPI_DIR`
  (defaults: `../la93xx_freertos/common_headers` and `../la93xx_host_sw/uapi`):
  - NXP platforms (LimeSDR-Micro): https://github.com/nxp-qoriq/la93xx_freertos and
    https://github.com/nxp-qoriq/la93xx_host_sw
  - RFNM: https://github.com/rfnm/la9310-freertos and https://github.com/rfnm/la9310-driver
    with `doc/patches/` applied (`uapi/la9310_modinfo.h` comes from the patches),
    see `doc/iqplayer_porting_guide_RFNM-SDK.rst`
- The Linux VSPA tool chain is freely available with the eval version of CodeWarrior for VSPA 10.3.0
  or at https://www.nxp.com/lgfiles/sdk/la1224/imx-la9310-sdk-10/cwvspa.vbeta_14_00_781_vspa.linux.tgz

# Contents

- `iqplayer_cwproj`: source code, Makefile and CW projects for the VSPA firmware variants
  1T1R, 1T2R, 1T2R_DIAG, 0T4R, 0T4R_DIAG, 0T4R_PROF
- `host-utils`: Linux user space utilities
  - shell scripts to load, start and stop transfers: `load-la9310.sh`, `iq-replay.sh`,
    `iq-capture.sh`, `iq-capture-ddr.sh`, `iq-app-tx.sh`, `iq-app-rx.sh`, `iq-stop.sh`,
    `iq-reload-vspa.sh`
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
 ./load-la9310.sh
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

## Usage - iq_app RX

Default 512 KB RX FIFO per channel in DDR, channel count read from the loaded firmware
```sh
 ./iq-app-rx.sh -D ./rx.bin 4915200      # capture 4915200 bytes per channel -> rx_ch<N>.bin
 ./iq-app-rx.sh -X                       # live spectrum with RX QEC keys
 ./iq-app-rx.sh -X -W /tmp/rx 10ms       # key w writes a 10 ms IQ snapshot per channel
 kill -INT <iq_app PID>                  # stop a background capture (file is saved)
 ./iq-stop.sh
```

## QEC

- Press `s` in `iq-app-rx.sh -X` / `iq-app-tx.sh -T` to save the QEC to `<script dir>/qec`.
  Saved files are applied at the next start, for every firmware variant
  (`iq-app-rx.sh -Q <dir>` / `-q`, `iq-app-tx.sh` `QEC_DIR` / `QEC_NOLOAD=1`).
- `imb2qec.py <tx|rx> <gain_dB> <phase_deg> <dc_re> <dc_im> [--ch N | --all]`
- Since v1.2 the AXIQ RX FIFOs swap I/Q (RX_IQ_SWAP=1, RFNM/LMS7002 mirrored spectrum fix).
  Re-run the RX QEC calibration after updating: previous phase/DC values change sign.
