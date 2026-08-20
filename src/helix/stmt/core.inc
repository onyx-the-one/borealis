/* stmt/core.inc — REM, KPAN, CLEAR, HALT, NEW, END, LIST, RUN, DIM, POKE,
   LET, bare assignment. Fragment contract: see dispatcher comment in basic.c. */
static int stmt_core(void){
    if(kw("REM")||*pp=='\'')return 1;
    if(kw("KPAN")){kpanic("KPAN");return 1;}
    if(kw("CLEAR")){draw_banner();return 1;}
    if(kw("HALT")){term_set_color(VGA_YELLOW,VGA_BLACK);term_puts(" halted.\n");__asm__ __volatile__("cli");for(;;)__asm__ __volatile__("hlt");}
    if(kw("NEW")){Pn=0;nvars=0;csp=0;fsp=0;wsp=0;dataline=0;datacol=0;narrays=0;aused=0;for(int i=0;i<NSTRS;i++)sused[i]=0;for(int i=0;i<MAXFILES;i++)files[i].open=0;return 1;}
    if(kw("END")){running=0;return 1;}
    if(kw("LIST")){for(int i=0;i<Pn;i++){term_set_color(VGA_DARK_GREY,VGA_BLACK);term_puti(P[i].num);term_putchar(' ');term_set_color(VGA_LIGHT_GREY,VGA_BLACK);term_puts(P[i].txt);term_putchar('\n');}return 1;}
    if(kw("RUN")){pc=0;running=1;csp=0;fsp=0;wsp=0;dataline=0;datacol=0;narrays=0;aused=0;for(int i=0;i<NSTRS;i++)sused[i]=0;nvars=0;for(int i=0;i<MAXFILES;i++)files[i].open=0;return 1;}

    /* DIM */
    if(kw("DIM")){
        for(;;){sw();char nm[NAMELEN];if(!readname(nm,NAMELEN)){berr("SYNTAX");return 1;}sw();if(*pp!='('){berr("SYNTAX");return 1;}pp++;Val d1=expr();if(err)return 1;Val d2=vint(-1),d3=vint(-1);if(*pp==','){pp++;d2=expr();if(err)return 1;}if(*pp==','){pp++;d3=expr();if(err)return 1;}sw();if(*pp!=')'){berr("SYNTAX");return 1;}pp++;
            if(narrays>=MAXARRAYS){berr("MEM");return 1;}Array *a=&arrays[narrays];bstrcpy(a->name,nm,NAMELEN);a->isstr=isstrname(nm);a->dims=d3.i>=0?3:d2.i>=0?2:1;a->size[0]=d1.i;a->size[1]=d2.i;a->size[2]=d3.i;
            int total;if(a->dims==1)total=d1.i+1;else if(a->dims==2)total=(d1.i+1)*(d2.i+1);else total=(d1.i+1)*(d2.i+1)*(d3.i+1);
            if(aused+total>MAXELEMS){berr("MEM");return 1;}a->base=aused;a->total=total;aused+=total;for(int i=a->base;i<a->base+total;i++)aelems[i]=vint(0);narrays++;
            sw();if(*pp!=',')break;pp++;}return 1;}

    /* POKE */
    if(kw("POKE")){Val addr=expr();if(err)return 1;sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;Val val=expr();if(err)return 1;*(uint8_t*)(uint32_t)addr.i=(uint8_t)val.i;return 1;}

    /* LET name = expr   |   LET name(i[,j[,k]]) = expr */
    if(kw("LET")){
        char nm[NAMELEN];
        if(!readname(nm,NAMELEN)){berr("SYNTAX");return 1;}
        sw();
        if(*pp=='('){
            Array *ar=arrget(nm);
            if(!ar){berr("SYNTAX");return 1;}
            pp++;Val i1=expr();if(err)return 1;Val i2=vint(0),i3=vint(0);
            if(*pp==','){pp++;i2=expr();if(err)return 1;}
            if(*pp==','){pp++;i3=expr();if(err)return 1;}
            sw();if(*pp!=')'){berr("SYNTAX");return 1;}pp++;
            sw();if(*pp!='='){berr("SYNTAX");return 1;}pp++;
            int idx=arridx(ar,i1.i,i2.i,i3.i);if(idx<0)return 1;
            if(isstrname(nm)){Val v=strexpr();if(!err){if(aelems[idx].ty==TY_STR&&aelems[idx].si>=0)sfree(aelems[idx].si);aelems[idx]=v;}}
            else{Val v=expr();if(!err)aelems[idx]=v;}
            return 1;
        }
        if(*pp!='='){berr("SYNTAX");return 1;}pp++;
        if(isstrname(nm)){Val v=strexpr();if(!err)varset(nm,v);}
        else{Val v=expr();if(!err)varset(nm,v);}
        return 1;
    }

    /* bare assignment: name = expr   |   name(i[,j[,k]]) = expr   (no LET keyword).
       Speculative parse: on any mismatch, pp must be restored so the next
       fragment sees the line untouched (dispatcher contract). */
    {
        const char *save_pp=pp;
        char nm[NAMELEN];
        if(readname(nm,NAMELEN)){
            sw();
            if(*pp=='('){
                Array *ar=arrget(nm);
                if(ar){
                    pp++;Val i1=expr();if(err)return 1;Val i2=vint(0),i3=vint(0);
                    if(*pp==','){pp++;i2=expr();if(err)return 1;}
                    if(*pp==','){pp++;i3=expr();if(err)return 1;}
                    sw();if(*pp!=')'){berr("SYNTAX");return 1;}pp++;
                    sw();if(*pp!='='){berr("SYNTAX");return 1;}pp++;
                    int idx=arridx(ar,i1.i,i2.i,i3.i);if(idx<0)return 1;
                    if(isstrname(nm)){Val v=strexpr();if(!err){if(aelems[idx].ty==TY_STR&&aelems[idx].si>=0)sfree(aelems[idx].si);aelems[idx]=v;}}
                    else{Val v=expr();if(!err)aelems[idx]=v;}
                    return 1;
                }
            }
            else if(*pp=='='){
                pp++;
                if(isstrname(nm)){Val v=strexpr();if(!err)varset(nm,v);}
                else{Val v=expr();if(!err)varset(nm,v);}
                return 1;
            }
        }
        pp=save_pp;
    }
    return 0;
}
