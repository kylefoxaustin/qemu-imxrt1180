/*
 * NXP MIMXRT1180-EVK reference board
 *
 * Thin board layer: provide the source clocks, instantiate the i.MX RT1180 SoC
 * with the chosen part (MIMXRT1189), and load the firmware image.  Everything
 * device-specific lives in the SoC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"     /* error_report */
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/arm/boot.h"
#include "hw/arm/imxrt1180_soc.h"
#include "hw/sd/sd.h"
#include "hw/core/qdev-properties-system.h"  /* qdev_prop_set_drive_err */
#include "system/blockdev.h"                  /* drive_get / IF_SD */
#include "system/block-backend.h"             /* blk_by_legacy_dinfo */
#include "hw/arm/machines-qom.h"   /* DEFINE_MACHINE_ARM / arm_machine_interfaces */
#include "hw/core/loader.h"        /* load_elf / load_image_targphys */
#include "system/reset.h"          /* qemu_register_reset */
#include "system/address-spaces.h" /* address_space_memory */
#include "exec/memattrs.h"         /* MEMTXATTRS_UNSPECIFIED */
#include "target/arm/cpu.h"        /* ARMCPU init_svtor */
#include "hw/core/cpu.h"           /* cpu_reset / CPU() */
#include "elf.h"                   /* EM_ARM / ELFDATA2LSB */
#include "qom/object.h"

/* CM33 boot core runs at 300 MHz on the RT1189 (M7 main core is 800 MHz). */
#define IMXRT1180_M33_SYSCLK_HZ  300000000ULL

/*
 * Load the firmware into the GLOBAL system address space (not the per-cpu AS
 * armv7m_load_kernel uses, whose ROM loading misbehaves for our larger FlexSPI
 * / external-RAM regions), then pick the M33 boot vector from where the image
 * actually landed.  SDK debug/RAM images link their vector table to the code
 * TCM (0x0FFE0000); FlexSPI-NOR / Zephyr images link it to the FlexSPI window
 * (secure 0x38000000 / non-secure 0x28000000).  Approximates the boot ROM VTOR.
 */
static void imxrt1180_set_boot(ARMCPU *m33, hwaddr vt)
{
    m33->init_svtor  = vt;   /* M33 boots secure -> init_svtor */
    m33->init_nsvtor = vt;   /* (M7 / non-secure uses nsvtor)  */
}

static bool imxrt1180_vt_plausible(hwaddr vt)
{
    uint32_t sp = address_space_ldl(&address_space_memory, vt,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
    uint32_t pc = address_space_ldl(&address_space_memory, vt + 4,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
    return (pc & 1) && pc > 0x1000 && pc != 0xFFFFFFFF &&
           sp > 0x1000 && sp != 0xFFFFFFFF;
}

/*
 * Load an ELF's PT_LOAD segments with IMMEDIATE writes into the system memory,
 * so the image is present at machine-init time (the normal ROM loader defers
 * writes to the first reset, which is too late to probe for the boot vector).
 * Returns the entry point, or 0 (with *is_elf=false) if the file is not an ELF.
 */
/*
 * Write one loadable segment.  Segments that land in the FlexSPI XIP window are
 * PROGRAMMED INTO THE NOR (i.e. the board is flashed with them), not written to
 * the memory-mapped view: that window is a rom_device backed by a real flash, so
 * a plain address_space_write would be refused as a store to read-only flash --
 * and even if it landed in the mirror, the mirror and the flash would disagree
 * and the next erase (or reset re-sync) would resurrect the old content.
 * `-kernel` of an XIP image means "this firmware is in the board's NOR", so put
 * it there.
 */
static void imxrt1180_load_segment(IMXRT1180State *soc, uint32_t paddr,
                                   const uint8_t *data, uint32_t filesz)
{
    /*
     * cm7 image: its segments are linked to the M7's LOCAL ITCM (0x0..) and DTCM
     * (0x20000000..), not in the shared system view -- translate them to the two
     * halves of the M7 TCM (global 0x303C0000) so the M7's per-core view finds them
     * at local 0x0 / 0x20000000.
     */
    if (soc->boot_cm7) {
        if (paddr < IMXRT1180_M7_TCM_HALF) {
            paddr = IMXRT1180_CM7_TCM_BASE + paddr;
        } else if (paddr >= IMXRT1180_M7_DTCM_BASE &&
                   paddr < IMXRT1180_M7_DTCM_BASE + IMXRT1180_M7_TCM_HALF) {
            paddr = IMXRT1180_CM7_TCM_BASE + IMXRT1180_M7_TCM_HALF +
                    (paddr - IMXRT1180_M7_DTCM_BASE);
        }
        address_space_write(&address_space_memory, paddr, MEMTXATTRS_UNSPECIFIED,
                            data, filesz);
        return;
    }

    struct { hwaddr base; } xip[] = {
        { IMXRT1180_FLEXSPI1_BASE },        /* non-secure XIP window */
        { IMXRT1180_FLEXSPI1_S_BASE },      /* secure alias of it    */
    };

    for (size_t i = 0; i < ARRAY_SIZE(xip); i++) {
        if (paddr >= xip[i].base &&
            paddr + filesz <= xip[i].base + IMXRT1180_FLEXSPI1_SIZE) {
            imxrt1180_flexspi_flash_program(&soc->flexspi1_ctrl,
                                            paddr - xip[i].base, data, filesz);
            return;
        }
    }
    address_space_write(&address_space_memory, paddr, MEMTXATTRS_UNSPECIFIED,
                        data, filesz);
}

static uint32_t imxrt1180_load_elf_direct(IMXRT1180State *soc,
                                          const char *filename, bool *is_elf)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;

    *is_elf = false;
    if (!g_file_get_contents(filename, &data, &len, NULL) ||
        len < sizeof(Elf32_Ehdr)) {
        return 0;
    }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return 0;   /* not a little-endian ELF32 -> caller tries a raw image */
    }

    uint32_t phoff     = ldl_le_p(&eh->e_phoff);
    uint16_t phnum     = lduw_le_p(&eh->e_phnum);
    uint16_t phentsize = lduw_le_p(&eh->e_phentsize);
    for (uint16_t i = 0; i < phnum; i++) {
        if ((gsize)phoff + (i + 1) * phentsize > len) {
            break;
        }
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(data + phoff + i * phentsize);
        if (ldl_le_p(&ph->p_type) != PT_LOAD) {
            continue;
        }
        uint32_t off    = ldl_le_p(&ph->p_offset);
        uint32_t paddr  = ldl_le_p(&ph->p_paddr);
        uint32_t filesz = ldl_le_p(&ph->p_filesz);
        if (filesz && (gsize)off + filesz <= len) {
            imxrt1180_load_segment(soc, paddr, (const uint8_t *)data + off,
                                   filesz);
        }
    }
    *is_elf = true;
    return ldl_le_p(&eh->e_entry);
}

/* Re-read the M7's reset vector once its TCM image is in place (see boot_cm7). */
static void imxrt1180_cm7_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

/*
 * Load the firmware and pick the M33 boot vector.  armv7m_load_kernel ignores
 * the ELF entry, so approximate the boot ROM's VTOR set: SDK debug/RAM images
 * link their vector table to the code TCM (0x0FFE0000); FlexSPI / Zephyr images
 * place it at an arbitrary offset past the boot header (Zephyr's _vector_table
 * is well into the FlexSPI window).  For an ELF the reset vector (VT+4) equals
 * entry|1, so scan the candidate regions for it.
 */
static void imxrt1180_load_and_boot(IMXRT1180State *soc, ARMCPU *m33,
                                    const char *filename)
{
    static const struct { hwaddr base; hwaddr size; } regions[] = {
        { IMXRT1180_CODE_TCM_BASE,   IMXRT1180_CODE_TCM_SIZE }, /* SDK TCM images */
        { IMXRT1180_FLEXSPI1_S_BASE, 0x40000 },  /* secure XIP (Zephyr) */
        { IMXRT1180_FLEXSPI1_BASE,   0x40000 },  /* non-secure XIP */
    };
    bool is_elf;
    uint32_t entry = imxrt1180_load_elf_direct(soc, filename, &is_elf);

    if (soc->boot_cm7) {
        /*
         * The cm7 image now lives in the M7 TCM.  The M7 has no TrustZone, so it
         * resets with env->v7m.secure == 0 and reads its vector from
         * vecbase[NS] == init_nsvtor, which defaults to 0 -- and the M7's per-core
         * view maps 0x0 to its local ITCM (the first half of cm7_tcm), exactly
         * where the cm7 vector table landed.  So no vector seeding is needed.
         *
         * But the image was written to cm7_tcm AFTER the M7's realize-time reset
         * (which read an empty table), and QEMU's machine reset re-reads the vector
         * only for the boot core arm_load_kernel registered.  Register the M7 the
         * same way so it re-reads its now-populated vector at machine reset.
         */
        qemu_register_reset(imxrt1180_cm7_reset,
                            soc->armv7m[IMXRT1180_CPU_M7].cpu);
        return;
    }

    if (!is_elf) {
        /*
         * Raw .bin: the SDK cm33 convention links the vector table to the code
         * TCM, but not always to its base -- e.g. the usb_device_dfu images link
         * at 0x0FFF0000, reserving the low 64 KiB of CODE_TCM as the DFU update
         * slot, while hello_world/led_blinky/etc. link at 0x0FFE0000.  A flat
         * blob carries no load address, so derive it from the image's own reset
         * vector (file word[1] = reset PC): align it down to 64 KiB.  This puts
         * the reset PC inside [base, base+size) for every SDK cm33 .bin.  Guard
         * that the derived base sits within CODE_TCM; otherwise fall back.
         */
        hwaddr load_base = IMXRT1180_CODE_TCM_BASE;
        g_autofree gchar *hdr = NULL;
        gsize hlen = 0;
        if (g_file_get_contents(filename, &hdr, &hlen, NULL) && hlen >= 8) {
            uint32_t reset_pc = ldl_le_p(hdr + 4);
            hwaddr cand = reset_pc & 0xFFFF0000u;
            if (cand >= IMXRT1180_CODE_TCM_BASE &&
                cand <  IMXRT1180_CODE_TCM_BASE + IMXRT1180_CODE_TCM_SIZE) {
                load_base = cand;
            }
        }
        hwaddr load_max = IMXRT1180_CODE_TCM_BASE + IMXRT1180_CODE_TCM_SIZE
                          - load_base;
        if (load_image_targphys(filename, load_base, load_max, NULL) < 0) {
            error_report("Could not load kernel '%s'", filename);
            exit(1);
        }
        /*
         * load_image_targphys registers a ROM blob committed at reset, so the
         * image is not yet visible in memory here -- vt_plausible() would read
         * zeroes.  But load_base was derived from the image's own reset vector,
         * so the vector table is known to land there: boot from it directly.
         */
        imxrt1180_set_boot(m33, load_base);
        return;
    }

    /* The image is in memory now (immediate writes); find its vector table. */
    if (is_elf && entry) {
        uint32_t target = entry | 1u;
        for (size_t r = 0; r < ARRAY_SIZE(regions); r++) {
            for (hwaddr off = 4; off < regions[r].size; off += 4) {
                hwaddr a = regions[r].base + off;
                if (address_space_ldl(&address_space_memory, a,
                                      MEMTXATTRS_UNSPECIFIED, NULL) == target &&
                    imxrt1180_vt_plausible(a - 4)) {
                    imxrt1180_set_boot(m33, a - 4);
                    return;
                }
            }
        }
    }
    for (size_t r = 0; r < ARRAY_SIZE(regions); r++) {
        if (imxrt1180_vt_plausible(regions[r].base)) {
            imxrt1180_set_boot(m33, regions[r].base);
            return;
        }
    }
    imxrt1180_set_boot(m33, IMXRT1180_CODE_TCM_BASE);   /* fallback (raw .bin) */
}

/*
 * A cm7 SDK image links code into the M7's local ITCM (0x0..), so it has a
 * loadable segment below the M33 code TCM (0x0FFE0000) -- which no cm33 image ever
 * has.  Detect that BEFORE the SoC realizes, so we can boot the M7 (power the M7,
 * hold the M33) and load the image into the M7 TCM.
 */
static bool imxrt1180_kernel_is_cm7(const char *filename)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;

    if (!g_file_get_contents(filename, &data, &len, NULL) ||
        len < sizeof(Elf32_Ehdr)) {
        return false;
    }
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)data;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }
    uint32_t phoff     = ldl_le_p(&eh->e_phoff);
    uint16_t phnum     = lduw_le_p(&eh->e_phnum);
    uint16_t phentsize = lduw_le_p(&eh->e_phentsize);
    for (uint16_t i = 0; i < phnum; i++) {
        if ((gsize)phoff + (i + 1) * phentsize > len) {
            break;
        }
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(data + phoff + i * phentsize);
        if (ldl_le_p(&ph->p_type) == PT_LOAD && ldl_le_p(&ph->p_filesz) &&
            ldl_le_p(&ph->p_paddr) < IMXRT1180_M7_TCM_HALF) {
            return true;
        }
    }
    return false;
}

static void mimxrt1180_evk_init(MachineState *machine)
{
    IMXRT1180State *soc;
    Clock          *sysclk, *refclk;
    DeviceState    *dev;

    /* Board-supplied source clocks. */
    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, IMXRT1180_M33_SYSCLK_HZ);
    refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(refclk, IMXRT1180_M33_SYSCLK_HZ);

    /* Instantiate the SoC. */
    soc = IMXRT1180_SOC(object_new(TYPE_IMXRT1180_SOC));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_unref(OBJECT(soc));

    dev = DEVICE(soc);
    qdev_prop_set_string(dev, "part", "MIMXRT1189");
    /* Decide the boot core BEFORE realize (it sets the CPUs' power state): a cm7
     * image boots the M7, everything else the M33. */
    if (machine->kernel_filename &&
        imxrt1180_kernel_is_cm7(machine->kernel_filename)) {
        qdev_prop_set_bit(dev, "boot-cm7", true);
    }
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    qdev_connect_clock_in(dev, "refclk", refclk);
    sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal);

    /*
     * Attach an SD card to each USDHC controller that has a backing drive
     * (-drive if=sd,index=N,...).  With no drive the controller reports no card
     * inserted, which is the honest state.
     */
    for (int i = 0; i < IMXRT1180_NUM_USDHC; i++) {
        DriveInfo *di = drive_get(IF_SD, 0, i);
        if (!di) {
            continue;
        }
        DeviceState *card = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                &error_fatal);
        qdev_realize_and_unref(card,
            qdev_get_child_bus(DEVICE(&soc->usdhc[i]), "sd-bus"), &error_fatal);
    }

    /*
     * Load the firmware (immediate writes) + resolve the boot vector, then let
     * armv7m_load_kernel(NULL) register the M-profile CPU reset (which will use
     * the init_svtor we just set).
     */
    if (machine->kernel_filename) {
        imxrt1180_load_and_boot(soc, ARM_CPU(first_cpu), machine->kernel_filename);
    }
    armv7m_load_kernel(ARM_CPU(first_cpu), NULL,
                       IMXRT1180_CODE_TCM_BASE, IMXRT1180_CODE_TCM_SIZE);
}

static void mimxrt1180_evk_machine_init(MachineClass *mc)
{
    mc->desc        = "NXP MIMXRT1180-EVK (i.MX RT1189, Cortex-M33 + Cortex-M7)";
    mc->init        = mimxrt1180_evk_init;
    mc->default_ram_size = 0;   /* SoC owns its memories */
    /* Heterogeneous dual-core: cpu0 = M33 (boots), cpu1 = M7 (released by the
     * M33 via SRC/BLK_CTRL).  Lock the count so TCG provisions both contexts. */
    mc->default_cpus = 2;
    mc->min_cpus     = 2;
    mc->max_cpus     = 2;
    /* Keep transaction failures visible so peripheral stubs are loud. */
    mc->ignore_memory_transaction_failures = false;
}

DEFINE_MACHINE_ARM("mimxrt1180-evk", mimxrt1180_evk_machine_init)
