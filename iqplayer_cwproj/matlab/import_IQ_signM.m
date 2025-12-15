% /* SPDX-License-Identifier: BSD-3-Clause */
% 
% /*
%  * Copyright 2024 NXP
%  */

% import and plot I/Q in VSPA sign magnitude format (16bit half fixed HF)
clc
clear all
close all

%fp = fopen("..\host-utils\tone_td_3p072Mhz_20ms_4KB300_signM.bin");  
%fp = fopen("c:\Temp\bufferDMEM.bin");  
fp = fopen("c:\Temp\txdump.bin");  
inputVec=fread(fp,'int16','l');

%R=inputVec;

% 2's comp negative values 
signIN=(sign(inputVec));
negEntries=min(signIN,0);
PosEntries=max(signIN,0);
negEntriesAbsVal=negEntries.*inputVec;
cmp1=32767.*negEntries+negEntriesAbsVal;
R=PosEntries.*inputVec + cmp1;

A=R(1:2:end);
B=R(2:2:end);

% whatever other finalization you need to do
fclose(fp);
figure(2)
subplot(2,1,1)
plot(A);
subplot(2,1,2)
plot(B);
