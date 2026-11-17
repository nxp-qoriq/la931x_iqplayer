.. sectnum::

**IQ Player — porting guide for LimeSDR-Micro on NXP Platform (i.MX or Layerscape)** 
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Overview
********

This guide describes how to add support for LA9310 IQ Player and the LimeSDR-Micro M.2 board (LA9310 + LMS7002M)
on NXP platforms such as FRDM-IMX8MPLUS, FRDM-IMX95, LS1043ARDB, LS1046ARDB, LX2160ARDB and HoneyComb LX2.
It replaces the LimeSuiteNG stock radio stack with IQ Player.

The LA9310 can run several software stacks. They use different M4 / VSPA firmware and host
software and cannot run at the same time:

.. list-table::
   :header-rows: 1
   :widths: 18 27 25 30

   * - Stack
     - LA9310 firmware (M4 / VSPA)
     - Host side
     - Use it for
   * - Stock LimeSDR-Micro
     - Lime firmware
     - LimeSuiteNG
     - Lime tools and SDR applications that need timestamped streaming
   * - Stock RFNM
     - RFNM ``la9310-freertos`` / RFNM VSPA
     - ``la9310rfnm``, librfnm, SoapySDR / GNU Radio
     - RFNM applications (RFNM board only)
   * - IQ Player (this repository)
     - NXP or patched RFNM M4 / IQ Player VSPA
     - ``iq_app``, ``libiqplayer``, host-utils
     - raw time-domain I/Q capture and replay, open VSPA source, starting point for new
       platforms
   * - fr1_fr2_test_tool (DFE)
     - NXP ``la93xx_freertos`` (DFE) / 5G DFE VSPA
     - DPDK ``dfe_app``
     - 5G low PHY in the VSPA (symbol sync, cyclic prefix, FFT / iFFT); the host exchanges
       frequency-domain samples. FDD on LimeSDR-Micro, TDD on RFNM only

This guide covers IQ Player and fr1_fr2_test_tool on LimeSDR-Micro. IQ Player on RFNM is
described in ``iqplayer_porting_guide_RFNM-SDK.rst``.

Hardware Requirements
*********************

FRDM-IMX8MPLUS or FRDM-IMX95 

- refer to https://www.nxp.com/design/design-center/development-boards-and-designs/frdm-development-boards:FRDM

Layerscape LS1043ARDB, LS1046ARDB, LX2160ARDB, HoneyComb LX2

- refer to https://www.nxp.com/design/design-center/development-boards-and-designs/LS1043A-RDB
- refer to https://www.nxp.com/design/design-center/development-boards-and-designs/LS1046A-RDB
- refer to https://www.nxp.com/design/design-center/software/qoriq-developer-resources/layerscape-lx2160a-reference-design-board:LX2160A-RDB
- refer to https://www.solid-run.com/arm-servers-networking-platforms/honeycomb-lx2/

limesdr-micro M.2 board (or miniPCI)

- refer to https://www.crowdsupply.com/lime-micro/limesdr-micro 

Hardware Setup
**************

LimeSDR-Micro
 .. image:: pictures/LimeSDR-Micro-board.jpg

imx95-frdm
 .. image:: pictures/imx95-frdm_LimeSDR-Micro-board.png

imx8mp-frdm
 .. image:: pictures/imx8mp-frdm_LimeSDR-Micro-board.png

LimeSDR-Micro official software environment
*******************************************

Lime Microsystems provides an official, productized software framework supporting various SDR ecosystems such as plug-ins for SoapySDR, GNU Radio, Amarisoft 4G/5G, and others. 
There is also support for OpenAirInterface and srsRAN Project.

References:

- https://myriadrf.org/projects/limesdr-micro/ 
- https://github.com/myriadrf/LimeSuiteNG/tree/limesdr-micro

The LimeSDR-Micro software is derived from basic NXP LA9310 drivers, applications and firmwares, in particular IQ Player. 
This porting guide uses the LimeSDR-Micro module as a reference to explain how to:

- Deploy NXP linux BSP
- Enable IQ Player : Basic time domain capture/replay firmware The LA9310 "Hello-world" app.
- Enable FR1FR2TestTool/DFE_app : 5G DFE and low PHY firmware for LA9310  

This porting guide will help users to enable LA9310 and new platforms.

Linux BSP images for i.MX or Layerscape boards
**********************************************

The starting point is the standard NXP Linux BSP for i.MX and Layerscape processors ( https://www.nxp.com/linux )

NXP Debian Linux BSP
https://www.nxp.com/design/design-center/software/embedded-software/linux-software-and-development-tools/nxp-debian-linux-sdk-distribution-for-i-mx-and-layerscape:NXPDEBIAN

NXP Yocto BSP
https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX

Debian Linux BSP is recommended, it is easier to enable various SDR framework allowing simple "apt install" of necessary packages.
Refer to the BSP user guide for instructions on building and deploying images for the selected target.

Example for Debian
..................

::

 git clone https://github.com/NXP/flexbuild.git
 cd flexbuild
 . setup.env  (in host environment)
 bld docker   (create or attach to docker)
 . setup.env  (in docker environment)
 bld host-dep (install host dependent packages)

 bld -m imx8mpfrdm
 bld -m imx95frdm
 bld -m ls1046ardb
 bld -m lx2160ardb  

Deployment sdcard using flex-installer on a laptop
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

user can deploy images on sdcard directly from compilation host machine
::

  [root@fbdebian flexbuild-lf-6.6.52-2.2.0]$ ls build_lsdk2506/images/flex-installer
  $ flex-installer -r rootfs_lsdk_debian_xx_arm64.tar.zst -d /dev/sdx         (only install rootfs partition image)
  $ flex-installer -b boot_IMX_arm64_lts_xx.tar.zst -d /dev/sdx -m <machine>  (only install boot partition image)
  $ flex-installer -f firmware_<machine>_sdboot.img -d /dev/sdx -m <machine>  (only install composite firmware image)
  machine = imx8mpfrdm, imx95frdm, ls1046ardb, lx2160ardb

Deployment sdcard using flex-installer on the target
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

start linux tinyDistro on the target from u-boot
::

 u-boot=> tftp lsdk2512_poky_tiny_IMX_arm64.itb
 u-boot=> bootm 0x90400000#imx95frdm
 u-boot=> bootm 0x40400000#imx8mpfrdm

copy images into ramfs /var/volatile or nfs mount location (solve size limitations)

nfs mount
::

 mkdir /mnt/nfs/
 root@TinyLinux:# busybox mount -o port=2049,nolock,proto=tcp -t nfs <serverip>:/tftpboot/ /mnt/nfs/

Deploy images
::

 root@TinyLinux:/mnt/nfs# flex-installer -f firmware_imx8mpfrdm_sdboot.img -d /dev/mmcblk1 -m imx8mpfrdm
 root@TinyLinux:/mnt/nfs# flex-installer -b boot_IMX_arm64_lts_6.12.20.tar.zst -r rootfs_lsdk2512_debian_imx8mpfrdm.tar.zst -d /dev/mmcblk1 -m imx8mpfrdm

Add LA9310 support 
******************

To enable LA9310 support, the following software components must be modified or added:

- Linux kernel : Add patches for LA9310 support (PCI quirks, memory reservation in the device tree)

- la93xx_host_sw : Compile and deploy the LA9310 PCI driver (shiva)

- la93xx_firmware : Compile and deploy LA9310 firmwares : FreeRTOS (dfe_app) and VSPA firmwares (IQPlayer and/or Fr1Fr2TestTool)

- dpdk-dfe-app : Compile and deploy linux dfe application providing basic CLI to control the LA9310 e200 firmwares and the RF chain

Tool chains
...........

ARMv8 tool chain 
~~~~~~~~~~~~~~~~
::

   sudo apt-get install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev zlib1g-dev bc
   sudo apt-get install gcc-aarch64-linux-gnu


M4 tool chain
~~~~~~~~~~~~~
::

   download and install gcc-arm-none-eabi-6-2017-q2-update-linux.tar.bz2
   wget https://www.nxp.com/lgfiles/geul/Tools/gcc-arm-none-eabi-6-2017-q2-update-linux.tar.bz2

   $ sudo ln -s /opt/gcc-arm-none-eabi-6-2017-q2-update/bin/arm-none-eabi-gcc /usr/bin/arm-none-eabi-gcc
   $ sudo ln -s /opt/gcc-arm-none-eabi-6-2017-q2-update/bin/arm-none-eabi-g++ /usr/bin/arm-none-eabi-g++

VSPA tool chain
~~~~~~~~~~~~~~~

Download
::

   VSPA2/LA9310 : wget https://www.nxp.com/lgfiles/sdk/la1224/imx-la9310-sdk-10/cwvspa.vbeta_14_00_781_vspa.linux.tgz
   VSPA3/LA1224 : wget https://www.nxp.com/lgfiles/sdk/la12xx_sdk3.2_revc_ext/cwvspa.valpha3_00_00_432_vspa.linux.tgz

Install  
::

 tar -xvf cwvspa.vbeta_14_00_781_vspa.linux.tgz
 sudo mv VSPA_Tools/ /opt/VSPA_Tools_vbeta_14_00_781

Compiler is 32bit executable, need to add 32bit support to avoid following errors
/opt/VSPA_Tools_vbeta_14_00_781/bin/fsvspacc: No such file or directory
libstdc++.so.6: cannot open shared object file: No such file or directory
::

   sudo apt install libc6:i386 lib32stdc++6

Linux Kernel 
............

clone linux source code 
~~~~~~~~~~~~~~~~~~~~~~~

NXP provides a specific kernel branch with necessary patches to support LA9310 and LA12xx devices.
User has choice to use this branch or port the commits listed below onto their kernel tree

::

 git clone https://github.com/nxp-qoriq/linux/ -b la12xx-linux-6.12-rt
 cd linux

Relevant commits
~~~~~~~~~~~~~~~~

  - f49a00ec7bea arm64: imx_v8_defconfig: add SCTP config by default
  - f92b307c3e4e arm64: imx_v8_defconfig disable CONFIG_STRICT_DEVMEM
  - 7d7f2f5fe9bf Fix i.mx8mp/dxl board selection for pci config
  - 80fe046ac541 5g-linux: fix imx8dxl pci bar configuration
  - 445ee98f4a75 Configure the LA9310 BAR 0 to 64MB due to limited size of PCIe region on i.mx8dxl & i.mx8mp
  - ae33a1d15a68 fix "make bindeb-pkg" use to generate kernel headers debian package
  - 81f6a95cf0f3 arm64: dts: imx95: Increase the outbound space size to 2G bytes

Compile Linux Kernel and Device Tree
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For i.MX
::

 export CROSS_COMPILE=aarch64-linux-gnu-
 export ARCH=arm64
 make imx_v8_defconfig
 make -j$(nproc)

For Layerscape
::

 export CROSS_COMPILE=aarch64-linux-gnu-
 export ARCH=arm64
 ./scripts/kconfig/merge_config.sh arch/arm64/configs/defconfig arch/arm64/configs/lsdk.config
 make -j$(nproc)

Deploy Linux Image
~~~~~~~~~~~~~~~~~~

Replace kernel image and device tree on the target :
::

  arch/arm64/boot/dts/freescale/imx95-15x15-frdm.dtb
  arch/arm64/boot/dts/freescale/imx8mp-frdm.dtb 
  arch/arm64/boot/Image 

  root@frdm:~# ls /boot/
  Image  imx95-15x15-frdm.dtb imx8mp-frdm.dtb   

prepare kernel modules
::

  mkdir modules_install
  make INSTALL_MOD_PATH=./modules_install modules_install
  cd modules_install
  tar -czvf modules.tar.gz lib

Copy modules.tar.gz to the target and extract and install under /lib/modules/

Deploy Kernel headers to kernel module native compilation 
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

prepare kernel headers
::

  make bindeb-pkg

deploy kernel headers on the target
::

  dpkg -i linux-headers-6.12.34-rt11-xxx_arm64.deb

la93xx_host_sw
..............

clone and patch la93xx_host_sw linux kernel module source code
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
::

 git clone  https://github.com/nxp-qoriq/la93xx_host_sw.git
 cd la93xx_host_sw
 git checkout bc0a100
 git apply --index <la931x_iqplayer>/doc/patches/la93xx_host_sw/*.patch

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Patch
     - Change
   * - ``la93xx_host_sw/0001-la9310shiva-modinfo-mmap-scratch-iqflood-and-PCIe-BA.patch``
     - ``la9310_modinfo`` mmap of the scratch buffer, the IQFLOOD buffer and the
       CCSR/TCML/TCMU BARs on ``/dev/shiva0``. The IQ Player host-utils then do
       not need ``/dev/mem`` and work on kernels built with ``CONFIG_STRICT_DEVMEM=y``.
       Without it, the host-utils fall back to ``/dev/mem`` (needs
       ``CONFIG_STRICT_DEVMEM`` not set).

Recompile the shiva driver against the kernel running on the board
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``KERNEL_DIR`` must be a fully built tree of the kernel running on the board, with
the same ``.config``: the kernel uses ``CONFIG_MODVERSIONS``, so a different
configuration gives a different ``Module.symvers`` and the module load fails
with ``disagrees about version of symbol``. To rebuild a matching tree from the
board configuration (same kernel commit as the board ``uname -r``):

::

 cd linux
 ssh root@<targetIP> 'zcat /proc/config.gz' > .config
 make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
 make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kernelrelease   # must match uname -r
 make -j$(nproc) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules

Build the driver:

::

 cd la93xx_host_sw
 git submodule update --init

 export KERNEL_DIR=$PWD/../linux
 export LA9310_COMMON_HEADERS=$PWD/../la93xx_freertos/common_headers
 export LA9310_DRV_HEADER_DIR=$PWD/../la93xx_host_sw/kernel_driver/la9310sdr
 export CROSS_COMPILE=aarch64-linux-gnu-
 export ARCH=arm64
 make clean
 CONFIG_ENABLE_FLOAT_BYPASS=y make
 modinfo -F vermagic kernel_driver/la9310shiva/la9310shiva.ko   # must match uname -r

.. note::

   dependency : need to clone FreeRTOS to get common_headers

Deploy images
::

 make install
 scp -r install/* root@<targetIP>:/

copy la9310shiva.ko to:
::

 cp /kernel_module/* /lib/modules/$(uname -r)/extra/

.. note::

   ``la9310shiva`` cannot be unloaded once loaded (``rmmod`` reports the module in
   use). To change the driver, install the new ``.ko`` in
   ``/lib/modules/$(uname -r)/extra/``, reboot, then run ``load-limesdr-micro.sh``.

la93xx_firmware (LA9310 FreeRTOS)
.................................

Dependency : need to clone LimeSuiteNG first
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
::

 git clone https://github.com/myriadrf/LimeSuiteNG.git
 git checkout -b limesdr-micro origin/limesdr-micro

clone FreeRTOS source code 
~~~~~~~~~~~~~~~~~~~~~~~~~~
::

 git clone https://github.com/nxp-qoriq/la93xx_freertos.git
 cd la93xx_freertos/

apply the IQ Player LimeSDR-Micro patches (LA9310 clock 122.88 MHz, LMS7002M RX channel B,
VSPA debugger sw_cmd, see `VSPA debugger (la9310_debug)`_, clean -Werror build with gcc 6)
::

 git am $IQP/doc/patches/la93xx_freertos/*.patch      # IQP = la931x_iqplayer clone

Compile FreeRTOS with LimeSDR-micro support 
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
::

 export ARMGCC_DIR=/opt/gcc-arm-none-eabi-6-2017-q2-update
 export LA9310_HOST_SW_UAPI=$PWD/../la93xx_host_sw/uapi
  cd Demo/CORTEX_M4_NXP_LA9310_GCC
 ./clean.sh 
 ./build_release.sh -m pcie -t lime_sdr_micro -f LA9310_DFE_APP=ON -b release

Warnings are errors (``CMAKE_COMPILE_WARNING_AS_ERROR``). Without
``0005-limesdr_micro-build-clean-under-Werror-with-gcc-arm-.patch`` the upstream tree fails
with gcc 6 (``unrecognized command line option '-Wno-address-of-packed-member'``, unused
``pxContext``). Check the image: ``strings release/la9310.bin | grep "sw cmd"``.

LA9310 clock (DCS_CLK) on LimeSDR-Micro
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

On LimeSDR-Micro the LA9310 reference clock is generated by the LMS7002M (CGEN) and set by
the M4 at boot from ``LA9310_SYS_CLK_FREQ``: CGEN = 4 x ``LA9310_SYS_CLK_FREQ``, divided
by 4 towards the LA9310. The VSPA runs at 4 x DCS_CLK and the ADC/DAC at DCS_CLK (or
DCS_CLK / 2 with the half-rate masks of ``load-limesdr-micro.sh``).

IQ Player requires ``LA9310_SYS_CLK_FREQ`` = 122880000 (same clocks as RFNM): VSPA
491.52 MHz, converters 61.44 Msps. This is the default with the patches above
(``0001-limesdr_micro-set-LA9310-system-clock-to-122.88-MHz.patch``); the upstream default
61440000 runs the VSPA at 245.76 MHz and the converters at 30.72 Msps, so iq_app shows
twice the real frequencies and TX can stall (see ``iqplayer_design_notes.rst``).

Check the configuration:

- build log: ``-- LA9310 system clock: 122880000`` (an old CMake cache keeps the previous
  value: ``cmake -DLA9310_SYS_CLK_FREQ=122880000 .`` or ``./clean.sh``)
- on the target, ``iq-app-tx.sh -T`` + ``iq_mon``: TX ``DMA_AXIQ_WR`` about 245 MB/s
  (122 MB/s means DCS_CLK is 61.44 MHz)
- ``la9310_modem_info`` always computes the ADC/DAC rates assuming DCS_CLK = 122.88 MHz,
  it does not detect a wrong ``LA9310_SYS_CLK_FREQ``

Deploy FreeRTOS Image
~~~~~~~~~~~~~~~~~~~~~
::

 scp release/la9310.bin  root@<target IP>:/lib/firmware/la9310_dfe.bin

dpdk dfe_app Lime CLI
.....................

clone dpdk source code 
~~~~~~~~~~~~~~~~~~~~~~

::

 git clone https://github.com/NXP/dpdk -b 25.11-qoriq
 cd dpdk 
 git am $IQP/doc/patches/dpdk/*.patch    # dfe_app: no core starvation, probe fixes

Compile dpdk dfe_app
~~~~~~~~~~~~~~~~~~~~

::

 export LA9310_COMMON_HEADERS=$(realpath $PWD/../la93xx_freertos/common_headers)
 export LA9310_HOST_SW_UAPI=$(realpath $PWD/../la93xx_host_sw/uapi)
 meson arm64-build --cross-file config/arm/arm64_dpaa_linux_gcc -Dbuildtype=debug -Dexamples=dfe_app
 ninja -C arm64-build

install dpdk
~~~~~~~~~~~~

::

 scp ./arm64-build/examples/dpdk-dfe_app  root@<target IP>:/usr/local/bin/

.. note::
   Older ``dpdk-dfe_app`` builds busy-spin a SCHED_FIFO 99 thread on an EAL worker
   core (``tti_poll_loop`` wait loop), which freezes other ssh sessions on that core
   while the interactive prompt is open. Use a dfe_app with the sleeping wait loops
   in ``examples/dfe_app/ipc.c``, or move it to spare cores: ``dpdk-dfe_app -i 4 -m 0x30``.

IQ Player
.........

Compile and deploy iqplayer firmware and utilities

Default VSPA tools path:
::

 VSPA_TOOL ?= /opt/VSPA_Tools_vbeta_14_00_781

IQ Player has two external header dependencies, taken from the NXP GitHub repositories
cloned above: ``LA9310_COMMON_HEADERS`` (``la93xx_freertos/common_headers``,
``la9310_host_if.h``) and ``UAPI_DIR`` (``la93xx_host_sw/uapi``, ``la9310_modinfo.h``).
The defaults expect both trees next to ``la931x_iqplayer``; otherwise pass them explicitly.

clone and compile IQPlayer
::

 git clone https://github.com/nxp-qoriq/la931x_iqplayer.git
 cd la931x_iqplayer
 export LA9310_COMMON_HEADERS=$PWD/../la93xx_freertos/common_headers
 export UAPI_DIR=$PWD/../la93xx_host_sw/uapi
 make
 make install
 scp -r install/* root@<targetIP>:/

Testing IQ Player playing and capturing time domain waveform
............................................................

load and start VSPA firmware
::

 ./load-limesdr-micro.sh

``load-limesdr-micro.sh`` applies the LimeSDR-Micro RX defaults (``iq-board-defaults.sh
limesdr-micro``):

- RX I/Q swap 0 (``rx-iqswap.sh 0``): the LimeSDR-Micro RX spectrum is not mirrored.
  ``load-rfnm.sh`` sets it to 1 (RFNM, also the VSPA boot default).
- RX order RX0 RX1 RO0 RO1 (``rx-chan.sh 3 1 2 0``). A firmware with N RX channels uses
  the first N: 1T1R streams Lime RX1, 1T2R Lime RX1 + RX2, 0T4R all four.

Both are applied on the next RX start. ``iq-reload-vspa.sh`` applies them again after a
VSPA image swap (the board is recorded in ``/run/iqplayer.board``). ``rx-chan.sh`` can
still override the order before an RX start.

``rx-chan.sh -r`` reads back the configuration published by the VSPA (proxy word 0x1C,
see the reference manual): slot to chanID, LA9310 input, board path, IQ swap and the
RX/TX channel counts. Example with 1T2R after ``load-limesdr-micro.sh``:
::

  ./rx-chan.sh -r
  proxy cfg 0x02130202: RX 2  IQ swap 0  TX 1  board limesdr-micro
   slot 0 -> chanID 3  RX0  LMS7002M RX1 (ch A)
   slot 1 -> chanID 1  RX1  LMS7002M RX2 (ch B)

The firmware TX count is also used as a guard: with 0T4R (no TX) ``iq-app-tx.sh`` and
``iq-replay.sh`` stop with an error. Load 1T1R or 1T2R first
(``iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld``).

configure LimeRF using dfe_app. On NXP platforms the RF chain is controlled by the M4
firmware through ``dpdk-dfe_app`` (on RFNM it is configured by Linux sysfs instead, see
``iqplayer_porting_guide_RFNM-SDK.rst``)
::

  echo 100 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
  dpdk-dfe_app
  DFE> lime tx 1 freq 1200000000
  DFE> lime rx 1 freq 1210000000
  DFE> quit

RX channel mapping on LimeSDR-Micro. The LMS7002M ADCs are not used: its two analog
baseband RX outputs feed LA9310 ADC inputs. ``rx-chan.sh`` takes the mbx chanID.

  ========  ==========  ==============================  ===================
  chanID    LA9310      LimeSDR-Micro                   ``lime rx`` channel
  ========  ==========  ==============================  ===================
  0         RO1         connector only (no RFIC)        -
  1         RX1         LMS7002M RX2 (channel B)        2
  2         RO0         connector only (no RFIC)        -
  3         RX0         LMS7002M RX1 (channel A)        1
  ========  ==========  ==============================  ===================

TX: only LMS7002M TX1 (channel A) is wired to the LA9310 DAC.

Both LMS7002M RX channels share one RX LO (SXR): ``lime rx 2 freq`` retunes channel A too.
``lime rx <n>`` powers up the selected LMS7002M RX channel (channel B is off at reset).

1T2R with both LMS7002M RX channels (slot 0 = Lime RX1, slot 1 = Lime RX2)
::

  dpdk-dfe_app
  DFE> lime rx 1 freq 1210000000
  DFE> lime rx 2 freq 1210000000
  DFE> quit
  ./iq-app-rx.sh -X           # rx-chan.sh 3 1 not needed: load default

optional: AXIQ digital loopback (TX looped back to RX inside the LA9310, no RF)
::

  dpdk-dfe_app -c "axiq_lb enable"
 

play (repeat) waveform from file
::

 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200

capture (one time) iq samples into file
::

 ./rx-chan.sh 3
 ./iq-capture.sh ./iqdata.bin 1200

capture (repeat) in DDR buffer
::

 ./rx-chan.sh 3
 ./iq-capture-ddr.sh 1200
 ./iq-stop.sh

Check LA9310 freeRTOS console
::

  cat /sys/devices/platform/soc@0/33800000.pcie/pci0000:00/0000:00:00.0/0000:01:00.0/la9310sysfs/target_log

display received spectrum (on a PC, after copying iqdata.bin)
::

  python compute_tone_params.py -f 61440000 -i ./iqdata.bin

.. image:: pictures/iq_player_single_tone_Frequency_spectrum.png

Streaming with iq_app (spectrum, QEC, snapshot)
::

  ./iq-app-tx.sh -T                     # interactive tone and TX QEC
  ./iq-app-rx.sh -X                     # live RX spectrum and RX QEC, key w = IQ snapshot
  ./iq-app-rx.sh -D ./rx.bin 4915200    # capture to rx_ch<N>.bin
  ./iq-stop.sh

Each channel header shows the slot, the VSPA chanID, the LA9310 input, the board path
and the RX IQ swap. Example, 1T2R on LimeSDR-Micro with one antenna on LMS7002M RX1
(channel A); RX2 (channel B) has no antenna, so it is about 20 dB lower. Charts are
shortened.

Spectrum view:
::

 RX QEC ch 0/2  gain  +0.00 dB  phase  +0.00 deg  DC I +0.000  DC Q +0.000   keys: c=chan g/G p/P i/I j/J r=rst s=save w=snap t=time q=quit

 Ch0 [chan 3 RX0 | LMS7002M RX1 (A)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
   RSSI -7.1  floor -78.2  DC -28.3 dBFS
   spurs: -9.0@-3.28 -23.9@-10.00 -48.3@-3.40 -49.9@-3.42 MHz
   peak I -5.4  Q -5.0 dBFS  clip 0 (0.000%)  crest 5.1 dB
  -12.5 |                               |                                                |
  -25.0 |             |                 |                                                |
  -37.5 |             |                 #        |                                       |
  -50.0 |        |    #|                #        #                                       |
  -62.5 |        |    ##              |###|      #                                   |   |
  -75.0 |        #|  |###|||||||################|#|||||| || ||| | |        |         |   |
  -87.5 |################################################################################|
 -100.0 |################################################################################|
        ++-------------------+-------------------+-------------------+-------------------+
      -15.36               -7.68               +0.00               +7.68              +15.36       MHz
 Ch1 [chan 1 RX1 | LMS7002M RX2 (B)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
   RSSI -27.9  floor -83.9  DC -28.5 dBFS
   spurs: -39.3@-3.28 -55.5@-10.00 -60.9@-11.92 -67.7@+3.28 MHz
   peak I -24.8  Q -24.8 dBFS  clip 0 (0.000%)  crest 6.1 dB
  -37.5 |                                        |                                       |
  -50.0 |                               |        #                                       |
  -62.5 |        |    |                 #        #                                       |
  -75.0 |        |    #|                #        #       |                               |
  -87.5 ||||||######################################################################||||||
 -100.0 |################################################################################|
        ++-------------------+-------------------+-------------------+-------------------+
      -15.36               -7.68               +0.00               +7.68              +15.36       MHz

Time view (key ``t``, 80 samples, auto scale):
::

 Ch0 [chan 3 RX0 | LMS7002M RX1 (A)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
 +0.500 |       o*       o**      o**      oo*       o*       o        o  *     oo **    |
 +0.375 |*     o* *     o   *    o o *      * **    o  **    o +***     +* *      *      |
 +0.250 | *   o    *   o *o  *        *   o          *o  *            o          *o  *   |
 +0.125 |....o...o..*.o.........o........o...o..*..o........o.*..........o.....o.........|
 +0.000 |  *   *               o  * o      *      o    o        o    o *  o * o    o    o|
 -0.125 |     *   o     *  o  *  *   o *o     o     *      o     o *o         *         *|
 -0.250 |    +**    o +* *      **       +*     o o* *      +        o      o o+ *   o  o|
 -0.375 |  o            *      o  *    o   *        *     oo **    oo **     o  *     oo |
 -0.500 |   o        o        o         o        o                                       |
        ++-------------------+-------------------+-------------------+-------------------+
         0 .. 2.604 us, 80 samples, 1/col  scale +-1.00 FS (auto)  keys: +/- zoom, a=auto/fixed, t=spectrum
 Ch1 [chan 1 RX1 | LMS7002M RX2 (B)]  IQ swap 0  Fs 30.72 MHz  FFT 8192 x8  RBW 3.8 kHz  rect/mag
 +0.050 |      o                 ooo                oo       o         oo       oo       |
 +0.025 |oo o     o  o     oo         oo     ooooo     oooo      oo o    oo  oo     oooo |
 +0.000 |        **      **         *       *         **                            *    |
 -0.025 |* * ***        *     *  *     * ***    *  ***     **     **  *     * * *     * *|
 -0.050 |                                                                                |
        ++-------------------+-------------------+-------------------+-------------------+
         0 .. 2.604 us, 80 samples, 1/col  scale +-0.10 FS (auto)  keys: +/- zoom, a=auto/fixed, t=spectrum

Saved QEC files (key ``s``) in ``<script dir>/qec`` are applied at start. Firmware variants,
script options and QEC details are described in ``iqplayer_reference_manual.rst``.

Testing FR1FR2TestTool playing and capturing 5G waveform
........................................................

IQ Player streams raw time-domain I/Q samples. LimeSuiteNG adds features such as timestamping,
required by some SDR use cases. NXP also provides a firmware for OFDM processing, a 5G low PHY
also called DFE, used by ``fr1_fr2_test_tool``: the VSPA does 5G symbol synchronization and
delineation, cyclic prefix insertion / removal and FFT / iFFT, and exchanges frequency-domain
samples with the host. The VSPA implements the L1 low PHY; the host implements the high PHY and
the upper layers.

clone Fr1FR2Tool  
:: 

 git clone https://github.com/nxp-qoriq/fr1_fr2_test_tool
 cd fr1_fr2_test_tool

booting up firmware for LA9310
::

 ./boot_vspa.sh MEvspa_images_LS.1T2R_10M_15K_61_61_TDDFDD_LA93  

start playing test_vectors/TM3.3_10MHz_15kHz_FDD.bin
::

  ln -s /usr/local/bin/dpdk-dfe_app /usr/bin/dpdk-dfe_app
  ./channels_start.sh

configure lime
::

  root@imx8mpfrdm:~/fr1_fr2_test_tool# dpdk-dfe_app
  DFE> lime tx 1 freq 1200000000
  DFE> lime rx 1 freq 1210000000
  DFE> quit

Capture time domain waveform ( QEC disabled )

.. image:: pictures/TM3.3_10MHz_15kHz_FDD_freq_spectrum_taken_with_RFNM_sdrpp.png 

The fr1_fr2_test_tool VSPA firmware source code is available at
https://github.com/nxp-qoriq/la931x_vspa_5g_dfe

Known limitation: TDD is not supported on LimeSDR-Micro (FDD only). TDD is supported on the
RFNM board only.

VSPA debugger (la9310_debug)
............................

Optional. ``la9310_debug`` (``vdbg``, ``profile``, DAP server for VS Code) runs on the
imx95frdm with the ``shiva`` transport: attach to the running VSPA, read memory while
streaming, breakpoints, watchpoints, profiling and reload. The requests go to the M4
through the HIF ``sw_cmd_desc`` (``LA9310_SW_CMD_VSPA_DBG``, MSG unit 2 doorbell).

Requirements:

- M4 image built with ``doc/patches/la93xx_freertos/0004-sw_cmd_engine-LA9310_SW_CMD_VSPA_DBG-proxy-for-the-V.patch``
  (applied by the ``git am`` of the FreeRTOS section above): adds the VSPA_DBG handler and
  starts the sw_cmd engine, which is not started by the upstream image. Check with
  ``strings release/la9310.bin | grep "sw cmd"``.
- ``la9310shiva.ko`` with ``doc/patches/la93xx_host_sw/0001`` (mmap of the PCIe BARs
  through ``/dev/shiva0``; without it the tools fall back to ``/dev/mem``).
- Python 3 on the target; run as root.

Clone and patch::

 git clone https://git.6g-for-all.de/6g4all/la9310_debug.git $DBG
 git -C $DBG checkout 2328871
 git -C $DBG apply --index $IQP/doc/patches/la9310_debug/*.patch
 python3 -m unittest discover -s $DBG/tests -p "test_shiva_port.py"   # offline

Deploy the rebuilt ``la9310.bin`` as ``/lib/firmware/la9310_dfe.bin``, run
``./load-limesdr-micro.sh``, copy ``$DBG`` to the target, then::

 python3 shiva_port.py heartbeat        # M4 answers VSPA debug requests
 python3 shiva_port.py info             # VSPA state snapshot
 python3 vdbg.py --transport shiva --attach /lib/firmware/apm-iqplayer-1T1R.eld
 python3 profile.py --transport shiva /lib/firmware/apm-iqplayer-1T1R.eld --seconds 10
 python3 shiva_port.py load /lib/firmware/apm-iqplayer-1T1R.eld   # reload a hung VSPA

``shiva_port.py`` reads the HIF offset published by the NXP firmware (DCFG SCRATCHRW[3],
0x1B000) and uses ``sw_cmd_desc`` at TCML + 0x1B240. Halting the VSPA (breakpoint,
watchpoint) stops the stream; restart with ``shiva_port.py load`` or
``iq-reload-vspa.sh``. Keep dfe_app idle while debugging: its commands also go through
the M4. Details: ``docs/shiva.md`` in ``la9310_debug``.

Verified on imx95frdm + LimeSDR-Micro (1T1R, TX + RX streaming): ``heartbeat``, ``info``,
``vdbg --attach`` with live ``dmem``, ``profile`` and a breakpoint. ``regs`` while halted,
resume after a breakpoint, ``load`` and ``--transport rfnm`` are not recorded yet.

Profile while streaming (10 s)::

 667 samples
  24.44%     163  _PUSH_RX_DATA
  23.39%     156  _PUSH_TX_DATA
  12.89%      86  _meter_end
   9.15%      61  _VSPA_PROXY_update
   7.65%      51  _main
   6.00%      40  _dma_done_from_state
   5.85%      39  ___internal_done

With no stream the profile is 100% ``___internal_done``: the event-driven VSPA sleeps
between passes.

Breakpoint in the RX path (halting stops the stream)::

 (vdbg) break _PUSH_RX_DATA
   breakpoint 0 at 0x10B8 <_PUSH_RX_DATA> at iqmod_rx.c:353
 (vdbg) c
 stopped: breakpoint/comparator 0
   0x10B8 <_PUSH_RX_DATA> at iqmod_rx.c:353

DMEM words that read ``DEADBEEF`` have not been written since boot (crt0 fills DMEM with
that pattern).

Tips 
****

Unexpected imx95frdm going to sleep
...................................

::

 root@imx95frdm:~/host_utils# [  916.073426] fsl_enetc4 0001:00:00.0 eth0: Link is Down
 [  916.419685] PM: suspend entry (deep)
 [  916.437359] Filesystems sync: 0.014 seconds
 [  916.445646] Freezing user space processes

To disable suspend on a Linux system, you can use the command: 
:: 

 sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target. 
 
After running this command, reboot your system to apply the changes.

If needed enable LA9310 EP BAR0-2 [disabled] 
............................................

user need to ensure la9310 BAR0-2 are enabled before issuing ./load-limesdr-micro.sh

::

 root@imx95frdm:# lspci |grep 1c12
 0000:01:00.0 Unclassified device [0002]: Freescale Semiconductor Inc Device 1c12 (rev 10) (prog-if 80)

 root@imx95frdm:~# lspci -v -s 0000:01:00.0
 0000:01:00.0 Unclassified device [0002]: Freescale Semiconductor Inc Device 1c12 (rev 10) (prog-if 80)
         Flags: fast devsel, IRQ 273, IOMMU group 11
         Memory at 910000000 (32-bit, non-prefetchable) [disabled] [size=256M]
         Memory at 921000000 (32-bit, non-prefetchable) [disabled] [size=128K]
         Memory at 928000000 (64-bit, prefetchable) [disabled] [size=8M]

 root@imx95frdm:~# setpci -s 01:00.0 COMMAND=0x02

 root@imx95frdm:~# lspci -v -s 0000:01:00.0
 0000:01:00.0 Unclassified device [0002]: Freescale Semiconductor Inc Device 1c12 (rev 10) (prog-if 80)
         Flags: fast devsel, IRQ 273, IOMMU group 11
         Memory at 910000000 (32-bit, non-prefetchable) [size=256M]
         Memory at 921000000 (32-bit, non-prefetchable) [size=128K]
         Memory at 928000000 (64-bit, prefetchable) [size=8M]

early IQ Player scripts expect default sh as bash shell
.......................................................

if needed replace script directive 
::

 cd ~/host_utils
 sed -i 's/\/bin\/sh/\/bin\/bash/g' *.sh






