/* Vib-Ribbon Wii host: brings up video, SD and pads, then hands control to the
 * PSXRecomp runtime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
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
    WPAD_Init();
    wii_run_game();
    return 0;
}
