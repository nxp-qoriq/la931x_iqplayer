# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
#Copyright 2024 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install

CFLAGS  +=  -g -O0 -Wall -Werror

CROSS_COMPILE?=aarch64-linux-gnu-
CC=gcc

SRCS_TEST := vspa_mbox.c
OBJS_TEST := $(SRCS_TEST:.c =.o)

BIN_TEST := vspa_mbox
BIN_INSTALL_DIR?=${DEST_DIR}/usr/bin


.PHONY: all

all: $(BIN_TEST)

$(BIN_TEST): ${OBJS_TEST}
	$(CROSS_COMPILE)${CC} ${CFLAGS} ${LDFLAGS} -o $(BIN_TEST) ${OBJS_TEST} $(INCLUDES)

%.o: %.c
	$(CROSS_COMPILE)${CC} -c ${CFLAGS} ${INCLUDES}  $< -o $@

clean:
	rm -rf *.o $(BIN_TEST)

install:
	install -D $(BIN_TEST) ${BIN_INSTALL_DIR}/$(BIN_TEST)
