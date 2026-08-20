/* log.c — debug log capture. All [DEBUG] output goes into a buffer; the
   asm-side thunk trace (entry.asm/thunk16.asm) lands in a fixed low-memory
   block and is drained into the same stream so ordering is kept.
   Echo to screen defaults ON at the hardware level (entry.asm sets it
   before its first checkpoint): a boot that dies before the flush point
   still shows everything live. log_flush_to_disk() + log_echo_off() on
   success are what make the screen clean — the fallback needs no code
   path of its own. Addresses mirror logmem.inc. */
#include "log.h"
#include "basic.h"
#include "fat12.h"
#include <stdint.h>

#define LOG_WPTR  (*(volatile uint16_t *)0x77F0u)
#define LOG_FLAGS (*(volatile uint8_t  *)0x77F2u)
#define LOG_BUF   ((volatile char *)0x7800u)
#define LOG_CAP   0x400u
#define LOGF_ECHO 0x01u

#define LOGB_CAP 16384
static char logb[LOGB_CAP];
static uint16_t logb_len;
static int logb_dropped;
static uint16_t asm_drained;

static void drain_asm(void){
    uint16_t w = LOG_WPTR;
    if(w > LOG_CAP) w = LOG_CAP;
    while(asm_drained < w && logb_len < LOGB_CAP)
        logb[logb_len++] = LOG_BUF[asm_drained++];
}

void log_putc(char c){
    drain_asm();
    if(LOG_FLAGS & LOGF_ECHO) term_putchar(c);
    if(logb_len >= LOGB_CAP){ logb_dropped = 1; return; }
    logb[logb_len++] = c;
}

void log_puts(const char *s){ while(*s) log_putc(*s++); }

void log_puti(int32_t n){
    char buf[12];
    int i = 0;
    if(n < 0){ log_putc('-'); n = -n; }
    if(!n){ log_putc('0'); return; }
    while(n){ buf[i++] = '0' + (int)(n % 10); n /= 10; }
    while(i--) log_putc(buf[i]);
}

void log_hex8(uint8_t v){
    const char *h = "0123456789ABCDEF";
    log_putc(h[v >> 4]);
    log_putc(h[v & 0xF]);
}

void log_hex32(uint32_t v){
    for(int s = 28; s >= 0; s -= 4)
        log_putc("0123456789ABCDEF"[(v >> s) & 0xF]);
}

int log_flush_to_disk(void){
    drain_asm();
    if(logb_dropped){
        const char *m = "\n[log truncated]\n";
        while(*m && logb_len < LOGB_CAP) logb[logb_len++] = *m++;
    }
    if(!fat_ready()) return -1;
    return fat_save("LOG     TXT", logb, logb_len) == 0 ? 0 : -1;
}

void log_echo_off(void){ LOG_FLAGS &= ~LOGF_ECHO; }
int log_echo(void){ return !!(LOG_FLAGS & LOGF_ECHO); }
