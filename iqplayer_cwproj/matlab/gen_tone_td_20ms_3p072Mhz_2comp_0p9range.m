% /* SPDX-License-Identifier: BSD-3-Clause */
% 
% /*
%  * Copyright 2024 NXP
%  */

%% Single_tone generation
clc
clear all
close all

% txiqdata expect multiple of 4KB in size ie multiple of 1024 samples
% 61440000/1024 = 60000 4KB chunck per sec
Fs = 61440000;
duration = 20e-3; % 20ms
Ns = Fs * duration; % 300x4KB buff
f = 3072000; 
range=0.9;
y = range*exp(2*pi*1i*(0:Ns-1) *f/Fs);
%x = exp(2*pi*1i* (0:Ns-1) *f/Fs) .* linspace(0.0, 1.0, Ns);
%x = linspace(-1.0, 1.0, Ns)+ i * linspace(1.0, -1.0, Ns);

r = linspace(0, 1, Ns);
x=r.*y;

sig = nan(1, 2*length(x));
sig(1:2:end) = round(real(x)*2^15);
sig(2:2:end) = round(imag(x)*2^15);

fid = fopen('../host-utils/tone_td_3p072Mhz_20ms_4KB1200_2c.bin', 'w');
fwrite(fid, sig,'int16');
fclose(fid);

% 2's comp negative values 
signIN=(sign(sig));
negEntries=min(signIN,0);
PosEntries=max(signIN,0);
negEntriesAbsVal=negEntries.*sig;
cmp1=32767.*negEntries+negEntriesAbsVal;
R=PosEntries.*sig + cmp1;

fid = fopen('../host-utils/tone_td_3p072Mhz_20ms_4KB1200_signM.bin', 'w');
fwrite(fid, R,'int16');
fclose(fid);



