#ifndef TIMER_H
#define TIMER_H
#include <stdint.h>

static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

/* PIT channel 0 free-runs at 1.193182 MHz from BIOS setup, counting down
   from 65536. Readable from any cpu mode, needs no interrupts, and unlike
   the port-0x80 loop it means the same millisecond on QEMU and on real
   LPC hardware. */
static inline uint16_t pit_count(void){
    outb(0x43, 0x00); /* latch channel 0 */
    uint8_t lo = inb(0x40);
    return (uint16_t)(lo | ((uint16_t)inb(0x40) << 8));
}

static inline int pit_alive(void){
    uint16_t a = pit_count();
    for(uint32_t i = 0; i < 10000; i++)
        if(pit_count() != a) return 1;
    return 0;
}

static inline void mdelay(uint32_t ms){
    static int use_pit = -1;
    if(use_pit < 0) use_pit = pit_alive();
    if(!use_pit){ /* fallback: the old port-0x80 loop, nominal at best */
        while(ms--) for(uint32_t i = 0; i < 1000; i++) outb(0x80, 0);
        return;
    }
    uint32_t left = ms * 1193; /* ticks per ms */
    uint16_t prev = pit_count();
    while(left){
        uint16_t now = pit_count();
        uint32_t d = (uint16_t)(prev - now); /* 16-bit wrap is natural */
        if(d > left) d = left;
        left -= d;
        prev = now;
    }
}

#endif
