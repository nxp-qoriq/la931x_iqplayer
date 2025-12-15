# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2025 NXP

.ONESHELL:

VSPA_TOOL ?= /opt/VSPA_Tools_vbeta_14_00_781
FSVSPAIncludes ?= $(VSPA_TOOL)/include
PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${CURDIR}/install

export FSVSPAIncludes DEST_DIR PROJ_DIR LA9310_COMMON_HEADERS 

DIRS := vspa-sdk vspa-lib Sources 

all: ${DIRS}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  all;)

clean: ${DIRS}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  clean;)

install: ${DIRS}
	mkdir -p ${DEST_DIR}
	$(foreach b, $(DIRS), ${MAKE} -C ${b}  install;)

