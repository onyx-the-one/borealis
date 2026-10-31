/* usb.c — USB core: enumeration on top of ehci's transfer engine.
   Per-port bring-up: reset one port, enumerate its device, assign a
   unique address (msd or not) before moving on -- two devices left at
   the default address 0 would answer the next port's enumeration
   simultaneously. First BOT mass-storage device that initializes wins.
   Delays are PIT-based (timer.h), same on QEMU and real hardware.
   DMA buffers are 4KB-aligned: real ICH8/9 silicon is strict about
   page-aligned qTD buffer lists for IN transfers, QEMU is not. */
#include "usb.h"
#include "usb_msd.h"
#include "ehci.h"
#include "log.h"
#include "timer.h"
#include <stdint.h>

static void hex16(uint16_t v){ log_hex8(v >> 8); log_hex8(v & 0xFF); }

static uint8_t dbuf[64]  __attribute__((aligned(4096)));
static uint8_t cbuf[256] __attribute__((aligned(4096)));

/* walk a full config descriptor; find the first interface that is
   class 08 (mass storage) / subclass 06 (scsi transparent) /
   protocol 50 (BOT), its number, and its bulk IN/OUT endpoints. */
static int find_msd(const uint8_t *d, int len, uint8_t *iface,
                    uint8_t *ep_in, uint16_t *pkt_in,
                    uint8_t *ep_out, uint16_t *pkt_out){
    int i = 0, in_msd = 0;
    while(i + 2 <= len){
        uint8_t blen = d[i], btype = d[i + 1];
        if(blen < 2 || i + blen > len) break;
        if(btype == 4){
            in_msd = blen >= 9 && d[i+5] == 0x08 && d[i+6] == 0x06 && d[i+7] == 0x50;
            if(in_msd) *iface = d[i+2];
        } else if(btype == 5 && in_msd && blen >= 7){
            uint8_t  ea = d[i+2];
            uint16_t mp = d[i+4] | ((uint16_t)d[i+5] << 8);
            if((d[i+3] & 3) == 2){
                if(ea & 0x80){ *ep_in = ea & 0x0F; *pkt_in = mp; }
                else           { *ep_out = ea;       *pkt_out = mp; }
            }
        }
        i += blen;
    }
    return (*ep_in && *ep_out) ? 0 : -1;
}

int usb_clear_halt(uint8_t addr, uint8_t ep){
    uint8_t setup[8] = { 0x02, 0x01, 0x00, 0x00, ep, 0x00, 0x00, 0x00 };
    return ehci_control(addr, setup, 0, 0);
}

/* enumerate the device at address 0 on a just-reset port, give it a
   unique address, and if it is a BOT mass-storage device, configure it
   and init the msd layer. returns 0 only when a usable stick resulted. */
static int enum_port(uint8_t port, uint8_t addr){
    static const uint8_t get_dev_desc[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
    if(ehci_control(0, get_dev_desc, dbuf, 18)){
        log_puts("usb: GET_DESCRIPTOR(device) failed\r\n");
        return -1;
    }
    if(dbuf[0] != 18 || dbuf[1] != 1){
        log_puts("usb: malformed device descriptor, first bytes 0x");
        log_hex8(dbuf[0]); log_hex8(dbuf[1]); log_puts("\r\n");
        return -1;
    }
    uint16_t vid = dbuf[8] | ((uint16_t)dbuf[9] << 8);
    uint16_t pid = dbuf[10] | ((uint16_t)dbuf[11] << 8);
    log_puts("usb: port "); log_puti(port);
    log_puts(" device vid=0x"); hex16(vid);
    log_puts(" pid=0x"); hex16(pid);
    log_puts(" usb="); log_hex8(dbuf[3]); log_hex8(dbuf[2]);
    log_puts(" class=0x"); log_hex8(dbuf[4]);
    log_puts(" maxpkt0="); log_puti(dbuf[7]);
    log_puts("\r\n");

    uint8_t sa[8] = { 0x00, 0x05, addr, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if(ehci_control(0, sa, 0, 0)){
        log_puts("usb: SET_ADDRESS failed\r\n");
        return -1;
    }
    mdelay(10); /* device applies its address after the status stage */
    log_puts("usb: address "); log_puti(addr); log_puts(" assigned\r\n");

    static const uint8_t get_cfg[8] = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0x09, 0x00 };
    if(ehci_control(addr, get_cfg, cbuf, 9)){
        log_puts("usb: GET_DESCRIPTOR(config header) failed\r\n");
        return -1;
    }
    uint16_t total = cbuf[2] | ((uint16_t)cbuf[3] << 8);
    if(total > sizeof(cbuf)) total = sizeof(cbuf);
    uint8_t get_cfg_full[8] = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00,
                                (uint8_t)total, (uint8_t)(total >> 8) };
    if(ehci_control(addr, get_cfg_full, cbuf, total)){
        log_puts("usb: GET_DESCRIPTOR(config full) failed\r\n");
        return -1;
    }

    uint8_t iface = 0, ep_in = 0, ep_out = 0;
    uint16_t pkt_in = 0, pkt_out = 0;
    if(find_msd(cbuf, total, &iface, &ep_in, &pkt_in, &ep_out, &pkt_out)){
        log_puts("usb: not a BOT mass-storage device (class 0x");
        log_hex8(dbuf[4]); log_puts("), skipping\r\n");
        return -1;
    }
    log_puts("usb: msd interface ");
    log_puti(iface);
    log_puts(", bulk-in ep");
    log_puti(ep_in); log_puts(" ("); log_puti(pkt_in); log_puts("B), bulk-out ep");
    log_puti(ep_out); log_puts(" ("); log_puti(pkt_out); log_puts("B)\r\n");

    uint8_t sc[8] = { 0x00, 0x09, cbuf[5], 0x00, 0x00, 0x00, 0x00, 0x00 };
    if(ehci_control(addr, sc, 0, 0)){
        log_puts("usb: SET_CONFIGURATION failed\r\n");
        return -1;
    }
    log_puts("usb: configuration set\r\n");
    mdelay(10);

    return usb_msd_init(addr, iface, ep_in, pkt_in, ep_out, pkt_out);
}

int usb_probe(void){
    if(ehci_probe()) return -1; /* logs its own reasons */
    uint8_t next_addr = 1;
    int found = 0;
    for(uint8_t p = 0; p < ehci_port_count(); p++){
        if(!ehci_port_reset(p)) continue;
        found = 1;
        mdelay(50); /* TRSTRCY: let the device recover from the port reset */
        if(!enum_port(p, next_addr++)) return 0;
    }
    if(!found) log_puts("usb: no high-speed device on ehci ports\r\n");
    return -1;
}
