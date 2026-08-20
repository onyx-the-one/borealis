# BOREALIS Makefile

CC := i686-elf-gcc
CFLAGS := -m32 -ffreestanding -fno-pic -std=gnu99 \
-Wall -Wextra -Wno-unused-parameter \
-O2 -mfpmath=387 -fno-stack-protector \
-I src/helix -I src/helix/fs -I src/helix/sound \
-I src/helix/rtc -I src/helix/gfx -I src/helix/usb
NASM := nasm
PYTHON := python3

CSRCS := src/helix/helix.c \
src/helix/basic.c \
src/helix/config.c \
src/helix/log.c \
src/helix/fs/fat12.c \
src/helix/sound/sound.c \
src/helix/rtc/rtc.c \
src/helix/gfx/gfx.c \
src/helix/usb/pci.c \
src/helix/usb/ehci.c \
src/helix/usb/usb.c \
src/helix/usb/usb_msd.c
COBJS := $(CSRCS:.c=.o)
ASMS := src/helix/entry.asm
AOBJS := src/helix/entry.o

all: borealis.img

# ── Assemble 16-bit blobs ──────────────────────────────────────────────────
coil/stage1.bin: coil/stage1.asm
	$(NASM) -f bin -o $@ $<

coil/bootmeta.inc: helix.bin
	$(PYTHON) mkfat.py --bootmeta

coil/stage2.bin: coil/stage2.asm coil/bootmeta.inc
	$(NASM) -f bin -o $@ $<

src/helix/fs/thunk16.bin: src/helix/fs/thunk16.asm src/helix/logmem.inc
	$(NASM) -f bin -o $@ $<

# ── Compile helix ─────────────────────────────────────────────────────────
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/helix/entry.o: src/helix/entry.asm src/helix/fs/thunk16.bin src/helix/logmem.inc
	$(NASM) -f elf32 -o $@ $<

helix.bin: $(AOBJS) $(COBJS) linker.ld
	$(CC) -m32 -ffreestanding -nostdlib -T linker.ld \
	-o helix.elf $(AOBJS) $(COBJS) -lgcc
	objcopy -O binary helix.elf $@

# ── Disk image ──────────────────────────────────────────────────────────────
borealis.img: coil/stage1.bin coil/stage2.bin helix.bin
	$(PYTHON) mkfat.py

# ── Run in QEMU ─────────────────────────────────────────────────────────────
run: borealis.img
	qemu-system-i386 -drive file=borealis.img,format=raw,if=floppy \
	-boot a -m 4 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

run-vga: borealis.img
	qemu-system-i386 -drive file=borealis.img,format=raw,if=floppy \
	-boot a -m 4 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

# USB development rig: boot from floppy (known-good path), stick image on an
# emulated EHCI controller. With drop 3 the filesystem switches to the stick.
stick.img: borealis.img
	cp borealis.img stick.img

run-usb: borealis.img stick.img
	qemu-system-i386 -drive file=borealis.img,format=raw,if=floppy \
	-boot a -m 4 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 \
	-device usb-ehci,id=ehci \
	-drive if=none,id=stick,format=raw,file=stick.img \
	-device usb-storage,bus=ehci.0,drive=stick

clean:
	rm -f $(COBJS) $(AOBJS) \
	coil/stage1.bin coil/stage2.bin coil/bootmeta.inc \
	src/helix/fs/thunk16.bin \
	helix.elf helix.bin borealis.img stick.img

pack:
	mcopy -i borealis.img SPLASH.BAS ::SPLASH.BAS
	mcopy -i borealis.img demos/primes.bas ::PRIMES.BAS
	mcopy -i borealis.img demos/scrtest.bas ::SCRTEST.BAS
	mcopy -i borealis.img demos/SNAKE.BAS ::SNAKE.BAS
	mcopy -i borealis.img demos/ode_to_joy.bas ::ODETOJOY.BAS
	mcopy -i borealis.img demos/primes.txt ::PRIMES.TXT
	mcopy -i borealis.img CONFIG.TXT ::CONFIG.TXT

box:
	cd .. && tar -czvf BOREALIS-vers.tar.gz BOREALIS/*

.PHONY: all run run-vga run-usb clean
