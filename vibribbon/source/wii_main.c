/* Vib-Ribbon Wii host: brings up video, SD and pads, then hands control to the
 * PSXRecomp runtime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <wiiuse/wpad.h>

#include "wii_host.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    SYS_STDIO_Report(true);
    VIDEO_Init();
    wii_video_init();
    if (!fatInitDefault())
        wii_fatal("SD card not found");
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
