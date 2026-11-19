/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <devctl.h>
#include <errno.h>
#include <string.h>
#include <hw/i2c.h>
#include "i2cdev.h"

static void slave(i2c_addr_t *s, unsigned addr)
{
    s->addr = addr;
    s->fmt = I2C_ADDRFMT_7BIT;
}

int i2cdev_write(int fd, unsigned addr, const uint8_t *data, unsigned n)
{
    struct { i2c_send_t hdr; uint8_t buf[I2CDEV_MAX]; } m;

    if (n > I2CDEV_MAX) {
        return EINVAL;
    }
    memset(&m.hdr, 0, sizeof(m.hdr));
    slave(&m.hdr.slave, addr);
    m.hdr.len = n;
    m.hdr.stop = 1;
    memcpy(m.buf, data, n);
    return devctl(fd, DCMD_I2C_SEND, &m, sizeof(m.hdr) + n, NULL);
}

int i2cdev_read(int fd, unsigned addr, uint8_t *data, unsigned n)
{
    struct { i2c_recv_t hdr; uint8_t buf[I2CDEV_MAX]; } m;
    int rc;

    if (n > I2CDEV_MAX) {
        return EINVAL;
    }
    memset(&m.hdr, 0, sizeof(m.hdr));
    slave(&m.hdr.slave, addr);
    m.hdr.len = n;
    m.hdr.stop = 1;
    rc = devctl(fd, DCMD_I2C_RECV, &m, sizeof(m.hdr) + n, NULL);
    if (rc == EOK) {
        memcpy(data, m.buf, n);
    }
    return rc;
}

int i2cdev_write_read(int fd, unsigned addr, const uint8_t *out, unsigned on, uint8_t *in, unsigned inn)
{
    struct { i2c_sendrecv_t hdr; uint8_t buf[I2CDEV_MAX]; } m;
    int rc;

    if (on > I2CDEV_MAX || inn > I2CDEV_MAX) {
        return EINVAL;
    }
    memset(&m.hdr, 0, sizeof(m.hdr));
    slave(&m.hdr.slave, addr);
    m.hdr.send_len = on;
    m.hdr.recv_len = inn;
    m.hdr.stop = 1;
    memcpy(m.buf, out, on);
    rc = devctl(fd, DCMD_I2C_SENDRECV, &m, sizeof(m.hdr) + (on > inn ? on : inn), NULL);
    if (rc == EOK) {
        memcpy(in, m.buf, inn);
    }
    return rc;
}

const char *i2cdev_error(int fd, int rc)
{
    i2c_status_t st = 0;

    if (rc == EIO && devctl(fd, DCMD_I2C_STATUS, &st, sizeof(st), NULL) == EOK) {
        if (st & I2C_STATUS_NACK)  return "no acknowledge";
        if (st & I2C_STATUS_ARBL)  return "bus error";
        if (st & I2C_STATUS_BUSY)  return "timed out";
        if (st & I2C_STATUS_ABORT) return "aborted";
        return "request not supported by the controller";
    }
    return strerror(rc);
}

int i2cdev_lost(int rc)
{
    return rc == EBADF || rc == ESRCH || rc == ENOTCONN || rc == ECONNRESET;
}
