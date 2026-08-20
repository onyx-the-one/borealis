/* stmt/file.inc — OPEN, CLOSE, DIR, LOAD, SAVE.
   PRINT #/INPUT # live in console.inc, not here.
   Fragment contract: see dispatcher comment in basic.c. */
static int stmt_file(void){
    /* OPEN */
    if(kw("OPEN")){sw();Val fn=strexpr();if(err)return 1;sw();if(!kw("FOR")){berr("SYNTAX");sfree(fn.si);return 1;}sw();int forw=0;if(kw("OUTPUT")||kw("APPEND"))forw=1;else if(kw("INPUT"))forw=0;else{berr("SYNTAX");sfree(fn.si);return 1;}sw();if(!kw("AS")){berr("SYNTAX");sfree(fn.si);return 1;}sw();if(*pp=='#')pp++;Val fno=expr();if(err){sfree(fn.si);return 1;}int fi=(int)fno.i-1;if(fi<0||fi>=MAXFILES){berr("BAD FILE NUMBER");sfree(fn.si);return 1;}if(files[fi].open){berr("FILE ALREADY OPEN");sfree(fn.si);return 1;}if(!fat_ready()){berr("DISK NOT READY");sfree(fn.si);return 1;}FileSlot *fs=&files[fi];toname11(spool[fn.si],fs->nm11);sfree(fn.si);fs->forwrite=forw;fs->dirty=0;fs->pos=0;fs->len=0;if(!forw){int n=fat_load(fs->nm11,fs->buf,FBUFSZ-1);if(n<=0){berr("FILE NOT FOUND");return 1;}fs->buf[n]=0;fs->len=n;}fs->open=1;return 1;}

    /* CLOSE */
    if(kw("CLOSE")){sw();int target=-1;if(*pp=='#')pp++;if(*pp>='0'&&*pp<='9'){Val n=expr();if(err)return 1;target=(int)n.i-1;}for(int i=0;i<MAXFILES;i++){if(!files[i].open)continue;if(target>=0&&i!=target)continue;if(files[i].forwrite&&files[i].dirty&&fat_ready())fat_save(files[i].nm11,files[i].buf,files[i].len);files[i].open=0;}return 1;}

    /* DIR */
    if(kw("DIR")){if(!fat_ready()){berr("DISK NOT READY");return 1;}term_set_color(VGA_CYAN,VGA_BLACK);term_puts("Name          Size\n------------- ------\n");term_set_color(VGA_LIGHT_GREY,VGA_BLACK);fat_dir(dircb);term_putchar('\n');return 1;}

    /* LOAD */
    if(kw("LOAD")){sw();Val fn=strexpr();if(err)return 1;if(!fat_ready()){sfree(fn.si);berr("DISK NOT READY");return 1;}char nm11[12];toname11(spool[fn.si],nm11);sfree(fn.si);int n=fat_load(nm11,fbuf,(int)sizeof(fbuf)-1);if(n<=0){berr("FILE NOT FOUND");return 1;}fbuf[n]=0;Pn=0;char *p=fbuf;while(*p){while(*p==' ')p++;if(!*p)break;if(*p>='0'&&*p<='9'){int ln=0;while(*p>='0'&&*p<='9')ln=ln*10+(*p++-'0');while(*p==' ')p++;char *start=p;while(*p&&*p!='\n')p++;char saved=*p;*p=0;storeline(ln,start);*p=saved;}else{while(*p&&*p!='\n')p++;}if(*p=='\n')p++;}term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puti(Pn);term_puts(" lines\n");term_set_color(VGA_LIGHT_GREY,VGA_BLACK);return 1;}

    /* SAVE */
    if(kw("SAVE")){sw();Val fn=strexpr();if(err)return 1;if(!fat_ready()){sfree(fn.si);berr("DISK NOT READY");return 1;}char nm11[12];toname11(spool[fn.si],nm11);sfree(fn.si);int pos=0;for(int i=0;i<Pn;i++){uint16_t ln=P[i].num;char nb[8];int ni=0;uint16_t tmp2=ln;if(!tmp2)nb[ni++]='0';else{uint16_t t=tmp2;while(t){nb[ni++]='0'+t%10;t/=10;}}for(int k=ni-1;k>=0;k--){if(pos<(int)sizeof(fbuf)-3)fbuf[pos++]=nb[k];}if(pos<(int)sizeof(fbuf)-2)fbuf[pos++]=' ';const char *src=P[i].txt;while(*src&&pos<(int)sizeof(fbuf)-3)fbuf[pos++]=*src++;if(pos<(int)sizeof(fbuf)-2)fbuf[pos++]='\n';}int r=fat_save(nm11,fbuf,pos);if(r<0){berr("DISK FULL OR WRITE ERROR");return 1;}term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puti(pos);term_puts(" bytes\n");term_set_color(VGA_LIGHT_GREY,VGA_BLACK);return 1;}

    return 0;
}
