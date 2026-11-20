# Where QNX is used

Every QNX-specific interface the code calls, where it is called, and how to see it working. Line numbers are for the tagged release; `python3 tests/check_evidence.py` checks that each one still points at the name next to it, and CI runs it on every push.

To see the things in the third column, start the bench (`bench/bench up`) and run the commands through `bench/bench ssh`. The test names are from `tests/cases/`, and their recorded output is in `docs/results/tests-q35.txt`.

## The driver (`driver/qnx/i2c-ichsmb.c`)

| QNX interface | Where | How to see it |
|---|---|---|
| I2C Driver Development Kit: the driver supplies a table of hardware functions, `libi2c-master` supplies `main()` and the resource manager | `i2c_master_getfuncs` (`driver/qnx/i2c-ichsmb.c:646`), table entries `I2C_ADD_FUNC` (`driver/qnx/i2c-ichsmb.c:648`); linked with `-li2c-master` (`driver/qnx/Makefile:7`) | `/dev/i2c0` appears when the driver starts: `ls /dev/i2c*` |
| DDK capability flag that makes the library pass write-then-read as one call | `I2C_CAPS_FORCE_STOP` (`driver/qnx/i2c-ichsmb.c:632`) in `ichsmb_driver_info()` | `smbctl info`; test "driver info: 7-bit, standard speed, force-stop" |
| DDK logging | `i2c_slogf` (`driver/qnx/i2c-ichsmb.c:117`) in `logmsg()` | `slog2info -b i2c_ichsmb` (by default only warnings and errors; start the driver with `-vv` to see everything) |
| slog2: own buffer set, made the default so the library's messages reach it | `slog2_register` (`driver/qnx/i2c-ichsmb.c:104`), `slog2_set_default_buffer` (`driver/qnx/i2c-ichsmb.c:105`) in `slog_setup()` | `slog2info -b i2c_ichsmb`; test "irq: driver logs to slog2 buffer i2c_ichsmb" |
| PCI server library: find the controller | `pci_device_find` (`driver/qnx/i2c-ichsmb.c:239`) in `locate()` | start the driver with `-vv`, then `slog2info -b i2c_ichsmb` shows "ICH9 SMBus at 0:1f.3, I/O 0x700, interrupt 16 (-I), status 0x00" |
| PCI server: read configuration space | `pci_device_cfg_rd16` (`driver/qnx/i2c-ichsmb.c:252`), `pci_device_cfg_rd8` (`driver/qnx/i2c-ichsmb.c:296`), `pci_device_cfg_rd32` (`driver/qnx/i2c-ichsmb.c:254`); errors through `pci_strerror` (`driver/qnx/i2c-ichsmb.c:284`) | same line, `-p 0:1f.3` |
| PCI server: claim the device, so only one driver can own it | `pci_device_attach` (`driver/qnx/i2c-ichsmb.c:282`) in `setup_ich()`, `pci_device_attach` (`driver/qnx/i2c-ichsmb.c:335`) in `setup_piix4()` | test "failure: a second copy of the driver refuses to start, the first keeps serving" |
| PCI server: I/O base address and interrupt | `pci_device_read_ba` (`driver/qnx/i2c-ichsmb.c:287`), `pci_device_read_irq` (`driver/qnx/i2c-ichsmb.c:312`) | start-up line shows "I/O 0x..."; with `-i`, test "irq: a wrong interrupt number is reported (PCI server's 10)" |
| PCI server: write configuration space (host enable, SMI off, I2C framing) | `pci_device_cfg_wr8` (`driver/qnx/i2c-ichsmb.c:301`) | test "smbus: SMBus block write and read (count byte)" (`-s` clears I2C framing) |
| PCI server: release on exit | `pci_device_detach` (`driver/qnx/i2c-ichsmb.c:551`) in `ichsmb_fini()` | the driver can be started again after a failed start |
| I/O privilege for port access | `ThreadCtl` (`driver/qnx/i2c-ichsmb.c:454`) with `_NTO_TCTL_IO_LEVEL`, `_NTO_IO_LEVEL_1` and `_NTO_TCTL_IO_LEVEL_INHERIT` | test "user: started without root it stops with a clear message" |
| Port I/O | `mmap_device_io` (`driver/qnx/i2c-ichsmb.c:471`), `in8` (`driver/qnx/i2c-ichsmb.c:127`), `out8` (`driver/qnx/i2c-ichsmb.c:128`) | every transfer |
| Private channel for interrupt pulses, keeping the client's priority | `ChannelCreate` (`driver/qnx/i2c-ichsmb.c:496`) with `_NTO_CHF_PRIVATE` and `_NTO_CHF_FIXED_PRIORITY`, `ConnectAttach` (`driver/qnx/i2c-ichsmb.c:497`) with `_NTO_SIDE_CHANNEL` | test "realtime: thermd at 30 wakes up in 99% of periods while hog spins at 20"; without the flag: `docs/results/thermd-inversion-before-fix.txt` |
| Interrupt delivered as a pulse | `SIGEV_PULSE_INIT` (`driver/qnx/i2c-ichsmb.c:499`), `InterruptAttachEvent` (`driver/qnx/i2c-ichsmb.c:500`) with `_NTO_INTR_FLAGS_TRK_MSK` | with `-I 16 -vv` the log shows "first interrupt received (interrupt 16)"; test "irq: interrupts arrive on -I 16 (logged once)" |
| Waiting for the pulse with a timeout | `TimerTimeout` (`driver/qnx/i2c-ichsmb.c:164`) with `_NTO_TIMEOUT_RECEIVE`, `MsgReceivePulse` (`driver/qnx/i2c-ichsmb.c:165`) in `q_wait()` | soak tests in interrupt mode, `docs/results/tests-q35.txt` |
| Collecting a pending pulse without blocking, then unmasking once per delivery | `TimerTimeout` (`driver/qnx/i2c-ichsmb.c:192`), `InterruptUnmask` (`driver/qnx/i2c-ichsmb.c:211`) in `q_irq_done()` | test "irq: transfers still work when the interrupt never comes" |
| Interrupt and channel cleanup | `InterruptDetach` (`driver/qnx/i2c-ichsmb.c:542`), `ConnectDetach` (`driver/qnx/i2c-ichsmb.c:545`), `ChannelDestroy` (`driver/qnx/i2c-ichsmb.c:548`) | |
| Process abilities: keep only what is needed, then lock everything else | `procmgr_ability` (`driver/qnx/i2c-ichsmb.c:364`), `procmgr_ability` (`driver/qnx/i2c-ichsmb.c:383`) in `drop_privileges()` | `-U qnxuser`; test "user: driver runs as uid 1000 after -U qnxuser" (`pidin -p i2c-ichsmb user`) |
| Security policy library: switch user | `set_ids_from_arg` (`driver/qnx/i2c-ichsmb.c:379`); linked with `-lsecpol` (`driver/qnx/Makefile:7`) | same test |
| Usage message built into the binary | `usemsg` (`driver/qnx/Makefile:12`), text in `driver/qnx/i2c-ichsmb.use` | `use /tmp/i2c-ichsmb` |

## Clients (standard I2C devctls through the resource manager)

These use only the documented QNX I2C interface, so they work with any QNX I2C driver. Each `devctl()` is a message to the resource manager that `libi2c-master` runs inside the driver; the driver thread serving it runs at the client's priority.

| QNX interface | Where | How to see it |
|---|---|---|
| `DCMD_I2C_SEND` | `DCMD_I2C_SEND` (`tools/lib/i2cdev.c:28`) in `i2cdev_write()` | `smbctl set 0x50 0 0x41` |
| `DCMD_I2C_RECV` | `DCMD_I2C_RECV` (`tools/lib/i2cdev.c:43`) in `i2cdev_read()` | `smbctl recv 0x48` |
| `DCMD_I2C_SENDRECV` (write, repeated start, read) | `DCMD_I2C_SENDRECV` (`tools/lib/i2cdev.c:64`) in `i2cdev_write_read()` | `smbctl temp 0x48`, `smbctl dump 0x50` |
| `DCMD_I2C_STATUS` after a failed transfer (NACK, busy, ...) | `DCMD_I2C_STATUS` (`tools/lib/i2cdev.c:75`) in `i2cdev_error()` | `smbctl get 0x30 0` prints "no acknowledge" |
| `DCMD_I2C_DRIVER_INFO` | `DCMD_I2C_DRIVER_INFO` (`tools/smbctl/smbctl.c:298`) in `cmd_info()` | `smbctl info` |
| `DCMD_I2C_SET_BUS_SPEED`, `DCMD_I2C_BUS_RESET` | `DCMD_I2C_SET_BUS_SPEED` (`tests/target/t_xfer.c:219`), `DCMD_I2C_BUS_RESET` (`tests/target/t_xfer.c:222`) | tests "bus speed 400 kHz refused at the next transfer", "bus reset, then a normal read" |
| QNX's own published I2C sample client, unchanged, against this driver | `tests/third_party/fetch_and_run.sh` | `docs/results/qnx-sample.txt` (6 of 6) |

## Real-time demo (`demo/`)

| QNX interface | Where | How to see it |
|---|---|---|
| Periodic timer delivering a pulse to a private channel | `ChannelCreate` (`demo/thermd/thermd.c:249`), `SIGEV_PULSE_INIT` (`demo/thermd/thermd.c:255`), `timer_create` (`demo/thermd/thermd.c:256`), `timer_settime` (`demo/thermd/thermd.c:272`) with `TIMER_ABSTIME`, `MsgReceivePulse` (`demo/thermd/thermd.c:284`) | `/tmp/thermd -t 5` prints a lateness and response-time report |
| SCHED_FIFO priority and locked memory | `mlockall` (`demo/thermd/thermd.c:229`), `pthread_setschedparam` (`demo/thermd/thermd.c:234`) | `docs/results/thermd-*.txt`; tests "realtime: ..." |
| Load generator pinned to each processor | `ThreadCtl` (`demo/hog/hog.c:43`) with `_NTO_TCTL_RUNMASK`, `_syspage_ptr` (`demo/hog/hog.c:53`) for the processor count, `pthread_setschedparam` (`demo/hog/hog.c:78`) | `/tmp/hog -t 5` while `thermd` runs |
| Death notice for a server, with no polling | `ChannelCreate` (`demo/supervisor/smb-supervisor.c:117`) with `_NTO_CHF_COID_DISCONNECT`, `MsgReceivePulse` (`demo/supervisor/smb-supervisor.c:148`), `_PULSE_CODE_COIDDEATH` (`demo/supervisor/smb-supervisor.c:153`) | `bench/recovery.sh`; `docs/results/recovery.txt` (20 kills, 20 restarts) |
| Starting the driver again | `posix_spawn` (`demo/supervisor/smb-supervisor.c:67`) in `start_driver()` | same |

## Build and image

| QNX tool | Where | How to see it |
|---|---|---|
| `qcc -Vgcc_ntox86_64` from QNX SDP 8.0 | `qcc` (`driver/qnx/Makefile:5`), `qcc` (`tools/smbctl/Makefile:5`), `qcc` (`demo/Makefile:5`) | `make qnx` |
| `mkqnximage --type=qemu` builds the QNX OS 8.0 image the bench boots | `mkqnximage` (`bench/bench:58`) | first `bench/bench up` |
| QNX target utilities used by the tests: `slog2info`, `pidin`, `slay`, `use` | `tests/cases/*.json` | `make test` |
