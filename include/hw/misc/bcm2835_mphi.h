/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This code is licensed under the GNU GPLv2 and later.
 */

#ifndef BCM2835_MPHI_H
#define BCM2835_MPHI_H

#include "hw/sysbus.h"

#define TYPE_BCM2835_MPHI "bcm2835_mphi"
#define BCM2835_MPHI(obj) \
        OBJECT_CHECK(BCM2835MphiState, (obj), TYPE_BCM2835_MPHI)

typedef struct {
    SysBusDevice busdev;
    MemoryRegion iomem;

    uint32_t mphi_base;
    uint32_t mphi_ctrl;
    uint32_t mphi_outdda;
    uint32_t mphi_outddb;
    uint32_t mphi_intstat;

    qemu_irq irq;
} BCM2835MphiState;

#endif
