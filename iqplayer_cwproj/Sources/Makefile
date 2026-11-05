# SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
# Copyright 2025 NXP

PROJ_DIR ?= ${CURDIR}/..
VSPA_TOOL ?= /opt/VSPA_Tools_vbeta_14_00_781
FSVSPAIncludes ?= $(VSPA_TOOL)/include
DEST_DIR ?=  ${PROJ_DIR}/install
FIRM_DIR ?= ${DEST_DIR}/lib/firmware
VSPA_SDK ?= ${PROJ_DIR}/vspa-sdk
VSPA_LIB ?= ${PROJ_DIR}/vspa-lib

VCC = $(VSPA_TOOL)/bin/fsvspacc
VAR = $(VSPA_TOOL)/bin/vspa-ar
VLD = $(VSPA_TOOL)/bin/fsvspacc

VCFLAGS_COMMON = -arch vspa2 -au_count 16 -core_type sp -ansi off -D__LA9310__ -g -O3 -Os -opt=noalias_by_type -mvcpu -msgstyle gcc -env "$(VSPA_TOOL)" -DELD_BUILD_VERSION=$(shell date +%-m%d%H%M)
VLDFLAGS_COMMON = -arch vspa2 -au_count 16 -core_type sp -no-startup-files -ansi off -g -cwd source -O3 -Os -msgstyle gcc -env "$(VSPA_TOOL)"
INCLUDES += -I${CURDIR}/inc -I${VSPA_SDK}/inc -I${VSPA_LIB}/inc -I${PROJ_DIR}/include
LDLIBS = -l../vspa-lib/vspa-kernel-lib.a -l../vspa-sdk/vspa-sdk-lib.a

LIB_VSPA_KERNEL = $(VSPA_LIB)/vspa-kernel-lib.a
LIB_VSPA_SDK    = $(VSPA_SDK)/vspa-sdk-lib.a

CONFIGS = 1T1R 1T2R 0T4R

_OBJS_1T1R = dc_cal.o dfe.o iqmod_rx.o iqmod_tx.o l1-trace.o _main.o ovly_init.o signal.o crt0.obj
_OBJS_1T2R = dc_cal.o dfe.o iqmod_rx_2R_decx2x4.o iqmod_tx_2R.o l1-trace.o _main.o ovly_init.o signal.o crt0.obj
_OBJS_0T4R = dc_cal.o dfe.o iqmod_rx_2R_decx2x4.o l1-trace.o _main.o ovly_init.o signal.o crt0.obj

ALL_TARGETS = $(foreach cfg,$(CONFIGS),$(PROJ_DIR)/Debug_IQPLAYER_$(cfg)/apm-iqplayer-$(cfg).eld)

# 0T4R_NOQEC: same objects as 0T4R but with QEC correction disabled
ODIR_0T4R_NOQEC = $(PROJ_DIR)/Debug_IQPLAYER_0T4R_NOQEC
VCFLAGS_0T4R_NOQEC = $(VCFLAGS_COMMON) -DIQMOD_RX_0T4R -DIQMOD_RX_NO_QEC
VLDFLAGS_0T4R_NOQEC = $(VLDFLAGS_COMMON) -Xlnk "-Map $(ODIR_0T4R_NOQEC)/la9310_cal.map" -mem "$(PROJ_DIR)/LCF/la9310_iqplayer.lcf"
OBJS_0T4R_NOQEC = $(patsubst %,$(ODIR_0T4R_NOQEC)/%,$(_OBJS_0T4R))

ALL_TARGETS += $(ODIR_0T4R_NOQEC)/apm-iqplayer-0T4R_NOQEC.eld

# 0T4R_REV: diagnostic 0T4R build that services RX channels in reverse order
ODIR_0T4R_REV = $(PROJ_DIR)/Debug_IQPLAYER_0T4R_REV
VCFLAGS_0T4R_REV = $(VCFLAGS_COMMON) -DIQMOD_RX_0T4R -DRX_REVERSE_ORDER
VLDFLAGS_0T4R_REV = $(VLDFLAGS_COMMON) -Xlnk "-Map $(ODIR_0T4R_REV)/la9310_cal.map" -mem "$(PROJ_DIR)/LCF/la9310_iqplayer.lcf"
OBJS_0T4R_REV = $(patsubst %,$(ODIR_0T4R_REV)/%,$(_OBJS_0T4R))

ALL_TARGETS += $(ODIR_0T4R_REV)/apm-iqplayer-0T4R_REV.eld

.PHONY: all
all: libs $(ALL_TARGETS)

.PHONY: libs
libs:
	$(MAKE) -C $(VSPA_LIB) PROJ_DIR=$(PROJ_DIR)
	$(MAKE) -C $(VSPA_SDK) PROJ_DIR=$(PROJ_DIR)

define BUILD_template
ODIR_$1 = $(PROJ_DIR)/Debug_IQPLAYER_$1
VCFLAGS_$1 = $(VCFLAGS_COMMON) -DIQMOD_RX_$1 $(EXTRA_FLAGS_$1)
VLDFLAGS_$1 = $(VLDFLAGS_COMMON) -Xlnk "-Map $$(ODIR_$1)/la9310_cal.map" -mem "$$(PROJ_DIR)/LCF/la9310_iqplayer.lcf"
OBJS_$1 = $(patsubst %,$$(ODIR_$1)/%,$(_OBJS_$1))

$$(ODIR_$1)/apm-iqplayer-$1.eld: $$(OBJS_$1) | $(LIB_VSPA_KERNEL) $(LIB_VSPA_SDK)
	$$(VLD) -o $$@ $$(VLDFLAGS_$1) $(LDLIBS) $$^

$$(ODIR_$1)/%.o: %.c
	echo $$(ALL_TARGETS)
	mkdir -p $$(ODIR_$1)
	$$(VCC) $$(VCFLAGS_$1) $(INCLUDES) -c $$< -o $$@

$$(ODIR_$1)/%.obj: %.s
	mkdir -p $$(ODIR_$1)
	$$(VCC) $$(VCFLAGS_$1) $(INCLUDES) -c $$< -o $$@
endef

$(foreach cfg,$(CONFIGS),$(eval $(call BUILD_template,$(cfg))))

$(ODIR_0T4R_NOQEC)/apm-iqplayer-0T4R_NOQEC.eld: $(OBJS_0T4R_NOQEC) | $(LIB_VSPA_KERNEL) $(LIB_VSPA_SDK)
	$(VLD) -o $@ $(VLDFLAGS_0T4R_NOQEC) $(LDLIBS) $^

$(ODIR_0T4R_NOQEC)/%.o: %.c
	mkdir -p $(ODIR_0T4R_NOQEC)
	$(VCC) $(VCFLAGS_0T4R_NOQEC) $(INCLUDES) -c $< -o $@

$(ODIR_0T4R_NOQEC)/%.obj: %.s
	mkdir -p $(ODIR_0T4R_NOQEC)
	$(VCC) $(VCFLAGS_0T4R_NOQEC) $(INCLUDES) -c $< -o $@

$(ODIR_0T4R_REV)/apm-iqplayer-0T4R_REV.eld: $(OBJS_0T4R_REV) | $(LIB_VSPA_KERNEL) $(LIB_VSPA_SDK)
	$(VLD) -o $@ $(VLDFLAGS_0T4R_REV) $(LDLIBS) $^

$(ODIR_0T4R_REV)/%.o: %.c
	mkdir -p $(ODIR_0T4R_REV)
	$(VCC) $(VCFLAGS_0T4R_REV) $(INCLUDES) -c $< -o $@

$(ODIR_0T4R_REV)/%.obj: %.s
	mkdir -p $(ODIR_0T4R_REV)
	$(VCC) $(VCFLAGS_0T4R_REV) $(INCLUDES) -c $< -o $@

# Diagnostic variants (same objects as 0T4R).
#   0T4R_NOQEC_DIAG: 0T4R_NOQEC + stall/loss counters (costs cycles, debug only)
VARIANT_FLAGS_0T4R_NOQEC_DIAG = -DIQMOD_RX_0T4R -DIQMOD_RX_NO_QEC -DRX_STALL_STATS_ON
VARIANT_OBJS_0T4R_NOQEC_DIAG  = $(_OBJS_0T4R)
#   1T2R_DIAG: 1T2R + RX stall counters (rx_stats only, TX stats untouched);
#              TX and loop cycle meters are off to fit in PRAM
VARIANT_FLAGS_1T2R_DIAG = -DIQMOD_RX_1T2R -DRX_STALL_STATS_ON -DTX_CYCLE_METER_OFF -DLOOP_CYCLE_METER_OFF
VARIANT_OBJS_1T2R_DIAG  = $(_OBJS_1T2R)
VARIANTS = 0T4R_NOQEC_DIAG 1T2R_DIAG

define VARIANT_template
ODIR_$1 = $(PROJ_DIR)/Debug_IQPLAYER_$1
VCFLAGS_$1 = $(VCFLAGS_COMMON) $(VARIANT_FLAGS_$1)
VLDFLAGS_$1 = $(VLDFLAGS_COMMON) -Xlnk "-Map $$(ODIR_$1)/la9310_cal.map" -mem "$$(PROJ_DIR)/LCF/la9310_iqplayer.lcf"
OBJS_$1 = $(patsubst %,$$(ODIR_$1)/%,$(VARIANT_OBJS_$1))
ALL_TARGETS += $$(ODIR_$1)/apm-iqplayer-$1.eld

$$(ODIR_$1)/apm-iqplayer-$1.eld: $$(OBJS_$1) | $(LIB_VSPA_KERNEL) $(LIB_VSPA_SDK)
	$$(VLD) -o $$@ $$(VLDFLAGS_$1) $(LDLIBS) $$^

$$(ODIR_$1)/%.o: %.c
	mkdir -p $$(ODIR_$1)
	$$(VCC) $$(VCFLAGS_$1) $(INCLUDES) -c $$< -o $$@

$$(ODIR_$1)/%.obj: %.s
	mkdir -p $$(ODIR_$1)
	$$(VCC) $$(VCFLAGS_$1) $(INCLUDES) -c $$< -o $$@
endef

$(foreach v,$(VARIANTS),$(eval $(call VARIANT_template,$(v))))
all: $(foreach v,$(VARIANTS),$(PROJ_DIR)/Debug_IQPLAYER_$(v)/apm-iqplayer-$(v).eld)

clean:
	rm -rf $(PROJ_DIR)/Debug_IQPLAYER_*

# CONFIG: firmware variant to symlink as apm.eld (default 1T1R).
# Override at install time to select a different active firmware, e.g.:
#   make install CONFIG=1T2R
#   make install CONFIG=0T4R
CONFIG ?= 1T1R

install:
	mkdir -p ${FIRM_DIR}
	cp $(PROJ_DIR)/Debug_IQPLAYER_*/apm-iqplayer*.eld ${FIRM_DIR}/
	ln -sf apm-iqplayer-$(CONFIG).eld ${FIRM_DIR}/apm.eld
	@echo "apm.eld -> apm-iqplayer-$(CONFIG).eld"
