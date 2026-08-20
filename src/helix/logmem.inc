; logmem.inc — shared low-memory debug-log control block.
; Included by entry.asm and thunk16.asm; log.c carries matching defines.
; The block sits at 0x77F0-0x7BFF: above the thunk blob (0x7100+, ~600 B),
; below the dead stage1/stage2 area, clear of the 0x6E00 thunk stack,
; the 0x6FE0-0x7100 thunk handoff, and the 0x0800 bounce buffer.
LOG_WPTR  equ 0x77F0  ; word: write offset into LOG_BUF (asm-side)
LOG_FLAGS equ 0x77F2  ; byte: bit 0 = echo to screen
LOG_BUF   equ 0x7800  ; 1 KiB trace buffer for the asm side
LOG_CAP   equ 0x0400
LOGF_ECHO equ 0x01
