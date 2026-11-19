/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * smbctl: talk to chips on an I2C bus through the standard QNX I2C
 * resource manager (/dev/i2cN). Works with any QNX I2C master driver;
 * nothing here is specific to i2c-ichsmb. Usage text: smbctl.use.
 *
 * Numbers accept C syntax (0x48, 72). Exit status: 0 success, 1 the
 * transfer or the device failed, 2 bad usage.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <hw/i2c.h>
#include "i2cdev.h"

static const char *dev = "/dev/i2c0";
static int fd = -1;
static int json;

static void usage(void)
{
    fprintf(stderr,
        "usage: smbctl [-d /dev/i2cN] [-j] COMMAND ...\n"
        "  scan                       list chips that answer (0x08..0x77)\n"
        "  get   ADDR REG             read one byte register\n"
        "  set   ADDR REG VALUE       write one byte register\n"
        "  getw  ADDR REG             read a 16-bit register, low byte first (SMBus word)\n"
        "  setw  ADDR REG VALUE       write a 16-bit register, low byte first\n"
        "  read  ADDR REG [COUNT]     read COUNT bytes from REG on\n"
        "  write ADDR REG BYTE...     write bytes from REG on\n"
        "  recv  ADDR [COUNT]         read with no register byte\n"
        "  dump  ADDR [LEN]           hex dump of registers 0..LEN-1 (default 256)\n"
        "  temp  ADDR                 LM75/TMP105-style temperature\n"
        "  watch ADDR REG MS [COUNT]  read REG every MS ms, print when it changes\n"
        "  info                       what the driver reports about itself\n");
    exit(2);
}

static unsigned num(const char *s, unsigned max)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    if (*s == '\0' || *s == '-' || *end != '\0' || errno || v > max) {
        fprintf(stderr, "smbctl: bad number '%s' (0 to %u)\n", s, max);
        exit(2);
    }
    return (unsigned)v;
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "smbctl: %s: %s\n", what, i2cdev_error(fd, rc));
    return 1;
}

static void open_dev(void)
{
    fd = open(dev, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "smbctl: %s: %s\n", dev, strerror(errno));
        exit(1);
    }
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void print_bytes(const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (json) {
            printf("%s%u", i ? "," : "", b[i]);
        } else {
            printf("%02x%c", b[i], (i % 16 == 15 || i + 1 == n) ? '\n' : ' ');
        }
    }
}

static int cmd_scan(void)
{
    uint8_t found[0x80], b;
    unsigned a, n = 0;

    /* Receive Byte, not Quick Write: a quick write can change state on some chips. */
    for (a = 0x08; a <= 0x77; a++) {
        if (i2cdev_read(fd, a, &b, 1) == EOK) {
            found[n++] = (uint8_t)a;
        }
    }
    if (json) {
        printf("{\"bus\":\"%s\",\"devices\":[", dev);
        print_bytes(found, n);
        printf("]}\n");
        return 0;
    }
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (a = 0; a < 0x80; a++) {
        unsigned i, hit = 0;

        if (a % 16 == 0) {
            printf("%02x:", a);
        }
        for (i = 0; i < n; i++) {
            hit |= found[i] == a;
        }
        if (a < 0x08 || a > 0x77) {
            printf("   ");
        } else {
            printf(hit ? " %02x" : " --", a);
        }
        if (a % 16 == 15) {
            printf("\n");
        }
    }
    printf("%u device(s)\n", n);
    return 0;
}

static int cmd_get(unsigned addr, unsigned reg, unsigned width)
{
    uint8_t r = (uint8_t)reg, in[2];
    unsigned v;
    int rc = i2cdev_write_read(fd, addr, &r, 1, in, width);

    if (rc != EOK) {
        return fail("get", rc);
    }
    v = width == 2 ? (unsigned)(in[0] | in[1] << 8) : in[0];
    if (json) {
        printf("{\"addr\":%u,\"reg\":%u,\"value\":%u}\n", addr, reg, v);
    } else {
        printf(width == 2 ? "0x%04x\n" : "0x%02x\n", v);
    }
    return 0;
}

static int cmd_set(unsigned addr, unsigned reg, unsigned v, unsigned width)
{
    uint8_t out[3] = { (uint8_t)reg, (uint8_t)v, (uint8_t)(v >> 8) };
    int rc = i2cdev_write(fd, addr, out, 1 + width);

    if (rc != EOK) {
        return fail("set", rc);
    }
    if (json) {
        printf("{\"ok\":true}\n");
    }
    return 0;
}

/*
 * Registers 0..len-1 in 32-byte I2C Block Reads. A controller that can't do
 * block reads (PIIX4) refuses them; then fall back to one byte at a time.
 */
static int cmd_dump(unsigned addr, unsigned len)
{
    uint8_t buf[256];
    unsigned off, i, chunk = 32;
    int rc;

    for (off = 0; off < len; off += chunk) {
        uint8_t r = (uint8_t)off;

        if (chunk > len - off) {
            chunk = len - off;
        }
        rc = chunk > 1 ? i2cdev_write_read(fd, addr, &r, 1, buf + off, chunk) : EIO;
        if (rc != EOK && !i2cdev_lost(rc)) {
            chunk = 1;          /* stay at one byte from here on */
            rc = i2cdev_write_read(fd, addr, &r, 1, buf + off, 1);
        }
        if (rc != EOK) {
            return fail("dump", rc);
        }
    }
    if (json) {
        printf("{\"addr\":%u,\"data\":[", addr);
        print_bytes(buf, len);
        printf("]}\n");
        return 0;
    }
    for (off = 0; off < len; off += 16) {
        printf("%02x: ", off);
        for (i = 0; i < 16; i++) {
            printf(off + i < len ? "%02x " : "   ", buf[off + i]);
        }
        printf(" ");
        for (i = 0; i < 16 && off + i < len; i++) {
            putchar(buf[off + i] >= 0x20 && buf[off + i] < 0x7f ? buf[off + i] : '.');
        }
        printf("\n");
    }
    return 0;
}

static int cmd_temp(unsigned addr)
{
    uint8_t r = 0, in[2];
    int16_t raw;
    int rc = i2cdev_write_read(fd, addr, &r, 1, in, 2);

    if (rc != EOK) {
        return fail("temp", rc);
    }
    raw = (int16_t)((in[0] << 8) | in[1]);      /* most significant byte first, 1/256 C steps */
    if (json) {
        printf("{\"addr\":%u,\"celsius\":%.4f,\"raw\":%u}\n", addr, raw / 256.0, (unsigned)(uint16_t)raw);
    } else {
        printf("%.4f C (raw 0x%02x%02x)\n", raw / 256.0, in[0], in[1]);
    }
    return 0;
}

/*
 * Print the register whenever it changes. If the driver goes away (killed,
 * restarted by smb-supervisor), reopen the device and carry on, and say how
 * long the gap was.
 */
static int cmd_watch(unsigned addr, unsigned reg, unsigned ms, unsigned count)
{
    struct timespec period = { ms / 1000, (ms % 1000) * 1000000L };
    double t0 = now_ms(), lost_at = 0;
    int last = -1, last_err = 0;
    unsigned n;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (n = 0; count == 0 || n < count; n++) {
        uint8_t r = (uint8_t)reg, v;
        int rc;

        if (n) {
            nanosleep(&period, NULL);
        }
        if (fd == -1) {
            fd = open(dev, O_RDWR);
            if (fd == -1) {
                continue;
            }
        }
        rc = i2cdev_write_read(fd, addr, &r, 1, &v, 1);
        if (i2cdev_lost(rc)) {
            close(fd);
            fd = -1;
            if (lost_at == 0) {
                lost_at = now_ms();
                printf(json ? "{\"t_ms\":%.0f,\"event\":\"lost\"}\n" : "%9.0f ms  driver gone, reopening\n",
                       lost_at - t0);
            }
            continue;
        }
        if (lost_at != 0 && rc == EOK) {
            double t = now_ms();
            printf(json ? "{\"t_ms\":%.0f,\"event\":\"back\",\"gap_ms\":%.0f}\n" : "%9.0f ms  back after %.0f ms\n",
                   t - t0, t - lost_at);
            lost_at = 0;
            last = -1;
        }
        if (rc != EOK) {
            if (rc != last_err) {
                fprintf(stderr, "smbctl: watch: %s\n", i2cdev_error(fd, rc));
            }
            last_err = rc;
            continue;
        }
        last_err = 0;
        if (v != last) {
            printf(json ? "{\"t_ms\":%.0f,\"value\":%u}\n" : "%9.0f ms  0x%02x  %u\n", now_ms() - t0, v, v);
            last = v;
        }
    }
    return 0;
}

static int cmd_info(void)
{
    i2c_driver_info_t info;
    int rc;

    memset(&info, 0, sizeof(info));
    rc = devctl(fd, DCMD_I2C_DRIVER_INFO, &info, sizeof(info), NULL);
    if (rc != EOK) {
        return fail("info", rc);
    }
    if (json) {
        printf("{\"speed_mode\":%u,\"addr_mode\":%u,\"caps\":%u}\n", (unsigned)info.speed_mode,
               (unsigned)info.addr_mode, (unsigned)info.caps);
        return 0;
    }
    printf("speed modes  0x%x%s\n", (unsigned)info.speed_mode,
           info.speed_mode == I2C_SPEED_STANDARD ? " (100 kHz only)" : "");
    printf("addressing   0x%x%s\n", (unsigned)info.addr_mode,
           info.addr_mode == I2C_ADDRFMT_7BIT ? " (7-bit)" : "");
    printf("caps         0x%x%s\n", (unsigned)info.caps,
           (info.caps & I2C_CAPS_FORCE_STOP) ? " (write-then-read goes out as one combined transfer)" : "");
    return 0;
}

int main(int argc, char *argv[])
{
    uint8_t out[I2CDEV_MAX], in[I2CDEV_MAX];
    const char *cmd;
    unsigned addr, n, i;
    int c, rc;

    while ((c = getopt(argc, argv, "d:j")) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'j': json = 1; break;
        default:  usage();
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 1) {
        usage();
    }
    cmd = argv[0];
    signal(SIGPIPE, SIG_IGN);

    if (strcmp(cmd, "scan") == 0 || strcmp(cmd, "info") == 0) {
        if (argc != 1) usage();
        open_dev();
        return cmd[0] == 's' ? cmd_scan() : cmd_info();
    }
    if (argc < 2) {
        usage();
    }
    addr = num(argv[1], 0x7F);

    if ((strcmp(cmd, "get") == 0 || strcmp(cmd, "getw") == 0) && argc == 3) {
        open_dev();
        return cmd_get(addr, num(argv[2], 0xFF), cmd[3] == 'w' ? 2 : 1);
    }
    if (strcmp(cmd, "set") == 0 && argc == 4) {
        open_dev();
        return cmd_set(addr, num(argv[2], 0xFF), num(argv[3], 0xFF), 1);
    }
    if (strcmp(cmd, "setw") == 0 && argc == 4) {
        open_dev();
        return cmd_set(addr, num(argv[2], 0xFF), num(argv[3], 0xFFFF), 2);
    }
    if (strcmp(cmd, "read") == 0 && (argc == 3 || argc == 4)) {
        out[0] = (uint8_t)num(argv[2], 0xFF);
        n = argc == 4 ? num(argv[3], I2CDEV_MAX) : 1;
        if (n == 0) usage();
        open_dev();
        rc = i2cdev_write_read(fd, addr, out, 1, in, n);
        if (rc != EOK) return fail("read", rc);
        if (json) printf("{\"addr\":%u,\"reg\":%u,\"data\":[", addr, out[0]);
        print_bytes(in, n);
        if (json) printf("]}\n");
        return 0;
    }
    if (strcmp(cmd, "write") == 0 && argc >= 3) {
        n = (unsigned)argc - 2;
        if (n > I2CDEV_MAX) usage();
        for (i = 0; i < n; i++) out[i] = (uint8_t)num(argv[2 + i], 0xFF);
        open_dev();
        rc = i2cdev_write(fd, addr, out, n);
        if (rc != EOK) return fail("write", rc);
        if (json) printf("{\"ok\":true}\n");
        return 0;
    }
    if (strcmp(cmd, "recv") == 0 && (argc == 2 || argc == 3)) {
        n = argc == 3 ? num(argv[2], I2CDEV_MAX) : 1;
        if (n == 0) usage();
        open_dev();
        rc = i2cdev_read(fd, addr, in, n);
        if (rc != EOK) return fail("recv", rc);
        if (json) printf("{\"addr\":%u,\"data\":[", addr);
        print_bytes(in, n);
        if (json) printf("]}\n");
        return 0;
    }
    if (strcmp(cmd, "dump") == 0 && (argc == 2 || argc == 3)) {
        n = argc == 3 ? num(argv[2], 256) : 256;
        if (n == 0) usage();
        open_dev();
        return cmd_dump(addr, n);
    }
    if (strcmp(cmd, "temp") == 0 && argc == 2) {
        open_dev();
        return cmd_temp(addr);
    }
    if (strcmp(cmd, "watch") == 0 && (argc == 4 || argc == 5)) {
        unsigned reg = num(argv[2], 0xFF), ms = num(argv[3], 3600000);

        n = argc == 5 ? num(argv[4], 0xFFFFFFFFu) : 0;
        if (ms == 0) usage();
        open_dev();
        return cmd_watch(addr, reg, ms, n);
    }
    usage();
    return 2;
}
