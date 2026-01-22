.. sectnum::

**IQ Player — porting guide for LimeSDR-Micro on imx8mp / imx95** 
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Overview
********

The NXP LA9310 BSP provides support for the RFNM platform. To run IQ Player on other platforms, additional integration work is required. This document describes how to add support for LA9310 and IQ Player on new host platforms.
This guide provides an example of enabling the LimeSDR‑Micro M.2 board (LA9310 + LMS7002M) on the following NXP platforms FRDM-IMX8MPLUS and FRDM-IMX95

Hardware Requirements
*********************

FRDM-IMX8MPLUS or FRDM-IMX95 

- refer to https://www.nxp.com/design/design-center/development-boards-and-designs/frdm-development-boards:FRDM

limesdr-micro M.2 board

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

Lime Microsystems provides an official, productized software framework supporting various SDR software ecosystems such as GNU Radio, MATLAB, and OpenAirInterface.
This framework includes:

- Host drivers
- VSPA firmware source code
- Utilities to develop and customize applications for the LimeSDR‑Micro module

References:

- https://limesdr-micro.myriadrf.net/beta/
- https://github.com/myriadrf/LimeSuiteNG/tree/limesdr-micro

The LimeSDR‑Micro VSPA firmware is derived from IQ Player.
This porting guide uses the LimeSDR‑Micro module as a reference to explain how to:

- Modify the NXP BSP
- Port and enable IQ Player

This process applies to users developing new hardware based on i.MX8MP or i.MX95 with LA9310.

Software Components
*******************

The starting point is the standard NXP i.MX Linux BSP for i.MX8MP or i.MX95.
Two BSPs are availble at https://www.nxp.com/linux 

NXP Yocto BSP
https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX

NXP Debian Linux BSP
https://www.nxp.com/design/design-center/software/embedded-software/linux-software-and-development-tools/nxp-debian-linux-sdk-distribution-for-i-mx-and-layerscape:NXPDEBIAN


Build BSP images for FRDM-IMX8MPLUS or FRDM-IMX95
*************************************************

Refer to the BSP user guide for instructions on building and deploying images for the selected target.
NXP also provides prebuilt images for the Yocto BSP. 

Example for Yocto 
.................

::

 git clone https://github.com/nxp-imx/imx-docker.git
 cd imx-docker/
 ./docker-build.sh Dockerfile-Ubuntu-22.04

::

  repo init -u https://github.com/nxp-imx/imx-manifest -b imx-linux-walnascar -m imx-6.12.34-2.1.0.xml
  cd imx-6.12.34-2.1.0/
  repo init -u https://github.com/nxp-imx/imx-manifest -b imx-linux-walnascar -m imx-6.12.34-2.1.0.xml
  repo sync

  MACHINE=imx8mp-lpddr4-frdm DISTRO=fsl-imx-xwayland source ./imx-setup-release.sh -b bld-xwayland

  MACHINE=imx95-15x15-lpddr4x-frdm DISTRO=fsl-imx-xwayland source ./imx-setup-release.sh -b bld-xwayland
  
  bitbake imx-image-core


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

Deployment sdcard using flex-installer on target or on laptop
::

  [root@fbdebian flexbuild-lf-6.6.52-2.2.0]$ ls build_lsdk2506/images/flex-installer
  $ flex-installer -r rootfs_lsdk_debian_xx_arm64.tar.zst -d /dev/sdx         (only install rootfs partition image)
  $ flex-installer -b boot_IMX_arm64_lts_xx.tar.zst -d /dev/sdx -m <machine>  (only install boot partition image)
  $ flex-installer -f firmware_<machine>_sdboot.img -d /dev/sdx -m <machine>  (only install composite firmware image)
  machine = imx8mpfrdm or imx95frdm

start linux tinyDistro on the target from u-boot
::

 u-boot=> tftp lsdk2512_poky_tiny_IMX_arm64.itb
 u-boot=> bootm 0x90400000#imx95frdm

 use ramfs /var/volatile if not large enough use nfs mount
 root@TinyLinux:/var/volatile# busybox mount -o port=2049,nolock,proto=tcp -t nfs <serverip>:/tftpboot/ /mnt/nfs/

Add LA9310 support 
******************

To enable LA9310 support, the following software components must be modified or added:

- Linux kernel : Add patches for LA9310 support (PCI quirks, memory reservation in the device tree)

- la93xx_host_sw : Compile and deploy the LA9310 PCI driver (shiva)

- la93xx_firmware : Deploy LA9310 M4/FreeRTOS firmware and IQ Player VSPA firmware images

Linux Kernel Modifications
..........................

clone linux source code 
~~~~~~~~~~~~~~~~~~~~~~~

::

 git clone https://github.com/nxp-imx/linux-imx -b lf-6.12.y
 cd linux-imx

Apply necessary linux kernel patches 
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:: 

  cd linux-imx
  patch -p 1 < 0001-Add-CONFIG_STRICT_DEVMEM-n-in-imx_v8_defconfig.patch
  patch -p 1 < 0002-Add-fixup-for-la9310-pci-class-code.patch
  patch -p 1 < 0003-Increase-imx95-PCIe-outbound-space-size-in-device-tr.patch
  patch -p 1 < 0004-Add-imx98mplus-and-imx8dxl-pcie-quirk-to-force-la931.patch

.. note:: 
   - 0001 is required to allow userspace iqplayer scrips/app (root) access to PCI space 
   - 0002 is required for proper enumeration at bootup allowing BAR0-2 mapping
   - 0003 only imx95, dts declare only 256MB of PCI outbound region when 4G are actually available. Should be fixed in future kernels
   - 0004 only imx8, workaround host limitation, PCI outbound region not large enough to fit LA9310 BAR0-2 windows
  
Compile Linux Kernel and Device Tree
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
::

 cd linux-imx
 export CROSS_COMPILE=aarch64-linux-gnu-
 export ARCH=arm64
 make imx_v8_defconfig
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

Copy modules.tar.gz to the target root directory (/) and extract it.

If needed enable LA9310 EP BAR0-2 [disabled] 
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

user needto ensure la9310 BAR0-2 are enabled before issuing ./load-la9310.sh

::

 root@imx95frdm:# dmesg |grep 1c12
 [    3.413672] pci 0000:01:00.0: [1957:1c12] type 00 class 0x000280 PCIe Endpoint

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


la93xx_firmware (LA9310 FreeRTOS)
.................................

On the LimeSDR‑Micro board, additional clock initialization is required in the LA9310 boot flow. This is specific to LimeSDR‑Micro hardware.
Replace the default NXP LA9310 FreeRTOS image with the LimeSDR‑Micro‑specific firmware.

::

 git clone -b limesdr-micro https://github.com/myriadrf/LimeSuiteNG

 la93xx_freertos firmware can be found under 'firmware/limesdr-micro/la9310.bin'
 copy la9310.bin to the target /lib/firmware/la9310_dfe.bin  

 la93xx_freertos common headers can be found 'drivers/linux/la9310_limesdr/common_headers'
 This will be used for la93xx_host_sw compilation.

la93xx_host_sw
..............


clone la93xx_host_sw linux kernel module source code  
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
::

 git clone  https://github.com/nxp-qoriq/la93xx_host_sw.git
 cd la93xx_host_sw

Apply necessary la93xx_host_sw patches 
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:: 

 cd la93xx_host_sw
 patch -p 1 < 0001-Add-kernel-6.12-support.patch
 patch -p 1 < 0002-Dynamic-allocation-for-iqflood-and-scratch-buffer.patch

.. note::
    - 0001 is required for recent kernels. Should be fixed in future releseases
    - 0002 is optional, experimental alternative to device tree based static reservation of iqflood and scratch buffer regions
 

Recompile the shiva driver against the newly built kernel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

 cd la93xx_host_sw
 git checkout -b imx-la93xx-1.0 imx-la93xx-1.0
 git submodule update --init

 export KERNEL_DIR=<path to linux>
 export LA9310_COMMON_HEADERS=< path to la93xx_freertos/common_headers>
 export LA9310_DRV_HEADER_DIR=< path to la93xx_host_sw/kernel_driver/la9310sdr>
 export CROSS_COMPILE=aarch64-linux-gnu-
 export ARCH=arm64
 make clean
 CONFIG_ENABLE_FLOAT_BYPASS=y make

Deploy images
::

 make install
 scp -r install/* root@<targetIP>:/

 
copy la9310shiva.ko to:
::

 /lib/modules/$(uname -r)/extra/  

pmu_el0_cycle_counter
.....................

iq_mon is relying on standard ARMv8 linux kernel allowing access to CPU perf counter. 
It is deployed by default in our BSP images. Otherwise here how to  compile it
::

 git clone https://github.com/jerinjacobk/armv8_pmu_cycle_counter_el0
 cd armv8_pmu_cycle_counter_el0/
 make CROSS_COMPILE=aarch64-linux-gnu-   ARCH=arm64 KDIR=<PATH_TO_LINUX_KERNEL>

copy pmu_el0_cycle_counter.ko to:
::

 /lib/modules/$(uname -r)/extra/ 


IQ Player
.........

Compile and deploy iqplayer firmware and utilities

Default VSPA tools path:
::

 VSPA_TOOL ?= /opt/VSPA_Tools_vbeta_14_00_781

::

 make
 make install
 scp -r install/* root@<targetIP>:/

Testing 
*******

IQ Player scripts expect default sh as bash shell
if not need to replace script directive 
::

 cd ~/host_utils
 sed -i 's/\/bin\/sh/\/bin\/bash/g' *.sh


load and start VSPA firmware
::

 ./load-la9310.sh

confugre LimeRF using LimeSuiteNG freeRTOS CLI (if available)

play (repeat) waveform from file
::

 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200

capture (one time) iq samples into file
::

 ./iq-capture.sh ./iqdata.bin 1200

capture (repeat) in DDR buffer
::

 ./iq-capture-ddr.sh 1200
 ./iq-stop.sh

Tips 
****

Unexpected imx95frdm going to sleep
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::
 root@imx95frdm:~/host_utils# [  916.073426] fsl_enetc4 0001:00:00.0 eth0: Link is Down
 [  916.419685] PM: suspend entry (deep)
 [  916.437359] Filesystems sync: 0.014 seconds
 [  916.445646] Freezing user space processes

 To disable suspend on a Linux system, you can use the command: 
 
:: 
 sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target. 
 
 After running this command, reboot your system to apply the changes.

