/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * t_xfer: test client for /dev/i2cN. Runs raw I2C operations through the
 * standard devctls, in order, in one process, and prints one line per
 * operation for tests/run_tests.py to compare:
 *
 *   OK [hex bytes]
 *   ERR <errno name> [status bits]
 *
 * Operations (numbers in C syntax, hex data as one string, "-" for none):
 *   send:ADDR:HEX           write, then stop           (DCMD_I2C_SEND)
 *   sendns:ADDR:HEX         write, no stop             (DCMD_I2C_SEND, stop=0)
 *   recv:ADDR:N             read N bytes               (DCMD_I2C_RECV)
 *   sendrecv:ADDR:HEX:N     write, repeated start, read N   (DCMD_I2C_SENDRECV)
 *   send10:ADDR:HEX         write with a 10-bit address
 *   speed:HZ                DCMD_I2C_SET_BUS_SPEED
 *   reset                   DCMD_I2C_BUS_RESET
 *   info                    DCMD_I2C_DRIVER_INFO, prints caps
 *   soak:N:ADDR:HEX:LEN     N x sendrecv; every result must equal the first.
 *                           Prints "OK n=N us=<mean microseconds per transfer>"
 *   recover:N:BAD:ADDR:HEX:LEN
 *                           N x (recv 1 byte from BAD, which must fail, then
 *                           sendrecv on ADDR, which must succeed)
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <hw/i2c.h>

#define MAX_LEN 64

static int fd = -1;

/*
 * Each devctl takes its own header followed directly by the data, and the
 * headers differ in size (i2c_sendrecv_t is 4 bytes longer), so each call
 * builds its message in a plain buffer.
 */
typedef union {
    uint8_t  raw[sizeof(i2c_sendrecv_t) + MAX_LEN];
    uint64_t align;
} xmsg_t;

static unsigned num(const char *s)
{
    return (unsigned)strtoul(s, NULL, 0);
}

static unsigned parse_hex(const char *s, uint8_t *out)
{
    unsigned n = 0;

    if (strcmp(s, "-") == 0) {
        return 0;
    }
    while (s[0] != '\0' && s[1] != '\0' && n < MAX_LEN) {
        char b[3] = { s[0], s[1], 0 };

        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    return n;
}

static const char *errname(int e)
{
    switch (e) {
    case EIO:       return "EIO";
    case EINVAL:    return "EINVAL";
    case EMSGSIZE:  return "EMSGSIZE";
    case ENOTSUP:   return "ENOTSUP";
    case EBUSY:     return "EBUSY";
    case ETIMEDOUT: return "ETIMEDOUT";
    default:        return "E?";
    }
}

static void print_err(int rc)
{
    i2c_status_t st = 0;

    printf("ERR %s", errname(rc));
    if (rc == EIO && devctl(fd, DCMD_I2C_STATUS, &st, sizeof(st), NULL) == EOK) {
        if (st & I2C_STATUS_NACK)  printf(" NACK");
        if (st & I2C_STATUS_ARBL)  printf(" ARBL");
        if (st & I2C_STATUS_BUSY)  printf(" BUSY");
        if (st & I2C_STATUS_ABORT) printf(" ABORT");
        if (st & I2C_STATUS_ERROR) printf(" ERROR");
    }
    printf("\n");
}

static void print_ok(const uint8_t *b, unsigned n)
{
    unsigned i;

    printf("OK");
    for (i = 0; i < n; i++) {
        printf(" %02x", b[i]);
    }
    printf("\n");
}

static int do_send(unsigned addr, unsigned fmt, const uint8_t *d, unsigned n, unsigned stop)
{
    xmsg_t      m;
    i2c_send_t *hdr = (i2c_send_t *)m.raw;

    memset(&m, 0, sizeof(m));
    hdr->slave.addr = addr;
    hdr->slave.fmt = fmt;
    hdr->len = n;
    hdr->stop = stop;
    memcpy(m.raw + sizeof(*hdr), d, n);
    return devctl(fd, DCMD_I2C_SEND, &m, sizeof(*hdr) + n, NULL);
}

static int do_recv(unsigned addr, uint8_t *d, unsigned n)
{
    xmsg_t      m;
    i2c_recv_t *hdr = (i2c_recv_t *)m.raw;
    int         rc;

    memset(&m, 0, sizeof(m));
    hdr->slave.addr = addr;
    hdr->slave.fmt = I2C_ADDRFMT_7BIT;
    hdr->len = n;
    hdr->stop = 1;
    rc = devctl(fd, DCMD_I2C_RECV, &m, sizeof(*hdr) + n, NULL);
    if (rc == EOK) {
        memcpy(d, m.raw + sizeof(*hdr), n);
    }
    return rc;
}

static int do_sendrecv(unsigned addr, const uint8_t *o, unsigned on, uint8_t *in, unsigned inn)
{
    xmsg_t          m;
    i2c_sendrecv_t *hdr = (i2c_sendrecv_t *)m.raw;
    int             rc;

    memset(&m, 0, sizeof(m));
    hdr->slave.addr = addr;
    hdr->slave.fmt = I2C_ADDRFMT_7BIT;
    hdr->send_len = on;
    hdr->recv_len = inn;
    hdr->stop = 1;
    memcpy(m.raw + sizeof(*hdr), o, on);
    rc = devctl(fd, DCMD_I2C_SENDRECV, &m, sizeof(*hdr) + (on > inn ? on : inn), NULL);
    if (rc == EOK) {
        memcpy(in, m.raw + sizeof(*hdr), inn);
    }
    return rc;
}

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Split "a:b:c" in place into at most max fields. */
static int split(char *s, char **f, int max)
{
    int n = 0;

    while (n < max) {
        f[n++] = s;
        s = strchr(s, ':');
        if (s == NULL) {
            break;
        }
        *s++ = '\0';
    }
    return n;
}

static void run_op(char *op)
{
    char    *f[8];
    int      nf = split(op, f, 8), rc;
    uint8_t  out[MAX_LEN], in[MAX_LEN];
    unsigned n;

    if (strcmp(f[0], "send") == 0 && nf == 3) {
        n = parse_hex(f[2], out);
        rc = do_send(num(f[1]), I2C_ADDRFMT_7BIT, out, n, 1);
        rc == EOK ? print_ok(NULL, 0) : print_err(rc);
    } else if (strcmp(f[0], "sendns") == 0 && nf == 3) {
        n = parse_hex(f[2], out);
        rc = do_send(num(f[1]), I2C_ADDRFMT_7BIT, out, n, 0);
        rc == EOK ? print_ok(NULL, 0) : print_err(rc);
    } else if (strcmp(f[0], "send10") == 0 && nf == 3) {
        n = parse_hex(f[2], out);
        rc = do_send(num(f[1]), I2C_ADDRFMT_10BIT, out, n, 1);
        rc == EOK ? print_ok(NULL, 0) : print_err(rc);
    } else if (strcmp(f[0], "recv") == 0 && nf == 3) {
        n = num(f[2]);
        rc = (n <= MAX_LEN) ? do_recv(num(f[1]), in, n) : EINVAL;
        rc == EOK ? print_ok(in, n) : print_err(rc);
    } else if (strcmp(f[0], "sendrecv") == 0 && nf == 4) {
        unsigned on = parse_hex(f[2], out);

        n = num(f[3]);
        rc = (n <= MAX_LEN) ? do_sendrecv(num(f[1]), out, on, in, n) : EINVAL;
        rc == EOK ? print_ok(in, n) : print_err(rc);
    } else if (strcmp(f[0], "speed") == 0 && nf == 2) {
        unsigned hz = num(f[1]);

        rc = devctl(fd, DCMD_I2C_SET_BUS_SPEED, &hz, sizeof(hz), NULL);
        rc == EOK ? print_ok(NULL, 0) : print_err(rc);
    } else if (strcmp(f[0], "reset") == 0) {
        rc = devctl(fd, DCMD_I2C_BUS_RESET, NULL, 0, NULL);
        rc == EOK ? print_ok(NULL, 0) : print_err(rc);
    } else if (strcmp(f[0], "info") == 0) {
        i2c_driver_info_t info;

        memset(&info, 0, sizeof(info));
        rc = devctl(fd, DCMD_I2C_DRIVER_INFO, &info, sizeof(info), NULL);
        if (rc == EOK) {
            printf("OK speed=%x addr=%x caps=%x\n", (unsigned)info.speed_mode,
                   (unsigned)info.addr_mode, (unsigned)info.caps);
        } else {
            print_err(rc);
        }
    } else if (strcmp(f[0], "soak") == 0 && nf == 5) {
        unsigned count = num(f[1]), addr = num(f[2]), on, i, bad = 0;
        uint8_t  first[MAX_LEN];
        uint64_t t0;

        n = num(f[4]);
        on = parse_hex(f[3], out);
        if (n > MAX_LEN || do_sendrecv(addr, out, on, first, n) != EOK) {
            printf("ERR first transfer failed\n");
            return;
        }
        t0 = now_us();
        for (i = 0; i < count; i++) {
            if (do_sendrecv(addr, out, on, in, n) != EOK || memcmp(in, first, n) != 0) {
                bad++;
            }
        }
        if (bad == 0) {
            printf("OK n=%u us=%u\n", count, count ? (unsigned)((now_us() - t0) / count) : 0u);
        } else {
            printf("ERR n=%u failed=%u\n", count, bad);
        }
    } else if (strcmp(f[0], "recover") == 0 && nf == 6) {
        unsigned count = num(f[1]), badaddr = num(f[2]), addr = num(f[3]), i, wrong = 0;
        unsigned on = parse_hex(f[4], out);

        n = num(f[5]);
        for (i = 0; i < count && n <= MAX_LEN; i++) {
            if (do_recv(badaddr, in, 1) == EOK) {
                wrong++;                        /* the empty address must not answer */
            }
            if (do_sendrecv(addr, out, on, in, n) != EOK) {
                wrong++;                        /* and the next request must work */
            }
        }
        if (wrong == 0) {
            printf("OK n=%u\n", count);
        } else {
            printf("ERR n=%u wrong=%u\n", count, wrong);
        }
    } else {
        printf("ERR bad operation\n");
    }
}

int main(int argc, char *argv[])
{
    const char *dev = "/dev/i2c0";
    int i = 1;

    if (argc > 2 && strcmp(argv[1], "-d") == 0) {
        dev = argv[2];
        i = 3;
    }
    fd = open(dev, O_RDWR);
    if (fd == -1) {
        printf("ERR open %s: %s\n", dev, strerror(errno));
        return 1;
    }
    for (; i < argc; i++) {
        run_op(argv[i]);
    }
    return 0;
}
