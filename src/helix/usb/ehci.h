#ifndef EHCI_H
#define EHCI_H
#include <stdint.h>

/* capability registers, offsets from BAR */
#define EHCI_CAPLENGTH   0x00
#define EHCI_HCIVERSION  0x02
#define EHCI_HCSPARAMS   0x04
#define EHCI_HCCPARAMS   0x08

/* operational registers, offsets from BAR + CAPLENGTH */
#define EHCI_USBCMD          0x00
#define EHCI_USBSTS          0x04
#define EHCI_USBINTR         0x08
#define EHCI_FRINDEX         0x0C
#define EHCI_CTRLDSSTRUCT    0x10
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR   0x18
#define EHCI_CONFIGFLAG      0x40
#define EHCI_PORTSC(n)       (0x44 + 4 * (n))

#define USBCMD_RS      (1u << 0)
#define USBCMD_HCRESET (1u << 1)
#define USBCMD_IAA     (1u << 6)
#define USBCMD_PSE     (1u << 4)
#define USBCMD_ASE     (1u << 5)

#define USBSTS_IAA      (1u << 5)
#define USBSTS_HCHALTED (1u << 12)
#define USBSTS_ASS      (1u << 15)

#define PORTSC_CCS  (1u << 0)
#define PORTSC_CSC  (1u << 1)
#define PORTSC_PED  (1u << 2)
#define PORTSC_PEDC (1u << 3)
#define PORTSC_OCC  (1u << 5)
#define PORTSC_PR   (1u << 8)
#define PORTSC_PP   (1u << 12)
#define PORTSC_RWC  (PORTSC_CSC | PORTSC_PEDC | PORTSC_OCC)

/* queue head: 17 hardware dwords, padded to a 32-byte multiple.
   next_qtd/alt_qtd/token/buf form the overlay area the controller
   scribbles on while executing a qTD. */
typedef struct {
    volatile uint32_t hlp;       /* bit 2:1 = 01 (QH), bit 0 = T */
    volatile uint32_t ep1;       /* RL|MaxPacket|H|C|EPS|EndPt|I|Addr */
    volatile uint32_t ep2;       /* Mult|Port|Hub|C-mask|S-mask */
    volatile uint32_t cur_qtd;
    volatile uint32_t next_qtd;
    volatile uint32_t alt_qtd;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    volatile uint32_t extbuf[5]; /* 64-bit high halves, unused here */
    volatile uint32_t pad[7];
} EhciQH;

/* transfer descriptor: exactly 32 bytes, 32-byte aligned */
typedef struct {
    volatile uint32_t next;
    volatile uint32_t alt;
    volatile uint32_t token;
    volatile uint32_t buf[5];
} EhciTD;

int ehci_probe(void);
int ehci_device_present(void);

/* synchronous EP0 control transfer at the given device address.
   setup is the 8-byte setup packet; data/len describe the data stage
   (len 0 = no-data control). direction comes from setup bit 7.
   returns 0 on success, -1 on timeout/stall/error (details logged). */
int ehci_control(uint8_t addr, const uint8_t setup[8], void *data, uint16_t len);

/* synchronous bulk transfer, single qTD. toggle is the endpoint's
   current data toggle (the caller tracks it per endpoint).
   returns bytes actually transferred, -1 on error/timeout. */
int ehci_bulk(uint8_t addr, uint8_t ep, uint16_t maxpkt, int dir_in,
              int toggle, void *data, uint16_t len);

#endif
