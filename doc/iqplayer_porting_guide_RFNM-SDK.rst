.. sectnum::

**RFNM SDK -- Build and Bringup Guide for NXP Lab (RFNM Blue behind lab-jump)**
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Overview
********

This document describes how to clone, build and deploy the RFNM SDK
(https://github.com/rfnm/sdk) from an NXP lab development machine onto an
RFNM Blue board that sits behind the NXP lab jump server (lab-jump).

The RFNM Blue is an i.MX8MP-based software-defined radio motherboard hosting
an NXP LA9310 modem SoC. The SDK builds the complete software stack:

- Linux 6.6 kernel + device tree for i.MX8MP
- LA9310 kernel modules (RF datapath, daughterboard drivers, QEC, AGC)
- LA9310 Cortex-M4 firmware (``la9310.bin``)
- i.MX8MP Cortex-M7 front-end firmware (``rfnm_m7_v0.elf``)
- librfnm (host + aarch64 cross build)
- vsg, webgui daemons, on-board apps

References:

- RFNM SDK README: ``README.md`` at the SDK root
- RFNM SDK repository: https://github.com/rfnm/sdk

Hardware Requirements
*********************

- RFNM Blue board (i.MX8MP, 4x Cortex-A53) running the current firmware image.
  If the board is still on the original factory image, update it first via the
  web GUI (Settings -> Update) before running any SDK command -- ``sysroot`` and
  ``deploy`` enforce this check.
- NXP lab development machine with Ubuntu 22.04 / Debian 12 (x86-64).
- Network access to the NXP lab jump server (lab-jump, 10.161.204.76).

Host Dependency Installation
*****************************

The README lists the required packages. In practice the following additional
packages were also needed on Ubuntu 22.04 and must be installed before
building:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Package
     - Why needed
   * - ``gcc-aarch64-linux-gnu``
     - aarch64 cross compiler (kernel, modules, librfnm, apps)
   * - ``gcc-arm-none-eabi``
     - Cortex-M4 / M7 bare-metal compiler
   * - ``binutils-arm-none-eabi``
     - Provides ``arm-none-eabi-objcopy`` (M7 build)
   * - ``cmake make git rsync``
     - Build system + source management
   * - ``bison flex bc libssl-dev``
     - Kernel build dependencies
   * - ``qemu-user-static``
     - Optional: off-board aarch64 selftests
   * - ``pkg-config``
     - Required by librfnm CMakeLists (not in README)
   * - ``libspdlog-dev``
     - Required by librfnm (not in README)
   * - ``libusb-1.0-0-dev``
     - Required by librfnm (not in README)
   * - ``sshpass``
     - Required by rfnm-sdk when ``password`` is set in boards.toml

Install everything at once::

    sudo apt install -y \
      gcc-aarch64-linux-gnu gcc-arm-none-eabi binutils-arm-none-eabi \
      cmake make git rsync bison flex bc libssl-dev qemu-user-static \
      pkg-config libspdlog-dev libusb-1.0-0-dev sshpass


Clone the SDK
*************

::

    git clone https://github.com/rfnm/sdk ~/github/rfnm-sdk
    cd ~/github/rfnm-sdk

The SDK README is at ``README.md``. Read it for the full component list and
optional plugin details.


Initialize: clone all component repositories
*********************************************

::

    ./rfnm-sdk init

This checks that all host tools are present, then clones every component
listed in ``manifest.conf`` into ``sources/``. Optional SDR framework plugins
(gr-rfnm, soapy-rfnm, etc.) can be added with::

    ./rfnm-sdk init --plugins


Configure boards.toml
*********************

Copy the example file and edit it::

    cp boards.toml.example boards.toml

Minimal configuration for the NXP lab board::

    [board.blue0]
    ip       = "172.16.0.181"
    user     = "root"
    password = "rfnm"        # board default; remove once SSH key is deployed

Note: ``rfnm-sdk`` uses ``sshpass`` to pass the password transparently to SSH
and rsync. Once the SSH key is installed on the board (see SSH Access Setup
above) the ``password`` line can be commented out.


Pull the cross sysroot from the board
***************************************

::

    ./rfnm-sdk sysroot blue0

This rsyncs ``/usr/include``, ``/usr/lib``, and ``/lib`` from the live board
into ``sysroot/blue0/`` on the host and creates a ``sysroot/current``
symlink. All subsequent cross builds pick this up automatically.

If the board image is too old this command will abort with an error asking you
to update the firmware via the web GUI first.


Build the full stack
********************

::

    ./rfnm-sdk build

This builds in order: kernel, LA9310 modules, M4 firmware, M7 firmware,
librfnm (host + aarch64 cross), vsg, webgui daemons, on-board apps.
Artifacts land in ``out/``:

- ``out/boot/``            -- kernel Image + imx8mp-rfnm.dtb
- ``out/modules/``         -- LA9310 ``*.ko`` files
- ``out/firmware/``        -- la9310.bin, rfnm_m7_v0.elf
- ``out/rootfs-overlay/``  -- everything that rsyncs to the board rootfs

Individual components can be built in isolation::

    ./rfnm-sdk build kernel
    ./rfnm-sdk build modules
    ./rfnm-sdk build m4
    ./rfnm-sdk build m7
    ./rfnm-sdk build librfnm
    ./rfnm-sdk build vsg
    ./rfnm-sdk build webgui
    ./rfnm-sdk build apps

If a build directory has stale CMake state from a previous failed run, clean
it before retrying::

    rm -rf sources/librfnm/build-host sources/librfnm/build-imx8mp
    ./rfnm-sdk build librfnm


Deploy onto the board
*********************

::

    ./rfnm-sdk deploy blue0

The script shows what it will copy and asks for confirmation. It rsyncs
``out/rootfs-overlay/`` to the board root and ``out/modules/`` to
``/rfnm/kernel/``. Reboot the board after deploying::

    ssh root@172.16.0.139 reboot

Wait for the driver ready marker in dmesg before using the radio::

    ssh root@172.16.0.139 'dmesg | grep "hard reset complete"'


Known Issues and Extra Steps (NXP Lab)
***************************************

Missing host packages not listed in the SDK README
...................................................

The SDK README lists ``sshpass`` and the basic build tools, but the following
are also required and were not installed by default on the NXP lab Ubuntu 22.04
machine:

- ``pkg-config`` -- CMake FindPkgConfig module cannot run without this.
- ``libspdlog-dev`` -- librfnm CMakeLists.txt requires spdlog headers.
- ``libusb-1.0-0-dev`` -- librfnm CMakeLists.txt requires libusb-1.0.
- ``binutils-arm-none-eabi`` -- provides ``arm-none-eabi-objcopy`` needed by
  the M7 firmware CMake build. Without it the M7 build fails at the ELF
  post-processing step.

See the dependency table and the single ``apt install`` command above.

Board SSH via lab-jump: host key verification failure
......................................................

When running ``ssh-copy-id`` for the first time through ProxyJump, SSH may
refuse with ``Host key verification failed`` if the board key is not yet in
``~/.ssh/known_hosts``. Pass ``-o StrictHostKeyChecking=accept-new`` to
accept it automatically::

    ssh-copy-id -o ProxyJump=lab-jump -o StrictHostKeyChecking=accept-new root@172.16.0.139

Board password after firmware reflash
......................................

After reflashing the board firmware the board reverts to its factory defaults.
The default root password is ``rfnm``. Set ``password = "rfnm"`` in
``boards.toml`` and ensure ``sshpass`` is installed so the SDK can
authenticate without interactive prompts. Re-deploy your SSH key after
reflashing to restore passwordless access.

Reflash / IP change
...................

After reflashing, the board may receive a different DHCP address. Update the
following files whenever the board IP changes:

1. ``~/.ssh/config`` -- the ``Host <ip>`` stanza
2. ``~/github/rfnm-sdk/boards.toml`` -- the ``ip`` field under ``[board.blue0]``


NFS Boot Developer Workflow
***************************

NFS boot is the recommended development mode for the NXP lab. The board loads
its kernel over TFTP and mounts its rootfs over NFS from lab-jump. Deploying
any file becomes a local write; no eMMC writes are ever needed.

Architecture
............

::

    Development machine  --sshfs-->  lab-jump:/tftpboot  (mounted at /srv/tftp)
                                          |
                          TFTP            |   NFS
                          blue0/Image     +---> blue0-rootfs/
                          blue0/imx8mp-rfnm.dtb

    board (blue0)  <-- TFTP + NFS --  lab-jump (10.161.204.76)
                       serverip=10.161.204.76
                       nfsroot=10.161.204.76:/tftpboot/blue0-rootfs

Step 1 -- Mount lab-jump:/tftpboot via sshfs
............................................

On the development machine (one-time setup)::

    sudo apt install -y sshfs
    sudo mkdir -p /srv/tftp

Make the mount persistent using a systemd mount unit. Create
``/etc/systemd/system/srv-tftp.mount`` with the following content::

    [Unit]
    Description=sshfs mount lab-jump:/tftpboot at /srv/tftp
    After=network-online.target
    Wants=network-online.target

    [Mount]
    What=nxp@10.161.204.76:/tftpboot
    Where=/srv/tftp
    Type=fuse.sshfs
    Options=defaults,_netdev,allow_other,reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,IdentityFile=/home/nxa15228/.ssh/id_ed25519,StrictHostKeyChecking=no

    [Install]
    WantedBy=multi-user.target

Then enable and start it::

    sudo systemctl daemon-reload
    sudo systemctl enable --now srv-tftp.mount
    systemctl status srv-tftp.mount

.. note::

   ``/etc/fuse.conf`` must contain ``user_allow_other`` (uncommented) for
   ``allow_other`` to work. On Ubuntu this is present by default.

Verify::

    ls /srv/tftp

Step 2 -- Create directories on lab-jump
.........................................

::

    ssh lab-jump "mkdir -p /tftpboot/blue0 /tftpboot/blue0-rootfs"

Step 3 -- Populate the NFS rootfs by cloning the live board
............................................................

Run from the development machine (rsync tunnels through ProxyJump)::

    rsync -aHxAX --numeric-ids \
        --exclude=/proc/ --exclude=/sys/ --exclude=/dev/ \
        --exclude=/run/  --exclude=/tmp/ \
        -e "ssh -J lab-jump" \
        root@172.16.0.139:/ /srv/tftp/blue0-rootfs/
    mkdir -p /srv/tftp/blue0-rootfs/{proc,sys,dev,run,tmp}

Fix ``/etc/fstab`` inside the rootfs -- comment out any line that remounts
``/`` from eMMC (it hangs NFS boot)::

    sed -i 's|^\([^#].*\s/\s\)|\# \1|' /srv/tftp/blue0-rootfs/etc/fstab

Step 4 -- Copy kernel and DTB to the TFTP directory
....................................................

::

    scp -J lab-jump root@172.16.0.139:/boot/Image \
        /srv/tftp/blue0/Image
    scp -J lab-jump root@172.16.0.139:/boot/imx8mp-rfnm.dtb \
        /srv/tftp/blue0/imx8mp-rfnm.dtb

Always copy ``Image`` and ``imx8mp-rfnm.dtb`` as a matched pair from the same
build -- a mismatched DTB produces subtle half-working hardware failures.

Step 5 -- Configure NFS exports on lab-jump
...........................................

SSH to lab-jump and add the export::

    ssh lab-jump
    echo '/tftpboot/blue0-rootfs 172.16.0.0/24(rw,no_root_squash,no_subtree_check,async,insecure)' \
        | sudo tee -a /etc/exports
    sudo exportfs -ra
    sudo showmount -e localhost   # confirm the export is visible

The ``insecure`` flag is required because the board NFS client uses a
high-numbered source port. ``no_root_squash`` is required so the board can
write to its own rootfs as root.

Step 6 -- Switch U-Boot to NFS boot
....................................

.. note::

   **U-Boot console quirk on RFNM Blue**: long strings containing semicolons,
   ``${...}`` expansions, or quotes typed at the U-Boot prompt may be silently
   truncated or mis-parsed. Use ``fw_setenv`` from Linux (method A below) --
   it bypasses the console entirely and is the reliable path. Only fall back to
   the U-Boot prompt (method B) if the board cannot boot Linux at all.

   In particular, **do not embed** ``setenv bootargs ...`` inside
   ``rfnm_run_nfs_boot``. That pattern worked in the upstream docs but fails
   here because ``setenv`` inside a script variable is mis-parsed by this
   U-Boot build. The fix is to store ``bootargs`` as a plain static variable.

**Method A (preferred) -- fw_setenv from eMMC Linux**

While the board is still running from eMMC, SSH in::

    ssh -J lab-jump root@172.16.0.139

Save the factory eMMC boot command and configure NFS boot::

    # Save the factory eMMC boot command so you can restore it later
    MMC_BOOT=$(fw_printenv -n rfnm_run_normal_boot)
    fw_setenv rfnm_run_normal_boot_mmc "$MMC_BOOT"

    # Store bootargs as a static variable -- no setenv at runtime
    fw_setenv bootargs "console=ttymxc1,115200 root=/dev/nfs rw nfsroot=10.161.204.76:/tftpboot/blue0-rootfs,v3,tcp,nolock ip=dhcp"

    # Simplified boot script: TFTP + booti only, no setenv
    fw_setenv rfnm_run_nfs_boot 'tftp ${loadaddr} blue0/Image; tftp ${fdt_addr_r} blue0/imx8mp-rfnm.dtb; booti ${loadaddr} - ${fdt_addr_r};'

    fw_setenv rfnm_run_normal_boot 'run rfnm_run_nfs_boot;'
    fw_setenv serverip 10.161.204.76
    fw_setenv bootdelay 3
    reboot

**Method B -- U-Boot prompt (fallback, one command at a time)**

If Linux cannot boot, catch the U-Boot prompt during the bootdelay window
and enter each command separately::

    setenv bootargs console=ttymxc1,115200 root=/dev/nfs rw nfsroot=10.161.204.76:/tftpboot/blue0-rootfs,v3,tcp,nolock ip=dhcp
    setenv rfnm_run_nfs_boot 'tftp ${loadaddr} blue0/Image; tftp ${fdt_addr_r} blue0/imx8mp-rfnm.dtb; booti ${loadaddr} - ${fdt_addr_r};'
    setenv rfnm_run_normal_boot 'run rfnm_run_nfs_boot;'
    setenv serverip 10.161.204.76
    setenv bootdelay 3
    saveenv
    boot

After rebooting, verify NFS root is active::

    cat /proc/cmdline    # must contain root=/dev/nfs
    mount | grep ' / '   # must show type nfs

Step 7 -- Restore eMMC boot if needed
......................................

::

    fw_setenv rfnm_run_normal_boot 'run rfnm_run_normal_boot_mmc;'
    reboot

Kernel Publish Loop (after NFS boot is active)
...............................................

Once NFS boot is set up, deploying a new kernel takes three commands::

    cp sources/linux/arch/arm64/boot/Image              /srv/tftp/blue0/Image
    cp sources/linux/arch/arm64/boot/dts/freescale/imx8mp-rfnm.dtb \
                                                        /srv/tftp/blue0/imx8mp-rfnm.dtb
    ssh root@172.16.0.139 reboot

No board-side install, no eMMC writes. The rootfs in ``/srv/tftp/blue0-rootfs/``
is also the cross-compile sysroot -- ``./rfnm-sdk sysroot blue0`` will keep it
in sync after any board-side package updates.


U-Boot Console Buffer Fix (Future Work)
****************************************

.. note::

   This section describes a **future improvement** that has not yet been
   attempted on this board. The NFS boot workaround (static ``bootargs``
   variable set via ``fw_setenv``) is the current working solution. Apply
   this fix only if direct U-Boot console interaction with long strings is
   required.

Root Cause
..........

The U-Boot hush shell on RFNM Blue silently truncates or mis-parses command
strings longer than ``CONFIG_SYS_CBSIZE`` bytes (typically 256 or 512 in the
NXP i.MX8MP default defconfig). This is why ``setenv bootargs "..."`` typed at
the console, or embedded inside a ``run`` variable, corrupts the bootargs
string. The fix is to increase the console line buffer in the U-Boot defconfig
and reflash U-Boot.

Step 1 -- Locate the RFNM U-Boot fork
......................................

RFNM almost certainly maintains a fork of NXP's ``uboot-imx`` repository. Check::

    # Look for a uboot-imx entry in the SDK manifest
    grep -i uboot ~/github/rfnm-sdk/manifest.conf

    # Check the RFNM GitHub organisation directly
    # https://github.com/rfnm -- look for a repo named uboot-imx or similar

Clone it once found::

    git clone https://github.com/rfnm/uboot-imx ~/github/rfnm-uboot
    cd ~/github/rfnm-uboot

Identify the correct defconfig::

    ls configs/ | grep -i rfnm
    # Expected: rfnm_imx8mp_defconfig  (or similar)

Step 2 -- Increase the console buffer
......................................

Edit the defconfig (e.g. ``configs/rfnm_imx8mp_defconfig``) and add or change::

    CONFIG_SYS_CBSIZE=2048
    CONFIG_SYS_PBSIZE=2080

``CONFIG_SYS_PBSIZE`` must be at least ``CONFIG_SYS_CBSIZE + prompt overhead``
(prompt is typically ~16 chars).

Step 3 -- Build U-Boot
......................

Cross-compile for i.MX8MP using the same aarch64 toolchain already installed
for rfnm-sdk::

    export CROSS_COMPILE=aarch64-linux-gnu-
    export ARCH=arm64

    make rfnm_imx8mp_defconfig   # substitute actual defconfig name
    make -j$(nproc)

The build produces ``flash.bin`` (or ``u-boot-nodtb.bin`` + SPL depending on
the NXP build system version). Verify::

    ls -lh flash.bin u-boot.bin 2>/dev/null

Step 4 -- Flash U-Boot to eMMC
...............................

.. warning::

   Writing the wrong image or to the wrong device/offset will brick the board.
   Double-check the device node (``mmcblk2`` is the standard eMMC on i.MX8MP)
   and the offset (``seek=33`` = 33 kB = standard i.MX8MP SPL offset) before
   running ``dd``.

   Have the recovery procedure (USB serial download mode via ``uuu``) ready
   before proceeding.

Copy the binary to the board and flash::

    # Copy to board
    scp -J lab-jump flash.bin root@172.16.0.181:/tmp/flash.bin

    # SSH to board and flash (33 kB offset = standard i.MX8MP U-Boot offset)
    ssh -J lab-jump root@172.16.0.181
    dd if=/tmp/flash.bin of=/dev/mmcblk2 bs=1k seek=33 conv=fsync
    sync
    reboot

Step 5 -- Verify at the U-Boot prompt
......................................

Interrupt the boot and test a long string::

    setenv testvar "console=ttymxc1,115200 root=/dev/nfs rw nfsroot=10.161.204.76:/tftpboot/blue0-rootfs,v3,tcp,nolock ip=dhcp"
    printenv testvar

If the full string is printed back untruncated, the fix is working. Proceed to
reprogram the NFS boot env vars (see Step 6 of the NFS Boot section above) --
this time the embedded ``setenv bootargs`` form inside a run-script will work
correctly.

Recovery (USB serial download / uuu)
.....................................

If the board does not boot after flashing, recover via NXP ``uuu`` (Universal
Update Utility)::

    sudo apt install -y uuu   # or build from https://github.com/nxp-imx/mfgtools
    # Enter download mode: hold BOOT_MODE button while powering on (check RFNM HW guide)
    uuu -b emmc_all flash.bin


IQ Player Stack: Native Build and Deploy
*****************************************

Overview
........

The IQ Player stack replaces the RFNM stock VSPA firmware with an NXP
IQ-streaming kernel, patches the ``la9310shiva.ko`` driver with four streaming
fixes, and rebuilds the Cortex-M4 firmware with VSPA mailbox 0 masked from the
M4 so that the host can drive it directly.

The definitive reference is::

    ~/bitbucket/LA9310/la931x_iqplayer_rfnm-iqplayer-bringup/doc/rfnm_iqplayer_bringup_guide.md

This section documents the full native (Docker-free) build and deploy procedure
as used in the NXP lab.

Prerequisites
.............

The following toolchains must be installed at fixed paths (the build scripts
expect these exact locations):

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Toolchain
     - Required for
   * - ``/opt/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi``
     - Cortex-M4 firmware (``arm-none-eabi-gcc 13.2.1``)
   * - ``/opt/VSPA_Tools_vbeta_14_00_781`` (``fsvspacc``)
     - VSPA ``.eld`` image assembly
   * - ``aarch64-linux-gnu-`` (system cross toolchain)
     - ``la9310shiva.ko``, host-utils

The NFS boot workflow (see `NFS Boot Developer Workflow`_ above) must be active
so that the board is reachable as ``root@172.16.0.181`` via ProxyJump.

Repository Layout
.................

All IQ Player source trees live under ``~/bitbucket/LA9310/``:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Directory
     - Content
   * - ``la931x_iqplayer_rfnm-iqplayer-bringup/``
     - Build wrappers, ``rfnm-bringup/`` scripts, and the embedded M4 fork
       under ``rfnm-bringup/la9310-freertos/`` (already cloned, branch
       ``iqplayer-mbox0-vdbg``)
   * - ``la931x_iqplayer_rfnm-iqplayer-bringup/iqplayer_cwproj/``
     - VSPA kernel project (produces ``.eld`` images)
   * - ``la931x_iqplayer_rfnm-iqplayer-bringup/host-utils/``
     - IQ player host utilities (``iq_app``, ``iq_trace``, etc.)
   * - ``la9310-driver/`` (clone: ``mxstrk/la9310-driver``, branch ``la9310-modinfo``)
     - Patched kernel module source
   * - ``imx8mp-kernel/`` (clone: ``rfnm/imx8mp-kernel``, branch ``rfnm_6.6.36``)
     - Kernel tree needed for ``Module.symvers``
   * - ``la93xx_host_sw/``
     - NXP-tree uapi headers (``uapi/``) referenced by host-utils build

.. note::

   **Header tree note.** The M4 firmware must be built against the RFNM fork
   headers (``la9310-freertos/common_headers``), **not** NXP's. Host-utils use
   the RFNM fork headers for ``common_headers`` as well; the UAPI headers come
   from ``la9310-driver/uapi``. Neither tree defines ``LA9310_DEV_NAME_PREFIX``
   -- apply the one-time source patch (Step 6) before building host-utils.

Step 1 -- Clone the patched driver and kernel tree
..................................................

::

    cd ~/bitbucket/LA9310

    # Patched la9310shiva driver (four streaming fixes + modinfo)
    git clone git@github.com:mxstrk/la9310-driver.git
    cd la9310-driver && git checkout la9310-modinfo && cd ..

    # RFNM kernel tree -- only needed for Module.symvers; do NOT flash this kernel
    git clone --depth 1 -b rfnm_6.6.36 https://github.com/rfnm/imx8mp-kernel.git

The M4 firmware source is already present inside the bringup repo at
``la931x_iqplayer_rfnm-iqplayer-bringup/rfnm-bringup/la9310-freertos/``
(branch ``iqplayer-mbox0-vdbg``). No extra clone is needed.

Step 2 -- Build the kernel (for Module.symvers only)
.....................................................

A full kernel build is required to produce ``Module.symvers``; ``modules_prepare``
alone is not sufficient::

    make -C ~/bitbucket/LA9310/imx8mp-kernel \
        ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
        imx8mp_rfnm_defconfig
    make -C ~/bitbucket/LA9310/imx8mp-kernel \
        ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
        -j$(nproc) Image modules

This takes 20-40 minutes on first run. Subsequent incremental builds are fast.

Step 3 -- Build the patched la9310shiva.ko
..........................................

Set shell variables for the four path arguments, then build::

    export KDIR=~/bitbucket/LA9310/imx8mp-kernel
    export DRIVER=~/bitbucket/LA9310/la9310-driver
    export M4=~/bitbucket/LA9310/la931x_iqplayer_rfnm-iqplayer-bringup/rfnm-bringup/la9310-freertos

    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
        -C $KDIR \
        M=$DRIVER/kernel_driver/la9310shiva \
        LA9310_COMMON_HEADERS=$M4/common_headers \
        UAPI_DIR=$DRIVER/uapi \
        COMMON_DIR=$DRIVER/kernel_driver/common \
        CONFIG_ENABLE_FLOAT_BYPASS=y \
        modules

Verify the vermagic matches the board::

    modinfo -F vermagic $DRIVER/kernel_driver/la9310shiva/la9310shiva.ko
    # Must print: 6.6.36-rt35-ga13146252326 SMP preempt mod_unload aarch64


Step 3 -- Build other rfnm kernel mod (rfnm_daughterboard.ko for adc dac config param)
......................................................................................

__Step 1__: Build shiva first (you already know this command works):

```bash
export KDIR=~/bitbucket/LA9310/imx8mp-kernel
export M4=~/bitbucket/LA9310/la931x_iqplayer_rfnm-iqplayer-bringup/rfnm-bringup/la9310-freertos
export DRIVER=~/bitbucket/LA9310/la9310-driver
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    -C $KDIR \
    M=$DRIVER/kernel_driver/la9310shiva \
    LA9310_COMMON_HEADERS=$M4/common_headers \
    UAPI_DIR=$DRIVER/uapi \
    COMMON_DIR=$DRIVER/kernel_driver/common \
    CONFIG_ENABLE_FLOAT_BYPASS=y \
    modules
```

__Step 2__: Build rfnm with shiva's `Module.symvers` provided:

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    -C $KDIR \
    M=$DRIVER/kernel_driver/la9310rfnm \
    LA9310_DRV_HEADER_DIR=$DRIVER/kernel_driver/la9310shiva \
    LA9310_COMMON_HEADERS=$M4/common_headers \
    UAPI_DIR=$DRIVER/uapi \
    COMMON_DIR=$DRIVER/kernel_driver/common \
    KERNEL_DIR=$KDIR \
    RFNM_BUILD_DIR=/home/nxa15228/github/rfnm-sdk/sources \
    DIORA_SDK=/nonexistent \
    ICEWINGS_SDK=/nonexistent \
    KBUILD_EXTRA_SYMBOLS=$DRIVER/kernel_driver/la9310shiva/Module.symvers \
    modules
```


Step 4 -- Build the M4 firmware (native, no Docker)
....................................................

The cmake flag ``LA9310_HOST_OWNS_MBOX0`` must be passed at cmake time, not at
make time. A clean CMake directory is required between variants::

    export ARMGCC_DIR=/opt/gcc-arm-none-eabi-6-2017-q2-update
    export M4SRC=~/bitbucket/LA9310/la931x_iqplayer_rfnm-iqplayer-bringup/rfnm-bringup/la9310-freertos
    export BUILDDIR=$M4SRC/Demo/CORTEX_M4_NXP_LA9310_GCC

    cd $BUILDDIR
    git clean -xfd .
    ARMGCC_DIR=/opt/gcc-arm-none-eabi-6-2017-q2-update \
    LA9310_HOST_OWNS_MBOX0=ON cmake \
        -DCMAKE_TOOLCHAIN_FILE=./armgcc.cmake \
        -G "Unix Makefiles" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DTURN_ON_STANDALONE_MODE=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DDCS=ON \
        -DDCS_FULL_RATE=OFF .
    echo "#define TURN_ON_HOST_MODE 1" > include/la9310_boot_mode.h
    make -j$(nproc)

The flags ``-DDCS=ON -DDCS_FULL_RATE=ON`` compile ``vDcsInit(Full_Freq)`` into the
M4 firmware so the ADC/DAC run at 122.88 MHz from boot. Without these flags the
M4 boots at 61.44 MHz and the iqplayer DDR capture bandwidth is halved.

Copy the output binary to the artifacts directory::

    export ARTIFACTS=~/bitbucket/LA9310/la931x_iqplayer_rfnm-iqplayer-bringup/rfnm-bringup/artifacts
    mkdir -p $ARTIFACTS/firmware-m4
    cp $BUILDDIR/release/la9310.bin $ARTIFACTS/firmware-m4/la9310_dfe_122.bin


Step 5 -- IQPlayer (elds, host-utils )
......................................

The host-utils top-level Makefile exports ``LA9310_COMMON_HEADERS`` and
``UAPI_DIR`` to all sub-makes. Set them as environment variables before
invoking make::

    export LA9310_COMMON_HEADERS=~/github/rfnm-sdk/sources/la93xx_freertos/common_headers
    export UAPI_DIR=~/bitbucket/LA9310/la9310-driver/uapi
    cd ~/bitbucket/LA9310/la931x_iqplayer
    make

Step 6 -- Deploy IQPlayer
..............................................

    cd ~/bitbucket/LA9310/la931x_iqplayer
    make install
    ./deploy.sh 172.16.0.181

Step 8 -- Stage artifacts and deploy to the board
.................................................

Copy the patched driver separately::

    export DRIVER=~/bitbucket/LA9310/la9310-driver
    # Back up the stock module first
    ssh root@172.16.0.181 'cp -a /rfnm/kernel/la9310shiva.ko /rfnm/kernel/la9310shiva.ko.stock'
    rsync -a $DRIVER/kernel_driver/la9310shiva/la9310shiva.ko root@172.16.0.181:/rfnm/kernel/la9310shiva.ko

Copy the patched freeRTOS separately::

    rsync -a ~/bitbucket/LA9310/la931x_iqplayer_rfnm-iqplayer-bringup/rfnm-bringup/la9310-freertos/Demo/CORTEX_M4_NXP_LA9310_GCC/release/la9310.bin root@172.16.0.181:/lib/firmware/la9310_dfe_122.bin

Compile and deploy devmem::
    
    cd /home/nxa15228/bitbucket/LA9310/la93xx_host_sw/app/devmem
    CC=aarch64-linux-gnu-gcc UAPI_DIR=~/bitbucket/LA9310/la9310-driver/uapi make
    rsync -a devmem root@172.16.0.181:/usr/bin/devmem

Compile and deploy bin2mem::
    
    cd /home/nxa15228/bitbucket/LA9310/la931x_host_sw/app/bin2mem
    CC=aarch64-linux-gnu-gcc UAPI_DIR=~/bitbucket/LA9310/la9310-driver/uapi make
    rsync -a bin2mem root@172.16.0.181:/usr/bin/bin2mem


Compile and deploy modem_info::
    
    cd /home/nxa15228/bitbucket/LA9310/la931x_host_sw/app/modem_info
    CC=aarch64-linux-gnu-gcc UAPI_DIR=~/bitbucket/LA9310/la9310-driver/uapi make
    rsync -a devmem root@172.16.0.181:/usr/bin/devmem



Step 9 -- Activate the IQ Player pairing and reboot
....................................................

::

    ssh root@172.16.0.181 '
        systemctl disable rfnm-drivers.service
        mkdir -p /usr/local/etc
        echo 0x18000000 > /usr/local/etc/bar0.cfg
        sync && reboot
    '

After the board comes back up, load the driver manually (the autoloader is now
disabled)::
    ssh root@172.16.0.181 \
        '
        mkdir -p /srv/tftp/blue0-rootfs/lib/modules/6.6.36-rt35-ga13146252326/extra
        cd /lib/modules/6.6.36-rt35-ga13146252326/extra
        ln -s /rfnm/kernel/la9310shiva.ko .
        '

    ssh root@172.16.0.181 \
        '
        # la9310shiva.ko (rfnm build) does not implement alt_firmware_name /
        # alt_vspa_fw_name -- firmware selection is done by symlinking the
        # desired binaries as the default names before insmod.
        ln -sf /lib/firmware/la9310_dfe_122.bin /lib/firmware/la9310.bin
        ln -sf /lib/firmware/apm-iqplayer.eld   /lib/firmware/apm.eld
        cd /root/host_utils
        echo 7 > /proc/sys/kernel/printk
        insmod /rfnm/kernel/la9310shiva.ko scratch_buf_size=0x4000000 scratch_buf_phys_addr=0x92400000
        '

Step 10 -- Go/no-go checks
...........................

Run these in order; each validates one layer of the stack::

    # 1. IQFLOOD window registered on OUTBOUND_3
    ssh root@172.16.0.181 'dmesg | grep "iqplayer IQFLOOD"'
    # Expected: ...0xb0001000[H]-0x96400000[M],size 117440512 (win 3)

    # 2. /dev/shiva0 device node created
    ssh root@172.16.0.181 'dmesg | grep "/dev/shiva0"'

    # 3. Global sysfs status file present
    ssh root@172.16.0.181 'cat /sys/shiva/shiva_status'
    # Expected: modem 0: nlm0 ready

    # 4. VSPA mailbox round-trip (proves MBOX0 mask is active)
    ssh root@172.16.0.181 'vspa_mbox send 0 0 0x11100000 0 && vspa_mbox recv 0 0'
    # Expected: Received from VSPA:0, MBox:0, ...  (NOT "is not responding")

    # 5. IQ flood carveout reported correctly
    ssh root@172.16.0.181 'la9310_modem_info | grep FLOOD'
    # Expected: 0x96400000 | 0xb0001000 | 0x7000000

Step 11 -- RX streaming smoke test
...................................

::

    ssh root@172.16.0.181 '
        cd /opt/iqplayer/scripts
        ./set-proxy-offset.sh 0x6FFFC00        # iqflood_size(112 MB) - 1024
        iq_app -r -f 0 0x800000               # consumed must track produced
    '

.. note::

   The ``iq-*.sh`` helper scripts call each other as ``./name.sh``. Always
   ``cd /opt/iqplayer/scripts`` before running them; having the directory on
   ``$PATH`` alone is not enough.

Step 12 -- Restore the stock RFNM pairing
..........................................

::

    ssh root@172.16.0.181 '
        /opt/iqplayer/deactivate-iqplayer.sh
        cp -a /rfnm/kernel/la9310shiva.ko.stock /rfnm/kernel/la9310shiva.ko
        systemctl enable rfnm-drivers.service
        reboot
    '

Known Traps
...........

1. **M4 firmware tree:** Never build the M4 from NXP's ``la93xx_freertos`` tree.
   The RFNM fork places the host-interface at ``0x1C000`` (size 672); NXP mainline
   uses ``0x1B000`` (size 808). The driver is compiled against the RFNM offsets,
   so a NXP-tree M4 binary causes ``probe failed -22`` and a hung PCIe link
   (requires power cycle to recover).

2. **cmake flag timing:** ``LA9310_HOST_OWNS_MBOX0``, ``DCS``, and
   ``DCS_FULL_RATE`` are all read at cmake configure time. Running
   ``make LA9310_HOST_OWNS_MBOX0=ON`` without re-running cmake produces a
   silently unpatched binary. Always do ``git clean -xfd .`` and re-run cmake
   when switching variants. ``ARMGCC_DIR`` must also be exported or the
   toolchain cmake script exits with an error.

3. **One RX capture per VSPA boot:** ``iq-capture.sh`` works once on a fresh
   boot. A second run after ``iq-swreset.sh`` is refused (``LSB:0x00000000``),
   and ``bin2mem`` then silently re-reads the old buffer. ``md5sum`` every pair
   of capture files before comparing them.

4. **bin2mem address is board-specific:** RX capture lands at ``host_base +
   size/2``. Here that is ``0x96400000 + 0x3800000 = 0x99C00000``. Read both
   numbers from ``la9310_modem_info`` (IQ FLOOD phys row) rather than copying
   from any document.

5. **alt_firmware_name / alt_vspa_fw_name are not implemented** in the rfnm
   build of ``la9310shiva.ko``. Passing them as insmod parameters produces
   ``unknown parameter ... ignored`` and the driver loads its hardcoded defaults
   (``la9310.bin`` and ``apm.eld``). Always create symlinks to the desired
   firmware files before calling insmod (see Step 9).

7. **rfnm_qec and rfnm_agc must not be loaded** under the IQ Player pairing.
   Those modules poll and rewrite VSPA parameter DMEM and corrupt a foreign
   VSPA kernel. All other ``rfnm_*`` modules (``kpage_ncache``, ``rfnm_gpio``,
   ``la9310rfnm``, ``rfnm_lalib``, ``rfnm_daughterboard``, ``rfnm_lime``) are
   safe and can be loaded for RF front-end control.
