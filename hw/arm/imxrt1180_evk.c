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
#include "hw/arm/machines-qom.h"   /* DEFINE_MACHINE_ARM / arm_machine_interfaces */
#include "hw/core/loader.h"        /* load_elf / load_image_targphys */
#include "system/reset.h"          /* qemu_register_reset */
#include "system/address-spaces.h" /* address_space_memory */
#include "exec/memattrs.h"         /* MEMTXATTRS_UNSPECIFIED */
#include "target/arm/cpu.h"        /* ARMCPU init_svtor */
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
static uint32_t imxrt1180_load_elf_direct(const char *filename, bool *is_elf)
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
            address_space_write(&address_space_memory, paddr,
                                MEMTXATTRS_UNSPECIFIED, data + off, filesz);
        }
    }
    *is_elf = true;
    return ldl_le_p(&eh->e_entry);
}

/*
 * Load the firmware and pick the M33 boot vector.  armv7m_load_kernel ignores
 * the ELF entry, so approximate the boot ROM's VTOR set: SDK debug/RAM images
 * link their vector table to the code TCM (0x0FFE0000); FlexSPI / Zephyr images
 * place it at an arbitrary offset past the boot header (Zephyr's _vector_table
 * is well into the FlexSPI window).  For an ELF the reset vector (VT+4) equals
 * entry|1, so scan the candidate regions for it.
 */
static void imxrt1180_load_and_boot(ARMCPU *m33, const char *filename)
{
    static const struct { hwaddr base; hwaddr size; } regions[] = {
        { IMXRT1180_CODE_TCM_BASE,   IMXRT1180_CODE_TCM_SIZE }, /* SDK TCM images */
        { IMXRT1180_FLEXSPI1_S_BASE, 0x40000 },  /* secure XIP (Zephyr) */
        { IMXRT1180_FLEXSPI1_BASE,   0x40000 },  /* non-secure XIP */
    };
    bool is_elf;
    uint32_t entry = imxrt1180_load_elf_direct(filename, &is_elf);

    if (!is_elf) {
        /* Raw image: goes to the code TCM (SDK .bin convention). */
        if (load_image_targphys(filename, IMXRT1180_CODE_TCM_BASE,
                                IMXRT1180_CODE_TCM_SIZE, NULL) < 0) {
            error_report("Could not load kernel '%s'", filename);
            exit(1);
        }
        entry = 0;
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
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    qdev_connect_clock_in(dev, "refclk", refclk);
    sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal);

    /*
     * Load the firmware (immediate writes) + resolve the boot vector, then let
     * armv7m_load_kernel(NULL) register the M-profile CPU reset (which will use
     * the init_svtor we just set).
     */
    if (machine->kernel_filename) {
        imxrt1180_load_and_boot(ARM_CPU(first_cpu), machine->kernel_filename);
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
