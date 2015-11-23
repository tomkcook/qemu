#ifndef BCM2835_H
#define BCM2835_H

#include "hw/arm/arm.h"
#include "bcm2835_peripherals.h"

#define TYPE_BCM2835 "bcm2835"
#define BCM2835(obj) OBJECT_CHECK(BCM2835State, (obj), TYPE_BCM2835)

typedef struct BCM2835State {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    ARMCPU cpu;
    BCM2835PeripheralState peripherals;

    uint64_t vcram_size;
} BCM2835State;

#endif /* BCM2835_H */
