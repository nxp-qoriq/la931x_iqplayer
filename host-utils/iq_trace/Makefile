# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2024-2025 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install
LA9310_COMMON_HEADERS ?= $(CURDIR)/../../../la93xx_freertos/common_headers
LA9310_IQPLAYER_VSPA_CWPROJ ?= $(CURDIR)/../../iqplayer_cwproj
LA9310_IQPLAYER_LIB_HEADERS ?= $(CURDIR)/../lib_iqplayer
UAPI_DIR  ?= $(CURDIR)/../../../la93xx_host_sw/uapi

CFLAGS  = -g -O3 -Wall -D_GNU_SOURCE -Werror -Wno-unused-variable
CFLAGS  += -I../common -I../../include -I. -I${LA9310_COMMON_HEADERS}
CFLAGS  += -I$(LA9310_IQPLAYER_LIB_HEADERS)
CFLAGS  += -I${LA9310_IQPLAYER_VSPA_CWPROJ}/include
CFLAGS  += -I${UAPI_DIR}
LDFLAGS += -pthread -lpthread -lrt
ifeq ($(IMX8DXL),1)
	CFLAGS += -DIMX8DXL
endif

CC            = gcc
CROSS_COMPILE ?= aarch64-linux-gnu-

ODIR = ${CURDIR}/obj

SRCS = iq_trace.c
OBJS = $(patsubst %.c,$(ODIR)/%.o,$(SRCS))

.PHONY: all clean install

all: vspa_trace_enum.h $(ODIR)/iq_trace

vspa_trace_enum.h:
	./vspa_trace_code_extract.sh ${LA9310_IQPLAYER_VSPA_CWPROJ}/include/l1-trace.h

$(ODIR)/iq_trace: $(OBJS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(ODIR)/%.o: %.c vspa_trace_enum.h
	mkdir -p $(ODIR)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -I$(ODIR) -c $< -o $@

clean:
	rm -rf $(CURDIR)/vspa_trace_enum.h
	rm -rf $(CURDIR)/obj

install:
	mkdir -p ${DEST_DIR}/usr/bin/
	install -D $(ODIR)/iq_trace ${DEST_DIR}/usr/bin/iq_trace
