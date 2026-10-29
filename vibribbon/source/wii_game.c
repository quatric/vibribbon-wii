/* Boots the recompiled game on the PSXRecomp runtime: the same init order as
 * the desktop host (runtime/src/main.cpp), minus SDL/GL/netplay/launcher. */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <input_wii.h>
#include <wiiuse/wpad.h>

#include "cpu_state.h"
#include "gpu.h"
#include "sio.h"
#include "psx_bios_backend.h"
#include "bios_hle.h"
#include "cdrom.h"
#include "psx_scheduler.h"
#include "wii_host.h"

#define APP_DIR   "sd:/apps/vibribbon/"
#define BIOS_PATH APP_DIR "bios.bin"
#define DISC_CUE  APP_DIR "disc/Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue"

extern uint32_t psx_read_word(uint32_t);
extern void     psx_write_word(uint32_t, uint32_t);
extern uint16_t psx_read_half(uint32_t);
extern void     psx_write_half(uint32_t, uint16_t);
extern uint8_t  psx_read_byte(uint32_t);
extern void     psx_write_byte(uint32_t, uint8_t);

extern void memory_init(const char *bios_path);
extern void memory_set_sr_ptr(uint32_t *sr);
extern void psx_irq_set_cause_ptr(uint32_t *cause);
extern void dma_init(void);
extern void mdec_init(void);
extern void timers_init(void);
extern void interrupts_init(void);
extern void spu_init(void);
extern void gpu_init(void);
extern void cdrom_init(const char *disc_path);
extern void psx_icache_reset(void);

extern void dirty_ram_register_text_image(uint32_t phys_lo, const uint8_t *bytes, uint32_t len);

/* The runtime trusts recompiled code only while guest RAM still matches the
 * reference text; gen.sh builds that image from the disc (see scripts/gen.sh). */
static void arm_text_guard(void)
{
    FILE *f = fopen(APP_DIR "text.bin", "rb");
    uint8_t hdr[8];
    if (!f || fread(hdr, 1, 8, f) != 8) {
        wii_log("text.bin missing: running without the native-code guard\n");
        if (f) fclose(f);
        return;
    }
    uint32_t lo  = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    uint32_t len = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
    uint8_t *img = (uint8_t *)malloc(len);
    if (img && fread(img, 1, len, f) == len) {
        dirty_ram_register_text_image(lo, img, len);   /* the runtime owns img now */
        wii_log("text guard armed: phys 0x%08X len 0x%X\n", lo, len);
    } else {
        free(img);
        wii_log("text.bin short read\n");
    }
    fclose(f);
}

extern uint64_t g_dirty_ram_insns_run;
extern unsigned g_wii_iso_reads, g_wii_iso_fail;
extern int cdrom_get_delivered_lba(void);
extern uint32_t cdrom_get_burst_total(void);
extern uint64_t psx_get_cycle_count(void);
extern int g_idle_skip_enabled;
static CPUState s_cpu;
static volatile unsigned s_frames;

#ifdef VIB_EXIT_SECONDS
/* Debug aid for headless emulator runs: sample the guest every 2 seconds. */
extern volatile uint32_t g_psx_last_fn_entry;
static void *watchdog(void *arg)
{
    (void)arg;
    for (int i = 1; i <= VIB_EXIT_SECONDS / 2; i++) {
        sleep(2);
        wii_log("t=%ds frames=%u pc=%08X ra=%08X sp=%08X lastfn=%08X\n", i * 2, s_frames,
                s_cpu.pc, s_cpu.gpr[31], s_cpu.gpr[29], (unsigned)g_psx_last_fn_entry);
    }
    return NULL;
}

/* Debug aid: sample the dispatcher through the BIOS HLE hook it calls first. */
static int (*s_real_hook)(struct CPUState *, uint32_t);
static int trace_hook(struct CPUState *cpu, uint32_t phys)
{
    static uint32_t n;
    int r = s_real_hook ? s_real_hook(cpu, phys) : 0;
    if ((++n & 0xFFFF) == 0 || r || n < 32)
        wii_log("hook #%u phys=%08X ra=%08X r=%d\n", (unsigned)n, (unsigned)phys,
                (unsigned)cpu->gpr[31], r);
    return r;
}
#endif

/* PS1 pad word is active low: SELECT L3 R3 START UP RIGHT DOWN LEFT
 *                             L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE */
static uint16_t map_buttons(uint32_t held)
{
    uint16_t b = 0xFFFF;
    if (held & WPAD_BUTTON_MINUS) b &= ~(1u << 0);
    if (held & WPAD_BUTTON_PLUS)  b &= ~(1u << 3);
    if (held & WPAD_BUTTON_UP)    b &= ~(1u << 4);
    if (held & WPAD_BUTTON_RIGHT) b &= ~(1u << 5);
    if (held & WPAD_BUTTON_DOWN)  b &= ~(1u << 6);
    if (held & WPAD_BUTTON_LEFT)  b &= ~(1u << 7);
    if (held & WPAD_BUTTON_2)     b &= ~(1u << 12);   /* triangle */
    if (held & WPAD_BUTTON_B)     b &= ~(1u << 13);   /* circle   */
    if (held & WPAD_BUTTON_A)     b &= ~(1u << 14);   /* cross    */
    if (held & WPAD_BUTTON_1)     b &= ~(1u << 15);   /* square   */
    return b;
}

static void on_vblank(void)
{
    static unsigned frame;
    frame++;
    s_frames = frame;
    if (frame == 1)   wii_stage(9,  "first vblank");
    if (frame == 10)  wii_stage(10, "10 vblanks");
    if (frame == 120) wii_stage(11, "120 vblanks");
    if (frame == 600) wii_stage(12, "600 vblanks");
    INPUT_Update();
    sio_set_pad_state_slot(0, map_buttons(INPUT_PadsHeld()));

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (frame == 1 || frame % 60 == 0)
        wii_log("frame %u display %ux%u at (%u,%u) disabled=%d pc=%08X\n", frame,
                di.width, di.height, di.display_x, di.display_y, di.disabled, s_cpu.pc);
    if (frame % 300 == 299) {
        char name[64];
        snprintf(name, sizeof(name), APP_DIR "dump_%05u.ppm", frame);
        wii_dump_ppm(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                     (int)di.width, (int)di.height, name);
    }
    static uint64_t t_present, t_last;
    uint64_t t0 = gettime();
    if (!di.disabled && frame % 4 == 0)
        wii_present(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                    (int)di.width, (int)di.height);
    t_present += gettime() - t0;
    if (frame % 30 == 0) {
        uint64_t now = gettime();
        wii_log("perf: 30 frames %u ms (present %u), cycles %uM, cd: lba %d reads %u fail %u bursts %u\n",
                (unsigned)ticks_to_millisecs(now - t_last), (unsigned)ticks_to_millisecs(t_present),
                (unsigned)(psx_get_cycle_count() / 1000000u),
                cdrom_get_delivered_lba(), g_wii_iso_reads, g_wii_iso_fail, (unsigned)cdrom_get_burst_total());
        t_last = now; t_present = 0;
    }
}

#define GUEST_STACK_SIZE (1024 * 1024)

static void *guest_thread(void *arg)
{
    (void)arg;
    psx_scheduler_run(&s_cpu);
    return NULL;
}

void wii_run_game(void)
{
    wii_log("Vib-Ribbon (recompiled) starting\n");

    INPUT_Init();
    wii_stage(3, "libwiiinput up");

    /* gen.sh registers exactly one BIOS: the user's SCPH-5552 or OpenBIOS. */
    if (psx_bios_registry_count < 1 || !psx_bios_activate(psx_bios_registry[0]))
        wii_fatal("no BIOS backend linked");
    wii_log("bios: %s\n", psx_bios_image.image_id ? psx_bios_image.image_id : "?");
    psx_bios_hle_configure(0, 1);   /* skip the BIOS shell intro, as the desktop's fast boot does */
#ifdef VIB_EXIT_SECONDS
    s_real_hook = g_psx_bios_hle_hook;
    g_psx_bios_hle_hook = trace_hook;
#endif

    s_cpu.read_word  = psx_read_word;
    s_cpu.write_word = psx_write_word;
    s_cpu.read_half  = psx_read_half;
    s_cpu.write_half = psx_write_half;
    s_cpu.read_byte  = psx_read_byte;
    s_cpu.write_byte = psx_write_byte;

    wii_stage(4, "bios backend active");
    memory_init(BIOS_PATH);
    wii_stage(5, "memory_init done");
    gpu_init();
    dma_init();
    mdec_init();
    timers_init();
    interrupts_init();
    sio_init();
    spu_init();
    wii_stage(6, "gpu/dma/mdec/timers/irq/sio/spu init done");
    cdrom_init(DISC_CUE);
    /* GetID reports the inserted disc's licence region; this disc is PAL. */
    cdrom_set_disc_scex("SCEE");
    wii_stage(7, "cdrom_init done");
    arm_text_guard();

    sio_set_pad_connected(0, 1);
    gpu_set_vblank_callback(on_vblank);

    psx_icache_reset();
    s_cpu.pc = 0xBFC00000u;
    s_cpu.cop0[12] = 0x00400000u;       /* SR: BEV=1 */
    memory_set_sr_ptr(&s_cpu.cop0[12]);
    psx_irq_set_cause_ptr(&s_cpu.cop0[13]);

#ifdef VIB_EXIT_SECONDS
    { lwp_t t; LWP_CreateThread(&t, watchdog, NULL, NULL, 8192, LWP_PRIO_HIGHEST); }
#endif
    g_idle_skip_enabled = 1;      /* skip spin-waits for vblank instead of burning cycles */
    wii_stage(8, "about to run scheduler");
    wii_log("executing from PC=0x%08X\n", s_cpu.pc);

    /* The runtime was written for desktop stacks: some CD/overlay paths keep
     * 64-128 KB buffers on the stack, more than libogc's whole 128 KB main
     * stack. Run the guest on its own thread with room to spare. */
    void *stack = memalign(32, GUEST_STACK_SIZE);
    lwp_t guest;
    if (!stack || LWP_CreateThread(&guest, guest_thread, NULL, stack, GUEST_STACK_SIZE, 64) != 0)
        wii_fatal("could not start the guest thread");
    LWP_JoinThread(guest, NULL);

    wii_fatal("guest exited");
}
