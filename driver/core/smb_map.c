/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mapping generic I2C requests onto SMBus transactions.
 *
 * An I2C framework thinks in raw bytes: "write these n bytes", "read n
 * bytes", "write, repeated start, read". The chipset SMBus controller cannot
 * put arbitrary bytes on the wire; it can only run a fixed set of
 * transaction shapes. This file picks, for each request, the one shape that
 * produces exactly the same bytes on the wire, and refuses the request
 * (SMB_E_UNSUPPORTED) when no shape does. It never substitutes a transfer
 * that merely looks similar.
 *
 *   write phase   read phase   SMBus transaction             needs
 *   -----------   ----------   ---------------------------   ----------------
 *   0 bytes       0 bytes      Quick (write)                 -
 *   1             0            Send Byte                     -
 *   2             0            Write Byte Data               -
 *   3             0            Write Word Data               -
 *   4..33         0            block write, no count byte    SMB_CAP_I2C_WRITE
 *   4..34 (*)     0            SMBus Block Write             SMB_CAP_BLOCK
 *   0             1            Receive Byte                  -
 *   1             1            Read Byte Data                -
 *   1             2            Read Word Data                -
 *   1             3..32        I2C Block Read                SMB_CAP_I2C_READ
 *   1             2..33 (**)   SMBus Block Read              SMB_CAP_BLOCK
 *   anything else              refused
 *
 *  (*)  only when the controller inserts the count byte itself and the
 *       request's second byte equals the number of bytes after it, i.e. the
 *       request already is a well-formed SMBus block write.
 *  (**) only when the bus is switched to SMBus block reads
 *       (smbus_block_read = 1). The buffer receives the count byte first,
 *       then the data, exactly as a raw I2C master would see it.
 */
#include <stddef.h>
#include <string.h>
#include "smb_core.h"

void smb_bus_init(smb_bus_t *b, smb_dev_t *d)
{
    memset(b, 0, sizeof(*b));
    b->dev = d;
}

int smb_bus_set_addr(smb_bus_t *b, unsigned addr)
{
    if (addr > 0x7F) {
        return SMB_E_INVAL;                 /* 10-bit addressing does not exist on SMBus */
    }
    /*
     * I2C reserves 0x00-0x07 and 0x78-0x7F. 0x00 is the general call: every
     * chip on the bus takes the write. On QEMU a Send Byte to 0x00 moved the
     * register pointer of both sensors and of the EEPROMs. No client asking for
     * one chip means that, so refuse the whole reserved range.
     */
    if (addr < 0x08 || addr > 0x77) {
        return SMB_E_INVAL;
    }
    b->addr = (uint8_t)addr;
    b->pend_valid = 0;                      /* a held send belongs to the old target */
    return SMB_OK;
}

static int write_only(smb_bus_t *b, const uint8_t *s, unsigned n)
{
    smb_dev_t *d = b->dev;

    switch (n) {
    case 0: return smb_quick(d, b->addr, 0);
    case 1: return smb_send_byte(d, b->addr, s[0]);
    case 2: return smb_write_byte_data(d, b->addr, s[0], s[1]);
    case 3: return smb_write_word_data(d, b->addr, s[0], (uint16_t)(s[1] | (s[2] << 8)));
    default: break;
    }
    if (d->caps & SMB_CAP_I2C_WRITE) {
        /* Controller sends: address, s[0], then the data with no count byte. */
        if (n - 1 > SMB_BLOCK_MAX) {
            return SMB_E_UNSUPPORTED;
        }
        return smb_block_write(d, b->addr, s[0], s + 1, n - 1);
    }
    if ((d->caps & SMB_CAP_BLOCK) && n - 2 <= SMB_BLOCK_MAX && s[1] == n - 2) {
        /* Controller sends: address, s[0], count, data. The request already has that form. */
        return smb_block_write(d, b->addr, s[0], s + 2, n - 2);
    }
    return SMB_E_UNSUPPORTED;
}

static int write_then_read(smb_bus_t *b, uint8_t cmd, uint8_t *r, unsigned n)
{
    smb_dev_t *d = b->dev;
    uint16_t w;
    int rc;

    if (b->smbus_block_read && n >= 2 && n <= SMB_BLOCK_MAX + 1) {
        unsigned count = 0;

        if (!(d->caps & SMB_CAP_BLOCK)) {
            return SMB_E_UNSUPPORTED;
        }
        memset(r, 0, n);
        rc = smb_block_read(d, b->addr, cmd, r + 1, n - 1, &count);
        r[0] = (uint8_t)count;              /* tells the caller if its buffer was too short */
        return rc;
    }
    if (n == 1) {
        return smb_read_byte_data(d, b->addr, cmd, r);
    }
    if (n == 2) {
        rc = smb_read_word_data(d, b->addr, cmd, &w);
        if (rc == SMB_OK) {
            r[0] = (uint8_t)(w & 0xFF);
            r[1] = (uint8_t)(w >> 8);
        }
        return rc;
    }
    if (n <= SMB_BLOCK_MAX && (d->caps & SMB_CAP_I2C_READ)) {
        return smb_i2c_block_read(d, b->addr, cmd, r, n);
    }
    return SMB_E_UNSUPPORTED;
}

int smb_bus_xfer(smb_bus_t *b, const uint8_t *sbuf, unsigned slen, uint8_t *rbuf, unsigned rlen)
{
    if ((slen > 0 && sbuf == NULL) || (rlen > 0 && rbuf == NULL)) {
        return SMB_E_INVAL;
    }
    if (rlen == 0) {
        return write_only(b, sbuf, slen);
    }
    if (slen == 0) {
        /* A read with no register index: only the one-byte form exists on SMBus. */
        return (rlen == 1) ? smb_receive_byte(b->dev, b->addr, rbuf) : SMB_E_UNSUPPORTED;
    }
    if (slen == 1) {
        return write_then_read(b, sbuf[0], rbuf, rlen);
    }
    return SMB_E_UNSUPPORTED;               /* e.g. two index bytes, then a read */
}

int smb_bus_send(smb_bus_t *b, const uint8_t *buf, unsigned len, int stop)
{
    if (len > 0 && buf == NULL) {
        return SMB_E_INVAL;
    }
    if (stop) {
        b->pend_valid = 0;
        return smb_bus_xfer(b, buf, len, NULL, 0);
    }
    /*
     * No stop: the caller will follow with a recv(). The controller cannot
     * hold the bus between two commands, so nothing is sent yet. If the
     * recv() never comes, the bus has seen nothing and nothing is lost.
     */
    if (len > sizeof(b->pend)) {
        return SMB_E_UNSUPPORTED;
    }
    if (len > 0) {
        memcpy(b->pend, buf, len);
    }
    b->pend_len = len;
    b->pend_valid = 1;
    return SMB_OK;
}

int smb_bus_recv(smb_bus_t *b, uint8_t *buf, unsigned len, int stop)
{
    unsigned slen = 0;

    if (!stop) {
        return SMB_E_UNSUPPORTED;           /* cannot leave the bus claimed after a read */
    }
    if (b->pend_valid) {
        slen = b->pend_len;
        b->pend_valid = 0;
    }
    return smb_bus_xfer(b, b->pend, slen, buf, len);
}
