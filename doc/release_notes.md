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

Known limitations:
- 0T4R_NOQEC: a few AXIQ overruns counted at stream start, before host drain threads run
- The v1.1 "only 2 channels" 4R limitation no longer applies to 0T4R_NOQEC
