# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2024 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install
LA9310_COMMON_HEADERS ?= $(CURDIR)/../../../la93xx_freertos/common_headers
UAPI_DIR ?= $(CURDIR)/../../../la93xx_host_sw/uapi
LA9310_IQPLAYER_LIB_HEADERS ?= $(CURDIR)/../lib_iqplayer

CFLAGS  += -g -O3 -Wall -D_GNU_SOURCE -Werror -Wno-unused-variable 
CFLAGS  += -I../common -I../lib_iqplayer -I../../include -I. -I${LA9310_COMMON_HEADERS} -I{LA9310_IQPLAYER_LIB_HEADERS} -I${UAPI_DIR}
LDFLAGS += -L$(CURDIR)/../lib_iqplayer -pthread -lpthread -lrt -liqplayer

CC=gcc
CROSS_COMPILE?=aarch64-linux-gnu-

SRCS_TEST := iq_app.c
OBJS_TEST := $(SRCS_TEST:.c =.o)
BIN_TEST := iq_app

.PHONY: all

all: $(BIN_TEST)

$(BIN_TEST): ${OBJS_TEST}
	$(CROSS_COMPILE)${CC} ${CFLAGS} -o $(BIN_TEST) ${OBJS_TEST} $(INCLUDES) ${LDFLAGS}

%.o: %.c
	$(CROSS_COMPILE)${CC} -c ${CFLAGS} ${INCLUDES}  $< -o $@

clean:
	rm -rf *.o $(BIN_TEST)

install:
	install -D $(BIN_TEST) ${DEST_DIR}/usr/bin/$(BIN_TEST)

