#Makefile for an autoexec.bin able to run on all cameras

# This makefile is broken after the make refactor.
# It's used for installer, and I think "rescue"

x: prepare-all autoexec-all autoexec.bin
#	cp autoexec-all.bin autoexec.bin
	$(call build,STAT,$(STAT_CMD) autoexec.bin)

prepare-all:
	$(MAKE) -C .. all-supported-unified-models

reboot-all.o : prepare-all

autoexec.bin: autoexec-all.bin
	$(call build,CP,$(CP) autoexec-all.bin autoexec.bin)

autoexec-all: reboot-all.o
	$(call build,LD,$(LD) \
		-o autoexec-all \
		-nostdlib \
		-e _start \
		-Ttext 0x800000 \
		$^ \
	); \

MODEL := UNIFIED

ML_MODEL_DIR = all


zip_contents:: $(PLATFORM_INSTALL_SUPPORTED_TARGETS) autoexec.bin
	$(call build,CP,$(CP) autoexec.bin $(INSTALL_DIR)/)

$(PLATFORM_INSTALL_ALL_TARGETS):
	$(call call_make_platform)

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

PLATFORM_ARCH = armv5te
ML_BOOT_OBJ = boot-d45.o

MAIN_FIRMWARE_ADDR = 0xFF010000

RESTARTSTART = 0

ML_SRC_REBOOT_OBJ = reboot-all.o

CONFIG_MODULES = n

include  ../Makefile

# Makefile to build and collect all camera zips into a single archive

PLATFORM_DIR = ..
BUILD_DIR = build

include ../Makefile  # This brings in ML_CAMS

# Where to collect all per-cam zips
ALL_ZIP_DIR = $(BUILD_DIR)/all_zips
ALL_CAMS_ZIP = $(BUILD_DIR)/magiclantern.zip

# Each cam's zip is expected at <cam>/build/magiclantern.zip
CAM_ZIPS = $(addsuffix /build/magiclantern.zip, $(ML_CAMS))

.PHONY: all all_cams_zip clean cams_zips

all: all_cams_zip

# Build all camera zips
cams_zips:
	@for cam in $(ML_CAMS); do \
	  echo "Building $$cam..."; \
	  $(MAKE) -C ../$$cam; \
	done

# Collect all zips and create a single archive
all_cams_zip: cams_zips | $(ALL_ZIP_DIR)
	@echo "Copying all camera zips to $(ALL_ZIP_DIR)..."
	@rm -f $(ALL_ZIP_DIR)/*.zip
	@for cam in $(ML_CAMS); do \
	  cp ../$$cam/build/magiclantern.zip $(ALL_ZIP_DIR)/magiclantern-$$cam.zip || true; \
	done
	@echo "Creating $(ALL_CAMS_ZIP)..."
	@cd $(ALL_ZIP_DIR) && zip -r ../magiclantern.zip *.zip
	@ls -l $(ALL_CAMS_ZIP)
	@echo "All camera zips collected in $(ALL_CAMS_ZIP)"

$(ALL_ZIP_DIR):
	@mkdir -p $@

clean:
	@for cam in $(ML_CAMS); do \
	  $(MAKE) -C ../$$cam clean; \
	done
	@rm -rf $(ALL_ZIP_DIR) $(ALL_CAMS_ZIP)

.DEFAULT_GOAL := all
