ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment.")
endif
include $(DEVKITPPC)/wii_rules
TARGET   := boot
BUILD    := build
SOURCES  := source
CFLAGS   = -g -O2 -Wall $(MACHDEP) $(INCLUDE)
LDFLAGS  = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS     := -lfat -lwiiuse -lbte -logc -lm
LIBDIRS  := $(DEVKITPRO)/libogc
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)
export INCLUDE := -I$(LIBOGC_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS := -L$(LIBOGC_LIB)
.PHONY: $(BUILD) clean
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol
else
$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPSDIR)/*.d
endif
