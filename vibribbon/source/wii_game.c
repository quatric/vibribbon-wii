/* Boots the recompiled game on the PSXRecomp runtime: the same init order as
 * the desktop host (runtime/src/main.cpp), minus SDL/GL/netplay/launcher. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <input_wii.h>
#include <wiiuse/wpad.h>

#include "cpu_state.h"
#include "gpu.h"
#include "sio.h"
#include "psx_bios_backend.h"
#include "psx_scheduler.h"
#include "wii_host.h"

#define APP_DIR   "sd:/apps/vibribbon/"
#define BIOS_PATH APP_DIR "openbios.bin"
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

static CPUState s_cpu;
static volatile unsigned s_frames;

#ifdef VIB_EXIT_SECONDS
/* Debug aid for headless emulator runs: report state, then power off cleanly so
 * the emulator flushes the SD card folder. */
static void *watchdog(void *arg)
{
    (void)arg;
    for (int i = 0; i < VIB_EXIT_SECONDS; i++) {
        sleep(1);
        if (i % 5 == 4)
            wii_log("t=%ds frames=%u pc=%08X ra=%08X sp=%08X\n", i + 1, s_frames,
                    s_cpu.pc, s_cpu.gpr[31], s_cpu.gpr[29]);
    }
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    wii_dump_ppm(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                 (int)di.width, (int)di.height, APP_DIR "final.ppm");
    wii_dump_ppm(gpu_get_vram(), 0, 0, 1024, 512, APP_DIR "vram.ppm");
    wii_log("watchdog: exiting\n");
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    return NULL;
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
    INPUT_Update();
    sio_set_pad_state_slot(0, map_buttons(INPUT_PadsHeld()));

    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (frame % 300 == 0)
        wii_log("frame %u display %ux%u at (%u,%u) disabled=%d\n", frame,
                di.width, di.height, di.display_x, di.display_y, di.disabled);
    if (frame == 600 || frame == 1800 || frame == 3600 || frame == 7200) {
        char name[64];
        snprintf(name, sizeof(name), APP_DIR "dump_%05u.ppm", frame);
        wii_dump_ppm(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                     (int)di.width, (int)di.height, name);
    }
    if (!di.disabled)
        wii_present(gpu_get_vram(), (int)di.display_x, (int)di.display_y,
                    (int)di.width, (int)di.height);
}

void wii_run_game(void)
{
    wii_log("Vib-Ribbon (recompiled) starting\n");

    INPUT_Init();

    if (!psx_bios_bundled() || !psx_bios_activate(psx_bios_bundled()))
        wii_fatal("no bundled BIOS backend");

    s_cpu.read_word  = psx_read_word;
    s_cpu.write_word = psx_write_word;
    s_cpu.read_half  = psx_read_half;
    s_cpu.write_half = psx_write_half;
    s_cpu.read_byte  = psx_read_byte;
    s_cpu.write_byte = psx_write_byte;

    memory_init(BIOS_PATH);
    gpu_init();
    dma_init();
    mdec_init();
    timers_init();
    interrupts_init();
    sio_init();
    spu_init();
    cdrom_init(DISC_CUE);

    sio_set_pad_connected(0, 1);
    gpu_set_vblank_callback(on_vblank);

    psx_icache_reset();
    s_cpu.pc = 0xBFC00000u;
    s_cpu.cop0[12] = 0x00400000u;       /* SR: BEV=1 */
    memory_set_sr_ptr(&s_cpu.cop0[12]);
    psx_irq_set_cause_ptr(&s_cpu.cop0[13]);

#ifdef VIB_EXIT_SECONDS
    { lwp_t t; LWP_CreateThread(&t, watchdog, NULL, NULL, 8192, 100); }
#endif
    wii_log("executing from PC=0x%08X\n", s_cpu.pc);
    psx_scheduler_run(&s_cpu);

    wii_fatal("guest exited");
}
