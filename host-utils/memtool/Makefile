# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2026 NXP

PROJ_DIR ?= $(CURDIR)
DEST_DIR ?= ${PROJ_DIR}/install

CFLAGS  = -g -O2 -Wall -Werror -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
LDFLAGS =

CC            = gcc
CROSS_COMPILE ?= aarch64-linux-gnu-

ODIR = ${CURDIR}/obj

SRCS = memtool.c
OBJS = $(patsubst %.c,$(ODIR)/%.o,$(SRCS))

.PHONY: all clean install

all: $(ODIR)/memtool

$(ODIR)/memtool: $(OBJS)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(ODIR)/%.o: %.c
	mkdir -p $(ODIR)
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(CURDIR)/obj

install:
	mkdir -p ${DEST_DIR}/usr/bin/
	install -D $(ODIR)/memtool ${DEST_DIR}/usr/bin/memtool
