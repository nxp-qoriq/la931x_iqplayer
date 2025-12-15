% /* SPDX-License-Identifier: BSD-3-Clause */
% 
% /*
%  * Copyright 2024 NXP
%  */

clc
clear all
close all

fp = fopen(".\tone_td_3p072Mhz_20ms_4KB1200_2c.bin");  
R=fread(fp,'int16','l');

A=R(1:2:end);
B=R(2:2:end);
y=A+i*B;

figure
plot(real(y))
figure
plot(imag(y),'r')
%y=blackman(16384).*y;
%% plot fft
% y_fft = fftshift(fft(y));
% figure
% x=(-length(y)/2:length(y)/2-1)*160/length(y_fft);
% plot(x,20*log10(abs(y_fft)))


%plot the entire two-sided amplitude spectra
fs=61440000;
T=20e-3;        %total signal duration (20ms)
df=1/T;         %frequency resolution of the FFT
N=T*fs;         %points in the signal
f=(0:N-1)*df;   %vector of frequencies for the FFT
S0 = fft(y);
figure;
plot(f,abs(S0));
xlabel('Frequency (Hz)'); ylabel('Amplitude'); grid on
legend('fft')
