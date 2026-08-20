; thunk16.asm — real-mode BIOS trampoline (ORG 0x7100)
; Copied to low memory at boot by entry.asm.

; DEBUG: every opcode branch below logs checkpoint characters through
; dbg_putc, which appends to the low-memory debug-log block (see
; logmem.inc) and additionally echoes via BIOS teletype (int 10h ah=0Eh)
; while LOGF_ECHO is set. Echo defaults on from entry.asm and is cleared
; by helix only after the log is safely on disk, so a hang here is
; always visible on screen regardless of the LOG.TXT outcome.

; EDD path letters (the laptop discriminator):
; e = entered EDD read branch
; r = about to issue pre-read disk reset (ah=00h, NO data transfer)
; k/x after r = reset returned ok / failed
; d = DAP built, about to issue the extended read (ah=42h, transfer)
; k/x after d = read returned ok / failed
; So: hang after "r" => even a no-transfer reset hangs post-PM (int 13h
; is dead after the protected-mode excursion). Hang after "d" => reset
; works but the USB transfer hangs (legacy-USB SMM trampoline issue).

BITS 16
ORG 0x7100

%include "src/helix/logmem.inc"

THUNK_REQ equ 0x7000
GDTR_SAVE equ 0x6FE0
ESP_SAVE equ 0x6FEC
THUNK_RET equ 0x6FF0

thunk_entry:
mov eax, cr0
and al, 0xFE
mov cr0, eax
jmp 0x0000:.real

.real:
xor ax, ax
mov ds, ax
mov es, ax
mov ss, ax
mov sp, 0x6E00
sti ; CRITICAL: Enable interrupts so floppy IRQ6 works!

mov al, 'R'
call dbg_putc

cmp byte [THUNK_REQ + 0x0B], 0
je .chs_path
cmp byte [THUNK_REQ + 0x0B], 1
je .edd_path
cmp byte [THUNK_REQ + 0x0B], 2
je .chs_write
cmp byte [THUNK_REQ + 0x0B], 3
je .edd_write
cmp byte [THUNK_REQ + 0x0B], 4
je .edd_probe
cmp byte [THUNK_REQ + 0x0B], 5
je .reset
cmp byte [THUNK_REQ + 0x0B], 6
je .set_video
cmp byte [THUNK_REQ + 0x0B], 7
je .get_geometry

mov al, '?'
call dbg_putc
mov byte [THUNK_REQ + 0x0A], 0x01
jmp .return_pm

.reset:
mov al, 'r'
call dbg_putc
mov ah, 0x00
mov dl, [THUNK_REQ + 0x00]
int 0x13
call dbg_result
jmp .done

.set_video:
mov al, 'V'
call dbg_putc
mov al, [THUNK_REQ + 0x00]
mov ah, 0x00
int 0x10
mov al, 'k'
call dbg_putc
jmp .done_ok

.get_geometry:
mov al, 'g'
call dbg_putc
mov dl, [THUNK_REQ + 0x00]
mov ah, 0x08
xor cx, cx
xor dx, dx
int 0x13
call dbg_result
jc .geom_fail
mov al, cl
and al, 0x3F
mov [THUNK_REQ + 0x08], al
mov al, dh
inc al
mov [THUNK_REQ + 0x09], al
mov ah, 0x00
jmp .done_ok
.geom_fail:
mov ah, 0x01
jmp .done_err

.edd_probe:
mov al, 'p'
call dbg_putc
mov ah, 0x41
mov bx, 0x55AA
mov dl, [THUNK_REQ + 0x00]
int 0x13
call dbg_result
jc .probe_fail
cmp bx, 0xAA55
jne .probe_fail
test cx, 1
jz .probe_fail
mov ah, 0x00
jmp .done_ok
.probe_fail:
mov ah, 0x01
jmp .done_err

.chs_write:
mov al, 'W'
call dbg_putc
mov ebx, [THUNK_REQ + 0x06]
mov eax, ebx
shr eax, 4
and ebx, 0x0000000F
mov es, ax
mov bx, bx
mov ch, [THUNK_REQ + 0x03]
mov cl, [THUNK_REQ + 0x02]
mov dh, [THUNK_REQ + 0x01]
mov dl, [THUNK_REQ + 0x00]
mov al, [THUNK_REQ + 0x04]
mov ah, 0x03
int 0x13
call dbg_result
jmp .done

.chs_path:
mov al, 'C'
call dbg_putc
mov ebx, [THUNK_REQ + 0x06]
mov eax, ebx
shr eax, 4
and ebx, 0x0000000F
mov es, ax
mov bx, bx
mov ch, [THUNK_REQ + 0x03]
mov cl, [THUNK_REQ + 0x02]
mov dh, [THUNK_REQ + 0x01]
mov dl, [THUNK_REQ + 0x00]
mov al, [THUNK_REQ + 0x04]
mov ah, 0x02
int 0x13
call dbg_result
jmp .done

.edd_path:
mov al, 'e'
call dbg_putc
; --- pre-read disk reset (ah=00h, no transfer), lettered to isolate ---
mov al, 'r'
call dbg_putc
mov ah, 0x00
mov dl, [THUNK_REQ + 0x00]
int 0x13
call dbg_result
; --- build the DAP, then letter the actual extended read ---
mov word [0x6E10], 0x0010
mov ax, [THUNK_REQ + 0x04]
mov [0x6E12], ax
mov eax, [THUNK_REQ + 0x06]
mov ebx, eax
shr eax, 4
and ebx, 0x0000000F
mov [0x6E14], bx
mov [0x6E16], ax
mov eax, [THUNK_REQ + 0x0C]
mov [0x6E18], eax
mov dword [0x6E1C], 0
mov dl, [THUNK_REQ + 0x00]
mov si, 0x6E10
mov al, 'd'
call dbg_putc
mov ah, 0x42
int 0x13
call dbg_result
jmp .done

.edd_write:
mov al, 'E'
call dbg_putc
mov al, 'r'
call dbg_putc
mov ah, 0x00
mov dl, [THUNK_REQ + 0x00]
int 0x13
call dbg_result
mov word [0x6E10], 0x0010
mov ax, [THUNK_REQ + 0x04]
mov [0x6E12], ax
mov eax, [THUNK_REQ + 0x06]
mov ebx, eax
shr eax, 4
and ebx, 0x0000000F
mov [0x6E14], bx
mov [0x6E16], ax
mov eax, [THUNK_REQ + 0x0C]
mov [0x6E18], eax
mov dword [0x6E1C], 0
mov dl, [THUNK_REQ + 0x00]
mov si, 0x6E10
mov al, 'd'
call dbg_putc
mov ah, 0x43
int 0x13
call dbg_result
jmp .done

.done:
jc .done_err
.done_ok:
mov ah, 0x00
.done_err:
cmp ah, 0
jne .store_err
jc .store_cf
jmp .store_err
.store_cf:
mov ah, 0x01
.store_err:
mov [THUNK_REQ + 0x0A], ah

cmp byte [THUNK_REQ + 0x0A], 0
jne .dbg_final_fail
mov al, '0'
jmp .dbg_final_emit
.dbg_final_fail:
mov al, 'F'
.dbg_final_emit:
call dbg_putc

.return_pm:
cli ; CRITICAL: Disable interrupts before returning to PM!
lgdt [GDTR_SAVE]
mov eax, cr0
or al, 1
mov cr0, eax
jmp 0x08:.prot32

BITS 32
.prot32:
mov ax, 0x10
mov ds, ax
mov es, ax
mov ss, ax
mov fs, ax
mov gs, ax
mov esp, [ESP_SAVE]
jmp [THUNK_RET]

; ── DEBUG helpers ───────────────────────────────────────────────────
; Placed at the very end, AFTER .prot32, so NASM's dot-label scoping
; never re-parents .prot32 under one of these helper labels.
BITS 16

; dbg_putc: log al to the debug block; teletype too while echo is on.
dbg_putc:
push ax
push bx
push di
mov di, [LOG_WPTR]
cmp di, LOG_CAP
jae .no_buf
mov [LOG_BUF + di], al
inc di
mov [LOG_WPTR], di
.no_buf:
test byte [LOG_FLAGS], LOGF_ECHO
jz .done
mov ah, 0x0E
xor bh, bh
int 0x10
.done:
pop di
pop bx
pop ax
ret

dbg_result:
pushf
push ax
jc .dbg_result_fail
mov al, 'k'
jmp .dbg_result_emit
.dbg_result_fail:
mov al, 'x'
.dbg_result_emit:
call dbg_putc
pop ax
popf
ret

; the blob is copied to 0x7100 and the log control block lives at
; 0x77F0 — fail the build rather than overlap it
%if ($ - $$) > 0x6F0
%error "thunk16 blob overlaps debug-log control block at 0x77F0"
%endif
