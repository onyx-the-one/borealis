#ifndef USB_MSD_H
#define USB_MSD_H
#include <stdint.h>

int usb_msd_init(uint8_t addr, uint8_t ep_in, uint16_t pkt_in,
                 uint8_t ep_out, uint16_t pkt_out);
int usb_msd_ready(void);

/* one 512-byte sector; buf_phys is a physical address (identity-mapped) */
int usb_msd_xfer(int is_write, uint32_t lba, uint32_t buf_phys);

#endif
