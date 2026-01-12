.. sectnum::

**IQ Player — Quick Start Guide on RFNM Board (i.MX8M Plus)** 
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Overview
********

The RFNM board (LA9310 + i.MX8M Plus) is the reference evaluation platform used by NXP for the LA9310 device. 
This board is supported by default in the NXP LA9310 Linux BSP.
Users can generate Linux images using the NXP BSP or directly use prebuilt images available on www.nxp.com.

Hardware Requirements
*********************

*	RFNM motherboard with LA9310 

*	RF modules options :

 - limeRF/LMS7002M (recommended)
 - Yucca/MT3812
 - RF breakout boards 

refer to https://rfnm.com/ for details and quick start guide for initial setup

Hardware Setup
**************

•	UART console access (TTL-232R-RPI) or SSH; login root/root.
•	Power supply: 12 V / 5 A recommended.

.. image:: rfnm-board.png

RFNM official firmware
**********************

RFNM provides an official, productized firmware environment enabling various SDR software frameworks, such as GNU Radio and SoapySDR drivers.
Users can run SDR applications (e.g. SDR++) directly on a laptop when connecting the RFNM board over USB. refer to

-  'rfnm_shipping_intro_doc.pdf'

The RFNM VSPA firmware is derived from IQ Player.

RFNM software sources are available at:

- https://github.com/rfnm

Recommended Usage Models

- Standard SDR usage : Users developing GNU Radio–based applications should use the RFNM software environment directly, rather than the raw NXP BSP and IQ Player code.
- Custom LA9310 development : Users developing custom LA9310 firmware or applications should start from the NXP BSP and IQ Player sources.

Best practice: Keep the official RFNM firmware in Flash, and deploy the NXP BSP with IQ Player on an SD card. This allows switching between:

- RFNM image → standard SDR tools (e.g. SDR++)
- NXP image → IQ Player utilities

IQ Player Software Components
******************************

The RFNM board (LA9310 + i.MX8M Plus) is supported by default in the NXP LA9310 Linux BSP.

IQ Player components are preinstalled in the NXP BSP. NXP provides prebuilt SD card images downloadable from nxp.com.

Example BSP Version : i.MX8MP–LA9310 BSP 1.0

- Linux kernel: 5.15‑rt
- Real‑Time Edge Software Project v2.5
- ARM RAL 24.01
- DPDK 22.11

Sources: 

- https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/tree/la93xx


Download and deploy pre-built images
************************************

1/ Download prebuilt image 
::

 wget https://www.nxp.com/lgfiles/sdk/la1224/imx-la9310-sdk-10/nxp-image-real-time-edge-imx8mp-sdr.rootfs.wic.zst

2/ Decompress and program SD card
::

 zstd -d nxp-image-real-time-edge-imx8mp-sdr.rootfs.wic.zst 
 sudo dd if= nxp-image-real-time-edge-imx8mp-sdr.rootfs.wic of=/dev/sdb bs=8M oflag=direct status=progress
 Sync

3/ Boot from SD card
- Insert the SD card
- Set DIP switches to SD
- Power on the board; the NXP image should boot

Build LA9310 BSP images for RFNM
********************************

please refer to https://github.com/nxp-real-time-edge-sw/yocto-real-time-edge/blob/la93xx/IMX8MP-LA9310-UG10193_Rev.1.0.pdf

Linux console
*************

UART access requires a 3.3 V TTL‑to‑UART adapter.

- One UART for i.MX8MP / Linux
- One UART for LA9310 / FreeRTOS (not required for IQ Player usage)

::

 minicom -D /dev/ttyUSB0 -b 115200 stty -F /dev/ttyUSB0 115200


Play and Capture I/Q Waveforms
******************************

Load and start VSPA firmware
::

 ./load-nlm.sh

Confugre LimeRF
::

 ./config_run_rf.sh <limetx|/limerx>

Replay waveform (continuous TX)
::

 ./iq-replay.sh ./tone_td_3p072Mhz_20ms_4KB1200_2c.bin 1200

Capture IQ samples (single-shot RX)
::

 ./iq-capture.sh ./iqdata.bin 1200

Capture IQ samples (continuous DDR capture)
::

 ./iq-capture-ddr.sh 1200
 ./iq-stop.sh
 
TX QEC Manual Calibration Example
**********************************

Python script for manual TX/RX QEC calibration.
Inputs are specified in dB and degrees, coefficients are computed and applied to VSPA.
::

  imb2qec.py <tx|rx> <iq_gain_imb_dB> <iq_phase_imb_deg> <dc_re> <dc_im>


Support for MT3812 RF Module (Yucca)
************************************

Initialize the MT3812 RFIC using Python scripts based on the Metanoia SDK (SPI control).
::

 export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/root/rf_ctrl/fr1/
 python3 /home/root/rf_ctrl/fr1/fr1.py 
 set_log_level 3
 rf_init /home/root/rf_ctrl/utils/fr1/fr1_def_config.json
 set_active act_mode 0 receiver 0
 set_path path 0 band 2 rssi 0 dpd 0 rx_bw 4 tx_bw 0
 set_path path 12 band 2 rssi 0 dpd 0 rx_bw 4 tx_bw 0
 set_active act_mode 1 receiver 0
