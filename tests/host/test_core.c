/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for driver/core against the software controller model.
 * Build and run: see tests/host/Makefile. Exit status is non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>
#include "smb_core.h"
#include "ichsmb_regs.h"
#include "sim.h"

static int g_checks, g_failed, g_case_failed, g_cases;
static const char *g_case;

#define CHECK(cond) do {                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_case_failed++;                                                 \
            printf("    FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                    \
    } while (0)

#define CHECK_INT(got, want) do {                                            \
        long g_ = (long)(got), w_ = (long)(want);                            \
        g_checks++;                                                          \
        if (g_ != w_) {                                                      \
            g_case_failed++;                                                 \
            printf("    FAILED %s:%d  %s = %ld, expected %ld\n",             \
                   __FILE__, __LINE__, #got, g_, w_);                        \
        }                                                                    \
    } while (0)

#define CHECK_TRACE(sim, want) do {                                          \
        g_checks++;                                                          \
        if (strcmp((sim)->trace, (want)) != 0) {                             \
            g_case_failed++;                                                 \
            printf("    FAILED %s:%d  wire trace\n      got:      \"%s\"\n"  \
                   "      expected: \"%s\"\n", __FILE__, __LINE__,           \
                   (sim)->trace, (want));                                    \
        }                                                                    \
    } while (0)

static void run_case(const char *name, void (*fn)(void))
{
    g_case = name;
    g_cases++;
    g_case_failed = 0;
    fn();
    printf("%-4s %s\n", g_case_failed ? "FAIL" : "ok", name);
    if (g_case_failed) {
        g_failed++;
    }
}

/* ---------------------------------------------------------------- fixture */

typedef struct {
    sim_t      sim;
    smb_dev_t  dev;
    smb_bus_t  bus;
    sim_chip_t *chip;       /* at 0x50 */
} fx_t;

enum { CTRL_ICH_I2C, CTRL_ICH_SMBUS, CTRL_BASIC };

static void fx_init(fx_t *f, int ctrl)
{
    sim_init(&f->sim);
    if (ctrl == CTRL_ICH_I2C) {
        f->sim.i2c_en = 1;
    } else if (ctrl == CTRL_BASIC) {            /* PIIX4-class: no block commands */
        f->sim.has_block = 0;
        f->sim.has_i2c_read = 0;
    }
    f->chip = sim_add_chip(&f->sim, 0x50);
    sim_bind(&f->sim, &f->dev);
    smb_bus_init(&f->bus, &f->dev);
    smb_bus_set_addr(&f->bus, 0x50);
}

/* The controller must be left idle, clean and unkilled after every request. */
static void check_idle(fx_t *f)
{
    CHECK_INT(f->sim.active, 0);
    CHECK_INT(f->sim.sts & (STS_EVENTS | STS_HOST_BUSY), 0);
    CHECK_INT(f->sim.cnt & CNT_KILL, 0);
    CHECK_INT(f->sim.on_wire, 0);
    CHECK_INT(f->sim.irq_done_while_asserted, 0);
}

/* ------------------------------------------------- mapping table, by wire */

static void t_map_quick(void)
{
    fx_t f; fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, NULL, 0, NULL, 0), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w P");
    check_idle(&f);
}

static void t_map_send_byte(void)
{
    fx_t f; uint8_t s[] = { 0x10 };
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, NULL, 0), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 P");
    check_idle(&f);
}

static void t_map_write_byte_data(void)
{
    fx_t f; uint8_t s[] = { 0x10, 0x77 };
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 2, NULL, 0), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 77 P");
    CHECK_INT(f.chip->mem[0x10], 0x77);
    check_idle(&f);
}

static void t_map_write_word_data(void)
{
    fx_t f; uint8_t s[] = { 0x10, 0x34, 0x12 };
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 3, NULL, 0), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 34 12 P");
    CHECK_INT(f.chip->mem[0x10], 0x34);
    CHECK_INT(f.chip->mem[0x11], 0x12);
    check_idle(&f);
}

static void t_map_i2c_write(void)
{
    fx_t f; uint8_t s[] = { 0x20, 1, 2, 3, 4 };
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 5, NULL, 0), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 20 01 02 03 04 P");
    CHECK(memcmp(&f.chip->mem[0x20], s + 1, 4) == 0);
    check_idle(&f);
}

static void t_map_smbus_block_write(void)
{
    /* Controller inserts the count byte itself; request is already in SMBus block form. */
    fx_t f; uint8_t s[] = { 0x20, 3, 9, 8, 7 };
    fx_init(&f, CTRL_ICH_SMBUS);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 5, NULL, 0), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 20 03 09 08 07 P");
    check_idle(&f);
}

static void t_map_block_write_not_wellformed(void)
{
    /* Count byte would not match: sending it would put different bytes on the wire. */
    fx_t f; uint8_t s[] = { 0x20, 1, 2, 3, 4 };
    fx_init(&f, CTRL_ICH_SMBUS);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 5, NULL, 0), SMB_E_UNSUPPORTED);
    CHECK_TRACE(&f.sim, "");
    CHECK_INT(f.sim.starts, 0);
}

static void t_map_receive_byte(void)
{
    fx_t f; uint8_t r = 0;
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, NULL, 0, &r, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50r A5n P");
    CHECK_INT(r, 0xA5);
    check_idle(&f);
}

static void t_map_read_byte_data(void)
{
    fx_t f; uint8_t s[] = { 0x10 }, r = 0;
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, &r, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5n P");
    CHECK_INT(r, 0xB5);
    check_idle(&f);
}

static void t_map_read_word_data(void)
{
    fx_t f; uint8_t s[] = { 0x10 }, r[2] = { 0, 0 };
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 2), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5a B4n P");
    CHECK_INT(r[0], 0xB5);
    CHECK_INT(r[1], 0xB4);
    check_idle(&f);
}

static void t_map_i2c_block_read(void)
{
    fx_t f; uint8_t s[] = { 0x10 }, r[4] = { 0 };
    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 4), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5a B4a B7a B6n P");
    CHECK(r[0] == 0xB5 && r[1] == 0xB4 && r[2] == 0xB7 && r[3] == 0xB6);
    check_idle(&f);
}

static void t_map_smbus_block_read(void)
{
    fx_t f; uint8_t s[] = { 0x30 }, r[4] = { 0 };
    fx_init(&f, CTRL_ICH_SMBUS);
    f.bus.smbus_block_read = 1;
    f.chip->mem[0x30] = 3; f.chip->mem[0x31] = 0x11; f.chip->mem[0x32] = 0x22; f.chip->mem[0x33] = 0x33;
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 4), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 30 Sr50r 03a 11a 22a 33n P");
    CHECK(r[0] == 3 && r[1] == 0x11 && r[2] == 0x22 && r[3] == 0x33);
    check_idle(&f);
}

static void t_map_smbus_block_read_short_buffer(void)
{
    /* Device announces 3 bytes, caller has room for the count and 1: the whole
     * block is still clocked out, the count byte reports the truncation. */
    fx_t f; uint8_t s[] = { 0x30 }, r[3] = { 0, 0, 0xEE };
    fx_init(&f, CTRL_ICH_SMBUS);
    f.bus.smbus_block_read = 1;
    f.chip->mem[0x30] = 3; f.chip->mem[0x31] = 0x11; f.chip->mem[0x32] = 0x22; f.chip->mem[0x33] = 0x33;
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 2), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 30 Sr50r 03a 11a 22a 33n P");
    CHECK(r[0] == 3 && r[1] == 0x11 && r[2] == 0xEE);
    check_idle(&f);
}

static void t_map_refusals(void)
{
    fx_t f; uint8_t s[40] = { 0 }, r[40];

    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_xfer(&f.bus, NULL, 0, r, 2), SMB_E_UNSUPPORTED);  /* multi-byte read, no index */
    CHECK_INT(smb_bus_xfer(&f.bus, s, 2, r, 1), SMB_E_UNSUPPORTED);     /* two index bytes, then read */
    CHECK_INT(smb_bus_xfer(&f.bus, s, 34, NULL, 0), SMB_E_UNSUPPORTED); /* over 32 data bytes */
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 33), SMB_E_UNSUPPORTED);    /* over 32 bytes read */
    CHECK_INT(smb_bus_xfer(&f.bus, NULL, 2, NULL, 0), SMB_E_INVAL);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, NULL, 2), SMB_E_INVAL);
    CHECK_INT(smb_bus_set_addr(&f.bus, 0x80), SMB_E_INVAL);             /* 10-bit address */
    CHECK_INT(f.sim.starts, 0);                                          /* none reached the bus */
    CHECK_TRACE(&f.sim, "");
}

static void t_map_basic_controller(void)
{
    /* PIIX4-class controller: the four basic shapes work, block requests are refused. */
    fx_t f; uint8_t s[5] = { 0x10, 3, 1, 2, 3 }, r[4];

    fx_init(&f, CTRL_BASIC);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 1), SMB_OK);
    CHECK_INT(r[0], 0xB5);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 2), SMB_OK);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 4), SMB_E_UNSUPPORTED);
    CHECK_INT(smb_bus_xfer(&f.bus, s, 5, NULL, 0), SMB_E_UNSUPPORTED);
    f.bus.smbus_block_read = 1;
    CHECK_INT(smb_bus_xfer(&f.bus, s, 1, r, 4), SMB_E_UNSUPPORTED);
    check_idle(&f);
}

/* --------------------------------------------------- send()/recv() pairing */

static void t_send_recv_pair(void)
{
    fx_t f; uint8_t s[] = { 0x10 }, r[2];

    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_send(&f.bus, s, 1, 0), SMB_OK);
    CHECK_INT(f.sim.starts, 0);                         /* held: nothing on the bus yet */
    CHECK_INT(smb_bus_recv(&f.bus, r, 2, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5a B4n P");     /* one combined transaction */
    CHECK_INT(f.sim.starts, 1);

    sim_trace_clear(&f.sim);                            /* the held send is used once only */
    CHECK_INT(smb_bus_recv(&f.bus, r, 1, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50r B7n P");                  /* chip pointer moved on to 0x12 */
    check_idle(&f);
}

static void t_send_recv_edge(void)
{
    fx_t f; uint8_t s[40] = { 0x10, 0x55 }, r[2];

    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_bus_recv(&f.bus, r, 1, 0), SMB_E_UNSUPPORTED);    /* read without stop */
    CHECK_INT(smb_bus_send(&f.bus, s, 34, 0), SMB_E_UNSUPPORTED);   /* too long to hold */

    /* A held send is dropped when the target changes ... */
    CHECK_INT(smb_bus_send(&f.bus, s, 1, 0), SMB_OK);
    CHECK_INT(smb_bus_set_addr(&f.bus, 0x50), SMB_OK);
    CHECK_INT(smb_bus_recv(&f.bus, r, 1, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50r A5n P");

    /* ... and when a complete send follows it. */
    sim_trace_clear(&f.sim);
    CHECK_INT(smb_bus_send(&f.bus, s, 1, 0), SMB_OK);
    CHECK_INT(smb_bus_send(&f.bus, s, 2, 1), SMB_OK);
    CHECK_INT(smb_bus_recv(&f.bus, r, 1, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 55 PS50r B4n P");
    check_idle(&f);
}

/* ------------------------------------------------------------------ errors */

static void t_err_no_device(void)
{
    fx_t f; uint8_t v = 0xEE; uint16_t w = 0xEEEE;

    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_quick(&f.dev, 0x20, 0), SMB_E_NACK);
    CHECK_TRACE(&f.sim, "S20w! P");
    CHECK_INT(smb_read_byte_data(&f.dev, 0x20, 0, &v), SMB_E_NACK);
    CHECK_INT(v, 0xEE);                                 /* output untouched on failure */
    CHECK_INT(smb_read_word_data(&f.dev, 0x20, 0, &w), SMB_E_NACK);
    CHECK_INT(w, 0xEEEE);
    CHECK_INT(smb_receive_byte(&f.dev, 0x20, &v), SMB_E_NACK);
    CHECK_INT(smb_i2c_block_read(&f.dev, 0x20, 0, &v, 1), SMB_E_NACK);
    CHECK_INT(f.dev.stats.nacks, 5);
    check_idle(&f);
}

static void t_err_data_nack(void)
{
    fx_t f; uint8_t d[4] = { 1, 2, 3, 4 };

    fx_init(&f, CTRL_ICH_I2C);
    f.chip->nack_data = 1;
    CHECK_INT(smb_write_byte_data(&f.dev, 0x50, 0x10, 0x77), SMB_E_NACK);
    CHECK_TRACE(&f.sim, "S50w 10 77! P");
    sim_trace_clear(&f.sim);
    CHECK_INT(smb_block_write(&f.dev, 0x50, 0x10, d, 4), SMB_E_NACK);    /* fails on the first data byte */
    CHECK_TRACE(&f.sim, "S50w 10 01! P");
    check_idle(&f);
}

static void t_err_bus(void)
{
    fx_t f; uint8_t v;

    fx_init(&f, CTRL_ICH_I2C);
    f.sim.bus_err_next = 1;
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_E_BUS);
    CHECK_INT(f.dev.stats.bus_errors, 1);
    check_idle(&f);
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_OK);
    CHECK_INT(v, 0xB5);
}

static void t_err_timeout_kills(void)
{
    fx_t f; uint8_t v = 0; uint64_t t0;

    fx_init(&f, CTRL_ICH_I2C);
    f.sim.stuck = 1;
    t0 = f.sim.clock_us;
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_E_TIMEOUT);
    CHECK(f.sim.clock_us - t0 >= SMB_DEFAULT_TIMEOUT_US);
    CHECK(f.sim.clock_us - t0 < 2 * SMB_DEFAULT_TIMEOUT_US);
    CHECK_INT(f.dev.stats.timeouts, 1);
    check_idle(&f);                                     /* killed, KILL bit released */

    f.sim.stuck = 0;                                    /* controller usable straight away */
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_OK);
    CHECK_INT(v, 0xB5);
}

static void t_err_byte_storm(void)
{
    fx_t f; uint8_t data[32], v = 0; unsigned i;

    for (i = 0; i < 32; i++) data[i] = (uint8_t)i;
    fx_init(&f, CTRL_ICH_I2C);
    f.sim.byte_storm = 1;
    CHECK_INT(smb_block_write(&f.dev, 0x50, 0x40, data, 32), SMB_E_PROTO);
    CHECK(f.sim.clock_us < SMB_DEFAULT_TIMEOUT_US);       /* stopped by the event limit */
    check_idle(&f);

    f.sim.byte_storm = 0;                                 /* controller usable straight away */
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_OK);
    CHECK_INT(v, 0xB5);
}

static void t_err_custom_timeout(void)
{
    fx_t f; uint8_t v; uint64_t t0;

    fx_init(&f, CTRL_ICH_I2C);
    f.dev.timeout_us = 3000;
    f.sim.stuck = 1;
    t0 = f.sim.clock_us;
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_E_TIMEOUT);
    CHECK(f.sim.clock_us - t0 >= 3000 && f.sim.clock_us - t0 < 6000);
}

static void t_err_busy(void)
{
    fx_t f; uint8_t v = 0;

    fx_init(&f, CTRL_ICH_I2C);
    f.sim.ext_busy_reads = -1;                          /* someone else holds the controller */
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_E_BUSY);
    CHECK_INT(f.sim.starts, 0);                         /* we did not disturb their transaction */
    CHECK_INT(f.dev.stats.busy, 1);

    f.sim.ext_busy_reads = 3;                           /* briefly busy: we wait, then run */
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_OK);
    CHECK_INT(v, 0xB5);
}

static void t_err_bad_block_count(void)
{
    fx_t f; uint8_t r[SMB_BLOCK_MAX]; unsigned n = 99; uint8_t v;

    fx_init(&f, CTRL_ICH_SMBUS);
    f.chip->mem[0x30] = 0;                              /* device announces a zero-length block */
    CHECK_INT(smb_block_read(&f.dev, 0x50, 0x30, r, sizeof(r), &n), SMB_E_PROTO);
    check_idle(&f);
    f.chip->mem[0x30] = 33;                             /* and one over the limit */
    CHECK_INT(smb_block_read(&f.dev, 0x50, 0x30, r, sizeof(r), &n), SMB_E_PROTO);
    check_idle(&f);
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_OK);
}

static void t_err_arguments(void)
{
    fx_t f; uint8_t b[40]; unsigned n; uint16_t w;

    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_quick(&f.dev, 0x80, 0), SMB_E_INVAL);
    CHECK_INT(smb_receive_byte(&f.dev, 0x50, NULL), SMB_E_INVAL);
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0, NULL), SMB_E_INVAL);
    CHECK_INT(smb_read_word_data(&f.dev, 0x50, 0, NULL), SMB_E_INVAL);
    CHECK_INT(smb_block_write(&f.dev, 0x50, 0, b, 0), SMB_E_INVAL);
    CHECK_INT(smb_block_write(&f.dev, 0x50, 0, b, 33), SMB_E_INVAL);
    CHECK_INT(smb_block_write(&f.dev, 0x50, 0, NULL, 4), SMB_E_INVAL);
    CHECK_INT(smb_block_read(&f.dev, 0x50, 0, NULL, 4, &n), SMB_E_INVAL);
    CHECK_INT(smb_block_read(&f.dev, 0x50, 0, b, 4, NULL), SMB_E_INVAL);
    CHECK_INT(smb_i2c_block_read(&f.dev, 0x50, 0, b, 0), SMB_E_INVAL);
    CHECK_INT(smb_i2c_block_read(&f.dev, 0x50, 0, b, 33), SMB_E_INVAL);
    CHECK_INT(smb_write_word_data(&f.dev, 0x90, 0, 0), SMB_E_INVAL);
    (void)w;
    CHECK_INT(f.sim.starts, 0);
}

static void t_recovery_1000(void)
{
    /* A failed request followed immediately by a successful one, 1,000 times,
     * rotating through the four failure kinds. */
    fx_t f; int i, bad = 0; uint8_t v;

    fx_init(&f, CTRL_ICH_I2C);
    f.dev.timeout_us = 2000;
    for (i = 0; i < 1000; i++) {
        int rc, want;

        switch (i % 4) {
        case 0:  rc = smb_read_byte_data(&f.dev, 0x21, 0, &v); want = SMB_E_NACK; break;
        case 1:  f.sim.bus_err_next = 1;
                 rc = smb_read_byte_data(&f.dev, 0x50, 0, &v); want = SMB_E_BUS; break;
        case 2:  f.sim.stuck = 1;
                 rc = smb_read_byte_data(&f.dev, 0x50, 0, &v); want = SMB_E_TIMEOUT;
                 f.sim.stuck = 0; break;
        default: f.sim.ext_busy_reads = -1;
                 rc = smb_read_byte_data(&f.dev, 0x50, 0, &v); want = SMB_E_BUSY;
                 f.sim.ext_busy_reads = 0; break;
        }
        if (rc != want) bad++;
        v = 0;
        if (smb_read_byte_data(&f.dev, 0x50, (uint8_t)i, &v) != SMB_OK || v != (uint8_t)(i ^ 0xA5)) bad++;
    }
    CHECK_INT(bad, 0);
    check_idle(&f);
}

/* ------------------------------------- block transfers across all variants */

/*
 * Every block length 1..32, for each combination of:
 *   - how the controller announces the final byte (BYTE_DONE first, or INTR alone)
 *   - controller latency (0..3 status reads per step)
 *   - polling or interrupt mode
 * Checks data both ways and that the interrupt is never re-armed while asserted.
 */
static void t_block_matrix(void)
{
    int model, lat, irq, bad = 0, runs = 0;
    unsigned len, i;

    for (model = 0; model <= 1; model++)
    for (lat = 0; lat <= 3; lat++)
    for (irq = 0; irq <= 1; irq++)
    for (len = 1; len <= SMB_BLOCK_MAX; len++) {
        fx_t f; uint8_t w[SMB_BLOCK_MAX], r[SMB_BLOCK_MAX]; unsigned n = 0;

        /* I2C framing: block write without count, I2C block read */
        fx_init(&f, CTRL_ICH_I2C);
        f.sim.last_by_intr = model; f.sim.latency = lat; f.dev.use_irq = irq;
        for (i = 0; i < len; i++) w[i] = (uint8_t)(0xC0 + i + len);
        memset(r, 0, sizeof(r));
        if (smb_block_write(&f.dev, 0x50, 0x40, w, len) != SMB_OK) bad++;
        if (memcmp(&f.chip->mem[0x40], w, len) != 0) bad++;
        if (smb_i2c_block_read(&f.dev, 0x50, 0x40, r, len) != SMB_OK) bad++;
        if (memcmp(r, w, len) != 0) bad++;
        if (f.sim.active || f.sim.on_wire || f.sim.irq_done_while_asserted) bad++;
        if (f.sim.sts & (STS_EVENTS | STS_HOST_BUSY)) bad++;
        if (irq && f.sim.irq_done_calls == 0) bad++;

        /* SMBus framing: count byte on the wire both ways */
        fx_init(&f, CTRL_ICH_SMBUS);
        f.sim.last_by_intr = model; f.sim.latency = lat; f.dev.use_irq = irq;
        memset(r, 0, sizeof(r));
        if (smb_block_write(&f.dev, 0x50, 0x40, w, len) != SMB_OK) bad++;
        if (f.chip->mem[0x40] != len || memcmp(&f.chip->mem[0x41], w, len) != 0) bad++;
        if (smb_block_read(&f.dev, 0x50, 0x40, r, sizeof(r), &n) != SMB_OK) bad++;
        if (n != len || memcmp(r, w, len) != 0) bad++;
        if (f.sim.active || f.sim.on_wire || f.sim.irq_done_while_asserted) bad++;
        if (f.sim.sts & (STS_EVENTS | STS_HOST_BUSY)) bad++;
        runs += 4;
    }
    CHECK_INT(bad, 0);
    CHECK_INT(runs, 2 * 4 * 2 * 32 * 4);
}

static void t_simple_matrix(void)
{
    /* The non-block transactions under the same latency and interrupt variations. */
    int lat, irq, bad = 0;

    for (lat = 0; lat <= 5; lat++)
    for (irq = 0; irq <= 1; irq++) {
        fx_t f; uint8_t v = 0; uint16_t w = 0;

        fx_init(&f, CTRL_BASIC);
        f.sim.latency = lat; f.dev.use_irq = irq;
        if (smb_quick(&f.dev, 0x50, 0) != SMB_OK) bad++;
        if (smb_quick(&f.dev, 0x50, 1) != SMB_OK) bad++;
        if (smb_write_byte_data(&f.dev, 0x50, 0x60, 0x5A) != SMB_OK) bad++;
        if (smb_read_byte_data(&f.dev, 0x50, 0x60, &v) != SMB_OK || v != 0x5A) bad++;
        if (smb_write_word_data(&f.dev, 0x50, 0x62, 0xBEEF) != SMB_OK) bad++;
        if (smb_read_word_data(&f.dev, 0x50, 0x62, &w) != SMB_OK || w != 0xBEEF) bad++;
        if (smb_send_byte(&f.dev, 0x50, 0x60) != SMB_OK) bad++;
        if (smb_receive_byte(&f.dev, 0x50, &v) != SMB_OK || v != 0x5A) bad++;
        if (f.sim.irq_done_while_asserted) bad++;
        if ((f.sim.cnt & CNT_INTREN) != (irq ? CNT_INTREN : 0)) bad++;
        if (lat > 0 && f.dev.stats.waits == 0) bad++;
    }
    CHECK_INT(bad, 0);
}

static void t_wire_block_last_byte(void)
{
    /* The final byte of a read must be the only one the controller does not acknowledge. */
    fx_t f; uint8_t r[3];

    fx_init(&f, CTRL_ICH_I2C);
    CHECK_INT(smb_i2c_block_read(&f.dev, 0x50, 0x10, r, 1), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5n P");
    sim_trace_clear(&f.sim);
    CHECK_INT(smb_i2c_block_read(&f.dev, 0x50, 0x10, r, 2), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5a B4n P");
    sim_trace_clear(&f.sim);
    CHECK_INT(smb_i2c_block_read(&f.dev, 0x50, 0x10, r, 3), SMB_OK);
    CHECK_TRACE(&f.sim, "S50w 10 Sr50r B5a B4a B7n P");
}

static void t_reset(void)
{
    fx_t f; uint8_t v;

    fx_init(&f, CTRL_ICH_I2C);
    f.sim.sts |= STS_DEV_ERR | STS_INTR;                /* stale status from before start-up */
    smb_reset(&f.dev);
    check_idle(&f);
    CHECK_INT(smb_read_byte_data(&f.dev, 0x50, 0x10, &v), SMB_OK);
}

static void t_strerror(void)
{
    int e;

    for (e = 0; e >= SMB_E_PROTO; e--) {
        CHECK(smb_strerror(e) != NULL && strcmp(smb_strerror(e), "unknown error") != 0);
    }
    CHECK(strcmp(smb_strerror(-99), "unknown error") == 0);
}

int main(void)
{
    run_case("map: 0 bytes            -> Quick", t_map_quick);
    run_case("map: write 1            -> Send Byte", t_map_send_byte);
    run_case("map: write 2            -> Write Byte Data", t_map_write_byte_data);
    run_case("map: write 3            -> Write Word Data", t_map_write_word_data);
    run_case("map: write 5 (I2C_EN)   -> block write, no count byte", t_map_i2c_write);
    run_case("map: write cmd,count,.. -> SMBus Block Write", t_map_smbus_block_write);
    run_case("map: block write whose count would be wrong is refused", t_map_block_write_not_wellformed);
    run_case("map: read 1             -> Receive Byte", t_map_receive_byte);
    run_case("map: write 1, read 1    -> Read Byte Data", t_map_read_byte_data);
    run_case("map: write 1, read 2    -> Read Word Data", t_map_read_word_data);
    run_case("map: write 1, read 4    -> I2C Block Read", t_map_i2c_block_read);
    run_case("map: write 1, read n    -> SMBus Block Read (count first)", t_map_smbus_block_read);
    run_case("map: SMBus Block Read into a short buffer", t_map_smbus_block_read_short_buffer);
    run_case("map: inexpressible requests are refused before the bus", t_map_refusals);
    run_case("map: controller without block commands", t_map_basic_controller);
    run_case("pair: send(no stop)+recv is one combined transaction", t_send_recv_pair);
    run_case("pair: held send edge cases", t_send_recv_edge);
    run_case("error: no device at address", t_err_no_device);
    run_case("error: device refuses data byte", t_err_data_nack);
    run_case("error: bus collision", t_err_bus);
    run_case("error: stuck controller is killed after timeout", t_err_timeout_kills);
    run_case("error: configurable timeout", t_err_custom_timeout);
    run_case("error: endless byte events end in an error, not a hang", t_err_byte_storm);
    run_case("error: controller busy with another owner", t_err_busy);
    run_case("error: device announces illegal block count", t_err_bad_block_count);
    run_case("error: invalid arguments never reach the bus", t_err_arguments);
    run_case("recovery: 1000 x (failure, then success)", t_recovery_1000);
    run_case("block: lengths 1..32 x 2 controller behaviours x latency x irq", t_block_matrix);
    run_case("basic transactions x latency x irq", t_simple_matrix);
    run_case("wire: only the final read byte is not acknowledged", t_wire_block_last_byte);
    run_case("reset clears stale status", t_reset);
    run_case("every error code has a message", t_strerror);

    printf("\n%d cases, %d failed, %d checks\n", g_cases, g_failed, g_checks);
    return g_failed ? 1 : 0;
}
