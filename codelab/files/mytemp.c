/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mytemp: read a TMP105 (or LM75) temperature through any QNX I2C driver.
 * The example client of the codelab "Write and test a QNX 8.0 I2C driver
 * without hardware".
 *
 *   qcc -Vgcc_ntox86_64 -o mytemp mytemp.c
 *   mytemp [ADDR]          (default 0x48)
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hw/i2c.h>

int main(int argc, char *argv[])
{
    unsigned addr = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 0x48;
    struct {
        i2c_sendrecv_t hdr;         /* who to talk to, how many bytes each way */
        uint8_t        buf[2];      /* bytes to send, then the bytes received */
    } msg;
    int16_t raw;
    int fd, rc;

    fd = open("/dev/i2c0", O_RDWR);
    if (fd == -1) {
        perror("mytemp: /dev/i2c0");
        return 1;
    }

    /* Write the register number (0, temperature), repeated start, read 2 bytes. */
    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = addr;
    msg.hdr.slave.fmt = I2C_ADDRFMT_7BIT;
    msg.hdr.send_len = 1;
    msg.hdr.recv_len = 2;
    msg.hdr.stop = 1;
    msg.buf[0] = 0x00;

    rc = devctl(fd, DCMD_I2C_SENDRECV, &msg, sizeof(msg), NULL);
    if (rc != EOK) {
        i2c_status_t st = 0;

        /* EIO means the transfer failed on the bus; the driver says why. */
        if (rc == EIO && devctl(fd, DCMD_I2C_STATUS, &st, sizeof(st), NULL) == EOK
            && (st & I2C_STATUS_NACK)) {
            fprintf(stderr, "mytemp: no chip answers at 0x%02x\n", addr);
        } else {
            fprintf(stderr, "mytemp: 0x%02x: %s\n", addr, strerror(rc));
        }
        return 1;
    }

    /* Most significant byte first, two's complement, 1/256 C per step. */
    raw = (int16_t)((msg.buf[0] << 8) | msg.buf[1]);
    printf("0x%02x: %.2f C\n", addr, raw / 256.0);
    return 0;
}
