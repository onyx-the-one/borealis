#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>

/* Boot-time config from CONFIG.TXT on the boot disk. Keys are
   case-insensitive, values are integers; unknown keys are ignored.
   Missing file is not an error -- defaults apply. */
typedef struct {
    int splash; /* 1 = run SPLASH.BAS at boot (default), 0 = skip */
    int log;    /* 1 = boot debug log to LOG.TXT (default), 0 = off */
} Config;

extern Config cfg;

void config_init(void);
void klog(const char *msg); /* no-op unless cfg.log and disk ready */

#endif
