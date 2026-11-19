/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software model of the chipset SMBus controller. See sim.h.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "sim.h"
#include "ichsmb_regs.h"

enum { PEND_NONE, PEND_START, PEND_NEXT };
enum { MODE_NONE, MODE_WRITE, MODE_READ_SMBUS, MODE_READ_I2C };

/* ---------------------------------------------------------------- wire */

static void tr(sim_t *s, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (s->trace_len >= SIM_TRACE_MAX - 1) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(s->trace + s->trace_len, SIM_TRACE_MAX - s->trace_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s->trace_len += (unsigned)n;
        if (s->trace_len > SIM_TRACE_MAX - 1) {
            s->trace_len = SIM_TRACE_MAX - 1;
        }
    }
}

static sim_chip_t *find_chip(sim_t *s, uint8_t addr)
{
    int i;

    for (i = 0; i < s->nchips; i++) {
        if (s->chips[i].addr == addr) {
            return &s->chips[i];
        }
    }
    return NULL;
}

/* Start or repeated start. Returns 1 if a chip acknowledged its address. */
static int w_start(sim_t *s, uint8_t addr, int read)
{
    s->cur = find_chip(s, addr);
    tr(s, "%s%02X%c%s ", s->on_wire ? "Sr" : "S", addr, read ? 'r' : 'w', s->cur ? "" : "!");
    s->on_wire = 1;
    if (s->cur != NULL && !read) {
        s->cur->got_ptr = 0;
    }
    return s->cur != NULL;
}

/* Controller sends one byte. Returns 1 if the chip acknowledged it. */
static int w_write(sim_t *s, uint8_t b)
{
    sim_chip_t *c = s->cur;
    int ack = 1;

    if (!c->got_ptr) {
        c->ptr = b;
        c->got_ptr = 1;
    } else if (c->nack_data) {
        ack = 0;
    } else {
        c->mem[c->ptr++] = b;
    }
    tr(s, "%02X%s ", b, ack ? "" : "!");
    return ack;
}

/* Controller receives one byte and answers with ack (more wanted) or nack (last). */
static uint8_t w_read(sim_t *s, int ack)
{
    uint8_t b = s->cur->mem[s->cur->ptr++];

    tr(s, "%02X%c ", b, ack ? 'a' : 'n');
    return b;
}

static void w_stop(sim_t *s)
{
    if (s->on_wire) {
        tr(s, "P");
        s->on_wire = 0;
    }
}

/* ---------------------------------------------------------- controller */

static void finish_ok(sim_t *s)
{
    w_stop(s);
    s->sts |= STS_INTR;
    s->sts &= (uint8_t)~STS_HOST_BUSY;
    s->active = 0;
    s->pend = PEND_NONE;
}

static void finish_err(sim_t *s, uint8_t bit)
{
    w_stop(s);
    s->sts |= bit;
    s->sts &= (uint8_t)~(STS_HOST_BUSY | STS_BYTE_DONE);
    s->active = 0;
    s->pend = PEND_NONE;
}

static void after_write_byte(sim_t *s)
{
    if (s->blk_idx == s->blk_count) {
        if (s->last_by_intr) {
            finish_ok(s);
            return;
        }
        s->blk_final_shown = 1;
    }
    s->sts |= STS_BYTE_DONE;
}

/* Receive the next block byte into the data register and announce it. */
static void next_read_byte(sim_t *s, int first)
{
    int final;

    if (s->last_by_intr) {
        /* This behaviour ignores LAST_BYTE on the first byte and announces a
         * requested final byte with INTR alone. */
        if (!first && (s->cnt & CNT_LAST_BYTE)) {
            s->blk = w_read(s, 0);
            s->blk_idx++;
            finish_ok(s);
            return;
        }
        s->blk = w_read(s, 1);
        s->blk_idx++;
        s->sts |= STS_BYTE_DONE;
        return;
    }
    final = (s->cnt & CNT_LAST_BYTE) != 0
         || (s->blk_mode == MODE_READ_SMBUS && s->blk_idx + 1 == s->blk_count)
         || s->blk_idx + 1 == SMB_BLOCK_MAX;
    s->blk = w_read(s, !final);
    s->blk_idx++;
    s->blk_final_shown = final;
    s->sts |= STS_BYTE_DONE;
}

static void exec_start(sim_t *s)
{
    const uint8_t prot = (s->cnt >> CNT_PROT_SHIFT) & 7;
    const uint8_t addr = s->xslva >> 1;
    const int     read = s->xslva & 1;

    if (s->bus_err_next) {
        s->bus_err_next = 0;
        finish_err(s, STS_BUS_ERR);
        return;
    }

    switch (prot) {
    case PROT_QUICK:
        if (!w_start(s, addr, read)) goto nack;
        finish_ok(s);
        return;

    case PROT_BYTE:
        if (!w_start(s, addr, read)) goto nack;
        if (read) {
            s->d0 = w_read(s, 0);
        } else if (!w_write(s, s->cmd)) {
            goto nack;
        }
        finish_ok(s);
        return;

    case PROT_BYTE_DATA:
    case PROT_WORD_DATA:
        if (!w_start(s, addr, 0)) goto nack;
        if (!w_write(s, s->cmd)) goto nack;
        if (read) {
            if (!w_start(s, addr, 1)) goto nack;
            if (prot == PROT_WORD_DATA) {
                s->d0 = w_read(s, 1);
                s->d1 = w_read(s, 0);
            } else {
                s->d0 = w_read(s, 0);
            }
        } else {
            if (!w_write(s, s->d0)) goto nack;
            if (prot == PROT_WORD_DATA && !w_write(s, s->d1)) goto nack;
        }
        finish_ok(s);
        return;

    case PROT_BLOCK:
        if (!s->has_block) goto nack;       /* illegal command is reported as a device error */
        if (!w_start(s, addr, 0)) goto nack;
        if (!w_write(s, s->cmd)) goto nack;
        if (read) {
            if (!w_start(s, addr, 1)) goto nack;
            s->blk_mode = MODE_READ_SMBUS;
            s->blk_count = w_read(s, 1);
            s->d0 = (uint8_t)s->blk_count;
            next_read_byte(s, 1);
        } else {
            if (s->d0 < 1 || s->d0 > SMB_BLOCK_MAX) goto nack;
            s->blk_mode = MODE_WRITE;
            s->blk_count = s->d0;
            if (!s->i2c_en && !w_write(s, s->d0)) goto nack;
            if (!w_write(s, s->blk)) goto nack;
            s->blk_idx = 1;
            after_write_byte(s);
        }
        return;

    case PROT_I2C_READ:
        if (!s->has_i2c_read) goto nack;
        if (!w_start(s, addr, 0)) goto nack;
        if (!w_write(s, s->d1)) goto nack;
        if (!w_start(s, addr, 1)) goto nack;
        s->blk_mode = MODE_READ_I2C;
        s->blk_count = 0;
        next_read_byte(s, 1);
        return;

    default:
        break;
    }
nack:
    finish_err(s, STS_DEV_ERR);
}

/* The driver cleared BYTE_DONE: move on to the next block byte. */
static void exec_next(sim_t *s)
{
    if (s->byte_storm) {
        s->sts |= STS_BYTE_DONE;
        return;
    }
    if (s->blk_final_shown) {
        finish_ok(s);
        return;
    }
    if (s->blk_mode == MODE_WRITE) {
        if (!w_write(s, s->blk)) {
            finish_err(s, STS_DEV_ERR);
            return;
        }
        s->blk_idx++;
        after_write_byte(s);
    } else {
        next_read_byte(s, 0);
    }
}

static uint8_t sim_in8(void *ctx, unsigned reg)
{
    sim_t *s = ctx;

    switch (reg) {
    case SMB_HST_STS:
        if (!s->active && s->ext_busy_reads != 0) {
            if (s->ext_busy_reads > 0) {
                s->ext_busy_reads--;
            }
            return s->sts | STS_HOST_BUSY;
        }
        if (s->active && s->pend != PEND_NONE && !s->stuck) {
            if (s->delay > 0) {
                s->delay--;
            } else {
                int p = s->pend;

                s->pend = PEND_NONE;
                if (p == PEND_START) {
                    exec_start(s);
                } else {
                    exec_next(s);
                }
            }
        }
        return s->sts;
    case SMB_HST_CNT:   return s->cnt;
    case SMB_HST_CMD:   return s->cmd;
    case SMB_XMIT_SLVA: return s->xslva;
    case SMB_HST_D0:    return s->d0;
    case SMB_HST_D1:    return s->d1;
    case SMB_BLOCK_DB:  return s->blk;
    default:            return 0xFF;
    }
}

static void sim_out8(void *ctx, unsigned reg, uint8_t val)
{
    sim_t *s = ctx;

    switch (reg) {
    case SMB_HST_STS: {
        const int byte_done_cleared = (val & s->sts & STS_BYTE_DONE) != 0;

        s->sts &= (uint8_t)~(val & STS_EVENTS);
        if (byte_done_cleared && s->active && s->blk_mode != MODE_NONE) {
            s->pend = PEND_NEXT;
            s->delay = s->latency;
        }
        break;
    }
    case SMB_HST_CNT:
        s->cnt = val & (uint8_t)~CNT_START;
        if (val & CNT_KILL) {
            if (s->active || (val & CNT_START)) {
                finish_err(s, STS_FAILED);
            }
        } else if ((val & CNT_START) && !s->active) {
            s->active = 1;
            s->starts++;
            s->sts |= STS_HOST_BUSY;
            s->pend = PEND_START;
            s->delay = s->latency;
            s->blk_mode = MODE_NONE;
            s->blk_idx = 0;
            s->blk_count = 0;
            s->blk_final_shown = 0;
        }
        break;
    case SMB_HST_CMD:   s->cmd = val;   break;
    case SMB_XMIT_SLVA: s->xslva = val; break;
    case SMB_HST_D0:    s->d0 = val;    break;
    case SMB_HST_D1:    s->d1 = val;    break;
    case SMB_BLOCK_DB:  s->blk = val;   break;
    default: break;
    }
}

int sim_irq_line(const sim_t *s)
{
    return (s->cnt & CNT_INTREN) && (s->sts & STS_EVENTS);
}

static void sim_wait(void *ctx, uint32_t max_us)
{
    sim_t *s = ctx;

    s->clock_us += max_us ? max_us : 1;
}

static void sim_irq_done(void *ctx)
{
    sim_t *s = ctx;

    s->irq_done_calls++;
    if (sim_irq_line(s)) {
        s->irq_done_while_asserted++;
    }
}

static uint64_t sim_now(void *ctx)
{
    sim_t *s = ctx;

    return ++s->clock_us;
}

void sim_init(sim_t *s)
{
    memset(s, 0, sizeof(*s));
    s->has_block = 1;
    s->has_i2c_read = 1;
}

sim_chip_t *sim_add_chip(sim_t *s, uint8_t addr)
{
    sim_chip_t *c;
    int i;

    if (s->nchips >= SIM_MAX_CHIPS) {
        return NULL;
    }
    c = &s->chips[s->nchips++];
    memset(c, 0, sizeof(*c));
    c->addr = addr;
    for (i = 0; i < 256; i++) {
        c->mem[i] = (uint8_t)(i ^ 0xA5);    /* recognisable, position-dependent content */
    }
    return c;
}

void sim_trace_clear(sim_t *s)
{
    s->trace_len = 0;
    s->trace[0] = '\0';
}

void sim_bind(sim_t *s, smb_dev_t *d)
{
    memset(d, 0, sizeof(*d));
    d->io.ctx = s;
    d->io.in8 = sim_in8;
    d->io.out8 = sim_out8;
    d->io.wait = sim_wait;
    d->io.irq_done = sim_irq_done;
    d->io.now_us = sim_now;
    if (s->has_block)    d->caps |= SMB_CAP_BLOCK;
    if (s->has_i2c_read) d->caps |= SMB_CAP_I2C_READ;
    if (s->has_block && s->i2c_en) d->caps |= SMB_CAP_I2C_WRITE;
}
