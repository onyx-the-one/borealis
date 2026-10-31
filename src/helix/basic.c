#include "basic.h"
#include "fs/fat12.h"
#include "sound/sound.h"
#include "rtc/rtc.h"
#include "gfx/gfx.h"
#include "config.h"

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

static uint32_t rng_state=12345;
static double rnd(void){rng_state=rng_state*1664525u+1013904223u;return (double)rng_state/4294967296.0;}

#define TY_INT 0
#define TY_FLOAT 1
#define TY_STR 2
typedef struct{int ty;int32_t i;double f;int si;}Val;

#define NSTRS 128
#define SLEN 256
static char spool[NSTRS][SLEN];
static int sused[NSTRS];
static int snew(void){for(int i=0;i<NSTRS;i++){if(!sused[i]){sused[i]=1;spool[i][0]=0;return i;}}return -1;}
static void sfree(int i){if(i>=0&&i<NSTRS)sused[i]=0;}
static Val vint(int32_t i){Val v;v.ty=TY_INT;v.i=i;v.f=(double)i;v.si=-1;return v;}
static Val vflt(double f){Val v;v.ty=TY_FLOAT;v.f=f;v.i=(int32_t)f;v.si=-1;return v;}
static Val vstr(int si){Val v;v.ty=TY_STR;v.si=si;v.i=0;v.f=0;return v;}
static int32_t toint(Val v){return v.ty==TY_FLOAT?(int32_t)v.f:v.i;}
static Val vbitand(Val a,Val b){return vint(toint(a)&toint(b));}
static Val vbitor(Val a,Val b){return vint(toint(a)|toint(b));}
static Val vbitxor(Val a,Val b){return vint(toint(a)^toint(b));}
static Val vbitnot(Val a){return vint(~toint(a));}
static Val vshl(Val a,Val b){return vint((int32_t)((uint32_t)toint(a)<<(toint(b)&31)));}
static Val vshr(Val a,Val b){return vint((int32_t)((uint32_t)toint(a)>>(toint(b)&31)));}

#define NUMVAL(v) ((v).ty==TY_FLOAT?(v).f:(double)(v).i)
static Val vadd(Val a,Val b){if(a.ty==TY_STR||b.ty==TY_STR)return vint(0);if(a.ty==TY_FLOAT||b.ty==TY_FLOAT)return vflt(NUMVAL(a)+NUMVAL(b));return vint(a.i+b.i);}
static Val vsub(Val a,Val b){if(a.ty==TY_STR||b.ty==TY_STR)return vint(0);if(a.ty==TY_FLOAT||b.ty==TY_FLOAT)return vflt(NUMVAL(a)-NUMVAL(b));return vint(a.i-b.i);}
static Val vmul(Val a,Val b){if(a.ty==TY_STR||b.ty==TY_STR)return vint(0);if(a.ty==TY_FLOAT||b.ty==TY_FLOAT)return vflt(NUMVAL(a)*NUMVAL(b));return vint(a.i*b.i);}
static Val vdiv(Val a,Val b){return vflt(NUMVAL(a)/NUMVAL(b));}
static Val vmod(Val a,Val b){if(!b.i&&b.ty==TY_INT)return vint(0);return vint(a.i%b.i);}
static int vcmp(Val a,Val b){if(a.ty==TY_STR&&b.ty==TY_STR){const char *p=spool[a.si],*q=spool[b.si];while(*p&&*p==*q){p++;q++;}return *p-*q;}double d=NUMVAL(a)-NUMVAL(b);return d>0?1:d<0?-1:0;}
static void vprint(Val v){if(v.ty==TY_STR){term_puts(spool[v.si]);return;}if(v.ty==TY_INT)term_puti(v.i);else term_putf(v.f);}

#define MAXVARS 256
#define NAMELEN 32
typedef struct{char name[NAMELEN];Val val;int isstr;}Var;
static Var vars[MAXVARS];
static int nvars=0;

#define MAXARRAYS 32
#define MAXELEMS 512
typedef struct{char name[NAMELEN];int isstr;int dims;int size[3];int base;int total;}Array;
static Array arrays[MAXARRAYS];
static int narrays=0;
static Val aelems[MAXELEMS];
static int aused=0;

#define MAXFILES 4
#define FBUFSZ 8192
typedef struct{int open;int forwrite;char nm11[12];char buf[FBUFSZ];int len;int pos;int dirty;}FileSlot;
static FileSlot files[MAXFILES];

static int bstreq(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return !*a&&!*b;}
static void bstrcpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static void upr(char *s){while(*s){if(*s>='a'&&*s<='z')*s-=32;s++;}}
static int bstrlen(const char *s){int n=0;while(s[n])n++;return n;}

static void itos(int32_t iv,char *out,int max){
    int neg=0,i=0;
    if(iv<0){neg=1;iv=-iv;}
    char tmp[12];int ti=0;
    if(!iv)tmp[ti++]='0';
    else{int32_t t=iv;while(t){tmp[ti++]='0'+t%10;t/=10;}}
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
    if(!ip)tmp[ti++]='0';
    else{int32_t t=ip;while(t){tmp[ti++]='0'+t%10;t/=10;}}
    if(neg&&i<max-2)out[i++]='-';
    while(ti>0&&i<max-1)out[i++]=tmp[--ti];
    char fb[10];int fi=0;
    for(int k=0;k<6;k++){fp*=10.0;int d=(int)fp;fb[fi++]='0'+d;fp-=d;}
    while(fi>1&&fb[fi-1]=='0')fi--;
    if(fi>0&&i<max-2)out[i++]='.';
    for(int k=0;k<fi&&i<max-1;k++)out[i++]=fb[k];
    out[i]=0;
}

static Var *varget(const char *nm){for(int i=0;i<nvars;i++){if(bstreq(vars[i].name,nm))return &vars[i];}return 0;}
static Var *varset(const char *nm,Val v){
    Var *vp=varget(nm);
    if(vp){if(vp->val.ty==TY_STR&&v.ty!=TY_STR)sfree(vp->val.si);vp->val=v;return vp;}
    if(nvars>=MAXVARS)return 0;
    Var *nv=&vars[nvars++];
    int i=0;while(nm[i]&&i<NAMELEN-1){nv->name[i]=nm[i];i++;}
    nv->name[i]=0;
    nv->isstr=nm[i-1]=='$';
    nv->val=v;
    return nv;
}

#define MAXLINES 1024
#define LINELEN 128
typedef struct{uint16_t num;char txt[LINELEN];}Line;
static Line P[MAXLINES];
static int Pn=0;

static char fbuf[MAXLINES*LINELEN];

#define CALLDEPTH 64
#define FORDEPTH 32
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
/* kw is not a tokenizer: on a match it consumes following spaces, and a
   trailing '$' counts as part of the word, so a statement keyword can never
   swallow a string variable like TIME$ or PRINT$ as its prefix. */
static int kw(const char *k){
    const char *p0=pp;
    while(*k){
        char a=*pp,b=*k;
        if(a>='a'&&a<='z')a-=32;
        if(a!=b){pp=p0;return 0;}
        pp++;k++;
    }
    char nx=*pp;
    if((nx>='A'&&nx<='Z')||(nx>='a'&&nx<='z')||(nx>='0'&&nx<='9')||nx=='_'||nx=='$'){pp=p0;return 0;}
    while(*pp==' ')pp++;
    return 1;
}
static int readname(char *out,int max){
    sw();
    char c=*pp;
    if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')))return 0;
    int i=0;
    while((*pp>='A'&&*pp<='Z')||(*pp>='a'&&*pp<='z')||(*pp>='0'&&*pp<='9')||*pp=='_'||*pp=='$'){
        char ch=*pp++;
        if(ch>='a'&&ch<='z')ch-=32;
        if(i<max-2)out[i++]=ch;
    }
    out[i]=0;
    return i>0;
}
static int isstrname(const char *nm){int i=0;while(nm[i])i++;return i>0&&nm[i-1]=='$';}

static Val expr(void);
static Val strexpr(void);
static void stmt(const char *line);

static int pnum(double *o){
    sw();
    if(*pp=='&'&&(pp[1]=='H'||pp[1]=='h')){
        pp+=2;uint32_t v=0;int got=0;
        for(;;){
            char c=*pp;int d=-1;
            if(c>='0'&&c<='9')d=c-'0';
            else if(c>='A'&&c<='F')d=10+(c-'A');
            else if(c>='a'&&c<='f')d=10+(c-'a');
            else break;
            v=(v<<4)|(uint32_t)d;pp++;got=1;
        }
        if(!got)return 0;
        *o=(double)(int32_t)v;
        return 1;
    }
    if(*pp=='&'&&(pp[1]=='B'||pp[1]=='b')){
        pp+=2;uint32_t v=0;int got=0;
        while(*pp=='0'||*pp=='1'){v=(v<<1)|(uint32_t)(*pp++-'0');got=1;}
        if(!got)return 0;
        *o=(double)(int32_t)v;
        return 1;
    }
    if(!((*pp>='0'&&*pp<='9')||(*pp=='.'&&pp[1]>='0'&&pp[1]<='9')))return 0;
    double v=0;int fdiv=1,frac=0;
    while(*pp>='0'&&*pp<='9')v=v*10+(*pp++-'0');
    if(*pp=='.'){
        pp++;
        while(*pp>='0'&&*pp<='9'){v=v*10+(*pp++-'0');fdiv*=10;frac=1;}
    }
    if(frac)v/=fdiv;
    if(*pp=='E'||*pp=='e'){
        pp++;int neg=0,ex=0;
        if(*pp=='-'){neg=1;pp++;}
        else if(*pp=='+')pp++;
        while(*pp>='0'&&*pp<='9')ex=ex*10+(*pp++-'0');
        double m=1;
        for(int i=0;i<ex;i++)m*=10.0;
        if(neg)v/=m;else v*=m;
    }
    *o=v;
    return 1;
}

static Array *arrget(const char *nm){for(int i=0;i<narrays;i++){if(bstreq(arrays[i].name,nm))return &arrays[i];}return 0;}
static int arridx(Array *a,int i1,int i2,int i3){
    if(a->dims==1){
        if(i1<0||i1>a->size[0]){berr("SUBSCRIPT");return -1;}
        return a->base+i1;
    }
    if(a->dims==2){
        if(i1<0||i1>a->size[0]||i2<0||i2>a->size[1]){berr("SUBSCRIPT");return -1;}
        return a->base+(i1*(a->size[1]+1))+i2;
    }
    if(i1<0||i1>a->size[0]||i2<0||i2>a->size[1]||i3<0||i3>a->size[2]){berr("SUBSCRIPT");return -1;}
    return a->base+(i1*(a->size[1]+1)*(a->size[2]+1))+(i2*(a->size[2]+1))+i3;
}

static Val callstrfn(const char *nm){
    sw();pp++;
    if(bstreq(nm,"LEFT$")){
        Val s=strexpr();if(err)return vstr(-1);
        sw();if(*pp!=','){berr("SYNTAX");return vstr(-1);}pp++;
        Val n=expr();if(err)return vstr(-1);
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        const char *src=spool[s.si];
        int i=0;
        while(src[i]&&i<n.i&&i<SLEN-1){spool[r][i]=src[i];i++;}
        spool[r][i]=0;
        if(s.si>=0&&s.ty==TY_STR)sfree(s.si);
        return vstr(r);
    }
    if(bstreq(nm,"RIGHT$")){
        Val s=strexpr();if(err)return vstr(-1);
        sw();if(*pp!=','){berr("SYNTAX");return vstr(-1);}pp++;
        Val n=expr();if(err)return vstr(-1);
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        const char *src=spool[s.si];
        int l=bstrlen(src),st=l-n.i;
        if(st<0)st=0;
        int i=0;
        while(src[st+i]&&i<SLEN-1){spool[r][i]=src[st+i];i++;}
        spool[r][i]=0;
        if(s.si>=0&&s.ty==TY_STR)sfree(s.si);
        return vstr(r);
    }
    if(bstreq(nm,"MID$")){
        Val s=strexpr();if(err)return vstr(-1);
        sw();if(*pp!=','){berr("SYNTAX");return vstr(-1);}pp++;
        Val start=expr();if(err)return vstr(-1);
        int cnt=-1;
        sw();
        if(*pp==','){pp++;Val n2=expr();if(err)return vstr(-1);cnt=n2.i;}
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        const char *src=spool[s.si];
        int st=start.i-1;
        if(st<0)st=0;
        src+=st;
        int i=0;
        while(*src&&(cnt<0||i<cnt)&&i<SLEN-1){spool[r][i++]=*src++;}
        spool[r][i]=0;
        if(s.si>=0&&s.ty==TY_STR)sfree(s.si);
        return vstr(r);
    }
    if(bstreq(nm,"STR$")){
        Val n=expr();if(err)return vstr(-1);
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        if(n.ty==TY_FLOAT)ftos(n.f,spool[r],SLEN);
        else itos(n.i,spool[r],SLEN);
        return vstr(r);
    }
    if(bstreq(nm,"CHR$")){
        Val n=expr();if(err)return vstr(-1);
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        spool[r][0]=(char)n.i;spool[r][1]=0;
        return vstr(r);
    }
    if(bstreq(nm,"VAL")){
        Val s=strexpr();if(err)return vint(0);
        sw();if(*pp!=')'){berr("SYNTAX");return vint(0);}pp++;
        const char *p=spool[s.si];
        int neg=0;double n=0;int fdiv=1,frac=0;
        if(*p=='-'){neg=1;p++;}
        while(*p>='0'&&*p<='9')n=n*10+(*p++-'0');
        if(*p=='.'){p++;while(*p>='0'&&*p<='9'){n=n*10+(*p++-'0');fdiv*=10;frac=1;}}
        if(frac)n/=fdiv;
        if(neg)n=-n;
        if(s.ty==TY_STR)sfree(s.si);
        return n!=(double)(int32_t)n?vflt(n):vint((int32_t)n);
    }
    if(bstreq(nm,"ASC")){
        Val s=strexpr();if(err)return vint(0);
        sw();if(*pp!=')'){berr("SYNTAX");return vint(0);}pp++;
        int c=(unsigned char)spool[s.si][0];
        if(s.ty==TY_STR)sfree(s.si);
        return vint(c);
    }
    if(bstreq(nm,"HEX$")){
        Val n=expr();if(err)return vstr(-1);
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        uint32_t v=(uint32_t)toint(n);
        char *d=spool[r];
        int i=0,start=0;
        for(int s=28;s>=0;s-=4){
            char c="0123456789ABCDEF"[(v>>s)&0xF];
            if(c!='0'||start||s==0){d[i++]=c;start=1;}
        }
        d[i]=0;
        return vstr(r);
    }
    if(bstreq(nm,"BIN$")){
        Val n=expr();if(err)return vstr(-1);
        sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        uint32_t v=(uint32_t)toint(n);
        char *d=spool[r];
        int i=0,start=0;
        for(int s=31;s>=0;s--){
            char c=((v>>s)&1)?'1':'0';
            if(c!='0'||start||s==0){d[i++]=c;start=1;}
        }
        d[i]=0;
        return vstr(r);
    }
    if(bstreq(nm,"DATE$")){
        pp--;
        if(*pp=='('){pp++;sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;}
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        RtcTime rt;
        if(rtc_read(&rt)<0){berr("RTC");return vstr(-1);}
        rtc_format_date(&rt,spool[r],SLEN);
        return vstr(r);
    }
    if(bstreq(nm,"TIME$")){
        pp--;
        if(*pp=='('){pp++;sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;}
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        RtcTime rt;
        if(rtc_read(&rt)<0){berr("RTC");return vstr(-1);}
        rtc_format_time(&rt,spool[r],SLEN);
        return vstr(r);
    }
    if(bstreq(nm,"INKEY$")){
        pp--;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        int c=term_peekkey();
        spool[r][0]=c?(char)c:0;spool[r][1]=0;
        return vstr(r);
    }
    berr("UNKNOWN FUNCTION");
    return vstr(-1);
}

static Val numfn(const char *nm){
    sw();pp++;
    Val a=expr();if(err)return vint(0);
    Val second=vint(0);int hassecond=0;
    sw();
    if(*pp==','){pp++;second=expr();if(err)return vint(0);hassecond=1;}
    sw();if(*pp!=')'){berr("SYNTAX");return vint(0);}pp++;
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
    berr("UNKNOWN FUNCTION");
    return vint(0);
}

static Val strexpr(void){
    sw();
    Val a;
    if(*pp=='"'){
        pp++;
        int r=snew();if(r<0){berr("MEM");return vstr(-1);}
        int i=0;
        while(*pp&&*pp!='"'&&i<SLEN-1)spool[r][i++]=*pp++;
        spool[r][i]=0;
        if(*pp=='"')pp++;
        a=vstr(r);
    }else{
        const char *save=pp;
        char nm[NAMELEN];
        if(readname(nm,NAMELEN)&&isstrname(nm)){
            sw();
            if(*pp=='('){
                Array *ar=arrget(nm);
                if(ar){
                    pp++;Val i1=expr();if(err)return vstr(-1);
                    Val i2=vint(0),i3=vint(0);
                    if(*pp==','){pp++;i2=expr();if(err)return vstr(-1);}
                    if(*pp==','){pp++;i3=expr();if(err)return vstr(-1);}
                    sw();if(*pp!=')'){berr("SYNTAX");return vstr(-1);}pp++;
                    int idx=arridx(ar,i1.i,i2.i,i3.i);
                    if(idx<0)return vstr(-1);
                    int r=snew();if(r<0){berr("MEM");return vstr(-1);}
                    if(aelems[idx].ty==TY_STR&&aelems[idx].si>=0)bstrcpy(spool[r],spool[aelems[idx].si],SLEN);
                    else spool[r][0]=0;
                    a=vstr(r);
                }else a=callstrfn(nm);
            }else{
                Var *vp=varget(nm);
                int r=snew();if(r<0){berr("MEM");return vstr(-1);}
                if(vp&&vp->val.ty==TY_STR)bstrcpy(spool[r],spool[vp->val.si],SLEN);
                else spool[r][0]=0;
                a=vstr(r);
            }
        }else{pp=save;berr("TYPE MISMATCH");return vstr(-1);}
    }
    for(;;){
        sw();
        if(*pp!='+')break;
        pp++;
        Val b=strexpr();
        if(err)break;
        int r=snew();
        if(r<0){berr("MEM");sfree(a.si);sfree(b.si);return vstr(-1);}
        int la=bstrlen(spool[a.si]),lb=bstrlen(spool[b.si]);
        int n=la+lb<SLEN-1?la+lb:SLEN-1;
        for(int j=0;j<la&&j<SLEN-1;j++)spool[r][j]=spool[a.si][j];
        for(int j=0;j<lb&&la+j<SLEN-1;j++)spool[r][la+j]=spool[b.si][j];
        spool[r][n]=0;
        sfree(a.si);sfree(b.si);
        a=vstr(r);
    }
    return a;
}

static Val fact(void){
    sw();
    if(err)return vint(0);
    if(*pp=='('){pp++;Val v=expr();sw();if(*pp!=')')berr("SYNTAX");else pp++;return v;}
    if(*pp=='-'){pp++;Val v=fact();v.f=-v.f;v.i=-v.i;return v;}
    if(*pp=='+')return fact();
    double n;
    if(pnum(&n))return n!=(double)(int32_t)n?vflt(n):vint((int32_t)n);
    char nm[NAMELEN];const char *save=pp;
    if(readname(nm,NAMELEN)){
        sw();
        if(bstreq(nm,"INKEY")){
            if(*pp=='('){
                pp++;
                int r=snew();if(r<0){berr("MEM");return vint(0);}
                int c=term_peekkey();
                spool[r][0]=c?(char)c:0;spool[r][1]=0;
                if(*pp==')')pp++;
                return vstr(r);
            }
        }
        if(isstrname(nm)){pp=save;return strexpr();}
        if(bstreq(nm,"LEN")){
            sw();if(*pp!='('){berr("SYNTAX");return vint(0);}pp++;
            Val s=strexpr();if(err)return vint(0);
            sw();if(*pp!=')')berr("SYNTAX");else pp++;
            int l=bstrlen(spool[s.si]);
            if(s.ty==TY_STR)sfree(s.si);
            return vint(l);
        }
        if(bstreq(nm,"PEEK")){
            sw();if(*pp!='('){berr("SYNTAX");return vint(0);}pp++;
            Val a=expr();if(err)return vint(0);
            sw();if(*pp!=')')berr("SYNTAX");else pp++;
            uint8_t *p=(uint8_t*)(uint32_t)a.i;
            return vint((int32_t)*p);
        }
        if(*pp=='('){
            Array *ar=arrget(nm);
            if(ar){
                pp++;
                Val i1=expr();if(err)return vint(0);
                Val i2=vint(0),i3=vint(0);
                if(*pp==','){pp++;i2=expr();if(err)return vint(0);}
                if(*pp==','){pp++;i3=expr();if(err)return vint(0);}
                sw();if(*pp!=')')berr("SYNTAX");else pp++;
                int idx=arridx(ar,i1.i,i2.i,i3.i);
                if(idx<0)return vint(0);
                Val el=aelems[idx];
                if(el.ty==TY_STR){
                    int r=snew();if(r<0){berr("MEM");return vint(0);}
                    bstrcpy(spool[r],el.si>=0?spool[el.si]:"",SLEN);
                    return vstr(r);
                }
                return el;
            }
            return numfn(nm);
        }
        if(bstreq(nm,"RND"))return vflt(rnd());
        Var *vp=varget(nm);
        return vp?vp->val:vint(0);
    }
    pp=save;berr("SYNTAX");
    return vint(0);
}

static Val termexpr(void){
    Val a=fact();
    while(!err){
        sw();
        if(*pp=='*'){pp++;a=vmul(a,fact());}
        else if(*pp=='/'){pp++;Val b=fact();if(b.ty==TY_INT&&b.i==0){berr("DIV0");return vint(0);}a=vdiv(a,b);}
        else if(*pp=='\\'){pp++;Val b=fact();if(!b.i&&b.ty==TY_INT){berr("DIV0");return vint(0);}a=vint(a.i/b.i);}
        else break;
    }
    return a;
}
static Val expr(void){
    Val a=termexpr();
    while(!err){
        sw();
        if(*pp=='+'){pp++;a=vadd(a,termexpr());}
        else if(*pp=='-'){pp++;a=vsub(a,termexpr());}
        else break;
    }
    return a;
}
static int relop(void){
    sw();
    if(*pp=='<'){pp++;if(*pp=='='){pp++;return 4;}if(*pp=='>'){pp++;return 6;}return 3;}
    if(*pp=='>'){pp++;if(*pp=='='){pp++;return 5;}return 2;}
    if(*pp=='='){pp++;return 1;}
    return 0;
}
static int applyrel(int op,Val a,Val b){
    int c=vcmp(a,b);
    switch(op){
    case 1:return c==0;
    case 2:return c>0;
    case 3:return c<0;
    case 4:return c<=0;
    case 5:return c>=0;
    case 6:return c!=0;
    }
    return 0;
}

static int findge(int n){for(int i=0;i<Pn;i++){if((int)P[i].num>=n)return i;}return Pn;}

static void storeline(int n,const char *t){
    int lo=0,hi=Pn,idx=0,found=0;
    while(lo<hi){
        int m=(lo+hi)/2;
        if((int)P[m].num==n){idx=m;found=1;break;}
        if((int)P[m].num<n)lo=m+1;
        else hi=m;
        idx=lo;
    }
    if(!found)idx=lo;
    if(found){
        if(!t||!*t){for(int i=idx;i<Pn-1;i++)P[i]=P[i+1];Pn--;return;}
        bstrcpy(P[idx].txt,t,LINELEN);
        return;
    }
    if(!t||!*t)return;
    if(Pn>=MAXLINES){berr("MEM");return;}
    for(int i=Pn;i>idx;i--)P[i]=P[i-1];
    Pn++;
    P[idx].num=(uint16_t)n;
    bstrcpy(P[idx].txt,t,LINELEN);
}

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
    for(int i=7;i>=0;i--){if(name11[i]!=' '){nlen=i+1;break;}}
    for(int i=10;i>=8;i--){if(name11[i]!=' '){elen=i-8+1;break;}}
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
        while(*p==' ')p++;
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

static void dircb(const char *name11,uint32_t size){
    char out[13];int oi=0;
    int nlen=0;
    for(int i=7;i>=0;i--){if(name11[i]!=' '){nlen=i+1;break;}}
    for(int i=0;i<nlen;i++)out[oi++]=name11[i];
    int elen=0;
    for(int i=10;i>=8;i--){if(name11[i]!=' '){elen=i-8+1;break;}}
    if(elen){
        out[oi++]='.';
        for(int i=0;i<elen;i++)out[oi++]=name11[8+i];
    }
    out[oi]=0;
    term_puts(" ");
    term_puts(out);
    int pad=13-oi;
    while(pad-->0)term_putchar(' ');
    if(size>0)term_putchar(' ');
    term_puti((int32_t)size);
    term_puts(" bytes\n");
}

#define EDIT_ROWS 22

static void edit_setmsg(char *dst,const char *src){bstrcpy(dst,src,80);}

static void edit_draw(int cur,int top,const char *target,const char *msg){
    term_clear();
    term_set_color(VGA_CYAN,VGA_BLACK);
    term_puts("BOREALIS EDIT");
    if(target&&*target){term_puts(" ");term_puts(target);}
    term_putchar('\n');
    term_set_color(VGA_DARK_GREY,VGA_BLACK);
    if(target&&*target)term_puts("J/K move E edit A add D del W write S save+exit X exit\n");
    else term_puts("J/K move E edit A add D del X exit\n");
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
            term_puts(" ");
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
    if(target11&&*target11)name11txt(target11,targettxt,(int)sizeof(targettxt));
    else targettxt[0]=0;
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
            for(int i=0;i<ln&&lp<(int)sizeof(prompt)-1;i++)prompt[lp++]=nb[i];
            prompt[lp]=0;
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
            if(!target11||!*target11){edit_setmsg(msg,"No target file. Use SAVE after exit.");continue;}
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
    const char *save_pp=pp;int save_err=err;
    char nm11[12];int havefile=0;
    pp=line;err=0;
    if(!kw("EDIT")){pp=save_pp;err=save_err;return 0;}
    sw();
    if(*pp){
        if(!fat_ready()){pp=save_pp;err=0;berr("DISK NOT READY");return 1;}
        Val fn=strexpr();
        if(err){pp=save_pp;err=0;return 1;}
        toname11(spool[fn.si],nm11);sfree(fn.si);
        int r=prog_load_name11(nm11);
        if(r==-2){pp=save_pp;err=0;berr("DISK NOT READY");return 1;}
        if(r<0){pp=save_pp;err=0;berr("FILE NOT FOUND");return 1;}
        havefile=1;
    }
    pp=save_pp;err=save_err;
    run_editor(havefile?nm11:0);
    return 1;
}

/* Shared by LET and bare assignment. 1 = handled, 0 = not an assignment
   (caller decides between berr SYNTAX and falling through). */
static int do_assign(const char *nm){
    sw();
    if(*pp=='('){
        Array *ar=arrget(nm);
        if(!ar)return 0;
        pp++;Val i1=expr();if(err)return 1;
        Val i2=vint(0),i3=vint(0);
        if(*pp==','){pp++;i2=expr();if(err)return 1;}
        if(*pp==','){pp++;i3=expr();if(err)return 1;}
        sw();if(*pp!=')'){berr("SYNTAX");return 1;}pp++;
        sw();if(*pp!='='){berr("SYNTAX");return 1;}pp++;
        int idx=arridx(ar,i1.i,i2.i,i3.i);if(idx<0)return 1;
        if(isstrname(nm)){Val v=strexpr();if(!err){if(aelems[idx].ty==TY_STR&&aelems[idx].si>=0)sfree(aelems[idx].si);aelems[idx]=v;}}
        else{Val v=expr();if(!err)aelems[idx]=v;}
        return 1;
    }
    if(*pp!='=')return 0;
    pp++;
    if(isstrname(nm)){Val v=strexpr();if(!err)varset(nm,v);}
    else{Val v=expr();if(!err)varset(nm,v);}
    return 1;
}

/* Statement dispatch. Order is semantic (ABI):
   core (incl. LET) -> console -> flow -> file -> system -> graphics ->
   bare assignment (last: matches any name) -> ?WHAT? fallback, owned
   here and nowhere else. Any handler that starts with a keyword must
   restore pp on a failed match; bare assignment relies on that. */
static void stmt(const char *line){
    pp=line;
    sw();
    if(!*pp)return;

    if(kw("REM")||*pp=='\'')return;
    if(kw("KPAN")){kpanic("KPAN");return;}
    if(kw("CLEAR")){draw_banner();return;}
    if(kw("HALT")){term_set_color(VGA_YELLOW,VGA_BLACK);term_puts(" halted.\n");__asm__ __volatile__("cli");for(;;)__asm__ __volatile__("hlt");}
    if(kw("NEW")){Pn=0;nvars=0;csp=0;fsp=0;wsp=0;dataline=0;datacol=0;narrays=0;aused=0;for(int i=0;i<NSTRS;i++)sused[i]=0;for(int i=0;i<MAXFILES;i++)files[i].open=0;return;}
    if(kw("END")){running=0;return;}
    if(kw("LIST")){for(int i=0;i<Pn;i++){term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puti(P[i].num);term_putchar(' ');term_set_color(VGA_LIGHT_GREY,VGA_BLACK);term_puts(P[i].txt);term_putchar('\n');}return;}
    if(kw("RUN")){pc=0;running=1;csp=0;fsp=0;wsp=0;dataline=0;datacol=0;narrays=0;aused=0;for(int i=0;i<NSTRS;i++)sused[i]=0;nvars=0;for(int i=0;i<MAXFILES;i++)files[i].open=0;return;}

    if(kw("DIM")){
        for(;;){
            sw();char nm[NAMELEN];if(!readname(nm,NAMELEN)){berr("SYNTAX");return;}sw();if(*pp!='('){berr("SYNTAX");return;}pp++;
            Val d1=expr();if(err)return;
            Val d2=vint(-1),d3=vint(-1);
            if(*pp==','){pp++;d2=expr();if(err)return;}
            if(*pp==','){pp++;d3=expr();if(err)return;}
            sw();if(*pp!=')'){berr("SYNTAX");return;}pp++;
            if(narrays>=MAXARRAYS){berr("MEM");return;}
            Array *a=&arrays[narrays];
            bstrcpy(a->name,nm,NAMELEN);
            a->isstr=isstrname(nm);
            a->dims=d3.i>=0?3:d2.i>=0?2:1;
            a->size[0]=d1.i;a->size[1]=d2.i;a->size[2]=d3.i;
            int total;
            if(a->dims==1)total=d1.i+1;
            else if(a->dims==2)total=(d1.i+1)*(d2.i+1);
            else total=(d1.i+1)*(d2.i+1)*(d3.i+1);
            if(aused+total>MAXELEMS){berr("MEM");return;}
            a->base=aused;a->total=total;aused+=total;
            for(int i=a->base;i<a->base+total;i++)aelems[i]=vint(0);
            narrays++;
            sw();if(*pp!=',')break;pp++;
        }
        return;
    }

    if(kw("POKE")){
        Val addr=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val val=expr();if(err)return;
        *(uint8_t*)(uint32_t)addr.i=(uint8_t)val.i;
        return;
    }

    if(kw("LET")){
        char nm[NAMELEN];
        if(!readname(nm,NAMELEN)){berr("SYNTAX");return;}
        if(!do_assign(nm))berr("SYNTAX");
        return;
    }

    if(kw("PRINT")||*pp=='?'){
        if(*pp=='?')pp++;
        sw();
        if(*pp=='#'){
            pp++;Val fno=expr();if(err)return;sw();if(*pp==',')pp++;
            int fi=(int)fno.i-1;
            if(fi<0||fi>=MAXFILES||!files[fi].open||!files[fi].forwrite){berr("FILE NOT OPEN");return;}
            FileSlot *fs=&files[fi];int nlf=1;
            for(;;){
                sw();if(!*pp)break;
                int issf=0;const char *s2f=pp;
                if(*pp=='"')issf=1;
                else{char nmf[NAMELEN];if(readname(nmf,NAMELEN)&&isstrname(nmf))issf=1;pp=s2f;}
                char tmpf[SLEN];tmpf[0]=0;
                if(issf){Val v=strexpr();if(!err){bstrcpy(tmpf,spool[v.si],SLEN);sfree(v.si);}}
                else{Val v=expr();if(!err){if(v.ty==TY_STR){bstrcpy(tmpf,spool[v.si],SLEN);}else if(v.ty==TY_INT)itos(v.i,tmpf,(int)sizeof(tmpf));else ftos(v.f,tmpf,(int)sizeof(tmpf));}}
                int tlf=bstrlen(tmpf);
                if(fs->len+tlf<FBUFSZ-2){for(int k=0;k<tlf;k++)fs->buf[fs->len++]=tmpf[k];fs->dirty=1;}
                sw();nlf=1;
                if(*pp==';'){pp++;nlf=0;}
                else if(*pp==','){pp++;if(fs->len<FBUFSZ-2){fs->buf[fs->len++]='\t';fs->dirty=1;}nlf=0;}
                else break;
            }
            if(nlf&&fs->len<FBUFSZ-2){fs->buf[fs->len++]='\n';fs->dirty=1;}
            return;
        }
        int nl=1;
        if(!*pp){term_putchar('\n');return;}
        for(;;){
            sw();if(!*pp||*pp=='\n')break;
            int iss=0;const char *s2=pp;
            if(*pp=='"')iss=1;
            else{
                char nm2[NAMELEN];
                if(readname(nm2,NAMELEN)&&isstrname(nm2))iss=1;
                else if(bstreq(nm2,"INKEY"))iss=1;
                pp=s2;
            }
            if(iss){Val v=strexpr();if(!err){term_puts(spool[v.si]);sfree(v.si);}}
            else{Val v=expr();if(!err)vprint(v);}
            if(err)return;
            sw();nl=1;
            if(*pp==';'){pp++;nl=0;}
            else if(*pp==','){pp++;term_putchar('\t');nl=0;}
            else break;
        }
        if(nl&&!err)term_putchar('\n');
        return;
    }

    if(kw("INPUT")){
        sw();
        if(*pp=='#'){
            pp++;Val fno=expr();if(err)return;sw();if(*pp==',')pp++;
            int fi=(int)fno.i-1;
            if(fi<0||fi>=MAXFILES||!files[fi].open||files[fi].forwrite){berr("FILE NOT OPEN");return;}
            FileSlot *fs=&files[fi];
            for(;;){
                char nm9[NAMELEN];if(!readname(nm9,NAMELEN)){berr("SYNTAX");return;}
                while(fs->pos<fs->len&&(fs->buf[fs->pos]=='\n'||fs->buf[fs->pos]=='\r'))fs->pos++;
                if(isstrname(nm9)){
                    int r=snew();if(r<0){berr("MEM");return;}
                    int k=0;
                    if(fs->pos<fs->len&&fs->buf[fs->pos]=='"'){
                        fs->pos++;
                        while(fs->pos<fs->len&&fs->buf[fs->pos]!='"'&&k<SLEN-1)spool[r][k++]=fs->buf[fs->pos++];
                        if(fs->pos<fs->len&&fs->buf[fs->pos]=='"')fs->pos++;
                    }else{
                        while(fs->pos<fs->len&&fs->buf[fs->pos]!=','&&fs->buf[fs->pos]!='\n'&&k<SLEN-1)spool[r][k++]=fs->buf[fs->pos++];
                    }
                    spool[r][k]=0;
                    varset(nm9,vstr(r));
                }else{
                    char tmp2[64];int k=0;
                    while(fs->pos<fs->len&&fs->buf[fs->pos]!=','&&fs->buf[fs->pos]!='\n'&&k<63)tmp2[k++]=fs->buf[fs->pos++];
                    tmp2[k]=0;
                    const char *sv=pp;pp=tmp2;double n2=0;pnum(&n2);pp=sv;
                    varset(nm9,n2!=(double)(int32_t)n2?vflt(n2):vint((int32_t)n2));
                }
                if(fs->pos<fs->len&&fs->buf[fs->pos]==',')fs->pos++;
                sw();if(*pp!=',')break;pp++;
            }
            return;
        }
        if(*pp=='"'){pp++;while(*pp&&*pp!='"')term_putchar(*pp++);if(*pp=='"')pp++;sw();if(*pp==','||*pp==';')pp++;else{term_putchar('?');term_putchar(' ');}}
        else{term_putchar('?');term_putchar(' ');}
        for(;;){
            char nm3[NAMELEN];if(!readname(nm3,NAMELEN)){berr("SYNTAX");return;}
            char lb[IBUF];
            term_getline(lb,IBUF);
            if(isstrname(nm3)){int r=snew();if(r<0){berr("MEM");return;}bstrcpy(spool[r],lb,SLEN);varset(nm3,vstr(r));}
            else{upr(lb);const char *sv=pp;pp=lb;double n=0;pnum(&n);pp=sv;varset(nm3,n!=(double)(int32_t)n?vflt(n):vint((int32_t)n));}
            sw();if(*pp!=',')break;pp++;term_puts("? ");
        }
        return;
    }

    if(kw("DATA"))return;

    if(kw("READ")){
        for(;;){
            char nm4[NAMELEN];if(!readname(nm4,NAMELEN)){berr("SYNTAX");return;}
            int found=0;
            for(int i=dataline;i<Pn&&!found;i++){
                const char *dp=P[i].txt;
                const char *save2=pp;pp=dp;
                if(kw("DATA")){
                    int col=0;
                    while(*pp&&col<datacol){while(*pp&&*pp!=',')pp++;if(*pp==','){pp++;col++;}}
                    if(*pp){
                        if(isstrname(nm4)){
                            int r=snew();if(r<0){berr("MEM");pp=save2;return;}
                            int k=0;
                            if(*pp=='"'){pp++;while(*pp&&*pp!='"'&&k<SLEN-1)spool[r][k++]=*pp++;if(*pp=='"')pp++;}
                            else{while(*pp&&*pp!=','&&k<SLEN-1)spool[r][k++]=*pp++;}
                            spool[r][k]=0;
                            varset(nm4,vstr(r));
                        }else{
                            double n=0;pnum(&n);
                            varset(nm4,n!=(double)(int32_t)n?vflt(n):vint((int32_t)n));
                        }
                        dataline=i;datacol++;found=1;
                    }
                    pp=save2;
                }else pp=save2;
            }
            if(!found){berr("OUT OF DATA");return;}
            sw();if(*pp!=',')break;pp++;
        }
        return;
    }

    if(kw("RESTORE")){dataline=0;datacol=0;return;}

    if(kw("GOTO")){sw();Val n=expr();if(err)return;int i=findge((int)n.i);if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return;}pc=i;return;}

    if(kw("GOSUB")){sw();Val n=expr();if(err)return;int i=findge((int)n.i);if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return;}if(csp>=CALLDEPTH){berr("GOSUB OVERFLOW");return;}cstk[csp++]=pc;pc=i;return;}

    if(kw("RETURN")){if(csp<=0){berr("RETURN WITHOUT GOSUB");return;}pc=cstk[--csp];return;}

    if(kw("FOR")){
        sw();char nm5[NAMELEN];if(!readname(nm5,NAMELEN)){berr("SYNTAX");return;}
        sw();if(*pp!='='){berr("SYNTAX");return;}pp++;
        Val from=expr();if(err)return;
        sw();if(!kw("TO")){berr("SYNTAX");return;}
        sw();Val to=expr();if(err)return;
        Val step=vint(1);
        sw();if(kw("STEP")){sw();step=expr();if(err)return;}
        if(!step.i&&!step.f){berr("STEP 0");return;}
        varset(nm5,from);
        int go=step.f>=0?vcmp(from,to)<=0:vcmp(from,to)>=0;
        if(!go){
            int depth=0;
            while(pc<Pn){
                const char *t=P[pc].txt;
                const char *sv=pp;pp=t;
                if(kw("FOR"))depth++;
                else if(kw("NEXT")){if(!depth){pp=sv;pc++;return;}depth--;}
                pp=sv;pc++;
            }
            return;
        }
        if(fsp>=FORDEPTH){berr("FOR OVERFLOW");return;}
        ForFrame *f=&fstk[fsp++];
        bstrcpy(f->var,nm5,NAMELEN);
        f->lim=to;f->step=step;f->ret=pc;
        return;
    }

    /* NEXT searches the frame stack by name so a GOTO that jumps out of an
       inner loop (skipping its NEXT) doesn't corrupt an outer loop's frame.
       Frames above the matched one are discarded (abandoned loops). */
    if(kw("NEXT")){
        if(fsp<=0){berr("NEXT WITHOUT FOR");return;}
        sw();char nm6[NAMELEN];int hasname=readname(nm6,NAMELEN);
        int k=fsp-1;
        if(hasname){
            k=-1;
            for(int j=fsp-1;j>=0;j--){if(bstreq(fstk[j].var,nm6)){k=j;break;}}
            if(k<0){berr("NEXT MISMATCH");return;}
        }
        fsp=k+1;
        ForFrame *f=&fstk[k];
        Var *vp=varget(f->var);if(!vp){berr("NEXT VAR LOST");return;}
        vp->val=vadd(vp->val,f->step);
        int go=f->step.f>=0?vcmp(vp->val,f->lim)<=0:vcmp(vp->val,f->lim)>=0;
        if(go)pc=f->ret;else fsp--;
        return;
    }

    if(kw("WHILE")){
        Val cond=expr();if(err)return;
        if(cond.i||cond.f!=0.0){
            if(wsp>=WHILEDEPTH){berr("WHILE OVERFLOW");return;}
            wstk[wsp++].ret=pc-1;
        }else{
            int depth=0;
            while(pc<Pn){
                const char *t=P[pc].txt;
                const char *sv=pp;pp=t;
                if(kw("WHILE"))depth++;
                else if(kw("WEND")){if(!depth){pp=sv;pc++;return;}depth--;}
                pp=sv;pc++;
            }
        }
        return;
    }

    /* WEND re-parses the WHILE line to re-evaluate its condition. */
    if(kw("WEND")){
        if(wsp<=0){berr("WEND WITHOUT WHILE");return;}
        pc=wstk[wsp-1].ret;
        const char *sv=pp;
        pp=P[pc].txt;
        if(!kw("WHILE")){berr("WEND MISMATCH");pp=sv;return;}
        Val cond=expr();pp=sv;
        if(!cond.i&&cond.f==0.0)wsp--;
        return;
    }

    if(kw("IF")){
        Val cond;const char *save3=pp;
        char nm7[NAMELEN];
        if(readname(nm7,NAMELEN)&&isstrname(nm7)){
            Val a=strexpr();if(err)return;
            int op=relop();if(!op){berr("SYNTAX");sfree(a.si);return;}
            Val b=strexpr();if(err){sfree(a.si);return;}
            cond=vint(applyrel(op,a,b));
            sfree(a.si);sfree(b.si);
        }else{
            pp=save3;
            Val a=expr();if(err)return;
            int op=relop();
            if(op){Val b=expr();if(err)return;cond=vint(applyrel(op,a,b));}
            else cond=a;
        }
        sw();if(!kw("THEN")){berr("SYNTAX");return;}
        sw();
        if(*pp>='0'&&*pp<='9'){
            if(!cond.i&&!cond.f)return;
            Val n=expr();if(err)return;
            int i=findge((int)n.i);
            if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return;}
            pc=i;
            return;
        }
        /* single-line inline IF: false condition is a no-op, no scan */
        if(*pp&&*pp!=' '){if(cond.i||cond.f!=0.0)stmt(pp);return;}
        /* block IF: skip forward to matching ELSE/ENDIF when false */
        if(!cond.i&&cond.f==0.0){
            int depth=0;
            while(pc<Pn){
                const char *t=P[pc].txt;
                const char *sv=pp;pp=t;
                if(kw("IF"))depth++;
                else if(!depth&&kw("ELSE")){pp=sv;break;}
                else if(!depth&&kw("ENDIF")){pp=sv;break;}
                else if(kw("ENDIF"))depth--;
                pp=sv;pc++;
            }
            return;
        }
        return;
    }

    if(kw("ELSE")||kw("ENDIF")){
        int depth=0;
        while(pc<Pn){
            const char *t=P[pc].txt;
            const char *sv=pp;pp=t;
            if(kw("IF"))depth++;
            else if(kw("ENDIF")){if(!depth){pp=sv;return;}depth--;}
            pp=sv;pc++;
        }
        return;
    }

    if(kw("ON")){
        Val cond=expr();if(err)return;
        sw();int isgosub=0;
        if(kw("GOSUB"))isgosub=1;
        else if(!kw("GOTO")){berr("SYNTAX");return;}
        sw();int idx=(int)cond.i,ci=0;
        for(;;){
            sw();Val n=expr();if(err)return;
            ci++;
            if(ci==idx){
                int i=findge((int)n.i);
                if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return;}
                if(isgosub){
                    if(csp>=CALLDEPTH){berr("GOSUB OVERFLOW");return;}
                    cstk[csp++]=pc;
                }
                pc=i;
                return;
            }
            sw();if(*pp!=',')break;pp++;
        }
        return;
    }

    if(kw("OPEN")){
        sw();Val fn=strexpr();if(err)return;
        sw();if(!kw("FOR")){berr("SYNTAX");sfree(fn.si);return;}
        sw();int forw=0;
        if(kw("OUTPUT")||kw("APPEND"))forw=1;
        else if(kw("INPUT"))forw=0;
        else{berr("SYNTAX");sfree(fn.si);return;}
        sw();if(!kw("AS")){berr("SYNTAX");sfree(fn.si);return;}
        sw();if(*pp=='#')pp++;
        Val fno=expr();if(err){sfree(fn.si);return;}
        int fi=(int)fno.i-1;
        if(fi<0||fi>=MAXFILES){berr("BAD FILE NUMBER");sfree(fn.si);return;}
        if(files[fi].open){berr("FILE ALREADY OPEN");sfree(fn.si);return;}
        if(!fat_ready()){berr("DISK NOT READY");sfree(fn.si);return;}
        FileSlot *fs=&files[fi];
        toname11(spool[fn.si],fs->nm11);sfree(fn.si);
        fs->forwrite=forw;fs->dirty=0;fs->pos=0;fs->len=0;
        if(!forw){
            int n=fat_load(fs->nm11,fs->buf,FBUFSZ-1);
            if(n<=0){berr("FILE NOT FOUND");return;}
            fs->buf[n]=0;fs->len=n;
        }
        fs->open=1;
        return;
    }

    if(kw("CLOSE")){
        sw();int target=-1;
        if(*pp=='#')pp++;
        if(*pp>='0'&&*pp<='9'){Val n=expr();if(err)return;target=(int)n.i-1;}
        for(int i=0;i<MAXFILES;i++){
            if(!files[i].open)continue;
            if(target>=0&&i!=target)continue;
            if(files[i].forwrite&&files[i].dirty&&fat_ready())fat_save(files[i].nm11,files[i].buf,files[i].len);
            files[i].open=0;
        }
        return;
    }

    if(kw("DIR")){
        if(!fat_ready()){berr("DISK NOT READY");return;}
        term_set_color(VGA_CYAN,VGA_BLACK);
        term_puts("Name          Size\n------------- ------\n");
        term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        fat_dir(dircb);
        term_putchar('\n');
        return;
    }

    if(kw("LOAD")){
        sw();Val fn=strexpr();if(err)return;
        if(!fat_ready()){sfree(fn.si);berr("DISK NOT READY");return;}
        char nm11[12];
        toname11(spool[fn.si],nm11);sfree(fn.si);
        int n=fat_load(nm11,fbuf,(int)sizeof(fbuf)-1);
        if(n<=0){berr("FILE NOT FOUND");return;}
        fbuf[n]=0;
        prog_from_buf(fbuf);
        term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puti(Pn);term_puts(" lines\n");term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        return;
    }

    if(kw("SAVE")){
        sw();Val fn=strexpr();if(err)return;
        if(!fat_ready()){sfree(fn.si);berr("DISK NOT READY");return;}
        char nm11[12];
        toname11(spool[fn.si],nm11);sfree(fn.si);
        int pos=prog_to_buf(fbuf,(int)sizeof(fbuf));
        int r=fat_save(nm11,fbuf,pos);
        if(r<0){berr("DISK FULL OR WRITE ERROR");return;}
        term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puti(pos);term_puts(" bytes\n");term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        return;
    }

    if(kw("BEEP")){
        uint32_t freq=800,ms=200;
        sw();
        if(*pp&&*pp!='\''){Val f=expr();if(err)return;freq=(uint32_t)f.i;sw();if(*pp==','){pp++;Val m=expr();if(err)return;ms=(uint32_t)m.i;}}
        beep(freq,ms);
        return;
    }

    if(kw("SAY")){
        sw();int sayisstr=0;
        if(*pp=='"')sayisstr=1;
        else{
            const char *peek=pp;
            while((*peek>='A'&&*peek<='Z')||(*peek>='a'&&*peek<='z')||(*peek>='0'&&*peek<='9')||*peek=='_')peek++;
            sayisstr=(*peek=='$');
        }
        if(sayisstr){Val v=strexpr();if(err)return;say_string(spool[v.si]);sfree(v.si);}
        else{Val v=expr();if(err)return;char tmp[16];itos(v.i,tmp,16);say_string(tmp);}
        return;
    }

    if(kw("DELAY")){Val ms=expr();if(err)return;delay_ms((uint32_t)ms.i);return;}

    if(kw("BLOAD")){
        sw();Val fn=strexpr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");sfree(fn.si);return;}pp++;
        Val addr=expr();if(err){sfree(fn.si);return;}
        char nm11[12];
        toname11(spool[fn.si],nm11);sfree(fn.si);
        if(!fat_ready()){berr("DISK NOT READY");return;}
        int n=fat_load(nm11,(void*)(uint32_t)addr.i,0x7F000);
        if(n<=0){berr("FILE NOT FOUND");return;}
        term_set_color(VGA_DARK_GREY,VGA_BLACK);
        term_puti((int32_t)n);term_puts(" bytes - 0x");
        uint32_t av=(uint32_t)addr.i;
        for(int s=28;s>=0;s-=4)term_putchar("0123456789ABCDEF"[(av>>s)&0xF]);
        term_putchar('\n');
        term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        return;
    }

    if(kw("SYS")){
        Val addr=expr();if(err)return;
        typedef void(*fnt)(void);
        fnt fn=(fnt)(uintptr_t)(uint32_t)addr.i;
        fn();
        return;
    }

    if(kw("TIME")){
        RtcTime rt;char db[16],tb[16];
        if(rtc_read(&rt)<0){berr("RTC");return;}
        rtc_format_date(&rt,db,(int)sizeof(db));
        rtc_format_time(&rt,tb,(int)sizeof(tb));
        term_puts(db);term_putchar(' ');term_puts(tb);term_putchar('\n');
        return;
    }

    if(kw("HELP")){
        term_set_color(VGA_CYAN,VGA_BLACK);
        term_puts("BOREALIS BASIC command reference\n");
        term_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        term_puts(" PRINT/? expr,expr,... INPUT prompt$, var LET var=expr\n");
        term_puts(" REM comment IF expr THEN .. [ELSE] [ENDIF] GOTO n\n");
        term_puts(" FOR v=n TO n [STEP n] NEXT v WHILE expr WEND\n");
        term_puts(" GOSUB n RETURN ON expr GOTO/GOSUB n,...\n");
        term_puts(" DATA READ RESTORE DIM name(d,d) NEW RUN LIST END\n");
        term_puts(" PEEK(addr) POKE addr,v INKEY$ DATE$ TIME$ TIME\n");
        term_puts(" AND OR XOR NOT SHL SHR HEX$ BIN$ &H &B\n");
        term_puts(" SCREEN mode PSET x,y,c LINE x0,y0,x1,y1,c\n");
        term_puts(" RECT/CIRCLE x,y,..,c,[F] PAINT x,y,c PALETTE i,r,g,b\n");
        term_puts(" GCLS c POINT(x,y) DELAY ms EDIT [f.bas]\n");
        term_puts(" SAVE f.bas LOAD f.bas DIR OPEN f$ FOR INPUT/OUTPUT AS n\n");
        term_puts(" CLOSE n BEEP freq,ms SAY strexpr BLOAD f.bin,addr SYS addr\n");
        term_puts(" Math: SIN COS TAN ATN EXP LOG SQR ABS INT FIX SGN RND\n");
        term_puts(" Str: LEFT$ RIGHT$ MID$ STR$ CHR$ VAL ASC LEN\n");
        term_puts(" HALT CLEAR HELP\n");
        return;
    }

    if(kw("SCREEN")){
        Val m=expr();if(err)return;
        if(m.i==13||m.i==1){
            if(gfx_enter()<0){berr("NO VIDEO THUNK");return;}
        }else if(m.i==0){
            if(gfx_leave()<0){berr("NO VIDEO THUNK");return;}
        }else{
            berr("BAD MODE");
        }
        return;
    }

    if(kw("GCLS")){
        Val c=vint(0);sw();
        if(*pp&&*pp!='\''){c=expr();if(err)return;}
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return;}
        gfx_cls((uint8_t)c.i);
        return;
    }

    if(kw("PSET")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return;}
        Val x=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val y=expr();if(err)return;
        Val c=vint(15);sw();
        if(*pp==','){pp++;c=expr();if(err)return;}
        gfx_pset(x.i,y.i,(uint8_t)c.i);
        return;
    }

    if(kw("LINE")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return;}
        Val x0=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val y0=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val x1=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val y1=expr();if(err)return;
        Val c=vint(15);sw();
        if(*pp==','){pp++;c=expr();if(err)return;}
        gfx_line(x0.i,y0.i,x1.i,y1.i,(uint8_t)c.i);
        return;
    }

    if(kw("RECT")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return;}
        Val x0=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val y0=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val x1=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val y1=expr();if(err)return;
        Val c=vint(15);int fill=0;sw();
        if(*pp==','){
            pp++;c=expr();if(err)return;
            sw();
            if(*pp==','){pp++;sw();if(kw("F"))fill=1;}
        }
        gfx_rect(x0.i,y0.i,x1.i,y1.i,(uint8_t)c.i,fill);
        return;
    }

    if(kw("CIRCLE")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return;}
        Val cx=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val cy=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val r=expr();if(err)return;
        Val c=vint(15);int fill=0;sw();
        if(*pp==','){
            pp++;c=expr();if(err)return;
            sw();
            if(*pp==','){pp++;sw();if(kw("F"))fill=1;}
        }
        gfx_circle(cx.i,cy.i,r.i,(uint8_t)c.i,fill);
        return;
    }

    if(kw("PAINT")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return;}
        Val x=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val y=expr();if(err)return;
        Val c=vint(15);sw();
        if(*pp==','){pp++;c=expr();if(err)return;}
        gfx_paint(x.i,y.i,(uint8_t)c.i);
        return;
    }

    if(kw("PALETTE")){
        Val idx=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val r=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val g=expr();if(err)return;
        sw();if(*pp!=','){berr("SYNTAX");return;}pp++;
        Val b=expr();if(err)return;
        gfx_set_palette((uint8_t)idx.i,(uint8_t)r.i,(uint8_t)g.i,(uint8_t)b.i);
        return;
    }

    /* bare assignment: no LET keyword; speculative, must be last */
    {
        char nm[NAMELEN];
        if(readname(nm,NAMELEN)&&do_assign(nm))return;
    }

    berr("WHAT?");
}

/* Run loop pre-increments pc before each statement, so every handler sees
   pc already pointing at the NEXT line: FOR/GOSUB store it as-is, RETURN
   restores it as-is, and "pc==here+1" means sequential flow. */
void basic_run(void){
    for(;;){
        if(running){
            if(pc>=Pn){running=0;continue;}
            int here=pc++;
            err=0;
            stmt(P[here].txt);
            if(running&&pc==here+1)continue;
            err=0;
            continue;
        }
        term_sync_cursor();
        term_set_color(VGA_LIGHT_GREEN,VGA_BLACK);term_puts("=");
        term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puts("> ");
        term_set_color(VGA_WHITE,VGA_BLACK);
        term_getline(ibuf,IBUF);upr(ibuf);
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
