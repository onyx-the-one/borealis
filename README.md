# Borealis

Version 1.8.64-pre43

Borealis is an operating system built from scratch: its own bootloader,
kernel, and language environment, with nothing underneath. It boots off
a FAT12 floppy image (real or virtual), goes 32-bit, and drops you into
a line-numbered BASIC REPL with file I/O, arrays, sound, and graphics.
Closer to a Commodore-era machine than a modern OS.

Three layers, each with its own name:

- coil - the bootloader. Gets the machine from BIOS handoff to a loaded kernel.
- helix - the kernel. Hardware bring-up, drivers, filesystem, the BIOS thunk.
- BOREALIS - the whole thing, what you see.

Was called BTBX (Bare Tiny(?) BASIC eXecutor) until it stopped being tiny.

## Building

Needs nasm, i686-elf-gcc, python3, qemu.

    make
    make run

`make` assembles coil, compiles helix + the interpreter, links at
0x10000, and mkfat.py stamps out borealis.img (1.44 MB FAT12, kernel in
the data area as a regular file starting at LBA 37). `make run` boots it
in QEMU. For real hardware: dd the image to a stick, boot legacy BIOS
(not UEFI). `make run-usb` is the USB development rig, see below.

## Boot process

1. stage1.asm (512 bytes) - the boot sector. BIOS puts it at 0x7C00. It
   loads stage2 via CHS and jumps.
2. stage2.asm (2 KB) - A20, GDT, picks a working disk geometry by
   *verifying* (reads the first kernel sector and checksums it against a
   build-time constant; EDD first, then a brute-force CHS candidate
   list), loads the kernel one sector per int 13h call, checksums the
   whole image before jumping. A bad read can never present as a
   mysterious reset anymore.
3. entry.asm - 32-bit entry. Copies the thunk16 real-mode trampoline to
   0x7100, sets up the C stack, calls kernel_main.
4. helix.c - VGA terminal, keyboard, banner, then the REPL.

The thunk (thunk16.asm at 0x7100) is how 32-bit helix still issues int
13h disk calls and int 10h video mode switches: drop to real mode, do
the call, come back. Both directions are instrumented with checkpoint
letters, which is how a hang names its own location.

## The dispatcher

basic.c's statement parser is assembled from .inc fragments via
#include, in order core, console, flow, file, system, graphics. Include
order is semantic - an earlier fragment can shadow a keyword a later one
also wants (this actually bit us with PRINT#/INPUT#). Only basic.c owns
the catch-all berr("WHAT?"); no .inc may have its own. If you chain kw()
calls you must call sw() between them yourself - kw() doesn't skip
whitespace after a match. These are learned-the-hard-way rules.

## Statements

| Statement | Syntax | Notes |
|---|---|---|
| PRINT | `PRINT expr, expr, ...` | trailing `;` suppresses newline |
| PRINT# / INPUT# | `PRINT #n, expr` / `INPUT #n, var` | file channel n |
| INPUT | `INPUT "prompt", var` | |
| LET | `LET var = expr` or bare `var = expr` | two separate parse paths, keep them that way |
| DIM | `DIM name(d1[,d2[,d3]])` | up to 3 dims, string arrays via `name$` |
| REM | `REM ...` | |
| IF | inline `IF e THEN stmt`, block `IF e THEN ... ELSE ... ENDIF` | inline false does nothing; block needs ENDIF |
| GOTO / GOSUB / RETURN | line numbers | |
| ON | `ON expr GOTO n1,n2,...` (or GOSUB) | |
| FOR / NEXT | `FOR v = a TO b [STEP s]` | NEXT searches the FOR stack by name, so GOTO out of a nested loop is safe |
| WHILE / WEND | | |
| DATA / READ / RESTORE | | |
| POKE / PEEK | `POKE addr, value` | raw 32-bit memory |
| OPEN / CLOSE | `OPEN "file" FOR INPUT|OUTPUT AS n` | |
| SAVE / LOAD / DIR | the current program, and the root dir | |
| NEW / RUN / LIST / END / HALT / CLEAR | | |
| BEEP | `BEEP freq, ms` | PIT ch2 square wave |
| SAY | `SAY s$` | SAM formant synth, 1-bit PWM. WIP, sounds wrong |
| SCREEN / PSET / LINE / RECT / CIRCLE / PALETTE / GCLS / POINT | mode 13h | work; SCREEN goes through thunk opcode 6 |
| BLOAD / SYS | `BLOAD "file.bin", addr` / `SYS addr` | flat binaries, see below |
| HELP | | |

Functions: SIN COS TAN ATN EXP LOG SQR ABS INT FIX SGN RND CINT CDBL
(all x87), AND OR XOR NOT SHL SHR, HEX$ BIN$ and &H/&B literals,
LEFT$ RIGHT$ MID$ STR$ CHR$ VAL ASC LEN, INKEY$, DATE$, TIME$.

Data types: 32-bit int, x87 80-bit float, strings up to 256 chars
(trailing `$`). Arrays need DIM.

## Config and the boot log

CONFIG.TXT on the boot disk, key=value, one per line. Leading line
numbers are tolerated so `EDIT CONFIG.TXT` just works. Keys: SPLASH
(run SPLASH.BAS at boot, default 1) and LOG (boot debug log, default 1).

With LOG=1: everything from the first instruction on (asm checkpoints,
the thunk letters, all [DEBUG] lines) is captured into a buffer. Once
the filesystem proves it can write, the buffer becomes LOG.TXT and the
screen clears to the banner. If the fs write fails, echo was never
turned off, so the whole log is already on screen - the fallback isn't a
code path, it's the default. `klog()` appends post-boot. LOG.TXT is
overwritten each boot and readable without booting: `mcopy -i
borealis.img ::LOG.TXT .`

## Native code

Drop a flat binary on the disk, then:

    BLOAD "DEMO.BIN", 131072
    SYS 131072

Load ceiling is 0x7F000 (stay clear of VGA at 0xA0000). Rules: raw
binary (objcopy -O binary), entry point must be the first byte (control
section order in your linker script, compilers don't guarantee function
order), nobody zeroes your .bss for you. If your code returns instead of
halting you fall back into the REPL, which looks like BASIC printing a
prompt out of nowhere. 0x20000 is a sane scratch address.

## USB

There's a native USB mass-storage driver in progress. Motivation: on at
least one real BIOS (Core 2 Duo laptop), int 13h dies after the CPU has
been in protected mode - even a no-transfer disk reset never returns -
because legacy USB on that machine is SMM and the trap mishandles the
post-PM state. Coil still boots via BIOS (that path is pure real mode
and works), helix takes over natively.

Layers, each independently instrumented: PCI scan finds the EHCI
controller by class code, the BIOS/SMM ownership handoff (EECP
semaphore, then kill every legacy SMI source), EHCI init with an idle
async schedule, port scan/reset, EP0 control transfers, BOT/SCSI (TEST
UNIT READY, INQUIRY, READ CAPACITY, READ/WRITE(10)) surfacing as one-
sector read/write. When usb_probe() succeeds, fat12's disk backend
switches from the BIOS thunk to the stick and stops caring what a disk
is. Polled, no interrupts, no periodic schedule. Works in QEMU
(`make run-usb`); the laptop port-reset is the remaining real-hardware
bug.

## Memory map

| Range | Contents |
|---|---|
| 0x7C00 | coil stage1 (BIOS puts it here) |
| 0x7100 | thunk16 trampoline |
| 0x77F0 | debug-log control block (see logmem.inc) |
| 0x7800 | asm-side trace buffer |
| 0x10000 | helix entry (linker.ld) |
| 0x20000 | scratch for BLOAD/SYS |
| 0x7F000 | BLOAD ceiling |
| 0xA0000 | VGA framebuffer (mode 13h) |
| 0xB8000 | VGA text buffer |
| 0x100000 | helix BSS starts here |

## Errors

SYNTAX, MEM, DIV0, SUBSCRIPT, TYPE MISMATCH, MATH, RTC, FILE NOT FOUND,
DISK NOT READY, UNKNOWN FUNCTION. Light red on the console, halts the
current program.

## mtools

borealis.img is a plain FAT12 floppy filesystem, so mtools reads and
writes it without booting:

    mdir -i borealis.img
    mcopy -i borealis.img ::PRIMES.TXT ./primes_result.txt
    mcopy -i borealis.img PONG.BIN ::PONG.BIN

Raw filesystem, not partitioned - always `-i`, and use
`MTOOLS_SKIP_CHECK=1` if it complains about geometry.

## Known issues / WIP

- SAY is ported but doesn't produce correct audio.
- No timer IRQ; anything needing real elapsed time should use the PIT
  (timer.h reads channel 0's count, works in any CPU mode, needs no
  interrupts).
- No native floppy driver yet; BIOS int 13h is still the disk path on
  floppy boot. The USB driver above is the start of fixing that class.
- The .inc dispatcher chain is brittle by construction (see The
  dispatcher). Cleanup debt.

## Licence

MIT - see LICENSE.
