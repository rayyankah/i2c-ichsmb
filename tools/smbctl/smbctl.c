/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * smbctl: talk to chips on an I2C bus through the standard QNX I2C
 * resource manager (/dev/i2cN). Works with any QNX I2C master driver;
 * nothing here is specific to i2c-ichsmb.
 *
 *   smbctl [-d /dev/i2c0] scan
 *   smbctl [-d dev] read  ADDR REG [COUNT]     write REG, repeated start, read COUNT (default 1)
 *   smbctl [-d dev] write ADDR REG BYTE...     write REG followed by the bytes
 *   smbctl [-d dev] recv  ADDR [COUNT]         plain read, no register byte
 *   smbctl [-d dev] temp  ADDR                 read an LM75/TMP105-style sensor (register 0)
 *   smbctl [-d dev] info
 *
 * Numbers accept C syntax (0x48, 72). Exit status is 0 on success, 1 when the
 * driver reports an error, 2 on bad usage.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <hw/i2c.h>

#define MAX_LEN 64

static int fd = -1;

static unsigned num(const char *s, unsigned max)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);

    if (*s == '\0' || *end != '\0' || v > max) {
        fprintf(stderr, "smbctl: bad number '%s' (max %u)\n", s, max);
        exit(2);
    }
    return (unsigned)v;
}

static const char *status_text(i2c_status_t st)
{
    if (st & I2C_STATUS_NACK)  return "no acknowledge";
    if (st & I2C_STATUS_ARBL)  return "bus error";
    if (st & I2C_STATUS_BUSY)  return "timed out";
    if (st & I2C_STATUS_ABORT) return "aborted";
    return "error";
}

/* devctl() returns an errno value. On EIO the driver's status says why. */
static int report(int rc, const char *what)
{
    i2c_status_t st = 0;

    if (rc == EOK) {
        return 0;
    }
    if (rc == EIO && devctl(fd, DCMD_I2C_STATUS, &st, sizeof(st), NULL) == EOK) {
        fprintf(stderr, "smbctl: %s: %s\n", what, status_text(st));
    } else {
        fprintf(stderr, "smbctl: %s: %s\n", what, strerror(rc));
    }
    return 1;
}

static int do_send(unsigned addr, const uint8_t *data, unsigned len)
{
    struct { i2c_send_t hdr; uint8_t buf[MAX_LEN]; } msg;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = addr;
    msg.hdr.slave.fmt = I2C_ADDRFMT_7BIT;
    msg.hdr.len = len;
    msg.hdr.stop = 1;
    memcpy(msg.buf, data, len);
    return devctl(fd, DCMD_I2C_SEND, &msg, sizeof(msg.hdr) + len, NULL);
}

static int do_recv(unsigned addr, uint8_t *data, unsigned len)
{
    struct { i2c_recv_t hdr; uint8_t buf[MAX_LEN]; } msg;
    int rc;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = addr;
    msg.hdr.slave.fmt = I2C_ADDRFMT_7BIT;
    msg.hdr.len = len;
    msg.hdr.stop = 1;
    rc = devctl(fd, DCMD_I2C_RECV, &msg, sizeof(msg.hdr) + len, NULL);
    if (rc == EOK) {
        memcpy(data, msg.buf, len);
    }
    return rc;
}

static int do_sendrecv(unsigned addr, const uint8_t *out, unsigned olen, uint8_t *in, unsigned ilen)
{
    struct { i2c_sendrecv_t hdr; uint8_t buf[MAX_LEN]; } msg;
    int rc;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = addr;
    msg.hdr.slave.fmt = I2C_ADDRFMT_7BIT;
    msg.hdr.send_len = olen;
    msg.hdr.recv_len = ilen;
    msg.hdr.stop = 1;
    memcpy(msg.buf, out, olen);
    rc = devctl(fd, DCMD_I2C_SENDRECV, &msg, sizeof(msg.hdr) + (olen > ilen ? olen : ilen), NULL);
    if (rc == EOK) {
        memcpy(in, msg.buf, ilen);
    }
    return rc;
}

static void hexdump(const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        printf("%02x%c", b[i], (i % 16 == 15 || i + 1 == n) ? '\n' : ' ');
    }
}

static int cmd_scan(void)
{
    unsigned a, found = 0;
    uint8_t b;

    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (a = 0; a < 0x80; a++) {
        if (a % 16 == 0) {
            printf("%02x:", a);
        }
        if (a < 0x08 || a > 0x77) {
            printf("   ");
        } else if (do_recv(a, &b, 1) == EOK) {      /* Receive Byte: safe for EEPROMs and sensors */
            printf(" %02x", a);
            found++;
        } else {
            printf(" --");
        }
        if (a % 16 == 15) {
            printf("\n");
        }
    }
    printf("%u device(s)\n", found);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: smbctl [-d /dev/i2cN] scan | info\n"
        "       smbctl [-d dev] read ADDR REG [COUNT]\n"
        "       smbctl [-d dev] write ADDR REG BYTE...\n"
        "       smbctl [-d dev] recv ADDR [COUNT]\n"
        "       smbctl [-d dev] temp ADDR\n");
    exit(2);
}

int main(int argc, char *argv[])
{
    const char *dev = "/dev/i2c0";
    uint8_t out[MAX_LEN], in[MAX_LEN];
    unsigned addr, n, i;
    int c;

    while ((c = getopt(argc, argv, "d:")) != -1) {
        if (c == 'd') dev = optarg; else usage();
    }
    argc -= optind;
    argv += optind;
    if (argc < 1) {
        usage();
    }
    fd = open(dev, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "smbctl: %s: %s\n", dev, strerror(errno));
        return 1;
    }

    if (strcmp(argv[0], "scan") == 0) {
        return cmd_scan();
    }
    if (strcmp(argv[0], "info") == 0) {
        i2c_driver_info_t info;

        memset(&info, 0, sizeof(info));
        if (report(devctl(fd, DCMD_I2C_DRIVER_INFO, &info, sizeof(info), NULL), "info")) return 1;
        printf("speed_mode 0x%x addr_mode 0x%x caps 0x%x%s\n", (unsigned)info.speed_mode,
               (unsigned)info.addr_mode, (unsigned)info.caps,
               (info.caps & I2C_CAPS_FORCE_STOP) ? " (force stop: combined transfers via xfer)" : "");
        return 0;
    }
    if (argc < 2) {
        usage();
    }
    addr = num(argv[1], 0x7F);

    if (strcmp(argv[0], "read") == 0 && argc >= 3) {
        out[0] = (uint8_t)num(argv[2], 0xFF);
        n = argc >= 4 ? num(argv[3], MAX_LEN) : 1;
        if (n == 0) usage();
        if (report(do_sendrecv(addr, out, 1, in, n), "read")) return 1;
        hexdump(in, n);
        return 0;
    }
    if (strcmp(argv[0], "write") == 0 && argc >= 3) {
        n = (unsigned)argc - 2;
        if (n > MAX_LEN) usage();
        for (i = 0; i < n; i++) out[i] = (uint8_t)num(argv[2 + i], 0xFF);
        return report(do_send(addr, out, n), "write");
    }
    if (strcmp(argv[0], "recv") == 0) {
        n = argc >= 3 ? num(argv[2], MAX_LEN) : 1;
        if (n == 0) usage();
        if (report(do_recv(addr, in, n), "recv")) return 1;
        hexdump(in, n);
        return 0;
    }
    if (strcmp(argv[0], "temp") == 0) {
        int16_t raw;

        out[0] = 0;
        if (report(do_sendrecv(addr, out, 1, in, 2), "temp")) return 1;
        raw = (int16_t)((in[0] << 8) | in[1]);          /* MSB first, 1/256 degree steps */
        printf("%.4f C (raw 0x%02x%02x)\n", raw / 256.0, in[0], in[1]);
        return 0;
    }
    usage();
    return 2;
}
