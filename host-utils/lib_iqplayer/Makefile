# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2024 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install
LA9310_IQPLAYER_VSPA_CWPROJ ?= $(CURDIR)/../../iqplayer_cwproj

CFLAGS  +=  -g -O3 -Wall -D_GNU_SOURCE -Werror -Wno-unused-variable -I../common -I../../include -I. -I${LA9310_IQPLAYER_VSPA_CWPROJ}/include
LDFLAGS += -pthread -lpthread -lrt

CC=gcc
CROSS_COMPILE?=aarch64-linux-gnu-

SRCS_TEST := libiqplayer.c
OBJS_TEST := $(SRCS_TEST:.c =.o)
BIN_TEST := lib_iqplayer.a

.PHONY: all

all: $(BIN_TEST)

$(BIN_TEST): ${OBJS_TEST}
	$(CROSS_COMPILE)${CC} -c ${CFLAGS} ${LDFLAGS} -o $(BIN_TEST) ${OBJS_TEST} $(INCLUDES)

%.o: %.c
	$(CROSS_COMPILE)${CC} -c ${CFLAGS} ${INCLUDES}  $< -o $@

clean:
	rm -rf *.o $(BIN_TEST)

install:
	install -D $(BIN_TEST) ${DEST_DIR}/usr/lib/$(BIN_TEST)

