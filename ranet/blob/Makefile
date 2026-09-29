#---------------------------------------------------------------------------------
# ranet.bin: the in-game network stack as a blob for nds-bootstrap-ra's ARM7
# card engine (include/ra_netblob.h).  Same sources as the test app's ARM7
# side, without its host (main.c, hostlog.c, crt0).  Build: ../build.sh -C blob
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/base_tools

ARM7		:=	../arm7
MBEDTLS		:=	../../external/mbedtls
RCHEEVOS	:=	../../external/rcheevos
BUILD		:=	build

SRCDIRS		:=	. $(ARM7)/source $(ARM7)/calico $(ARM7)/calico/dev $(ARM7)/calico/dev/ar6k \
			$(ARM7)/calico/nds $(ARM7)/sgIP $(MBEDTLS)/library
CFILES		:=	$(filter-out %/main.c %/hostlog.c,$(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c)))
SFILES		:=	$(filter-out %/crt0_ranet.s,$(foreach d,$(SRCDIRS),$(wildcard $(d)/*.s)))
# rcheevos' MD5 (the award signature) as rc_md5.o: mbedTLS has an md5.c too
OFILES		:=	$(addprefix $(BUILD)/,$(notdir $(CFILES:.c=.o) $(SFILES:.s=.o))) $(BUILD)/rc_md5.o
vpath %.c $(SRCDIRS)
vpath %.s $(SRCDIRS)

INCLUDE		:=	-I. -I$(ARM7)/source -I$(ARM7)/calico/include -I$(ARM7)/sgIP -I$(ARM7)/compat \
			-I$(MBEDTLS)/include -I$(RCHEEVOS)/src/rhash -I$(DEVKITPRO)/libnds/include

ARCH		:=	-mcpu=arm7tdmi -mtune=arm7tdmi -mthumb-interwork
CFLAGS		:=	-g -Wall -Os -std=gnu11 -fomit-frame-pointer -ffunction-sections -fdata-sections \
			$(ARCH) $(INCLUDE) -DARM7 -D__NDS__ -DRANET -DRANET_BLOB \
			'-DMBEDTLS_CONFIG_FILE="mbedtls_config_arm7.h"' \
			-Wno-unused-function -Wno-address-of-packed-member
ASFLAGS		:=	-g $(ARCH) $(INCLUDE) -DARM7 -D__NDS__ -x assembler-with-cpp
LDFLAGS		:=	-nostartfiles -nostdlib -T blob.ld -Wl,--gc-sections -Wl,-Map,$(BUILD)/ranet.map $(ARCH)

all: ranet.bin

ranet.bin: $(BUILD)/ranet.elf
	$(OBJCOPY) -O binary -j .header -j .text -j .rodata -j .data $< $@
	@echo built ... $@

$(BUILD)/ranet.elf: $(OFILES) blob.ld
	$(CC) $(LDFLAGS) $(OFILES) -lc -lgcc -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/rc_md5.o: $(RCHEEVOS)/src/rhash/md5.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD) ranet.bin
