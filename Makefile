#---------------------------------------------------------------------------------
# RA-NDS: RetroAchievements straight from a DSi.
# devkitARM + libnds 2.x/calico (DSi-mode WiFi with WPA2), mbedTLS 2.28 from
# external/mbedtls built with include/mbedtls_config_ds.h, rcheevos' web API.
#   make  -> ratest.nds (test: every set on the card against RA)
#            rasync.nds   (RA Sync direct: set prep before a game, unlocks after)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

APPS		:=	ratest rasync
BUILD		:=	build
MBEDTLS		:=	external/mbedtls
RCHEEVOS	:=	external/rcheevos

ratest.nds: GAME_TITLE := RA-NDS test
ratest.nds: GAME_SUBTITLE1 := HTTPS to RetroAchievements
ratest.nds: GAME_SUBTITLE2 := test: every set on the card
rasync.nds: GAME_TITLE := RA Sync
rasync.nds: GAME_SUBTITLE1 := RetroAchievements for DS
rasync.nds: GAME_SUBTITLE2 := sets before, unlocks after

ARCH		:=	-march=armv5te -mtune=arm946e-s -mthumb

CFLAGS		:=	-g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) \
			-D__NDS__ -DARM9 -I$(CALICO)/include -I$(LIBNDS)/include \
			-Iinclude -I$(MBEDTLS)/include -I$(RCHEEVOS)/include -I$(RCHEEVOS)/src/rhash \
			-DRC_NO_THREADS '-DMBEDTLS_CONFIG_FILE="mbedtls_config_ds.h"'

LDFLAGS		:=	-L$(CALICO)/lib -L$(LIBNDS)/lib -specs=$(CALICO)/share/ds9.specs -g $(ARCH) \
			-Wl,--gc-sections
LIBS		:=	-ldswifi9 -lfat -lnds9 -lcalico_ds9

COMMON_OBJ	:=	$(patsubst source/%.c,$(BUILD)/%.o,$(wildcard source/*.c))
TLS_OBJ		:=	$(patsubst $(MBEDTLS)/library/%.c,$(BUILD)/mbedtls/%.o,$(wildcard $(MBEDTLS)/library/*.c))
# rcheevos: only the web API (request builders and JSON parsers) and md5
RC_SRC		:=	src/rapi/rc_api_common.c src/rapi/rc_api_runtime.c src/rapi/rc_api_user.c \
			src/rc_compat.c src/rc_util.c src/rc_version.c src/rhash/md5.c \
			src/rcheevos/format.c
RC_OBJ		:=	$(patsubst %.c,$(BUILD)/rcheevos/%.o,$(RC_SRC))

.PHONY: all clean

all: $(addsuffix .nds,$(APPS))

$(addsuffix .elf,$(APPS)): %.elf: $(BUILD)/app/%.o $(COMMON_OBJ) $(TLS_OBJ) $(RC_OBJ)
	@echo linking $(notdir $@)
	@$(CC) $(LDFLAGS) -Wl,-Map,$(BUILD)/$*.map $^ $(LIBS) -o $@

$(BUILD)/app/%.o: app/%.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: source/%.c $(wildcard include/*.h)
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mbedtls/%.o: $(MBEDTLS)/library/%.c include/mbedtls_config_ds.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/rcheevos/%.o: $(RCHEEVOS)/%.c
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

.SECONDARY:

clean:
	@rm -rf $(BUILD) $(addsuffix .elf,$(APPS)) $(addsuffix .nds,$(APPS))
