/* devkitPPC's rvl.ld plus a MEM2 section. gcc -mrvl links with "-T rvl.ld"
 * and the Makefile's -B puts this copy first in the search path.
 *
 * Arrays tagged .bss.mem2 (scripts/patch_runtime.py) are cold: debug traces,
 * the BIOS ROM copy, savestate and upload staging, memory cards, the CD audio
 * ring. They live at the bottom of MEM2 so the generated code fits in MEM1;
 * libogc's MEM2 arena starts above them and wii_main.c zeroes them. */

PHDRS {
	stub PT_LOAD FLAGS(5);
}

SECTIONS {

	. = 0x00003400;

	.stub : {
		KEEP(*(.stub))
	} :stub = 0

	/* Listed before libogc_common.ld's .bss so its input pattern wins. */
	.mem2 0x90002000 (NOLOAD) : {
		__mem2_start = .;
		*(.bss.mem2)
		. = ALIGN(64);
		__mem2_end = .;
	} :NONE

	. = 0x80003f00;

}

__Arena2Lo = __mem2_end;
PROVIDE_HIDDEN( __Arena2Hi    = 0x933e0000 );
PROVIDE_HIDDEN( __ipcbufferLo = 0x933e0000 );
PROVIDE_HIDDEN( __ipcbufferHi = 0x93400000 );

INCLUDE libogc_common.ld
