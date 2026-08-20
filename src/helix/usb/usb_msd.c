/* usb_msd.c — USB mass storage: Bulk-Only Transport + a small SCSI
   subset, on top of ehci bulk transfers. Polled and synchronous like
   the rest of the stack. Surfaces one-sector read/write for fat12's
   disk backend. Scope: BOT only (protocol 0x50), LUN 0, 512-byte
   blocks. Every message here is a single packet, so the endpoint data
   toggle flips exactly once per successful transfer. */
#include "usb_msd.h"
#include "usb.h"
#include "ehci.h"
#include "log.h"
#include <stdint.h>

#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint32_t tag;
    uint32_t dlen;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  clen;
    uint8_t  cb[16];
} Cbw;   /* 31 bytes */

typedef struct __attribute__((packed)) {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} Csw;   /* 13 bytes */

static uint8_t  g_addr, g_ep_in, g_ep_out;
static uint16_t g_pkt_in, g_pkt_out;
static uint8_t  g_tog_in, g_tog_out;
static uint32_t g_tag;
static int      g_ready;
static uint32_t g_blocks, g_blksz;

static Cbw cbw __attribute__((aligned(32)));
static Csw csw __attribute__((aligned(32)));
static uint8_t scsi_buf[512] __attribute__((aligned(64)));

static int msd_bulk(int in, void *data, uint16_t len){
    if(!len) return 0;
    uint8_t ep   = in ? g_ep_in : g_ep_out;
    uint16_t pkt = in ? g_pkt_in : g_pkt_out;
    uint8_t *tog = in ? &g_tog_in : &g_tog_out;
    int n = ehci_bulk(g_addr, ep, pkt, in, *tog, data, len);
    if(n < 0) return -1;
    *tog ^= 1;
    return n;
}

static int msd_clear_stall(int in){
    uint8_t ep = in ? (uint8_t)(g_ep_in | 0x80) : g_ep_out;
    if(usb_clear_halt(g_addr, ep)) return -1;
    if(in) g_tog_in = 0; else g_tog_out = 0;
    return 0;
}

/* one BOT command: CBW out, optional data phase, CSW in.
   returns the CSW status (0 ok, 1 check condition, 2 phase error),
   or -1 on transport failure. */
static int bot_raw(int in, const uint8_t *cdb, uint8_t clen, void *data, uint32_t dlen){
    cbw.sig   = CBW_SIG;
    cbw.tag   = ++g_tag;
    cbw.dlen  = dlen;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun   = 0;
    cbw.clen  = clen;
    for(int i = 0; i < 16; i++) cbw.cb[i] = i < clen ? cdb[i] : 0;

    if(msd_bulk(0, &cbw, 31) != 31){
        log_puts("usb: CBW failed\r\n");
        return -1;
    }
    if(dlen){
        int n = msd_bulk(in, data, (uint16_t)dlen);
        if(n < 0){
            log_puts("usb: data phase stalled, clearing halt\r\n");
            if(msd_clear_stall(in)) return -1;
            /* the device still owes us a CSW */
        } else if((uint32_t)n != dlen){
            log_puts("usb: short data phase\r\n");
        }
    }
    int n = msd_bulk(1, &csw, 13);
    if(n < 0){
        log_puts("usb: CSW stalled, clearing halt\r\n");
        if(msd_clear_stall(1)) return -1;
        n = msd_bulk(1, &csw, 13);
        if(n < 0) return -1;
    }
    if(n != 13 || csw.sig != CSW_SIG || csw.tag != cbw.tag){
        log_puts("usb: bad CSW\r\n");
        return -1;
    }
    return csw.status;
}

/* bot_raw + check-condition mop-up: on status 1, issue REQUEST SENSE so
   the next command starts from a clean state. */
static int bot_cmd(int in, const uint8_t *cdb, uint8_t clen, void *data, uint32_t dlen){
    int st = bot_raw(in, cdb, clen, data, dlen);
    if(st == 1){
        static const uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
        uint8_t sense[18];
        bot_raw(1, rs, 6, sense, 18);
        return -1;
    }
    if(st == 2){
        log_puts("usb: phase error, device needs a bulk reset -- giving up\r\n");
        g_ready = 0;
    }
    return st ? -1 : 0;
}

static int scsi_ready(void){
    static const uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    for(int i = 0; i < 6; i++)
        if(!bot_cmd(0, tur, 6, 0, 0)) return 0;
    return -1;
}

static int scsi_inquiry(void){
    static const uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    for(int i = 0; i < 36; i++) scsi_buf[i] = 0;
    if(bot_cmd(1, cdb, 6, scsi_buf, 36)) return -1;
    log_puts("usb: inquiry '");
    for(int i = 8; i < 32; i++){
        char c = (char)scsi_buf[i];
        log_putc((c >= 32 && c < 127) ? c : ' ');
    }
    log_puts("'\r\n");
    return 0;
}

static int scsi_capacity(void){
    static const uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if(bot_cmd(1, cdb, 10, scsi_buf, 8)) return -1;
    uint32_t last = ((uint32_t)scsi_buf[0] << 24) | ((uint32_t)scsi_buf[1] << 16) |
                    ((uint32_t)scsi_buf[2] << 8) | scsi_buf[3];
    g_blksz  = ((uint32_t)scsi_buf[4] << 24) | ((uint32_t)scsi_buf[5] << 16) |
               ((uint32_t)scsi_buf[6] << 8) | scsi_buf[7];
    g_blocks = last + 1;
    log_puts("usb: capacity ");
    log_puti((int)g_blocks);
    log_puts(" blocks x ");
    log_puti((int)g_blksz);
    log_puts("\r\n");
    if(g_blksz != 512){
        log_puts("usb: non-512 block size, unsupported\r\n");
        return -1;
    }
    return 0;
}

static int scsi_rw(int write, uint32_t lba, void *buf){
    uint8_t cdb[10];
    cdb[0] = write ? 0x2A : 0x28;
    cdb[1] = 0;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;   /* one block */
    cdb[9] = 0;
    return bot_cmd(!write, cdb, 10, buf, 512);
}

int usb_msd_init(uint8_t addr, uint8_t ep_in, uint16_t pkt_in,
                 uint8_t ep_out, uint16_t pkt_out){
    g_addr = addr; g_ep_in = ep_in; g_ep_out = ep_out;
    g_pkt_in = pkt_in; g_pkt_out = pkt_out;
    g_tog_in = g_tog_out = 0;
    g_ready = 0;

    /* ready loop first: the first command after reset usually answers
       UNIT ATTENTION, and the sense mop-up clears it */
    if(scsi_ready()){    log_puts("usb: device never became ready\r\n"); return -1; }
    if(scsi_inquiry()){  log_puts("usb: inquiry failed\r\n");            return -1; }
    if(scsi_capacity()){ log_puts("usb: capacity failed\r\n");           return -1; }

    /* prove the read path before fat12 is allowed to trust it */
    if(scsi_rw(0, 0, scsi_buf)){
        log_puts("usb: block-0 read failed\r\n");
        return -1;
    }
    if(scsi_buf[510] != 0x55 || scsi_buf[511] != 0xAA){
        log_puts("usb: block 0 has no 55AA signature, refusing\r\n");
        return -1;
    }
    g_ready = 1;
    log_puts("usb: mass storage ready\r\n");
    return 0;
}

int usb_msd_ready(void){ return g_ready; }

int usb_msd_xfer(int is_write, uint32_t lba, uint32_t buf_phys){
    if(!g_ready) return -1;
    if(lba >= g_blocks){
        log_puts("usb: xfer past end of device, lba ");
        log_hex32(lba);
        log_puts("\r\n");
        return -1;
    }
    return scsi_rw(is_write, lba, (void *)(uintptr_t)buf_phys);
}
