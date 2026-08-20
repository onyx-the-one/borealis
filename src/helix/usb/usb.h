#ifndef USB_H
#define USB_H
#include <stdint.h>

/* single entry point for the whole USB stack: ehci probe + enumeration
   + mass-storage init. returns 0 with a usable device, -1 otherwise
   (reasons logged). */
int usb_probe(void);

/* standard CLEAR_FEATURE(ENDPOINT_HALT); also resets the host-side
   data toggle expectation for that endpoint (caller owns that) */
int usb_clear_halt(uint8_t addr, uint8_t ep);

#endif
