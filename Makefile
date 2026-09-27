#---------------------------------------------------------------------------------
# RA Direct (proof of concept): HTTPS from a DSi to RetroAchievements.
# devkitARM + libnds 2.x/calico (DSi-mode WiFi with WPA2), mbedTLS 2.28 from
# external/mbedtls built with include/mbedtls_config_ds.h.
#   make            -> radirect.nds
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

TARGET		:=	radirect
BUILD		:=	build
MBEDTLS		:=	external/mbedtls
RCHEEVOS	:=	external/rcheevos

GAME_TITLE	:=	RA Direct
GAME_SUBTITLE1	:=	HTTPS to RetroAchievements
GAME_SUBTITLE2	:=	step 2: rc_api

ARCH		:=	-march=armv5te -mtune=arm946e-s -mthumb

CFLAGS		:=	-g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) \
			-D__NDS__ -DARM9 -I$(CALICO)/include -I$(LIBNDS)/include \
			-Iinclude -I$(MBEDTLS)/include -I$(RCHEEVOS)/include -DRC_NO_THREADS \
			'-DMBEDTLS_CONFIG_FILE="mbedtls_config_ds.h"'

LDFLAGS		:=	-L$(CALICO)/lib -L$(LIBNDS)/lib -specs=$(CALICO)/share/ds9.specs -g $(ARCH) \
			-Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS		:=	-ldswifi9 -lfat -lnds9 -lcalico_ds9

APP_OBJ		:=	$(patsubst source/%.c,$(BUILD)/%.o,$(wildcard source/*.c))
TLS_OBJ		:=	$(patsubst $(MBEDTLS)/library/%.c,$(BUILD)/mbedtls/%.o,$(wildcard $(MBEDTLS)/library/*.c))
# rcheevos: only the web API (request builders and JSON parsers)
RC_SRC		:=	src/rapi/rc_api_common.c src/rapi/rc_api_runtime.c src/rapi/rc_api_user.c \
			src/rc_compat.c src/rc_util.c src/rc_version.c src/rhash/md5.c \
			src/rcheevos/format.c
RC_OBJ		:=	$(patsubst %.c,$(BUILD)/rcheevos/%.o,$(RC_SRC))

.PHONY: all clean

all: $(TARGET).nds

$(TARGET).nds: $(TARGET).elf

$(TARGET).elf: $(APP_OBJ) $(TLS_OBJ) $(RC_OBJ)
	@echo linking $(notdir $@)
	@$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.o: source/%.c include/mbedtls_config_ds.h include/https.h
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

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).nds
