#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

BUILD		:=	build
SOURCES		:=	source calico calico/dev calico/dev/ar6k calico/nds sgIP
INCLUDES	:=	source calico/include sgIP compat build
DATA		:=

ARCH	:=	-mthumb-interwork

CFLAGS	:=	-g -Wall -Os -std=gnu11 \
		-mcpu=arm7tdmi -mtune=arm7tdmi -fomit-frame-pointer -ffunction-sections -fdata-sections \
		$(ARCH)

CFLAGS	+=	$(INCLUDE) -DARM7 -D__NDS__ -DRANET -DSDIO_DEBUG -DTMIO_DEBUG -DAR6K_DEBUG -DTWLWIFI_DEBUG -DWPA_DEBUG -Wno-unused-function -Wno-address-of-packed-member
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH) $(INCLUDE) -DARM7 -D__NDS__
LDFLAGS	=	-specs=ds_arm7.specs -g $(ARCH) -Wl,--nmagic -Wl,--gc-sections -Wl,-Map,$(notdir $*).map

LIBS	:=	-lnds7
LIBDIRS	:=	$(LIBNDS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export ARM7ELF	:=	$(CURDIR)/$(TARGET).elf
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
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
