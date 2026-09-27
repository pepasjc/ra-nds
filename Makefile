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

GAME_TITLE	:=	RA Direct
GAME_SUBTITLE1	:=	HTTPS to RetroAchievements
GAME_SUBTITLE2	:=	proof of concept

ARCH		:=	-march=armv5te -mtune=arm946e-s -mthumb

CFLAGS		:=	-g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) \
			-D__NDS__ -DARM9 -I$(CALICO)/include -I$(LIBNDS)/include \
			-Iinclude -I$(MBEDTLS)/include \
			'-DMBEDTLS_CONFIG_FILE="mbedtls_config_ds.h"'

LDFLAGS		:=	-L$(CALICO)/lib -L$(LIBNDS)/lib -specs=$(CALICO)/share/ds9.specs -g $(ARCH) \
			-Wl,--gc-sections -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS		:=	-ldswifi9 -lnds9 -lcalico_ds9

APP_OBJ		:=	$(patsubst source/%.c,$(BUILD)/%.o,$(wildcard source/*.c))
TLS_OBJ		:=	$(patsubst $(MBEDTLS)/library/%.c,$(BUILD)/mbedtls/%.o,$(wildcard $(MBEDTLS)/library/*.c))

.PHONY: all clean

all: $(TARGET).nds

$(TARGET).nds: $(TARGET).elf

$(TARGET).elf: $(APP_OBJ) $(TLS_OBJ)
	@echo linking $(notdir $@)
	@$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/%.o: source/%.c include/mbedtls_config_ds.h
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/mbedtls/%.o: $(MBEDTLS)/library/%.c include/mbedtls_config_ds.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).nds
