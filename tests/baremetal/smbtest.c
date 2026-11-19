/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal test of driver/core against QEMU's own SMBus controller model.
 *
 * This is a UEFI program with no operating system under it. It links the
 * same driver/core sources as the QNX driver, finds the ICH9 SMBus
 * controller of QEMU's "q35" machine through PCI configuration ports, and
 * runs real transactions against the chips QEMU puts on the bus (the
 * memory-module EEPROMs at 0x50..0x57, plus any sensor added on the command
 * line). Results go to the first serial port; the exit code goes to QEMU's
 * isa-debug-exit device.
 *
 * Purpose: QEMU's controller model was written by other people from the
 * same datasheet, so agreement here is independent evidence, and it needs
 * no QNX licence. See run.sh for the QEMU command line.
 */
#include <stdint.h>
#include <stddef.h>
#include "smb_core.h"
#include "ichsmb_regs.h"

/* ------------------------------------------------------------ port I/O */

static inline void outb(uint16_t port, uint8_t v)  { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port)           { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port)          { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* The compiler may emit calls to these even in freestanding mode. */
void *memset(void *p, int c, size_t n)  { uint8_t *b = p; while (n--) *b++ = (uint8_t)c; return p; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }

/* -------------------------------------------------------------- serial */

#define COM1 0x3F8

static void ser_init(void)
{
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* divisor latch */
    outb(COM1 + 0, 0x01);   /* 115200 baud   */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1           */
    outb(COM1 + 2, 0x00);   /* no FIFO       */
}

static void putc_(char c)
{
    int spin = 100000;

    while (!(inb(COM1 + 5) & 0x20) && spin-- > 0) { }
    outb(COM1, (uint8_t)c);
}

static void puts_(const char *s) { while (*s) { if (*s == '\n') putc_('\r'); putc_(*s++); } }

static void hex(uint32_t v, int digits)
{
    while (digits-- > 0) {
        putc_("0123456789ABCDEF"[(v >> (4 * digits)) & 0xF]);
    }
}

static void dec(int v)
{
    char b[12]; int i = 0;

    if (v < 0) { putc_('-'); v = -v; }
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) putc_(b[i]);
}

/* ----------------------------------------------------------------- PCI */

static uint32_t pci_addr(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    return 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC);
}
static uint32_t pci_rd32(unsigned b, unsigned d, unsigned f, unsigned off) { outl(0xCF8, pci_addr(b, d, f, off)); return inl(0xCFC); }
static uint16_t pci_rd16(unsigned b, unsigned d, unsigned f, unsigned off) { return (uint16_t)(pci_rd32(b, d, f, off) >> (8 * (off & 2))); }
static uint8_t  pci_rd8(unsigned b, unsigned d, unsigned f, unsigned off)  { return (uint8_t)(pci_rd32(b, d, f, off) >> (8 * (off & 3))); }
static void pci_wr8(unsigned b, unsigned d, unsigned f, unsigned off, uint8_t v)
{
    outl(0xCF8, pci_addr(b, d, f, off));
    outb((uint16_t)(0xCFC + (off & 3)), v);
}
static void pci_wr16(unsigned b, unsigned d, unsigned f, unsigned off, uint16_t v)
{
    pci_wr8(b, d, f, off, (uint8_t)v);
    pci_wr8(b, d, f, off + 1, (uint8_t)(v >> 8));
}

/* ----------------------------------------------------- core I/O binding */

static uint16_t g_base;
static uint64_t g_clock;    /* nominal microseconds: advanced by waiting, not measured */

static uint8_t  io_in8(void *c, unsigned reg)             { (void)c; return inb((uint16_t)(g_base + reg)); }
static void     io_out8(void *c, unsigned reg, uint8_t v) { (void)c; outb((uint16_t)(g_base + reg), v); }
static void     io_wait(void *c, uint32_t us)             { (void)c; g_clock += us; while (us--) (void)inb(0x80); }
static uint64_t io_now(void *c)                           { (void)c; return ++g_clock; }

/* --------------------------------------------------------------- tests */

static int g_pass, g_fail;

static void result(const char *name, int ok)
{
    puts_(ok ? "PASS " : "FAIL ");
    puts_(name);
    puts_("\n");
    if (ok) g_pass++; else g_fail++;
}

static void finish(void)
{
    puts_("RESULT pass="); dec(g_pass); puts_(" fail="); dec(g_fail); puts_("\n");
    outb(0xF4, g_fail ? 1 : 0);     /* isa-debug-exit: QEMU exits with (value << 1) | 1 */
    for (;;) { __asm__ volatile ("hlt"); }
}

#define EE 0x50     /* first memory-module EEPROM on q35 */

unsigned long long __attribute__((ms_abi)) efi_main(void *image, void *systab)
{
    static smb_dev_t dev;
    static smb_bus_t bus;
    static uint8_t ref[64], buf[64];
    const unsigned B = 0, D = 0x1F, F = 3;      /* ICH9 SMBus lives at 00:1f.3 */
    unsigned a, i, n, found = 0;
    uint8_t hostc, v;
    uint16_t w;
    int rc, ok;

    (void)image; (void)systab;
    ser_init();
    puts_("\n== i2c-ichsmb core on bare metal ==\n");

    if (pci_rd16(B, D, F, 0) != PCI_VID_INTEL || pci_rd16(B, D, F, 2) != PCI_DID_ICH9_SMBUS) {
        puts_("no ICH9 SMBus controller at 00:1f.3 (run on the q35 machine)\n");
        g_fail++;
        finish();
    }
    g_base = (uint16_t)(pci_rd32(B, D, F, ICH_CFG_SMB_BASE) & 0xFFE0);
    hostc = pci_rd8(B, D, F, ICH_CFG_HOSTC);
    puts_("INFO io_base=0x"); hex(g_base, 4);
    puts_(" hostc=0x"); hex(hostc, 2);
    puts_(" irq_line="); dec(pci_rd8(B, D, F, 0x3C));
    puts_(" irq_pin="); dec(pci_rd8(B, D, F, 0x3D)); puts_("\n");
    result("controller has an I/O base", g_base != 0);

    pci_wr16(B, D, F, 0x04, (uint16_t)(pci_rd16(B, D, F, 0x04) | 0x0001));     /* I/O space on */
    pci_wr8(B, D, F, ICH_CFG_HOSTC, ICH_HOSTC_HST_EN);                          /* host on, SMBus framing */

    dev.io.in8 = io_in8; dev.io.out8 = io_out8; dev.io.wait = io_wait; dev.io.now_us = io_now;
    dev.caps = SMB_CAP_BLOCK | SMB_CAP_I2C_READ;
    smb_reset(&dev);
    smb_bus_init(&bus, &dev);

    /* --- scan --- */
    puts_("INFO chips:");
    for (a = 0x08; a <= 0x77; a++) {
        if (smb_quick(&dev, (uint8_t)a, 0) == SMB_OK) { puts_(" 0x"); hex(a, 2); found++; }
    }
    puts_("\n");
    result("scan finds the EEPROM at 0x50", smb_quick(&dev, EE, 0) == SMB_OK);
    result("empty address reports no-acknowledge", smb_quick(&dev, 0x20, 0) == SMB_E_NACK);
    result("read from empty address reports no-acknowledge", smb_read_byte_data(&dev, 0x20, 0, &v) == SMB_E_NACK);

    /* --- reference: 32 bytes of EEPROM content, one Read Byte Data each --- */
    ok = 1;
    for (i = 0; i < 32; i++) {
        if (smb_read_byte_data(&dev, EE, (uint8_t)i, &ref[i]) != SMB_OK) ok = 0;
    }
    result("Read Byte Data x32", ok);
    puts_("INFO eeprom[0..7]:");
    for (i = 0; i < 8; i++) { putc_(' '); hex(ref[i], 2); }
    puts_("\n");

    rc = smb_read_word_data(&dev, EE, 2, &w);
    result("Read Word Data matches two byte reads", rc == SMB_OK && w == (uint16_t)(ref[2] | (ref[3] << 8)));

    /* --- write and read back (the EEPROM model accepts writes) --- */
    rc = smb_write_byte_data(&dev, EE, 0x80, 0x5A);
    result("Write Byte Data", rc == SMB_OK);
    rc = smb_read_byte_data(&dev, EE, 0x80, &v);
    result("Write Byte Data reads back", rc == SMB_OK && v == 0x5A);
    rc = smb_write_word_data(&dev, EE, 0x82, 0xBEEF);
    w = 0;
    result("Write Word Data reads back", rc == SMB_OK && smb_read_word_data(&dev, EE, 0x82, &w) == SMB_OK && w == 0xBEEF);

    rc = smb_send_byte(&dev, EE, 0x80);
    v = 0;
    result("Send Byte then Receive Byte", rc == SMB_OK && smb_receive_byte(&dev, EE, &v) == SMB_OK && v == 0x5A);

    /* --- SMBus block write / read (count byte on the wire) --- */
    for (i = 0; i < 32; i++) buf[i] = (uint8_t)(0xA0 + i);
    for (n = 1; n <= 32; n += (n < 4 || n > 29) ? 1 : 9) {
        unsigned got = 0; uint8_t rb[32];

        if (n == 32) {
            /*
             * QEMU bug (hw/i2c/pm_smbus.c, still in 10.2 and master): in
             * byte-by-byte mode the buffer index wraps to 0 at 32 before it is
             * compared with the count, so a 32-byte write never completes and
             * BYTE_DONE repeats forever. Real ICH hardware allows 32 bytes.
             * What we check here is that the engine stops cleanly instead of
             * hanging.
             */
            rc = smb_block_write(&dev, EE, 0x90, buf, n);
            puts_("INFO smbus block write n=32 rc="); dec(rc); puts_(" (QEMU index-wrap bug)\n");
            result("32-byte write on QEMU's buggy model ends with an error, not a hang",
                   rc == SMB_E_PROTO || rc == SMB_E_TIMEOUT);
            continue;
        }
        rc = smb_block_write(&dev, EE, 0x90, buf, n);
        ok = (rc == SMB_OK);
        rc = smb_block_read(&dev, EE, 0x90, rb, sizeof(rb), &got);
        ok = ok && rc == SMB_OK && got == n;
        for (i = 0; ok && i < n; i++) if (rb[i] != buf[i]) ok = 0;
        puts_("INFO smbus block n="); dec((int)n); puts_(" rc="); dec(rc); puts_(" count="); dec((int)got); puts_("\n");
        result("SMBus Block Write then Block Read", ok);
    }

    /*
     * New reference with non-zero content: q35's memory-module EEPROMs read as
     * all zeros, which would let a broken block read pass. The block writes
     * above left a count byte and 0xA0, 0xA1, ... from 0x90 on.
     */
    ok = 1;
    for (i = 0; i < 32; i++) {
        if (smb_read_byte_data(&dev, EE, (uint8_t)(0x90 + i), &ref[i]) != SMB_OK) ok = 0;
    }
    result("reference bytes at 0x90 are not all zero", ok && ref[1] == 0xA0 && ref[31] == 0xBE);

    /* --- I2C framing: enable I2C_EN, block write without count, I2C block read --- */
    pci_wr8(B, D, F, ICH_CFG_HOSTC, ICH_HOSTC_HST_EN | ICH_HOSTC_I2C_EN);
    dev.caps |= SMB_CAP_I2C_WRITE;
    for (n = 1; n <= 32; n += (n < 4 || n > 29) ? 1 : 9) {
        uint8_t rb[32];

        rc = smb_i2c_block_read(&dev, EE, 0x90, rb, n);
        ok = (rc == SMB_OK);
        for (i = 0; ok && i < n; i++) if (rb[i] != ref[i]) ok = 0;
        puts_("INFO i2c read n="); dec((int)n); puts_(" rc="); dec(rc); puts_("\n");
        result("I2C Block Read matches byte reads", ok);
    }
    for (i = 0; i < 8; i++) buf[i] = (uint8_t)(0x11 * (i + 1));
    rc = smb_block_write(&dev, EE, 0xA0, buf, 8);
    ok = (rc == SMB_OK);
    for (i = 0; i < 8; i++) {
        if (smb_read_byte_data(&dev, EE, (uint8_t)(0xA0 + i), &v) != SMB_OK || v != buf[i]) ok = 0;
    }
    result("block write with I2C_EN stores data without a count byte", ok);

    /* --- the mapping layer, as the QNX framework will call it --- */
    smb_bus_set_addr(&bus, EE);
    buf[0] = 0x94;
    rc = smb_bus_send(&bus, buf, 1, 0);
    ok = (rc == SMB_OK) && smb_bus_recv(&bus, buf + 8, 6, 1) == SMB_OK;
    for (i = 0; ok && i < 6; i++) if (buf[8 + i] != ref[4 + i]) ok = 0;
    result("send(no stop) + recv(6) through the mapping layer", ok);

    /* --- optional temperature sensor (QEMU_EXTRA in run.sh can add one at 0x48) --- */
    if (smb_quick(&dev, 0x48, 0) == SMB_OK) {
        rc = smb_read_word_data(&dev, 0x48, 0, &w);
        puts_("INFO tmp105 raw=0x"); hex(w, 4); puts_(" whole_degrees="); dec((int8_t)(w & 0xFF)); puts_("\n");
        result("temperature sensor at 0x48 answers", rc == SMB_OK);
    } else {
        puts_("INFO no sensor at 0x48\n");
    }

    /* --- does this controller raise a PCI interrupt? (observed, not asserted) --- */
    io_out8(NULL, SMB_HST_STS, STS_EVENTS);
    io_out8(NULL, SMB_XMIT_SLVA, (uint8_t)((EE << 1) | 1));
    io_out8(NULL, SMB_HST_CMD, 0);
    io_out8(NULL, SMB_HST_CNT, (uint8_t)((PROT_BYTE_DATA << CNT_PROT_SHIFT) | CNT_INTREN | CNT_START));
    for (i = 0; i < 1000 && !(io_in8(NULL, SMB_HST_STS) & STS_INTR); i++) { }
    puts_("INFO with INTREN: hst_sts=0x"); hex(io_in8(NULL, SMB_HST_STS), 2);
    puts_(" pci_status.interrupt="); dec((pci_rd16(B, D, F, 0x06) >> 3) & 1); puts_("\n");
    io_out8(NULL, SMB_HST_STS, STS_EVENTS);
    io_out8(NULL, SMB_HST_CNT, 0);

    puts_("INFO chips found="); dec((int)found);
    puts_(" transactions="); dec((int)dev.stats.transactions);
    puts_(" timeouts="); dec((int)dev.stats.timeouts); puts_("\n");
    result("no transaction timed out", dev.stats.timeouts == 0);
    finish();
    return 0;
}
