# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2026 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install
CC = gcc
CROSS_COMPILE ?= aarch64-linux-gnu-
CFLAGS += -g -O2 -Wall -Wextra -Werror -Wno-format-truncation -D_GNU_SOURCE

BIN := rf_tune

.PHONY: all clean install

all: $(BIN)

$(BIN): rf_tune.c
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

install:
	install -D $(BIN) ${DEST_DIR}/usr/bin/$(BIN)
