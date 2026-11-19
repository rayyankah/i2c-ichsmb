/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * hog: keep every processor busy at a fixed SCHED_FIFO priority for a set
 * time. One spinning thread per processor, each pinned to its own one with
 * a run mask. Anything at a lower priority gets no processor time until hog
 * finishes; anything at a higher priority runs as if hog weren't there.
 *
 *   hog [-p priority] [-t seconds] [-n threads]
 *
 * Defaults: priority 20, 10 seconds, one thread per processor. The time limit
 * is mandatory on purpose: at priority 20 hog starves the shell, sshd and
 * the network stack, so it has to stop by itself.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

static uint64_t end_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void *spin(void *arg)
{
    unsigned cpu = (unsigned)(uintptr_t)arg;
    volatile uint64_t loops = 0;

    if (cpu < 32) {
        ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu));
    }
    while (now_ns() < end_ns) {
        loops++;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    unsigned prio = 20, seconds = 10, nthreads = _syspage_ptr->num_cpu, i;
    pthread_t tid[64];
    pthread_attr_t attr;
    struct sched_param sp;
    int c, rc;

    while ((c = getopt(argc, argv, "p:t:n:")) != -1) {
        switch (c) {
        case 'p': prio = (unsigned)atoi(optarg); break;
        case 't': seconds = (unsigned)atoi(optarg); break;
        case 'n': nthreads = (unsigned)atoi(optarg); break;
        default:
            fprintf(stderr, "usage: hog [-p priority] [-t seconds (1..600)] [-n threads]\n");
            return 2;
        }
    }
    if (prio < 1 || prio > 254 || seconds < 1 || seconds > 600 || nthreads < 1 || nthreads > 64) {
        fprintf(stderr, "hog: priority 1..254, seconds 1..600, threads 1..64\n");
        return 2;
    }
    printf("hog: %u thread(s) at priority %u FIFO for %u s\n", nthreads, prio, seconds);
    fflush(stdout);

    /* The main thread waits above the spinners so it can always join them. */
    sp.sched_priority = (int)prio + 1;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    end_ns = now_ns() + (uint64_t)seconds * 1000000000u;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    sp.sched_priority = (int)prio;
    pthread_attr_setschedparam(&attr, &sp);
    for (i = 0; i < nthreads; i++) {
        rc = pthread_create(&tid[i], &attr, spin, (void *)(uintptr_t)(i % _syspage_ptr->num_cpu));
        if (rc != EOK) {
            fprintf(stderr, "hog: thread %u: %s\n", i, strerror(rc));
            nthreads = i;
            break;
        }
    }
    for (i = 0; i < nthreads; i++) {
        pthread_join(tid[i], NULL);
    }
    printf("hog: done\n");
    return 0;
}
