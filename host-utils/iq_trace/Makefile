# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2024-2025 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install
LA9310_COMMON_HEADERS ?= $(CURDIR)/../../../la93xx_freertos/common_headers
LA9310_IQPLAYER_VSPA_CWPROJ ?= $(CURDIR)/../../iqplayer_cwproj
LA9310_IQPLAYER_LIB_HEADERS ?= $(CURDIR)/../lib_iqplayer


CFLAGS_COMMON  = -g -O3 -Wall -D_GNU_SOURCE -Werror -Wno-unused-variable 
CFLAGS_COMMON  += -I../common -I../../include -I. -I${LA9310_COMMON_HEADERS} -I$(LA9310_IQPLAYER_LIB_HEADERS) -I${LA9310_IQPLAYER_VSPA_CWPROJ}/include 
LDFLAGS_COMMON += -pthread -lpthread -lrt
ifeq ($(IMX8DXL),1)
	CFLAGS_COMMON  += -DIMX8DXL
endif

CC=gcc
CROSS_COMPILE?=aarch64-linux-gnu-

SRCS_TEST = iq_trace.c 
OBJS = $(subst .c,.o,$(SRCS_TEST))

CONFIGS = 0T1R 1T0R 1T1R 1T2R 1T4R

ALL_TARGETS = $(foreach cfg,$(CONFIGS),${CURDIR}/obj_$(cfg)/iq_trace_$(cfg))
all: $(ALL_TARGETS)

vspa_trace_enum.h:
	./vspa_trace_code_extract.sh ${LA9310_IQPLAYER_VSPA_CWPROJ}/include/l1-trace.h

define BUILD_template
ODIR_$1 = ${CURDIR}/obj_$1
INC_$1 = $$(ODIR_$1)/vspa_exported_symbols.h vspa_trace_enum.h
CFLAGS_$1 = $(CFLAGS_COMMON) -DIQMOD_RX_$1
OBJS_$1 = $(patsubst %,$$(ODIR_$1)/%,$(OBJS))

$$(ODIR_$1)/iq_trace_$1: $$(INC_$1) $$(OBJS_$1)
	echo $$(OBJS_$1)
	$$(CROSS_COMPILE)$$(CC) $(CFLAGS_COMMON) $$(CFLAGS_$1) $(LDFLAGS_COMMON) -o $$(ODIR_$1)/iq_trace_$1 $$(OBJS_$1) 

$$(ODIR_$1)/%.o: %.c
	mkdir -p $$(ODIR_$1)
	$$(CROSS_COMPILE)$$(CC) $$(CFLAGS_$1) $(INCLUDES) -I $$(ODIR_$1) -c $$< -o $$@

$$(ODIR_$1)/vspa_exported_symbols.h: ${LA9310_IQPLAYER_VSPA_CWPROJ}/Debug_IQPLAYER_$1/apm-iqplayer-$1.eld
	mkdir -p $$(ODIR_$1)
	./vspa_symbols_extract.sh ${LA9310_IQPLAYER_VSPA_CWPROJ}/Debug_IQPLAYER_$1/apm-iqplayer-$1.eld $$(ODIR_$1)/vspa_exported_symbols.h
endef

$(foreach cfg,$(CONFIGS),$(eval $(call BUILD_template,$(cfg))))

clean:
	rm -rf $(CURDIR)/vspa_trace_enum.h
	rm -rf $(CURDIR)/obj_*

install:
	mkdir -p  ${DEST_DIR}/usr/bin/
	install -D $(CURDIR)/obj_*/iq_trace_* ${DEST_DIR}/usr/bin/; 
	install -D $(CURDIR)/obj_1T1R/iq_trace_1T1R ${DEST_DIR}/usr/bin/iq_trace



