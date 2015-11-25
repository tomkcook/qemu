/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This code is licensed under the GNU GPLv2 and later.
 */

#include "hw/arm/bcm2835_peripherals.h"
#include "hw/arm/bcm2835_mbox.h"
#include "hw/arm/raspi_platform.h"

static void bcm2835_peripherals_init(Object *obj)
{
    BCM2835PeripheralState *s = BCM2835_PERIPHERALS(obj);
    SysBusDevice *dev;

    /* Memory region for peripheral devices, which we export to our parent */
    memory_region_init_io(&s->peri_mr, OBJECT(s), NULL, s,
                          "bcm2835_peripherals", 0x1000000);
    object_property_add_child(obj, "peripheral_io", OBJECT(&s->peri_mr), NULL);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->peri_mr);

    /* Internal memory region for peripheral bus addresses (not exported) */
    memory_region_init_io(&s->gpu_bus_mr, OBJECT(s), NULL, s, "bcm2835_gpu_bus",
                          (uint64_t)1 << 32);
    object_property_add_child(obj, "gpu_bus", OBJECT(&s->gpu_bus_mr), NULL);

    /* Internal memory region for communication of mailbox channel data */
    memory_region_init_io(&s->mbox_mr, OBJECT(s), NULL, s, "bcm2835_mbox",
                          MBOX_CHAN_COUNT << 4);

    /* Interrupt Controller */
    s->ic = dev = SYS_BUS_DEVICE(object_new("bcm2835_ic"));
    object_property_add_child(obj, "ic", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* UART0 */
    s->uart0 = dev = SYS_BUS_DEVICE(object_new("pl011"));
    object_property_add_child(obj, "uart0", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* AUX / UART1 */
    object_initialize(&s->aux, sizeof(s->aux), TYPE_BCM2835_AUX);
    object_property_add_child(obj, "aux", OBJECT(&s->aux), NULL);
    qdev_set_parent_bus(DEVICE(&s->aux), sysbus_get_default());

    /* System timer */
    s->systimer = dev = SYS_BUS_DEVICE(object_new("bcm2835_st"));
    object_property_add_child(obj, "systimer", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* ARM timer */
    s->armtimer = dev = SYS_BUS_DEVICE(object_new("bcm2835_timer"));
    object_property_add_child(obj, "armtimer", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* USB controller */
    s->usb = dev = SYS_BUS_DEVICE(object_new("bcm2835_usb"));
    object_property_add_child(obj, "usb", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* MPHI - Message-based Parallel Host Interface */
    s->mphi = dev = SYS_BUS_DEVICE(object_new("bcm2835_mphi"));
    object_property_add_child(obj, "mphi", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* Semaphores / Doorbells / Mailboxes */
    s->sbm = dev = SYS_BUS_DEVICE(object_new("bcm2835_sbm"));
    object_property_add_child(obj, "sbm", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    object_property_add_const_link(OBJECT(dev), "mbox_mr",
                                   OBJECT(&s->mbox_mr), &error_abort);

    /* Power management */
    s->power = dev = SYS_BUS_DEVICE(object_new("bcm2835_power"));
    object_property_add_child(obj, "power", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* Framebuffer */
    object_initialize(&s->fb, sizeof(s->fb), TYPE_BCM2835_FB);
    object_property_add_child(obj, "fb", OBJECT(&s->fb), NULL);
    object_property_add_alias(obj, "vcram-size", OBJECT(&s->fb), "vcram-size",
                              &error_abort);
    qdev_set_parent_bus(DEVICE(&s->fb), sysbus_get_default());

    object_property_add_const_link(OBJECT(&s->fb), "dma_mr",
                                   OBJECT(&s->gpu_bus_mr), &error_abort);

    /* Property channel */
    s->property = dev = SYS_BUS_DEVICE(object_new("bcm2835_property"));
    object_property_add_child(obj, "property", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    object_property_add_const_link(OBJECT(dev), "bcm2835_fb",
                                   OBJECT(&s->fb), &error_abort);
    object_property_add_const_link(OBJECT(dev), "dma_mr",
                                   OBJECT(&s->gpu_bus_mr), &error_abort);

    /* VCHIQ */
    s->vchiq = dev = SYS_BUS_DEVICE(object_new("bcm2835_vchiq"));
    object_property_add_child(obj, "vchiq", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* Extended Mass Media Controller */
    s->emmc = dev = SYS_BUS_DEVICE(object_new("bcm2835_emmc"));
    object_property_add_child(obj, "emmc", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* DMA Channels */
    object_initialize(&s->dma, sizeof(s->dma), TYPE_BCM2835_DMA);
    object_property_add_child(obj, "dma", OBJECT(&s->dma), NULL);
    qdev_set_parent_bus(DEVICE(&s->dma), sysbus_get_default());

    object_property_add_const_link(OBJECT(&s->dma), "dma_mr",
                                   OBJECT(&s->gpu_bus_mr), &error_abort);

}

static void bcm2835_peripherals_realize(DeviceState *dev, Error **errp)
{
    BCM2835PeripheralState *s = BCM2835_PERIPHERALS(dev);
    MemoryRegion *ram;
    qemu_irq pic[72];
    qemu_irq mbox_irq[MBOX_CHAN_COUNT];
    Error *err = NULL;
    uint32_t ram_size, vcram_size;
    int n;

    /* Map peripherals and RAM into the GPU address space. */
    memory_region_init_alias(&s->peri_mr_alias, OBJECT(s),
                             "bcm2835_peripherals", &s->peri_mr, 0,
                             memory_region_size(&s->peri_mr));

    memory_region_add_subregion_overlap(&s->gpu_bus_mr, BCM2835_VC_PERI_BASE,
                                        &s->peri_mr_alias, 1);

    /* XXX: assume that RAM is contiguous and mapped at system address zero */
    ram = memory_region_find(get_system_memory(), 0, 1).mr;
    assert(ram != NULL && memory_region_size(ram) >= 128 * 1024 * 1024);
    ram_size = memory_region_size(ram);

    /* RAM is aliased four times (different cache configurations) on the GPU */
    for (n = 0; n < 4; n++) {
        memory_region_init_alias(&s->ram_alias[n], OBJECT(s),
                                 "bcm2835_gpu_ram_alias[*]", ram, 0, ram_size);
        memory_region_add_subregion_overlap(&s->gpu_bus_mr, (hwaddr)n << 30,
                                            &s->ram_alias[n], 0);
    }

    /* Interrupt Controller */
    object_property_set_bool(OBJECT(s->ic), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, ARMCTRL_IC_OFFSET,
                                sysbus_mmio_get_region(s->ic, 0));
    sysbus_pass_irq(SYS_BUS_DEVICE(s), s->ic);

    for (n = 0; n < 72; n++) {
        pic[n] = qdev_get_gpio_in(DEVICE(s->ic), n);
    }

    /* UART0 */
    object_property_set_bool(OBJECT(s->uart0), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, UART0_OFFSET,
                                sysbus_mmio_get_region(s->uart0, 0));
    sysbus_connect_irq(s->uart0, 0, pic[INTERRUPT_VC_UART]);

    /* AUX / UART1 */
    object_property_set_bool(OBJECT(&s->aux), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, UART1_OFFSET,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->aux), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->aux), 0, pic[INTERRUPT_AUX]);

    /* System timer */
    object_property_set_bool(OBJECT(s->systimer), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, ST_OFFSET,
                                sysbus_mmio_get_region(s->systimer, 0));
    sysbus_connect_irq(s->systimer, 0, pic[INTERRUPT_TIMER0]);
    sysbus_connect_irq(s->systimer, 1, pic[INTERRUPT_TIMER1]);
    sysbus_connect_irq(s->systimer, 2, pic[INTERRUPT_TIMER2]);
    sysbus_connect_irq(s->systimer, 3, pic[INTERRUPT_TIMER3]);

    /* ARM timer */
    object_property_set_bool(OBJECT(s->armtimer), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, ARMCTRL_TIMER0_1_OFFSET,
                                sysbus_mmio_get_region(s->armtimer, 0));
    sysbus_connect_irq(s->armtimer, 0, pic[INTERRUPT_ARM_TIMER]);

    /* USB controller */
    object_property_set_bool(OBJECT(s->usb), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, USB_OFFSET,
                                sysbus_mmio_get_region(s->usb, 0));
    sysbus_connect_irq(s->usb, 0, pic[INTERRUPT_VC_USB]);

    /* MPHI - Message-based Parallel Host Interface */
    object_property_set_bool(OBJECT(s->mphi), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, MPHI_OFFSET,
                                sysbus_mmio_get_region(s->mphi, 0));
    sysbus_connect_irq(s->mphi, 0, pic[INTERRUPT_HOSTPORT]);

    /* Semaphores / Doorbells / Mailboxes */
    object_property_set_bool(OBJECT(s->sbm), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, ARMCTRL_0_SBM_OFFSET,
                                sysbus_mmio_get_region(s->sbm, 0));
    sysbus_connect_irq(s->sbm, 0, pic[INTERRUPT_ARM_MAILBOX]);

    for (n = 0; n < MBOX_CHAN_COUNT; n++) {
        mbox_irq[n] = qdev_get_gpio_in(DEVICE(s->sbm), n);
    }

    /* Mailbox-addressable peripherals use the private mbox_mr address space
     * and pseudo-irqs to dispatch requests and responses. */

    /* Power management */
    object_property_set_bool(OBJECT(s->power), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->mbox_mr, MBOX_CHAN_POWER<<4,
                                sysbus_mmio_get_region(s->power, 0));
    sysbus_connect_irq(s->power, 0, mbox_irq[MBOX_CHAN_POWER]);

    /* Framebuffer */
    vcram_size = (uint32_t)object_property_get_int(OBJECT(s), "vcram-size",
                                                   &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    object_property_set_int(OBJECT(&s->fb), ram_size - vcram_size,
                            "vcram-base", &err);

    object_property_set_bool(OBJECT(&s->fb), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->mbox_mr, MBOX_CHAN_FB<<4,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->fb), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fb), 0, mbox_irq[MBOX_CHAN_FB]);

    /* Property channel */
    object_property_set_bool(OBJECT(s->property), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->mbox_mr, MBOX_CHAN_PROPERTY<<4,
                                sysbus_mmio_get_region(s->property, 0));
    sysbus_connect_irq(s->property, 0, mbox_irq[MBOX_CHAN_PROPERTY]);

    /* VCHIQ */
    object_property_set_bool(OBJECT(s->vchiq), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->mbox_mr, MBOX_CHAN_VCHIQ<<4,
                                sysbus_mmio_get_region(s->vchiq, 0));
    sysbus_connect_irq(s->vchiq, 0, mbox_irq[MBOX_CHAN_VCHIQ]);

    /* Extended Mass Media Controller */
    object_property_set_bool(OBJECT(s->emmc), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, EMMC_OFFSET,
                                sysbus_mmio_get_region(s->emmc, 0));
    sysbus_connect_irq(s->emmc, 0, pic[INTERRUPT_VC_ARASANSDIO]);

    /* DMA Channels */
    object_property_set_bool(OBJECT(&s->dma), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->peri_mr, DMA_OFFSET,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dma), 0));
    /* XXX: this address was in the original raspi port.
     * It's unclear where it is derived from. */
    memory_region_add_subregion(&s->peri_mr, 0xe05000,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dma), 1));

    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 0, pic[INTERRUPT_DMA0]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 1, pic[INTERRUPT_DMA1]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 2, pic[INTERRUPT_VC_DMA2]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 3, pic[INTERRUPT_VC_DMA3]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 4, pic[INTERRUPT_DMA4]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 5, pic[INTERRUPT_DMA5]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 6, pic[INTERRUPT_DMA6]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 7, pic[INTERRUPT_DMA7]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 8, pic[INTERRUPT_DMA8]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 9, pic[INTERRUPT_DMA9]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 10, pic[INTERRUPT_DMA10]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 11, pic[INTERRUPT_DMA11]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma), 12, pic[INTERRUPT_DMA12]);
}

static void bcm2835_peripherals_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = bcm2835_peripherals_realize;
}

static const TypeInfo bcm2835_peripherals_type_info = {
    .name = TYPE_BCM2835_PERIPHERALS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PeripheralState),
    .instance_init = bcm2835_peripherals_init,
    .class_init = bcm2835_peripherals_class_init,
};

static void bcm2835_peripherals_register_types(void)
{
    type_register_static(&bcm2835_peripherals_type_info);
}

type_init(bcm2835_peripherals_register_types)
