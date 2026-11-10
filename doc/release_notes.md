[la931x_iqplayer v1.0]

The iq-player v1.0 delivers the following features:
 - Enable iq receive/transmit of time domain IQ samples to/from binary files
 - Support Tx/Rx QEC
 - Support x2 or x4 decimation, x2 used by default
 - Provide iq_mon utility to monitor statistics
 - Provide iq_app/lib_iqplayer to send and receive time domain IQ samples from linux user space application
 - Support full duplex 1T1R at 61.44MSPS
 - Support half duplex 1T or 1R at 122.88MSPS
 - Support full duplex 1T2R at 61.44MSPS
 - Experimental support for
   - External DMA agent Rx and/or Tx in linux user space "iq_streamer" aiming at (1) reach 160MSPS (2) achieve FD 122.88MSPS  
   - External DMA Rx agent offload in i.mx8mp M7 aiming to reach 160MSPS  

[la931x_iqplayer v1.1]

- Add linux Makefiles
- Add runtime VSPA image reload/swap
- Add sync start capability

Known limitations:
- Full Duplex 122.88 MSPS is working but suffers drop of samples due to RX/TX fifo overflow/underrun
- 4R firmware doesn't support all 4 channels enabled, user can enable only 2 channels at the same time

[la931x_iqplayer v1.2]

- 0T4R_NOQEC: lossless 4 x RX at 61.44 MSPS (4 x 245 MB/s AXIQ, 4 x 122 MB/s DDR),
  with or without host flow control
- Fix half-rate RX stall with flow control (merged sticky DDR write completions);
  DDR produced count now derived from DMA state, 2 writes in flight per channel
- lib_iqplayer: RX ACK batching (IQ_RX_ACK_MIN) and mailbox GO kick (IQ_RX_KICK)
- iq_mon: real-interval rates, screen refresh, VSPA RX cycle meter;
  stall/loss counters with the new 0T4R_NOQEC_DIAG firmware
- RX start while streaming restarts the stream instead of NACK
- 1T1R: fix RX stall (firmware used the host-written consumed counter as its own);
  produced now follows DDR write completion, DMA-state completion accounting

- Per-channel RX QEC in all firmware variants: MBOX_OPC_IQ_CORR / RX_DCO_CORR
  channel in msg bits 49-48, bit 50 = all channels, NACK on a channel not in the build
  (old tools with bits 50-48 = 0 update channel 0 only)
- iq_app -X: key c cycles the QEC channel, -C <ch> start channel, per-channel save file;
  imb2qec.py / imb2qec-2.py: --ch N / --all
- 1T2R: main-loop and TX pass cycle meters off to fit PRAM (1T2R_DIAG: RX pass meter off too)

- 0T4R: fused QEC + x2 decimation kernel (rx_qec_decx2_8t), lossless 4 x RX at 61.44 MSPS
  with QEC, incremental QEC refresh while streaming (no loss on updates)
- Firmware variants reduced to 1T1R, 1T2R, 1T2R_DIAG, 0T4R, 0T4R_DIAG, 0T4R_PROF;
  removed the two-stage QEC 0T4R, 0T4R_REV, 0T4R_NOQEC, 0T4R_NOQEC_DIAG, 0T4R_NOQEC_PROF
  and the 0T4R_FUSED* names; make install copies only these ELDs
- Removed unused build switches: RX_REVERSE_ORDER, RX_DDR_STATUS_FULL, RX/TX_DDR_LEGACY_CMPL,
  RX_DDR_INFLIGHT (fixed at 2), RX_FC_POLL, TX_DDR_LOAD_TEST, DBG_SPIN_ON (firmware unchanged)
- Fix mirrored RX spectrum on RFNM/LMS7002 (a +f tone was seen at -f): the AXIQ RX FIFOs now
  swap I/Q in hardware (RX_IQ_SWAP, default 1; build with -DRX_IQ_SWAP=0 to disable).
  Re-run RX QEC calibration after updating, previous phase/DC values change sign.
- iq_app / iq-app-rx.sh / iq-app-tx.sh: saved RX and TX QEC files are applied at start
  (same files for every firmware), QEC directory is <script dir>/qec by default
  (-Q dir to change, -q / QEC_NOLOAD=1 to skip)
- iq-app-rx.sh -X: key w writes an IQ snapshot of all channels,
  <base>_<rate>Msps_ch<N>_<timestamp>.bin, default <script dir>/capture/rx and 1 ms
  (-W base [size|<N>ms])
- iq_app -X / -P: per-channel saturation line (peak I/Q dBFS, clip count, crest factor,
  [CLIP] / [HIGH] flags); -X key t toggles a time-domain I/Q chart (as compute_tone_params.py):
  auto 1-2-5 Y scale (key a: fixed +-1 FS with clip rails), 1 sample per column by
  default, keys +/- zoom (min/max envelope above 1 sample per column)
- rf_tune (host-utils/rf_tune): interactive RF panel for the RFNM Lime / Granita / Yucca
  daughterboards (sysfs rfnm_primary/secondary), board ranges and paths, TX/RX loop
  budget guard (-G/-x/-o), PA-step / bias-tee / loopback confirmations, Lime RX-then-TX
  apply order, -s status, -c ch.field=value, profiles (s/l keys, -l, txrx_en.sh profile)
- rf_tune defaults: built-in per-board table (Lime = txrx_en.sh values), saved next-run
  defaults /etc/rf_tune/<board>.cfg (--save / S, -d / L, .bak kept), --reset / R to the
  built-in values; txrx_en.sh defaults / reset
- rf_tune 1.1 panel: nothing written at start; channels on in sysfs are live, channels
  off are prepared from the saved defaults (or built-in) and applied, guards checked,
  when enabled; edits to off channels are held (-n: sysfs only). Keys o (cabled / over
  the air) and x (external attenuation), saved in the cfg; refusal message shows how to
  change the budget. Refusal / confirmation on background colours with !! / ?? prefixes,
  value marks * pending, ~ held, ! driver; -m / NO_COLOR for no colours
- Reference manual simplified and updated (current scripts, variants, QEC save/load);
  development history moved to doc/iqplayer_design_notes.rst; 1T4R removed
- RFNM SDK porting guide rewritten: generic variables, M4 DCS full/half rate build,
  manual restore of the stock RFNM stack
- RFNM: IQ Player builds from the RFNM la9310-driver / la9310-freertos repositories plus
  doc/patches/ only (modinfo port, iqflood mapping, mailbox 0 host ownership, iqflood mmap,
  DCS_FULL_RATE); uapi/la9310_modinfo.h comes from la9310-driver patch 0001.
  host-utils iq_app no longer defaults to a local bring-up tree for LA9310_COMMON_HEADERS
- RFNM: la9310-freertos patch 0005 adds the AXIQ_LOOPBACK cmake option (-DAXIQ_LOOPBACK=ON,
  M4 enables AXIQ digital loopback at start; build-tested only). Patches must be applied
  with git apply --index. RF configuration on RFNM (sysfs rfnm_primary/secondary,
  txrx_en.sh) documented, versus dpdk-dfe_app on NXP/Lime platforms
- RFNM: optional la9310-driver patch (doc/patches/la9310-driver/optional/) adds the
  la9310rfnm rfnm_rx_park_fault parameter; load-rfnm.sh sets it to 0 when present, which
  silences the "phytimer: fw parked an RX lane" console messages during captures.
  RFNM porting guide: Known Trap 6 and Annex A (example build/deploy on a development host)
- iq-reload-vspa.sh: VSPA software reset, per-section DMA check (exit 1 on timeout),
  apm.eld update, VSPA boot handshake (0x70000000/0xF0700000, previously it swallowed
  the next mailbox) and checked IQFLOOD proxy offset resend after the load (the new image
  previously ran with the default proxy offset, iq_mon showed zero counters); a single
  command replaces iq-swreset.sh + iq-reload-vspa.sh + stats.sh. Obsolete apm.eld md5
  warning removed from load-la9310.sh / load-rfnm.sh (iq_trace reads the ELD at runtime)
- apm.eld is now a copy of the selected ELD instead of a symlink (make install and
  iq-reload-vspa.sh), so deploy.sh (scp -r) installs a regular file
- txrx_en.sh: limetxrx (RX then TX, TX at 1860 MHz), off and status modes; limerx
  re-applies tx0 when it is on (rfnm_lime keeps the port owned by TX on an RX apply,
  TX to RX coupling +35 dB). RFNM porting guide Known Trap 7
- iq_app -T / -X: terminal always restored on exit (atexit), Ctrl-C handled as the quit
  key; iq-app-tx.sh / iq-app-rx.sh wait for iq_app on Ctrl-C (prompt no longer
  misaligned, VSPA TX always stopped after -T)
- RFNM quick start guide marked legacy (discontinued NXP LA9310 BSP); for a recent kernel
  use the RFNM SDK and iqplayer_porting_guide_RFNM-SDK.rst. readme.md and LimeSDR-Micro
  porting guide updated (variants, iq-app usage, QEC save/load, doc index)

Known limitations:
- 0T4R: a few AXIQ overruns counted at stream start, before host drain threads run
- The v1.1 "only 2 channels" 4R limitation no longer applies to 0T4R
