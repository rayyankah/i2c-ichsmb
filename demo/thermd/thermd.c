/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * thermd: a small real-time temperature monitor, written against the
 * standard QNX I2C interface (/dev/i2cN) only.
 *
 * A periodic timer delivers a pulse every period. On each pulse thermd reads
 * every sensor and, when one crosses the threshold, turns the "fan" on by
 * writing a byte to a chip register (an EEPROM byte on the bench). It runs at
 * a fixed SCHED_FIFO priority and records, for every period:
 *
 *   lateness  how long after the period started thermd woke up
 *   response  how long after the period started the work was done
 *
 * A period whose work isn't done within the deadline, or that passes with no
 * wake-up at all, counts as a missed deadline. The report at the end gives
 * minimum, median, 99th percentile and maximum of both.
 *
 * If the driver goes away (killed, restarted by smb-supervisor), thermd
 * reopens the device and logs how long it had no data.
 *
 * Usage text: thermd.use. Times on the bench are emulated (QEMU).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include "i2cdev.h"

#define MAX_SENSORS   4
#define MAX_RECORDS   (1u << 20)
#define MAX_GAPS      64
#define PULSE_TICK    (_PULSE_CODE_MINAVAIL + 0)

typedef struct {
    unsigned addr;
    int      hot;
    double   celsius;
    int      valid;
} sensor_t;

static const char *dev = "/dev/i2c0";
static sensor_t sensors[MAX_SENSORS];
static unsigned nsensors;
static unsigned period_us = 10000, deadline_us, prio = 30, seconds, quiet;
static double threshold = 80.0, hysteresis = 5.0;
static int fan_addr = 0x57, fan_reg = 0x00, fan_on;
static volatile sig_atomic_t stop;
static uint64_t start;                  /* when period 0 begins (CLOCK_MONOTONIC, ns) */

/* per-period records, in microseconds */
static uint32_t *late_rec, *resp_rec;
static unsigned nrec;

static unsigned periods, missed, skipped, failed_reads, fan_events;
static uint64_t worst_reaction;
static double gaps_ms[MAX_GAPS];
static unsigned ngaps;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: thermd [-d /dev/i2cN] [-s ADDR]... [-T period_ms] [-D deadline_ms]\n"
        "              [-H threshold_C] [-f ADDR:REG | -f none] [-P priority] [-t seconds] [-q]\n");
    exit(2);
}

static unsigned num(const char *s, unsigned max)
{
    char *end;
    unsigned long v = strtoul(s, &end, 0);

    if (*s == '\0' || *end != '\0' || v > max) {
        fprintf(stderr, "thermd: bad number '%s'\n", s);
        exit(2);
    }
    return (unsigned)v;
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return x < y ? -1 : x > y;
}

static void stats(const char *name, uint32_t *v, unsigned n)
{
    if (n == 0) {
        printf("%-18s %9s\n", name, "-");
        return;
    }
    qsort(v, n, sizeof(*v), cmp_u32);
    printf("%-18s %9u %9u %9u %9u\n", name, v[0], v[n / 2], v[(uint64_t)n * 99 / 100], v[n - 1]);
}

static void report(void)
{
    unsigned i, n = nrec;
    uint32_t late_max = 0, resp_max = 0, resp_p99 = 0, late_p99 = 0;

    printf("\nthermd report: %u periods of %u us (%.1f s), SCHED_FIFO priority %u, %u sensor(s)\n",
           periods, period_us, periods * (period_us / 1e6), prio, nsensors);
    printf("%-18s %9s %9s %9s %9s   (microseconds)\n", "", "min", "median", "99th %", "max");
    stats("wake-up lateness", late_rec, n);
    stats("response time", resp_rec, n);
    if (n) {
        late_max = late_rec[n - 1];
        late_p99 = late_rec[(uint64_t)n * 99 / 100];
        resp_max = resp_rec[n - 1];
        resp_p99 = resp_rec[(uint64_t)n * 99 / 100];
    }
    printf("deadline %u us: %u missed, of which %u periods had no wake-up at all\n", deadline_us, missed, skipped);
    printf("fan switched %u time(s), worst reaction %llu us after the period started\n",
           fan_events, (unsigned long long)worst_reaction);
    printf("failed reads %u, driver gaps %u", failed_reads, ngaps);
    for (i = 0; i < ngaps; i++) {
        printf("%s%.0f", i ? " " : " (ms without data:", gaps_ms[i]);
    }
    printf("%s\n", ngaps ? ")" : "");
    printf("RESULT periods=%u missed=%u skipped=%u late_p99_us=%u late_max_us=%u resp_p99_us=%u "
           "resp_max_us=%u failed_reads=%u gaps=%u prio=%u\n",
           periods, missed, skipped, late_p99, late_max, resp_p99, resp_max, failed_reads, ngaps, prio);
}

static void set_fan(int fd, int on, uint64_t release)
{
    uint8_t out[2] = { (uint8_t)fan_reg, (uint8_t)on };
    uint64_t t;
    int rc = fd == -1 ? ENOTCONN : EOK;

    if (fan_addr >= 0 && fd != -1) {
        rc = i2cdev_write(fd, (unsigned)fan_addr, out, 2);
    }
    t = now_ns() - release;
    fan_on = on;
    fan_events++;
    if (t / 1000 > worst_reaction) {
        worst_reaction = t / 1000;
    }
    printf("%10.3f s  fan %s, %llu us after the period started%s%s\n", (release - start) / 1e9, on ? "ON " : "off",
           (unsigned long long)(t / 1000), rc == EOK ? "" : ", fan write failed: ",
           rc == EOK ? "" : i2cdev_error(fd, rc));
}

int main(int argc, char *argv[])
{
    struct sched_param sp;
    struct sigevent ev;
    struct itimerspec its;
    struct _pulse pulse;
    timer_t timer;
    uint64_t next, end = 0, t0, last_good, lost_at = 0, last_print = 0;
    int chid, coid, fd, c, last_err = 0;
    unsigned i;
    char *colon;

    while ((c = getopt(argc, argv, "d:s:T:D:H:f:P:t:q")) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 's':
            if (nsensors == MAX_SENSORS) usage();
            sensors[nsensors++].addr = num(optarg, 0x7F);
            break;
        case 'T': period_us = num(optarg, 10000) * 1000; break;
        case 'D': deadline_us = num(optarg, 10000) * 1000; break;
        case 'H': threshold = atof(optarg); break;
        case 'f':
            if (strcmp(optarg, "none") == 0) {
                fan_addr = -1;
                break;
            }
            colon = strchr(optarg, ':');
            if (colon == NULL) usage();
            *colon = '\0';
            fan_addr = (int)num(optarg, 0x7F);
            fan_reg = (int)num(colon + 1, 0xFF);
            break;
        case 'P': prio = num(optarg, 255); break;
        case 't': seconds = num(optarg, 86400); break;
        case 'q': quiet = 1; break;
        default:  usage();
        }
    }
    if (optind != argc || period_us == 0 || prio == 0) {
        usage();
    }
    if (nsensors == 0) {
        sensors[nsensors++].addr = 0x48;
        sensors[nsensors++].addr = 0x49;
    }
    if (deadline_us == 0) {
        deadline_us = period_us;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    late_rec = malloc(MAX_RECORDS * sizeof(*late_rec));
    resp_rec = malloc(MAX_RECORDS * sizeof(*resp_rec));
    if (late_rec == NULL || resp_rec == NULL) {
        perror("thermd: malloc");
        return 1;
    }
    /* Touch every page now and lock it, so no page fault lands inside a period. */
    memset(late_rec, 0, MAX_RECORDS * sizeof(*late_rec));
    memset(resp_rec, 0, MAX_RECORDS * sizeof(*resp_rec));
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("thermd: mlockall (continuing)");
    }

    sp.sched_priority = (int)prio;
    c = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (c != EOK) {
        fprintf(stderr, "thermd: priority %u: %s\n", prio, strerror(c));
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = open(dev, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "thermd: %s: %s\n", dev, strerror(errno));
        return 1;
    }

    /* The timer sends a pulse at our own priority to a private channel. */
    chid = ChannelCreate(_NTO_CHF_PRIVATE);
    coid = chid == -1 ? -1 : ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("thermd: channel");
        return 1;
    }
    SIGEV_PULSE_INIT(&ev, coid, (int)prio, PULSE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("thermd: timer_create");
        return 1;
    }

    /* Periods start at absolute times start + k * period. */
    t0 = now_ns();
    start = (t0 / 1000000u + 20) * 1000000u;           /* a whole millisecond, 19 to 20 ms from now */
    next = start;
    if (seconds) {
        end = start + (uint64_t)seconds * 1000000000u;
    }
    its.it_value.tv_sec = (time_t)(start / 1000000000u);
    its.it_value.tv_nsec = (long)(start % 1000000000u);
    its.it_interval.tv_sec = period_us / 1000000u;
    its.it_interval.tv_nsec = (long)(period_us % 1000000u) * 1000;
    if (timer_settime(timer, TIMER_ABSTIME, &its, NULL) == -1) {
        perror("thermd: timer_settime");
        return 1;
    }
    printf("thermd: %u sensor(s) every %u us, deadline %u us, threshold %.1f C, priority %u FIFO%s\n",
           nsensors, period_us, deadline_us, threshold, prio, seconds ? "" : ", until stopped");
    last_good = start;

    while (!stop) {
        uint64_t now, done, behind;
        int any_hot = 0, ok = 1;

        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) == -1) {
            continue;                               /* EINTR from a signal: loop checks stop */
        }
        now = now_ns();
        if (now < next) {
            continue;                               /* an extra pulse queued while we were behind */
        }
        behind = (now - next) / (period_us * 1000u);   /* whole periods that passed with no wake-up */
        periods += (unsigned)behind + 1;
        missed += (unsigned)behind;
        skipped += (unsigned)behind;

        /* Reconnect if the driver went away. */
        if (fd == -1) {
            fd = open(dev, O_RDWR);
        }
        for (i = 0; i < nsensors && fd != -1; i++) {
            uint8_t reg = 0, in[2];
            int rc = i2cdev_write_read(fd, sensors[i].addr, &reg, 1, in, 2);

            if (rc == EOK) {
                sensors[i].celsius = (int16_t)((in[0] << 8) | in[1]) / 256.0;
                sensors[i].valid = 1;
                continue;
            }
            ok = 0;
            failed_reads++;
            if (i2cdev_lost(rc)) {
                close(fd);
                fd = -1;
                break;
            }
            if (rc != last_err) {
                printf("%10.3f s  0x%02x: %s\n", (next - start) / 1e9, sensors[i].addr, i2cdev_error(fd, rc));
            }
            last_err = rc;
        }
        if (fd == -1) {
            ok = 0;
            if (lost_at == 0) {
                lost_at = now;
                printf("%10.3f s  driver gone, reopening %s\n", (now - start) / 1e9, dev);
            }
        } else if (ok) {
            last_err = 0;
            if (lost_at != 0) {
                double gap = (now - last_good) / 1e6;

                if (ngaps < MAX_GAPS) gaps_ms[ngaps] = gap;
                ngaps++;
                printf("%10.3f s  driver back, %.0f ms without data\n", (now - start) / 1e9, gap);
                lost_at = 0;
            }
            last_good = now;
        }

        /* Threshold with hysteresis; the fan is on while any sensor is hot. */
        for (i = 0; i < nsensors; i++) {
            sensor_t *s = &sensors[i];

            if (!s->valid) {
                continue;
            }
            if (!s->hot && s->celsius >= threshold) {
                s->hot = 1;
                printf("%10.3f s  0x%02x at %.1f C, over %.1f C\n", (next - start) / 1e9, s->addr, s->celsius, threshold);
            } else if (s->hot && s->celsius < threshold - hysteresis) {
                s->hot = 0;
                printf("%10.3f s  0x%02x at %.1f C, back under %.1f C\n", (next - start) / 1e9, s->addr, s->celsius,
                       threshold - hysteresis);
            }
            any_hot |= s->hot;
        }
        if (any_hot != fan_on) {
            set_fan(fd, any_hot, next);
        }

        done = now_ns();
        if (done - next > deadline_us * 1000u) {
            missed++;
        }
        if (nrec < MAX_RECORDS) {
            late_rec[nrec] = (uint32_t)((now - next) / 1000u);
            resp_rec[nrec] = (uint32_t)((done - next) / 1000u);
            nrec++;
        }
        if (!quiet && done - last_print >= 1000000000u) {
            last_print = done;
            printf("%10.3f s ", (next - start) / 1e9);
            for (i = 0; i < nsensors; i++) {
                if (sensors[i].valid) {
                    printf(" 0x%02x %6.2f C", sensors[i].addr, sensors[i].celsius);
                } else {
                    printf(" 0x%02x     -- ", sensors[i].addr);
                }
            }
            printf("  fan %s\n", fan_on ? "ON" : "off");
        }
        next += (behind + 1) * period_us * 1000u;
        if (end && next >= end) {
            break;
        }
    }
    timer_delete(timer);
    report();
    return 0;
}
