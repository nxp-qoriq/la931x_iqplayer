% /* SPDX-License-Identifier: BSD-3-Clause */
% 
% /*
%  * Copyright 2024 NXP
%  */

% import and plot I/Q in 2's complement format (int16)
clc
clear all
close all

%fp = fopen("../host-utils/tone_td_3p072Mhz_20ms_4KB1200_2c.bin");  
fp = fopen("c:\Temp\txdump.bin");  
inputVec=fread(fp,'int16','l');

R=inputVec;

A=R(1:2:end);
B=R(2:2:end);

fclose(fp);
figure(2)
subplot(2,1,1)
plot(A);
subplot(2,1,2)
plot(B);
