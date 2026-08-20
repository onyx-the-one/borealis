#include "config.h"
#include "basic.h"
#include "fs/fat12.h"
#include "log.h"

Config cfg = { 1, 1 };

#define CFG_BUFSZ 2048
#define LOG_BUFSZ 4096

static int keyeq(const char *a, const char *b){
    while(*a && *b){
        char x = *a, y = *b;
        if(x >= 'a' && x <= 'z') x -= 32;
        if(y >= 'a' && y <= 'z') y -= 32;
        if(x != y) return 0;
        a++; b++;
    }
    return !*a && !*b;
}

static void parse_line(char *line){
    char *p = line;
    while(*p == ' ' || *p == '\t') p++;
    if(!*p || *p == '#') return;
    /* optional leading line number, so EDIT CONFIG.TXT works as-is */
    if(*p >= '0' && *p <= '9'){
        char *q = p;
        while(*q >= '0' && *q <= '9') q++;
        if(*q == ' ' || *q == '\t'){ p = q; while(*p == ' ' || *p == '\t') p++; }
    }
    if(!*p || *p == '#') return;
    char key[24];
    int ki = 0;
    while(*p && *p != '=' && *p != ' ' && *p != '\t' && ki < (int)sizeof(key) - 1)
        key[ki++] = *p++;
    key[ki] = 0;
    while(*p == ' ' || *p == '\t') p++;
    if(*p != '=') return;
    p++;
    while(*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if(*p == '-'){ neg = 1; p++; }
    if(!(*p >= '0' && *p <= '9')) return;
    int v = 0;
    while(*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if(neg) v = -v;
    if(keyeq(key, "SPLASH")) cfg.splash = v ? 1 : 0;
    else if(keyeq(key, "LOG")) cfg.log = v ? 1 : 0;
}

void config_init(void){
    static char buf[CFG_BUFSZ];
    if(!fat_ready()){
        log_puts("[DEBUG helix: config skipped, disk not ready]\n");
        return;
    }
    int n = fat_load("CONFIG  TXT", buf, CFG_BUFSZ - 1);
    if(n <= 0){
        log_puts("[DEBUG helix: no CONFIG.TXT, using defaults]\n");
        return;
    }
    buf[n] = 0;
    char *p = buf;
    while(*p){
        char *e = p;
        while(*e && *e != '\n') e++;
        char save = *e;
        *e = 0;
        parse_line(p);
        *e = save;
        p = save ? e + 1 : e;
    }
    log_puts("[DEBUG helix: config loaded, splash=");
    log_puti(cfg.splash);
    log_puts(", log=");
    log_puti(cfg.log);
    log_puts("]\n");
}

void klog(const char *msg){
    if(!cfg.log) return;
    if(!fat_ready()) return;
    static char buf[LOG_BUFSZ];
    int n = fat_load("LOG     TXT", buf, LOG_BUFSZ - 1);
    if(n < 0) n = 0;
    if(n > LOG_BUFSZ - 128) n = LOG_BUFSZ - 128;
    buf[n++] = '\n';
    const char *p = msg;
    while(*p && n < LOG_BUFSZ - 1) buf[n++] = *p++;
    buf[n] = 0;
    fat_save("LOG     TXT", buf, n);
}
