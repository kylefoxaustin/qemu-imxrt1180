/*
 * SD Association Host Standard Specification v2.0 controller emulation
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Mitsyanko Igor <i.mitsyanko@samsung.com>
 * Peter A.G. Crosthwaite <peter.crosthwaite@petalogix.com>
 *
 * Based on MMC controller for Samsung S5PC1xx-based board emulation
 * by Alexey Merkulov and Vladimir Monakhov.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU _General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SDHCI_H
#define SDHCI_H

#include "hw/pci/pci_device.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qom/object.h"

/* SD/MMC host controller state */
struct SDHCIState {
    /*< private >*/
    union {
        PCIDevice pcidev;
        SysBusDevice busdev;
    };

    /*< public >*/
    SDBus sdbus;
    MemoryRegion iomem;
    AddressSpace sysbus_dma_as;
    AddressSpace *dma_as;
    MemoryRegion *dma_mr;
    const MemoryRegionOps *io_ops;

    QEMUTimer *insert_timer;       /* timer for 'changing' sd card. */
    QEMUTimer *transfer_timer;
    qemu_irq irq;

    /* Registers cleared on reset */
    uint32_t sdmasysad;    /* SDMA System Address register */
    uint16_t blksize;      /* Host DMA Buff Boundary and Transfer BlkSize Reg */
    uint16_t blkcnt;       /* Blocks count for current transfer */
    uint32_t argument;     /* Command Argument Register */
    uint16_t trnmod;       /* Transfer Mode Setting Register */
    uint16_t cmdreg;       /* Command Register */
    uint32_t rspreg[4];    /* Response Registers 0-3 */
    uint32_t prnsts;       /* Present State Register */
    uint8_t  hostctl1;     /* Host Control Register */
    uint8_t  pwrcon;       /* Power control Register */
    uint8_t  blkgap;       /* Block Gap Control Register */
    uint8_t  wakcon;       /* WakeUp Control Register */
    uint16_t clkcon;       /* Clock control Register */
    uint8_t  timeoutcon;   /* Timeout Control Register */
    uint8_t  admaerr;      /* ADMA Error Status Register */
    uint16_t norintsts;    /* Normal Interrupt Status Register */
    uint16_t errintsts;    /* Error Interrupt Status Register */
    uint16_t norintstsen;  /* Normal Interrupt Status Enable Register */
    uint16_t errintstsen;  /* Error Interrupt Status Enable Register */
    uint16_t norintsigen;  /* Normal Interrupt Signal Enable Register */
    uint16_t errintsigen;  /* Error Interrupt Signal Enable Register */
    uint16_t acmd12errsts; /* Auto CMD12 error status register */
    uint16_t hostctl2;     /* Host Control 2 */
    uint64_t admasysaddr;  /* ADMA System Address Register */
    /*
     * VENDOR-SPECIFIC (eSDHC/uSDHC VEND_SPEC @0xC0).
     *
     * WAS uint16_t FOR A 32-BIT REGISTER.  The i.MX reset value is 0x3000_7809, which
     * does not fit in 16 bits -- WHICH IS WHY IT READ BACK WRONG.  Bits 28/29 were
     * truncated on every write.  That truncation was a bug for EVERY platform, not
     * just the i.MX ones.  (Found by 91emulator's reset-value audit; confirmed against
     * the i.MX RT1180 RM independently -- VEND_SPEC resets to 0x30007809 there too.)
     *
     * vendor_spec_reset is a QDEV PROPERTY, DEFAULT 0, so generic SDHCI and
     * FSL_ESDHC_BE/LE reset to 0 EXACTLY as before.  Only a machine that sets it gets
     * the RM value.
     */
    uint32_t vendor_spec;

    /* Read-only registers */
    uint64_t capareg;      /* Capabilities Register */
    uint64_t maxcurr;      /* Maximum Current Capabilities Register */
    uint16_t version;      /* Host Controller Version Register */

    uint8_t  *fifo_buffer; /* SD host i/o FIFO buffer */
    uint32_t buf_maxsz;
    uint16_t data_count;   /* current element in FIFO buffer */
    uint8_t  stopped_state;/* Current SDHC state */
    bool     pending_insert_state;
    /* Buffer Data Port Register - virtual access point to R and W buffers */
    /* Software Reset Register - always reads as 0 */
    /* Force Event Auto CMD12 Error Interrupt Reg - write only */
    /* Force Event Error Interrupt Register- write only */
    /* RO Host Controller Version Register always reads as 0x2401 */

    /* Configurable properties */

    /*
     * VEND_SPEC's reset value.  THIS MUST LIVE HERE, WITH THE OTHER PROPERTIES, AND
     * NOT UP WITH THE REGISTERS -- because sdhci_reset() does
     *
     *     memset(&s->sdmasysad, 0, (uintptr_t)&s->capareg - (uintptr_t)&s->sdmasysad);
     *
     * and everything in that window is STATE that reset clears.  I first declared this
     * next to `vendor_spec`, inside the window, so THE RESET FUNCTION ZEROED ITS OWN
     * CONFIGURATION and then copied the zero it had just written.  The property was set
     * correctly (0x30007809, verified at the SoC) and read back as 0 forever.
     *
     *   ⭐ A RESET THAT SPANS A RANGE CLEARS WHATEVER YOU PUT IN THE RANGE. CONFIGURATION
     *      IS NOT STATE, AND A STRUCT THAT MIXES THEM WILL EVENTUALLY ERASE THE ONE WITH
     *      THE OTHER.
     *
     * (The fields from `capareg` down are the ones the existing comment calls
     * "not cleared and assumed to always preserve their value" -- that boundary is
     * load-bearing, and it is enforced by nothing but the memset's arithmetic.)
     */
    uint32_t vendor_spec_reset;

    bool pending_insert_quirk; /* Quirk for Raspberry Pi card insert int */
    uint32_t quirks;
    uint8_t sd_spec_version;
    uint8_t uhs_mode;
    /*
     * Write Protect pin default active low for detecting SD card
     * to be protected. Set wp_inverted to invert the signal.
     */
    bool wp_inverted;
};
typedef struct SDHCIState SDHCIState;

/*
 * Controller does not provide transfer-complete interrupt when not
 * busy.
 *
 * NOTE: This definition is taken out of Linux kernel and so the
 * original bit number is preserved
 */
#define SDHCI_QUIRK_NO_BUSY_IRQ    BIT(14)

#define TYPE_PCI_SDHCI "sdhci-pci"
DECLARE_INSTANCE_CHECKER(SDHCIState, PCI_SDHCI,
                         TYPE_PCI_SDHCI)

#define TYPE_SYSBUS_SDHCI "generic-sdhci"
DECLARE_INSTANCE_CHECKER(SDHCIState, SYSBUS_SDHCI,
                         TYPE_SYSBUS_SDHCI)

#define TYPE_FSL_ESDHC_BE "fsl-esdhc-be"
#define TYPE_FSL_ESDHC_LE "fsl-esdhc-le"

#define TYPE_IMX_USDHC "imx-usdhc"

#define TYPE_S3C_SDHCI "s3c-sdhci"

#endif /* SDHCI_H */
