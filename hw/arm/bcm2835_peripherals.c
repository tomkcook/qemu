#include "hw/arm/bcm2835_peripherals.h"
#include "hw/arm/bcm2835_common.h"
#include "hw/arm/raspi_platform.h"

// XXX: FIXME:
AddressSpace *bcm2835_peripheral_as;

static void bcm2835_peripherals_init(Object *obj)
{
    BCM2835PeripheralState *s = BCM2835_PERIPHERALS(obj);
    SysBusDevice *dev;

    memory_region_init_io(&s->iomem, OBJECT(s), NULL, s, "mmio", 0x1000000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);

    /* Interrupt Controller */
    s->ic = dev = SYS_BUS_DEVICE(object_new("bcm2835_ic"));
    object_property_add_child(obj, "ic", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* UART0 */
    s->uart0 = dev = SYS_BUS_DEVICE(object_new("pl011"));
    object_property_add_child(obj, "uart0", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* UART1 */
    s->uart1 = dev = SYS_BUS_DEVICE(object_new("bcm2835_aux"));
    object_property_add_child(obj, "uart1", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

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

    /* Power management */
    s->power = dev = SYS_BUS_DEVICE(object_new("bcm2835_power"));
    object_property_add_child(obj, "power", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* Framebuffer */
    s->fb = dev = SYS_BUS_DEVICE(object_new("bcm2835_fb"));
    object_property_add_child(obj, "fb", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* Property channel */
    s->property = dev = SYS_BUS_DEVICE(object_new("bcm2835_property"));
    object_property_add_child(obj, "property", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* VCHIQ */
    s->vchiq = dev = SYS_BUS_DEVICE(object_new("bcm2835_vchiq"));
    object_property_add_child(obj, "vchiq", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* Extended Mass Media Controller */
    s->emmc = dev = SYS_BUS_DEVICE(object_new("bcm2835_emmc"));
    object_property_add_child(obj, "emmc", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());

    /* DMA Channels */
    s->dma = dev = SYS_BUS_DEVICE(object_new("bcm2835_dma"));
    object_property_add_child(obj, "dma", OBJECT(dev), NULL);
    qdev_set_parent_bus(DEVICE(dev), sysbus_get_default());
}

static void bcm2835_peripherals_realize(DeviceState *dev, Error **errp)
{
    BCM2835PeripheralState *s = BCM2835_PERIPHERALS(dev);
    qemu_irq pic[72];
    qemu_irq mbox_irq[MBOX_CHAN_COUNT];
    hwaddr tmpoffset;
    Error *err = NULL;
    int n;

    /* Interrupt Controller */
    object_property_set_bool(OBJECT(s->ic), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, ARMCTRL_IC_OFFSET,
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

    memory_region_add_subregion(&s->iomem, UART0_OFFSET,
                                sysbus_mmio_get_region(s->uart0, 0));
    sysbus_connect_irq(s->uart0, 0, pic[INTERRUPT_VC_UART]);

    /* UART1 */
    object_property_set_bool(OBJECT(s->uart1), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, UART1_OFFSET,
                                sysbus_mmio_get_region(s->uart1, 0));
    sysbus_connect_irq(s->uart1, 0, pic[INTERRUPT_AUX]);

    /* System timer */
    object_property_set_bool(OBJECT(s->systimer), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, ST_OFFSET,
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

    memory_region_add_subregion(&s->iomem, ARMCTRL_TIMER0_1_OFFSET,
                                sysbus_mmio_get_region(s->armtimer, 0));
    sysbus_connect_irq(s->armtimer, 0, pic[INTERRUPT_ARM_TIMER]);

    /* USB controller */
    object_property_set_bool(OBJECT(s->usb), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, USB_OFFSET,
                                sysbus_mmio_get_region(s->usb, 0));
    sysbus_connect_irq(s->usb, 0, pic[INTERRUPT_VC_USB]);

    /* MPHI - Message-based Parallel Host Interface */
    object_property_set_bool(OBJECT(s->mphi), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, MPHI_OFFSET,
                                sysbus_mmio_get_region(s->mphi, 0));
    sysbus_connect_irq(s->mphi, 0, pic[INTERRUPT_HOSTPORT]);

    /* Semaphores / Doorbells / Mailboxes */
    object_property_set_bool(OBJECT(s->sbm), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, ARMCTRL_0_SBM_OFFSET,
                                sysbus_mmio_get_region(s->sbm, 0));
    sysbus_connect_irq(s->sbm, 0, pic[INTERRUPT_ARM_MAILBOX]);

    for (n = 0; n < MBOX_CHAN_COUNT; n++) {
        mbox_irq[n] = qdev_get_gpio_in(DEVICE(s->sbm), n);
    }

    /* Mailbox-addressable peripherals using (hopefully) free address space
     * locations and pseudo-irqs to dispatch mailbox requests and responses
     * between them. */

    tmpoffset = ARMCTRL_0_SBM_OFFSET + 0x400;

    /* Power management */
    object_property_set_bool(OBJECT(s->power), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, tmpoffset + (MBOX_CHAN_POWER<<4),
                                sysbus_mmio_get_region(s->power, 0));
    sysbus_connect_irq(s->power, 0, mbox_irq[MBOX_CHAN_POWER]);

    /* Framebuffer */
    object_property_set_bool(OBJECT(s->fb), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, tmpoffset + (MBOX_CHAN_FB<<4),
                                sysbus_mmio_get_region(s->fb, 0));
    sysbus_connect_irq(s->fb, 0, mbox_irq[MBOX_CHAN_FB]);

    /* Property channel */
    object_property_set_bool(OBJECT(s->property), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, tmpoffset + (MBOX_CHAN_PROPERTY<<4),
                                sysbus_mmio_get_region(s->property, 0));
    sysbus_connect_irq(s->property, 0, mbox_irq[MBOX_CHAN_PROPERTY]);

    /* VCHIQ */
    object_property_set_bool(OBJECT(s->vchiq), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, tmpoffset + (MBOX_CHAN_VCHIQ<<4),
                                sysbus_mmio_get_region(s->vchiq, 0));
    sysbus_connect_irq(s->vchiq, 0, mbox_irq[MBOX_CHAN_VCHIQ]);

    /* Extended Mass Media Controller */
    object_property_set_bool(OBJECT(s->emmc), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, EMMC_OFFSET,
                                sysbus_mmio_get_region(s->emmc, 0));
    sysbus_connect_irq(s->emmc, 0, pic[INTERRUPT_VC_ARASANSDIO]);

    /* DMA Channels */
    object_property_set_bool(OBJECT(s->dma), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    memory_region_add_subregion(&s->iomem, DMA_OFFSET,
                                sysbus_mmio_get_region(s->dma, 0));
    memory_region_add_subregion(&s->iomem, 0xe05000,
                                sysbus_mmio_get_region(s->dma, 1));

    // XXX: tmp kludge
    static AddressSpace tmpas;
    address_space_init(&tmpas, &s->iomem, "bcm2835_peripheral_as");
    bcm2835_peripheral_as = &tmpas;

    sysbus_connect_irq(s->dma, 0, pic[INTERRUPT_DMA0]);
    sysbus_connect_irq(s->dma, 1, pic[INTERRUPT_DMA1]);
    sysbus_connect_irq(s->dma, 2, pic[INTERRUPT_VC_DMA2]);
    sysbus_connect_irq(s->dma, 3, pic[INTERRUPT_VC_DMA3]);
    sysbus_connect_irq(s->dma, 4, pic[INTERRUPT_DMA4]);
    sysbus_connect_irq(s->dma, 5, pic[INTERRUPT_DMA5]);
    sysbus_connect_irq(s->dma, 6, pic[INTERRUPT_DMA6]);
    sysbus_connect_irq(s->dma, 7, pic[INTERRUPT_DMA7]);
    sysbus_connect_irq(s->dma, 8, pic[INTERRUPT_DMA8]);
    sysbus_connect_irq(s->dma, 9, pic[INTERRUPT_DMA9]);
    sysbus_connect_irq(s->dma, 10, pic[INTERRUPT_DMA10]);
    sysbus_connect_irq(s->dma, 11, pic[INTERRUPT_DMA11]);
    sysbus_connect_irq(s->dma, 12, pic[INTERRUPT_DMA12]);
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
