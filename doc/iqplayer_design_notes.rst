.. sectnum::

**IQ Player Design Notes**
++++++++++++++++++++++++++

These are development records kept for reference: measurement sessions, root-cause
analyses and fixes. Several of them were measured on firmware variants or build options
that no longer exist (``0T4R_NOQEC``, ``IQMOD_RX_NO_QEC``, two-stage QEC 0T4R, legacy DMA
completion accounting). The current behaviour is described in
``iqplayer_reference_manual.rst``.

0T4R development history
*************************

AXIQ Timing Budget
------------------

At 61.44 MSPS and RX_DMA_TXR_size = 256 samples, the AXIQ DMA transfer window is:

::

  window = 256 samples / 61.44 MSPS  = 4.167 us
         = 4.167 us  x  491.52 MHz   = 2048 VSPA cycles

The complete RX pipeline (AXIQ completion -> next AXIQ re-arm) must fit within 2048 cycles
to avoid AXIQ FIFO overrun (RXOVR).

VSPA clock on RFNM
~~~~~~~~~~~~~~~~~~

The LA9310 platform PLL locks on DCS_CLK, and the VSPA runs at 4 x DCS_CLK.
On RFNM, DCS_CLK comes from the Si5510 programmed by the i.MX kernel driver
(``i2c-rfnm-si5510.c``, boot value 122.88 MHz; ``la9310rfnm`` can reclock it later and then
prints ``reclocking dcs frequency``). The ADC/DAC rate is DCS_CLK divided by a power of two, so
61.44 MSPS requires DCS = 122.88 MHz, and the VSPA then runs at 491.52 MHz (491.22 MHz
measured with the ``0T4R_NOQEC_DIAG`` clock check). 614.4 MHz (DCS 153.6 MHz) cannot
produce 61.44 MSPS.

::

  budget (cycles per buffer) = N_samples x 4 x DCS / Fs
                             = 256 x 4 x 122.88 MHz / 61.44 MSPS = 2048

.. note::
   Measurement sessions 1-4 below were recorded against the former 2560-cycle
   assumption; the measured cycle counts are unchanged, only the budget is 2048.

Measurement Session 1 -- Pre-fix (live iq_trace on RFNM Blue)
--------------------------------------------------------------

.. note::
   Sessions 1-5 were measured on the ``0T4R_NOQEC`` firmware (``-DIQMOD_RX_NO_QEC``),
   the 4R workaround before the fused QEC kernel. That variant and the
   ``IQMOD_RX_NO_QEC`` code paths were removed in v1.2; 0T4R is now the fused build.

Platform: RFNM Blue, i.MX8MP host, LA9310 VSPA @ 491.52 MHz, ADC @ 61.44 MSPS.
Build: ``Debug_IQPLAYER_0T4R_NOQEC`` (flags: ``-DIQMOD_RX_0T4R -DIQMOD_RX_NO_QEC``).
Measurement instrument: ``L1_TRACE_MSG_DMA_XFR`` events inserted at the top of the
PUSH_RX_DATA() hot path; inter-event spacing read from live ``iq_trace`` capture.

**Result: loop period ~4310 cy (budget 2560 cy; 68% over).**

The loop was 1750 cy over budget. The primary contributor was the AXIQ overrun recovery
block: on every iteration the firmware detected an AXIQ FIFO overrun
(``L1_TRACE_MSG_DMA_AXIQ_RX_OVER``) and issued four ``axiq_fifo_rx_sr`` reads plus eight
``axiq_fifo_rx_cr`` writes (2 per channel x 4 channels), adding ~886 cy to the hot path.
This created a self-sustaining cascade: the slow loop caused RXOVR, which slowed the loop
further, which caused more RXOVR.

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ COMP + re-arm (4 channels)            ~700      completion poll + stream_read re-arm
  AXIQ overrun check + clear (4 ch)          ~886      axiq_fifo_rx_sr + 2x axiq_fifo_rx_cr per ch
  Decimation kernel (4 channels)             ~900      decimator_2x_8_Taps_asm, 4 ch sequential
  DDR WR check + enqueue (4 channels)        ~550      slot check + stream_write
  Dispatch / control overhead                ~1274     inter-phase branches, trace calls, counters
  **Total measured (DMA_XFR spacing)**    **~4310**   **1750 cy over the 2560-cycle budget**
 ======================================= ============ ================================================

Root Cause
----------

The ``axiq_fifo_rx_sr`` / ``axiq_fifo_rx_cr`` pair (AXIQ overrun detection and clear) is
required only when QEC is active to protect the QEC coefficient pipeline. In the NOQEC
build (``IQMOD_RX_NO_QEC``) it provides no benefit but adds ~886 cy per loop iteration
across 4 channels, pushing the loop well above the 2560-cycle AXIQ window and triggering
a self-reinforcing RXOVR cascade.

Two additional fixes were identified independently:

- **Fix 4** -- the DMA completion-check loop and the re-arm loop were implemented as two
  separate ``for`` loops over 4 channels, adding ~250 cy of redundant loop overhead.
- **Fixes 5/6/7** -- measurement-only ``l1_trace`` calls (ENTRY_MAIN, DEC_START/DEC_COMP,
  and ``_main.c`` dispatch traces) remain in the NOQEC hot path and account for ~550 cy.

Applied Fixes
-------------

- **Fix 1** (``iqmod_rx_2R_decx2x4.c``): The entire AXIQ overrun check/clear block is
  wrapped in ``#ifndef IQMOD_RX_NO_QEC``, removing it from the NOQEC hot path.
  *Note: superseded by Fix A v2 (see below), which reinstates a lightweight throttled
  AXIQ check covering all 4 channels with a single register read.*
- **Fix 2** (same file): The ``axiq_fifo_rx_cr`` double-write (set then clear) is also
  guarded, preventing the ~886 cy cost from appearing even in a partial-guard scenario.
  *Note: also superseded by Fix A v2.*
- **Fix 4** (same file): The two separate ``for`` loops (COMP-check and re-arm) are fused
  into a single pass per channel, saving ~250 cy of loop overhead.
  *Note: superseded by the two-loop structure (Fix B resolution, see below), which is
  required by VSPA2 DMA FIFO propagation latency constraints.*

Measurement Session 2 -- Post-fix Fix 1+2+4 (live iq_trace on RFNM Blue)
--------------------------------------------------------------------------

Same platform and build as Session 1. ELD rebuilt, deployed, and measured after applying
Fixes 1, 2, and 4.

**Result: loop period ~3124 cy, RXOVR = 0, throughput 234 MB/s.**

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ COMP + re-arm (fused, 4 channels)     ~700      single-pass fused loop; Fix 4 applied
  AXIQ overrun check + clear                    0      removed by Fix 1+2 (#ifndef IQMOD_RX_NO_QEC)
  Decimation kernel (4 channels)             ~900      unchanged
  DDR WR check + enqueue (4 channels)        ~550      unchanged
  Dispatch / trace overhead (residual)      ~974      ENTRY_MAIN + DEC traces + dispatch traces
  **Total measured (DMA_XFR spacing)**    **~3124**   **564 cy over the 2560-cycle budget**
 ======================================= ============ ================================================

The 234 MB/s result (vs. 245 MB/s target) corresponds to the 564 cy residual overage.
This residual is entirely due to measurement/trace instrumentation left in the NOQEC hot
path. The breakdown is:

- ``L1_TRACE_MSG_ENTRY_MAIN`` in PUSH_RX_DATA: ~50 cy (Fix 5: remove unconditionally).
- ``L1_TRACE_L1APP_RX_DEC_START`` + ``L1_TRACE_L1APP_RX_DEC_COMP`` x 4 channels: ~200 cy
  (Fix 6: wrap in ``#ifndef IQMOD_RX_NO_QEC``).
- ``_main.c`` dispatch trace calls x ~5 sites: ~300 cy
  (Fix 7: wrap in ``#ifndef IQMOD_RX_NO_QEC``).

Projected loop after Fixes 5+6+7: ~3124 - 550 = **~2574 cy**, within the 2560-cycle
budget, yielding the **~245 MB/s** throughput target.

Measurement Session 3 -- Post-fix Fix 5+6+7 (live iq_trace on RFNM Blue)
--------------------------------------------------------------------------

Same platform and build as Session 2. ELD rebuilt and deployed after applying
Fixes 5, 6, and 7.

**Result: loop period ~2490 cy, RXOVR = 0, throughput 245 MB/s.**

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ COMP + re-arm (fused, 4 channels)     ~700      unchanged from Session 2
  AXIQ overrun check + clear                    0      removed by Fix 1+2 (#ifndef IQMOD_RX_NO_QEC)
  Decimation kernel (4 channels)             ~900      unchanged
  DDR WR check + enqueue (4 channels)        ~550      unchanged
  Dispatch / trace overhead (residual)       ~340      ENTRY_MAIN removed (Fix 5); DEC_START/DEC_COMP
                                                       guarded (Fix 6); ENTRY_MAIN_DMASTAT + ENTRY_IDLE
                                                       guarded in _main.c (Fix 7)
  **Total measured (DMA_XFR spacing)**    **~2490**   **70 cy within the 2560-cycle budget**
 ======================================= ============ ================================================

The 245 MB/s result confirms all fixes are effective. RXOVR counter = 0 across
all 4 channels in 300 steady-state trace entries. The firmware operates within
the 2560-cycle AXIQ window with ~70 cy margin.

Fix A v2 -- Throttled AXIQ Overrun Check
-----------------------------------------

Session 3 removed the AXIQ overrun/underrun check entirely via ``#ifndef IQMOD_RX_NO_QEC``
(Fix 1+2). While correct for NOQEC builds, this leaves AXIQ hardware faults silently
undetected. Fix A v2 reinstates a lightweight safety net compatible with the NOQEC hot path:

- A single ``iord(AXIQ_SR0)`` read covers all 4 RX channels simultaneously (bits for
  RX0..RX3 are packed in one register). The original per-channel ``axiq_fifo_rx_sr`` loop
  (4 reads) is replaced by this single read.
- The check is throttled via a ``rxovr_check_throttle`` counter: the SR+CR sequence runs
  once every 16 PUSH_RX_DATA iterations.
- Amortised hot-path cost: ~70 cy (1 iord + branch-not-taken, amortised over 16 calls).
  Un-throttled cost if errors present: ~170 cy (includes per-channel stat increments + 2
  iowr to clear). Both are well within the 2560-cycle AXIQ window.
- RXOVR is detected within ~16 x 4.167 us = ~67 us of occurrence.
- The counter is pre-loaded to 16 in the capture start path to suppress a false RXUDR on
  the first iteration (FIFO just enabled; any stale underrun flag is a false positive).

This replaces Fix 1+2. Fix 1+2's complete removal of the AXIQ check is no longer needed
because the throttled single-read path is fast enough even in the NOQEC hot path.

Fix B Attempt and DMA FIFO Propagation Latency
-----------------------------------------------

After confirming Session 3 results, an optimisation attempt (Fix B) tried to further
reduce hot-path cost by hoisting all three DMA status register reads before the
completion/re-arm loop and fusing completion processing and re-arm into a single
per-channel pass (one loop instead of two). This introduced a regression:

**Observed regression:** ch2 ``DMA_RX_CMD_OVR`` incrementing continuously; ch3 running at
~132 MB/s (half-rate); ch0/ch1 unaffected. Error appeared immediately on first iq_mon
sample after reboot.

**Root cause -- VSPA2 DMA FIFO propagation latency:**
The VSPA2 DMA operates in FIFO mode (``DMAC_FIFO``): each channel has 2 command slots.
``DMA_FIFO_STAT`` bit for a channel is cleared when both slots are occupied and is set
again when a slot becomes free. The hardware mechanism is:

1. ``dmac_clear_complete(ch)`` writes to ``DMA_COMP_STAT`` to acknowledge a completed
   transfer. This is what frees the FIFO slot.
2. ``dmac_is_available(ch)`` reads ``DMA_FIFO_STAT`` to check whether a slot is free.

There is a pipeline propagation delay between step 1 and step 2: the freed slot does not
appear in ``DMA_FIFO_STAT`` immediately. The VSPA2 memory-mapped register bus requires
several pipeline cycles before the clear in ``DMA_COMP_STAT`` propagates into the
``DMA_FIFO_STAT`` read. When ``dmac_clear_complete(ch)`` and ``dmac_is_available(ch)``
are back-to-back in the same loop iteration (the fused Fix B approach), the FIFO slot
appears still occupied, ``stream_read`` is not issued, and the AXIQ FIFO overflows on
the next window. This was not visible on ch0/ch1 because those channels had already been
processed earlier in the loop and the propagation had time to settle.

**Fix B resolution -- two-loop structure:**

The proven fix reverts to two separate loops:

- **Loop 1** iterates over all 4 channels and calls ``dmac_clear_complete()`` and
  ``dmac_clear_event()`` for every channel that has completed. No ``is_available`` calls
  are made in this loop.
- **Loop 2** iterates over all 4 channels and calls ``dmac_is_available()``. By the time
  Loop 2 reaches channel 0, the entire body of Loop 1 (4 iterations of completion check,
  stat increment, error check) has executed, providing sufficient elapsed cycles for the
  HW to propagate all freed FIFO slots into ``DMA_FIFO_STAT``.

The minimum safe separation between ``dmac_clear_complete(ch_N)`` and
``dmac_is_available(ch_N)`` is satisfied by the ~3-4 loop-1 iterations that execute
between them (one full pass of Loop 1 completes before Loop 2 starts for channel 0).

Measurement Session 4 -- Post Fix A v2 + Two-Loop (live iq_trace on RFNM Blue)
--------------------------------------------------------------------------------

Same platform and build as Session 3. ELD rebuilt and deployed after applying Fix A v2
(throttled AXIQ check) and the two-loop structure (Fix B resolution).

**Result: loop period ~1900 cy (typical), RXOVR = 0, all 4 channels at 245 MB/s.**

Loop period extracted from consecutive ``L1_TRACE_MSG_ENTRY_IDLE`` timestamps in a live
``iq_trace`` capture (300 entries, steady-state streaming):

 ======================================================= ============ =============================================
  Iteration                                               Period (cy)  Notes
 ======================================================= ============ =============================================
  Typical (DMA completion present every call)              ~1900-1960  steady state; all 4 channels active
  Short (no DMA completion in this pass)                   ~1518-1580  FIFO slot not yet ready; loop exits early
  Budget                                                     2048      AXIQ window at 61.44 MSPS
  Margin                                                    ~600-1000  comfortable headroom
 ======================================================= ============ =============================================

 ======================================= ============ ================================================
  Phase                                   Cycles       Notes
 ======================================= ============ ================================================
  AXIQ overrun check (throttled)             ~70        1 iord(AXIQ_SR0); amortised over 16 iterations
  Loop 1: completion check (4 channels)     ~200        dmac_is_complete + clear_complete + clear_event
  Loop 2: re-arm (4 channels)               ~200        dmac_is_available + stream_read
  Decimation kernel (4 channels)            ~900        decimator_2x_8_Taps_asm, 4 ch sequential
  DDR WR check + enqueue (4 channels)       ~550        slot check + stream_write
  Dispatch / control overhead               ~80         counters, proxy update, branch targets
  **Total (typical)**                     **~2000**    **~560 cy within the 2560-cycle budget**
 ======================================= ============ ================================================

No ``DMA_RX_CMD_OVR``, ``FIFO_RX_UDR``, ``FIFO_RX_OVR``, or ``DMA_XFER_ERROR`` counters
increment across the full 300-entry trace window. The two-loop structure resolves the Fix B
regression without incurring any measurable throughput penalty versus the fused approach.

Increasing RX_NUM_BUF to 4 provides one extra DMEM slot for in-flight DMA:

- **0T4R_NOQEC** (no QEC struct): ``input_buffer`` = 4 x 4 x 256 x 4 = 16384 B fits
  exactly in IPPU DRAM (16384 B total). This variant uses RX_NUM_BUF=4.
- **0T4R with QEC**: ``rxiqcompcfg`` occupies 512 B of IPPU DRAM, leaving only 15872 B
  for ``input_buffer``; RX_NUM_BUF=4 (16384 B) does not fit. Limited to RX_NUM_BUF=3
  unless the QEC struct is relocated.

Measurement Session 5 -- 4R Streaming with Host Flow Control
---------------------------------------------------------------

Platform: RFNM, i.MX8MP host, LA9310 VSPA @ 491.52 MHz, 4 x RX @ 61.44 MSPS
(245 MB/s AXIQ per channel, 122.88 MB/s DDR per channel after x2 decimation).

**Problem: half-rate stall with flow control on.** The DDR write completion flag
(``DMA_COMP_STAT``) is a single sticky bit per channel. With two writes queued in the
2-entry DMA command FIFO, two completions can merge into one; ``la9310_fifo_produced_size``
then falls behind for good, the host sees less data than was written and the flow-control
gate closes early, halving throughput.

**Fix: DDR accounting from DMA state** (0T4R, 1T2R, 1T1R). Each pass,
for each channel with writes outstanding, the number of writes in flight is derived from
``XRUN`` and ``DMA_FIFO_STAT``:

- not running, FIFO entry free -> 0
- running, FIFO entry free -> 1
- no FIFO entry free -> 2

and ``produced = enqueued - in_flight * RX_DDR_STEP``. The produced count never moves back
and advances to the minimum of this pass and the previous pass, so a single transient read
cannot overestimate it.

**Cycle budget.** A bisect with flow control off (all variants at the same commit) showed
the accounting must stay cheap at 4R:

 ======================================= ============================================
  Variant                                 Result (FC off)
 ======================================= ============================================
  Legacy completion bit                   lossless (but half-rate stall with FC on)
  State accounting, 1 write in flight,    ch3 (serviced last) starves: DMEM ring full,
  3 status reads + completion read        AXIQ FIFO overrun
  Same with 2 writes in flight            ~5x fewer events, still overruns
  2 writes in flight, 2 status reads      lossless (retained default)
 ======================================= ============================================

**Result (default 0T4R_NOQEC):** pass ~1780 cy average, 1866 cy max (2048 cy budget,
none above). 4 x 245 MB/s AXIQ, 4 x 122 MB/s DDR and app. Lossless in steady state with
flow control on and off (iq_app ``-X`` and ``-D``), no ``DDR_WR_OVR`` increase.


Event-driven main loop and host flow control
********************************************

In v1.1 the VSPA main loop kept running and read the host flow-control counters from
DMEM on every pass. With 4 RX channels the extra DMEM reads compete with the DMA, IPPU and
PCIe slave accesses (DMEM access priority: DMA write, DMA read, AXI slave, IPPU, debug,
VCPU) and the loop missed the 2048-cycle budget.

v1.2 halts the VCPU with ``__idle()`` at the end of each pass. It wakes on a GO event:
a DMA completion (``DMA_GO_STAT``) or a host mailbox message. The host still writes the
produced / consumed counter directly in DMEM (a posted PCIe write, no read), then posts
``MBOX_OPC_TX_GO`` (0xF2) or ``MBOX_OPC_RX_GO`` (0xF3) on mailbox 1. A DMEM write does not
wake the VCPU. During streaming, DMA completions are enough. After a flow-control stop
(TX empty, RX ring full) no DMA is running, so only the mailbox brings the VSPA back.
RX acks are batched (``IQ_RX_ACK_MIN``, default 32 KB) to limit the mailbox and PCIe
traffic.

1T1R and 1T2R accounting fixes
******************************

1T1R RX accounting fix
----------------------

``iqmod_rx.c`` (1T1R) used ``rx_vspa_proxy[0].la9310_fifo_consumed_size`` as its own
DDR-done counter. Since libiqplayer writes that field directly in VSPA DMEM
(``rx_fc_addr``), every host ACK moved the counter back, the QEC ring looked full and RX
stalled with ``DDR_WR_OVR`` and ``FIFO_RX_OVR`` counting on every pass (``-X`` and ``-D``).

- The firmware keeps its own counters; the host consumed field is only read, for flow control.
- ``la9310_fifo_produced_size`` is raised on DDR write completion (data in DDR), not on compress.
- DDR completion is counted from DMA state (writes in flight), as in 0T4R.
- Start and stop abort the DDR write DMA and clear produced.

Verified on RFNM: 1T1R iq-app TX and RX (``-X`` and ``-D``); 1T2R iq-app TX and RX
(``-X`` and ``-T``).

1T1R TX DMA completion accounting
---------------------------------

At 122.88 MSPS TX only, with 2 DDR read channels and mBurst (``iq-replay.sh <file> <size> 1``),
the TX rate started at 491 MB/s and dropped to 327 MB/s after a few seconds, with
``DDR_RD_UDR`` and ``FIFO_TX_UDR`` counting. The cycle meters were well under budget.

Cause: each FIFO-mode DMA channel has 2 command slots but a single sticky completion bit.
When two transfers complete between two polls, only one completion is seen. TX counted
fetched DDR reads and consumed AXIQ writes from that bit, so each merge lost one buffer for
good and the TX rings looked fuller than they were.

Fix (``iqmod_tx.c``, default): the number of transfers still in flight is derived from the
DMA state, as for RX (``dma_done_from_state()`` in ``_main.c``): no FIFO slot free -> 2,
slot free and running -> 1, idle -> 0, and done = enqueued - in_flight. Done never moves
back and advances to the minimum of this pass and the previous pass.

- iq_mon prints ``TX in flight (last pass): DDR rd N buf, AXIQ wr N buf``
  (``t_stats.meter[13..14]``).
- 1T2R and ``1T2R_DIAG`` use the same TX accounting. To fit in PRAM the TX DDR read load
  test (start bit ``0x00200000``) is not built (the request is NACKed); the legacy
  completion-bit accounting, ``RX_DDR_STATUS_FULL``, ``RX_REVERSE_ORDER``, ``RX_FC_POLL``
  and the ``dbg_gbl`` debug spin loops were removed in the v1.2 cleanup.

Verified on RFNM, 1T1R, TX only at 122.88 MSPS with 2 read channels + mBurst:
491 MB/s steady, no ``FIFO_TX_UDR``, TX pass max 501 cy, loop max 1204 cy.


RFNM U-Boot console buffer (not attempted)
******************************************

The RFNM Blue U-Boot console truncates long command lines (``CONFIG_SYS_CBSIZE``, typically
256 or 512 bytes), which is why the RFNM porting guide sets ``bootargs`` statically with
``fw_setenv``. A possible fix is to rebuild U-Boot with ``CONFIG_SYS_CBSIZE=2048`` and
``CONFIG_SYS_PBSIZE=2080`` and flash it at the i.MX8MP offset (``dd ... of=/dev/mmcblk2
bs=1k seek=33``), with ``uuu`` USB recovery ready. This has not been tried and is not needed
by the current workflow.


Known issues / to investigate
*****************************

LimeSDR-Micro clock chain
-------------------------

On LimeSDR-Micro the LA9310 DCS_CLK is generated by the LMS7002M CGEN, not by a Si5510.
The FreeRTOS build option ``LA9310_SYS_CLK_FREQ`` sets CGEN = 4 x ``LA9310_SYS_CLK_FREQ``,
which is divided by 4 towards the LA9310, so DCS_CLK = ``LA9310_SYS_CLK_FREQ``. The VSPA
runs at 4 x DCS_CLK and the ADC/DAC at DCS_CLK (or DCS_CLK / 2 with the half-rate masks).

The former FreeRTOS default was 61.44 MHz. With it the VSPA ran at 245.76 MHz and the
converters at 30.72 MSPS: every tone was off by x2, and TX stalled (see below). The
FreeRTOS patch ``doc/patches/la93xx_freertos/0001-*.patch`` sets 122.88 MHz, the same as RFNM.
The rest of FreeRTOS (UART, I2C, DSPI, phy timer) already assumed 122.88 MHz
(``POST_SYS_FREQ`` in ``include/la9310.h``).

``la9310_modem_info`` computes the rates assuming DCS = 122.88 MHz, so it cannot show a
wrong ``LA9310_SYS_CLK_FREQ``. To check the clock, look at the FreeRTOS build log, or check
that iq_mon reports 245 MB/s on one 61.44 MSPS channel.

TX stall at DCS_CLK 61.44 MHz (open)
------------------------------------

Captured on i.MX95 + LimeSDR-Micro, 1T1R, DCS_CLK 61.44 MHz (VSPA 245.76 MHz,
DAC 30.72 MSPS):

- 1 DDR read channel + mBurst (start command ``0x05100040``, the default when the
  channel count is 0): TX stalls after start. iq_mon shows
  ``TX in flight (last pass): DDR rd 2 buf, AXIQ wr 0 buf``, with ``DDR_RD_UDR`` and
  ``FIFO_TX_UDR`` increasing.
- No ``DMA_CFG_ERROR`` / ``DMA_XFER_ERROR``, and no PCIe AER or SMMU error on the host.
- The stall persists across a runtime VSPA reload.
- 1 DDR read channel without mBurst (``0x05110040``) is stable at 122 MB/s, which is
  the full rate at 30.72 MSPS.

With DCS_CLK 122.88 MHz, the same 1ch + mBurst command runs at 245 MB/s on TX and RX
with the ``UDR`` / ``OVR`` counters flat, so the issue does not affect the supported
configuration. The two DDR read DMA transfers stay in flight and never complete at the
lower DMA clock, which suggests a problem with mBurst DDR reads over PCIe when the VSPA/DMA
clock is low. Not investigated further. A possible follow-up is to reproduce on RFNM with
DCS lowered to 61.44 MHz (``la9310rfnm`` reclock).

PCIe ATS advertised but not supported
-------------------------------------

The LA9310 (``1957:1c12``) advertises the PCIe ATS capability (offset 0x158, Invalidate
Queue Depth 00) but does not answer ATS invalidation requests. On a host where the IOMMU
translates for the LA9310 and enables ATS, this gives ``IOTLB_INV_TIMEOUT`` /
``CMD_SYNC`` timeouts, and the IOMMU back-pressure can also slow down the other devices in
the same group.

On i.MX95 with ``iommu.passthrough=1`` (the LimeSDR-Micro setup) it is harmless:
``lspci -vvv`` shows ``ATSCtl: Enable-`` and the device uses an identity domain. This was
ruled out as a cause of the TX stall above.

Workarounds on other hosts:

- kernel command line ``pci=noats``, or
- IOMMU passthrough for the device (``iommu.passthrough=1`` or an identity domain), or
- remove the ATS capability from the LA9310 PCIe config space for an EEPROM boot.

A possible kernel quirk (not applied, not tested):

::

  static void quirk_la9310_no_ats(struct pci_dev *pdev)
  {
          pci_info(pdev, "disabling ATS (no invalidation support)\n");
          pdev->ats_cap = 0;
  }
  DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_FREESCALE, 0x1c12, quirk_la9310_no_ats);

Open question for the PCIe team: does the LA9310 have an ATC or any invalidation handling?
If not, the capability should not be advertised.

i.MX95 hang on unmapped BAR0 read
---------------------------------

Reading an unmapped offset in LA9310 BAR0 from the i.MX95 host (for example
``memtool -32 0x919040300``) hangs the host: RCU stall on the CPU, then the ``fsl-se``
hsm0 driver hangs, and only a reboot recovers. Use only mapped offsets. The ADC/DAC clock
registers are at BAR0 0x910000000 + 0x1040300: ``memtool -32 0x911040300`` (CLKCFG) and
``0x911040310`` (CLKCTRL).

Runtime RX IQ swap
------------------

The I/Q order seen by the RX AXIQ differs between boards (RFNM vs LimeSDR-Micro / LMS7002M),
so the RX tone sign was inverted on one of them with a fixed ``RX_IQ_SWAP``. The swap is now
set at runtime with mailbox opcode ``MBOX_OPC_RX_IQ_SWAP`` (0x12, LSB 0 = no swap,
1 = swap, other values NACKed); it is applied on the next RX FIFO enable. ``RX_IQ_SWAP``
is the boot default (1). ``host-utils/rx-iqswap.sh`` sends it; ``load-limesdr-micro.sh``
sets 0 and ``load-rfnm.sh`` sets 1. Not available in 1T0R builds.
