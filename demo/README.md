# Demo programs

Three small QNX programs that show what the driver is for. `thermd` uses only the standard I2C interface (`/dev/i2c0` and the `DCMD_I2C_*` devctls) and would run unchanged on any QNX I2C driver. Build them with `make qnx` from the top of the repository; `bench/bench up` builds and copies them to the guest's `/tmp`.

## thermd

A temperature monitor with a deadline. A timer created with `timer_create()` sends a pulse to a private channel every 10 ms. On each pulse thermd reads every sensor and, when one reaches the threshold (80 C by default, back to normal 5 C below), it turns the "fan" on by writing 1 to a register. On the bench that register is byte 0 of the EEPROM at 0x57, so `smbctl get 0x57 0` shows the fan state.

thermd runs at a fixed SCHED_FIFO priority (30 by default) with its memory locked. For every period it records:

- wake-up lateness: how long after the period started it woke up;
- response time: how long after the period started its work was done.

A period whose work isn't done within the deadline (the period, by default), or that passes with no wake-up at all, is a missed deadline. At the end it prints minimum, median, 99th percentile and maximum of both, and a RESULT line for scripts. If the driver dies, thermd reopens `/dev/i2c0` and logs how long it had no data. Options: `use thermd`.

## hog

Spins one thread per processor, each pinned to its own one, at a fixed SCHED_FIFO priority (20 by default) for a set time (10 s by default, at most 600). While it runs, nothing at its priority or below gets a processor, the shell and sshd included, which is why it insists on a time limit.

## smb-supervisor

Starts the driver and starts it again when it dies:

```
smb-supervisor -- /tmp/i2c-ichsmb -I 16
```

It doesn't poll and doesn't need to be the driver's parent (libi2c-master detaches the driver with `procmgr_daemon()`). It keeps `/dev/i2c0` open from a process that has a channel created with `_NTO_CHF_COID_DISCONNECT`. When the driver's channel goes away, for any reason including `kill -9`, the kernel sends that channel a `_PULSE_CODE_COIDDEATH` pulse naming the connection. The driver's exit status at start-up still tells it whether `init()` worked; if not, it retries after 1 s, doubling to 30 s.

## The story the demo tells

On the bench (`bench/measure.sh`, results in `docs/RESULTS.md`):

1. thermd at priority 30 with nothing else running keeps every deadline.
2. With hog spinning at 20 on both processors, thermd at 30 still keeps all but a handful in 6000 (10 in the recorded run), and the fan comes on within the period that sees the over-temperature. The handful most likely comes from the emulator: thermd misses a couple with no load in QNX at all when only the host is busy (`docs/RESULTS.md`). QNX gives the processor to the highest-priority ready thread, and the driver serving thermd's request runs at thermd's priority because message passing carries it over.
3. The same run with thermd at 10, below hog, loses every period while hog runs and never sees the over-temperature.
4. Before a one-flag fix in the driver, run 2 failed like run 3. The interrupt pulse dropped the driver thread to priority 10 in the middle of thermd's request (`docs/DESIGN.md`, Priority).
5. `kill -9` of the driver under smb-supervisor: thermd logs a gap of a couple of periods and carries on (`bench/recovery.sh`).

All times are measured inside QEMU and are emulated; see `docs/LIMITS.md`.
