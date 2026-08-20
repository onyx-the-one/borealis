/* ehci.c — EHCI host controller driver. Polled, async-schedule only.
   Stage 1: PCI find, BIOS/SMM ownership handoff, reset, idle schedule,
   port scan. Stage 2: transfer engine — one static QH plus a qTD pool
   linked into the async ring per transfer, reclaimed with the IAA
   doorbell; synchronous EP0 control. Stage 3: bulk transfers for the
   mass-storage layer. No interrupts, no periodic schedule (mass storage
   needs neither).
   Delays are PIT-based (timer.h): the port-0x80 loop ran fast on real
   LPC hardware, which made the 50 ms port-reset hold nominal-only and
   cost the laptop its device detection.
   Memory model: helix runs identity-mapped, so &static == physical. */
#include "ehci.h"
#include "pci.h"
#include "log.h"
#include "timer.h"
#include <stdint.h>

static inline void barrier(void){ __asm__ __volatile__("" ::: "memory"); }

static volatile uint8_t *cap, *op;
static uint8_t nports, ppc, eecp;
static int g_up;            /* probe completed, schedule running */
static int g_dev_port = -1; /* first port that came up high-speed */

static uint32_t op_r(uint32_t r){ return *(volatile uint32_t *)(op + r); }
static void op_w(uint32_t r, uint32_t v){ *(volatile uint32_t *)(op + r) = v; }

static int op_wait(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms){
    while(ms--){
        if((op_r(reg) & mask) == want) return 0;
        mdelay(1);
    }
    return -1;
}

/* ── transfer engine ─────────────────────────────────────────────── */

#define TD_PID_OUT   0u
#define TD_PID_IN    1u
#define TD_PID_SETUP 2u
#define TD_ACTIVE    0x80u
#define TD_IOC       (1u << 15)
#define TD_ERR_MASK  0x7Cu   /* halted|buf-err|babble|xact-err|missed-mf */

#define TD_POOL 4            /* setup + data + status, plus one spare */

static EhciQH async_head __attribute__((aligned(32)));
static EhciQH xfer_qh    __attribute__((aligned(32)));
static EhciTD tds[TD_POOL] __attribute__((aligned(32)));
static uint8_t setup_buf[8] __attribute__((aligned(32)));

static void td_fill(EhciTD *td, uint32_t next, uint8_t pid, uint8_t dt,
                    const void *buf, uint16_t len, int ioc){
    td->next = next;
    td->alt  = 1; /* T: no recovery alternate, errors halt the QH */
    td->token = ((uint32_t)dt << 31) | ((uint32_t)len << 16) |
                (ioc ? TD_IOC : 0) | (3u << 10) /* CERR */ |
                (pid << 8) | TD_ACTIVE;
    if(!buf){
        for(int i = 0; i < 5; i++) td->buf[i] = 0;
        return;
    }
    /* successive 4KB pages; buf[0] carries the start offset */
    uint32_t p = (uint32_t)(uintptr_t)buf;
    for(int i = 0; i < 5; i++){
        td->buf[i] = p & 0xFFFFF000u;
        p = (p & 0xFFFFF000u) + 0x1000;
    }
    td->buf[0] |= (uint32_t)(uintptr_t)buf & 0xFFF;
}

/* run the QH's td chain: link into the async ring after the head,
   poll for completion, unlink, ring the doorbell so the controller
   forgets the QH before we reuse it. */
static int qh_run(int last_td){
    xfer_qh.hlp = async_head.hlp;
    barrier();
    async_head.hlp = (uint32_t)(uintptr_t)&xfer_qh | 0x02;
    barrier();

    uint32_t t = 0;
    while(t < 1000){
        int active = 0;
        for(int i = 0; i <= last_td; i++)
            if(tds[i].token & TD_ACTIVE){ active = 1; break; }
        if(!active) break;
        mdelay(1);
        t++;
    }

    int rc = 0;
    if(t >= 1000){
        log_puts("ehci: xfer TIMEOUT\r\n");
        rc = -1;
    }
    for(int i = 0; i <= last_td; i++){
        uint32_t tok = tds[i].token;
        if(tok & TD_ERR_MASK){
            log_puts("ehci: td"); log_puti(i);
            log_puts(" error token=0x"); log_hex32(tok); log_puts("\r\n");
            rc = -1;
        }
    }

    async_head.hlp = xfer_qh.hlp;
    barrier();
    op_w(EHCI_USBCMD, op_r(EHCI_USBCMD) | USBCMD_IAA);
    if(op_wait(EHCI_USBSTS, USBSTS_IAA, USBSTS_IAA, 250))
        log_puts("ehci: WARNING iaa doorbell timeout\r\n");
    op_w(EHCI_USBSTS, USBSTS_IAA);
    return rc;
}

int ehci_control(uint8_t addr, const uint8_t setup[8], void *data, uint16_t len){
    if(!g_up) return -1;
    if(len > 0x3000){ /* never needed for enumeration; keeps one data qtd safe */
        log_puts("ehci: control len too big for one qtd\r\n");
        return -1;
    }
    for(int i = 0; i < 8; i++) setup_buf[i] = setup[i];
    int dir_in = setup[0] & 0x80;

    td_fill(&tds[0], (uint32_t)(uintptr_t)&tds[1], TD_PID_SETUP, 0, setup_buf, 8, 0);
    int last;
    if(len){
        td_fill(&tds[1], (uint32_t)(uintptr_t)&tds[2],
                dir_in ? TD_PID_IN : TD_PID_OUT, 1, data, len, 0);
        last = 2;
    } else {
        last = 1;
    }
    td_fill(&tds[last], 1, dir_in ? TD_PID_OUT : TD_PID_IN, 1, 0, 0, 1);

    /* HS control EP0: C=0 (toggles come from the td tokens set above),
       H=0 (only the async head is a reclamation head), maxpkt 64 */
    xfer_qh.ep1 = (8u << 28) | (64u << 16) | (2u << 12) | addr;
    xfer_qh.ep2 = (1u << 30); /* Mult=1 */
    xfer_qh.cur_qtd  = 0;
    xfer_qh.next_qtd = (uint32_t)(uintptr_t)&tds[0];
    xfer_qh.alt_qtd  = 1;
    xfer_qh.token    = 0;
    for(int i = 0; i < 5; i++){ xfer_qh.buf[i] = 0; xfer_qh.extbuf[i] = 0; }

    return qh_run(last);
}

int ehci_bulk(uint8_t addr, uint8_t ep, uint16_t maxpkt, int dir_in,
              int toggle, void *data, uint16_t len){
    if(!g_up) return -1;
    if(!len) return 0;
    if(len > 0x4000){ /* single qtd: five 4K pages */
        log_puts("ehci: bulk len too big for one qtd\r\n");
        return -1;
    }
    td_fill(&tds[0], 1, dir_in ? TD_PID_IN : TD_PID_OUT, (uint8_t)toggle, data, len, 1);
    xfer_qh.ep1 = (8u << 28) | ((uint32_t)maxpkt << 16) | (2u << 12) |
                  ((uint32_t)ep << 8) | addr;
    xfer_qh.ep2 = (1u << 30);
    xfer_qh.cur_qtd  = 0;
    xfer_qh.next_qtd = (uint32_t)(uintptr_t)&tds[0];
    xfer_qh.alt_qtd  = 1;
    xfer_qh.token    = 0;
    for(int i = 0; i < 5; i++){ xfer_qh.buf[i] = 0; xfer_qh.extbuf[i] = 0; }

    if(qh_run(0)) return -1;
    uint32_t resid = (tds[0].token >> 16) & 0x7FFF;
    return (int)(len - resid);
}

int ehci_device_present(void){ return g_dev_port >= 0; }

/* ── probe / init ────────────────────────────────────────────────── */

/* Take the controller away from SMM legacy USB. This is the layer the
   laptop actually cares about: the SMM trap is the prime suspect for
   int 13h dying after the protected-mode excursion. */
static void ehci_handoff(uint8_t bus, uint8_t dev, uint8_t fn){
    if(eecp < 0x40){
        log_puts("ehci: no eecp, nothing to hand off\r\n");
        return;
    }
    uint32_t legsup = pci_read32(bus, dev, fn, eecp);
    if((legsup & 0xFF) != 0x01){
        log_puts("ehci: eecp 0x"); log_hex8(eecp);
        log_puts(" cap id 0x"); log_hex8(legsup & 0xFF);
        log_puts(" is not usb legsup\r\n");
        return;
    }
    log_puts("ehci: usb legsup at 0x"); log_hex8(eecp);
    log_puts((legsup & (1u << 16)) ? " bios-owned, requesting\r\n" : " already os-owned\r\n");
    pci_write32(bus, dev, fn, eecp, legsup | (1u << 24));
    int owned = 0;
    for(int i = 0; i < 1000 && !owned; i++){
        owned = !(pci_read32(bus, dev, fn, eecp) & (1u << 16));
        if(!owned) mdelay(1);
    }
    if(!owned) log_puts("ehci: WARNING bios semaphore never released, continuing anyway\r\n");
    pci_write32(bus, dev, fn, eecp + 4, 0); /* kill every legacy SMI source */
    log_puts("ehci: ownership taken, smi enables cleared\r\n");
}

static void ehci_ports(void){
    for(uint8_t p = 0; p < nports; p++){
        uint32_t r = EHCI_PORTSC(p);
        if(ppc){
            op_w(r, (op_r(r) & ~PORTSC_RWC) | PORTSC_PP);
            mdelay(100); /* power-good settle, real ms */
        }
        uint32_t v = op_r(r);
        log_puts("ehci: port "); log_puti(p); log_puts(" sc=0x"); log_hex32(v);
        if(!(v & PORTSC_CCS)){
            log_puts(" empty\r\n");
            continue;
        }
        int ok = 0;
        for(int attempt = 0; attempt < 2 && !ok; attempt++){
            if(attempt) mdelay(500);
            v = op_r(r);
            op_w(r, (v & ~PORTSC_RWC) | PORTSC_CSC);   /* ack connect change */
            v = op_r(r);
            op_w(r, (v & ~PORTSC_RWC) | PORTSC_PR);    /* assert reset */
            mdelay(50);                                 /* real 50 ms */
            v = op_r(r);
            op_w(r, v & ~PORTSC_RWC & ~PORTSC_PR);     /* deassert */
            if(op_wait(r, PORTSC_PEDC, PORTSC_PEDC, 1000)){
                log_puts(" reset-timeout sc=0x"); log_hex32(op_r(r));
                continue;
            }
            v = op_r(r);
            op_w(r, (v & ~PORTSC_RWC) | PORTSC_PEDC);  /* ack enable change */
            if(v & PORTSC_PED){
                log_puts(" high-speed device");
                if(g_dev_port < 0) g_dev_port = p;
            } else {
                log_puts(" full/low-speed, lost to companion controller");
            }
            ok = 1;
        }
        log_puts("\r\n");
    }
}

int ehci_probe(void){
    uint8_t bus, dev, fn;
    log_puts("ehci: scanning pci for class 0C/03/20\r\n");
    if(pci_find(0x0C, 0x03, 0x20, &bus, &dev, &fn)){
        log_puts("ehci: no ehci controller, usb storage unavailable\r\n");
        return -1;
    }
    log_puts("ehci: found at ");
    log_puti(bus); log_putc(':'); log_puti(dev); log_putc('.'); log_puti(fn);
    uint32_t bar = pci_read32(bus, dev, fn, 0x10);
    log_puts(" bar=0x"); log_hex32(bar); log_puts("\r\n");
    if((bar & 1) || !(bar & 0xFFFFFFF0u)){
        log_puts("ehci: bad bar (io-space or unassigned), giving up\r\n");
        return -1;
    }
    if((bar & 6) == 4){
        log_puts("ehci: 64-bit bar, not supported yet\r\n");
        return -1;
    }
    cap = (volatile uint8_t *)(uintptr_t)(bar & 0xFFFFFFF0u);

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write16(bus, dev, fn, 0x04, cmd | 0x06);   /* mem space + bus master */

    uint8_t caplen = *(volatile uint8_t *)(cap + EHCI_CAPLENGTH);
    uint32_t ver   = *(volatile uint32_t *)(cap + EHCI_HCIVERSION);
    uint32_t hcsp  = *(volatile uint32_t *)(cap + EHCI_HCSPARAMS);
    uint32_t hccp  = *(volatile uint32_t *)(cap + EHCI_HCCPARAMS);
    op     = cap + caplen;
    nports = hcsp & 0xF;
    ppc    = (hcsp >> 4) & 1;
    eecp   = (hccp >> 8) & 0xFF;
    log_puts("ehci: caplen=0x"); log_hex8(caplen);
    log_puts(" ver=0x"); log_hex32(ver & 0xFFFF);
    log_puts(" ports="); log_puti(nports);
    log_puts(" ppc="); log_puti(ppc);
    log_puts(" eecp=0x"); log_hex8(eecp);
    log_puts(" 64bit="); log_puti(hccp & 1);
    log_puts("\r\n");

    ehci_handoff(bus, dev, fn);

    op_w(EHCI_USBCMD, op_r(EHCI_USBCMD) & ~USBCMD_RS);
    if(op_wait(EHCI_USBSTS, USBSTS_HCHALTED, USBSTS_HCHALTED, 500))
        log_puts("ehci: WARNING controller would not halt\r\n");
    op_w(EHCI_USBCMD, USBCMD_HCRESET);
    if(op_wait(EHCI_USBCMD, USBCMD_HCRESET, 0, 500)){
        log_puts("ehci: reset stuck, giving up\r\n");
        return -1;
    }
    log_puts("ehci: reset ok\r\n");

    /* idle async schedule: one self-linked QH as reclamation head */
    async_head.hlp      = (uint32_t)(uintptr_t)&async_head | 0x02;
    async_head.ep1      = (64u << 16) | (1u << 15) | (2u << 12); /* 64B, H, HS */
    async_head.ep2      = 0;
    async_head.cur_qtd  = 0;
    async_head.next_qtd = 1;                                       /* T: no work */
    async_head.alt_qtd  = 1;
    async_head.token    = 0;
    op_w(EHCI_ASYNCLISTADDR, (uint32_t)(uintptr_t)&async_head);
    op_w(EHCI_USBSTS, 0x3F);                                       /* ack stale status */
    op_w(EHCI_USBINTR, 0);
    op_w(EHCI_USBCMD, (8u << 16) | USBCMD_ASE | USBCMD_RS);
    if(op_wait(EHCI_USBSTS, USBSTS_ASS, USBSTS_ASS, 500))
        log_puts("ehci: WARNING async schedule not reporting active\r\n");
    else
        log_puts("ehci: async schedule running\r\n");

    op_w(EHCI_CONFIGFLAG, 1);                                      /* route ports to ehci */
    mdelay(100);                                                   /* routing settle, real ms */
    ehci_ports();
    g_up = 1;
    log_puts("ehci: probe done\r\n");
    return 0;
}
