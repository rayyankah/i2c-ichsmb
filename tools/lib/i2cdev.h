/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thin client helpers for the standard QNX I2C interface (/dev/i2cN and
 * the DCMD_I2C_* devctls). Shared by smbctl and thermd. Nothing here knows
 * about i2c-ichsmb; any QNX I2C master driver will do.
 *
 * The transfer functions return an errno value (EOK on success), the same
 * way devctl() does.
 */
#ifndef I2CDEV_H
#define I2CDEV_H

#include <stdint.h>

#define I2CDEV_MAX 64           /* largest transfer the helpers handle */

/* write n bytes, with stop */
int i2cdev_write(int fd, unsigned addr, const uint8_t *data, unsigned n);

/* read n bytes, no register byte */
int i2cdev_read(int fd, unsigned addr, uint8_t *data, unsigned n);

/* write out[0..on), repeated start, read in[0..in) */
int i2cdev_write_read(int fd, unsigned addr, const uint8_t *out, unsigned on, uint8_t *in, unsigned inn);

/*
 * Text for a failed transfer. For EIO it asks the driver why (no
 * acknowledge, bus error, timeout...); otherwise strerror().
 */
const char *i2cdev_error(int fd, int rc);

/*
 * True when rc means the connection to the driver is gone (the driver
 * died or was restarted) rather than that the transfer failed on the bus.
 * The caller should close the descriptor and open the device again.
 */
int i2cdev_lost(int rc);

#endif
