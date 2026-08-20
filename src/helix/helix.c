#include "basic.h"
#include "fs/fat12.h"
#include "config.h"
#include "log.h"
#include "usb/usb.h"

static inline void outb(uint16_t p,uint8_t v){__asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t v;__asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p));return v;}

/* ── VGA text terminal ────────────────────────────────────────────── */
#define COLS 80
#define ROWS 25
#define VGA ((volatile uint16_t *)0xB8000)
static int col=0,row=0;
static uint8_t attr=0x07;

static void cur(void){uint16_t pos=(uint16_t)(row*COLS+col);outb(0x3D4,0x0F);outb(0x3D5,(uint8_t)(pos&0xFF));outb(0x3D4,0x0E);outb(0x3D5,(uint8_t)(pos>>8));}

void term_set_color(uint8_t fg,uint8_t bg){attr=(uint8_t)((bg<<4)|(fg&0xF));}

void term_clear(void){uint16_t blank=(uint16_t)((uint16_t)attr<<8|' ');for(int i=0;i<COLS*ROWS;i++)VGA[i]=blank;col=row=0;cur();}

static void scroll(void){
    for(int r=1;r<ROWS;r++)
        for(int c=0;c<COLS;c++)
            VGA[(r-1)*COLS+c]=VGA[r*COLS+c];
    uint16_t blank=(uint16_t)((uint16_t)attr<<8|' ');
    for(int c=0;c<COLS;c++)
        VGA[(ROWS-1)*COLS+c]=blank;
    row=ROWS-1;
}

void term_putchar(char c){
    if(c=='\n'){col=0;if(++row>=ROWS)scroll();}
    else if(c=='\r'){col=0;}
    else if(c=='\b'){if(col>0){col--;VGA[row*COLS+col]=(uint16_t)((uint16_t)attr<<8|' ');}}
    else{VGA[row*COLS+col]=(uint16_t)((uint16_t)attr<<8|(unsigned char)c);if(++col>=COLS){col=0;if(++row>=ROWS)scroll();}}
}

void term_puts(const char *s){while(*s)term_putchar(*s++);}

void term_puti(int32_t n){char buf[12];int i=0;if(n<0){term_putchar('-');n=-n;}if(!n){term_putchar('0');return;}while(n){buf[i++]='0'+(int)(n%10);n/=10;}while(i--)term_putchar(buf[i]);}

void term_putf(double f){
    if(f<0.0){term_putchar('-');f=-f;}
    if(f!=0.0&&(f>=1e10||f<1e-4)){
        int exp=0;
        while(f>=10.0){f/=10.0;exp++;}
        while(f<1.0){f*=10.0;exp--;}
        term_putf(f);
        term_puts("E");
        if(exp<0){term_putchar('-');exp=-exp;}
        term_puti((int32_t)exp);
        return;
    }
    int32_t ipart=(int32_t)f;
    double fpart=f-(double)ipart;
    term_puti(ipart);
    char buf[10];
    int n=0;
    for(int i=0;i<8;i++){fpart*=10.0;int d=(int)fpart;buf[n++]='0'+d;fpart-=(double)d;}
    while(n>1&&buf[n-1]=='0')n--;
    if(n>0){term_putchar('.');for(int i=0;i<n;i++)term_putchar(buf[i]);}
}

static void dbg_hex8(uint8_t v){
    const char hexd[]="0123456789ABCDEF";
    term_putchar(hexd[(v>>4)&0xF]);
    term_putchar(hexd[v&0xF]);
}

/* ── Keyboard ─────────────────────────────────────────────────────── */
/* arrow keys arrive as extended scancodes (0xE0 prefix); they are
   returned as KEY_* codes (>255) so they can never collide with ASCII */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103

static const char sclo[128]={0,0x1B,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0};
static const char schi[128]={0,0x1B,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0};
static int shift=0,caps=0;
static uint8_t kb_read(void){while(!(inb(0x64)&1));return inb(0x60);}

int term_getchar(void){
    for(;;){
        uint8_t sc=kb_read();
        if(sc==0xE0){
            sc=kb_read();
            if(sc&0x80)continue;              /* extended break code */
            if(sc==0x48)return KEY_UP;
            if(sc==0x50)return KEY_DOWN;
            if(sc==0x4B)return KEY_LEFT;
            if(sc==0x4D)return KEY_RIGHT;
            continue;                         /* other extended keys ignored */
        }
        if(sc&0x80){sc&=0x7F;if(sc==0x2A||sc==0x36)shift=0;continue;}
        if(sc==0x2A||sc==0x36){shift=1;continue;}
        if(sc==0x3A){caps^=1;continue;}
        if(sc>=128)continue;
        char c=shift?schi[sc]:sclo[sc];
        if(!c)continue;
        if(caps){if(c>='a'&&c<='z')c-=32;else if(c>='A'&&c<='Z')c+=32;}
        return(unsigned char)c;
    }
}

int term_peekkey(void){
    if(!(inb(0x64)&1))return 0;
    uint8_t sc=inb(0x60);
    if(sc==0xE0){while(!(inb(0x64)&1));(void)inb(0x60);return 0;} /* swallow extended key, report nothing */
    if(sc&0x80){sc&=0x7F;if(sc==0x2A||sc==0x36)shift=0;return 0;}
    if(sc==0x2A||sc==0x36){shift=1;return 0;}
    if(sc==0x3A){caps^=1;return 0;}
    if(sc>=128)return 0;
    char c=shift?schi[sc]:sclo[sc];
    if(!c)return 0;
    if(caps){if(c>='a'&&c<='z')c-=32;else if(c>='A'&&c<='Z')c+=32;}
    return(unsigned char)c;
}

/* ── Line editing + history ───────────────────────────────────────── */
/* history lives in the terminal layer so REPL, INPUT and EDIT prompts
   all share it; duplicates of the most recent entry are not stored */
#define HIST_MAX 16
#define HIST_LEN 256
static char hist[HIST_MAX][HIST_LEN];
static int hist_n=0;

static void hist_push(const char *s){
    if(!*s)return;
    if(hist_n>0){
        const char *last=hist[hist_n-1];
        int i=0;while(s[i]&&s[i]==last[i])i++;
        if(!s[i]&&!last[i])return;
    }
    if(hist_n==HIST_MAX){
        for(int i=0;i<HIST_MAX-1;i++){int j=0;while((hist[i][j]=hist[i+1][j]))j++;}
        hist_n--;
    }
    int i=0;while(i<HIST_LEN-1&&(hist[hist_n][i]=s[i]))i++;
    hist[hist_n][i]=0;
    hist_n++;
}

void term_getline(char *buf,int max){
    int len=0,pos=0,hidx=hist_n,startcol=col;
    char scratch[HIST_LEN];
    scratch[0]=0;
    (void)startcol;
    for(;;){
        int c=term_getchar();
        if(c=='\n'||c=='\r'){
            term_putchar('\n');buf[len]=0;cur();
            hist_push(buf);
            return;
        }
        if(c==KEY_UP||c==KEY_DOWN){
            const char *e=0;
            if(c==KEY_UP){
                if(hidx<=0)continue;
                if(hidx==hist_n){int i=0;while(i<HIST_LEN-1&&i<len){scratch[i]=buf[i];i++;}scratch[i]=0;}
                hidx--;e=hist[hidx];
            }else{
                if(hidx>=hist_n)continue;
                hidx++;
                e=(hidx==hist_n)?scratch:hist[hidx];
            }
            while(len>0){term_putchar('\b');len--;}
            int i=0;while(e[i]&&i<max-1){buf[i]=e[i];term_putchar(e[i]);i++;}
            len=pos=i;cur();
            continue;
        }
        if(c==KEY_LEFT){if(pos>0){pos--;col--;cur();}continue;}
        if(c==KEY_RIGHT){if(pos<len){pos++;col++;cur();}continue;}
        if(c=='\b'){
            if(pos>0){
                for(int i=pos-1;i<len-1;i++)buf[i]=buf[i+1];
                pos--;len--;
                term_putchar('\b');                         /* visual erase, col-- */
                for(int i=pos;i<len;i++)term_putchar(buf[i]); /* reprint tail */
                term_putchar(' ');                          /* erase leftover tail cell */
                int back=len-pos+1;
                while(back-->0){col--;cur();}
            }
            continue;
        }
        if(c<32||c>126)continue;
        if(len>=max-1)continue;
        if(pos==len){
            buf[len++]=(char)c;pos++;
            term_putchar((char)c);cur();
        }else{
            for(int i=len;i>pos;i--)buf[i]=buf[i-1];
            buf[pos]=(char)c;len++;
            for(int i=pos;i<len;i++)term_putchar(buf[i]);
            pos++;
            int back=len-pos;
            while(back-->0){col--;cur();}
        }
    }
}

void term_sync_cursor(void){cur();}

/* ── Kernel panic ─────────────────────────────────────────────────── */
void kpanic(const char *msg){
    term_set_color(VGA_RED,VGA_WHITE);
    term_puts(" HELIX FAILURE ");
    term_set_color(VGA_YELLOW,VGA_RED);
    term_putchar(' ');
    term_puts(msg);
    term_putchar(' ');
    term_set_color(VGA_WHITE,VGA_RED);
    term_puts(" System halted. Power off or reset. ");
    __asm__ __volatile__("cli");
    for(;;)__asm__ __volatile__("hlt");
}

/* ── Boot banner ──────────────────────────────────────────────────── */
static void bannerrow(const char *s,uint8_t fg,uint8_t bg){
    term_set_color(fg,bg);
    term_putchar(' ');
    const char *p=s;
    int len=0;
    while(*p++)len++;
    term_puts(s);
    for(int i=len+1;i<COLS;i++)term_putchar(' ');
}

void draw_banner(void){
    term_clear();
    bannerrow("",VGA_BLACK,VGA_CYAN);
    bannerrow("BOREALIS",VGA_WHITE,VGA_BLUE);
    bannerrow("x86-32 ver. 1.7.122-3r 29-07-2026",VGA_LIGHT_GREY,VGA_BLUE);
    bannerrow("",VGA_BLACK,VGA_CYAN);
    term_set_color(VGA_DARK_GREY,VGA_BLACK);
    for(int i=0;i<COLS;i++)term_putchar('-');
    term_putchar('\n');
    term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
}

/* ── Kernel entry ─────────────────────────────────────────────────── */
/* DEBUG POLICY (permanent, not to be removed): this kernel runs on
 * unknown hardware with no debugger and no serial port, so every boot
 * step that can fail prints an explicit checkpoint, on success and on
 * failure both. Lines are tagged "[DEBUG helix:" and go to the normal
 * scrolling terminal so they persist in scrollback. Quiet on the happy
 * path is fine; silence on failure or anomaly is not. */
void kernel_main(uint8_t boot_drive){
    /* flush stale PS/2 output bytes left by the BIOS; bounded so a
     * stuck controller can never hang boot silently */
    int flushed=0,safety=0;
    while(inb(0x64)&1){
        (void)inb(0x60);
        flushed++;
        if(++safety>1000)break;
    }

    draw_banner();

    /* first checkpoint inside helix.c. entry.asm's [EZTG on the bottom
     * row covers everything before this; if you see those but never
     * this line, the call into kernel_main itself is failing (stack,
     * calling convention, or linker/relocation). */
    term_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    log_puts("[DEBUG helix: kernel_main ENTER boot_drive=0x");
    log_hex8(boot_drive);
    log_puts("]\n");
    term_set_color(VGA_LIGHT_GREY,VGA_BLACK);

    if(safety>1000){
        term_set_color(VGA_YELLOW,VGA_BLACK);
        log_puts("[DEBUG helix: WARNING PS/2 flush hit safety limit (stuck controller?) -- continuing anyway]\n");
        term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    }else if(flushed){
        log_puts("[DEBUG helix: PS/2 flush discarded ");
        log_puti(flushed);
        log_puts(" stale byte(s)]\n");
    }

	usb_probe();
    fat_init(boot_drive);

    if(!fat_ready()){
        term_set_color(VGA_YELLOW,VGA_BLACK);
        term_puts(" WARNING: FAT init failed (drive=0x");
        dbg_hex8(boot_drive);
        term_puts(") -- LOAD/SAVE/DIR unavailable\n\n");
        term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    }else{
        term_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        log_puts("[DEBUG helix: filesystem ready, LOAD/SAVE/DIR available]\n");
        term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    }

    config_init();

/* boot debug log: if config allows and the fs write works, park the
   captured log in LOG.TXT and take debug output off the screen. on any
   failure echo stays on and everything so far is already visible --
   the fallback is the default, not a code path. */
if(cfg.log && log_flush_to_disk()==0){
log_echo_off();
term_clear();
draw_banner();
term_puts("BOOT LOG IN LOG.TXT\n");
}


    /* splash: if enabled and present on disk, run SPLASH.BAS before the
     * REPL. running=1 makes basic_run() execute the loaded program first;
     * when it ends (END/error/keypress out) the REPL starts clean. */
    if(cfg.splash && fat_ready()){
        int r = prog_load_name11("SPLASH  BAS");
        if(r > 0) running = 1;
    }

    term_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
    log_puts("[DEBUG helix: system checks complete -- starting BASIC interpreter]\n");
    term_set_color(VGA_LIGHT_GREY,VGA_BLACK);

    basic_run();

    /* basic_run() never returns; landing here means something inside
     * basic.c fell through every one of its own loops */
    term_set_color(VGA_LIGHT_RED,VGA_BLACK);
    log_puts("[DEBUG helix: FATAL basic_run() returned control to kernel_main() -- this should be impossible]\n");
    term_set_color(VGA_LIGHT_GREY,VGA_BLACK);

    kpanic("basic_run() returned unexpectedly");
}
