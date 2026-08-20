; entry.asm — 32-bit kernel entry point, linked at 0x10000
BITS 32

GLOBAL _start
GLOBAL bios_disk_thunk
GLOBAL bios_set_video_mode
GLOBAL bios_get_geometry
EXTERN kernel_main

%include "src/helix/logmem.inc"

THUNK_BASE equ 0x7100
THUNK_REQ equ 0x7000
GDTR_SAVE equ 0x6FE0
ESP_SAVE equ 0x6FEC
THUNK_RET equ 0x6FF0
DRIVE_STASH equ 0x0600

; ── DEBUG: checkpoint output, usable before helix.c's terminal is up.
; Every character is appended to the low-memory debug-log block (see
; logmem.inc); while LOGF_ECHO is set it is ALSO written to the bottom
; VGA row (row 24), which is what you see during early boot. helix
; clears the echo flag only after the log is safely written to LOG.TXT,
; so a boot that dies before the flush keeps showing everything live --
; the fallback is the default, not a code path. The echo column is
; tracked in dbg_col (bss) and is NEVER reset, so checkpoints appear
; left-to-right in order.
DBG_VGA_ROW equ 24
DBG_VGA_BASE equ (0xB8000 + DBG_VGA_ROW*80*2)
DBG_ATTR equ 0x1F ; white on blue, stands out from kernel's own colours

; _start lives in its own section, which linker.ld places before all
; other .text. stage2 jumps to 0x10000 = the FIRST BYTES of the flat
; binary, so _start must physically come first. Nothing else may ever
; go into .text.start: any code that lands before _start gets executed
; as if it were the entry point, and a subroutine's trailing ret with
; no return address on the stack is a triple fault (confirmed 2026-08-07).
SECTION .text.start progbits alloc exec nowrite align=16

_start:
mov ax, 0x10
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
mov ss, ax
mov esp, 0x9F000
cld

; init the debug-log control block BEFORE the first checkpoint:
; empty buffer, echo on. Must happen before anything logs.
mov word [LOG_WPTR], 0
mov byte [LOG_FLAGS], LOGF_ECHO

; DEBUG: earliest possible checkpoint -- if you never see this
; character, the problem is before entry.asm even runs correctly
; (bootloader handoff / GDT / protected-mode switch in stage2.asm).
mov al, '['
call dbg32_putc
mov al, 'E'
call dbg32_putc

extern _bss_start
extern _bss_end
mov edi, _bss_start
mov ecx, _bss_end
sub ecx, edi
xor eax, eax
rep stosb

; DEBUG: bss cleared successfully
mov al, 'Z'
call dbg32_putc

; copy thunk16 blob to 0x7100
mov esi, thunk16_blob
mov edi, THUNK_BASE
mov ecx, thunk16_blob_end - thunk16_blob
rep movsb

; DEBUG: real-mode BIOS thunk blob copied into low memory
mov al, 'T'
call dbg32_putc

sgdt [GDTR_SAVE]

; DEBUG: GDT register saved (needed to return from real mode later)
mov al, 'G'
call dbg32_putc

movzx eax, byte [DRIVE_STASH]
push eax

; DEBUG: about to hand off to kernel_main() -- if you see E Z T G
; but nothing from helix.c ever appears, the crash/hang is inside
; kernel_main() itself or something it calls very early.
mov al, ']'
call dbg32_putc

call kernel_main
add esp, 4

; DEBUG: kernel_main() returned. It NEVER should -- BOREALIS either
; runs basic_run() forever or kpanics. Reaching here means both of
; those returned too, which is a serious bug. Make it loud.
mov al, '['
call dbg32_putc
mov al, '!'
call dbg32_putc
mov al, 'R'
call dbg32_putc
mov al, 'E'
call dbg32_putc
mov al, 'T'
call dbg32_putc
mov al, ']'
call dbg32_putc

cli
.hang:
hlt
jmp .hang

SECTION .text

; void dbg32_putc(al = char)
; Appends to the debug-log block; also writes VGA row 24 while echo is on.
; Clobbers: none preserved beyond normal registers used internally
dbg32_putc:
push eax
push ebx
push edx
movzx edx, word [LOG_WPTR]
cmp edx, LOG_CAP
jae .logged
mov [LOG_BUF + edx], al
inc edx
mov [LOG_WPTR], dx
.logged:
test byte [LOG_FLAGS], LOGF_ECHO
jz .done
movzx edx, byte [dbg_col]
cmp edx, 80
jb .dbg_ok
xor edx, edx ; wrap column rather than run off-screen
.dbg_ok:
mov ebx, edx
shl ebx, 1
add ebx, DBG_VGA_BASE
mov ah, DBG_ATTR
mov [ebx], ax
inc edx
mov [dbg_col], dl
.done:
pop edx
pop ebx
pop eax
ret

; void dbg32_puts(esi = zero-terminated string)
dbg32_puts:
push eax
push esi
.dbg_puts_loop:
mov al, [esi]
test al, al
jz .dbg_puts_done
call dbg32_putc
inc esi
jmp .dbg_puts_loop
.dbg_puts_done:
pop esi
pop eax
ret

; uint8_t bios_disk_thunk(drive, head, sector, cyl, count, buf_phys, use_edd, lba_lo)
; Calling convention: cdecl — args on stack above saved regs (pushad = 32 bytes)
; DEBUG: logs '<' before dropping to real mode and '>' + result digit
; after returning, so a hang inside the BIOS call is visibly
; distinguishable from a hang before/after it.
bios_disk_thunk:
pushad
mov [ESP_SAVE], esp

mov eax, esp
add eax, 36 ; skip pushad(32) + return addr(4)

mov bl, [eax+0]
mov [THUNK_REQ + 0x00], bl ; drive
mov bl, [eax+4]
mov [THUNK_REQ + 0x01], bl ; head
mov bl, [eax+8]
mov [THUNK_REQ + 0x02], bl ; sector
mov bl, [eax+12]
mov [THUNK_REQ + 0x03], bl ; cyl
mov bx, [eax+16]
mov [THUNK_REQ + 0x04], bx ; count
mov ebx, [eax+20]
mov [THUNK_REQ + 0x06], ebx ; buf_phys
mov bl, [eax+24]
mov [THUNK_REQ + 0x0B], bl ; use_edd
mov ebx, [eax+28]
mov [THUNK_REQ + 0x0C], ebx ; lba_lo

push eax
mov al, '<'
call dbg32_putc ; DEBUG: entering real-mode disk thunk
pop eax

mov dword [THUNK_RET], thunk_returned
jmp 0x18:THUNK_BASE

thunk_returned:
movzx eax, byte [THUNK_REQ + 0x0A]

push eax
mov al, '>'
call dbg32_putc ; DEBUG: returned from real-mode thunk
pop eax
push eax
cmp al, 0
jne .dbg_disk_fail
mov al, '0'
jmp .dbg_disk_emit
.dbg_disk_fail:
mov al, 'F' ; DEBUG: BIOS reported an error (non-zero status)
.dbg_disk_emit:
call dbg32_putc
pop eax

mov [esp+28], eax ; poke return value into pushad's eax slot

popad
ret

; int bios_set_video_mode(int mode)
; Calling convention: cdecl — single arg on stack above saved regs (pushad = 32 bytes)
; Returns 0 on success, -1 on failure.
bios_set_video_mode:
pushad
mov [ESP_SAVE], esp

mov eax, esp
add eax, 36 ; skip pushad(32) + return addr(4)

mov bl, [eax+0]
mov [THUNK_REQ + 0x00], bl ; video mode
mov byte [THUNK_REQ + 0x0B], 6 ; opcode 6 = set video mode

mov al, 'V'
call dbg32_putc ; DEBUG: entering set-video-mode thunk

mov dword [THUNK_RET], video_thunk_returned
jmp 0x18:THUNK_BASE

video_thunk_returned:
movzx eax, byte [THUNK_REQ + 0x0A]
test eax, eax
jz .video_ok
mov eax, -1
push eax
mov al, 'f'
call dbg32_putc ; DEBUG: set-video-mode failed
pop eax
jmp .video_done
.video_ok:
xor eax, eax
push eax
mov al, 'v'
call dbg32_putc ; DEBUG: set-video-mode ok
pop eax
.video_done:
mov [esp+28], eax ; poke return value into pushad's eax slot

popad
ret

; uint8_t bios_get_geometry(uint8_t drive, uint8_t *spt_out, uint8_t *heads_out)
; Calling convention: cdecl — args on stack above saved regs (pushad = 32 bytes)
; Returns 0 on success (spt_out/heads_out filled in), non-zero on failure.
; Used as a fallback when EDD (int 13h ah=41h/42h) hangs on real BIOSes —
; queries plain legacy CHS geometry via int 13h ah=08h instead.
bios_get_geometry:
pushad
mov [ESP_SAVE], esp

mov eax, esp
add eax, 36 ; skip pushad(32) + return addr(4)

mov bl, [eax+0]
mov [THUNK_REQ + 0x00], bl ; drive
mov byte [THUNK_REQ + 0x0B], 7 ; opcode 7 = get geometry

mov al, 'g'
call dbg32_putc ; DEBUG: entering geometry-query thunk

mov dword [THUNK_RET], geometry_thunk_returned
jmp 0x18:THUNK_BASE

geometry_thunk_returned:
movzx eax, byte [THUNK_REQ + 0x0A]
test eax, eax
jnz .geom_fail_dbg ; failure: leave spt_out/heads_out untouched

mov ecx, [esp+40] ; spt_out ptr (arg1, above pushad+retaddr)
mov edx, [esp+44] ; heads_out ptr (arg2)
mov bl, [THUNK_REQ + 0x08]
mov [ecx], bl
mov bl, [THUNK_REQ + 0x09]
mov [edx], bl

push eax
mov al, 'G'
call dbg32_putc ; DEBUG: geometry query ok
pop eax
jmp .geom_done

.geom_fail_dbg:
push eax
mov al, 'x'
call dbg32_putc ; DEBUG: geometry query failed (BIOS error)
pop eax

.geom_done:
mov [esp+28], eax ; poke return value into pushad's eax slot

popad
ret

SECTION .bss
dbg_col: resb 1

SECTION .data
thunk16_blob:
incbin "src/helix/fs/thunk16.bin"
thunk16_blob_end:
