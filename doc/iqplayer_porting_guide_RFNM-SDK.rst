.. sectnum::

**IQ Player Porting Guide -- RFNM Blue with the RFNM SDK**
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Overview
********

This guide describes how to build the RFNM SDK (https://github.com/rfnm/sdk) and how to
replace the RFNM stock radio stack with IQ Player on an RFNM Blue board (i.MX8MP + LA9310).

The LA9310 can run several software stacks. They use different M4 / VSPA firmware and host
software and cannot run at the same time:

.. list-table::
   :header-rows: 1
   :widths: 18 27 25 30

   * - Stack
     - LA9310 firmware (M4 / VSPA)
     - Host side
     - Use it for
   * - Stock RFNM
     - RFNM ``la9310-freertos`` / RFNM VSPA
     - ``la9310rfnm``, librfnm, SoapySDR / GNU Radio
     - RFNM applications, timestamped streaming, RFNM web GUI
   * - Stock LimeSDR-Micro
     - Lime firmware
     - LimeSuiteNG
     - Lime tools and SDR applications (LimeSDR-Micro board only)
   * - IQ Player (this repository)
     - patched RFNM M4 / IQ Player VSPA
     - ``iq_app``, ``libiqplayer``, host-utils; RF set through the RFNM sysfs
     - raw time-domain I/Q capture and replay, open VSPA source
   * - fr1_fr2_test_tool (DFE)
     - NXP ``la93xx_freertos`` (DFE) / 5G DFE VSPA
     - DPDK ``dfe_app``
     - 5G low PHY in the VSPA (symbol sync, cyclic prefix, FFT / iFFT); the host exchanges
       frequency-domain samples. FDD on LimeSDR-Micro, TDD on RFNM

This guide covers IQ Player only. fr1_fr2_test_tool on LimeSDR-Micro is described in
``iqplayer_porting_guide_LimeSDR-Micro.rst``; its RFNM port (including TDD) needs a different
M4 firmware and driver build and will be documented separately.

The stock RFNM image cannot run IQ Player as is:

- its M4 firmware uses VSPA mailbox 0, which IQ Player reserves for the host;
- ``rfnm_qec`` and ``rfnm_agc`` rewrite VSPA data memory and corrupt a foreign VSPA kernel;
- the stock ``la9310shiva.ko`` does not map the IQFLOOD region at the IQ Player address and
  does not provide ``la9310_modem_info``.

IQ Player on RFNM therefore uses:

- the IQ Player VSPA images (``apm-iqplayer-*.eld``) and host-utils (this repository);
- a patched ``la9310shiva.ko`` (RFNM ``la9310-driver``); the ``la9310rfnm`` / ``rfnm_*``
  modules are used unchanged;
- an M4 firmware built with VSPA mailbox 0 released to the host (RFNM ``la9310-freertos``).

All changes are provided as patches in ``doc/patches/`` on top of the RFNM repositories at
the commits given in `Sources`_. They are being upstreamed to RFNM (pull requests to come);
until they are merged, use only the RFNM repositories plus these patches.

Requirements
************

Hardware
--------

- RFNM Blue board on the current RFNM firmware image. A board still on the original factory
  image must first be updated from the web GUI (Settings -> Update); ``rfnm-sdk sysroot``
  and ``deploy`` check this.
- Linux development host, Ubuntu 22.04 or Debian 12, x86-64, with SSH access to the board.
- Optional: a server providing TFTP and NFS for network boot (see `NFS Boot (optional)`_).

Host packages
-------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Package
     - Used for
   * - ``gcc-aarch64-linux-gnu``
     - kernel, modules, librfnm, host-utils
   * - ``gcc-arm-none-eabi``, ``binutils-arm-none-eabi``
     - RFNM SDK M4 / M7 builds (``arm-none-eabi-objcopy``)
   * - ``cmake make git rsync``
     - build system, sources
   * - ``bison flex bc libssl-dev``
     - kernel build
   * - ``pkg-config libspdlog-dev libusb-1.0-0-dev``
     - librfnm (not listed in the SDK README)
   * - ``sshpass``
     - rfnm-sdk when ``password`` is set in ``boards.toml``
   * - ``qemu-user-static``
     - optional, off-board aarch64 tests

::

    sudo apt install -y \
      gcc-aarch64-linux-gnu gcc-arm-none-eabi binutils-arm-none-eabi \
      cmake make git rsync bison flex bc libssl-dev qemu-user-static \
      pkg-config libspdlog-dev libusb-1.0-0-dev sshpass

Toolchains
----------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Toolchain
     - Used for
   * - ``gcc-arm-none-eabi-6-2017-q2-update`` (``ARMGCC_DIR``)
     - IQ Player M4 firmware (validated version)
   * - VSPA tools (``fsvspacc``), e.g. ``VSPA_Tools_vbeta_14_00_781``
     - VSPA ``.eld`` images
   * - ``aarch64-linux-gnu-`` (system package)
     - ``la9310shiva.ko``, host-utils

Conventions
***********

The commands below use these shell variables. Set them once in the development host shell:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Variable
     - Meaning (example)
   * - ``BOARD``
     - board address or ``~/.ssh/config`` alias (``root@rfnm-blue``)
   * - ``WS``
     - workspace holding all source trees (``$HOME/work/la9310``)
   * - ``RFNM_SDK``
     - RFNM SDK clone (``$WS/rfnm-sdk``)
   * - ``KDIR``
     - RFNM kernel tree (``$WS/imx8mp-kernel``)
   * - ``DRIVER``
     - RFNM la9310-driver, patched (``$WS/la9310-driver``)
   * - ``M4SRC``
     - RFNM la9310-freertos, patched (``$WS/la9310-freertos``)
   * - ``IQP``
     - this repository (``$WS/la931x_iqplayer``)
   * - ``ARMGCC_DIR``
     - M4 toolchain (``/opt/gcc-arm-none-eabi-6-2017-q2-update``)
   * - ``NFS_SERVER``, ``NFS_SUBNET``
     - NFS boot only: server IP and board subnet (``192.168.1.10``, ``192.168.1.0/24``)

::

    export WS=$HOME/work/la9310 BOARD=root@rfnm-blue
    export RFNM_SDK=$WS/rfnm-sdk KDIR=$WS/imx8mp-kernel DRIVER=$WS/la9310-driver
    export M4SRC=$WS/la9310-freertos IQP=$WS/la931x_iqplayer
    export ARMGCC_DIR=/opt/gcc-arm-none-eabi-6-2017-q2-update

If the board is behind a jump host, declare it once in ``~/.ssh/config`` (``ProxyJump``) so
that ``ssh``, ``scp`` and ``rsync`` work unchanged.

RFNM SDK
********

This part builds and deploys the stock RFNM stack. It provides the kernel tree, the
sysroot and a known-good board before switching to IQ Player.

Clone and initialize
--------------------

::

    git clone https://github.com/rfnm/sdk $RFNM_SDK
    cd $RFNM_SDK
    ./rfnm-sdk init              # checks host tools, clones manifest.conf into sources/
    ./rfnm-sdk init --plugins    # optional: gr-rfnm, soapy-rfnm, ...

Configure boards.toml
---------------------

::

    cp boards.toml.example boards.toml

::

    [board.blue0]
    ip       = "<board ip>"
    user     = "root"
    password = "rfnm"        # factory default; remove once an SSH key is installed

Install an SSH key on the board (accept the host key on first use)::

    ssh-copy-id -o StrictHostKeyChecking=accept-new $BOARD

After a firmware reflash the board returns to the factory password and may get a new DHCP
address: update ``boards.toml`` and ``~/.ssh/config`` and reinstall the key.

Sysroot, build, deploy
----------------------

::

    ./rfnm-sdk sysroot blue0     # rsync /usr/include, /usr/lib, /lib to sysroot/blue0
    ./rfnm-sdk build             # kernel, modules, m4, m7, librfnm, vsg, webgui, apps

Single components: ``./rfnm-sdk build kernel|modules|m4|m7|librfnm|vsg|webgui|apps``.
After a failed librfnm configure, remove ``sources/librfnm/build-host`` and
``sources/librfnm/build-imx8mp`` before retrying.

Outputs in ``out/``: ``boot/`` (Image, imx8mp-rfnm.dtb), ``modules/`` (``*.ko``),
``firmware/`` (la9310.bin, rfnm_m7_v0.elf), ``rootfs-overlay/``.

::

    ./rfnm-sdk deploy blue0      # rootfs-overlay to /, modules to /rfnm/kernel/
    ssh $BOARD reboot
    ssh $BOARD 'dmesg | grep "hard reset complete"'

NFS Boot (optional)
*******************

With NFS boot the board loads its kernel by TFTP and mounts its rootfs by NFS, so deploying a
file is a local copy and nothing is written to eMMC. The paths below assume the server
exports ``/tftpboot``; adapt them to your server.

Prepare the server
------------------

On the server::

    mkdir -p /tftpboot/blue0 /tftpboot/blue0-rootfs
    echo "/tftpboot/blue0-rootfs $NFS_SUBNET(rw,no_root_squash,no_subtree_check,async,insecure)" \
        | sudo tee -a /etc/exports
    sudo exportfs -ra && showmount -e localhost

``insecure`` is needed because the board NFS client uses a high source port;
``no_root_squash`` lets the board write its rootfs as root.

If the server is remote, it is convenient to mount its ``/tftpboot`` on the development host
(for example ``sshfs <user>@$NFS_SERVER:/tftpboot /srv/tftp -o allow_other,reconnect``).
``/srv/tftp`` is used below for that mount point.

Populate the rootfs and kernel
------------------------------

Clone the running board rootfs::

    rsync -aHxAX --numeric-ids \
        --exclude=/proc/ --exclude=/sys/ --exclude=/dev/ --exclude=/run/ --exclude=/tmp/ \
        $BOARD:/ /srv/tftp/blue0-rootfs/
    mkdir -p /srv/tftp/blue0-rootfs/{proc,sys,dev,run,tmp}

Comment out any ``/`` entry in ``/srv/tftp/blue0-rootfs/etc/fstab`` (it hangs NFS boot)::

    sed -i 's|^\([^#].*\s/\s\)|# \1|' /srv/tftp/blue0-rootfs/etc/fstab

Copy the kernel and DTB as a matched pair from the same build::

    scp $BOARD:/boot/Image            /srv/tftp/blue0/Image
    scp $BOARD:/boot/imx8mp-rfnm.dtb  /srv/tftp/blue0/imx8mp-rfnm.dtb

Switch U-Boot to NFS boot
-------------------------

The RFNM Blue U-Boot console truncates long command lines and mis-parses ``setenv`` inside
script variables. Set the environment from Linux with ``fw_setenv`` and keep ``bootargs`` as
a static variable.

From the board, still booted from eMMC (replace ``$NFS_SERVER`` with its IP)::

    fw_setenv rfnm_run_normal_boot_mmc "$(fw_printenv -n rfnm_run_normal_boot)"
    fw_setenv bootargs "console=ttymxc1,115200 root=/dev/nfs rw nfsroot=$NFS_SERVER:/tftpboot/blue0-rootfs,v3,tcp,nolock ip=dhcp"
    fw_setenv rfnm_run_nfs_boot 'tftp ${loadaddr} blue0/Image; tftp ${fdt_addr_r} blue0/imx8mp-rfnm.dtb; booti ${loadaddr} - ${fdt_addr_r};'
    fw_setenv rfnm_run_normal_boot 'run rfnm_run_nfs_boot;'
    fw_setenv serverip $NFS_SERVER
    fw_setenv bootdelay 3
    reboot

If Linux no longer boots, enter the same variables at the U-Boot prompt one ``setenv`` at a
time, then ``saveenv`` and ``boot``.

Check after reboot::

    cat /proc/cmdline     # contains root=/dev/nfs
    mount | grep ' / '    # type nfs

Restore eMMC boot::

    fw_setenv rfnm_run_normal_boot 'run rfnm_run_normal_boot_mmc;'
    reboot

Kernel update loop::

    cp $KDIR/arch/arm64/boot/Image                         /srv/tftp/blue0/Image
    cp $KDIR/arch/arm64/boot/dts/freescale/imx8mp-rfnm.dtb /srv/tftp/blue0/imx8mp-rfnm.dtb
    ssh $BOARD reboot

IQ Player Stack
***************

Sources
-------

.. list-table::
   :header-rows: 1
   :widths: 22 38 40

   * - Component
     - Repository / branch
     - Pinned commit and patches
   * - la9310-driver
     - https://github.com/rfnm/la9310-driver, ``main``
     - ``c20e58140977ef9e44c3a218bea062f292f65105`` +
       ``doc/patches/la9310-driver/*.patch``
   * - la9310-freertos (M4)
     - https://github.com/rfnm/la9310-freertos, ``main``
     - ``be3536ca4a1b5b604a60572d5c5faea7ab06c3c9`` +
       ``doc/patches/la9310-freertos/*.patch``
   * - imx8mp-kernel
     - https://github.com/rfnm/imx8mp-kernel, ``rfnm_6.6.36``
     - branch head (used for ``Module.symvers`` only)
   * - la9310_debug (optional, VSPA debugger)
     - https://git.6g-for-all.de/6g4all/la9310_debug.git, ``main``
     - ``2328871`` + ``doc/patches/la9310_debug/*.patch``
   * - la931x_iqplayer
     - this repository
     - -

Patches (``doc/patches/``, upstream pull requests to come). 0001-0003 of each set are
the commits of the development forks https://github.com/mxstrk/la9310-driver and
https://github.com/mxstrk/la9310-freertos, exported with ``git format-patch``:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Patch
     - Change
   * - ``la9310-driver/0001-la9310shiva-port-la9310_modinfo-from-NXP-mainline.patch``
     - port ``la9310_modinfo`` from NXP mainline; adds ``uapi/la9310_modinfo.h``, used by
       the IQ Player host-utils (``UAPI_DIR``)
   * - ``la9310-driver/0002-la9310shiva-map-iqflood-at-the-iqplayer-EP-address-0.patch``
     - map IQFLOOD at the IQ Player EP address 0xB0001000
   * - ``la9310-driver/0003-la9310shiva-make-the-NXP-iqplayer-host-stack-work-on.patch``
     - make the NXP IQ Player host stack work on the RFNM driver
   * - ``la9310-driver/0004-la9310shiva-modinfo-mmap-iqflood-carveout.patch``
     - ``la9310_modinfo`` mmap of the IQFLOOD and scratch carveouts (``VM_IO|VM_PFNMAP``);
       needed because ``/dev/mem`` is blocked by ``CONFIG_STRICT_DEVMEM``. The NXP
       ``la93xx_host_sw`` equivalent (also covering the PCIe BARs) is
       ``doc/patches/la93xx_host_sw/0001-*.patch``. The BARs are still mapped through
       ``/dev/mem`` on RFNM (MMIO, allowed with ``CONFIG_STRICT_DEVMEM``); host-utils
       print ``note: ... using /dev/mem`` once
   * - ``la9310-freertos/0001-sw_cmd_engine-LA9310_SW_CMD_VSPA_DBG-proxy-for-the-V.patch``
     - ``LA9310_SW_CMD_VSPA_DBG`` proxy for the VSPA debug block
   * - ``la9310-freertos/0002-avi-let-the-PCIe-host-own-VSPA-mailbox-0-LA9310_HOST.patch``
     - let the PCIe host own VSPA mailbox 0 (``LA9310_HOST_OWNS_MBOX0``)
   * - ``la9310-freertos/0003-avi-rfic-stop-the-MBOX0-refusal-from-flooding-the-ta.patch``
     - stop the MBOX0 refusal from flooding the target log
   * - ``la9310-freertos/0004-dcs-full-rate-cmake-option.patch``
     - ``DCS_FULL_RATE`` cmake option: ``vDcsInit(Full_Freq)`` instead of ``Half_Freq``
   * - ``la9310-freertos/0005-axiq-loopback-cmake-option.patch``
     - ``AXIQ_LOOPBACK`` cmake option: defines ``AXIQ_LOOPBACK_ENABLE``, the M4 enables the
       AXIQ digital loopback at start (see `AXIQ loopback`_)
   * - ``la9310_debug/0001-vdbg-typed-print-of-global-and-static-variables-from.patch``
     - ``vdbg``: typed print of global and static variables from DWARF (needed by 0002)
   * - ``la9310_debug/0002-Add-RFNM-board-support-rfnm-transport.patch``
     - ``rfnm`` transport for ``vdbg``, ``profile`` and the DAP server: VSPA debug requests
       sent as ``LA9310_SW_CMD_VSPA_DBG`` sw_cmds (served by ``la9310-freertos/0001``); see
       `VSPA debugger on RFNM`_
   * - ``la9310_debug/0003-Rename-the-rfnm-transport-to-shiva-support-the-NXP-l.patch``
     - transport renamed ``shiva`` (``rfnm`` kept as alias), NXP la93xx_freertos HIF layout
       (LimeSDR-Micro on the NXP BSP), ``/dev/shiva0`` mmap with ``/dev/mem`` fallback

Optional patch, not applied by the commands below (the ``*.patch`` glob does not enter
``optional/``):

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Patch
     - Change
   * - ``la9310-driver/optional/0001-la9310rfnm-rfnm_rx_park_fault-parameter-to-silence-R.patch``
     - ``la9310rfnm`` ``rfnm_rx_park_fault`` parameter (default 1, stock behaviour);
       ``load-rfnm.sh`` sets it to 0 to drop the RX lane park message and session fault
       (see `Known Traps`_, item 6). Applies on the stock RFNM tree; requires rebuilding
       the ``la9310rfnm`` modules (`la9310rfnm modules`_)

The RFNM repositories do not provide ``uapi/la9310_modinfo.h``; it comes from
``la9310-driver/0001``. ``la9310_host_if.h`` (``LA9310_COMMON_HEADERS``) is in RFNM
``la9310-freertos/common_headers``.

Clone and patch. Use ``git apply --index``: some patches add new files (for example
``sw_cmd_engine/vspa_debug.c``), and without ``--index`` they stay untracked and are deleted
by the ``git clean -xfd .`` of the M4 build::

    git clone https://github.com/rfnm/la9310-driver.git $DRIVER
    git -C $DRIVER checkout c20e58140977ef9e44c3a218bea062f292f65105
    git -C $DRIVER apply --index $IQP/doc/patches/la9310-driver/*.patch

    git clone https://github.com/rfnm/la9310-freertos.git $M4SRC
    git -C $M4SRC checkout be3536ca4a1b5b604a60572d5c5faea7ab06c3c9
    git -C $M4SRC apply --index $IQP/doc/patches/la9310-freertos/*.patch

    git clone --depth 1 -b rfnm_6.6.36 https://github.com/rfnm/imx8mp-kernel.git $KDIR

Kernel tree (Module.symvers)
----------------------------

Out-of-tree modules need ``Module.symvers`` from a full build; ``modules_prepare`` is not
enough. Do not flash this kernel, it is only used to build modules::

    make -C $KDIR ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- imx8mp_rfnm_defconfig
    make -C $KDIR ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image modules

la9310shiva.ko
--------------

::

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -C $KDIR \
        M=$DRIVER/kernel_driver/la9310shiva \
        LA9310_COMMON_HEADERS=$M4SRC/common_headers \
        UAPI_DIR=$DRIVER/uapi \
        COMMON_DIR=$DRIVER/kernel_driver/common \
        CONFIG_ENABLE_FLOAT_BYPASS=y \
        modules

    modinfo -F vermagic $DRIVER/kernel_driver/la9310shiva/la9310shiva.ko
    # must match the board: 6.6.36-rt35-ga13146252326 SMP preempt_rt mod_unload modversions aarch64

la9310rfnm modules
------------------

The ``la9310rfnm`` / ``rfnm_*`` modules are not patched; keep the ones on the board. They
import symbols from ``la9310shiva.ko``: if ``insmod`` fails with
``disagrees about version of symbol`` in dmesg, rebuild them from the unpatched RFNM
``la9310-driver`` (``$RFNM_SDK/sources/la93xx_host_sw``) against the new
``la9310shiva`` ``Module.symvers``. The same build is used with the optional patch
(``git apply`` it in that tree first). The ``LimeSuiteNG`` submodule must be populated
(``./rfnm-sdk init`` does it); in a ``git worktree`` copy, fill it with
``rsync -a --exclude .git $RFNM_SDK/sources/la93xx_host_sw/LimeSuiteNG/ <worktree>/LimeSuiteNG/``
(a symlink does not work with kbuild)::

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -C $KDIR \
        M=$RFNM_SDK/sources/la93xx_host_sw/kernel_driver/la9310rfnm \
        LA9310_DRV_HEADER_DIR=$DRIVER/kernel_driver/la9310shiva \
        LA9310_COMMON_HEADERS=$M4SRC/common_headers \
        UAPI_DIR=$DRIVER/uapi \
        COMMON_DIR=$DRIVER/kernel_driver/common \
        KERNEL_DIR=$KDIR \
        RFNM_BUILD_DIR=$RFNM_SDK/sources \
        DIORA_SDK=/nonexistent ICEWINGS_SDK=/nonexistent \
        KBUILD_EXTRA_SYMBOLS=$DRIVER/kernel_driver/la9310shiva/Module.symvers \
        modules

M4 firmware and DCS clock rate
------------------------------

The M4 firmware must come from the patched RFNM ``la9310-freertos`` (see `Known Traps`_) and be built
with ``LA9310_HOST_OWNS_MBOX0=ON`` so that VSPA mailbox 0 belongs to the host.

The DCS clock, and therefore the ADC/DAC rate, is selected at compile time:

.. list-table::
   :header-rows: 1
   :widths: 30 20 20 30

   * - cmake flags
     - M4 init
     - ADC/DAC rate
     - Output name / host option
   * - ``-DDCS=ON -DDCS_FULL_RATE=OFF``
     - ``vDcsInit(Half_Freq)``
     - 61.44 MSPS
     - ``la9310_dfe_61.bin``, default
   * - ``-DDCS=ON -DDCS_FULL_RATE=ON``
     - ``vDcsInit(Full_Freq)``
     - 122.88 MSPS
     - ``la9310_dfe_122.bin``, ``iq-app-*.sh -f``

All 0T4R and 1T2R use cases run at 61.44 MSPS; 122.88 MSPS is for 1T1R half duplex (see the
reference manual). The rate is selected only by the M4 binary (``la9310.bin`` symlink, see
`Activate IQ Player`_) followed by a reload of ``la9310shiva.ko``.

``LA9310_HOST_OWNS_MBOX0``, ``DCS`` and ``DCS_FULL_RATE`` are read when cmake configures the
tree: always clean and re-run cmake when changing them. Build one rate::

    RATE=61          # or 122
    FULL=$([ $RATE = 122 ] && echo ON || echo OFF)
    cd $M4SRC/Demo/CORTEX_M4_NXP_LA9310_GCC
    git clean -xfd .
    LA9310_HOST_OWNS_MBOX0=ON cmake -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE=./armgcc.cmake \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release \
        -DTURN_ON_STANDALONE_MODE=OFF \
        -DDCS=ON -DDCS_FULL_RATE=$FULL .
    echo "#define TURN_ON_HOST_MODE 1" > include/la9310_boot_mode.h
    make -j$(nproc)
    cp release/la9310.bin $WS/la9310_dfe_$RATE.bin

``ARMGCC_DIR`` must be exported, otherwise ``armgcc.cmake`` stops with an error.

AXIQ loopback
~~~~~~~~~~~~~

On NXP platforms AXIQ digital loopback is enabled at runtime with
``dpdk-dfe_app -c "axiq_lb enable"``. ``dpdk-dfe_app`` is not used on RFNM, so the loopback
is selected when building the M4 firmware: add ``-DAXIQ_LOOPBACK=ON`` to the cmake command
above and save the binary under its own name::

    LA9310_HOST_OWNS_MBOX0=ON cmake ... -DDCS=ON -DDCS_FULL_RATE=$FULL -DAXIQ_LOOPBACK=ON .
    ...
    cp release/la9310.bin $WS/la9310_dfe_${RATE}_lb.bin

The M4 then writes ``DBGGNCR`` (0xE00800EC) = 0x5e at start: AXIQ TX is looped back to RX1
inside the LA9310, the RF front end is not used. Select it with the ``la9310.bin`` symlink
(see `Activate IQ Player`_) and reload ``la9310shiva.ko``. Without the option the binary is
unchanged. This option is build-tested only; it has not yet been checked on a board.

IQ Player VSPA images and host-utils
------------------------------------

IQ Player has two external header dependencies: ``LA9310_COMMON_HEADERS``
(``la9310_host_if.h``) and ``UAPI_DIR`` (``la9310_modinfo.h``). On RFNM, take both from the
patched RFNM trees::

    export LA9310_COMMON_HEADERS=$M4SRC/common_headers
    export UAPI_DIR=$DRIVER/uapi
    cd $IQP
    make
    make install               # optional: CONFIG=<variant> to select apm.eld
    ./deploy.sh <board ip>     # scp -r install/* to the board root

This installs ``/lib/firmware/apm-iqplayer-*.eld`` and ``apm.eld``, ``vspa_mbox``,
``iq_app``, ``iq_mon`` and ``memtool`` in ``/usr/bin``, and the scripts in
``/root/host_utils``.

Optional NXP host tools
-----------------------

``devmem``, ``bin2mem`` and ``la9310_modem_info`` come from ``la93xx_host_sw``
(https://github.com/nxp-qoriq/la93xx_host_sw)::

    for app in devmem bin2mem modem_info; do
        make -C $WS/la93xx_host_sw/app/$app CC=aarch64-linux-gnu-gcc UAPI_DIR=$DRIVER/uapi
    done
    scp $WS/la93xx_host_sw/app/devmem/devmem         $BOARD:/usr/bin/
    scp $WS/la93xx_host_sw/app/bin2mem/bin2mem       $BOARD:/usr/bin/
    scp $WS/la93xx_host_sw/app/modem_info/la9310_modem_info $BOARD:/usr/bin/

Install on the board
--------------------

Back up the stock files, then copy the new ones::

    ssh $BOARD 'cd /rfnm/kernel && { [ -e la9310shiva.ko.stock ] || cp -a la9310shiva.ko la9310shiva.ko.stock; }'
    scp $DRIVER/kernel_driver/la9310shiva/la9310shiva.ko  $BOARD:/rfnm/kernel/
    scp $WS/la9310_dfe_61.bin $WS/la9310_dfe_122.bin      $BOARD:/lib/firmware/

Activate IQ Player
------------------

Disable the RFNM autoloader once, then reboot::

    ssh $BOARD 'systemctl disable rfnm-drivers.service
                mkdir -p /usr/local/etc && echo 0x18000000 > /usr/local/etc/bar0.cfg
                sync && reboot'

After each boot, select the firmware and load the driver. ``la9310shiva.ko`` loads the fixed
names ``la9310.bin`` and ``apm.eld``: ``la9310.bin`` is a symlink, ``apm.eld`` a copy of the
selected ELD (a symlink breaks ``deploy.sh``)::

    ssh $BOARD 'ln -sf /lib/firmware/la9310_dfe_61.bin /lib/firmware/la9310.bin
                rm -f /lib/firmware/apm.eld; cp /lib/firmware/apm-iqplayer-1T1R.eld /lib/firmware/apm.eld
                echo 7 > /proc/sys/kernel/printk
                insmod /rfnm/kernel/la9310shiva.ko \
                    scratch_buf_size=0x4000000 scratch_buf_phys_addr=0x92400000'

For RF front-end control the other ``rfnm_*`` modules can be loaded, except ``rfnm_qec``
and ``rfnm_agc`` (see `Known Traps`_).

RF configuration
----------------

On NXP platforms (LimeSDR-Micro) the RF chain is configured by the M4 through
``dpdk-dfe_app`` (``lime tx 1 freq ...``). On RFNM it is configured by Linux: the
``rfnm_daughterboard`` module exposes one sysfs directory per channel,
``/sys/kernel/rfnm_primary/{tx0,rx0,rx1,...}`` (``rfnm_secondary`` for the second
daughterboard slot). Write the attributes, then ``1`` to ``apply``::

    cd /sys/kernel/rfnm_primary/rx0
    echo on         > enable
    echo sma_a      > path        # sma_a .. sma_h, embed_ant, loopback
    echo 1850000000 > freq        # Hz
    echo 100        > rfic_lpf_bw # MHz
    echo 20         > gain        # dB (TX: power)
    echo 1          > apply

Other attributes: ``agc``, ``bias_tee``, ``fm_notch``, ``rfic_dc_i``, ``rfic_dc_q``,
``freq_min`` / ``freq_max`` (supported range) and ``adc_id`` / ``dac_id`` (LA9310 converter
used by the channel). ``path=loopback`` selects the RF loopback of the daughterboard
(LMS7002 on Lime), not the AXIQ digital loopback.

``host-utils/txrx_en.sh`` (installed in ``/root/host_utils``) applies ready-made settings at
1850 MHz; edit the script or use sysfs directly for other frequencies:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Mode
     - Setting
   * - ``limetx``
     - Lime daughterboard, tx0 on sma_b, 20 MHz LPF, power 10
   * - ``limerx``
     - Lime daughterboard, rx0 on sma_a, 100 MHz LPF, gain 20 dB; re-applies tx0 if it is
       on (Known Trap 7)
   * - ``limetxrx``
     - Lime, rx0 then tx0 (FDD), TX at 1860 MHz (``TX_FREQ`` to change)
   * - ``off``
     - Lime, tx0 off, rx0 re-applied (port back to RX)
   * - ``status``
     - print the rx0 / tx0 settings
   * - ``granitatx``
     - Granita daughterboard, tx0 on sma_b
   * - ``granitatxrx``
     - Granita, tx0 on sma_b and rx1 on sma_a (FDD)
   * - ``granita2R``
     - Granita, rx0 on sma_b and rx1 on sma_a

Go / no-go checks
-----------------

::

    ssh $BOARD 'dmesg | grep "iqplayer IQFLOOD"'
    # ...0xb0001000[H]-0x96400000[M],size 117440512 (win 3)
    ssh $BOARD 'dmesg | grep /dev/shiva0'
    ssh $BOARD 'cat /sys/shiva/shiva_status'
    # modem 0: nlm0 ready
    ssh $BOARD 'vspa_mbox send 0 0 0x11100000 0 && vspa_mbox recv 0 0'
    # Received from VSPA:0, MBox:0, ...   (not "is not responding": mailbox 0 is free)
    ssh $BOARD 'la9310_modem_info | grep FLOOD'
    # 0x96400000 | 0xb0001000 | 0x7000000

Smoke test
----------

The scripts call each other with ``./``; run them from ``/root/host_utils``::

    ssh $BOARD 'cd /root/host_utils && ./txrx_en.sh limetxrx'
    ssh -t $BOARD 'cd /root/host_utils && ./iq-app-rx.sh -X'     # live RX spectrum, q to quit
    ssh -t $BOARD 'cd /root/host_utils && ./iq-app-tx.sh -T'     # interactive tone TX
    ssh $BOARD 'cd /root/host_utils && ./iq-app-rx.sh -D ./rx.bin 4915200'
    ssh -t $BOARD 'cd /root/host_utils && ./iq_mon'              # rates, no overrun counters

In ``-X``, check that ``[CLIP]`` stays off on the saturation line (key ``t`` shows the
I/Q waveform); keep the peak around -6 dBFS with the TX power and RX gain.

RF tuning with rf_tune
----------------------

``rf_tune`` (``host-utils/rf_tune``, installed in ``/usr/bin`` by ``make install`` / ``deploy.sh``)
is an interactive panel on the ``/sys/kernel/rfnm_primary`` sysfs channels, run in a
second terminal next to ``iq-app-rx.sh -X``::

    ssh -t $BOARD rf_tune                                      # panel
    ssh $BOARD rf_tune -s                                      # status
    ssh $BOARD rf_tune -c rx0.gain=26 tx0.power=20
    ssh $BOARD 'cd /root/host_utils && ./txrx_en.sh profile rf_profile_lime.cfg'

Keys: up/down field, tab (or ``,`` ``.``) channel, left/right (or ``-`` ``+``) change,
``[`` ``]`` big step, ``f`` frequency step (1 kHz..10 MHz), digits type a value
(frequency in MHz, ``k`` ``M`` ``G`` suffixes), ``z`` safe state (TX power minimum,
RX gain 0), ``s`` / ``l`` save / load ``rf_profile_<board>.cfg``, ``r`` re-read,
``o`` cabled / over the air (loop budget 40 / 70), ``x`` external attenuation (dB),
``q`` quit. A change is applied 150 ms after the last key. Marks after a value:
``*`` pending, ``~`` held (channel off), ``!`` changed by the driver on apply.
Refusals (``!!``, white on red) and confirmations (``??``, black on yellow) use
background colours; ``-m`` or ``NO_COLOR=1`` turns colours off. The board is taken
from ``-b``, else from the ``RFNM: Loading <board> driver`` kernel message, else
guessed from sysfs.

At start nothing is written. A channel that is on in sysfs is shown live. A
channel that is off is prepared from ``/etc/rf_tune/<board>.cfg`` (built-in
defaults if there is no saved file): its values are held while it stays off and
are written, guards checked, when it is enabled. Edits to an off channel are held
too. Turning a channel off keeps its values for the next enable. ``-n`` starts
from sysfs only.

Defaults, three levels:

- built-in defaults in ``rf_tune.c`` (one table per board, checked against the
  board ranges at start). Lime: rx0 on / sma_a / 1850 MHz / lpf 100 / gain 20,
  tx0 on / sma_b / 1850 MHz / lpf 20 / power 10, the same values as
  ``txrx_en.sh limetxrx`` (kept in sync by hand). Granita: same values. Yucca:
  2600 MHz, gain 20, power 0.
- saved next-run defaults ``/etc/rf_tune/<board>.cfg`` (``RF_TUNE_CFG_DIR``
  to change the directory). Not installed by ``deploy.sh``, so a deploy does not
  overwrite a saved tuning. The previous file is kept as ``.cfg.bak``. ``S`` saves
  the panel values (held values of off channels included) and the ``o`` / ``x``
  settings (``#!ota=``, ``#!ext_att=`` lines; ``-o`` / ``-x`` on the command line
  take precedence). In the panel, ``L`` / ``R`` / ``l`` do not change ``enable``:
  channels that are on apply the loaded values, channels that are off hold them.
- the live sysfs values, shown by the panel.

::

    rf_tune --save            # save the current state as next-run defaults (panel: S)
    rf_tune -d                # apply the saved defaults, built-in if no file (panel: L)
    rf_tune -d tx0.freq=1860M # saved defaults with an override
    rf_tune --reset           # apply the built-in defaults, saved file kept (panel: R + y)
    rf_tune --reset --save    # back to built-in, and save them
    txrx_en.sh defaults       # rf_tune -y -d (echo fallback without rf_tune)
    txrx_en.sh reset          # rf_tune -y --reset

Ranges (from the la9310rfnm driver sources):

.. list-table::
   :header-rows: 1

   * - Board
     - Channels
     - RX gain (dB)
     - TX power
     - Paths
     - Frequency
   * - Lime
     - rx0, tx0
     - -24..30 (<0 uses the 12 / 24 dB attenuators)
     - -60..40 raw ladder (driver accepts 52, 41..52 = 40); PA1 above 0, PA2 above 20
     - rx sma_a / sma_b / embed_ant, tx sma_a / sma_b
     - 10 MHz..3.5 GHz
   * - Granita
     - rx0, rx1, tx0
     - -40..60
     - -60..30
     - rx sma_a / sma_b / embed_ant / loopback, tx sma_b / sma_a / loopback
     - sysfs freq_min / freq_max
   * - Yucca
     - rx0, rx1, tx0
     - 0..77
     - -10..21 (PA step index power + 10, 0..31, uncalibrated)
     - rx0 sma_a, rx1 sma_b, tx sma_b / sma_a
     - 2496..5000 MHz

Front-end guards. The driver only clamps the RX gain, rejects an out-of-range TX power
and, on Lime, keeps the shared antenna switch on TX while tx0 is on (RX attenuators out,
RX gain floored at 0 dB). Nothing in the driver protects the LNA from the board's own
TX. ``rf_tune`` adds:

* loop budget: with TX and RX on, ``rx gain + tx power - external attenuation`` must
  stay <= 40 (``-G`` limit, ``-x`` attenuator dB, ``-o`` over the air adds 30 dB);
  a change that raises the loop above the limit is refused
* confirmation (``y``) when the TX power crosses the PA step (Lime / Granita 20,
  Yucca 15), when a bias-tee is turned on, and for a loopback TX power other than 0
* Granita TX loopback refused until an RX channel is on the loopback path
* Lime apply order: RX apply then tx0 re-apply when tx0 is on, tx0 off then RX
  re-apply (Known Trap 7)
* paths outside the board list refused, values clamped to the ranges above
* at start nothing is applied; a warning is shown when TX and RX are both on

These guards are a convenience, not a hardware protection: for cabled TX to RX tests
use an external attenuator (30 dB or more) and pass it with ``-x``.

See ``iqplayer_reference_manual.rst`` for all options.

Restore the stock RFNM stack
----------------------------

::

    ssh $BOARD 'cd /rfnm/kernel && { [ -e la9310shiva.ko.stock ] && cp -a la9310shiva.ko.stock la9310shiva.ko; }
                rm -f /lib/firmware/la9310.bin /lib/firmware/apm.eld
                systemctl enable rfnm-drivers.service
                reboot'

If the RFNM image shipped a real ``la9310.bin`` file or its own ``apm.eld``, back
them up before the first activation and restore them here, or redeploy with
``./rfnm-sdk deploy blue0``.

VSPA debugger on RFNM
---------------------

Optional. ``la9310_debug`` (``vdbg``, ``profile``, DAP server for VS Code) runs on the
RFNM board with the ``shiva`` transport (``rfnm`` is an alias): attach to the running VSPA, read memory while
streaming, breakpoints, watchpoints, profiling and reload. The requests go to the M4
through the HIF ``sw_cmd_desc`` (``LA9310_SW_CMD_VSPA_DBG``), served by the M4 image built
with ``la9310-freertos/0001`` (part of the standard build above). The host side finds
CCSR/TCML/TCMU through the ``la9310shiva`` modinfo ioctl and maps them through
``/dev/shiva0`` when the driver allows it, else ``/dev/mem`` (PCIe MMIO, allowed with
``CONFIG_STRICT_DEVMEM``).

Clone and patch::

    git clone https://git.6g-for-all.de/6g4all/la9310_debug.git $DBG
    git -C $DBG checkout 2328871
    git -C $DBG apply --index $IQP/doc/patches/la9310_debug/*.patch
    python3 $DBG/tests/test_shiva_port.py     # offline, no board needed

Copy ``$DBG`` to the board (Python 3 is enough; the VSPA toolchain is only needed for
source lines and disassembly), then as root::

    python3 shiva_port.py heartbeat        # M4 answers VSPA debug requests
    python3 shiva_port.py info             # VSPA state snapshot
    python3 vdbg.py --transport shiva --attach /lib/firmware/apm.eld
    python3 profile.py --transport shiva /lib/firmware/apm.eld --seconds 10
    python3 shiva_port.py load /lib/firmware/apm.eld   # reload a hung VSPA

Halting the VSPA (breakpoint, watchpoint) stops the stream; restart with
``shiva_port.py load`` or ``iq-reload-vspa.sh``. Details: ``docs/shiva.md`` in
``la9310_debug``. Status: offline tests against a simulated M4; on-board verification on
RFNM still to be recorded.

Known Traps
***********

1. **M4 source tree.** Do not build the M4 firmware from NXP ``la93xx_freertos``. RFNM
   ``la9310-freertos`` places the host interface at ``0x1C000`` (672 bytes), NXP mainline at ``0x1B000``
   (808 bytes). The driver uses the RFNM layout; an NXP M4 binary gives ``probe failed -22``
   and a hung PCIe link (power cycle needed). Use the same ``$M4SRC/common_headers`` for the
   driver and host-utils builds.

2. **cmake flags.** ``LA9310_HOST_OWNS_MBOX0``, ``DCS`` and ``DCS_FULL_RATE`` are read at
   configure time; ``make LA9310_HOST_OWNS_MBOX0=ON`` alone silently builds an unpatched
   binary. Run ``git clean -xfd .`` and cmake again for every variant.

3. **No alternate firmware names.** ``alt_firmware_name`` / ``alt_vspa_fw_name`` are not
   implemented in the RFNM ``la9310shiva.ko`` (``unknown parameter ... ignored``). Use the
   ``la9310.bin`` symlink and the ``apm.eld`` copy.

4. **rfnm_qec and rfnm_agc.** They poll and rewrite VSPA parameter DMEM and corrupt the IQ
   Player kernel. ``kpage_ncache``, ``rfnm_gpio``, ``la9310rfnm``, ``rfnm_lalib``,
   ``rfnm_daughterboard`` and ``rfnm_lime`` are safe.

5. **vermagic.** Modules must be built against ``rfnm_6.6.36``; check ``modinfo -F
   vermagic`` against ``uname -r`` on the board before installing.

6. **RFNM messages on the console during captures.** With the stock ``la9310rfnm.ko``,
   ``iq-app-rx.sh`` prints repeatedly on the UART console::

       phytimer: fw parked an RX lane after overrun (cnt N)

   ``la9310rfnm`` monitors ``rx_regate_cnt``, an RFNM stream counter that has no meaning
   with the IQ Player VSPA kernel: the message does not indicate IQ Player sample loss
   (use ``iq_mon`` for that). ``load-rfnm.sh`` sets console log level 7, so the warning
   reaches the UART and mixes with the ``-X`` display. Workarounds:

   - apply the optional patch ``la9310-driver/optional/0001`` and rebuild the
     ``la9310rfnm`` modules; ``load-rfnm.sh`` then sets ``rfnm_rx_park_fault=0``;
   - or, without patch, lower the console level before the capture: ``dmesg -n 4``
     (messages stay in ``dmesg``);
   - or run the interactive tools over ``ssh`` instead of the UART console.

7. **Lime: RX apply while TX is on.** ``rfnm_lime`` leaves the front-end port to TX
   when rx0 is applied with tx0 on: TX to RX coupling rises by about 35 dB and the RX
   floor by about 15 dB (measured with TX at RX LO + 10 MHz: TX line -40 dBFS after a TX
   apply, -6 dBFS after a later RX apply). A TX apply restores the switches. Apply RX
   before TX (``limetxrx``); ``limerx`` re-applies tx0 when it is on. ``off`` turns TX
   off and gives the port back to RX.

References
**********

- RFNM SDK: https://github.com/rfnm/sdk (``README.md``)
- RFNM kernel: https://github.com/rfnm/imx8mp-kernel
- RFNM la9310-driver: https://github.com/rfnm/la9310-driver
- RFNM la9310-freertos: https://github.com/rfnm/la9310-freertos
- development forks (source of ``doc/patches/`` 0001-0003): https://github.com/mxstrk/la9310-driver,
  https://github.com/mxstrk/la9310-freertos
- ``iqplayer_reference_manual.rst``, ``iqplayer_design_notes.rst``

Annex A: Example build on a development host
********************************************

A complete build and deploy, as run on one development host. Paths and board name are
examples; adapt them. The RFNM SDK was already initialized (``./rfnm-sdk init`` and
``./rfnm-sdk build``), so ``sources/imx8mp-kernel`` has its ``.config`` and
``Module.symvers``. The board is reached as ``$BOARD``, an ``~/.ssh/config`` alias with
an SSH key installed (add ``ProxyJump`` if the board is behind a jump host)::

    Host rfnm-blue
        HostName <board ip>
        User root

Environment
-----------

Set ``S`` in its own ``export`` line: in ``export A=x B=$A`` the second variable does not
see the first one::

    export S=$HOME/work/rfnm-sdk/sources
    export IQP=$HOME/work/la931x_iqplayer
    export KDIR=$S/imx8mp-kernel
    export DRIVER=$S/la93xx_host_sw-iqp       # patched copy of RFNM la9310-driver
    export M4SRC=$S/la93xx_freertos-iqp       # patched copy of RFNM la9310-freertos
    export ARMGCC_DIR=/opt/gcc-arm-none-eabi-6-2017-q2-update
    export LA9310_COMMON_HEADERS=$M4SRC/common_headers
    export UAPI_DIR=$DRIVER/uapi
    export OUT=/tmp/iqp_rfnm_out
    export BOARD=rfnm-blue
    mkdir -p $OUT

Patched source trees
--------------------

The SDK clones in ``sources/`` are left untouched; ``git worktree`` creates patched copies
on a local branch::

    git -C $S/la93xx_host_sw   worktree add -b iqplayer-v1.2 $DRIVER c20e581
    git -C $S/la93xx_freertos  worktree add -b iqplayer-v1.2 $M4SRC  be3536c
    git -C $DRIVER apply --index $IQP/doc/patches/la9310-driver/*.patch
    git -C $DRIVER commit -qm "iqplayer patches"
    git -C $M4SRC  apply --index $IQP/doc/patches/la9310-freertos/*.patch
    git -C $M4SRC  commit -qm "iqplayer patches"

To update the patches later: ``git -C $DRIVER reset --hard c20e581`` (``be3536c`` for
``$M4SRC``) and apply again.

la9310shiva.ko
--------------

::

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -C $KDIR \
        M=$DRIVER/kernel_driver/la9310shiva \
        LA9310_COMMON_HEADERS=$LA9310_COMMON_HEADERS UAPI_DIR=$UAPI_DIR \
        COMMON_DIR=$DRIVER/kernel_driver/common CONFIG_ENABLE_FLOAT_BYPASS=y modules
    modinfo -F vermagic $DRIVER/kernel_driver/la9310shiva/la9310shiva.ko
    # 6.6.36-rt35-ga13146252326 SMP preempt_rt mod_unload modversions aarch64
    cp $DRIVER/kernel_driver/la9310shiva/la9310shiva.ko $OUT/

M4 firmware, both rates
-----------------------

``load-rfnm.sh`` requires ``/lib/firmware/la9310_dfe_122.bin``, so build both::

    for RATE in 61 122; do
        FULL=$([ $RATE = 122 ] && echo ON || echo OFF)
        cd $M4SRC/Demo/CORTEX_M4_NXP_LA9310_GCC
        git clean -qxfd .
        LA9310_HOST_OWNS_MBOX0=ON cmake -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE=./armgcc.cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            -DCMAKE_BUILD_TYPE=Release -DTURN_ON_STANDALONE_MODE=OFF \
            -DDCS=ON -DDCS_FULL_RATE=$FULL .
        echo "#define TURN_ON_HOST_MODE 1" > include/la9310_boot_mode.h
        make -j$(nproc) && cp release/la9310.bin $OUT/la9310_dfe_$RATE.bin
    done

For the AXIQ loopback variant add ``-DAXIQ_LOOPBACK=ON`` and copy to
``$OUT/la9310_dfe_${RATE}_lb.bin``.

IQ Player
---------

::

    cd $IQP
    make clean
    make
    make CONFIG=1T1R install
    ls $OUT install/lib/firmware

Deploy
------

Back up everything that is overwritten, then copy::

    F=$(cd $IQP/install && find . -type f | sed 's|^\./||' | tr '\n' ' ')
    ssh $BOARD "cd / && tar czf /root/iqp_backup_\$(date +%Y%m%d_%H%M).tgz --ignore-failed-read $F
                     cd /rfnm/kernel && cp -n la9310shiva.ko la9310shiva.ko.prev
                     cd /lib/firmware && for f in la9310*.bin; do cp -n \$f \$f.prev; done"
    scp $OUT/la9310shiva.ko $BOARD:/rfnm/kernel/
    scp $OUT/la9310_dfe_61.bin $OUT/la9310_dfe_122.bin $BOARD:/lib/firmware/
    ssh $BOARD 'ln -sf /lib/firmware/la9310_dfe_61.bin /lib/firmware/la9310.bin; sync'
    cd $IQP && ./deploy.sh $BOARD

``la9310_dfe_61.bin`` is used for every variant except 1T1R at 122.88 MSPS (link
``la9310_dfe_122.bin`` instead).

Bring-up and check
------------------

On the board (RFNM autoloader disabled, see `Activate IQ Player`_)::

    cd /root/host_utils
    ./load-rfnm.sh
    dmesg | grep -E "iqplayer IQFLOOD|disagrees about version"
    la9310_modem_info
    ./txrx_en.sh limerx
    ./iq-reload-vspa.sh /lib/firmware/apm-iqplayer-1T2R.eld   # runtime switch to 1T2R
    iq_trace                                                   # BUILD_VER of the new image

If a stock ``la9310rfnm`` / ``rfnm_*`` module reports ``disagrees about version of symbol``,
rebuild it as in `la9310rfnm modules`_.

Optional: silence the RX lane park message
-------------------------------------------

See `Known Traps`_, item 6. Build the ``la9310rfnm`` modules from a stock copy with the
optional patch, against the new ``la9310shiva`` symbols::

    export RFNMOPT=/tmp/rfnm_park_wt
    git -C $S/la93xx_host_sw worktree add --detach $RFNMOPT c20e581
    git -C $RFNMOPT apply --index $IQP/doc/patches/la9310-driver/optional/*.patch
    rm -rf $RFNMOPT/LimeSuiteNG && mkdir $RFNMOPT/LimeSuiteNG
    rsync -a --exclude .git $S/la93xx_host_sw/LimeSuiteNG/ $RFNMOPT/LimeSuiteNG/
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -C $KDIR \
        M=$RFNMOPT/kernel_driver/la9310rfnm \
        LA9310_DRV_HEADER_DIR=$DRIVER/kernel_driver/la9310shiva \
        LA9310_COMMON_HEADERS=$LA9310_COMMON_HEADERS UAPI_DIR=$UAPI_DIR \
        COMMON_DIR=$DRIVER/kernel_driver/common KERNEL_DIR=$KDIR RFNM_BUILD_DIR=$S \
        DIORA_SDK=/nonexistent ICEWINGS_SDK=/nonexistent \
        KBUILD_EXTRA_SYMBOLS=$DRIVER/kernel_driver/la9310shiva/Module.symvers modules
    modinfo -p $RFNMOPT/kernel_driver/la9310rfnm/la9310rfnm.ko | grep park

Deploy the modules that ``load-rfnm.sh`` loads, keeping ``.prev`` copies::

    M="la9310rfnm rfnm_gpio rfnm_lalib rfnm_daughterboard rfnm_lime"
    ssh $BOARD "cd /rfnm/kernel && for m in $M; do cp -n \$m.ko \$m.ko.prev; done"
    for m in $M; do scp $RFNMOPT/kernel_driver/la9310rfnm/$m.ko $BOARD:/rfnm/kernel/; done

After ``./load-rfnm.sh``, ``cat /sys/module/la9310rfnm/parameters/rfnm_rx_park_fault``
returns 0.

Rollback
--------

::

    ssh $BOARD 'cd /rfnm/kernel && for f in *.ko.prev; do cp -a $f ${f%.prev}; done
                     cd /lib/firmware && for f in la9310*.bin.prev; do cp -a $f ${f%.prev}; done
                     cd / && tar xzf /root/iqp_backup_<date>.tgz && sync'
