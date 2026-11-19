/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software model of the chipset SMBus controller and of simple I2C chips,
 * for unit-testing driver/core on a development host.
 *
 * The model works at wire level: every transaction is played as start /
 * byte / acknowledge / stop events against the chip models, and the events
 * are recorded as text. Tests can therefore assert the exact bytes a request
 * puts on the bus, not only the data that comes back.
 *
 * The model is written from the same datasheets as the driver, so it checks
 * the driver's logic and its handling of timing, errors and corner cases.
 * It is not evidence about real hardware; tests/baremetal and the QNX bench
 * run the same core against QEMU's independent controller model for that.
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include "smb_core.h"

#define SIM_MAX_CHIPS  8
#define SIM_TRACE_MAX  2048

/* A chip with 256 one-byte registers and an auto-incrementing pointer:
 * the first byte written sets the pointer, further bytes are stored,
 * reads return bytes from the pointer onwards. Models a small EEPROM and,
 * closely enough for transfer tests, a register-based sensor. */
typedef struct sim_chip {
    uint8_t  addr;
    uint8_t  mem[256];
    uint8_t  ptr;
    int      got_ptr;       /* within the current write: pointer byte already received */
    int      nack_data;     /* fault: refuse every data byte after the pointer */
} sim_chip_t;

typedef struct sim {
    /* --- configuration --- */
    int      has_block;         /* controller implements SMBus block commands        */
    int      has_i2c_read;      /* controller implements the I2C block read command  */
    int      i2c_en;            /* block write omits the count byte                  */
    int      last_by_intr;      /* 0: final block byte is announced with BYTE_DONE,
                                      then INTR after it is cleared
                                   1: final read byte is announced with INTR alone   */
    int      latency;           /* status reads before each controller step completes */
    /* --- fault injection --- */
    int      stuck;             /* controller never completes                        */
    int      byte_storm;        /* block: re-raises BYTE_DONE forever without moving
                                   on (QEMU's pm_smbus does this on 32-byte writes)  */
    int      bus_err_next;      /* next transaction ends with a bus error            */
    int      ext_busy_reads;    /* HOST_BUSY reads while idle; -1 = forever          */
    /* --- chips --- */
    sim_chip_t chips[SIM_MAX_CHIPS];
    int        nchips;
    /* --- controller registers --- */
    uint8_t  sts, cnt, cmd, xslva, d0, d1, blk;
    /* --- controller run state --- */
    int      active;            /* a transaction is running                          */
    int      pend;              /* step waiting for its latency to pass              */
    int      delay;
    int      on_wire;           /* a start has been issued, stop still owed          */
    int      blk_mode;
    unsigned blk_idx;           /* bytes moved                                       */
    unsigned blk_count;
    int      blk_final_shown;   /* final byte is in the data register, INTR owed     */
    sim_chip_t *cur;            /* chip addressed by the running transaction         */
    /* --- observation --- */
    char     trace[SIM_TRACE_MAX];
    unsigned trace_len;
    uint64_t clock_us;
    unsigned irq_done_calls;
    unsigned irq_done_while_asserted;   /* must stay 0: unmasking with the line high */
    unsigned starts;                    /* transactions started                      */
} sim_t;

void        sim_init(sim_t *s);
sim_chip_t *sim_add_chip(sim_t *s, uint8_t addr);
void        sim_trace_clear(sim_t *s);
int         sim_irq_line(const sim_t *s);
void        sim_bind(sim_t *s, smb_dev_t *d);    /* fill d->io with the model's callbacks */

#endif /* SIM_H */
