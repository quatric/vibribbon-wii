# Vib-Ribbon for Wii: recompiled game code + a Wii host for the PSXRecomp runtime.
#
#   scripts/gen.sh "<Track 1.bin>"   # once, generates build/gen from your disc
#   make                             # builds vibribbon.dol
#
# Requires devkitPPC and libogc (DEVKITPRO=/opt/devkitpro).

ifeq ($(strip $(DEVKITPPC)),)
DEVKITPPC := /opt/devkitpro/devkitPPC
endif
export PATH := $(DEVKITPPC)/bin:$(PATH)
PREFIX := powerpc-eabi-
CC  := $(PREFIX)gcc
CXX := $(PREFIX)g++

TOP   := $(CURDIR)
BUILD := build
FW    := $(BUILD)/psxrecomp
RT    := $(FW)/runtime
GEN   := $(BUILD)/gen
OBJ   := $(BUILD)/obj
LIBOGC := /opt/devkitpro/libogc

# Screenshots every 300 frames: on in test builds (EXIT_SECONDS), SCREENSHOTS=0 to drop.
SCREENSHOTS ?= $(if $(EXIT_SECONDS),1,0)

MACH := -mrvl -mcpu=750 -meabi -mhard-float
DEFS := -DPSX_NO_DEBUG_TOOLS -DPSX_WII=1 -DPSX_NATIVE_BUILD=1 -DPSX_ENABLE_BLOCK_CYCLES=1 -DPSX_HAS_GAME_DISPATCH=1 $(if $(EXIT_SECONDS),-DVIB_EXIT_SECONDS=$(EXIT_SECONDS)) $(if $(STOP_AT),-DVIB_STOP_AT=$(STOP_AT)) $(if $(PROFILE),-DVIB_PROFILE=$(PROFILE)) $(if $(AUTOPLAY),-DVIB_AUTOPLAY=$(AUTOPLAY)) $(if $(AUTOPLAY_END),-DVIB_AUTOPLAY_END=$(AUTOPLAY_END)) $(if $(FAST),-DVIB_FAST=1) $(if $(filter 1,$(SCREENSHOTS)),-DVIB_SCREENSHOTS=1) $(if $(WAVDUMP),-DVIB_WAVDUMP=$(WAVDUMP))
INC  := -Isource/shim -Isource -I$(RT)/include -I$(GEN)/main -I$(FW)/recompiler/src -I$(FW)/recompiler/lib/toml11 \
        -I$(BUILD)/libwiiinput/include -I$(LIBOGC)/include -I/opt/devkitpro/portlibs/ppc/include -I/opt/devkitpro/portlibs/wii/include
OPT      ?= -O3
GEN_OPT  ?= -O2
CFLAGS   := $(OPT) $(MACH) $(DEFS) $(INC) -ffunction-sections -fdata-sections -Wno-unused-result
CXXFLAGS := $(CFLAGS) -std=gnu++17
# Link-time optimisation over the runtime and host only (the generated code is
# too large for it): lets device servicing and host stubs inline across files.
LTO      ?= -flto
LDFLAGS  := $(MACH) $(OPT) $(LTO) -Wl,--gc-sections -Wl,--wrap=psx_fatal_halt -Wl,--wrap=psx_crash_trace_dump -Wl,--wrap=psx_idle_note_check -B$(CURDIR)/ld/ -L$(BUILD)/libwiiinput/lib -L$(LIBOGC)/lib/wii -L/opt/devkitpro/portlibs/ppc/lib -L/opt/devkitpro/portlibs/wii/lib
LIBS     := -linput-wii -lwiikeyboard -lfat -lz -lwiiuse -lbte -logc -lm

RT_C := memory gpu gpu_sw_renderer gpu_render gpu_vram_dirty dma dma_gpu_ll timers \
        interrupts sio memcard cdrom spu mdec traps psx_cycles psx_icache bios_hle \
        bios_hle_plan psx_bios_backend load_accel color_lut crc32 host_time guest_tty \
        debug_trace_ranges crash_trace event_ring fntrace device_trace parity_trace \
        overlay_capture overlay_loader overlay_path_canon overlay_backend autocompile \
        code_provider data_shards starvation_ring latency_ring savestate \
        savestate_admission psx_rewind psx_selfcheck dirty_ram_interp \
        card_read_summary card_data_writes audio_shadow audio_trace \
        spu_shadow ws_ui_group ws_aspect_cone_math game_options controller_policy \
        controller_port_route frame_pacing frame_interpolation netplay_snap_ring \
        psx_stick freeze_dump_policy freeze_heartbeat cosim cosim_state mod_builtin_speed boot_state
RT_CXX := gte pgxp mod_runtime mod_packages text_xlate cue_sheet disc_path disc_identity

# Game programs: the boot loader plus the main executables the game chains to
# (MAIN_T title, MAIN_G game). Each is partially linked into one object.
PROGS    := boot main title
gen_objs  = $(patsubst $(GEN)/$(1)/%.c,$(OBJ)/gen_$(1)_%.o,$(wildcard $(GEN)/$(1)/*.c))
GEN_OBJS := $(foreach d,$(PROGS),$(OBJ)/prog_$(d).o) $(call gen_objs,bios)
HOST_C := $(wildcard source/*.c)

OBJS := $(addprefix $(OBJ)/rt_,$(addsuffix .o,$(RT_C))) \
        $(addprefix $(OBJ)/rt_,$(addsuffix .o,$(RT_CXX))) \
        $(GEN_OBJS) \
        $(patsubst source/%.c,$(OBJ)/host_%.o,$(HOST_C))

.SECONDEXPANSION:
.PHONY: all clean
all: vibribbon.dol

vibribbon.dol: vibribbon.elf
	elf2dol $< $@

vibribbon.elf: $(OBJS) ld/rvl.ld
	$(CXX) $(LDFLAGS) $(filter %.o,$^) $(LIBS) -o $@

$(OBJ):
	mkdir -p $(OBJ)

$(OBJ)/rt_%.o: $(RT)/src/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(LTO) -c $< -o $@
$(OBJ)/rt_%.o: $(RT)/src/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) $(LTO) -c $< -o $@
$(OBJ)/host_%.o: source/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(LTO) -c $< -o $@

# generated code: boot / main / bios each include their own decls header. The
# two game programs export identical dispatch entry points, so each gets its
# own prefix (combined again in source/wii_dispatch.c).
DISP := psx_dispatch_game_compiled psx_game_address_in_text psx_game_is_function_entry \
        psx_game_text_native_ok psx_game_text_native_ok_full
ren = $(foreach s,$(DISP),-D$(s)=$(1)_$(s))
# The generated code is most of the binary; it fits MEM1 at -O2 because the
# runtime's cold arrays live in MEM2 (ld/rvl.ld).
define GENRULE
$(OBJ)/gen_$(1)_%.o: $(GEN)/$(1)/%.c | $(OBJ)
	$(CC) $(filter-out $(OPT),$(CFLAGS)) $(GEN_OPT) $(2) -I$(GEN)/$(1) -c $$< -o $$@
endef
$(foreach d,$(PROGS),$(eval $(call GENRULE,$(d),$(call ren,$(d)))))
$(eval $(call GENRULE,bios,))

# Keep the per-file objects so a single changed file doesn't rebuild a program.
.SECONDARY: $(foreach d,$(PROGS),$(call gen_objs,$(d)))

# The main executables load at the same address, so their func_* symbols
# clash; only each program's renamed dispatch entry points stay global.
$(OBJ)/prog_%.o: $$(call gen_objs,$$*)
	$(PREFIX)ld -r $^ -o $@.tmp
	$(PREFIX)objcopy $(foreach s,$(DISP),--keep-global-symbol=$*_$(s)) $@.tmp $@
	rm -f $@.tmp

clean:
	rm -f vibribbon.elf vibribbon.dol
	rm -rf $(OBJ)
