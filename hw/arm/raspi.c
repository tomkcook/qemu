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

typedef struct RaspiMachineState {
    union {
        Object obj;
        BCM2835State pi1;
        BCM2836State pi2;
    } soc;
    MemoryRegion ram;
} RaspiMachineState;

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

static void setup_boot(MachineState *machine, int board_id, size_t ram_size,
                       const uint32_t *bootloader, size_t bootloader_len)
{
    static struct arm_boot_info raspi_binfo;
    int n;

    raspi_binfo.board_id = board_id;
    raspi_binfo.ram_size = ram_size;

    /* If the user specified a "firmware" image (e.g. UEFI), we bypass
       the normal Linux boot process */
    if (machine->firmware) {
        /* load the firmware image (typically kernel.img) at 0x8000 */
        load_image_targphys(machine->firmware, 0x8000, ram_size - 0x8000);

        /* Write RAM size in ATAG structure */
        bootloader_100[7] = ram_size;

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

static void raspi_machine_init(MachineState *machine, int version)
{
    RaspiMachineState *s = g_new0(RaspiMachineState, 1);
    uint32_t vcram_size;
    Error *err = NULL;

    /* Initialise the relevant SOC */
    assert(version == 1 || version == 2);
    switch (version) {
    case 1:
        object_initialize(&s->soc.pi1, sizeof(s->soc.pi1), TYPE_BCM2835);
        break;
    case 2:
        object_initialize(&s->soc.pi2, sizeof(s->soc.pi2), TYPE_BCM2836);
        break;
    }

    object_property_add_child(OBJECT(machine), "soc", &s->soc.obj,
                              &error_abort);

    /* Allocate and map RAM */
    memory_region_allocate_system_memory(&s->ram, OBJECT(machine), "ram",
                                         machine->ram_size);
    memory_region_add_subregion_overlap(get_system_memory(), 0, &s->ram, 0);

    /* Setup the SOC */
    object_property_set_bool(&s->soc.obj, true, "realized", &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        exit(1);
    }

    vcram_size = object_property_get_int(&s->soc.obj, "vcram-size", &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        exit(1);
    }

    /* Boot! */
    switch (version) {
    case 1:
        setup_boot(machine, 0xc42, machine->ram_size - vcram_size,
                   bootloader_0_pi1, ARRAY_SIZE(bootloader_0_pi1));
        break;
    case 2:
        setup_boot(machine, 0xc43, machine->ram_size - vcram_size,
                   bootloader_0_pi2, ARRAY_SIZE(bootloader_0_pi2));
        break;
    }
}

static void raspi1_init(MachineState *machine)
{
    raspi_machine_init(machine, 1);
}

static void raspi2_init(MachineState *machine)
{
    raspi_machine_init(machine, 2);
}

static void raspi1_machine_init(MachineClass *mc)
{
    mc->desc = "Raspberry Pi";
    mc->init = raspi1_init;
    mc->block_default_type = IF_SD;
};
DEFINE_MACHINE("raspi", raspi1_machine_init)

static void raspi2_machine_init(MachineClass *mc)
{
    mc->desc = "Raspberry Pi 2";
    mc->init = raspi2_init;
    mc->block_default_type = IF_SD;
    mc->max_cpus = BCM2836_NCPUS;
};
DEFINE_MACHINE("raspi2", raspi2_machine_init)
