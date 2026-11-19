/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable core of the i2c-ichsmb driver.
 *
 * Everything in driver/core is plain C99 with no operating-system calls.
 * The controller is reached only through the smb_io_t callbacks, so the
 * same code runs
 *   - inside the QNX driver (driver/qnx), on real I/O ports,
 *   - on a development host against a software model (tests/host),
 *   - on bare metal inside QEMU against QEMU's own controller model
 *     (tests/baremetal).
 *
 * Two layers:
 *   smb_*      one function per SMBus transaction type
 *   smb_bus_*  maps generic I2C requests (send / recv / combined transfer),
 *              as issued by the QNX I2C framework, onto those transactions
 */
#ifndef SMB_CORE_H
#define SMB_CORE_H

#include <stdint.h>

#define SMB_BLOCK_MAX   32      /* SMBus limit for one block transfer */

/* Result codes. 0 is success, the rest are negative. */
enum {
    SMB_OK            =  0,
    SMB_E_NACK        = -1,     /* no device answered, or device refused the byte */
    SMB_E_BUS         = -2,     /* bus collision / lost arbitration               */
    SMB_E_TIMEOUT     = -3,     /* controller did not finish; transaction killed  */
    SMB_E_BUSY        = -4,     /* controller busy with someone else's transaction */
    SMB_E_ABORTED     = -5,     /* controller reports the transaction was killed  */
    SMB_E_UNSUPPORTED = -6,     /* request cannot be expressed on this controller */
    SMB_E_INVAL       = -7,     /* bad argument                                   */
    SMB_E_PROTO       = -8      /* device broke the protocol (bad block count)    */
};

const char *smb_strerror(int err);

/* How the core reaches the hardware. */
typedef struct smb_io {
    void     *ctx;
    uint8_t (*in8)(void *ctx, unsigned reg);
    void    (*out8)(void *ctx, unsigned reg, uint8_t val);
    /*
     * Give the controller time to make progress, at most max_us microseconds.
     * Polling mode: a short sleep. Interrupt mode: block until the interrupt
     * event arrives. The core re-reads the status register after every return,
     * so an early or spurious return is harmless.
     */
    void    (*wait)(void *ctx, uint32_t max_us);
    /*
     * Interrupt mode only (may be NULL): called after the core has serviced
     * and cleared the controller's status, so the interrupt line is low again
     * and can be unmasked.
     */
    void    (*irq_done)(void *ctx);
    uint64_t (*now_us)(void *ctx);  /* monotonic clock */
} smb_io_t;

/* What this controller can do beyond quick/byte/byte-data/word-data. */
#define SMB_CAP_BLOCK      0x01u  /* SMBus block read/write                      */
#define SMB_CAP_I2C_READ   0x02u  /* I2C block read command                      */
#define SMB_CAP_I2C_WRITE  0x04u  /* block write without count byte (I2C_EN set) */

typedef struct smb_stats {
    uint32_t transactions;
    uint32_t nacks;
    uint32_t bus_errors;
    uint32_t timeouts;
    uint32_t busy;
    uint32_t waits;             /* times the core had to wait for the controller */
} smb_stats_t;

typedef struct smb_dev {
    smb_io_t    io;
    unsigned    caps;           /* SMB_CAP_*                                 */
    int         use_irq;        /* set CNT_INTREN on every transaction       */
    uint32_t    timeout_us;     /* per transaction; 0 selects the default    */
    uint32_t    poll_us;        /* longest single wait; 0 selects the default */
    smb_stats_t stats;
} smb_dev_t;

#define SMB_DEFAULT_TIMEOUT_US  100000u  /* 100 ms; SMBus devices must answer within 35 ms */
#define SMB_DEFAULT_POLL_US        250u

/* ---- SMBus transactions. addr is the 7-bit address. ---- */
int smb_quick(smb_dev_t *d, uint8_t addr, int read);
int smb_send_byte(smb_dev_t *d, uint8_t addr, uint8_t value);
int smb_receive_byte(smb_dev_t *d, uint8_t addr, uint8_t *value);
int smb_write_byte_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint8_t value);
int smb_read_byte_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint8_t *value);
int smb_write_word_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint16_t value);
int smb_read_word_data(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint16_t *value);
/* Block write of len (1..32) bytes. Whether the controller puts a count byte
 * on the wire depends on its I2C_EN setting (see SMB_CAP_I2C_WRITE). */
int smb_block_write(smb_dev_t *d, uint8_t addr, uint8_t cmd, const uint8_t *data, unsigned len);
/* SMBus block read: the device supplies the count. Stores at most cap bytes,
 * *count receives the count the device announced. */
int smb_block_read(smb_dev_t *d, uint8_t addr, uint8_t cmd, uint8_t *data, unsigned cap, unsigned *count);
/* I2C block read: write one offset byte, repeated start, read exactly len (1..32) bytes. */
int smb_i2c_block_read(smb_dev_t *d, uint8_t addr, uint8_t offset, uint8_t *data, unsigned len);
/* Abort whatever the controller is doing and clear its status. */
void smb_reset(smb_dev_t *d);

/* ---- Generic I2C requests mapped onto SMBus transactions ---- */

#define SMB_BUS_SEND_MAX  (SMB_BLOCK_MAX + 1)   /* command byte + 32 data bytes */

typedef struct smb_bus {
    smb_dev_t *dev;
    uint8_t    addr;                    /* current 7-bit target address */
    int        smbus_block_read;        /* 0: 3..32-byte reads use the I2C block read command;
                                           1: they use SMBus block read, count byte first */
    uint8_t    pend[SMB_BUS_SEND_MAX];  /* a send() without stop, held for the recv() that follows */
    unsigned   pend_len;
    int        pend_valid;
} smb_bus_t;

void smb_bus_init(smb_bus_t *b, smb_dev_t *d);
int  smb_bus_set_addr(smb_bus_t *b, unsigned addr);
/* One complete transfer: optional write phase, optional read phase, then stop. */
int  smb_bus_xfer(smb_bus_t *b, const uint8_t *sbuf, unsigned slen, uint8_t *rbuf, unsigned rlen);
/* The two-call form used by I2C frameworks. A send without stop touches no
 * hardware: it is held and combined with the next recv into one transaction. */
int  smb_bus_send(smb_bus_t *b, const uint8_t *buf, unsigned len, int stop);
int  smb_bus_recv(smb_bus_t *b, uint8_t *buf, unsigned len, int stop);

#endif /* SMB_CORE_H */
