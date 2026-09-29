#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

BUILD		:=	build
TLS		?=	1
LAYOUT		?=	ewram
SOURCES		:=	source calico calico/dev calico/dev/ar6k calico/nds sgIP
ifeq ($(TLS),1)
SOURCES		+=	../../external/mbedtls/library
endif
INCLUDES	:=	source calico/include sgIP compat ../../external/mbedtls/include build
DATA		:=

ARCH	:=	-mthumb-interwork

CFLAGS	:=	-g -Wall -Os -std=gnu11 \
		-mcpu=arm7tdmi -mtune=arm7tdmi -fomit-frame-pointer -ffunction-sections -fdata-sections \
		$(ARCH)

CFLAGS	+=	$(INCLUDE) -DARM7 -D__NDS__ -DRANET '-DMBEDTLS_CONFIG_FILE="mbedtls_config_arm7.h"' -DAR6K_DEBUG -DTWLWIFI_DEBUG -DWPA_DEBUG -Wno-unused-function -Wno-address-of-packed-member
ifneq ($(TLS),1)
CFLAGS	+=	-DRANET_NO_TLS
endif
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH) $(INCLUDE) -DARM7 -D__NDS__
ifeq ($(LAYOUT),iwram)
SPECS	:=	ds_arm7.specs
else
SPECS	:=	../ds_arm7_ewram.specs
endif
LDFLAGS	=	-specs=$(SPECS) -g $(ARCH) -Wl,--nmagic -Wl,--gc-sections -Wl,-Map,$(notdir $*).map

LIBS	:=	-lnds7
LIBDIRS	:=	$(LIBNDS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export ARM7ELF	:=	$(CURDIR)/$(TARGET).elf
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
ifneq ($(TLS),1)
CFILES		:=	$(filter-out tls.c,$(CFILES))
endif
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export LD	:=	$(CC)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) *.elf

else

DEPENDS	:=	$(OFILES:.o=.d)

$(ARM7ELF)	:	$(OFILES)
	@echo linking $(notdir $@)
	@$(LD)  $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

-include $(DEPENDS)

endif
