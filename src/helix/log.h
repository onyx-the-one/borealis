#ifndef LOG_H
#define LOG_H
#include <stdint.h>

void log_putc(char c);
void log_puts(const char *s);
void log_puti(int32_t n);
void log_hex8(uint8_t v);
void log_hex32(uint32_t v);

/* writes the captured log to LOG.TXT via fat_save; 0 on success,
   -1 if the fs is unwritable (caller leaves echo on in that case) */
int  log_flush_to_disk(void);

/* screen goes quiet; the log keeps capturing */
void log_echo_off(void);
int  log_echo(void);

#endif
