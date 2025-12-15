# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2025 NXP

.ONESHELL:

VSPA_TOOL ?= /opt/VSPA_Tools_vbeta_14_00_781
FSVSPAIncludes = $(VSPA_TOOL)/include

LA9310_COMMON_HEADERS ?= $(CURDIR)/../la93xx_freertos/common_headers

DEST_DIR ?= ${CURDIR}/install

export FSVSPAIncludes DEST_DIR LA9310_COMMON_HEADERS VSPA_TOOL 

DIRS := iqplayer_cwproj host-utils

all: ${DIRS}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  all;)

clean: ${DIRS}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  clean;)
	rm -rf ${DEST_DIR}

install: ${DIRS}
	mkdir -p ${DEST_DIR}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  install;)

