#ifndef BASIC_H
#define BASIC_H
#include <stdint.h>

/* VGA colour constants */
#define VGA_BLACK        0x0
#define VGA_BLUE         0x1
#define VGA_GREEN        0x2
#define VGA_CYAN         0x3
#define VGA_RED          0x4
#define VGA_MAGENTA      0x5
#define VGA_BROWN        0x6
#define VGA_LIGHT_GREY   0x7
#define VGA_DARK_GREY    0x8
#define VGA_LIGHT_BLUE   0x9
#define VGA_LIGHT_GREEN  0xA
#define VGA_LIGHT_CYAN   0xB
#define VGA_LIGHT_RED    0xC
#define VGA_LIGHT_MAGENTA 0xD
#define VGA_YELLOW       0xE
#define VGA_WHITE        0xF

/* Terminal I/O */
void term_set_color(uint8_t fg, uint8_t bg);
void term_clear(void);
void term_putchar(char c);
void term_puts(const char *s);
void term_puti(int32_t n);
void term_putf(double f);
int  term_getchar(void);
int  term_peekkey(void);
void term_getline(char *buf, int max);
void term_sync_cursor(void);

/* Kernel services */
void draw_banner(void);
void kpanic(const char *msg);

/* BASIC interpreter entry point */
void basic_run(void);

int prog_load_name11(const char nm11[12]);
extern int running;

#endif
