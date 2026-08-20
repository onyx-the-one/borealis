/* stmt/graphics.inc — SCREEN, GCLS, PSET, LINE, RECT, CIRCLE, PAINT, PALETTE.
   gfx/gfx.h is included at the top of basic.c, not here.
   Fragment contract: see dispatcher comment in basic.c.
   Note: this file arrived in the rewrite with missing closing braces in
   every handler (transit damage); brace structure below is reconstructed
   to the obvious intended form — behavior unchanged. */
static int stmt_graphics(void){
    /* SCREEN */
    if(kw("SCREEN")){
        Val m=expr();if(err)return 1;
        if(m.i==13||m.i==1){
            if(gfx_enter()<0){berr("NO VIDEO THUNK");return 1;}
        }else if(m.i==0){
            if(gfx_leave()<0){berr("NO VIDEO THUNK");return 1;}
        }else{
            berr("BAD MODE");
        }
        return 1;
    }

    /* GCLS color — clear framebuffer in graphics mode */
    if(kw("GCLS")){
        Val c=vint(0);sw();
        if(*pp&&*pp!='\''){c=expr();if(err)return 1;}
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return 1;}
        gfx_cls((uint8_t)c.i);
        return 1;
    }

    /* PSET x,y,color */
    if(kw("PSET")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return 1;}
        Val x=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val y=expr();if(err)return 1;
        Val c=vint(15);sw();
        if(*pp==','){pp++;c=expr();if(err)return 1;}
        gfx_pset(x.i,y.i,(uint8_t)c.i);
        return 1;
    }

    /* LINE x0,y0,x1,y1,color */
    if(kw("LINE")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return 1;}
        Val x0=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val y0=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val x1=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val y1=expr();if(err)return 1;
        Val c=vint(15);sw();
        if(*pp==','){pp++;c=expr();if(err)return 1;}
        gfx_line(x0.i,y0.i,x1.i,y1.i,(uint8_t)c.i);
        return 1;
    }

    /* RECT x0,y0,x1,y1,color[,F] */
    if(kw("RECT")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return 1;}
        Val x0=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val y0=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val x1=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val y1=expr();if(err)return 1;
        Val c=vint(15);int fill=0;sw();
        if(*pp==','){
            pp++;c=expr();if(err)return 1;
            sw();
            if(*pp==','){
                pp++;sw();
                if(kw("F"))fill=1;
            }
        }
        gfx_rect(x0.i,y0.i,x1.i,y1.i,(uint8_t)c.i,fill);
        return 1;
    }

    /* CIRCLE cx,cy,r,color[,F] */
    if(kw("CIRCLE")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return 1;}
        Val cx=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val cy=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val r=expr();if(err)return 1;
        Val c=vint(15);int fill=0;sw();
        if(*pp==','){
            pp++;c=expr();if(err)return 1;
            sw();
            if(*pp==','){
                pp++;sw();
                if(kw("F"))fill=1;
            }
        }
        gfx_circle(cx.i,cy.i,r.i,(uint8_t)c.i,fill);
        return 1;
    }

    /* PAINT x,y,color — flood fill from (x,y) */
    if(kw("PAINT")){
        if(!gfx_active()){berr("NOT IN GRAPHICS MODE");return 1;}
        Val x=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val y=expr();if(err)return 1;
        Val c=vint(15);sw();
        if(*pp==','){pp++;c=expr();if(err)return 1;}
        gfx_paint(x.i,y.i,(uint8_t)c.i);
        return 1;
    }

    /* PALETTE index,r,g,b (each component 0-63, VGA 6-bit DAC) */
    if(kw("PALETTE")){
        Val idx=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val r=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val g=expr();if(err)return 1;
        sw();if(*pp!=','){berr("SYNTAX");return 1;}pp++;
        Val b=expr();if(err)return 1;
        gfx_set_palette((uint8_t)idx.i,(uint8_t)r.i,(uint8_t)g.i,(uint8_t)b.i);
        return 1;
    }

    return 0;
}
