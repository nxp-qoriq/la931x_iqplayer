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
# Static worst-case VCPU stack check after every link: fails the build
# when the deepest call chain (frames from vspa-ds) plus margin does not
# fit in StackSize from the LCF. See tools/stack_check.py.
STACK_CHECK = VSPA_TOOL=$(VSPA_TOOL) python3 $(PROJ_DIR)/tools/stack_check.py --margin 64
LCF_FILE = $(PROJ_DIR)/LCF/la9310_iqplayer.lcf

VCFLAGS_COMMON = -arch vspa2 -au_count 16 -core_type sp -ansi off -D__LA9310__ -g -O3 -Os -opt=noalias_by_type -mvcpu -msgstyle gcc -env "$(VSPA_TOOL)" -DELD_BUILD_VERSION=$(shell date +%-m%d%H%M)
VLDFLAGS_COMMON = -arch vspa2 -au_count 16 -core_type sp -no-startup-files -ansi off -g -cwd source -O3 -Os -msgstyle gcc -env "$(VSPA_TOOL)"
INCLUDES += -I${CURDIR}/inc -I${VSPA_SDK}/inc -I${VSPA_LIB}/inc -I${PROJ_DIR}/include
LDLIBS = -l../vspa-lib/vspa-kernel-lib.a -l../vspa-sdk/vspa-sdk-lib.a

LIB_VSPA_KERNEL = $(VSPA_LIB)/vspa-kernel-lib.a
LIB_VSPA_SDK    = $(VSPA_SDK)/vspa-sdk-lib.a

CONFIGS = 1T1R 1T2R

# 1T2R PRAM is full: the main-loop and TX pass meters are left out (RX pass meter stays).
EXTRA_FLAGS_1T2R = -DLOOP_CYCLE_METER_OFF -DTX_CYCLE_METER_OFF

_OBJS_1T1R = dc_cal.o dfe.o iqmod_rx.o iqmod_tx.o l1-trace.o _main.o ovly_init.o signal.o crt0.obj
_OBJS_1T2R = dc_cal.o dfe.o iqmod_rx_2R_decx2x4.o iqmod_tx_2R.o l1-trace.o _main.o ovly_init.o signal.o crt0.obj
_OBJS_0T4R = dc_cal.o dfe.o iqmod_rx_2R_decx2x4.o l1-trace.o _main.o ovly_init.o signal.o crt0.obj

ALL_TARGETS = $(foreach cfg,$(CONFIGS),$(PROJ_DIR)/Debug_IQPLAYER_$(cfg)/apm-iqplayer-$(cfg).eld)

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

$$(ODIR_$1)/apm-iqplayer-$1.eld: $$(OBJS_$1) $(LCF_FILE) | $(LIB_VSPA_KERNEL) $(LIB_VSPA_SDK)
	$$(VLD) -o $$@ $$(VLDFLAGS_$1) $(LDLIBS) $$(filter-out $(LCF_FILE),$$^)
	$$(STACK_CHECK) $$@ $$(ODIR_$1)/la9310_cal.map

$$(ODIR_$1)/%.o: %.c
	echo $$(ALL_TARGETS)
	mkdir -p $$(ODIR_$1)
	$$(VCC) $$(VCFLAGS_$1) $(INCLUDES) -c $$< -o $$@

$$(ODIR_$1)/%.obj: %.s
	mkdir -p $$(ODIR_$1)
	$$(VCC) $$(VCFLAGS_$1) $(INCLUDES) -c $$< -o $$@
endef

$(foreach cfg,$(CONFIGS),$(eval $(call BUILD_template,$(cfg))))

# 0T4R: fused QEC+decimation build (rx_qec_decx2_8t), lossless at
# 4 x 61.44 MSPS with live QEC updates. Loop/RX cycle meters off, light
# AXIQ error path (counters only). Linked with la9310_iqplayer_0T4R.lcf
# (0x480 stack for the soft-float rx_qec_decx2_prepare, 1024 B worst case).
# Same defines and sources as the Debug_IQPLAYER_0T4R CodeWarrior config.
VARIANT_FLAGS_0T4R = -DIQMOD_RX_0T4R -DIQMOD_RX_QEC_DECX2_FUSED -DLOOP_CYCLE_METER_OFF -DRX_CYCLE_METER_OFF -DRX_AXIQ_CHK_LIGHT
VARIANT_OBJS_0T4R  = $(_OBJS_0T4R)
LCF_0T4R = $(PROJ_DIR)/LCF/la9310_iqplayer_0T4R.lcf
VARIANTS = 0T4R

define VARIANT_template
LCF_$1 ?= $(LCF_FILE)
ODIR_$1 = $(PROJ_DIR)/Debug_IQPLAYER_$1
VCFLAGS_$1 = $(VCFLAGS_COMMON) $(VARIANT_FLAGS_$1)
VLDFLAGS_$1 = $(VLDFLAGS_COMMON) -Xlnk "-Map $$(ODIR_$1)/la9310_cal.map" -mem "$$(LCF_$1)"
OBJS_$1 = $(patsubst %,$$(ODIR_$1)/%,$(VARIANT_OBJS_$1))
ALL_TARGETS += $$(ODIR_$1)/apm-iqplayer-$1.eld

$$(ODIR_$1)/apm-iqplayer-$1.eld: $$(OBJS_$1) $$(LCF_$1) | $(LIB_VSPA_KERNEL) $(LIB_VSPA_SDK)
	$$(VLD) -o $$@ $$(VLDFLAGS_$1) $(LDLIBS) $$(filter-out $$(LCF_$1),$$^)
	$$(STACK_CHECK) $$@ $$(ODIR_$1)/la9310_cal.map

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

# CONFIG: firmware variant copied as apm.eld (default 1T1R). A copy, not a
# symlink, so that deploy.sh (scp -r) installs a regular file on the board.
# Override at install time to select a different active firmware, e.g.:
#   make install CONFIG=1T2R
#   make install CONFIG=0T4R
CONFIG ?= 1T1R

# Only the supported builds are installed (stale Debug_IQPLAYER_* dirs are ignored).
FIRMWARES = $(CONFIGS) $(VARIANTS)

install:
	mkdir -p ${FIRM_DIR}
	cp $(foreach f,$(FIRMWARES),$(PROJ_DIR)/Debug_IQPLAYER_$(f)/apm-iqplayer-$(f).eld) ${FIRM_DIR}/
	rm -f ${FIRM_DIR}/apm.eld
	cp ${FIRM_DIR}/apm-iqplayer-$(CONFIG).eld ${FIRM_DIR}/apm.eld
	@echo "apm.eld = copy of apm-iqplayer-$(CONFIG).eld"
