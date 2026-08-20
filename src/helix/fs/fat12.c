#include "fat12.h"
#include "log.h"
#include "usb_msd.h"
#include <stdint.h>

#define SPT            18
#define HEADS          2
#define SECTOR_SZ      512
#define FAT1_LBA       5
#define FAT_SECS       9
#define FAT2_LBA       (FAT1_LBA + FAT_SECS)
#define ROOT_LBA       (FAT2_LBA + FAT_SECS)
#define ROOT_SECS      14
#define DATA_LBA       (ROOT_LBA + ROOT_SECS)
#define SECS_PER_CLUS  1
#define ROOT_ENTRIES   224
#define TOTAL_SECTORS  2880
#define BOUNCE_PHYS    0x0800u
#define BOUNCE_PTR     ((uint8_t *)BOUNCE_PHYS)

enum {
    BIOS_CHS_READ  = 0,
    BIOS_EDD_READ  = 1,
    BIOS_CHS_WRITE = 2,
    BIOS_EDD_WRITE = 3,
    BIOS_EDD_PROBE = 4
};

typedef struct {
    uint8_t drive;
    uint8_t use_edd;
    uint8_t spt;
    uint8_t heads;
} DiskState;

static DiskState g_disk;
static uint8_t fat_buf[FAT_SECS * SECTOR_SZ];
static uint8_t root_buf[ROOT_SECS * SECTOR_SZ];
static uint8_t io_buf[SECTOR_SZ];
static int fat_loaded;

extern uint8_t bios_disk_thunk(uint8_t drive, uint8_t head, uint8_t sector,
                               uint8_t cyl, uint16_t count, uint32_t buf_phys,
                               uint8_t use_edd, uint32_t lba_lo);
extern uint8_t bios_get_geometry(uint8_t drive, uint8_t *spt_out, uint8_t *heads_out);

/* Disk backend selection (corrects the stale note this replaces):
   EDD works fine on the laptop FROM BOOT-TIME REAL MODE -- coil loads
   the kernel through it. What hangs on that machine is ANY int 13h
   from the post-PM thunk, CHS or EDD alike, even a no-transfer ah=00h
   reset (SMM legacy USB suspect). So helix trusts the coil-verified
   stash at 0x0601/0x0602: EDD when coil booted via EDD, coil CHS
   geometry when it booted via CHS, floppy defaults only if the stash
   is empty. The native USB stack in usb/ replaces this whole backend. */
static void disk_init_backend(uint8_t drive){
if(usb_msd_ready()){
g_disk.drive = drive;
log_puts("[DEBUG fat12: backend = native USB mass storage]\n");
return;
}
    g_disk.drive = drive;
    g_disk.use_edd = 0;
    g_disk.spt = SPT;
    g_disk.heads = HEADS;

    uint8_t coil_spt = *(volatile uint8_t *)0x0601;
    uint8_t coil_heads = *(volatile uint8_t *)0x0602;

    log_puts("[DEBUG fat12: disk_init_backend drive=0x");
    log_hex8(drive);
    log_puts(" coil_mode=");
    log_puts(coil_spt ? "chs" : "edd");
    log_puts("]\n");

    if(drive >= 0x80 && coil_spt == 0){
        g_disk.use_edd = 1;
        log_puts("[DEBUG fat12: using EDD/LBA backend (coil-verified)]\n");
        return;
    }

    if(coil_spt){
        g_disk.spt = coil_spt;
        g_disk.heads = coil_heads;
        log_puts("[DEBUG fat12: using coil-verified CHS geometry spt=");
        log_puti(coil_spt);
        log_puts(" heads=");
        log_puti(coil_heads);
        log_puts("]\n");
    } else {
        log_puts("[DEBUG fat12: WARNING no coil geometry, falling back to floppy defaults SPT=18 HEADS=2]\n");
    }
}

static void disk_lba_to_chs(uint32_t lba, uint8_t *cyl, uint8_t *head, uint8_t *sec){
    *sec  = (uint8_t)(lba % g_disk.spt) + 1;
    *head = (uint8_t)((lba / g_disk.spt) % g_disk.heads);
    *cyl  = (uint8_t)(lba / g_disk.spt / g_disk.heads);
}

static int disk_xfer_one(int is_write, uint32_t lba, uint32_t buf_phys){
if(usb_msd_ready()) return usb_msd_xfer(is_write, lba, buf_phys);
    uint8_t cyl = 0, head = 0, sec = 0;
    uint8_t op;
    uint8_t *bounce = BOUNCE_PTR;
    uint8_t *user = (uint8_t *)(uintptr_t)buf_phys;

    if(!g_disk.use_edd) disk_lba_to_chs(lba, &cyl, &head, &sec);
    op = is_write ? (g_disk.use_edd ? BIOS_EDD_WRITE : BIOS_CHS_WRITE)
                  : (g_disk.use_edd ? BIOS_EDD_READ  : BIOS_CHS_READ);

    if(is_write){
        for(int i=0;i<SECTOR_SZ;i++) bounce[i] = user[i];
    }

    uint8_t st = bios_disk_thunk(g_disk.drive, head, sec, cyl, 1,
                                 BOUNCE_PHYS, op, lba);
    if(st){
        log_puts("[DEBUG fat12: DISK ");
        log_puts(is_write ? "WRITE" : "READ");
        log_puts(" FAILED lba=0x");
        log_hex32(lba);
        log_puts(" chs=c");
        log_puti(cyl);
        log_puts(",h");
        log_puti(head);
        log_puts(",s");
        log_puti(sec);
        log_puts(" status=0x");
        log_hex8(st);
        log_puts("]\n");
        return -1;
    }
    if(!is_write){
        for(int i=0;i<SECTOR_SZ;i++) user[i] = bounce[i];
    }
    return 0;
}

static int disk_read_lba(uint32_t lba, uint16_t count, uint32_t buf_phys){
    while(count){
        if(disk_xfer_one(0, lba, buf_phys)){
            /* DEBUG: propagate exactly where in a multi-sector read it broke */
            log_puts("[DEBUG fat12: disk_read_lba aborted, sectors_remaining=");
            log_puti(count);
            log_puts("]\n");
            return -1;
        }
        lba++;
        count--;
        buf_phys += SECTOR_SZ;
    }
    return 0;
}

static int disk_write_lba(uint32_t lba, uint16_t count, uint32_t buf_phys){
    while(count){
        if(disk_xfer_one(1, lba, buf_phys)){
            /* DEBUG: propagate exactly where in a multi-sector write it broke */
            log_puts("[DEBUG fat12: disk_write_lba aborted, sectors_remaining=");
            log_puti(count);
            log_puts("]\n");
            return -1;
        }
        lba++;
        count--;
        buf_phys += SECTOR_SZ;
    }
    return 0;
}

static uint16_t fat12_get(uint16_t clus){
    uint32_t off = (uint32_t)clus + clus / 2;
    uint16_t v = *(uint16_t *)(fat_buf + off);
    return (clus & 1) ? (uint16_t)(v >> 4) : (uint16_t)(v & 0x0FFF);
}

static void fat12_set(uint16_t clus, uint16_t val){
    uint32_t off = (uint32_t)clus + clus / 2;
    uint16_t *p = (uint16_t *)(fat_buf + off);
    if(clus & 1) *p = (uint16_t)((*p & 0x000F) | (val << 4));
    else         *p = (uint16_t)((*p & 0xF000) | (val & 0x0FFF));
}

static uint32_t clus_to_lba(uint16_t clus){
    return DATA_LBA + (uint32_t)(clus - 2) * SECS_PER_CLUS;
}

#define TOTAL_CLUS ((uint16_t)((TOTAL_SECTORS - DATA_LBA) / SECS_PER_CLUS + 2))

static uint16_t fat12_alloc(void){
    for(uint16_t c=2;c<TOTAL_CLUS;c++){
        if(!fat12_get(c)) return c;
    }
    /* DEBUG: FAT is completely full -- worth knowing loudly, since a
     * silent 0 return here just looks like "save failed" otherwise. */
    log_puts("[DEBUG fat12: fat12_alloc FAILED -- no free clusters left, disk is full]\n");
    return 0;
}

static int fat_flush(void){
    log_puts("[DEBUG fat12: fat_flush -- writing FAT1+FAT2]\n");
    uint32_t phys = (uint32_t)(uintptr_t)fat_buf;
    if(disk_write_lba(FAT1_LBA, FAT_SECS, phys)){
        log_puts("[DEBUG fat12: fat_flush FAILED writing FAT1]\n");
        return -1;
    }
    if(disk_write_lba(FAT2_LBA, FAT_SECS, phys)){
        log_puts("[DEBUG fat12: fat_flush FAILED writing FAT2]\n");
        return -1;
    }
    log_puts("[DEBUG fat12: fat_flush ok]\n");
    return 0;
}

static int root_flush(void){
    log_puts("[DEBUG fat12: root_flush -- writing root directory]\n");
    int st = disk_write_lba(ROOT_LBA, ROOT_SECS, (uint32_t)(uintptr_t)root_buf);
    if(st) log_puts("[DEBUG fat12: root_flush FAILED]\n");
    else   log_puts("[DEBUG fat12: root_flush ok]\n");
    return st;
}

static uint8_t *root_find(const char *name11){
    uint8_t *p = root_buf;
    for(int i=0;i<ROOT_ENTRIES;i++,p+=32){
        if(!p[0] || p[0] == 0xE5) continue;
        if(p[11] & 0x08) continue;
        int match = 1;
        for(int j=0;j<11;j++){
            if(p[j] != (uint8_t)name11[j]){
                match = 0;
                break;
            }
        }
        if(match) return p;
    }
    return 0;
}

static uint8_t *root_alloc_slot(void){
    uint8_t *p = root_buf;
    for(int i=0;i<ROOT_ENTRIES;i++,p+=32){
        if(!p[0] || p[0] == 0xE5) return p;
    }
    /* DEBUG: root directory is completely full (224 entries used up) */
    log_puts("[DEBUG fat12: root_alloc_slot FAILED -- root directory full]\n");
    return 0;
}

static int fat_cache_load(void){
    log_puts("[DEBUG fat12: fat_cache_load -- reading FAT + root directory into RAM]\n");
    if(disk_read_lba(FAT1_LBA, FAT_SECS, (uint32_t)(uintptr_t)fat_buf)){
        log_puts("[DEBUG fat12: fat_cache_load FAILED reading FAT table]\n");
        return -1;
    }
    if(disk_read_lba(ROOT_LBA, ROOT_SECS, (uint32_t)(uintptr_t)root_buf)){
        log_puts("[DEBUG fat12: fat_cache_load FAILED reading root directory]\n");
        return -1;
    }
    fat_loaded = 1;
    log_puts("[DEBUG fat12: fat_cache_load ok, filesystem is ready]\n");
    return 0;
}

void fat_init(uint8_t drive){
    /* DEBUG: entry point for the whole filesystem subsystem -- every
     * boot will print this exactly once, right before disk I/O
     * starts. If this never prints, the hang is before fat_init() is
     * even called (i.e. in kernel_main() itself, before this call). */
    log_puts("[DEBUG fat12: fat_init ENTER drive=0x");
    log_hex8(drive);
    log_puts("]\n");

    fat_loaded = 0;
    disk_init_backend(drive);
    if(fat_cache_load()){
        log_puts("[DEBUG fat12: fat_init FAILED -- filesystem will be unavailable]\n");
        return;
    }
    log_puts("[DEBUG fat12: fat_init EXIT ok]\n");
}

int fat_ready(void){
    return fat_loaded;
}

int fat_load(const char *name11, void *buf, int len){
    log_puts("[DEBUG fat12: fat_load ENTER]\n");

    if(!fat_loaded){
        log_puts("[DEBUG fat12: fat_load FAILED -- filesystem not ready]\n");
        return -1;
    }

    uint8_t *e = root_find(name11);
    if(!e){
        log_puts("[DEBUG fat12: fat_load FAILED -- file not found]\n");
        return -1;
    }

    uint16_t clus = *(uint16_t *)(e + 26);
    uint32_t size = *(uint32_t *)(e + 28);

    if((int)size < len) len = (int)size;

    uint8_t *dst = (uint8_t *)buf;
    int left = len;

    while(clus >= 2 && clus < 0xFF8 && left > 0){
        uint32_t lba = clus_to_lba(clus);
        int take = (left < SECTOR_SZ) ? left : SECTOR_SZ;

        if(take == SECTOR_SZ){
            if(disk_read_lba(lba, 1, (uint32_t)(uintptr_t)dst)){
                log_puts("[DEBUG fat12: fat_load FAILED mid-file at cluster ");
                log_puti(clus);
                log_puts("]\n");
                return -1;
            }
            dst += SECTOR_SZ;
            left -= SECTOR_SZ;
        } else {
            if(disk_read_lba(lba, 1, (uint32_t)(uintptr_t)io_buf)){
                log_puts("[DEBUG fat12: fat_load FAILED on final partial sector, cluster ");
                log_puti(clus);
                log_puts("]\n");
                return -1;
            }
            for(int i=0;i<take;i++) dst[i] = io_buf[i];
            left = 0;
        }

        clus = fat12_get(clus);
    }

    log_puts("[DEBUG fat12: fat_load EXIT ok, bytes=");
    log_puti(len - left);
    log_puts("]\n");
    return len - left;
}

int fat_save(const char *name11, const void *buf, int len){
    log_puts("[DEBUG fat12: fat_save ENTER len=");
    log_puti(len);
    log_puts("]\n");

    if(!fat_loaded){
        log_puts("[DEBUG fat12: fat_save FAILED -- filesystem not ready]\n");
        return -1;
    }

    uint8_t *e = root_find(name11);
    if(e){
        uint16_t c = *(uint16_t *)(e + 26);
        while(c >= 2 && c < 0xFF8){
            uint16_t nx = fat12_get(c);
            fat12_set(c, 0);
            c = nx;
        }
    } else {
        e = root_alloc_slot();
        if(!e){
            log_puts("[DEBUG fat12: fat_save FAILED -- could not allocate directory entry]\n");
            return -1;
        }
        for(int i=0;i<32;i++) e[i] = 0;
        for(int i=0;i<11;i++) e[i] = (uint8_t)name11[i];
        e[11] = 0x20;
    }

    const uint8_t *src = (const uint8_t *)buf;
    int left = len;
    uint16_t first = 0, prev = 0;

    while(left > 0){
        uint16_t c = fat12_alloc();
        if(!c){
            log_puts("[DEBUG fat12: fat_save FAILED -- disk full mid-write]\n");
            return -1;
        }

        fat12_set(c, 0xFF8);
        if(prev) fat12_set(prev, c);
        else first = c;
        prev = c;

        int take = (left < SECTOR_SZ) ? left : SECTOR_SZ;
        if(take < SECTOR_SZ){
            for(int i=0;i<SECTOR_SZ;i++) io_buf[i] = (i < take) ? src[i] : 0;
            if(disk_write_lba(clus_to_lba(c), 1, (uint32_t)(uintptr_t)io_buf)){
                log_puts("[DEBUG fat12: fat_save FAILED writing final partial sector, cluster ");
                log_puti(c);
                log_puts("]\n");
                return -1;
            }
        } else {
            if(disk_write_lba(clus_to_lba(c), 1, (uint32_t)(uintptr_t)src)){
                log_puts("[DEBUG fat12: fat_save FAILED writing cluster ");
                log_puti(c);
                log_puts("]\n");
                return -1;
            }
        }

        src += take;
        left -= take;
    }

    *(uint16_t *)(e + 26) = first;
    *(uint32_t *)(e + 28) = (uint32_t)len;

    if(fat_flush()){
        log_puts("[DEBUG fat12: fat_save FAILED -- could not flush FAT table]\n");
        return -1;
    }
    if(root_flush()){
        log_puts("[DEBUG fat12: fat_save FAILED -- could not flush root directory]\n");
        return -1;
    }

    log_puts("[DEBUG fat12: fat_save EXIT ok]\n");
    return 0;
}

void fat_dir(void (*cb)(const char *name11, uint32_t size)){
    if(!fat_loaded){
        log_puts("[DEBUG fat12: fat_dir called but filesystem not ready]\n");
        return;
    }

    uint8_t *p = root_buf;
    char nm[12];

    for(int i=0;i<ROOT_ENTRIES;i++,p+=32){
        if(!p[0] || p[0] == 0xE5) continue;
        if(p[11] & 0x08) continue;

        for(int j=0;j<11;j++) nm[j] = (char)p[j];
        nm[11] = 0;

        cb(nm, *(uint32_t *)(p + 28));
    }
}
