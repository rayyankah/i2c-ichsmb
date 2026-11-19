/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-ichsmb: QNX OS 8.0 I2C master driver for the Intel chipset SMBus
 * host controller, built on the QNX I2C Driver Development Kit.
 *
 * libi2c-master supplies main() and the resource manager behind /dev/i2cN.
 * It takes its own options (--u unit, --b speed), then calls our init()
 * with the rest, attaches /dev/i2cN, and becomes a daemon.
 *
 * This file binds driver/core to QNX: PCI lookup, I/O ports, the interrupt
 * event, the clock, logging and privileges.
 *
 * The controller cannot hold the bus between two messages, so the driver
 * reports I2C_CAPS_FORCE_STOP and implements xfer(); the library then
 * hands write-then-read requests over as one combined transfer.
 *
 * Options: see i2c-ichsmb.use.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <sys/procmgr.h>
#include <sys/slog.h>
#include <hw/inout.h>
#include <hw/i2c.h>
#include <pci/pci.h>
#include <secpol/ids.h>

#include "smb_core.h"
#include "ichsmb_regs.h"

#define IRQ_PULSE_CODE  (_PULSE_CODE_MINAVAIL + 0)
#define SMBUS_HZ        100000u     /* the chipset fixes the SMBus clock */

/* ------------------------------------------------------------ hardware table */

typedef enum { KIND_ICH, KIND_PIIX4 } hw_kind_t;

typedef struct {
    uint16_t    did;
    hw_kind_t   kind;
    const char *name;
} hw_entry_t;

/* Only controllers that have been tested are listed. Others of the ICH
 * family can be tried with -p, which accepts any Intel SMBus-class function. */
static const hw_entry_t hw_table[] = {
    { PCI_DID_ICH9_SMBUS, KIND_ICH,   "ICH9"  },     /* QEMU q35 */
    { PCI_DID_PIIX4_PM,   KIND_PIIX4, "PIIX4" },     /* QEMU pc  */
};
#define HW_COUNT (sizeof(hw_table) / sizeof(hw_table[0]))

#define PCI_CLASS_SMBUS 0x0C05u     /* serial bus controller, SMBus */

/* -------------------------------------------------------------------- state */

typedef struct {
    smb_dev_t    dev;
    smb_bus_t    bus;
    const char  *model;
    pci_bdf_t    bdf;
    pci_devhdl_t pci;
    uintptr_t    iobase;
    int          irq;           /* -1: polling */
    int          want_irq;
    int          iid;
    int          chid, coid;
    int          irq_masked;    /* the kernel masked the line when it delivered our event */
    unsigned     verbose;       /* slog2 severity threshold (_SLOG_*) */
    int          starting;      /* still inside init(): errors also go to stderr */
    const char  *run_as;        /* -U argument */
} ichsmb_t;

/* ------------------------------------------------------------------ logging */

static void logmsg(ichsmb_t *h, unsigned level, const char *fmt, ...)
{
    char    msg[200];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    i2c_slogf(h->verbose, level, "i2c-ichsmb: %s", msg);
    /* Before the library turns us into a daemon, the person starting the
     * driver should see why it failed without reading the system log. */
    if (h->starting && level <= h->verbose && level <= _SLOG_INFO) {
        fprintf(stderr, "i2c-ichsmb: %s\n", msg);
    }
}

/* ------------------------------------------------------------ core callbacks */

static uint8_t q_in8(void *ctx, unsigned reg)             { return in8(((ichsmb_t *)ctx)->iobase + reg); }
static void    q_out8(void *ctx, unsigned reg, uint8_t v) { out8(((ichsmb_t *)ctx)->iobase + reg, v); }

static uint64_t q_now(void *ctx)
{
    struct timespec ts;

    (void)ctx;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/*
 * Wait for the controller. Polling: sleep. Interrupt mode: block on the
 * channel until the interrupt pulse or the timeout. The core re-reads the
 * status register after every return, so a stale or early pulse is harmless.
 *
 * Lost-interrupt check: the core clears the status bits and only then calls
 * q_irq_done(), which unmasks. If the controller raises the line again after
 * the status read but before this wait, the pulse is already queued on the
 * channel and MsgReceivePulse() returns at once. The kernel masks the line
 * each time it delivers the event, so at most one pulse is ever pending.
 */
static void q_wait(void *ctx, uint32_t max_us)
{
    ichsmb_t *h = ctx;

    if (h->irq < 0) {
        struct timespec ts = { 0, (long)max_us * 1000L };

        nanosleep(&ts, NULL);
        return;
    }
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
 * so the line is low again. Unmask only if the kernel masked it for us. */
static void q_irq_done(void *ctx)
{
    ichsmb_t *h = ctx;

    if (h->irq >= 0 && h->irq_masked) {
        h->irq_masked = 0;
        InterruptUnmask(h->irq, h->iid);
    }
}

/* ---------------------------------------------------------------------- PCI */

static int parse_bdf(const char *s, pci_bdf_t *bdf)
{
    unsigned b, d, f;
    char     end;

    if (sscanf(s, "%x:%x.%x%c", &b, &d, &f, &end) != 3 || b > 0xFF || d > 0x1F || f > 7) {
        return -1;
    }
    *bdf = PCI_BDF(b, d, f);
    return 0;
}

/* Pick the controller: the -p address if given, otherwise the first table match. */
static const hw_entry_t *locate(ichsmb_t *h, const char *where)
{
    static hw_entry_t forced;
    uint16_t vid = 0, did = 0;
    uint32_t class = 0;
    unsigned i;

    if (where == NULL) {
        for (i = 0; i < HW_COUNT; i++) {
            h->bdf = pci_device_find(0, PCI_VID_INTEL, hw_table[i].did, PCI_CCODE_ANY);
            if (h->bdf != PCI_BDF_NONE) {
                return &hw_table[i];
            }
        }
        logmsg(h, _SLOG_ERROR, "no supported SMBus controller found (use -p to name one)");
        return NULL;
    }

    if (parse_bdf(where, &h->bdf) != 0) {
        logmsg(h, _SLOG_ERROR, "-p wants bus:device.function in hex, e.g. 0:1f.3");
        return NULL;
    }
    pci_device_cfg_rd16(h->bdf, 0x00, &vid);
    pci_device_cfg_rd16(h->bdf, 0x02, &did);
    pci_device_cfg_rd32(h->bdf, 0x08, &class);
    if (vid != PCI_VID_INTEL) {
        logmsg(h, _SLOG_ERROR, "%s is not an Intel device (vendor %04x)", where, vid);
        return NULL;
    }
    for (i = 0; i < HW_COUNT; i++) {
        if (hw_table[i].did == did) {
            return &hw_table[i];
        }
    }
    if ((class >> 16) == PCI_CLASS_SMBUS) {
        logmsg(h, _SLOG_WARNING, "device %04x at %s is untested; treating it as ICH-compatible", did, where);
        forced.did = did;
        forced.kind = KIND_ICH;
        forced.name = "ICH-compatible";
        return &forced;
    }
    logmsg(h, _SLOG_ERROR, "%s (device %04x) is not an SMBus controller", where, did);
    return NULL;
}

static int setup_ich(ichsmb_t *h)
{
    pci_err_t err;
    pci_ba_t  ba[6];
    int_t     nba = 6, i;
    uint8_t   hostc = 0;

    h->pci = pci_device_attach(h->bdf, pci_attachFlags_DEFAULT, &err);
    if (h->pci == NULL) {
        logmsg(h, _SLOG_ERROR, "pci_device_attach: %s", pci_strerror(err));
        return -1;
    }
    if (pci_device_read_ba(h->pci, &nba, ba, pci_reqType_e_UNSPECIFIED) != PCI_ERR_OK) {
        logmsg(h, _SLOG_ERROR, "cannot read the base address registers");
        return -1;
    }
    for (i = 0; i < nba; i++) {
        if (ba[i].type == pci_asType_e_IO) {
            h->iobase = (uintptr_t)ba[i].addr;
        }
    }
    pci_device_cfg_rd8(h->bdf, ICH_CFG_HOSTC, &hostc);
    /* Host on, interrupts to the PCI line (not SMI), I2C framing unless -s was given. */
    hostc = (uint8_t)((hostc | ICH_HOSTC_HST_EN) & ~ICH_HOSTC_SMI_EN);
    hostc = h->bus.smbus_block_read ? (uint8_t)(hostc & ~ICH_HOSTC_I2C_EN)
                                    : (uint8_t)(hostc | ICH_HOSTC_I2C_EN);
    pci_device_cfg_wr8(h->pci, ICH_CFG_HOSTC, hostc, NULL);
    h->dev.caps = SMB_CAP_BLOCK | SMB_CAP_I2C_READ;
    if (hostc & ICH_HOSTC_I2C_EN) {
        h->dev.caps |= SMB_CAP_I2C_WRITE;
    }
    if (h->want_irq) {
        pci_irq_t irq[1];
        int_t     nirq = 1;

        if (pci_device_read_irq(h->pci, &nirq, irq) == PCI_ERR_OK && nirq > 0) {
            h->irq = (int)irq[0];
        } else {
            logmsg(h, _SLOG_WARNING, "no interrupt for this controller, using polling");
        }
    }
    return 0;
}

static int setup_piix4(ichsmb_t *h)
{
    pci_err_t err;
    uint32_t  base = 0;
    uint8_t   cfg = 0;

    pci_device_cfg_rd32(h->bdf, PIIX4_CFG_SMBBA, &base);
    h->iobase = base & 0xFFF0u;
    pci_device_cfg_rd8(h->bdf, PIIX4_CFG_SMBHSTCFG, &cfg);
    if (!(cfg & PIIX4_HSTCFG_HST_EN)) {
        h->pci = pci_device_attach(h->bdf, pci_attachFlags_DEFAULT, &err);
        if (h->pci == NULL) {
            logmsg(h, _SLOG_ERROR, "pci_device_attach: %s", pci_strerror(err));
            return -1;
        }
        pci_device_cfg_wr8(h->pci, PIIX4_CFG_SMBHSTCFG, (uint8_t)(cfg | PIIX4_HSTCFG_HST_EN), NULL);
    }
    h->dev.caps = 0;            /* quick, byte, byte data and word data only (see DEVLOG, 14 Nov) */
    if (h->want_irq) {
        logmsg(h, _SLOG_WARNING, "PIIX4 support is polling only");
    }
    return 0;
}

/* ------------------------------------------------------------- privileges */

/*
 * -U: keep only what the driver needs once it is running, then become an
 * ordinary user. I/O port access was granted to this thread by ThreadCtl()
 * and stays with it. The library still has to register /dev/i2cN after
 * init() returns, so PATHSPACE stays, and PUBLIC_CHANNEL for its channel.
 */
static int drop_privileges(ichsmb_t *h)
{
    int rc;

    if (h->irq >= 0) {
        rc = procmgr_ability(0,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_PATHSPACE,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_PUBLIC_CHANNEL,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_INTERRUPT,
            PROCMGR_AID_EOL);
    } else {
        rc = procmgr_ability(0,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_PATHSPACE,
            PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | PROCMGR_AID_PUBLIC_CHANNEL,
            PROCMGR_AID_EOL);
    }
    if (rc != EOK) {
        logmsg(h, _SLOG_ERROR, "procmgr_ability: %s", strerror(rc));
        return -1;
    }
    if (set_ids_from_arg(h->run_as) != EOK) {
        logmsg(h, _SLOG_ERROR, "-U %s: %s", h->run_as, strerror(errno));
        return -1;
    }
    rc = procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);
    if (rc != EOK) {
        logmsg(h, _SLOG_ERROR, "procmgr_ability (drop the rest): %s", strerror(rc));
        return -1;
    }
    logmsg(h, _SLOG_INFO, "running as uid %d gid %d", (int)getuid(), (int)getgid());
    return 0;
}

/* ------------------------------------------------------- DDK entry points */

/*
 * libi2c-master counts a call as successful only when it returns exactly
 * I2C_STATUS_DONE; on anything else it logs each error bit and fails the
 * client's devctl with EIO. DONE means "finished, with or without error",
 * so errors are reported as DONE plus the cause.
 */
static i2c_status_t to_status(ichsmb_t *h, int rc)
{
    if (rc != SMB_OK) {
        logmsg(h, _SLOG_DEBUG1, "addr 0x%02x: %s", h->bus.addr, smb_strerror(rc));
    }
    switch (rc) {
    case SMB_OK:        return I2C_STATUS_DONE;
    case SMB_E_NACK:    return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_NACK);
    case SMB_E_BUS:     return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_ARBL);
    case SMB_E_TIMEOUT:
    case SMB_E_BUSY:    return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_BUSY);
    case SMB_E_ABORTED: return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_ABORT);
    default:            return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_ERROR);
    }
}

static void ichsmb_fini(void *hdl);

static void *ichsmb_init(int argc, char *argv[])
{
    ichsmb_t         *h = calloc(1, sizeof(*h));
    const hw_entry_t *hw;
    const char       *where = NULL;
    int               c;

    if (h == NULL) {
        return NULL;
    }
    h->irq = -1;
    h->iid = -1;
    h->chid = h->coid = -1;
    h->verbose = _SLOG_WARNING;
    h->starting = 1;
    smb_bus_init(&h->bus, &h->dev);

    optind = 1;
    while ((c = getopt(argc, argv, "ip:st:U:v")) != -1) {
        switch (c) {
        case 'i': h->want_irq = 1; break;
        case 'p': where = optarg; break;
        case 's': h->bus.smbus_block_read = 1; break;
        case 't': h->dev.timeout_us = (uint32_t)strtoul(optarg, NULL, 0) * 1000u; break;
        case 'U': h->run_as = optarg; break;
        case 'v': if (h->verbose < _SLOG_DEBUG2) h->verbose++; break;
        default:
            logmsg(h, _SLOG_ERROR, "unknown option (see: use i2c-ichsmb)");
            goto fail;
        }
    }

    /* x86 port I/O needs I/O privilege level 1 (and the PROCMGR_AID_IO ability). */
    if (ThreadCtl(_NTO_TCTL_IO_LEVEL, (void *)(_NTO_IO_LEVEL_1 | _NTO_TCTL_IO_LEVEL_INHERIT)) == -1) {
        logmsg(h, _SLOG_ERROR, "no I/O privilege (%s): start as root, then use -U to drop to a user",
               strerror(errno));
        goto fail;
    }
    hw = locate(h, where);
    if (hw == NULL) {
        goto fail;
    }
    h->model = hw->name;
    if ((hw->kind == KIND_ICH ? setup_ich(h) : setup_piix4(h)) != 0 || h->iobase == 0) {
        if (h->iobase == 0) {
            logmsg(h, _SLOG_ERROR, "%s SMBus has no I/O base (disabled by firmware?)", h->model);
        }
        goto fail;
    }
    h->iobase = mmap_device_io(SMB_IO_LEN, h->iobase);
    if (h->iobase == MAP_DEVICE_FAILED) {
        logmsg(h, _SLOG_ERROR, "mmap_device_io: %s", strerror(errno));
        goto fail;
    }

    h->dev.io.ctx = h;
    h->dev.io.in8 = q_in8;
    h->dev.io.out8 = q_out8;
    h->dev.io.wait = q_wait;
    h->dev.io.irq_done = q_irq_done;
    h->dev.io.now_us = q_now;

    if (h->irq >= 0) {
        struct sigevent ev;

        h->chid = ChannelCreate(_NTO_CHF_PRIVATE);
        h->coid = h->chid == -1 ? -1 : ConnectAttach(0, 0, h->chid, _NTO_SIDE_CHANNEL, 0);
        if (h->coid != -1) {
            SIGEV_PULSE_INIT(&ev, h->coid, SIGEV_PULSE_PRIO_INHERIT, IRQ_PULSE_CODE, 0);
            h->iid = InterruptAttachEvent(h->irq, &ev, _NTO_INTR_FLAGS_TRK_MSK);
        }
        if (h->chid == -1 || h->coid == -1 || h->iid == -1) {
            logmsg(h, _SLOG_ERROR, "interrupt %d: %s", h->irq, strerror(errno));
            goto fail;
        }
        h->dev.use_irq = 1;
    }

    smb_reset(&h->dev);
    logmsg(h, _SLOG_INFO, "%s SMBus at %x:%02x.%x, I/O 0x%lx, %s, status 0x%02x", h->model,
           (unsigned)PCI_BUS(h->bdf), (unsigned)PCI_DEV(h->bdf), (unsigned)PCI_FUNC(h->bdf),
           (unsigned long)h->iobase, h->irq >= 0 ? "interrupt" : "polling", q_in8(h, SMB_HST_STS));

    if (h->run_as != NULL && drop_privileges(h) != 0) {
        goto fail;
    }
    h->starting = 0;
    return h;

fail:
    ichsmb_fini(h);
    return NULL;
}

static void ichsmb_fini(void *hdl)
{
    ichsmb_t *h = hdl;

    if (h->iobase != 0 && h->dev.io.in8 != NULL) {
        smb_reset(&h->dev);
        logmsg(h, _SLOG_INFO, "%u transactions, %u NACKs, %u bus errors, %u timeouts",
               (unsigned)h->dev.stats.transactions, (unsigned)h->dev.stats.nacks,
               (unsigned)h->dev.stats.bus_errors, (unsigned)h->dev.stats.timeouts);
    }
    if (h->iid != -1) {
        InterruptDetach(h->iid);
    }
    if (h->coid != -1) {
        ConnectDetach(h->coid);
    }
    if (h->chid != -1) {
        ChannelDestroy(h->chid);
    }
    if (h->pci != NULL) {
        pci_device_detach(h->pci);
    }
    free(h);
}

static i2c_status_t ichsmb_send(void *hdl, void *buf, unsigned int len, unsigned int stop)
{
    ichsmb_t *h = hdl;

    return to_status(h, smb_bus_send(&h->bus, buf, len, (int)stop));
}

static i2c_status_t ichsmb_recv(void *hdl, void *buf, unsigned int len, unsigned int stop)
{
    ichsmb_t *h = hdl;

    return to_status(h, smb_bus_recv(&h->bus, buf, len, (int)stop));
}

static i2c_status_t ichsmb_xfer(void *const hdl, void *const sbuf, const uint32_t slen,
                                void *const rbuf, const uint32_t rlen)
{
    ichsmb_t *h = hdl;

    return to_status(h, smb_bus_xfer(&h->bus, sbuf, slen, rbuf, rlen));
}

static int ichsmb_abort(void *hdl, int rcvid)
{
    ichsmb_t *h = hdl;

    (void)rcvid;
    h->bus.pend_valid = 0;
    smb_reset(&h->dev);
    return 0;
}

static int ichsmb_set_slave_addr(void *hdl, unsigned int addr, i2c_addrfmt_t fmt)
{
    ichsmb_t *h = hdl;

    if (fmt != I2C_ADDRFMT_7BIT || smb_bus_set_addr(&h->bus, addr) != SMB_OK) {
        logmsg(h, _SLOG_DEBUG1, "refused address 0x%x (format %d): 7-bit addresses only", addr, (int)fmt);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* The chipset runs the SMBus at a fixed clock. Asking for anything else
 * fails and leaves it unchanged, as the DDK asks. */
static int ichsmb_set_bus_speed(void *hdl, unsigned int speed, unsigned int *ospeed)
{
    ichsmb_t *h = hdl;

    if (speed != SMBUS_HZ) {
        logmsg(h, _SLOG_WARNING, "bus speed %u refused: this controller runs at %u Hz only", speed, SMBUS_HZ);
        errno = EINVAL;
        return -1;
    }
    if (ospeed != NULL) {
        *ospeed = SMBUS_HZ;
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
    /* The library logs every failed transfer (each NACK of a bus scan, for
     * example) at error level when its verbosity allows; keep that for -v. */
    info->verbosity = ((ichsmb_t *)hdl)->verbose > _SLOG_WARNING ? ((ichsmb_t *)hdl)->verbose : _SLOG_CRITICAL;
    info->caps = I2C_CAPS_FORCE_STOP;   /* every message ends with a stop; use xfer() */
    return 0;
}

static int ichsmb_bus_reset(void *hdl)
{
    ichsmb_t *h = hdl;

    h->bus.pend_valid = 0;
    smb_reset(&h->dev);
    logmsg(h, _SLOG_INFO, "bus reset");
    return EOK;
}

int i2c_master_getfuncs(i2c_master_funcs_t *funcs, int tabsize)
{
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, init, ichsmb_init, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, fini, ichsmb_fini, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, send, ichsmb_send, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, recv, ichsmb_recv, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, xfer, ichsmb_xfer, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, abort, ichsmb_abort, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, set_slave_addr, ichsmb_set_slave_addr, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, set_bus_speed, ichsmb_set_bus_speed, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, version_info, ichsmb_version_info, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, driver_info, ichsmb_driver_info, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, bus_reset, ichsmb_bus_reset, tabsize);
    return 0;
}
