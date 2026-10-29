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
#include "spu.h"
#include "psx_scheduler.h"
#include "wii_host.h"
#ifdef VIB_PROFILE
#include <tuxedo/thread.h>
#endif

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
 * reference text; gen.sh builds one image per main program from the disc. */
static uint8_t *load_text_image(const char *path, uint32_t *lo, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[8];
    uint8_t *img = NULL;
    if (f && fread(hdr, 1, 8, f) == 8) {
        *lo  = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
        *len = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
        img = (uint8_t *)malloc(*len);
        if (img && fread(img, 1, *len, f) != *len) { free(img); img = NULL; }
    }
    if (f) fclose(f);
    if (!img) wii_log("%s missing or short: that program runs interpreted\n", path);
    return img;
}

static void arm_text_guard(void)
{
    static const char *const files[WII_MAIN_PROGS] = { APP_DIR "text.bin", APP_DIR "text_t.bin" };
    for (int k = 0; k < WII_MAIN_PROGS; k++) {
        uint32_t lo, len;
        uint8_t *img = load_text_image(files[k], &lo, &len);
        if (!img) continue;
        wii_dispatch_set_text(k, lo, img, len);
        if (k == 0)   /* the game runtime owns the first image's guard state */
            dirty_ram_register_text_image(lo, img, len);
        wii_log("text guard %d armed: phys 0x%08X len 0x%X\n", k, lo, len);
    }
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

#ifdef VIB_PROFILE
static void profile_flush(void);
#endif

#ifdef VIB_EXIT_SECONDS
/* Debug aid: what the guest has asked of the CD drive lately. */
static void cd_dump(void)
{
    static uint64_t seen_cmd, seen_sec;
    CDROMDebugState d;
    cdrom_debug_snapshot(&d);
    wii_log("cd: mode %02X reading %d cmd %02X filter %u/%u muted %u xa %d stat %02X lba %d\n",
            d.mode_reg, d.reading, d.read_cmd, d.filter_file, d.filter_channel, d.muted,
            cdrom_xa_stream_active(), d.stat_reg, d.last_sector_lba);
    const CDROMCommandHistoryEntry *ch;
    uint64_t n = cdrom_debug_get_command_history(&ch);
    uint64_t from = n > seen_cmd + 12 ? n - 12 : seen_cmd;
    for (uint64_t i = from; i < n; i++) {
        const CDROMCommandHistoryEntry *e = &ch[i % CDROM_COMMAND_HISTORY_CAP];
        wii_log("  cmd #%llu f%u pc %08X k%u %02X [%02X %02X %02X] mode %02X\n",
                (unsigned long long)i, (unsigned)e->frame, (unsigned)e->pc, e->kind, e->cmd,
                e->params[0], e->params[1], e->params[2], e->mode);
    }
    seen_cmd = n;
    const CDROMSectorHistoryEntry *sh;
    n = cdrom_debug_get_sector_history(&sh);
    from = n > 4 ? n - 4 : 0;
    if (from < seen_sec && n > seen_sec) from = seen_sec;
    for (uint64_t i = from; i < n; i++) {
        const CDROMSectorHistoryEntry *e = &sh[i % CDROM_SECTOR_HISTORY_CAP];
        wii_log("  sec lba %d size %d xa %u/%u sub %02X cod %02X data %u xaout %u skip %u\n",
                e->lba, e->size, e->xa_file, e->xa_channel, e->xa_submode, e->xa_coding,
                e->data_delivered, e->xa_audio_delivered, e->skip_reason);
    }
    seen_sec = n;
}
#endif

#define PACE_QUEUED_FRAMES 4800    /* 100 ms at 48 kHz */

static void on_vblank(void)
{
    static unsigned frame;
    frame++;
#ifdef VIB_PROFILE
    profile_flush();
#endif
    s_frames = frame;
#ifdef VIB_WAVDUMP
    { extern void wii_audio_wavdump(unsigned frame); wii_audio_wavdump(frame); }
#endif
    if (frame == 1)   wii_stage(9,  "first vblank");
    if (frame == 10)  wii_stage(10, "10 vblanks");
    if (frame == 120) wii_stage(11, "120 vblanks");
    if (frame == 600) wii_stage(12, "600 vblanks");
    /* Frame pacing: audio output is produced from guest time and drained in
     * real time, so a full queue means the guest is running ahead. */
    while (wii_audio_queued() > PACE_QUEUED_FRAMES)
        usleep(1000);

    INPUT_Update();
    uint16_t pad = map_buttons(INPUT_PadsHeld());
#ifdef VIB_AUTOPLAY
    /* Debug aid for headless runs: from frame VIB_AUTOPLAY to VIB_AUTOPLAY_END,
     * tap Start and then Cross every 300 frames to walk through the menus into
     * the first course. It must stop there: Start pauses a course. */
#ifndef VIB_AUTOPLAY_END
#define VIB_AUTOPLAY_END (VIB_AUTOPLAY + 1660)
#endif
    if (frame >= VIB_AUTOPLAY && frame < VIB_AUTOPLAY_END) {
        unsigned k = (frame - VIB_AUTOPLAY) % 300;
        if (k < 6)                pad &= ~(1u << 3);    /* start */
        if (k >= 150 && k < 156)  pad &= ~(1u << 14);   /* cross */
    }
#endif
    sio_set_pad_state_slot(0, pad);

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (frame == 1 || frame % 60 == 0)
        wii_log("frame %u display %ux%u at (%u,%u) disabled=%d pc=%08X\n", frame,
                di.width, di.height, di.display_x, di.display_y, di.disabled, s_cpu.pc);
#ifdef VIB_EXIT_SECONDS
    if (frame % 150 == 149) {
        cd_dump();
        extern uint32_t i_mask;
        SpuGlobalState g;
        spu_get_global_state(&g);
        wii_log("spu: ctrl %04X irq addr %05X flag %u mask %u reverb base %05X cur %05X capture %03X active %06X\n",
                g.ctrl, (unsigned)g.irq_addr, g.irq_flag, (unsigned)((i_mask >> 9) & 1), (unsigned)g.reverb_mbase,
                (unsigned)g.reverb_cur, (unsigned)g.capture_pos, (unsigned)g.active_mask);
    }
#endif
    if (frame % 300 == 299) {
        char name[64];
        snprintf(name, sizeof(name), APP_DIR "dump_%05u.ppm", frame);
        wii_dump_ppm(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                     (int)di.width, (int)di.height, name);
    }
    static uint64_t t_present, t_last;
    uint64_t t0 = gettime();
    if (frame == 1) t_last = t0;
    /* Show every 2nd frame while the guest keeps ahead of real time (audio
     * queued past 50 ms), every 4th while it is behind. */
    unsigned every = wii_audio_queued() > 2400 ? 2 : 4;
    if (!di.disabled && frame % every == 0)
        wii_present(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                    (int)di.width, (int)di.height);
    t_present += gettime() - t0;
    if (frame % 30 == 0) {
        uint64_t now = gettime();
        wii_log("perf: 30 frames %u ms (present %u), cycles %uM, cd: lba %d reads %u fail %u bursts %u\n",
                (unsigned)ticks_to_millisecs(now - t_last), (unsigned)ticks_to_millisecs(t_present),
                (unsigned)(psx_get_cycle_count() / 1000000u),
                cdrom_get_delivered_lba(), g_wii_iso_reads, g_wii_iso_fail, (unsigned)cdrom_get_burst_total());
        {
            unsigned under, drop, rendered;
            wii_audio_stats(&under, &drop, &rendered);
            extern uint64_t g_vsync_skip_count, g_vsync_skip_cycles;
            wii_log("audio: queued %u underruns %u dropped %u spu %u frames; vsync skips %llu (%lluM cycles)\n",
                    (unsigned)wii_audio_queued(), under, drop, rendered,
                    (unsigned long long)g_vsync_skip_count,
                    (unsigned long long)(g_vsync_skip_cycles / 1000000u));
        }
        {
            extern void dirty_ram_text_exact_mismatch_stats(uint64_t *count, uint32_t out[5]);
            uint64_t mm; uint32_t st[5];
            dirty_ram_text_exact_mismatch_stats(&mm, st);
            wii_log("text: mismatches %llu last range %08X+%u at %08X live %02X ref %02X\n",
                    (unsigned long long)mm, (unsigned)st[0], (unsigned)st[1], (unsigned)st[2],
                    (unsigned)st[3], (unsigned)st[4]);
        }
        t_last = now; t_present = 0;
    }
}

#define GUEST_STACK_SIZE (1024 * 1024)

#ifdef VIB_PROFILE
/* Debug aid: a top-priority thread wakes every ~500 us and records where the
 * guest thread was preempted (the kernel saves its pc/lr in the KThread).
 * Samples go to SD as (pc, lr, guest pc) triples; symbolise them with nm on the host. */
static KThread *volatile s_guest_kthread;
#define PROF_SAMPLES 16384
static uint32_t s_prof[PROF_SAMPLES][3];
static volatile unsigned s_prof_done, s_prof_idle;

static void *profiler(void *arg)
{
    (void)arg;
    for (int i = 0; i < VIB_PROFILE; i++) sleep(1);   /* skip boot; one long sleep overflows the tick count */
    wii_log("profile: sampling\n");
    unsigned n = 0, idle = 0;
    while (n < PROF_SAMPLES) {
        usleep(500);
        KThread *t = s_guest_kthread;
        if (!t) continue;
        if (t->state != KTHR_STATE_RUNNING) { idle++; continue; }
        s_prof[n][0] = t->ctx.pc;
        s_prof[n][1] = t->ctx.lr;
        s_prof[n][2] = s_cpu.pc;
        n++;
    }
    s_prof_idle = idle;
    s_prof_done = n;          /* the guest thread writes the file: libfat is not thread-safe */
    return NULL;
}

static void profile_flush(void)
{
    static int written;
    if (!s_prof_done || written) return;
    written = 1;
    FILE *f = fopen(APP_DIR "prof.bin", "wb");
    if (f) { fwrite(s_prof, sizeof(s_prof[0]), s_prof_done, f); fclose(f); }
    wii_log("profile: %u samples written, %u idle\n", s_prof_done, s_prof_idle);
}
#endif

static void *guest_thread(void *arg)
{
    (void)arg;
#ifdef VIB_PROFILE
    s_guest_kthread = KThreadGetSelf();
#endif
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
    wii_audio_init();
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
    /* The runtime's generic idle detector stays off, as on desktop (game.toml
     * leaves idle_skip off); enabling it routes interrupt checks to the
     * VSync wait skip in wii_idle.c instead. */
    g_idle_skip_enabled = 1;
#ifdef VIB_FAST
    {   /* make FAST=1: GPU linked-list DMA predicted and walked lazily
         * (patch_runtime.py); not packet-exact if the CPU rewrites a list in flight */
        extern int g_dma_gpu_ll_fast;
        g_dma_gpu_ll_fast = 1;
    }
#endif
    wii_stage(8, "about to run scheduler");
    wii_log("executing from PC=0x%08X\n", s_cpu.pc);

    /* The runtime was written for desktop stacks: some CD/overlay paths keep
     * 64-128 KB buffers on the stack, more than libogc's whole 128 KB main
     * stack. Run the guest on its own thread with room to spare. */
    void *stack = memalign(32, GUEST_STACK_SIZE);
    lwp_t guest;
    if (!stack || LWP_CreateThread(&guest, guest_thread, NULL, stack, GUEST_STACK_SIZE, 64) != 0)
        wii_fatal("could not start the guest thread");
#ifdef VIB_PROFILE
    { lwp_t t; LWP_CreateThread(&t, profiler, NULL, NULL, 16384, LWP_PRIO_HIGHEST); }
#endif
    LWP_JoinThread(guest, NULL);

    wii_fatal("guest exited");
}
