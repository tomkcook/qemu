/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This code is licensed under the GNU GPLv2 and later.
 */

/* Based on versatilepb.c, copyright terms below. */

/*
 * ARM Versatile Platform/Application Baseboard System emulation.
 *
 * Copyright (c) 2005-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

#include "hw/arm/bcm2835.h"
#include "hw/arm/bcm2836.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "hw/devices.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "hw/arm/raspi_platform.h"
#include "hw/arm/bcm2835_common.h"

/* Globals */
hwaddr bcm2835_vcram_base;

/* simple bootloader for pi1 */
static const uint32_t bootloader_0_pi1[] = {
    0xea000006, /* b 0x20 ; reset vector: branch to the bootloader below */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */

    0xe3a00000, /* mov r0, #0 */
    0xe3a01042, /* mov r1, #67 ; r1 = 0x42 */
    0xe3811c0c, /* orr r1, r1, #12, 24 ; r1 |= 0xc00 (Linux MACH_BCM2708) */
    0xe59f2000, /* ldr r2, [pc] ; 0x100 from below */
    0xe59ff000, /* ldr pc, [pc] ; jump to 0x8000, from below */

    /* constants */
    0x00000100, /* (Phys addr of tag list in RAM) */
    0x00008000  /* (Phys addr of kernel image entry, i.e. where we jump) */
};

/* multi-core-aware bootloader for pi2 */
static const uint32_t bootloader_0_pi2[] = {
    0xea000006, /* b 0x20 ; reset vector: branch to the bootloader below */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */
    0xe1a00000, /* nop ; (mov r0, r0) */

    /* start of bootloader */
    0xE3A03902, /*    mov   r3, #0x8000            ; boot core entry point */

    /* retrieve core ID */
    0xEE100FB0, /*    mrc   p15, 0, r0, c0, c0, 5  ; get core ID */
    0xE7E10050, /*    ubfx  r0, r0, #0, #2         ; extract LSB */
    0xE3500000, /*    cmp   r0, #0                 ; if zero, we're boot core */
    0x0A000004, /*    beq   2f */

    /* busy-wait for mailbox set on secondary cores */
    0xE59F501C, /*    ldr     r4, =0x400000CC      ; mbox 3 read/clear base */
    0xE7953200, /* 1: ldr     r3, [r4, r0, lsl #4] ; read mbox for our core */
    0xE3530000, /*    cmp     r3, #0               ; spin while zero */
    0x0AFFFFFC, /*    beq     1b */
    0xE7853200, /*    str     r3, [r4, r0, lsl #4] ; clear mbox */

    /* enter image at [r3] */
    0xE3A00000, /* 2: mov     r0, #0 */
    0xE59F1008, /*    ldr     r1, =0xc43           ; Linux MACH_BCM2709 */
    0xE3A02C01, /*    ldr     r2, =0x100           ; Address of ATAGS */
    0xE12FFF13, /*    bx      r3 */

    /* constants */
    0x400000CC,
    0x00000C43,
};

/* ATAG "tag list" in RAM at 0x100
 * ref: http://www.simtec.co.uk/products/SWLINUX/files/booting_article.html */
static uint32_t bootloader_100[] = {
    0x00000005, /* length of core tag (words) */
    0x54410001, /* ATAG_CORE */
    0x00000001, /* flags */
    0x00001000, /* page size (4k) */
    0x00000000, /* root device */
    0x00000004, /* length of mem tag (words) */
    0x54410002, /* ATAG_MEM */
    0x08000000, /* RAM size (to be overwritten by dynamic memory size) */
    0x00000000, /* start of RAM */
    0x00000000, /* "length" of none tag (magic) */
    0x00000000  /* ATAG_NONE */
};

static void init_ram(Object *parent, ram_addr_t ram_size)
{
    MemoryRegion *ram = g_new(MemoryRegion, 1);

    bcm2835_vcram_base = ram_size - VCRAM_SIZE;

    /* Write real RAM size in ATAG structure */
    bootloader_100[7] = bcm2835_vcram_base;

    memory_region_allocate_system_memory(ram, parent, "ram", ram_size);
    memory_region_add_subregion_overlap(get_system_memory(), 0, ram, 0);
}

static void setup_boot(MachineState *machine, int board_id,
                       const uint32_t *bootloader, size_t bootloader_len)
{
    static struct arm_boot_info raspi_binfo;
    int n;

    raspi_binfo.board_id = board_id;
    raspi_binfo.ram_size = bcm2835_vcram_base;

    /* If the user specified a "firmware" image (e.g. UEFI), we bypass
       the normal Linux boot process */
    if (machine->firmware) {
        /* XXX: Kludge for Windows support: put framebuffer in BGR
         * mode. We need a config switch somewhere to enable this. It
         * should ultimately be emulated by looking in config.txt (as
         * the real firmware does) for the relevant options */
        if (bootloader == bootloader_0_pi2) {
            bcm2835_fb.pixo = 0;
        }

        /* load the firmware image (typically kernel.img) at 0x8000 */
        load_image_targphys(machine->firmware,
                            0x8000,
                            bcm2835_vcram_base - 0x8000);

        /* copy over the bootloader */
        for (n = 0; n < bootloader_len; n++) {
            stl_phys(&address_space_memory, (n << 2), bootloader[n]);
        }
        for (n = 0; n < ARRAY_SIZE(bootloader_100); n++) {
            stl_phys(&address_space_memory, 0x100 + (n << 2),
                     bootloader_100[n]);
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

static void raspi_init(MachineState *machine)
{
    Object *bcm2835;
    Error *err = NULL;

    init_ram(OBJECT(machine), machine->ram_size);

    bcm2835 = object_new(TYPE_BCM2835);
    object_property_add_child(OBJECT(machine), "soc", bcm2835, &error_abort);

    object_property_set_bool(bcm2835, true, "realized", &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        exit(1);
    }

    setup_boot(machine, 0xc42, bootloader_0_pi1, ARRAY_SIZE(bootloader_0_pi1));
}

static void raspi2_init(MachineState *machine)
{
    Object *bcm2836;
    Error *err = NULL;

    init_ram(OBJECT(machine), machine->ram_size);

    bcm2836 = object_new(TYPE_BCM2836);
    object_property_add_child(OBJECT(machine), "soc", bcm2836, &error_abort);

    object_property_set_bool(bcm2836, true, "realized", &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        exit(1);
    }

    setup_boot(machine, 0xc43, bootloader_0_pi2, ARRAY_SIZE(bootloader_0_pi2));
}

static void raspi_machine_init(MachineClass *mc)
{
    mc->desc = "Raspberry Pi";
    mc->init = raspi_init;
    mc->block_default_type = IF_SD;
};
DEFINE_MACHINE("raspi", raspi_machine_init)

static void raspi2_machine_init(MachineClass *mc)
{
    mc->desc = "Raspberry Pi 2";
    mc->init = raspi2_init;
    mc->block_default_type = IF_SD;
    mc->max_cpus = BCM2836_NCPUS;
};
DEFINE_MACHINE("raspi2", raspi2_machine_init)
