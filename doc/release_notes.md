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

Known limitations:
- 0T4R: a few AXIQ overruns counted at stream start, before host drain threads run
- The v1.1 "only 2 channels" 4R limitation no longer applies to 0T4R
