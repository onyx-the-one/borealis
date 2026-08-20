; coil/stage2.asm — second-stage loader (0x7E00), EDD-first build
;
; The laptop's BIOS reports CHS geometry 18/1 via AH=08h but does not
; honor it (or any of 12 candidate geometries) for reads. the full
; kernel checksum fails for every clean (spt,heads) pair. CHS on this
; machine is not expressible, so this build bypasses it: EDD/LBA reads
; (int 13h ah=42h) take a flat 64-bit sector number with no geometry.
;
; Order of attempts, each with a distinct marker:
;   e  = entering EDD probe (ah=41h)
;   V  = EDD first sector read + checksum verified (EDD is real & works)
;   E  = full kernel loaded via EDD and image checksum passed
;   x  = EDD unavailable or failed -> fall back to CHS brute-force
;   g(s.h)K = CHS candidate verified and used
;   B(s.h)G! = no CHS geometry worked; BIOS claim printed for the record
;
; If the machine resets immediately after printing "ek", that confirms
; ah=42h is fatal on this BIOS (the old handoff4 observation) and the
; fallback is an empirical CHS sector-scan instead.

BITS 16
ORG 0x7E00

%include "coil/bootmeta.inc"

SPT         equ 18
NHEADS      equ 2
KBUF_SEG    equ 0x1000
SCRATCH_SEG equ 0x0080
DRIVE_STASH equ 0x0600
GEOM_STASH  equ 0x0601

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [drive], dl

    mov al, 'S'
    call putc
    mov al, '2'
    call putc

    call a20_enable
    mov al, 'A'
    call putc

    ; try EDD/LBA first
    mov al, 'e'
    call putc
    call edd_probe
    jc .chs
    call edd_verify_first
    jc .chs
    mov al, 'V'
    call putc
    call load_kernel_edd
    jc .chs
    call verify_kernel
    jc .chs
    mov al, 'E'
    call putc
    mov byte [geom_spt], 0        ; no CHS geometry in EDD mode
    mov byte [geom_heads], 0
    jmp .loaded

    ; CHS fallback: brute-force geometry with full-image verify
.chs:
    mov al, 'x'
    call putc
    xor si, si
.probe:
    mov al, [cand_spt + si]
    test al, al
    jz .all_failed
    mov [geom_spt], al
    mov al, [cand_heads + si]
    mov [geom_heads], al
    mov dl, [drive]
    mov ah, 0x00
    int 0x13
    push si
    call load_kernel
    pop si
    jc .next
    push si
    call verify_kernel
    pop si
    jnc .chs_found
.next:
    inc si
    jmp .probe

.chs_found:
    mov al, 'g'
    call putc
    mov al, '('
    call putc
    movzx ax, byte [geom_spt]
    call put_dec8
    mov al, '.'
    call putc
    movzx ax, byte [geom_heads]
    call put_dec8
    mov al, ')'
    call putc
    mov al, 'K'
    call putc
    jmp .loaded

.all_failed:
    mov al, 'B'
    call putc
    call query_geometry_raw
    jc .no_bios
    mov al, '('
    call putc
    movzx ax, byte [geom_spt]
    call put_dec8
    mov al, '.'
    call putc
    movzx ax, byte [geom_heads]
    call put_dec8
    mov al, ')'
    call putc
.no_bios:
    mov al, 'G'
    call putc
    mov al, '!'
    call putc
    cli
.hang:
    hlt
    jmp .hang

.loaded:
    mov al, [drive]
    mov [DRIVE_STASH], al
    mov al, [geom_spt]
    mov [GEOM_STASH], al
    mov al, [geom_heads]
    mov [GEOM_STASH+1], al

    mov al, '['
    call putc
    mov al, 'J'
    call putc
    mov al, ']'
    call putc

    cli
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pmode32

BITS 32
pmode32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9F000
    jmp 0x10000

BITS 16

; EDD (int 13h ah=41h/42h, LBA)

; edd_probe: CF=0 if the drive supports extended disk access.
edd_probe:
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive]
    int 0x13
    jc .no
    cmp bx, 0xAA55
    jne .no
    test cx, 1
    jz .no
    clc
    ret
.no:
    stc
    ret

; edd_read_one: read one sector at LBA [lba32] into buf_seg:0. CF=0 ok.
edd_read_one:
    mov word [dap], 16
    mov byte [dap+1], 0
    mov word [dap+2], 1
    mov word [dap+4], 0
    mov ax, [buf_seg]
    mov [dap+6], ax
    mov eax, [lba32]
    mov [dap+8], eax
    mov dword [dap+12], 0
    mov si, dap
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    ret                             ; CF carries the result

; edd_verify_first: read KERNEL_LBA via EDD into scratch, checksum it.
; CF=0 on match (proves EDD reads the right bytes before we commit).
edd_verify_first:
    mov word [buf_seg], SCRATCH_SEG
    mov dword [lba32], KERNEL_LBA
    call edd_read_one
    jc .bad
    mov bx, SCRATCH_SEG
    mov es, bx
    mov cx, 256
    call sum_words
    cmp ax, KERNEL_SEC0_SUM
    jne .bad
    clc
    ret
.bad:
    stc
    ret

; load_kernel_edd: KERNEL_SECS sectors from KERNEL_LBA to 0x1000:0000,
; one sector per ah=42h call (512-aligned, never crosses a 64K boundary).
; CF=0 on success.
load_kernel_edd:
    mov word [buf_seg], KBUF_SEG
    mov dword [lba32], KERNEL_LBA
    mov word [secs_rem], KERNEL_SECS
.next:
    call edd_read_one
    jc .fail
    add word [buf_seg], 32
    inc dword [lba32]
    dec word [secs_rem]
    jnz .next
    clc
    ret
.fail:
    stc
    ret

; CHS path (fallback)

; load_kernel: KERNEL_SECS sectors from KERNEL_LBA to 0x1000:0000 via
; ah=02h CHS, one sector per call, using geom_spt/geom_heads. CF=0 ok.
load_kernel:
    mov word [buf_seg], KBUF_SEG
    mov word [lba_cur], KERNEL_LBA
    mov word [secs_rem], KERNEL_SECS
.next:
    mov ax, [lba_cur]
    call lba_to_chs
    mov bx, [buf_seg]
    mov es, bx
    xor bx, bx
    mov ax, 0x0201
    mov dl, [drive]
    int 0x13
    jc .fail
    add word [buf_seg], 32
    inc word [lba_cur]
    dec word [secs_rem]
    jnz .next
    clc
    ret
.fail:
    stc
    ret

; query_geometry_raw: int 13h ah=08h, for reporting only on failure.
query_geometry_raw:
    mov dl, [drive]
    mov ah, 0x08
    xor cx, cx
    xor dx, dx
    push es
    push di
    xor di, di
    mov es, di
    int 0x13
    pop di
    pop es
    jc .bad
    mov al, cl
    and al, 0x3F
    jz .bad
    cmp dh, 0xFF
    je .bad
    mov ah, dh
    inc ah
    mov [geom_spt], al
    mov [geom_heads], ah
    clc
    ret
.bad:
    stc
    ret

; verify_kernel: per-sector word sum of the loaded image vs KERNEL_SUM.
; Quiet. CF=0 on match.
verify_kernel:
    mov word [buf_seg], KBUF_SEG
    mov word [secs_rem], KERNEL_SECS
    mov word [sum_acc], 0
.sector:
    mov bx, [buf_seg]
    mov es, bx
    mov cx, 256
    call sum_words
    add [sum_acc], ax
    add word [buf_seg], 32
    dec word [secs_rem]
    jnz .sector
    mov ax, [sum_acc]
    cmp ax, KERNEL_SUM
    jne .bad
    clc
    ret
.bad:
    stc
    ret

; lba_to_chs: AX = LBA (< 65536) -> CH/CL/DH using geom_spt/geom_heads.
lba_to_chs:
    xor dx, dx
    movzx bx, byte [geom_spt]
    div bx
    inc dx
    mov cl, dl
    xor dx, dx
    movzx bx, byte [geom_heads]
    div bx
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah
    ret

; sum_words: 16-bit word sum of CX words at ES:0 -> AX. Clobbers SI.
sum_words:
    push si
    xor si, si
    xor ax, ax
.add:
    add ax, [es:si]
    add si, 2
    loop .add
    pop si
    ret

put_dec8:
    push ax
    push bx
    push cx
    push dx
    xor cx, cx
    mov bx, 10
.divloop:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .divloop
.printloop:
    pop ax
    add al, '0'
    call putc
    loop .printloop
    pop dx
    pop cx
    pop bx
    pop ax
    ret

put_hex8:
    push ax
    push cx
    mov cl, al
    shr al, 4
    call .nib
    mov al, cl
    and al, 0x0F
    call .nib
    pop cx
    pop ax
    ret
.nib:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    push ax
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    pop ax
    ret

kbc_wait_in:
    in al, 0x64
    test al, 0x02
    jnz kbc_wait_in
    ret

kbc_wait_out:
    in al, 0x64
    test al, 0x01
    jz kbc_wait_out
    ret

a20_enable:
    call kbc_wait_in
    mov al, 0xAD
    out 0x64, al
    call kbc_wait_in
    mov al, 0xD0
    out 0x64, al
    call kbc_wait_out
    in al, 0x60
    push ax
    call kbc_wait_in
    mov al, 0xD1
    out 0x64, al
    call kbc_wait_in
    pop ax
    or al, 0x02
    out 0x60, al
    call kbc_wait_in
    mov al, 0xAE
    out 0x64, al
    call kbc_wait_in
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    ret

putc:
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    ret

; CHS candidate geometries for the fallback probe, zero-terminated.
cand_spt:   db 63, 63, 63, 63, 63, 32, 32, 36, 18, 18, 15, 9, 0
cand_heads: db 16,  8,  4,  2,  1,  2,  4,  2,  2,  1,  2, 2, 0

drive:      db 0x80
buf_seg:    dw KBUF_SEG
lba_cur:    dw KERNEL_LBA
lba32:      dd KERNEL_LBA
secs_rem:   dw KERNEL_SECS
sum_acc:    dw 0
geom_spt:   db SPT
geom_heads: db NHEADS
dap:        times 16 db 0

gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00009A000000FFFF
    dq 0x000092000000FFFF

gdtr:
    dw gdtr - gdt - 1
    dd gdt

times 2048 - ($ - $$) db 0
