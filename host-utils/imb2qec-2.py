# Copyright 2022-2024 NXP
# SPDX-License-Identifier: BSD-3-Clause
####################################################################

import numpy as np
from array import array
from sys import argv

# RX QEC channel selection (firmware MBOX_OPC_IQ_CORR): msg MSB bits 17-16
# = channel, bit 18 = all channels. Options --ch N / --all may appear
# anywhere on the command line; default is channel 0. TX has one QEC set.
QEC_CH_BITS = 0
_args = []
_i = 1
while _i < len(argv):
	if argv[_i] == '--ch':
		QEC_CH_BITS = (int(argv[_i + 1], 0) & 0x3) << 16
		_i += 2
	elif argv[_i] == '--all':
		QEC_CH_BITS = 1 << 18
		_i += 1
	else:
		_args.append(argv[_i])
		_i += 1
argv[1:] = _args

def ch_msb(msb):
	v = int(msb, 16)
	if not (v & (1 << 21)):
		v |= QEC_CH_BITS
	return hex(v)

def help():
	print('\n\nSUPPORTED COMANDS: \n')
	print('QEC coefficient computation:')
	print('    imb2qec-2.py <tx|rx> <iq_gain_imb_dB> <iq_phase_imb_deg> <dc_re> <dc_im> [--ch N | --all]')
	print('    --ch N : RX channel N (default 0), --all : all RX channels')
	print('Help:')
	print('    imb2qec.py help')
	
def float_to_hex(f):
	return hex(struct.unpack('<I', struct.pack('<f', f))[0])
    
def imb2qec_tx(iq_gain_imb_dB, iq_phase_imb_deg):
	alpha   = 10**(iq_gain_imb_dB/20)
	phi_z   = iq_phase_imb_deg*np.pi/180
	
	f1 = 1/np.cos(phi_z)
	f2 = -np.tan(phi_z)/alpha
	f4 = 1/alpha
	
	print('\n**Tx QEC coeffs:**')
	print('f2 (2) = ' + float_to_hex(f2))
	print('f1 (3) = ' + float_to_hex(f1))
	print('f4 (4) = ' + float_to_hex(f4))
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x08200002') , float_to_hex(f2) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0820000b') , float_to_hex(f1) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0820000c') , float_to_hex(f4) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	
	return [f1, f2, f4]

import struct
import sys
import subprocess
def imb2qec_rx(iq_gain_imb_dB, iq_phase_imb_deg):
	gamma     = 10**(iq_gain_imb_dB/20)
	theta_z   = iq_phase_imb_deg*np.pi/180
	
	f1 = 1/gamma
	f2 = np.tan(theta_z)/gamma
	f4 = 1/np.cos(theta_z)
	
	print('\n**Rx QEC coeffs:**')
	print('f2 (2) = ' + float_to_hex(f2))
	print('f1 (3) = ' + float_to_hex(f1))
	print('f4 (4) = ' + float_to_hex(f4))
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x08000002') , float_to_hex(f2) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0800000b') , float_to_hex(f1) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0800000c') , float_to_hex(f4) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])

	return [f1, f2, f4]	

#Main#

if argv[1] == 'help':
	help()
elif argv[1] == 'tx':
	print('\n**Tx gain coeffs:**')
	print('dc_g re (1) = ' + float_to_hex(float(argv[4])))
	print('dc_g im (2) = ' + float_to_hex(float(argv[5])))
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0820000D') , float_to_hex(float(argv[4])) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0820000E') , float_to_hex(float(argv[5])) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	tx_qec_coeffs = imb2qec_tx(float(argv[2]), float(argv[3]))
	print('\nTX QEC coefficients computed successfully!')
elif argv[1] == 'rx':
	print('\n**Rx gain coeffs:**')
	print('dc_g re (1) = ' + float_to_hex(float(argv[4])))
	print('dc_g im (2) = ' + float_to_hex(float(argv[5])))
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0800000D') , float_to_hex(float(argv[4])) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	subprocess.run(['vspa_mbox', 'send','0','0',ch_msb('0x0800000E') , float_to_hex(float(argv[5])) ])
	subprocess.run(['vspa_mbox', 'recv','0','0'])
	rx_qec_coeffs = imb2qec_rx(float(argv[2]), float(argv[3]))
	print('\nRX QEC coefficients computed successfully!')	
else:
	raise Exception('\n\nWrong input! Type imb2qec.py help')