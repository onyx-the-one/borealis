/* usb.c — USB core: enumeration on top of ehci's transfer engine.
   Drop 2: address-0 GET_DESCRIPTOR. Drop 3 (this version): full
   enumeration — SET_ADDRESS, config descriptor walk for a BOT
   mass-storage interface, SET_CONFIGURATION — then usb_msd_init.
   Delays are PIT-based (timer.h), same on QEMU and real hardware. */
#include "usb.h"
#include "usb_msd.h"
#include "ehci.h"
#include "log.h"
#include "timer.h"
#include <stdint.h>

static void hex16(uint16_t v){ log_hex8(v >> 8); log_hex8(v & 0xFF); }

static uint8_t dbuf[64]  __attribute__((aligned(64)));
static uint8_t cbuf[256] __attribute__((aligned(64)));

/* walk a full config descriptor; find the first interface that is
   class 08 (mass storage) / subclass 06 (scsi transparent) /
   protocol 50 (BOT) and its bulk IN/OUT endpoints. */
static int find_msd(const uint8_t *d, int len,
                    uint8_t *ep_in, uint16_t *pkt_in,
                    uint8_t *ep_out, uint16_t *pkt_out){
    int i = 0, in_msd = 0;
    while(i + 2 <= len){
        uint8_t blen = d[i], btype = d[i + 1];
        if(blen < 2 || i + blen > len) break;
        if(btype == 4){
            in_msd = blen >= 9 && d[i+5] == 0x08 && d[i+6] == 0x06 && d[i+7] == 0x50;
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

int usb_probe(void){
    if(ehci_probe()) return -1; /* logs its own reasons */
    if(!ehci_device_present()){
        log_puts("usb: no high-speed device on ehci ports\r\n");
        return -1;
    }
    mdelay(50); /* TRSTRCY: let the device recover from the port reset */

    static const uint8_t get_dev_desc[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
    if(ehci_control(0, get_dev_desc, dbuf, 18)){
        log_puts("usb: GET_DESCRIPTOR(device) failed at address 0\r\n");
        return -1;
    }
    if(dbuf[0] != 18 || dbuf[1] != 1){
        log_puts("usb: malformed device descriptor, first bytes 0x");
        log_hex8(dbuf[0]); log_hex8(dbuf[1]); log_puts("\r\n");
        return -1;
    }
    uint16_t vid = dbuf[8] | ((uint16_t)dbuf[9] << 8);
    uint16_t pid = dbuf[10] | ((uint16_t)dbuf[11] << 8);
    log_puts("usb: device vid=0x"); hex16(vid);
    log_puts(" pid=0x"); hex16(pid);
    log_puts(" usb="); log_hex8(dbuf[3]); log_hex8(dbuf[2]);
    log_puts(" class=0x"); log_hex8(dbuf[4]);
    log_puts(" maxpkt0="); log_puti(dbuf[7]);
    log_puts(" configs="); log_puti(dbuf[17]);
    log_puts("\r\n");

    static const uint8_t set_addr[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if(ehci_control(0, set_addr, 0, 0)){
        log_puts("usb: SET_ADDRESS failed\r\n");
        return -1;
    }
    mdelay(10); /* device applies its address after the status stage */
    uint8_t addr = 1;
    log_puts("usb: address 1 assigned\r\n");

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

    uint8_t ep_in = 0, ep_out = 0;
    uint16_t pkt_in = 0, pkt_out = 0;
    if(find_msd(cbuf, total, &ep_in, &pkt_in, &ep_out, &pkt_out)){
        log_puts("usb: no BOT mass-storage interface found\r\n");
        return -1;
    }
    log_puts("usb: msd interface, bulk-in ep");
    log_puti(ep_in); log_puts(" ("); log_puti(pkt_in); log_puts("B), bulk-out ep");
    log_puti(ep_out); log_puts(" ("); log_puti(pkt_out); log_puts("B)\r\n");

    uint8_t set_cfg[8] = { 0x00, 0x09, cbuf[5], 0x00, 0x00, 0x00, 0x00, 0x00 };
    if(ehci_control(addr, set_cfg, 0, 0)){
        log_puts("usb: SET_CONFIGURATION failed\r\n");
        return -1;
    }
    log_puts("usb: configuration set\r\n");
    mdelay(10);

    return usb_msd_init(addr, ep_in, pkt_in, ep_out, pkt_out);
}
