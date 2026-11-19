/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register map of the Intel chipset SMBus host controller.
 *
 * Written from Intel's public datasheets, not from any other driver:
 *   - Intel I/O Controller Hub 9 (ICH9) Family Datasheet, chapter
 *     "SMBus Controller Registers (D31:F3)".
 *   - Intel 82371AB PCI-to-ISA/IDE Xcelerator (PIIX4) Datasheet, chapter
 *     "SMBus I/O Space Registers".
 * The two controllers share the first eight I/O registers; the ICH-only
 * ones are marked below.
 */
#ifndef ICHSMB_REGS_H
#define ICHSMB_REGS_H

/* ---- I/O registers, offsets from the SMBus I/O base ---- */
#define SMB_HST_STS      0x00   /* host status                       */
#define SMB_HST_CNT      0x02   /* host control                      */
#define SMB_HST_CMD      0x03   /* command (register index) byte     */
#define SMB_XMIT_SLVA    0x04   /* target address << 1 | read bit    */
#define SMB_HST_D0       0x05   /* data 0 / block byte count         */
#define SMB_HST_D1       0x06   /* data 1                            */
#define SMB_BLOCK_DB     0x07   /* block data byte                   */
#define SMB_AUX_CTL      0x0D   /* auxiliary control (ICH only)      */
#define SMB_IO_LEN       0x10   /* bytes of I/O space the driver maps */

/* ---- HST_STS bits (write 1 to clear, except HOST_BUSY) ---- */
#define STS_HOST_BUSY    0x01   /* a transaction is running          */
#define STS_INTR         0x02   /* transaction completed             */
#define STS_DEV_ERR      0x04   /* no acknowledge / illegal command  */
#define STS_BUS_ERR      0x08   /* bus collision, lost arbitration   */
#define STS_FAILED       0x10   /* transaction killed by software    */
#define STS_SMBALERT     0x20
#define STS_INUSE        0x40   /* software semaphore; write 1 to release */
#define STS_BYTE_DONE    0x80   /* one block byte moved              */

#define STS_ERRORS       (STS_DEV_ERR | STS_BUS_ERR | STS_FAILED)
#define STS_EVENTS       (STS_INTR | STS_ERRORS | STS_BYTE_DONE)

/* ---- HST_CNT bits ---- */
#define CNT_INTREN       0x01   /* raise an interrupt on completion  */
#define CNT_KILL         0x02   /* abort the running transaction     */
#define CNT_PROT_SHIFT   2      /* bits 4:2 select the protocol      */
#define CNT_LAST_BYTE    0x20   /* next received block byte is the last */
#define CNT_START        0x40

/* ---- protocol codes for HST_CNT bits 4:2 ---- */
#define PROT_QUICK       0
#define PROT_BYTE        1      /* send byte / receive byte          */
#define PROT_BYTE_DATA   2
#define PROT_WORD_DATA   3
#define PROT_BLOCK       5      /* SMBus block read / write          */
#define PROT_I2C_READ    6      /* I2C block read (ICH only)         */

/* ---- PCI identification ---- */
#define PCI_VID_INTEL          0x8086
#define PCI_DID_ICH9_SMBUS     0x2930   /* QEMU "q35" machine, D31:F3 */
#define PCI_DID_PIIX4_PM       0x7113   /* QEMU "pc" machine, power-management function */

/* ---- PCI configuration space, ICH family ---- */
#define ICH_CFG_SMB_BASE       0x20     /* BAR4: I/O base, bit 0 = I/O space indicator */
#define ICH_CFG_HOSTC          0x40
#define ICH_HOSTC_HST_EN       0x01     /* enable the SMBus host             */
#define ICH_HOSTC_SMI_EN       0x02     /* route interrupts to SMI, not IRQ  */
#define ICH_HOSTC_I2C_EN       0x04     /* I2C framing for block commands    */

/* ---- PCI configuration space, PIIX4 power-management function ---- */
#define PIIX4_CFG_SMBBA        0x90     /* SMBus I/O base                    */
#define PIIX4_CFG_SMBHSTCFG    0xD2
#define PIIX4_HSTCFG_HST_EN    0x01

#endif /* ICHSMB_REGS_H */
