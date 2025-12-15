% /* SPDX-License-Identifier: BSD-3-Clause */
% 
% /*
%  * Copyright 2024 NXP
%  */

% convert VSPA sign magnitude format to 2'comp
clc
clear all
close all

%[fid, msg] = fopen('TM3.3_5MHz_15kHz_FDD.bin', 'rb');
[fid, msg] = fopen('c:\Temp\tx_timedomain_20ms_61440ksps_dump_ant0.bin', 'rb');
if fid == -1
   error(msg);
end
  inputVec = fread(fid, 'uint16');
fclose(fid);

ouputputVec=inputVec;
ouputputVec(ouputputVec > (2^15-1)) = -(ouputputVec(ouputputVec > (2^15-1)) - 2^15);

fid = fopen('c:\temp\output.bin', 'w');
fwrite(fid, ouputputVec,'int16');
fclose(fid);
