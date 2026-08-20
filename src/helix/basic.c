#include "basic.h"
#include "fs/fat12.h"
#include "sound/sound.h"
#include "rtc/rtc.h"
#include "gfx/gfx.h"
#include "config.h"

/* ── x87 math helpers ────────────────────────────────────────────── */
static double x87sin(double x){double r;__asm__("fsin":"=t"(r):"0"(x));return r;}
static double x87cos(double x){double r;__asm__("fcos":"=t"(r):"0"(x));return r;}
static double x87sqrt(double x){double r;__asm__("fsqrt":"=t"(r):"0"(x));return r;}
static double x87log(double x){double r;__asm__("fldln2;fxch;fyl2x":"=t"(r):"0"(x));return r;}
static double x87exp(double x){double r;__asm__("fldl2e;fmulp;fld %%st(0);frndint;fxch;fsub %%st(1),%%st(0);f2xm1;fld1;faddp;fscale;fstp %%st(1)":"=t"(r):"0"(x));return r;}
static double x87tan(double x){return x87sin(x)/x87cos(x);}
static double x87atn(double x){double r;__asm__("fpatan":"=t"(r):"0"(x),"u"(1.0));return r;}
static double x87abs(double x){return x<0.0?-x:x;}
static double x87int(double x){double r;__asm__("frndint":"=t"(r):"0"(x));return r;}
static double x87fix(double x){return x<0.0?-x87int(-x):x87int(x);}
static int32_t x87sgn(double x){return x>0.0?1:x<0.0?-1:0;}

/* ── RNG ─────────────────────────────────────────────────────────── */
static uint32_t rng_state=12345;
static double rnd(void){rng_state=rng_state*1664525u+1013904223u;return (double)rng_state/4294967296.0;}

/* ── Val ─────────────────────────────────────────────────────────── */
#define TY_INT   0
#define TY_FLOAT 1
#define TY_STR   2
typedef struct{int ty;int32_t i;double f;int si;}Val;

/* ── String pool ─────────────────────────────────────────────────── */
#define NSTRS 128
#define SLEN  256
static char spool[NSTRS][SLEN];
static int  sused[NSTRS];
static int snew(void){for(int i=0;i<NSTRS;i++)if(!sused[i]){sused[i]=1;spool[i][0]=0;return i;}return -1;}
static void sfree(int i){if(i>=0&&i<NSTRS)sused[i]=0;}
static Val vint(int32_t i){Val v;v.ty=TY_INT;v.i=i;v.f=(double)i;v.si=-1;return v;}
static Val vflt(double f){Val v;v.ty=TY_FLOAT;v.f=f;v.i=(int32_t)f;v.si=-1;return v;}
static Val vstr(int si){Val v;v.ty=TY_STR;v.si=si;v.i=0;v.f=0;return v;}

static int32_t toint(Val v){return v.ty==TY_FLOAT?(int32_t)v.f:v.i;}
static Val vbitand(Val a,Val b){return vint(toint(a)&toint(b));}
static Val vbitor(Val a,Val b){return vint(toint(a)|toint(b));}
static Val vbitxor(Val a,Val b){return vint(toint(a)^toint(b));}
static Val vbitnot(Val a){return vint(~toint(a));}
static Val vshl(Val a,Val b){return vint((int32_t)((uint32_t)toint(a) << (toint(b)&31)));}
static Val vshr(Val a,Val b){return vint((int32_t)((uint32_t)toint(a) >> (toint(b)&31)));}

/* ── Arithmetic helpers ──────────────────────────────────────────── */
#define NUMVAL(v) ((v).ty==TY_FLOAT?(v).f:(double)(v).i)
static Val vadd(Val a,Val b){if(a.ty==TY_STR||b.ty==TY_STR)return vint(0);if(a.ty==TY_FLOAT||b.ty==TY_FLOAT)return vflt(NUMVAL(a)+NUMVAL(b));return vint(a.i+b.i);}
static Val vsub(Val a,Val b){if(a.ty==TY_STR||b.ty==TY_STR)return vint(0);if(a.ty==TY_FLOAT||b.ty==TY_FLOAT)return vflt(NUMVAL(a)-NUMVAL(b));return vint(a.i-b.i);}
static Val vmul(Val a,Val b){if(a.ty==TY_STR||b.ty==TY_STR)return vint(0);if(a.ty==TY_FLOAT||b.ty==TY_FLOAT)return vflt(NUMVAL(a)*NUMVAL(b));return vint(a.i*b.i);}
static Val vdiv(Val a,Val b){return vflt(NUMVAL(a)/NUMVAL(b));}
static Val vmod(Val a,Val b){if(!b.i&&b.ty==TY_INT)return vint(0);return vint(a.i%b.i);}
static int vcmp(Val a,Val b){if(a.ty==TY_STR&&b.ty==TY_STR){const char *p=spool[a.si],*q=spool[b.si];while(*p&&*p==*q){p++;q++;}return *p-*q;}double d=NUMVAL(a)-NUMVAL(b);return d>0?1:d<0?-1:0;}
static void vprint(Val v){if(v.ty==TY_STR){term_puts(spool[v.si]);return;}if(v.ty==TY_INT)term_puti(v.i);else term_putf(v.f);}

/* ── Variable table ──────────────────────────────────────────────── */
#define MAXVARS  256
#define NAMELEN  32
typedef struct{char name[NAMELEN];Val val;int isstr;}Var;
static Var vars[MAXVARS];
static int nvars=0;

/* ── Array table ─────────────────────────────────────────────────── */
#define MAXARRAYS 32
#define MAXELEMS  512
typedef struct{char name[NAMELEN];int isstr;int dims;int size[3];int base;int total;}Array;
static Array arrays[MAXARRAYS];
static int narrays=0;
static Val aelems[MAXELEMS];
static int aused=0;

/* ── Open file table ─────────────────────────────────────────────── */
#define MAXFILES 4
#define FBUFSZ   8192
typedef struct{int open;int forwrite;char nm11[12];char buf[FBUFSZ];int len;int pos;int dirty;}FileSlot;
static FileSlot files[MAXFILES];

/* ── String helpers ──────────────────────────────────────────────── */
static int bstreq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return !*a&&!*b;}
static void bstrcpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static void upr(char *s){while(*s){if(*s>='a'&&*s<='z')*s-=32;s++;}}
static int bstrlen(const char *s){int n=0;while(s[n])n++;return n;}

/* ── Variable helpers ────────────────────────────────────────────── */
static Var *varget(const char *nm){for(int i=0;i<nvars;i++)if(bstreq(vars[i].name,nm))return &vars[i];return 0;}
static Var *varset(const char *nm,Val v){
    Var *vp=varget(nm);
    if(vp){if(vp->val.ty==TY_STR&&v.ty!=TY_STR)sfree(vp->val.si);vp->val=v;return vp;}
    if(nvars>=MAXVARS)return 0;
    Var *nv=&vars[nvars++];
    int i=0;while(nm[i]&&i<NAMELEN-1){nv->name[i]=nm[i];i++;}nv->name[i]=0;
    nv->isstr=nm[i-1]=='$';nv->val=v;return nv;}

/* ── Program store ───────────────────────────────────────────────── */
#define MAXLINES 1024
#define LINELEN  128
typedef struct{uint16_t num;char txt[LINELEN];}Line;
static Line P[MAXLINES];
static int Pn=0;

/* shared IO buffer */
static char fbuf[MAXLINES*LINELEN];

/* ── Runtime state ───────────────────────────────────────────────── */
#define CALLDEPTH 64
#define FORDEPTH  32
#define WHILEDEPTH 32
#define IBUF 256
typedef struct{char var[NAMELEN];Val lim,step;int ret;}ForFrame;
typedef struct{int ret;}WhileFrame;
int pc=0,running=0,err=0;
static int cstk[CALLDEPTH];
static int csp=0;
static ForFrame fstk[FORDEPTH];
static int fsp=0;
static WhileFrame wstk[WHILEDEPTH];
static int wsp=0;
static char ibuf[IBUF];
static const char *pp;
static int dataline=0,datacol=0;

static void sw(void){while(*pp==' ')pp++;}
static void berr(const char *m){if(err)return;err=1;running=0;term_set_color(VGA_LIGHT_RED,VGA_BLACK);term_puts("? ");term_puts(m);term_putchar('\n');term_set_color(VGA_LIGHT_GREY,VGA_BLACK);klog(m);}
static int kw(const char *k){const char *ppp=pp;while(*k){char a=*pp,b=*k;if(a>='a'&&a<='z')a-=32;if(a!=b){pp=ppp;return 0;}pp++;k++;}char nx=*pp;if((nx>='A'&&nx<='Z')||(nx>='a'&&nx<='z')||(nx>='0'&&nx<='9')||nx=='_'){pp=ppp;return 0;}return 1;}
static int readname(char *out,int max){sw();char c=*pp;if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')))return 0;int i=0;while((*pp>='A'&&*pp<='Z')||(*pp>='a'&&*pp<='z')||(*pp>='0'&&*pp<='9')||*pp=='_'){char ch=*pp++;if(ch>='a'&&ch<='z')ch-=32;if(i<max-2)out[i++]=ch;}if(*pp=='$'&&i<max-1)out[i++]=*pp++;out[i]=0;return i>0;}
static Val expr(void);
static Val strexpr(void);

/* ── Number literal ──────────────────────────────────────────────── */
static int pnum(double *o){sw();if(*pp=='&'&&(pp[1]=='H'||pp[1]=='h')){pp+=2;uint32_t v=0;int got=0;for(;;){char c=*pp;int d=-1;if(c>='0'&&c<='9')d=c-'0';else if(c>='A'&&c<='F')d=10+(c-'A');else if(c>='a'&&c<='f')d=10+(c-'a');else break;v=(v<<4)|(uint32_t)d;pp++;got=1;}if(!got)return 0;*o=(double)(int32_t)v;return 1;}if(*pp=='&'&&(pp[1]=='B'||pp[1]=='b')){pp+=2;uint32_t v=0;int got=0;while(*pp=='0'||*pp=='1'){v=(v<<1)|(uint32_t)(*pp++-'0');got=1;}if(!got)return 0;*o=(double)(int32_t)v;return 1;}if(!((*pp>='0'&&*pp<='9')||(*pp=='.'&&pp[1]>='0'&&pp[1]<='9')))return 0;double v=0;int fdiv=1,frac=0;while(*pp>='0'&&*pp<='9')v=v*10+(*pp++-'0');if(*pp=='.'){pp++;while(*pp>='0'&&*pp<='9'){v=v*10+(*pp++-'0');fdiv*=10;frac=1;}}if(frac)v/=fdiv;if(*pp=='E'||*pp=='e'){pp++;int neg=0,ex=0;if(*pp=='-'){neg=1;pp++;}else if(*pp=='+')pp++;while(*pp>='0'&&*pp<='9')ex=ex*10+(*pp++-'0');double m=1;for(int i=0;i<ex;i++)m*=10.0;if(neg)v/=m;else v*=m;}*o=v;return 1;}
static int isstrname(const char *nm){int i=0;while(nm[i])i++;return i>0&&nm[i-1]=='$';}

/* ── Array helpers ───────────────────────────────────────────────── */
static Array *arrget(const char *nm){for(int i=0;i<narrays;i++)if(bstreq(arrays[i].name,nm))return &arrays[i];return 0;}
static int arridx(Array *a,int i1,int i2,int i3){
    if(a->dims==1){if(i1<0||i1>a->size[0]){berr("SUBSCRIPT");return -1;}return a->base+i1;}
    if(a->dims==2){if(i1<0||i1>a->size[0]||i2<0||i2>a->size[1]){berr("SUBSCRIPT");return -1;}return a->base+(i1*(a->size[1]+1))+i2;}
    if(i1<0||i1>a->size[0]||i2<0||i2>a->size[1]||i3<0||i3>a->size[2]){berr("SUBSCRIPT");return -1;}
    return a->base+(i1*(a->size[1]+1)*(a->size[2]+1))+(i2*(a->size[2]+1))+i3;}

/* ── String functions ────────────────────────────────────────────── */
static Val callstrfn(const char *nm){
    sw();pp++;
    if(bstreq(nm,"INKEY")){pp--;int r=snew();if(r<0){berr("MEM");return vstr(-1);}int c=term_peekkey();spool[r][0]=c?(char)c:0;spool[r][1]=0;return vstr(r);}
    if(bstreq(nm,"LEFT$")){Val s=strexpr();if(err)return vstr(-1);sw();if(*pp!=','){berr("SYNTAX");return vstr(-1);}pp++;Val n=expr();if(err)return vstr(-1);sw();if(*pp!=')')  {berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}const char *src=spool[s.si];int i=0;while(src[i]&&i<n.i&&i<SLEN-1){spool[r][i]=src[i];i++;}spool[r][i]=0;if(s.si>=0&&s.ty==TY_STR)sfree(s.si);return vstr(r);}
    if(bstreq(nm,"RIGHT$")){Val s=strexpr();if(err)return vstr(-1);sw();if(*pp!=','){berr("SYNTAX");return vstr(-1);}pp++;Val n=expr();if(err)return vstr(-1);sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}const char *src=spool[s.si];int l=bstrlen(src);int st=l-n.i;if(st<0)st=0;int i=0;while(src[st+i]&&i<SLEN-1){spool[r][i]=src[st+i];i++;}spool[r][i]=0;if(s.si>=0&&s.ty==TY_STR)sfree(s.si);return vstr(r);}
    if(bstreq(nm,"MID$")){Val s=strexpr();if(err)return vstr(-1);sw();if(*pp!=','){berr("SYNTAX");return vstr(-1);}pp++;Val start=expr();if(err)return vstr(-1);int cnt=-1;sw();if(*pp==','){pp++;Val n2=expr();if(err)return vstr(-1);cnt=n2.i;}sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}const char *src=spool[s.si];int st=start.i-1;if(st<0)st=0;src+=st;int i=0;while(*src&&(cnt<0||i<cnt)&&i<SLEN-1)spool[r][i++]=*src++;spool[r][i]=0;if(s.si>=0&&s.ty==TY_STR)sfree(s.si);return vstr(r);}
    if(bstreq(nm,"STR$")){Val n=expr();if(err)return vstr(-1);sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}char *d=spool[r];int i=0;if(n.ty==TY_FLOAT){double f=n.f;if(f<0){d[i++]='-';f=-f;}int32_t ip=(int32_t)f;double fp=f-(double)ip;char tmp[12];int ti=0;if(!ip)tmp[ti++]='0';else{int32_t t=ip;while(t){tmp[ti++]='0'+t%10;t/=10;}while(ti>0&&i<SLEN-2)d[i++]=tmp[--ti];}char fb[10];int fi=0;for(int k=0;k<6;k++){fp*=10.0;int dg=(int)fp;fb[fi++]='0'+dg;fp-=(double)dg;}while(fi>1&&fb[fi-1]=='0')fi--;if(fi>0&&i<SLEN-2){d[i++]='.';for(int k=0;k<fi&&i<SLEN-2;k++)d[i++]=fb[k];}d[i]=0;}else{int32_t iv=n.i;int neg=0;if(iv<0){neg=1;iv=-iv;}char tmp[12];int ti=0;if(!iv)tmp[ti++]='0';else{int32_t t=iv;while(t){tmp[ti++]='0'+t%10;t/=10;}}if(neg&&i<SLEN-2)d[i++]='-';while(ti>0&&i<SLEN-1)d[i++]=tmp[--ti];d[i]=0;}return vstr(r);}
    if(bstreq(nm,"CHR$")){Val n=expr();if(err)return vstr(-1);sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}spool[r][0]=(char)n.i;spool[r][1]=0;return vstr(r);}
    if(bstreq(nm,"VAL")){Val s=strexpr();if(err)return vint(0);sw();if(*pp!=')'){berr("SYNTAX");return vint(0);}pp++;const char *p=spool[s.si];int neg=0;double n=0;int fdiv=1,frac=0;if(*p=='-'){neg=1;p++;}while(*p>='0'&&*p<='9')n=n*10+(*p++-'0');if(*p=='.'){p++;while(*p>='0'&&*p<='9'){n=n*10+(*p++-'0');fdiv*=10;frac=1;}}if(frac)n/=fdiv;if(neg)n=-n;if(s.ty==TY_STR)sfree(s.si);return n!=(double)(int32_t)n?vflt(n):vint((int32_t)n);}
    if(bstreq(nm,"ASC")){Val s=strexpr();if(err)return vint(0);sw();if(*pp!=')'){berr("SYNTAX");return vint(0);}pp++;int c=(unsigned char)spool[s.si][0];if(s.ty==TY_STR)sfree(s.si);return vint(c);}
    if(bstreq(nm,"HEX$")){Val n=expr();if(err)return vstr(-1);sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}uint32_t v=(uint32_t)toint(n);char *d=spool[r];int i=0,start=0;for(int s=28;s>=0;s-=4){char c="0123456789ABCDEF"[(v>>s)&0xF];if(c!='0'||start||s==0){d[i++]=c;start=1;}}d[i]=0;return vstr(r);}
if(bstreq(nm,"BIN$")){Val n=expr();if(err)return vstr(-1);sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}uint32_t v=(uint32_t)toint(n);char *d=spool[r];int i=0,start=0;for(int s=31;s>=0;s--){char c=((v>>s)&1)?'1':'0';if(c!='0'||start||s==0){d[i++]=c;start=1;}}d[i]=0;return vstr(r);}
if(bstreq(nm,"DATE$")){pp--;if(*pp=='('){pp++;sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;}int r=snew();if(r<0){berr("MEM");return vstr(-1);}RtcTime rt;if(rtc_read(&rt)<0){berr("RTC");return vstr(-1);}rtc_format_date(&rt,spool[r],SLEN);return vstr(r);}
if(bstreq(nm,"TIME$")){pp--;if(*pp=='('){pp++;sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;}int r=snew();if(r<0){berr("MEM");return vstr(-1);}RtcTime rt;if(rtc_read(&rt)<0){berr("RTC");return vstr(-1);}rtc_format_time(&rt,spool[r],SLEN);return vstr(r);}
if(bstreq(nm,"INKEY$")){pp--;int r=snew();if(r<0){berr("MEM");return vstr(-1);}int c=term_peekkey();spool[r][0]=c?(char)c:0;spool[r][1]=0;return vstr(r);}
    berr("UNKNOWN FUNCTION");return vstr(-1);}

/* ── Numeric functions ───────────────────────────────────────────── */
static Val numfn(const char *nm){
    sw();pp++;Val a=expr();if(err)return vint(0);Val second=vint(0);int hassecond=0;sw();if(*pp==','){pp++;second=expr();if(err)return vint(0);hassecond=1;}sw();if(*pp!=')'){berr("SYNTAX");return vint(0);}pp++;
    if(bstreq(nm,"MOD")){if(!hassecond){berr("SYNTAX");return vint(0);}return vmod(a,second);}
    if(bstreq(nm,"SIN"))return vflt(x87sin(NUMVAL(a)));
if(bstreq(nm,"COS"))return vflt(x87cos(NUMVAL(a)));
    if(bstreq(nm,"TAN"))return vflt(x87tan(NUMVAL(a)));
if(bstreq(nm,"ATN"))return vflt(x87atn(NUMVAL(a)));
    if(bstreq(nm,"EXP"))return vflt(x87exp(NUMVAL(a)));
    if(bstreq(nm,"LOG")){if(a.f<=0){berr("MATH");return vint(0);}return vflt(x87log(NUMVAL(a)));}
    if(bstreq(nm,"SQR")){if(a.f<0){berr("MATH");return vint(0);}return vflt(x87sqrt(NUMVAL(a)));}
    if(bstreq(nm,"ABS"))return a.ty==TY_FLOAT?vflt(x87abs(a.f)):vint(a.i<0?-a.i:a.i);
    if(bstreq(nm,"INT"))return vflt(x87int(NUMVAL(a)));
if(bstreq(nm,"FIX"))return vflt(x87fix(NUMVAL(a)));
    if(bstreq(nm,"SGN"))return vint(x87sgn(NUMVAL(a)));
if(bstreq(nm,"RND"))return vflt(rnd());
    if(bstreq(nm,"CINT"))return vint((int32_t)(NUMVAL(a)+0.5));
if(bstreq(nm,"CDBL"))return vflt(NUMVAL(a));
    if(bstreq(nm,"AND")){if(!hassecond){berr("SYNTAX");return vint(0);}return vbitand(a,second);}
    if(bstreq(nm,"OR")){if(!hassecond){berr("SYNTAX");return vint(0);}return vbitor(a,second);}
    if(bstreq(nm,"XOR")){if(!hassecond){berr("SYNTAX");return vint(0);}return vbitxor(a,second);}
    if(bstreq(nm,"SHL")){if(!hassecond){berr("SYNTAX");return vint(0);}return vshl(a,second);}
    if(bstreq(nm,"SHR")){if(!hassecond){berr("SYNTAX");return vint(0);}return vshr(a,second);}
    if(bstreq(nm,"NOT"))return vbitnot(a);
    if(bstreq(nm,"PEEK")){uint8_t *p=(uint8_t*)(uint32_t)a.i;return vint((int32_t)*p);}
    berr("UNKNOWN FUNCTION");return vint(0);}

/* ── strexpr ─────────────────────────────────────────────────────── */
static Val strexpr(void){
    sw();Val a;
    if(*pp=='"'){pp++;int r=snew();if(r<0){berr("MEM");return vstr(-1);}int i=0;while(*pp&&*pp!='"'&&i<SLEN-1)spool[r][i++]=*pp++;spool[r][i]=0;if(*pp=='"')pp++;a=vstr(r);}
    else{
        const char *save=pp;char nm[NAMELEN];
        if(readname(nm,NAMELEN)&&isstrname(nm)){
            sw();
            if(*pp=='('){Array *ar=arrget(nm);if(ar){pp++;Val i1=expr();if(err)return vstr(-1);Val i2=vint(0),i3=vint(0);if(*pp==','){pp++;i2=expr();if(err)return vstr(-1);}if(*pp==','){pp++;i3=expr();if(err)return vstr(-1);}sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;int idx=arridx(ar,i1.i,i2.i,i3.i);if(idx<0)return vstr(-1);int r=snew();if(r<0){berr("MEM");return vstr(-1);}if(aelems[idx].ty==TY_STR&&aelems[idx].si>=0)bstrcpy(spool[r],spool[aelems[idx].si],SLEN);else spool[r][0]=0;a=vstr(r);}
            else a=callstrfn(nm);}
            else{Var *vp=varget(nm);int r=snew();if(r<0){berr("MEM");return vstr(-1);}if(vp&&vp->val.ty==TY_STR)bstrcpy(spool[r],spool[vp->val.si],SLEN);else spool[r][0]=0;a=vstr(r);}
        }else{pp=save;berr("TYPE MISMATCH");return vstr(-1);}
    }
    for(;;){sw();if(*pp!='+')break;pp++;Val b=strexpr();if(err)break;int r=snew();if(r<0){berr("MEM");sfree(a.si);sfree(b.si);return vstr(-1);}int la=bstrlen(spool[a.si]),lb=bstrlen(spool[b.si]);int n=la+lb<SLEN-1?la+lb:SLEN-1;for(int j=0;j<la&&j<SLEN-1;j++)spool[r][j]=spool[a.si][j];for(int j=0;j<lb&&la+j<SLEN-1;j++)spool[r][la+j]=spool[b.si][j];spool[r][n]=0;sfree(a.si);sfree(b.si);a=vstr(r);}
    return a;}

/* ── Expression parser ───────────────────────────────────────────── */
static Val fact(void){
    sw();if(err)return vint(0);
    if(*pp=='('){pp++;Val v=expr();sw();if(*pp!=')')berr("SYNTAX");else pp++;return v;}
    if(*pp=='-'){pp++;Val v=fact();v.f=-v.f;v.i=-v.i;return v;}
    if(*pp=='+'){pp++;return fact();}
    double n;if(pnum(&n))return n!=(double)(int32_t)n?vflt(n):vint((int32_t)n);
    char nm[NAMELEN];const char *save=pp;
    if(readname(nm,NAMELEN)){
        sw();
        if(bstreq(nm,"INKEY")){if(*pp=='$')pp++;int r=snew();if(r<0){berr("MEM");return vint(0);}int c=term_peekkey();spool[r][0]=c?(char)c:0;spool[r][1]=0;if(*pp=='(')pp++;return vstr(r);}
        if(isstrname(nm)){pp=save;return strexpr();}
        if(bstreq(nm,"LEN")){sw();if(*pp!='('){berr("SYNTAX");return vint(0);}pp++;Val s=strexpr();if(err)return vint(0);sw();if(*pp!=')')berr("SYNTAX");else pp++;int l=bstrlen(spool[s.si]);if(s.ty==TY_STR)sfree(s.si);return vint(l);}
        if(bstreq(nm,"PEEK")){sw();if(*pp!='('){berr("SYNTAX");return vint(0);}pp++;Val a=expr();if(err)return vint(0);sw();if(*pp!=')')berr("SYNTAX");else pp++;uint8_t *p=(uint8_t*)(uint32_t)a.i;return vint((int32_t)*p);}
        if(*pp=='('){Array *ar=arrget(nm);if(ar){pp++;Val i1=expr();if(err)return vint(0);Val i2=vint(0),i3=vint(0);if(*pp==','){pp++;i2=expr();if(err)return vint(0);}if(*pp==','){pp++;i3=expr();if(err)return vint(0);}sw();if(*pp!=')')berr("SYNTAX");else pp++;int idx=arridx(ar,i1.i,i2.i,i3.i);if(idx<0)return vint(0);Val el=aelems[idx];if(el.ty==TY_STR){int r=snew();if(r<0){berr("MEM");return vint(0);}bstrcpy(spool[r],el.si>=0?spool[el.si]:"",SLEN);return vstr(r);}return el;}return numfn(nm);}
        if(bstreq(nm,"RND"))return vflt(rnd());
        Var *vp=varget(nm);return vp?vp->val:vint(0);}
    pp=save;berr("SYNTAX");return vint(0);}

static Val termexpr(void){Val a=fact();while(!err){sw();if(*pp=='*'){pp++;a=vmul(a,fact());}else if(*pp=='/'){pp++;Val b=fact();if(b.ty==TY_INT&&b.i==0){berr("DIV0");return vint(0);}a=vdiv(a,b);}else if(*pp=='\\'){pp++;Val b=fact();if(!b.i&&b.ty==TY_INT){berr("DIV0");return vint(0);}a=vint(a.i/b.i);}else break;}return a;}
static Val expr(void){Val a=termexpr();while(!err){sw();if(*pp=='+'){pp++;a=vadd(a,termexpr());}else if(*pp=='-'){pp++;a=vsub(a,termexpr());}else break;}return a;}
static int relop(void){sw();if(*pp=='<'){pp++;if(*pp=='='){pp++;return 4;}if(*pp=='>'){pp++;return 6;}return 3;}if(*pp=='>'){pp++;if(*pp=='='){pp++;return 5;}return 2;}if(*pp=='='){pp++;return 1;}return 0;}
static int applyrel(int op,Val a,Val b){int c=vcmp(a,b);switch(op){case 1:return c==0;case 2:return c>0;case 3:return c<0;case 4:return c<=0;case 5:return c>=0;case 6:return c!=0;}return 0;}

/* ── Program helpers ─────────────────────────────────────────────── */
static int findge(int n){for(int i=0;i<Pn;i++)if((int)P[i].num>=n)return i;return Pn;}

/* ── int/float to string helpers ─────────────────────────────────── */
static void itos(int32_t iv,char *out,int max){
    int neg=0,i=0;
    if(iv<0){neg=1;iv=-iv;}
    char tmp[12];int ti=0;
    if(!iv)tmp[ti++]='0';else{int32_t t=iv;while(t){tmp[ti++]='0'+t%10;t/=10;}}
    if(neg&&i<max-2)out[i++]='-';
    while(ti>0&&i<max-1)out[i++]=tmp[--ti];
    out[i]=0;
}

static void ftos(double f,char *out,int max){
    int neg=0,i=0;
    if(f<0){neg=1;f=-f;}
    int32_t ip=(int32_t)f;
    double fp=f-(double)ip;
    char tmp[12];int ti=0;
    if(!ip)tmp[ti++]='0';else{int32_t t=ip;while(t){tmp[ti++]='0'+t%10;t/=10;}}
    if(neg&&i<max-2)out[i++]='-';
    while(ti>0&&i<max-1)out[i++]=tmp[--ti];
    char fb[10];int fi=0;
    for(int k=0;k<6;k++){fp*=10.0;int dg=(int)fp;fb[fi++]='0'+dg;fp-=(double)dg;}
    while(fi>1&&fb[fi-1]=='0')fi--;
    if(fi>0&&i<max-2){out[i++]='.';for(int k=0;k<fi&&i<max-1;k++)out[i++]=fb[k];}
    out[i]=0;
}

static void storeline(int n,const char *t){
    int lo=0,hi=Pn,idx=0,found=0;
    while(lo<hi){int m=(lo+hi)/2;if((int)P[m].num==n){idx=m;found=1;break;}if((int)P[m].num<n)lo=m+1;else hi=m;idx=lo;}
    if(!found)idx=lo;
    if(found){if(!t||!t[0]){for(int i=idx;i<Pn-1;i++)P[i]=P[i+1];Pn--;return;}bstrcpy(P[idx].txt,t,LINELEN);return;}
    if(!t||!t[0])return;
    if(Pn>=MAXLINES){berr("MEM");return;}
    for(int i=Pn;i>idx;i--)P[i]=P[i-1];
    Pn++;
    P[idx].num=(uint16_t)n;
    bstrcpy(P[idx].txt,t,LINELEN);
}

/* ── 8.3 name helper ─────────────────────────────────────────────── */
static void toname11(const char *s,char nm11[12]){
    char base[8],ext[3];
    for(int i=0;i<8;i++)base[i]=' ';
    for(int i=0;i<3;i++)ext[i]=' ';
    int ni=0,ei=0;
    while(*s&&*s!='.'&&ni<8){char c=*s++;if(c>='a'&&c<='z')c-=32;base[ni++]=c;}
    if(*s=='.')s++;
    while(*s&&ei<3){char c=*s++;if(c>='a'&&c<='z')c-=32;ext[ei++]=c;}
    for(int i=0;i<8;i++)nm11[i]=base[i];
    for(int i=0;i<3;i++)nm11[8+i]=ext[i];
    nm11[11]=0;
}

static void name11txt(const char *name11,char *out,int max){
    int oi=0,nlen=0,elen=0;
    for(int i=7;i>=0;i--)if(name11[i]!=' '){nlen=i+1;break;}
    for(int i=10;i>=8;i--)if(name11[i]!=' '){elen=i-8+1;break;}
    for(int i=0;i<nlen&&oi<max-1;i++)out[oi++]=name11[i];
    if(elen&&oi<max-1){
        out[oi++]='.';
        for(int i=0;i<elen&&oi<max-1;i++)out[oi++]=name11[8+i];
    }
    out[oi]=0;
}

static int prog_to_buf(char *buf,int max){
    int pos=0;
    for(int i=0;i<Pn;i++){
        char nb[16];
        itos((int32_t)P[i].num,nb,(int)sizeof(nb));
        const char *ns=nb;
        while(*ns&&pos<max-1)buf[pos++]=*ns++;
        if(pos<max-1)buf[pos++]=' ';
        const char *src=P[i].txt;
        while(*src&&pos<max-1)buf[pos++]=*src++;
        if(pos<max-1)buf[pos++]='\n';
    }
    buf[pos]=0;
    return pos;
}

static void prog_from_buf(char *buf){
    Pn=0;
    char *p=buf;
    while(*p){
        while(*p==' '||*p=='\r'||*p=='\n')p++;
        if(!*p)break;
        if(*p>='0'&&*p<='9'){
            int ln=0;
            while(*p>='0'&&*p<='9')ln=ln*10+(*p++-'0');
            while(*p==' ')p++;
            char *start=p;
            while(*p&&*p!='\r'&&*p!='\n')p++;
            char save=*p;
            *p=0;
            storeline(ln,start);
            *p=save;
        }else{
            while(*p&&*p!='\r'&&*p!='\n')p++;
        }
        while(*p=='\r'||*p=='\n')p++;
    }
}

int prog_load_name11(const char nm11[12]){
    if(!fat_ready())return -2;
    int n=fat_load(nm11,fbuf,(int)sizeof(fbuf)-1);
    if(n<=0)return -1;
    fbuf[n]=0;
    prog_from_buf(fbuf);
    return n;
}

static int prog_save_name11(const char nm11[12]){
    if(!fat_ready())return -2;
    int len=prog_to_buf(fbuf,(int)sizeof(fbuf));
    int r=fat_save(nm11,fbuf,len);
    if(r<0)return -1;
    return len;
}

static int accept_numbered_line(const char *src,int *idx_out){
    while(*src==' ')src++;
    if(!*src)return 0;
    if(!(*src>='0'&&*src<='9'))return -1;
    int n=0;
    while(*src>='0'&&*src<='9')n=n*10+(*src++-'0');
    if(n<1||n>65535)return -1;
    while(*src==' ')src++;
    storeline(n,src);
    if(idx_out)*idx_out=findge(n);
    return 1;
}

/* ── DIR callback ────────────────────────────────────────────────── */
static void dircb(const char *name11,uint32_t size){
    char out[13];int oi=0;
    int nlen=0;for(int i=7;i>=0;i--){if(name11[i]!=' '){nlen=i+1;break;}}
    for(int i=0;i<nlen;i++)out[oi++]=name11[i];
    int elen=0;for(int i=10;i>=8;i--){if(name11[i]!=' '){elen=i-8+1;break;}}
    if(elen){out[oi++]='.';for(int i=0;i<elen;i++)out[oi++]=name11[8+i];}
    out[oi]=0;
    term_puts("  ");term_puts(out);int pad=13-oi;while(pad-->0)term_putchar(' ');term_puti((int32_t)size);term_puts(" bytes\n");
}

/* ── Editor ──────────────────────────────────────────────────────── */
#define EDIT_ROWS 22

static void edit_setmsg(char *dst,const char *src){bstrcpy(dst,src,80);}

static void edit_draw(int cur,int top,const char *target,const char *msg){
    term_clear();
    term_set_color(VGA_CYAN,VGA_BLACK);
    term_puts("BOREALIS EDIT");
    if(target&&*target){term_puts(" ");term_puts(target);}    
    term_putchar('\n');

    term_set_color(VGA_DARK_GREY,VGA_BLACK);
    if(target&&*target) term_puts("J/K move  E edit  A add  D del  W write  S save+exit  X exit\n");
    else term_puts("J/K move  E edit  A add  D del  X exit\n");

    for(int r=0;r<EDIT_ROWS;r++){
        int i=top+r;
        if(i<Pn){
            char nb[16];
            itos((int32_t)P[i].num,nb,(int)sizeof(nb));
            term_set_color(i==cur?VGA_YELLOW:VGA_LIGHT_GREY,VGA_BLACK);
            term_putchar(i==cur?'>':' ');
            int pad=5-bstrlen(nb);
            while(pad-->0)term_putchar(' ');
            term_puts(nb);
            term_puts("  ");
            term_puts(P[i].txt);
        }else{
            term_set_color(VGA_DARK_GREY,VGA_BLACK);
            term_putchar('~');
        }
        term_putchar('\n');
    }

    term_set_color(VGA_DARK_GREY,VGA_BLACK);
    if(msg&&*msg)term_puts(msg);
    else if(Pn)term_puts("ESC/X leaves editor. Blank edit cancels.");
    else term_puts("Empty program. Press A to add a numbered line.");
    term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    term_sync_cursor();
}

static void edit_prompt(const char *title,const char *oldv,char *out,int max){
    term_clear();
    term_set_color(VGA_CYAN,VGA_BLACK);
    term_puts(title);
    term_putchar('\n');
    term_set_color(VGA_DARK_GREY,VGA_BLACK);
    if(oldv&&*oldv){term_puts("OLD: ");term_puts(oldv);term_putchar('\n');}
    term_puts("Blank line cancels.\n");
    term_set_color(VGA_WHITE,VGA_BLACK);
    term_getline(out,max);
    upr(out);
}

static void run_editor(const char *target11){
    char targettxt[20];
    char msg[80];
    int cur=Pn?0:-1,top=0;
    msg[0]=0;
    if(target11&&*target11)name11txt(target11,targettxt,(int)sizeof(targettxt));else targettxt[0]=0;

    for(;;){
        if(Pn<=0){cur=-1;top=0;}
        else{
            if(cur<0)cur=0;
            if(cur>=Pn)cur=Pn-1;
            if(cur<top)top=cur;
            if(cur>=top+EDIT_ROWS)top=cur-EDIT_ROWS+1;
            if(top<0)top=0;
        }

        edit_draw(cur,top,targettxt,msg);
        msg[0]=0;

        int c=term_getchar();
        if(c>='a'&&c<='z')c-=32;

        if(c==0x1B||c=='X')return;
        if(c=='J'||c=='\n'){if(Pn&&cur<Pn-1)cur++;continue;}
        if(c=='K'){if(Pn&&cur>0)cur--;continue;}

        if(c=='D'){
            if(!Pn){edit_setmsg(msg,"Nothing to delete.");continue;}
            int n=(int)P[cur].num;
            storeline(n,"");
            if(cur>=Pn)cur=Pn-1;
            edit_setmsg(msg,"Line deleted.");
            continue;
        }

        if(c=='E'){
            if(!Pn){edit_setmsg(msg,"Nothing to edit.");continue;}
            char prompt[32],linebuf[IBUF];
            bstrcpy(prompt,"EDIT LINE ",(int)sizeof(prompt));
            char nb[16];
            itos((int32_t)P[cur].num,nb,(int)sizeof(nb));
            int lp=bstrlen(prompt),ln=bstrlen(nb);
            for(int i=0;i<ln&&lp+i<(int)sizeof(prompt)-1;i++)prompt[lp+i]=nb[i];
            prompt[lp+ln]=0;
            edit_prompt(prompt,P[cur].txt,linebuf,IBUF);
            if(!linebuf[0]){edit_setmsg(msg,"Edit cancelled.");continue;}
            storeline((int)P[cur].num,linebuf);
            edit_setmsg(msg,"Line updated.");
            continue;
        }

        if(c=='A'){
            char linebuf[IBUF];
            int ni=-1,rc;
            edit_prompt("ADD NUMBERED LINE",0,linebuf,IBUF);
            rc=accept_numbered_line(linebuf,&ni);
            if(rc==0){edit_setmsg(msg,"Add cancelled.");continue;}
            if(rc<0){edit_setmsg(msg,"Need full numbered BASIC line.");continue;}
            cur=ni;
            edit_setmsg(msg,"Line added.");
            continue;
        }

        if(c=='W'){
            if(!(target11&&*target11)){edit_setmsg(msg,"No target file. Use SAVE after exit.");continue;}
            int r=prog_save_name11(target11);
            if(r==-2)edit_setmsg(msg,"DISK NOT READY");
            else if(r<0)edit_setmsg(msg,"WRITE ERROR");
            else edit_setmsg(msg,"File written.");
            continue;
        }

        if(c=='S'){
            if(target11&&*target11){
                int r=prog_save_name11(target11);
                if(r==-2){edit_setmsg(msg,"DISK NOT READY");continue;}
                if(r<0){edit_setmsg(msg,"WRITE ERROR");continue;}
            }
            return;
        }
    }
}

static int maybe_edit(const char *line){
    const char *savepp=pp;
    int saveerr=err;
    char nm11[12];
    int havefile=0;

    pp=line;
    err=0;
    if(!kw("EDIT")){pp=savepp;err=saveerr;return 0;}
    sw();

    if(*pp){
        if(!fat_ready()){pp=savepp;err=0;berr("DISK NOT READY");return 1;}
        Val fn=strexpr();
        if(err){
            pp=savepp;
            err=0;
            return 1;
        }
        toname11(spool[fn.si],nm11);
        sfree(fn.si);
        int r=prog_load_name11(nm11);
        if(r==-2){pp=savepp;err=0;berr("DISK NOT READY");return 1;}
        if(r<0){pp=savepp;err=0;berr("FILE NOT FOUND");return 1;}
        havefile=1;
    }

    pp=savepp;
    err=saveerr;
    run_editor(havefile?nm11:0);
    return 1;
}

/* ── Statement dispatcher ────────────────────────────────────────── */
static void stmt(const char *line);

#include "stmt/core.inc"
#include "stmt/console.inc"
#include "stmt/flow.inc"
#include "stmt/file.inc"
#include "stmt/system.inc"
#include "stmt/graphics.inc"

static int (*const stmt_frag[])(void)={
    stmt_core,stmt_console,stmt_flow,stmt_file,stmt_system,stmt_graphics
};

static void stmt(const char *line){
    pp=line;sw();if(!*pp)return;
    for(unsigned i=0;i<sizeof(stmt_frag)/sizeof(stmt_frag[0]);i++)
        if(stmt_frag[i]())return;
    berr("WHAT?");
}

/* ── REPL ────────────────────────────────────────────────────────── */
void basic_run(void){
    for(;;){
        if(running){
            if(pc>=Pn){running=0;continue;}
            int here=pc++;err=0;stmt(P[here].txt);
            if(running&&pc==here+1)continue;
            err=0;continue;
        }
        term_sync_cursor();
        term_set_color(VGA_LIGHT_GREEN,VGA_BLACK);term_puts("=");
        term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puts("> ");
        term_set_color(VGA_WHITE,VGA_BLACK);term_getline(ibuf,IBUF);upr(ibuf);
        const char *p=ibuf;while(*p==' ')p++;if(!*p)continue;
		klog(p);
        if(*p>='0'&&*p<='9'){
            int n=0;while(*p>='0'&&*p<='9')n=n*10+(*p++-'0');
            if(n<1||n>65535){berr("BAD LINE NUMBER");continue;}
            while(*p==' ')p++;
            storeline(n,p);
            continue;
        }
        err=0;
        if(maybe_edit(p))continue;
        stmt(p);
    }
}
