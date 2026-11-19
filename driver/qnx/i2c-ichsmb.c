/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-ichsmb: QNX OS 8.0 I2C master driver for the Intel chipset SMBus
 * host controller, built on the QNX I2C Driver Development Kit.
 *
 * libi2c-master supplies main() and the resource manager behind /dev/i2cN.
 * This file supplies the hardware functions and binds driver/core to QNX:
 * PCI lookup, I/O ports, the interrupt event and the clock.
 *
 * ---------------------------------------------------------------------
 * DRAFT - NOT YET COMPILED. Written on 14 Nov 2026 from the public QNX
 * documentation before the QNX kit was available. Every line marked
 * CHECK must be compared with the installed headers (<hw/i2c.h>,
 * <pci/pci.h>) before this file is relied on.
 * ---------------------------------------------------------------------
 *
 * Options (after the library's own --u unit and --b speed):
 *   -i        interrupt mode (default: polling)
 *   -s        3..33-byte reads use SMBus Block Read (count byte first)
 *             instead of I2C Block Read
 *   -t ms     transaction timeout (default 100)
 *   -v        verbose
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <hw/inout.h>
#include <hw/i2c.h>
#include <pci/pci.h>

#include "smb_core.h"
#include "ichsmb_regs.h"

#define IRQ_PULSE_CODE  (_PULSE_CODE_MINAVAIL + 0)

typedef struct {
    smb_dev_t    dev;
    smb_bus_t    bus;
    const char  *model;
    pci_devhdl_t pci;
    uintptr_t    iobase;
    int          irq;           /* -1 in polling mode */
    int          iid;
    int          chid, coid;
    int          irq_masked;    /* the kernel masked the line when it delivered our event */
    int          verbose;
} ichsmb_t;

/* --------------------------------------------------------- core callbacks */

static uint8_t q_in8(void *ctx, unsigned reg)             { return in8(((ichsmb_t *)ctx)->iobase + reg); }
static void    q_out8(void *ctx, unsigned reg, uint8_t v) { out8(((ichsmb_t *)ctx)->iobase + reg, v); }

static uint64_t q_now(void *ctx)
{
    struct timespec ts;

    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void q_wait(void *ctx, uint32_t max_us)
{
    ichsmb_t *h = ctx;

    if (h->irq < 0) {
        struct timespec ts = { 0, (long)max_us * 1000L };

        nanosleep(&ts, NULL);
        return;
    }
    /* Interrupt mode: sleep until the interrupt pulse arrives or max_us passes. */
    {
        struct _pulse pulse;
        uint64_t ns = (uint64_t)max_us * 1000u;

        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
        if (MsgReceivePulse(h->chid, &pulse, sizeof(pulse), NULL) == 0 && pulse.code == IRQ_PULSE_CODE) {
            h->irq_masked = 1;
        }
    }
}

/* Called by the core once the controller's status is serviced and cleared,
 * i.e. the line is low again. Unmask only if the kernel masked it for us. */
static void q_irq_done(void *ctx)
{
    ichsmb_t *h = ctx;

    if (h->irq >= 0 && h->irq_masked) {
        h->irq_masked = 0;
        InterruptUnmask(h->irq, h->iid);
    }
}

/* ------------------------------------------------------------------- PCI */

static int find_controller(ichsmb_t *h)
{
    pci_bdf_t bdf;
    pci_err_t err;
    uint8_t   hostc;

    /* ICH family: the SMBus controller is its own PCI function, I/O base in BAR4. */
    bdf = pci_device_find(0, PCI_VID_INTEL, PCI_DID_ICH9_SMBUS, PCI_CCODE_ANY);
    if (bdf != PCI_BDF_NONE) {
        pci_ba_t ba[6];
        int_t    nba = 6, i;

        h->model = "ICH9";
        h->pci = pci_device_attach(bdf, pci_attachFlags_DEFAULT, &err);
        if (h->pci == NULL) {
            fprintf(stderr, "i2c-ichsmb: pci_device_attach: %s\n", pci_strerror(err));     /* CHECK pci_strerror */
            return -1;
        }
        if (pci_device_read_ba(h->pci, &nba, ba, pci_reqType_e_UNSPECIFIED) != PCI_ERR_OK) {
            return -1;
        }
        for (i = 0; i < nba; i++) {
            if (ba[i].type == pci_asType_e_IO) {                /* CHECK pci_ba_t field and enum names */
                h->iobase = (uintptr_t)ba[i].addr;
            }
        }
        pci_device_cfg_rd8(bdf, ICH_CFG_HOSTC, &hostc);
        /* Host on, interrupts to the PCI line (not SMI), I2C framing unless -s was given. */
        hostc = (uint8_t)((hostc | ICH_HOSTC_HST_EN) & ~ICH_HOSTC_SMI_EN);
        hostc = h->bus.smbus_block_read ? (uint8_t)(hostc & ~ICH_HOSTC_I2C_EN)
                                        : (uint8_t)(hostc | ICH_HOSTC_I2C_EN);
        pci_device_cfg_wr8(h->pci, ICH_CFG_HOSTC, hostc, NULL);  /* CHECK signature */
        h->dev.caps = SMB_CAP_BLOCK | SMB_CAP_I2C_READ;
        if (hostc & ICH_HOSTC_I2C_EN) {
            h->dev.caps |= SMB_CAP_I2C_WRITE;
        }
        if (h->irq == 0) {                                       /* -i given: look the line up */
            pci_irq_t irq[1];
            int_t     nirq = 1;

            if (pci_device_read_irq(h->pci, &nirq, irq) == PCI_ERR_OK && nirq > 0) {    /* CHECK */
                h->irq = (int)irq[0];
            } else {
                fprintf(stderr, "i2c-ichsmb: no interrupt for this controller, using polling\n");
                h->irq = -1;
            }
        }
        return 0;
    }

    /* PIIX4: SMBus is part of the power-management function; base at config 0x90. */
    bdf = pci_device_find(0, PCI_VID_INTEL, PCI_DID_PIIX4_PM, PCI_CCODE_ANY);
    if (bdf != PCI_BDF_NONE) {
        uint32_t base;
        uint8_t  cfg;

        h->model = "PIIX4";
        pci_device_cfg_rd32(bdf, PIIX4_CFG_SMBBA, &base);
        h->iobase = base & 0xFFF0u;
        pci_device_cfg_rd8(bdf, PIIX4_CFG_SMBHSTCFG, &cfg);
        if (!(cfg & PIIX4_HSTCFG_HST_EN)) {
            h->pci = pci_device_attach(bdf, pci_attachFlags_DEFAULT, &err);
            if (h->pci == NULL) {
                return -1;
            }
            pci_device_cfg_wr8(h->pci, PIIX4_CFG_SMBHSTCFG, (uint8_t)(cfg | PIIX4_HSTCFG_HST_EN), NULL);
        }
        h->dev.caps = 0;        /* quick, byte, byte data, word data only: see docs/LIMITS.md */
        h->irq = -1;            /* polling only on this controller */
        return 0;
    }

    fprintf(stderr, "i2c-ichsmb: no supported SMBus controller found\n");
    return -1;
}

/* ------------------------------------------------------ DDK entry points */

static i2c_status_t to_status(int rc)
{
    /* CHECK against <hw/i2c.h>: whether error states are combined with I2C_STATUS_DONE. */
    switch (rc) {
    case SMB_OK:        return I2C_STATUS_DONE;
    case SMB_E_NACK:    return I2C_STATUS_NACK;
    case SMB_E_BUS:     return I2C_STATUS_ARBL;
    case SMB_E_TIMEOUT:
    case SMB_E_BUSY:    return I2C_STATUS_BUSY;
    case SMB_E_ABORTED: return I2C_STATUS_ABORT;
    default:            return I2C_STATUS_ERROR;
    }
}

static void *ichsmb_init(int argc, char *argv[])
{
    ichsmb_t *h = calloc(1, sizeof(*h));
    int c;

    if (h == NULL) {
        return NULL;
    }
    h->irq = -1;
    smb_bus_init(&h->bus, &h->dev);

    optind = 1;
    while ((c = getopt(argc, argv, ":ist:v")) != -1) {
        switch (c) {
        case 'i': h->irq = 0; break;                            /* resolved in find_controller() */
        case 's': h->bus.smbus_block_read = 1; break;
        case 't': h->dev.timeout_us = (uint32_t)strtoul(optarg, NULL, 0) * 1000u; break;
        case 'v': h->verbose++; break;
        default:  break;                                        /* library options */
        }
    }

    /* x86 port I/O needs I/O privilege level 1 (and the PROCMGR_AID_IO ability). */
    if (ThreadCtl(_NTO_TCTL_IO_LEVEL, (void *)(_NTO_IO_LEVEL_1 | _NTO_TCTL_IO_LEVEL_INHERIT)) == -1) {
        perror("i2c-ichsmb: ThreadCtl(_NTO_TCTL_IO_LEVEL)");
        goto fail;
    }
    if (find_controller(h) != 0 || h->iobase == 0) {
        goto fail;
    }
    h->iobase = mmap_device_io(SMB_IO_LEN, h->iobase);          /* CHECK: docs prefer mmap(); identity on x86 */

    h->dev.io.ctx = h;
    h->dev.io.in8 = q_in8;
    h->dev.io.out8 = q_out8;
    h->dev.io.wait = q_wait;
    h->dev.io.irq_done = q_irq_done;
    h->dev.io.now_us = q_now;

    if (h->irq >= 0) {
        struct sigevent ev;

        h->chid = ChannelCreate(0);
        h->coid = ConnectAttach(0, 0, h->chid, _NTO_SIDE_CHANNEL, 0);
        SIGEV_PULSE_INIT(&ev, h->coid, SIGEV_PULSE_PRIO_INHERIT, IRQ_PULSE_CODE, 0);
        h->iid = InterruptAttachEvent(h->irq, &ev, 0);
        if (h->chid == -1 || h->coid == -1 || h->iid == -1) {
            perror("i2c-ichsmb: interrupt setup");
            goto fail;
        }
        h->dev.use_irq = 1;
    }

    smb_reset(&h->dev);
    if (h->verbose) {
        fprintf(stderr, "i2c-ichsmb: %s SMBus at I/O 0x%lx, %s, status 0x%02x\n", h->model,
                (unsigned long)h->iobase, h->irq >= 0 ? "interrupt" : "polling", q_in8(h, SMB_HST_STS));
    }
    return h;

fail:
    free(h);
    return NULL;
}

static void ichsmb_fini(void *hdl)
{
    ichsmb_t *h = hdl;

    smb_reset(&h->dev);
    if (h->irq >= 0) {
        InterruptDetach(h->iid);
    }
    if (h->pci != NULL) {
        pci_device_detach(h->pci);                              /* CHECK */
    }
    free(h);
}

static i2c_status_t ichsmb_send(void *hdl, void *buf, unsigned int len, unsigned int stop)
{
    return to_status(smb_bus_send(&((ichsmb_t *)hdl)->bus, buf, len, (int)stop));
}

static i2c_status_t ichsmb_recv(void *hdl, void *buf, unsigned int len, unsigned int stop)
{
    return to_status(smb_bus_recv(&((ichsmb_t *)hdl)->bus, buf, len, (int)stop));
}

static i2c_status_t ichsmb_xfer(void *const hdl, void *const sbuf, const uint32_t slen,
                                void *const rbuf, const uint32_t rlen)
{
    return to_status(smb_bus_xfer(&((ichsmb_t *)hdl)->bus, sbuf, slen, rbuf, rlen));
}

static int ichsmb_set_slave_addr(void *hdl, unsigned int addr, i2c_addrfmt_t fmt)
{
    if (fmt != I2C_ADDRFMT_7BIT || smb_bus_set_addr(&((ichsmb_t *)hdl)->bus, addr) != SMB_OK) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int ichsmb_set_bus_speed(void *hdl, unsigned int speed, unsigned int *ospeed)
{
    /* The chipset fixes the SMBus clock near 100 kHz; it cannot be changed. */
    (void)hdl; (void)speed;
    if (ospeed != NULL) {
        *ospeed = 100000;
    }
    return 0;
}

static int ichsmb_version_info(i2c_libversion_t *version)
{
    version->major = I2CLIB_VERSION_MAJOR;
    version->minor = I2CLIB_VERSION_MINOR;
    version->revision = I2CLIB_REVISION;
    return 0;
}

static int ichsmb_driver_info(void *hdl, i2c_driver_info_t *info)
{
    info->speed_mode = I2C_SPEED_STANDARD;
    info->addr_mode = I2C_ADDRFMT_7BIT;
    info->verbosity = (unsigned)((ichsmb_t *)hdl)->verbose;     /* CHECK field type */
    return 0;
}

static int ichsmb_bus_reset(void *hdl)
{
    ichsmb_t *h = hdl;

    h->bus.pend_valid = 0;
    smb_reset(&h->dev);
    return 0;
}

int i2c_master_getfuncs(i2c_master_funcs_t *funcs, int tabsize)
{
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, init, ichsmb_init, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, fini, ichsmb_fini, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, send, ichsmb_send, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, recv, ichsmb_recv, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, xfer, ichsmb_xfer, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, set_slave_addr, ichsmb_set_slave_addr, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, set_bus_speed, ichsmb_set_bus_speed, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, version_info, ichsmb_version_info, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, driver_info, ichsmb_driver_info, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, bus_reset, ichsmb_bus_reset, tabsize);
    return 0;
}
