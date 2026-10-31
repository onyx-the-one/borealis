#include "idt.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static inline void outb(uint16_t port, uint8_t val) {
	    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
	    outb(0x80, 0x00);
}

static struct idt_entry idt[256];

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
	    idt[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
    idt[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
}

static void pic_remap(void) {
	    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
	io_wait();

	outb(PIC1_DATA, 0x04);
	io_wait();
	outb(PIC2_DATA, 0x02);
	io_wait();

	outb(PIC1_DATA, 0x01);
	io_wait();
	outb(PIC2_DATA, 0x01);
	io_wait();

	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);


}

void pic_unmask_irq(uint8_t irq) {
	    uint16_t port;
    uint8_t value;

    if (irq < 8) {
	        port = PIC1_DATA;
    } else {
	        port = PIC2_DATA;
        irq -= 8;
    }
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    value &= ~(1 << irq);
    outb(port, value);
}

void idt_init(void) {
	    for (int i = 0; i < 256; i++) {
	        idt_set_gate(i, 0, 0, 0);
    }

    pic_remap();

    struct idt_ptr idtp;
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base  = (uint32_t)&idt[0];

    __asm__ __volatile__("lidt %0" : : "m"(idtp));
}

static volatile uint32_t system_ticks = 0;

void timer_tick_handler(void) {
	system_ticks++;
}

uint32_t timer_get_ticks(void) {
	return system_ticks;
}

void sleep_ticks(uint32_t count) {
	uint32_t target = system_ticks + count;
	while (system_ticks < target) {
		__asm__ __volatile__("hlt");
	}
}

extern void save_kernel_idtr(void);
extern void irq0_isr(void);

void timer_init(void) {
	idt_set_gate(0x20, (uint32_t)irq0_isr, 0x08, 0x8E);

	uint16_t divisor = 1192; /*1 tick per ms */
	outb(0x43, 0x36);
	outb(0x40, (uint8_t)(divisor & 0xFF));
	outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

	pic_unmask_irq(0);

	save_kernel_idtr();

	__asm__ __volatile__("sti");	
}
