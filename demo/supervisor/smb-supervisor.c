/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * smb-supervisor: start an I2C driver and start it again whenever it dies.
 *
 *   smb-supervisor [-d /dev/i2c0] [-w ms] -- /path/to/i2c-ichsmb [driver options]
 *
 * How it notices a death: it keeps a connection open to the driver's device
 * (/dev/i2c0) on a process that has a channel created with
 * _NTO_CHF_COID_DISCONNECT. When the driver's channel goes away, for any
 * reason including kill -9, the kernel sends this process a
 * _PULSE_CODE_COIDDEATH pulse for that connection. No polling, and no need
 * to be the driver's parent: libi2c-master detaches the driver with
 * procmgr_daemon(), so its start-up exit status is all a parent gets.
 *
 * That exit status is still useful: 0 means init() succeeded and the driver
 * is serving; anything else means it failed to start, and the supervisor
 * backs off (1 s doubling to 30 s) before trying again.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/wait.h>

extern char **environ;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    time_t t = time(NULL);
    char stamp[32];
    va_list ap;

    strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&t));
    printf("%s smb-supervisor: ", stamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* Run the driver; returns its exit status (0 = it initialised and detached). */
static int start_driver(char **argv)
{
    pid_t pid;
    int st, rc;

    rc = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != EOK) {
        say("%s: %s", argv[0], strerror(rc));
        return -1;
    }
    if (waitpid(pid, &st, 0) == -1) {
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Open the device once it appears; -1 if it doesn't within wait_ms. */
static int open_when_ready(const char *dev, unsigned wait_ms)
{
    double until = now_ms() + wait_ms;
    struct timespec ten_ms = { 0, 10000000L };
    int fd;

    for (;;) {
        fd = open(dev, O_RDWR);
        if (fd != -1 || now_ms() >= until) {
            return fd;
        }
        nanosleep(&ten_ms, NULL);
    }
}

int main(int argc, char *argv[])
{
    const char *dev = "/dev/i2c0";
    unsigned wait_ms = 5000, backoff = 1, restarts = 0;
    struct _pulse pulse;
    int c, chid, fd, st;
    double died_at = 0;

    while ((c = getopt(argc, argv, "d:w:")) != -1) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'w': wait_ms = (unsigned)atoi(optarg); break;
        default:  goto usage;
        }
    }
    if (optind >= argc) {
usage:
        fprintf(stderr, "usage: smb-supervisor [-d /dev/i2c0] [-w ms] -- DRIVER [options]\n");
        return 2;
    }
    argv += optind;
    signal(SIGPIPE, SIG_IGN);

    chid = ChannelCreate(_NTO_CHF_COID_DISCONNECT | _NTO_CHF_PRIVATE);
    if (chid == -1) {
        perror("smb-supervisor: ChannelCreate");
        return 1;
    }

    for (;;) {
        /* Something else may already be serving the device (first start). */
        fd = open(dev, O_RDWR);
        if (fd == -1) {
            st = start_driver(argv);
            fd = st == 0 ? open_when_ready(dev, wait_ms) : -1;
            if (fd == -1) {
                say("%s did not start (status %d), retrying in %u s", argv[0], st, backoff);
                sleep(backoff);
                backoff = backoff < 30 ? backoff * 2 : 30;
                continue;
            }
            if (died_at != 0) {
                restarts++;
                say("%s serving again, %.0f ms after the driver died (restart %u)", dev, now_ms() - died_at, restarts);
            } else {
                say("%s started, watching %s", argv[0], dev);
            }
        } else {
            say("%s already served by a running driver, watching it", dev);
        }
        backoff = 1;

        /* Block until the kernel says this connection's server is gone. */
        for (;;) {
            if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) == -1) {
                if (errno == EINTR) continue;
                perror("smb-supervisor: MsgReceivePulse");
                return 1;
            }
            if (pulse.code == _PULSE_CODE_COIDDEATH && pulse.value.sival_int == fd) {
                break;
            }
        }
        died_at = now_ms();
        say("driver for %s died, restarting it", dev);
        close(fd);
    }
}
