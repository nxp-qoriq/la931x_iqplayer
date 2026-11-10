# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2024 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install
LA9310_COMMON_HEADERS ?= $(CURDIR)/../../../la93xx_freertos/common_headers
UAPI_DIR ?= $(CURDIR)/../../../la93xx_host_sw/uapi
LA9310_IQPLAYER_LIB_HEADERS ?= $(CURDIR)/../lib_iqplayer

# ARM RAL (VSPA/RF acceleration library) - cross-built via cmake
RAL_DIR   ?= $(CURDIR)/../../../ral
RAL_BUILD  = $(RAL_DIR)/build_aarch64
RAL_LIB    = $(RAL_BUILD)/libarmral.a

CFLAGS  += -g -O3 -Wall -D_GNU_SOURCE -Werror -Wno-unused-variable
CFLAGS  += -I../common -I../lib_iqplayer -I../../include -I. -I${LA9310_COMMON_HEADERS} -I${LA9310_IQPLAYER_LIB_HEADERS} -I${UAPI_DIR}
LDFLAGS += -L$(CURDIR)/../lib_iqplayer -pthread -lpthread -lrt -liqplayer

ifneq ($(wildcard $(RAL_DIR)),)
  $(info armral: using RAL_DIR=$(RAL_DIR))
  CFLAGS  += -DHAVE_ARMRAL -I$(RAL_DIR)/include
  LDFLAGS += $(RAL_LIB) -lm -lstdc++
  HAVE_RAL := 1
else
  $(warning WARNING: ral directory not found at $(RAL_DIR) -- spectrum mode will use slow DFT fallback)
  LDFLAGS += -lm
  HAVE_RAL := 0
endif

CC=gcc
CROSS_COMPILE?=aarch64-linux-gnu-

SRCS_TEST := iq_app.c
BIN_TEST := iq_app

.PHONY: all clean install lib_iqplayer build_ral

ifeq ($(HAVE_RAL),1)
all: build_ral lib_iqplayer $(BIN_TEST)
else
all: lib_iqplayer $(BIN_TEST)
endif

build_ral: $(RAL_LIB)

$(RAL_LIB):
	cmake -S $(RAL_DIR) -B $(RAL_BUILD) \
		-DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
		-DCMAKE_C_COMPILER=$(CROSS_COMPILE)$(CC) \
		-DCMAKE_CXX_COMPILER=$(CROSS_COMPILE)g++ \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(RAL_BUILD)/install
	$(MAKE) -C $(RAL_BUILD) -j$(shell nproc)

lib_iqplayer:
	$(MAKE) -C $(CURDIR)/../lib_iqplayer CROSS_COMPILE=$(CROSS_COMPILE)

$(BIN_TEST): $(SRCS_TEST)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o $(BIN_TEST) $(SRCS_TEST) $(LDFLAGS)

clean:
	rm -rf *.o $(BIN_TEST)

clean_ral:
	rm -rf $(RAL_BUILD)

install:
	install -D $(BIN_TEST) ${DEST_DIR}/usr/bin/$(BIN_TEST)
