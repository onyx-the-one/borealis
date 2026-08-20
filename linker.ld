OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)
SECTIONS {
. = 0x10000;
/* .text.start MUST stay first: stage2 jumps to 0x10000, the first bytes
   of the flat binary, and _start is the only code allowed in it. */
.text ALIGN(16) : { *(.text.start) *(.text) }
.rodata ALIGN(16) : { *(.rodata*) }
.data ALIGN(16) : { *(.data) }
/* BSS is placed at 1 MiB. The loaded image (text+rodata+data, ~76 KiB)
   stays below 640 KiB where coil puts it, but BSS (~650 KiB and growing)
   would otherwise run past 0xA0000 into the VGA aperture, where reads
   return 0xFF and writes vanish -- that is what made gfx_is_active
   unreadable and silently killed SCREEN (the "graphics thunk situation"
   was never the thunk). BSS is NOBITS so the flat binary is unchanged.
   A20 is enabled by coil before entry; all targets have >= 4 MiB RAM.
   This also keeps BSS clear of the 0x9F000 stack top. */
. = 0x100000;
_bss_start = .;
.bss ALIGN(16) : { *(COMMON); *(.bss); *(.lowbss); }
_bss_end = .;
}
