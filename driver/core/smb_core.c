/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMBus transaction engine for the Intel chipset SMBus host controller.
 * See smb_core.h for the layering and docs/DESIGN.md for the reasoning.
 */
#include <stddef.h>
#include "smb_core.h"
#include "ichsmb_regs.h"

/* How long to wait for a controller that is busy before we even start. */
#define BUSY_WAIT_US   5000u
/* How long to wait for the controller to acknowledge a kill. */
#define KILL_WAIT_US   2000u

enum { BLK_NONE, BLK_WRITE, BLK_READ_SMBUS, BLK_READ_I2C };

/* One transaction, as the engine sees it. */
typedef struct {
    uint8_t        prot;        /* PROT_*                                   */
    uint8_t        addr_rw;     /* address << 1 | read                      */
    uint8_t        cmd, d0, d1;
    int            blk;         /* BLK_*                                    */
    const uint8_t *wbuf;        /* BLK_WRITE: bytes to send                 */
    uint8_t       *rbuf;        /* block reads: where bytes go              */
    unsigned       len;         /* BLK_WRITE / BLK_READ_I2C: byte count;
                                   BLK_READ_SMBUS: capacity of rbuf         */
    unsigned       count;       /* BLK_READ_SMBUS: count the device announced */
} txn_t;

static uint8_t rd(smb_dev_t *d, unsigned reg)            { return d->io.in8(d->io.ctx, reg); }
static void    wr(smb_dev_t *d, unsigned reg, uint8_t v) { d->io.out8(d->io.ctx, reg, v); }
static uint64_t now(smb_dev_t *d)                        { return d->io.now_us(d->io.ctx); }

static void irq_done(smb_dev_t *d)
{
    if (d->io.irq_done != NULL) {
        d->io.irq_done(d->io.ctx);
    }
}

/* Wait until `deadline`, at most one poll interval. Returns 0 if the deadline has passed. */
static int pause_until(smb_dev_t *d, uint64_t deadline)
{
    uint64_t t = now(d);
    uint32_t poll = d->poll_us ? d->poll_us : SMB_DEFAULT_POLL_US;

    if (t >= deadline) {
        return 0;
    }
    if (deadline - t < poll) {
        poll = (uint32_t)(deadline - t);
    }
    d->stats.waits++;
    d->io.wait(d->io.ctx, poll);
    return 1;
}

/* Abort the running transaction and leave the controller idle and clean. */
static void kill_txn(smb_dev_t *d)
{
    uint64_t deadline;

    wr(d, SMB_HST_CNT, CNT_KILL);
    deadline = now(d) + KILL_WAIT_US;
    while ((rd(d, SMB_HST_STS) & STS_HOST_BUSY) != 0 && pause_until(d, deadline)) {
        /* the controller sets FAILED and drops HOST_BUSY when the kill lands */
    }
    wr(d, SMB_HST_CNT, 0);                  /* KILL is not self-clearing */
    wr(d, SMB_HST_STS, STS_EVENTS);
}

void smb_reset(smb_dev_t *d)
{
    kill_txn(d);
    wr(d, SMB_HST_STS, STS_EVENTS | STS_INUSE);
    irq_done(d);
}

static int error_from_status(smb_dev_t *d, uint8_t sts)
{
    /* Order matters: a kill reports FAILED together with other bits. */
    if (sts & STS_FAILED) {
        return SMB_E_ABORTED;
    }
    if (sts & STS_BUS_ERR) {
        d->stats.bus_errors++;
        return SMB_E_BUS;
    }
    d->stats.nacks++;
    return SMB_E_NACK;
}

/*
 * Run one transaction to completion.
 *
 * Non-block transactions: program the registers, set START, wait for INTR
 * or an error bit.
 *
 * Block transactions use the controller's byte-by-byte handshake: it sets
 * BYTE_DONE after every byte; we move one byte through SMB_BLOCK_DB and
 * clear BYTE_DONE to let it continue. For reads we set LAST_BYTE while the
 * next-to-last byte is pending so that the controller does not acknowledge
 * the final byte. Controllers differ in how they signal that final byte
 * (BYTE_DONE then INTR, or INTR alone with the byte waiting in
 * SMB_BLOCK_DB); the loop accepts both.
 */
static int run(smb_dev_t *d, txn_t *t)
{
    const uint32_t timeout = d->timeout_us ? d->timeout_us : SMB_DEFAULT_TIMEOUT_US;
    const int      reading = (t->blk == BLK_READ_SMBUS || t->blk == BLK_READ_I2C);
    unsigned       idx = 0;                 /* block bytes moved so far            */
    unsigned       events = 0;              /* BYTE_DONE events seen               */
    unsigned       want = (t->blk == BLK_READ_SMBUS) ? 0 : t->len;  /* 0 = not known yet */
    uint8_t        ctl, sts;
    uint64_t       deadline;
    int            rc;

    d->stats.transactions++;

    /* 1. The controller must be idle. Firmware may share it, so give it a moment. */
    deadline = now(d) + BUSY_WAIT_US;
    while ((rd(d, SMB_HST_STS) & STS_HOST_BUSY) != 0) {
        if (!pause_until(d, deadline)) {
            d->stats.busy++;
            return SMB_E_BUSY;
        }
    }
    wr(d, SMB_HST_STS, STS_EVENTS);         /* clear anything left over */

    /* 2. Program the transaction. */
    wr(d, SMB_XMIT_SLVA, t->addr_rw);
    wr(d, SMB_HST_CMD, t->cmd);
    wr(d, SMB_HST_D0, t->d0);
    wr(d, SMB_HST_D1, t->d1);
    if (t->blk == BLK_WRITE) {
        wr(d, SMB_BLOCK_DB, t->wbuf[0]);
        idx = 1;
    }
    ctl = (uint8_t)(t->prot << CNT_PROT_SHIFT);
    if (d->use_irq) {
        ctl |= CNT_INTREN;
    }
    if (t->blk == BLK_READ_I2C && t->len == 1) {
        ctl |= CNT_LAST_BYTE;               /* the only byte is also the last */
    }

    /* 3. Go. */
    wr(d, SMB_HST_CNT, ctl | CNT_START);
    deadline = now(d) + timeout;

    /* 4. Service the controller until it finishes or fails. */
    for (;;) {
        int progressed = 0;

        sts = rd(d, SMB_HST_STS);

        if (sts & STS_ERRORS) {
            rc = error_from_status(d, sts);
            break;
        }

        if (t->blk != BLK_NONE && (sts & STS_BYTE_DONE)) {
            /*
             * A block moves at most SMB_BLOCK_MAX data bytes plus the count
             * byte. More events than that means the controller has lost
             * track (QEMU's model does this on 32-byte writes, see DEVLOG
             * 2026-11-19): stop instead of feeding it forever.
             */
            if (++events > SMB_BLOCK_MAX + 1) {
                kill_txn(d);
                rc = SMB_E_PROTO;
                break;
            }
            if (reading) {
                uint8_t b;

                if (want == 0) {            /* SMBus block read: first event carries the count */
                    want = rd(d, SMB_HST_D0);
                    if (want < 1 || want > SMB_BLOCK_MAX) {
                        kill_txn(d);
                        rc = SMB_E_PROTO;
                        break;
                    }
                    t->count = want;
                }
                b = rd(d, SMB_BLOCK_DB);
                if (idx < t->len) {
                    t->rbuf[idx] = b;
                }
                idx++;
                if (idx + 1 >= want) {
                    wr(d, SMB_HST_CNT, ctl | CNT_LAST_BYTE);
                }
            } else if (idx < t->len) {
                wr(d, SMB_BLOCK_DB, t->wbuf[idx++]);
            }
            wr(d, SMB_HST_STS, STS_BYTE_DONE);  /* lets the controller continue */
            progressed = 1;
        }

        if (sts & STS_INTR) {
            rc = SMB_OK;
            if (reading) {
                if (want == 0) {            /* completed without a byte event */
                    want = rd(d, SMB_HST_D0);
                    if (want < 1 || want > SMB_BLOCK_MAX) {
                        rc = SMB_E_PROTO;
                        break;
                    }
                    t->count = want;
                }
                if (idx + 1 == want) {      /* final byte signalled by INTR alone */
                    uint8_t b = rd(d, SMB_BLOCK_DB);
                    if (idx < t->len) {
                        t->rbuf[idx] = b;
                    }
                    idx++;
                }
                if (idx < want) {
                    rc = SMB_E_PROTO;
                }
            } else if (t->blk == BLK_WRITE && idx < t->len) {
                rc = SMB_E_PROTO;
            }
            break;
        }

        if (progressed) {
            irq_done(d);
            if (now(d) >= deadline) {       /* progress does not extend the deadline */
                kill_txn(d);
                d->stats.timeouts++;
                rc = SMB_E_TIMEOUT;
                break;
            }
            continue;
        }

        irq_done(d);                        /* nothing for us: re-arm before sleeping */
        if (!pause_until(d, deadline)) {
            kill_txn(d);
            d->stats.timeouts++;
            rc = SMB_E_TIMEOUT;
            break;
        }
    }

    /* 5. Clear our status bits, release the in-use semaphore, re-arm the interrupt. */
    wr(d, SMB_HST_STS, STS_EVENTS | STS_INUSE);
    irq_done(d);
    return rc;
}

static int bad_addr(uint8_t addr) { return addr > 0x7F; }

static void txn_init(txn_t *t, uint8_t prot, uint8_t addr, int read, uint8_t cmd)
{
    t->prot = prot;
    t->addr_rw = (uint8_t)((addr << 1) | (read ? 1 : 0));
    t->cmd = cmd;
    t->d0 = 0;
    t->d1 = 0;
    t->blk = BLK_NONE;
    t->wbuf = NULL;
    t->rbuf = NULL;
    t->len = 0;
    t->count = 0;
}

int smb_quick(smb_dev_t *d, uint8_t addr, int read)
{
    txn_t t;

    if (bad_addr(addr)) return SMB_E_INVAL;
    txn_init(&t, PROT_QUICK, addr, read, 0);
    return run(d, &t);
}

int smb_send_byte(smb_dev_t *d, uint8_t addr, uint8_t value)
{
    txn_t t;

    if (bad_addr(addr)) return SMB_E_INVAL;
    txn_init(&t, PROT_BYTE, addr, 0, value);
    return run(d, &t);
}

int smb_receive_byte(smb_dev_t *d, uint8_t addr, uint8_t *value)
{
    txn_t t;
    int rc;

    if (bad_addr(addr) || value == NULL) return SMB_E_INVAL;
    txn_init(&t, PROT_BYTE, addr, 1, 0);
    rc = run(d, &t);
    if (rc == SMB_OK) {
        *value = rd(d, SMB_HST_D0);
    }
    return rc;
}

int smb_write_byte_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint8_t value)
{
    txn_t t;

    if (bad_addr(addr)) return SMB_E_INVAL;
    txn_init(&t, PROT_BYTE_DATA, addr, 0, cmd);
    t.d0 = value;
    return run(d, &t);
}

int smb_read_byte_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint8_t *value)
{
    txn_t t;
    int rc;

    if (bad_addr(addr) || value == NULL) return SMB_E_INVAL;
    txn_init(&t, PROT_BYTE_DATA, addr, 1, cmd);
    rc = run(d, &t);
    if (rc == SMB_OK) {
        *value = rd(d, SMB_HST_D0);
    }
    return rc;
}

int smb_write_word_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint16_t value)
{
    txn_t t;

    if (bad_addr(addr)) return SMB_E_INVAL;
    txn_init(&t, PROT_WORD_DATA, addr, 0, cmd);
    t.d0 = (uint8_t)(value & 0xFF);         /* SMBus sends the low byte first */
    t.d1 = (uint8_t)(value >> 8);
    return run(d, &t);
}

int smb_read_word_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint16_t *value)
{
    txn_t t;
    int rc;

    if (bad_addr(addr) || value == NULL) return SMB_E_INVAL;
    txn_init(&t, PROT_WORD_DATA, addr, 1, cmd);
    rc = run(d, &t);
    if (rc == SMB_OK) {
        *value = (uint16_t)(rd(d, SMB_HST_D0) | (rd(d, SMB_HST_D1) << 8));
    }
    return rc;
}

int smb_block_write(smb_dev_t *d, uint8_t addr, uint8_t cmd, const uint8_t *data, unsigned len)
{
    txn_t t;

    if (bad_addr(addr) || data == NULL || len < 1 || len > SMB_BLOCK_MAX) return SMB_E_INVAL;
    if (!(d->caps & SMB_CAP_BLOCK)) return SMB_E_UNSUPPORTED;
    txn_init(&t, PROT_BLOCK, addr, 0, cmd);
    t.d0 = (uint8_t)len;
    t.blk = BLK_WRITE;
    t.wbuf = data;
    t.len = len;
    return run(d, &t);
}

int smb_block_read(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint8_t *data, unsigned cap, unsigned *count)
{
    txn_t t;
    int rc;

    if (bad_addr(addr) || data == NULL || count == NULL) return SMB_E_INVAL;
    if (!(d->caps & SMB_CAP_BLOCK)) return SMB_E_UNSUPPORTED;
    txn_init(&t, PROT_BLOCK, addr, 1, cmd);
    t.blk = BLK_READ_SMBUS;
    t.rbuf = data;
    t.len = cap;
    rc = run(d, &t);
    *count = t.count;
    return rc;
}

int smb_i2c_block_read(smb_dev_t *d, uint8_t addr, uint8_t offset, uint8_t *data, unsigned len)
{
    txn_t t;

    if (bad_addr(addr) || data == NULL || len < 1 || len > SMB_BLOCK_MAX) return SMB_E_INVAL;
    if (!(d->caps & SMB_CAP_I2C_READ)) return SMB_E_UNSUPPORTED;
    /*
     * The datasheet has the read bit set for this command and the offset in
     * the DATA1 register. The offset is also placed in the command register
     * and the length in DATA0: real controllers ignore both for this command,
     * and it keeps the request self-describing for emulated ones.
     */
    txn_init(&t, PROT_I2C_READ, addr, 1, offset);
    t.d0 = (uint8_t)len;
    t.d1 = offset;
    t.blk = BLK_READ_I2C;
    t.rbuf = data;
    t.len = len;
    return run(d, &t);
}

const char *smb_strerror(int err)
{
    switch (err) {
    case SMB_OK:            return "ok";
    case SMB_E_NACK:        return "no acknowledge (no device at this address, or it refused)";
    case SMB_E_BUS:         return "bus error (collision or lost arbitration)";
    case SMB_E_TIMEOUT:     return "timed out; transaction killed";
    case SMB_E_BUSY:        return "controller busy";
    case SMB_E_ABORTED:     return "transaction aborted";
    case SMB_E_UNSUPPORTED: return "request cannot be expressed as an SMBus transaction on this controller";
    case SMB_E_INVAL:       return "invalid argument";
    case SMB_E_PROTO:       return "device broke the block protocol";
    default:                return "unknown error";
    }
}
