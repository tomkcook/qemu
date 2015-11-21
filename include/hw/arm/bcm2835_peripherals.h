#include "qemu-common.h"
#include "hw/sysbus.h"

#ifndef BCM2835_PERIPHERALS_H
#define BCM2835_PERIPHERALS_H

#define TYPE_BCM2835_PERIPHERALS "bcm2835_peripherals"
#define BCM2835_PERIPHERALS(obj) \
    OBJECT_CHECK(BCM2835PeripheralState, (obj), TYPE_BCM2835_PERIPHERALS)

typedef struct BCM2835PeripheralState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    qemu_irq irq, fiq;

    SysBusDevice *ic, *uart0, *uart1, *systimer, *armtimer, *usb, *mphi, *sbm,
        *power, *fb, *property, *vchiq, *emmc, *dma;
} BCM2835PeripheralState;

#endif /* BCM2835_PERIPHERALS_H */
