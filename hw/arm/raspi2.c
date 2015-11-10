/*
 * Rasperry Pi 2 emulation
 * Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * Based on raspi.c (Raspberry Pi 1 emulation), copyright terms below:
 *
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 * This code is licensed under the GNU GPLv2 and later.
 * Based on versatilepb.c, copyright terms below.
 *
 * ARM Versatile Platform/Application Baseboard System emulation.
 *
 * Copyright (c) 2005-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "hw/boards.h"
#include "hw/devices.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "hw/arm/bcm2836_platform.h"
#include "hw/arm/bcm2835_common.h"
#include "net/net.h"

#define BUS_ADDR(x) (((x) - BCM2708_PERI_BASE) + 0x7e000000)

static const uint32_t bootloader_0[] = {
    0xea000006, // b 0x20 ; reset vector: branch to the bootloader below
    0xe1a00000, // nop ; (mov r0, r0)
    0xe1a00000, // nop ; (mov r0, r0)
    0xe1a00000, // nop ; (mov r0, r0)
    0xe1a00000, // nop ; (mov r0, r0)
    0xe1a00000, // nop ; (mov r0, r0)
    0xe1a00000, // nop ; (mov r0, r0)
    0xe1a00000, // nop ; (mov r0, r0)

    /* start of bootloader */
    0xE3A03902, //    mov   r3, #0x8000            ; entry point for primary core

    /* retrieve core ID */
    0xEE100FB0, //    mrc   p15, 0, r0, c0, c0, 5  ; get core ID
    0xE7E10050, //    ubfx  r0, r0, #0, #2         ; extract LSB
    0xE3500000, //    cmp   r0, #0                 ; if zero, we're the primary
    0x0A000004, //    beq   2f

    /* busy-wait for mailbox set on secondary cores */
    0xE59F501C, //    ldr     r4, =0x400000CC      ; mailbox 3 read/clear base
    0xE7953200, // 1: ldr     r3, [r4, r0, lsl #4] ; read mailbox for our core
    0xE3530000, //    cmp     r3, #0               ; spin while zero
    0x0AFFFFFC, //    beq     1b
    0xE7853200, //    str     r3, [r4, r0, lsl #4] ; clear mailbox

    /* enter image at [r3] */
    0xE3A00000, // 2: mov     r0, #0
    0xE59F1008, //    ldr     r1, =0xc43           ; Linux machine type MACH_BCM2709 = 0xc43
    0xE3A02C01, //    ldr     r2, =0x100           ; Address of ATAGS
    0xE12FFF13, //    bx      r3

    /* constants */
    0x400000CC,
    0x00000C43,
};

static uint32_t bootloader_100[] = { // this is the "tag list" in RAM at 0x100
    // ref: http://www.simtec.co.uk/products/SWLINUX/files/booting_article.html
    0x00000005, // length of core tag (words)
    0x54410001, // ATAG_CORE
    0x00000001, // flags
    0x00001000, // page size (4k)
    0x00000000, // root device
    0x00000004, // length of mem tag (words)
    0x54410002, // ATAG_MEM
    /* It will be overwritten by dynamically calculated memory size */
    0x08000000, // RAM size (to be overwritten)
    0x00000000, // start of RAM
    0x00000000, // "length" of none tag (magic)
    0x00000000  // ATAG_NONE
};

static struct arm_boot_info raspi_binfo;

static void init_cpus(const char *cpu_model, DeviceState *icdev)
{
    ObjectClass *cpu_oc = cpu_class_by_name(TYPE_ARM_CPU, cpu_model);
    int n;

    if (!cpu_oc) {
        fprintf(stderr, "Unable to find CPU definition\n");
        exit(1);
    }

    for (n = 0; n < smp_cpus; n++) {
        Object *cpu = object_new(object_class_get_name(cpu_oc));
        Error *err = NULL;

        /* Mirror bcm2836, which has clusterid set to 0xf */
        ARM_CPU(cpu)->mp_affinity = 0xF00 | n;

        /* set periphbase/CBAR value for CPU-local registers */
        object_property_set_int(cpu, MCORE_BASE,
                                "reset-cbar", &error_abort);

        object_property_set_bool(cpu, true, "realized", &err);
        if (err) {
            error_report_err(err);
            exit(1);
        }
        
        /* Connect irq/fiq outputs from the interrupt controller. */
        qdev_connect_gpio_out_named(icdev, "irq", n,
                                    qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
        qdev_connect_gpio_out_named(icdev, "fiq", n,
                                    qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));
    
        /* Connect timers from the CPU to the interrupt controller */
        ARM_CPU(cpu)->gt_timer_outputs[GTIMER_PHYS]
            = qdev_get_gpio_in_named(icdev, "cntpsirq", 0);
        ARM_CPU(cpu)->gt_timer_outputs[GTIMER_VIRT]
            = qdev_get_gpio_in_named(icdev, "cntvirq", 0);
    }
}

static void raspi2_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();

    MemoryRegion *bcm2835_ram = g_new(MemoryRegion, 1);
    MemoryRegion *bcm2835_vcram = g_new(MemoryRegion, 1);

    MemoryRegion *ram_alias = g_new(MemoryRegion, 4);
    MemoryRegion *vcram_alias = g_new(MemoryRegion, 4);

    MemoryRegion *per_todo_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_ic_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_control_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_uart0_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_uart1_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_st_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_sbm_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_power_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_fb_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_prop_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_vchiq_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_emmc_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_dma1_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_dma2_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_timer_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_usb_bus = g_new(MemoryRegion, 1);
    MemoryRegion *per_mphi_bus = g_new(MemoryRegion, 1);

    MemoryRegion *mr;

    qemu_irq pic[72];
    qemu_irq mbox_irq[MBOX_CHAN_COUNT];

    DeviceState *dev, *icdev;
    SysBusDevice *s;

    int n;

    bcm2835_vcram_base = machine->ram_size - VCRAM_SIZE;

    /* Write real RAM size in ATAG structure */
    bootloader_100[7] = bcm2835_vcram_base;

    memory_region_allocate_system_memory(bcm2835_ram, NULL, "raspi.ram",
                                         bcm2835_vcram_base);

    memory_region_allocate_system_memory(bcm2835_vcram, NULL, "vcram.ram",
                                         VCRAM_SIZE);

    memory_region_add_subregion(sysmem, (0 << 30), bcm2835_ram);
    memory_region_add_subregion(sysmem, (0 << 30) + bcm2835_vcram_base,
        bcm2835_vcram);
    for (n = 1; n < 4; n++) {
        memory_region_init_alias(&ram_alias[n], NULL, NULL, bcm2835_ram,
            0, bcm2835_vcram_base);
        memory_region_init_alias(&vcram_alias[n], NULL, NULL, bcm2835_vcram,
            0, VCRAM_SIZE);
        memory_region_add_subregion(sysmem, (n << 30), &ram_alias[n]);
        memory_region_add_subregion(sysmem, (n << 30) + bcm2835_vcram_base,
            &vcram_alias[n]);
    }

    /* (Yet) unmapped I/O registers */
    dev = sysbus_create_simple("bcm2835_todo", BCM2708_PERI_BASE, NULL);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_todo_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(BCM2708_PERI_BASE),
        per_todo_bus);

    /* Interrupt Controllers: BCM2835 chains to the new 2836 controller */
    icdev = dev = sysbus_create_varargs("bcm2836_control", 0x40000000, NULL);

    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_control_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(0x40000000),
        per_control_bus);

    /* Create the child controller, which handles all the devices */
    dev = sysbus_create_varargs("bcm2835_ic", ARMCTRL_IC_BASE,
                                qdev_get_gpio_in_named(icdev, "gpu_irq", 0),
                                qdev_get_gpio_in_named(icdev, "gpu_fiq", 0),
                                NULL);

    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_ic_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(ARMCTRL_IC_BASE),
        per_ic_bus);
    
    for (n = 0; n < 72; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    /* Create the CPUs, and wire them up to the interrupt controller */
    if (!machine->cpu_model) {
        machine->cpu_model = "cortex-a15"; /* Closest architecturally to the A7 */
    }

    init_cpus(machine->cpu_model, icdev);
    
    /* UART0 */
    dev = sysbus_create_simple("pl011", UART0_BASE, pic[INTERRUPT_VC_UART]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_uart0_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(UART0_BASE),
        per_uart0_bus);

    /* UART1 */
    dev = sysbus_create_simple("bcm2835_aux", UART1_BASE, pic[INTERRUPT_AUX]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_uart1_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(UART1_BASE),
        per_uart1_bus);

    /* System timer */
    dev = sysbus_create_varargs("bcm2835_st", ST_BASE,
            pic[INTERRUPT_TIMER0], pic[INTERRUPT_TIMER1],
            pic[INTERRUPT_TIMER2], pic[INTERRUPT_TIMER3],
            NULL);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_st_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(ST_BASE),
        per_st_bus);

    /* ARM timer */
    dev = sysbus_create_simple("bcm2835_timer", ARMCTRL_TIMER0_1_BASE,
        pic[INTERRUPT_ARM_TIMER]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_timer_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(ARMCTRL_TIMER0_1_BASE),
        per_timer_bus);

    /* USB controller */
    dev = sysbus_create_simple("bcm2835_usb", USB_BASE,
        pic[INTERRUPT_VC_USB]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_usb_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(USB_BASE),
        per_usb_bus);

    /* MPHI - Message-based Parallel Host Interface */
    dev = sysbus_create_simple("bcm2835_mphi", MPHI_BASE,
        pic[INTERRUPT_HOSTPORT]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_mphi_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(MPHI_BASE),
        per_mphi_bus);


    /* Semaphores / Doorbells / Mailboxes */
    dev = sysbus_create_simple("bcm2835_sbm", ARMCTRL_0_SBM_BASE,
        pic[INTERRUPT_ARM_MAILBOX]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_sbm_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(ARMCTRL_0_SBM_BASE),
        per_sbm_bus);

    for (n = 0; n < MBOX_CHAN_COUNT; n++) {
        mbox_irq[n] = qdev_get_gpio_in(dev, n);
    }

    /* Mailbox-addressable peripherals using (hopefully) free address space */
    /* locations and pseudo-irqs to dispatch mailbox requests and responses */
    /* between them. */

    /* Power management */
    dev = sysbus_create_simple("bcm2835_power",
        ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_POWER<<4),
        mbox_irq[MBOX_CHAN_POWER]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_power_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem,
        BUS_ADDR(ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_POWER<<4)),
        per_power_bus);

    /* Framebuffer */
    dev = sysbus_create_simple("bcm2835_fb",
        ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_FB<<4),
        mbox_irq[MBOX_CHAN_FB]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_fb_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem,
        BUS_ADDR(ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_FB<<4)),
        per_fb_bus);

    /* Property channel */
    dev = sysbus_create_simple("bcm2835_property",
        ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_PROPERTY<<4),
        mbox_irq[MBOX_CHAN_PROPERTY]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_prop_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem,
        BUS_ADDR(ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_PROPERTY<<4)),
        per_prop_bus);

    /* VCHIQ */
    dev = sysbus_create_simple("bcm2835_vchiq",
        ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_VCHIQ<<4),
        mbox_irq[MBOX_CHAN_VCHIQ]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_vchiq_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem,
        BUS_ADDR(ARMCTRL_0_SBM_BASE + 0x400 + (MBOX_CHAN_VCHIQ<<4)),
        per_vchiq_bus);

    /* Extended Mass Media Controller */
    dev = sysbus_create_simple("bcm2835_emmc", EMMC_BASE,
        pic[INTERRUPT_VC_ARASANSDIO]);
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_emmc_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(EMMC_BASE),
        per_emmc_bus);

    /* DMA Channels */
    dev = qdev_create(NULL, "bcm2835_dma");
    s = SYS_BUS_DEVICE(dev);
    qdev_init_nofail(dev);
    sysbus_mmio_map(s, 0, DMA_BASE);
    sysbus_mmio_map(s, 1, (BCM2708_PERI_BASE + 0xe05000));
    s = SYS_BUS_DEVICE(dev);
    mr = sysbus_mmio_get_region(s, 0);
    memory_region_init_alias(per_dma1_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(DMA_BASE),
        per_dma1_bus);
    mr = sysbus_mmio_get_region(s, 1);
    memory_region_init_alias(per_dma2_bus, NULL, NULL, mr,
        0, memory_region_size(mr));
    memory_region_add_subregion(sysmem, BUS_ADDR(BCM2708_PERI_BASE + 0xe05000),
        per_dma2_bus);
    sysbus_connect_irq(s, 0, pic[INTERRUPT_DMA0]);
    sysbus_connect_irq(s, 1, pic[INTERRUPT_DMA1]);
    sysbus_connect_irq(s, 2, pic[INTERRUPT_VC_DMA2]);
    sysbus_connect_irq(s, 3, pic[INTERRUPT_VC_DMA3]);
    sysbus_connect_irq(s, 4, pic[INTERRUPT_DMA4]);
    sysbus_connect_irq(s, 5, pic[INTERRUPT_DMA5]);
    sysbus_connect_irq(s, 6, pic[INTERRUPT_DMA6]);
    sysbus_connect_irq(s, 7, pic[INTERRUPT_DMA7]);
    sysbus_connect_irq(s, 8, pic[INTERRUPT_DMA8]);
    sysbus_connect_irq(s, 9, pic[INTERRUPT_DMA9]);
    sysbus_connect_irq(s, 10, pic[INTERRUPT_DMA10]);
    sysbus_connect_irq(s, 11, pic[INTERRUPT_DMA11]);
    sysbus_connect_irq(s, 12, pic[INTERRUPT_DMA12]);

    /* XXX: this is not present on a real pi, it's a kludge for Windows NIC/debug */
    if (nd_table[0].used) {
        lan9118_init(&nd_table[0], 0x3F900000, NULL); // no interrupt (yet)
    }
    
    /* Finally, the board itself */
    raspi_binfo.ram_size = bcm2835_vcram_base;
    raspi_binfo.board_id = 0xc43; // Linux MACH_BCM2709

    /* If the user specified a "firmware" image (e.g. UEFI), we bypass
       the normal Linux boot process */
    if (machine->firmware) {
        /* XXX: Kludge for Windows support: put framebuffer in BGR
         * mode. We need a config switch somewhere to enable this. It
         * should ultimately be emulated by looking in config.txt (as
         * the real firmware does) for the relevant options */
        bcm2835_fb.pixo = 0;

        /* load the firmware image (typically kernel.img) at 0x8000 */
        load_image_targphys(machine->firmware,
                            0x8000,
                            bcm2835_vcram_base - 0x8000);

        /* copy over the bootloader */
        for (n = 0; n < ARRAY_SIZE(bootloader_0); n++) {
            stl_phys(&address_space_memory, (n << 2), bootloader_0[n]);
        }
        for (n = 0; n < ARRAY_SIZE(bootloader_100); n++) {
            stl_phys(&address_space_memory, 0x100 + (n << 2), bootloader_100[n]);
        }

        /* set variables so arm_load_kernel does the right thing */
        raspi_binfo.is_linux = false;
        raspi_binfo.entry = 0x20;
        raspi_binfo.firmware_loaded = true;
    } else {
        /* Just let arm_load_kernel do everything for us... */
        raspi_binfo.kernel_filename = machine->kernel_filename;
        raspi_binfo.kernel_cmdline = machine->kernel_cmdline;
        raspi_binfo.initrd_filename = machine->initrd_filename;
    }

    arm_load_kernel(ARM_CPU(first_cpu), &raspi_binfo);
}

static QEMUMachine raspi2_machine = {
    .name = "raspi2",
    .desc = "Raspberry Pi 2",
    .init = raspi2_init,
    .block_default_type = IF_SD,
    .max_cpus = 4,
};

static void raspi2_machine_init(void)
{
    qemu_register_machine(&raspi2_machine);
}

machine_init(raspi2_machine_init);
