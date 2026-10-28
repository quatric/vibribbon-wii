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

MACH := -mrvl -mcpu=750 -meabi -mhard-float
DEFS := -DPSX_NO_DEBUG_TOOLS -DPSX_WII=1 $(if $(EXIT_SECONDS),-DVIB_EXIT_SECONDS=$(EXIT_SECONDS))
INC  := -Isource/shim -Isource -I$(RT)/include -I$(GEN)/main -I$(FW)/recompiler/src -I$(FW)/recompiler/lib/toml11 \
        -I$(BUILD)/libwiiinput/include -I$(LIBOGC)/include -I/opt/devkitpro/portlibs/ppc/include -I/opt/devkitpro/portlibs/wii/include
CFLAGS   := -O2 $(MACH) $(DEFS) $(INC) -ffunction-sections -fdata-sections -Wno-unused-result
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS  := $(MACH) -Wl,--gc-sections -L$(BUILD)/libwiiinput/lib -L$(LIBOGC)/lib/wii -L/opt/devkitpro/portlibs/ppc/lib -L/opt/devkitpro/portlibs/wii/lib
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

GEN_OBJS := $(foreach d,boot main bios,\
              $(patsubst $(GEN)/$(d)/%.c,$(OBJ)/gen_$(d)_%.o,$(wildcard $(GEN)/$(d)/*.c)))
HOST_C := $(wildcard source/*.c)

OBJS := $(addprefix $(OBJ)/rt_,$(addsuffix .o,$(RT_C))) \
        $(addprefix $(OBJ)/rt_,$(addsuffix .o,$(RT_CXX))) \
        $(GEN_OBJS) \
        $(patsubst source/%.c,$(OBJ)/host_%.o,$(HOST_C))

.PHONY: all clean
all: vibribbon.dol

vibribbon.dol: vibribbon.elf
	elf2dol $< $@

vibribbon.elf: $(OBJS)
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

$(OBJ):
	mkdir -p $(OBJ)

$(OBJ)/rt_%.o: $(RT)/src/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/rt_%.o: $(RT)/src/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/host_%.o: source/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# generated code: boot / main / bios each include their own decls header. The
# two game programs export identical dispatch entry points, so each gets its
# own prefix (combined again in source/wii_dispatch.c).
DISP := psx_dispatch_game_compiled psx_game_address_in_text psx_game_is_function_entry \
        psx_game_text_native_ok psx_game_text_native_ok_full
ren = $(foreach s,$(DISP),-D$(s)=$(1)_$(s))
define GENRULE
$(OBJ)/gen_$(1)_%.o: $(GEN)/$(1)/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(2) -I$(GEN)/$(1) -c $$< -o $$@
endef
$(eval $(call GENRULE,boot,$(call ren,boot)))
$(eval $(call GENRULE,main,$(call ren,main)))
$(eval $(call GENRULE,bios,))

clean:
	rm -f vibribbon.elf vibribbon.dol
	rm -rf $(OBJ)
