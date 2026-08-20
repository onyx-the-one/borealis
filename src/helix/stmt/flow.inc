/* stmt/flow.inc — GOTO, GOSUB, RETURN, FOR, NEXT, WHILE, WEND,
   IF/ELSE/ENDIF, ON. Fragment contract: see dispatcher comment in basic.c.
   Note: inline IF calls stmt() on its THEN-statement, so this fragment
   depends on the forward declaration of stmt() in basic.c. */
static int stmt_flow(void){
    /* GOTO */
    if(kw("GOTO")){sw();Val n=expr();if(err)return 1;int i=findge((int)n.i);if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return 1;}pc=i;return 1;}

    /* GOSUB */
    if(kw("GOSUB")){sw();Val n=expr();if(err)return 1;int i=findge((int)n.i);if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return 1;}if(csp>=CALLDEPTH){berr("GOSUB OVERFLOW");return 1;}cstk[csp++]=pc;pc=i;return 1;}

    if(kw("RETURN")){if(csp<=0){berr("RETURN WITHOUT GOSUB");return 1;}pc=cstk[--csp];return 1;}

    /* FOR */
    if(kw("FOR")){sw();char nm5[NAMELEN];if(!readname(nm5,NAMELEN)){berr("SYNTAX");return 1;}sw();if(*pp!='='){berr("SYNTAX");return 1;}pp++;Val from=expr();if(err)return 1;sw();if(!kw("TO")){berr("SYNTAX");return 1;}sw();Val to=expr();if(err)return 1;Val step=vint(1);sw();if(kw("STEP")){sw();step=expr();if(err)return 1;}if(!step.i&&!step.f){berr("STEP 0");return 1;}varset(nm5,from);int go=step.f>=0?vcmp(from,to)<=0:vcmp(from,to)>=0;if(!go){int depth=0;while(pc<Pn){const char *t=P[pc].txt;const char *sv=pp;pp=t;if(kw("FOR"))depth++;else if(kw("NEXT")){if(!depth){pp=sv;pc++;return 1;}depth--;}pp=sv;pc++;}return 1;}if(fsp>=FORDEPTH){berr("FOR OVERFLOW");return 1;}ForFrame *f=&fstk[fsp++];bstrcpy(f->var,nm5,NAMELEN);f->lim=to;f->step=step;f->ret=pc;return 1;}

    /* NEXT — search the frame stack by name so a GOTO that jumps out of an
       inner loop (skipping its NEXT) doesn't corrupt an outer loop's frame.
       Any frames above the matched one are discarded (their loops are
       considered abandoned). */
    if(kw("NEXT")){
        if(fsp<=0){berr("NEXT WITHOUT FOR");return 1;}
        sw();char nm6[NAMELEN];int hasname=readname(nm6,NAMELEN);
        int k=fsp-1;
        if(hasname){
            k=-1;
            for(int j=fsp-1;j>=0;j--){if(bstreq(fstk[j].var,nm6)){k=j;break;}}
            if(k<0){berr("NEXT MISMATCH");return 1;}
        }
        fsp=k+1;
        ForFrame *f=&fstk[k];
        Var *vp=varget(f->var);if(!vp){berr("NEXT VAR LOST");return 1;}
        vp->val=vadd(vp->val,f->step);
        int go=f->step.f>=0?vcmp(vp->val,f->lim)<=0:vcmp(vp->val,f->lim)>=0;
        if(go)pc=f->ret;else fsp--;
        return 1;
    }

    /* WHILE / WEND */
    if(kw("WHILE")){Val cond=expr();if(err)return 1;if(cond.i||cond.f!=0.0){if(wsp>=WHILEDEPTH){berr("WHILE OVERFLOW");return 1;}wstk[wsp++].ret=pc-1;}else{int depth=0;while(pc<Pn){const char *t=P[pc].txt;const char *sv=pp;pp=t;if(kw("WHILE"))depth++;else if(kw("WEND")){if(!depth){pp=sv;pc++;return 1;}depth--;}pp=sv;pc++;}}return 1;}
    if(kw("WEND")){if(wsp<=0){berr("WEND WITHOUT WHILE");return 1;}pc=wstk[wsp-1].ret;const char *sv=pp;pp=P[pc].txt;if(!kw("WHILE")){berr("WEND MISMATCH");pp=sv;return 1;}Val cond=expr();pp=sv;if(!cond.i&&cond.f==0.0)wsp--;return 1;}

    /* IF */
    if(kw("IF")){
        Val cond;const char *save3=pp;
        char nm7[NAMELEN];
        if(readname(nm7,NAMELEN)&&isstrname(nm7)){
            Val a=strexpr();if(err)return 1;int op=relop();if(!op){berr("SYNTAX");sfree(a.si);return 1;}Val b=strexpr();if(err){sfree(a.si);return 1;}cond=vint(applyrel(op,a,b));sfree(a.si);sfree(b.si);
        }else{
            pp=save3;
            Val a=expr();if(err)return 1;
            int op=relop();
            if(op){Val b=expr();if(err)return 1;cond=vint(applyrel(op,a,b));}
            else cond=a;
        }
        sw();if(!kw("THEN")){berr("SYNTAX");return 1;}sw();
        if(*pp>='0'&&*pp<='9'){if(!cond.i&&!cond.f)return 1;Val n=expr();if(err)return 1;int i=findge((int)n.i);if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return 1;}pc=i;return 1;}
        /* single-line inline IF: "IF cond THEN <stmt>" — execute stmt if true,
           otherwise this is a no-op (there is no block to skip to). */
        if(*pp&&*pp!=' '){if(cond.i||cond.f!=0.0)stmt(pp);return 1;}
        /* multi-line/block IF: "IF cond THEN" with nothing else on the line —
           skip forward to matching ELSE/ENDIF when false. */
        if(!cond.i&&cond.f==0.0){int depth=0;while(pc<Pn){const char *t=P[pc].txt;const char *sv=pp;pp=t;if(kw("IF"))depth++;else if(!depth&&kw("ELSE")){pp=sv;break;}else if(!depth&&kw("ENDIF")){pp=sv;break;}else if(kw("ENDIF"))depth--;pp=sv;pc++;}return 1;}
        return 1;
    }
    if(kw("ELSE")||kw("ENDIF")){int depth=0;while(pc<Pn){const char *t=P[pc].txt;const char *sv=pp;pp=t;if(kw("IF"))depth++;else if(kw("ENDIF")){if(!depth){pp=sv;return 1;}depth--;}pp=sv;pc++;}return 1;}

    /* ON cond GOTO/GOSUB */
    if(kw("ON")){Val cond=expr();if(err)return 1;sw();int isgosub=0;if(kw("GOSUB"))isgosub=1;else if(!kw("GOTO")){berr("SYNTAX");return 1;}sw();int idx=(int)cond.i,ci=0;for(;;){sw();Val n=expr();if(err)return 1;ci++;if(ci==idx){int i=findge((int)n.i);if(i>=Pn||(int)P[i].num!=(int)n.i){berr("LINE NOT FOUND");return 1;}if(isgosub){if(csp>=CALLDEPTH){berr("GOSUB OVERFLOW");return 1;}cstk[csp++]=pc;}pc=i;return 1;}sw();if(*pp!=',')break;pp++;}return 1;}

    return 0;
}
