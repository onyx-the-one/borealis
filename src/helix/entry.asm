; entry.asm — 32-bit kernel entry point, linked at 0x10000
BITS 32

GLOBAL _start
GLOBAL bios_disk_thunk
GLOBAL bios_set_video_mode
GLOBAL bios_get_geometry
GLOBAL irq0_isr
GLOBAL save_kernel_idtr
EXTERN kernel_main
EXTERN timer_tick_handler

%include "src/helix/logmem.inc"

THUNK_BASE  equ 0x7100
THUNK_REQ   equ 0x7000
GDTR_SAVE   equ 0x6FE0
IDTR_SAVE   equ 0x6FD8
ESP_SAVE    equ 0x6FEC
THUNK_RET   equ 0x6FF0
DRIVE_STASH equ 0x0600

; ── DEBUG: checkpoint output, usable before helix.c's terminal is up.
DBG_VGA_ROW  equ 24
DBG_VGA_BASE equ (0xB8000 + DBG_VGA_ROW*80*2)
DBG_ATTR     equ 0x1F ; white on blue, stands out from kernel's own colours

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

    ; init the debug-log control block BEFORE the first checkpoint
    mov word [LOG_WPTR], 0
    mov byte [LOG_FLAGS], LOGF_ECHO

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

    mov al, 'Z'
    call dbg32_putc

    ; copy thunk16 blob to 0x7100
    mov esi, thunk16_blob
    mov edi, THUNK_BASE
    mov ecx, thunk16_blob_end - thunk16_blob
    rep movsb

    mov al, 'T'
    call dbg32_putc

    sgdt [GDTR_SAVE]

    mov al, 'G'
    call dbg32_putc

    movzx eax, byte [DRIVE_STASH]
    push eax

    mov al, ']'
    call dbg32_putc

    call kernel_main
    add esp, 4

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

save_kernel_idtr:
    sidt [IDTR_SAVE]
    ret

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
    xor edx, edx
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

; uint8_t bios_disk_thunk(...)
bios_disk_thunk:
    cli
    pushad
    mov [ESP_SAVE], esp

    ; Remap PIC to BIOS real-mode vectors 0x08..0x0F
    mov al, 0x11
    out 0x20, al
    out 0x80, al
    out 0xA0, al
    out 0x80, al
    mov al, 0x08
    out 0x21, al
    out 0x80, al
    mov al, 0x70
    out 0xA1, al
    out 0x80, al
    mov al, 0x04
    out 0x21, al
    out 0x80, al
    mov al, 0x02
    out 0xA1, al
    out 0x80, al
    mov al, 0x01
    out 0x21, al
    out 0x80, al
    out 0xA1, al
    out 0x80, al
    ; Mask all IRQs on PIC1 except IRQ6 (floppy disk)
    mov al, 0xBF
    out 0x21, al
    mov al, 0xFF
    out 0xA1, al

    mov eax, esp
    add eax, 36

    mov bl, [eax+0]
    mov [THUNK_REQ + 0x00], bl
    mov bl, [eax+4]
    mov [THUNK_REQ + 0x01], bl
    mov bl, [eax+8]
    mov [THUNK_REQ + 0x02], bl
    mov bl, [eax+12]
    mov [THUNK_REQ + 0x03], bl
    mov bx, [eax+16]
    mov [THUNK_REQ + 0x04], bx
    mov ebx, [eax+20]
    mov [THUNK_REQ + 0x06], ebx
    mov bl, [eax+24]
    mov [THUNK_REQ + 0x0B], bl
    mov ebx, [eax+28]
    mov [THUNK_REQ + 0x0C], ebx

    push eax
    mov al, '<'
    call dbg32_putc
    pop eax

    mov dword [THUNK_RET], thunk_returned
    jmp 0x18:THUNK_BASE

thunk_returned:
    movzx eax, byte [THUNK_REQ + 0x0A]

    push eax
    mov al, '>'
    call dbg32_putc
    pop eax
    push eax
    cmp al, 0
    jne .dbg_disk_fail
    mov al, '0'
    jmp .dbg_disk_emit
.dbg_disk_fail:
    mov al, 'F'
.dbg_disk_emit:
    call dbg32_putc
    pop eax

    mov [esp+28], eax

    ; Remap PIC back to Protected Mode vectors 0x20..0x2F
    mov al, 0x11
    out 0x20, al
    out 0x80, al
    out 0xA0, al
    out 0x80, al
    mov al, 0x20
    out 0x21, al
    out 0x80, al
    mov al, 0x28
    out 0xA1, al
    out 0x80, al
    mov al, 0x04
    out 0x21, al
    out 0x80, al
    mov al, 0x02
    out 0xA1, al
    out 0x80, al
    mov al, 0x01
    out 0x21, al
    out 0x80, al
    out 0xA1, al
    out 0x80, al
    ; Enable IRQ0 (timer), mask all other IRQs
    mov al, 0xFE
    out 0x21, al
    mov al, 0xFF
    out 0xA1, al

    lidt [IDTR_SAVE]
    popad
    sti
    ret

; int bios_set_video_mode(...)
bios_set_video_mode:
    cli
    pushad
    mov [ESP_SAVE], esp

    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    mov eax, esp
    add eax, 36

    mov bl, [eax+0]
    mov [THUNK_REQ + 0x00], bl
    mov byte [THUNK_REQ + 0x0B], 6

    mov al, 'V'
    call dbg32_putc

    mov dword [THUNK_RET], video_thunk_returned
    jmp 0x18:THUNK_BASE

video_thunk_returned:
    movzx eax, byte [THUNK_REQ + 0x0A]
    test eax, eax
    jz .video_ok
    mov eax, -1
    push eax
    mov al, 'f'
    call dbg32_putc
    pop eax
    jmp .video_done
.video_ok:
    xor eax, eax
    push eax
    mov al, 'v'
    call dbg32_putc
    pop eax
.video_done:
    mov [esp+28], eax

    lidt [IDTR_SAVE]
    mov al, 0xFE
    out 0x21, al
    mov al, 0xFF
    out 0xA1, al
    popad
    sti
    ret

; uint8_t bios_get_geometry(...)
bios_get_geometry:
    cli
    pushad
    mov [ESP_SAVE], esp

    mov al, 0xFF
    out 0x21, al
    out 0xA1, al

    mov eax, esp
    add eax, 36

    mov bl, [eax+0]
    mov [THUNK_REQ + 0x00], bl
    mov byte [THUNK_REQ + 0x0B], 7

    mov al, 'g'
    call dbg32_putc

    mov dword [THUNK_RET], geometry_thunk_returned
    jmp 0x18:THUNK_BASE

geometry_thunk_returned:
    movzx eax, byte [THUNK_REQ + 0x0A]
    test eax, eax
    jnz .geom_fail_dbg

    mov ecx, [esp+40]
    mov edx, [esp+44]
    mov bl, [THUNK_REQ + 0x08]
    mov [ecx], bl
    mov bl, [THUNK_REQ + 0x09]
    mov [edx], bl

    push eax
    mov al, 'G'
    call dbg32_putc
    pop eax
    jmp .geom_done

.geom_fail_dbg:
    push eax
    mov al, 'x'
    call dbg32_putc
    pop eax

.geom_done:
    mov [esp+28], eax

    lidt [IDTR_SAVE]
    mov al, 0xFE
    out 0x21, al
    mov al, 0xFF
    out 0xA1, al
    popad
    sti
    ret

; ── IRQ0 Timer Interrupt Service Routine ────────────────────────────────────
irq0_isr:
    pushad
    cld

    call timer_tick_handler

    mov al, 0x20
    out 0x20, al

    popad
    iretd

SECTION .bss
dbg_col: resb 1

SECTION .data
thunk16_blob:
    incbin "src/helix/fs/thunk16.bin"
thunk16_blob_end:
