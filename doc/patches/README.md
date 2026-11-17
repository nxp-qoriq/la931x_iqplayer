# IQ Player patches for other repositories

> **Temporary.** These patches change repositories that IQ Player does not own.
> Pull requests have been or will be opened in each upstream repository. When a
> pull request is merged, the patch is removed from this folder and the upstream
> repository is used as is. Until then, apply the patches to the base commit
> listed below.

## Which folders do I need?

IQ Player runs in two software environments. Use the folders for your
environment only: the folder names are close but the repositories are different.

| Environment | Board | Guide | Folders to apply |
|---|---|---|---|
| **NXP BSP** (flexbuild, nxp-qoriq repositories) | LimeSDR-Micro on FRDM-IMX95 / FRDM-IMX8MPLUS / Layerscape | `doc/iqplayer_porting_guide_LimeSDR-Micro.rst` | `la93xx_host_sw/`, `la93xx_freertos/`, `dpdk/`; optional: `la9310_debug/` |
| **RFNM SDK** (rfnm repositories) | RFNM (LA9310 + i.MX8MP) | `doc/iqplayer_porting_guide_RFNM-SDK.rst` | `la9310-driver/`, `la9310-freertos/`; optional: `la9310_debug/`, `la9310-driver/optional/` |

- `la93xx_*` (NXP) and `la9310-*` (RFNM) are different repositories. Do not
  apply an NXP folder to an RFNM repository or the other way round.
- `la9310_debug/` (VSPA debugger) is the only folder used in both environments.
  The repository belongs to the 6G for All project, not to NXP or RFNM.
- The legacy NXP LA9310 BSP for RFNM (`iqplayer_quick_start_guide_RFNM.rst`)
  uses none of these patches.

## Patch sets

| Folder | Upstream repository | Base commit (checked) | Environment | Use | Upstream PR |
|---|---|---|---|---|---|
| `la93xx_host_sw/` | https://github.com/nxp-qoriq/la93xx_host_sw `main` | `ad9f1db` | NXP BSP | required (`CONFIG_STRICT_DEVMEM`) | pending |
| `la93xx_freertos/` | https://github.com/nxp-qoriq/la93xx_freertos `main` | `c914cfe` | NXP BSP | required (0001-0003, 0005); 0004 for the debugger | pending |
| `dpdk/` | https://github.com/NXP/dpdk `25.11-qoriq` | `93c0678597` | NXP BSP | required for `dfe_app` | pending |
| `la9310-driver/` | https://github.com/rfnm/la9310-driver `main` | `c20e5814` | RFNM SDK | required | pending |
| `la9310-driver/optional/` | https://github.com/rfnm/la9310-driver `main` | on top of `la9310-driver/` | RFNM SDK | optional (silences the RX park fault log) | pending |
| `la9310-freertos/` | https://github.com/rfnm/la9310-freertos `main` | `be3536ca` | RFNM SDK | required (0001-0003); 0004/0005 build options | pending |
| `la9310_debug/` | https://git.6g-for-all.de/6g4all/la9310_debug.git `main` | `2328871` | both | optional, VSPA debugger (0003 needed on the NXP BSP) | pending |

"Checked" means each set was applied to a fresh clone of the public upstream
repository at that commit for this release. `la9310-driver/optional/` applies
both on top of `la9310-driver/` and on the unpatched commit.

## Content

### NXP BSP

| Patch | Change |
|---|---|
| `la93xx_host_sw/0001` | `la9310shiva`: mmap of the scratch buffer, IQFLOOD and PCIe BARs through `/dev/shiva0`, so IQ Player works with `CONFIG_STRICT_DEVMEM=y` |
| `la93xx_freertos/0001` | LimeSDR-Micro: LA9310 system clock 122.88 MHz (the old 61.44 MHz default gave x2 tone errors and a TX stall) |
| `la93xx_freertos/0002` | LimeSDR-Micro: RX defaults for LMS7002M channel B |
| `la93xx_freertos/0003` | LimeSDR-Micro: RX channel select fixes |
| `la93xx_freertos/0004` | `LA9310_SW_CMD_VSPA_DBG` proxy for the VSPA debugger (`la9310_debug`); starts the sw_cmd engine |
| `la93xx_freertos/0005` | LimeSDR-Micro: clean build with `-Werror` and gcc 6 |
| `dpdk/0001` | `baseband/la93xx`: release the bbdev on probe failure |
| `dpdk/0002` | `examples/dfe_app`: do not starve the core, check bbdev capabilities |
| `dpdk/0003` | `baseband/la93xx`: map the LA9310 regions through `/dev/shiva0` (CONFIG_STRICT_DEVMEM; needs `la93xx_host_sw/0001`) |

### RFNM SDK

See the table in `doc/iqplayer_porting_guide_RFNM-SDK.rst`, section "Sources".
In short: `la9310-driver` adds `la9310_modinfo`, the IQFLOOD mapping at the IQ
Player EP address, the mmap needed with `CONFIG_STRICT_DEVMEM` and (0005) the
`la9310_modem_info` tool used by `load-rfnm.sh`, so RFNM needs no NXP
`la93xx_host_sw` tree;
`la9310-freertos` adds the VSPA debugger proxy, gives VSPA mailbox 0 to the
PCIe host and adds two build options.

### VSPA debugger (6G for All)

| Patch | Change |
|---|---|
| `la9310_debug/0001` | `vdbg`: typed print of global and static variables from the ELD |
| `la9310_debug/0002` | RFNM board support (`rfnm` transport) |
| `la9310_debug/0003` | `rfnm` transport renamed `shiva` (`rfnm` kept as an alias); NXP BSP host interface layout, so it works on LimeSDR-Micro |

Debugger dependencies on the other repositories:

- NXP BSP: M4 `la93xx_freertos/0004` only. The stock `la9310shiva` already
  provides the modinfo ioctl; `la93xx_host_sw/0001` (needed by IQ Player)
  lets the debugger map through `/dev/shiva0` instead of `/dev/mem`.
- RFNM SDK: M4 `la9310-freertos/0001` and the required `la9310-driver/` set
  (0001 adds the modinfo ioctl, missing from the stock RFNM driver).

## How to apply

```sh
IQP=<path to la931x_iqplayer>
git -C <repository> checkout <base commit>
git -C <repository> apply --index $IQP/doc/patches/<folder>/*.patch
```

Use `git apply --index` (as in the porting guides): `la9310-driver/0004` is a
plain diff without a mail header, so `git am` stops at it.

If a patch no longer applies, check first whether it was merged upstream
(compare with the upstream log). If it was, skip it.
