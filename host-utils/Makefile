# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2025 NXP

.ONESHELL:

VSPA_TOOL?=/opt/VSPA_Tools_vbeta_14_00_781
FSVSPAIncludes = $(VSPA_TOOL)/include
PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install

LA9310_COMMON_HEADERS ?= $(CURDIR)/../../la93xx_freertos/common_headers

CROSS_COMPILE?=aarch64-linux-gnu-

DEST_DIR ?= ${PROJ_DIR}/install

export FSVSPAIncludes DEST_DIR PROJ_DIR LA9310_COMMON_HEADERS 

DIRS := iq_monitor lib_iqplayer iq_app iq_trace vspa_mbox 

all: ${DIRS}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  all;)

clean: ${DIRS}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  clean;)

install: ${DIRS}
	mkdir -p ${DEST_DIR}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  install;)
	mkdir -p ${DEST_DIR}/home/root/host_utils
	install -D *.sh ${DEST_DIR}/home/root/host_utils
	install -D *.bin ${DEST_DIR}/home/root/host_utils
	install -D *.py ${DEST_DIR}/home/root/host_utils
