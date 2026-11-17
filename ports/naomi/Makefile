# OutRunners on a SEGA NAOMI / NAOMI 2: the Dreamcast port (../dreamcast) built for KallistiOS's naomi
# subarchitecture, with the cabinet's controls, the NetDIMM's files and the Raspberry's live block from naomi-kit.
#   source /opt/toolchains/dc/kos-naomi/environ.sh   (KOS_SUBARCH=naomi)
#   make                                             -> build/outrunners-naomi.elf
# then make_naomi.py makes the netboot image from your own ROM set (see README.md).

.DEFAULT_GOAL := all
include $(KOS_BASE)/Makefile.rules

ROOT := ../..
DC := ../dreamcast
BUILD := build
NAOMI_KIT := naomi-kit
include $(NAOMI_KIT)/kit.mk

CC := kos-cc
AR := $(KOS_AR)

CFLAGS := -std=gnu11 -Wall -Wextra -fno-strict-aliasing -ffunction-sections -fdata-sections
CFLAGS += -I$(ROOT)/runtime/include -I$(DC) $(NAOMI_KIT_CFLAGS)
CFLAGS += $(KOS_CFLAGS)
CFLAGS += -fno-fast-math
# as the Dreamcast build: the sound program runs, its samples are played by the AICA (aica_pcm.c)
CFLAGS += -DS32_SOUND_SILENT
CFLAGS += -DRT_BUILD_US=4000
# The arcade game as the cabinet has it (no ORUNNERS_CONSOLE: the PC version's menus are for a pad); its
# coins, START and the cabinet's own test menu. The data from the NetDIMM; no VMU (the SRAM, later)
CFLAGS += -DDC_NAOMI '-DDC_ASSETS="/dimm"' -DDC_NO_VMU
CFLAGS += -DS32_VIDEO_EXTRA_ELSEWHERE '-DORUNNERS_VERSION="0.4 NAOMI"' -I$(ROOT)/src
DEPFLAGS = -MMD -MP

RUNTIME_NAMES := machine io script sound ym3438 memory_census video multipcm sound_chips \
	game assets assets_media assets_midi assets_pack json png inflate sound_cpu
RUNTIME_OBJS := $(addprefix $(BUILD)/runtime/, $(addsuffix .o, $(RUNTIME_NAMES)))

GAME_SRCS := $(wildcard $(ROOT)/src/*.c $(ROOT)/src/*/*.c)
GAME_OBJS := $(patsubst $(ROOT)/src/%.c,$(BUILD)/game/%.o,$(GAME_SRCS))

# the Dreamcast port's files, all but its controls (naomi_input.c here)
DC_NAMES := main page_cache stream_media present fault_record aica_pcm pvr/drawlist pvr/render_emit \
	pvr/pvr_backend pvr/vram_alloc vmu_save startup diag_screen workers_serial tasks_kos
DC_OBJS := $(addprefix $(BUILD)/dc/, $(addsuffix .o, $(DC_NAMES)))
NAOMI_OBJS := $(BUILD)/naomi_input.o $(BUILD)/naomi_glue.o $(patsubst $(NAOMI_KIT)/src/%.c,$(BUILD)/kit/%.o,$(NAOMI_KIT_SOURCES))

ELF := $(BUILD)/outrunners-naomi.elf
OBJS := $(DC_OBJS) $(NAOMI_OBJS) $(GAME_OBJS) $(RUNTIME_OBJS)

.PHONY: all clean
all: $(ELF)

$(BUILD)/runtime/%.o: $(ROOT)/runtime/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@
$(BUILD)/game/%.o: $(ROOT)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@
$(BUILD)/dc/fault_record.o: $(DC)/fault_record.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fno-lto $(DEPFLAGS) -c $< -o $@
$(BUILD)/dc/%.o: $(DC)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@
$(BUILD)/kit/%.o: $(NAOMI_KIT)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@
$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm -Wl,-Map,$(BUILD)/outrunners-naomi.map -Wl,--wrap=exit,--wrap=abort

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
