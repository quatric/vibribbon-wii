/* Vib-Ribbon Wii host: brings up video, SD and pads, then hands control to the
 * PSXRecomp runtime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <wiiuse/wpad.h>

#include "wii_host.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    {   /* the MEM2 section (mem2.ld) is not part of the DOL's zeroed bss */
        extern char __mem2_start[], __mem2_end[];
        memset(__mem2_start, 0, (size_t)(__mem2_end - __mem2_start));
        DCFlushRange(__mem2_start, (u32)(__mem2_end - __mem2_start));
    }
    SYS_STDIO_Report(true);
    VIDEO_Init();
    wii_video_init();
    /* Only the front SD slot, and before USB comes up: fatInitDefault would
     * also try to mount USB storage, which can be the CD drive usbcd owns. */
    int sd_ok = 0;
    for (int i = 0; i < 10 && !sd_ok; i++) {
        sd_ok = fatMountSimple("sd", &__io_wiisd);
        if (!sd_ok) { __io_wiisd.shutdown(); usleep(200 * 1000); }
    }
    if (!sd_ok)
        wii_fatal("SD card not found");
    wii_usbcd_init();   /* takes its USB buffers from the top of MEM2 */
    wii_stage(1, "video + fat up");
    {   /* baseline: how fast does this machine (or emulator) run plain code? */
        uint64_t t0 = gettime();
        volatile uint32_t acc = 1;
        for (uint32_t i = 0; i < 50000000u; i++) acc = acc * 3u + i;
        wii_log("bench: 50M loop iterations in %u ms\n", (unsigned)ticks_to_millisecs(gettime() - t0));
    }
    WPAD_Init();
    wii_stage(2, "wpad up");
    wii_run_game();
    return 0;
}
