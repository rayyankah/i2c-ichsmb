# i2c-ichsmb

An I2C master driver for QNX 8.0 that talks to the SMBus controller in Intel PC chipsets (ICH family, and PIIX4 for basic transfers). It plugs into the QNX I2C framework, so clients use `/dev/i2cN` as with any other I2C driver. Through it they reach what sits on a PC's SMBus: memory-module EEPROMs, temperature sensors and similar chips.

Work in progress for the QNX Global Hackathon 2026.

## Status

| Part | State |
|---|---|
| Transaction engine (`driver/core`) | Done. Passes the host tests and runs against QEMU's ICH9 SMBus controller. |
| QNX driver (`driver/qnx`) | Runs on QNX 8.0 in QEMU on both ICH9 (q35) and PIIX4 (pc). Polling or interrupt mode, slog2 logging, can drop to a normal user after start-up. |
| On-target tests | All passing on q35 and pc; counts in `docs/RESULTS.md`. |
| `tools/smbctl` | Scan, byte and word registers, dumps, temperatures, watching a register, JSON output, through the standard I2C devctls. |
| Bench (`bench/bench`) | Boots QNX in QEMU with two sensors, deploys, sets temperatures from the host, runs the tests. |
| Demo (`demo/`) | `thermd` real-time monitor, `hog` load generator, `smb-supervisor` restarts a dead driver. |

Not tested on real hardware yet.

## How it is built

The controller logic is plain C99 in `driver/core`, with no OS calls. It reaches the hardware through a few callbacks (read a register, write a register, wait, read the clock). Because of that, the same code runs in three places:

- inside the QNX driver, on real I/O ports;
- on a PC, against a software model of the controller (`tests/host`);
- on bare metal inside QEMU, against QEMU's own controller model (`tests/baremetal`). QEMU's model was written by other people from the same Intel datasheet, so this is an independent check. It also needs no QNX licence.

`driver/core/smb_map.c` turns generic I2C requests into SMBus transactions. The chipset controller can only run a fixed set of transaction shapes, so for each request the mapper picks the one that puts exactly the same bytes on the wire. If no shape fits, it refuses the request rather than sending something close. The table is at the top of the file.

## Trying it on QNX

You need QNX SDP 8.0 (free non-commercial licence) installed under `~/qnx800` on Ubuntu or WSL2. The first `up` also builds QEMU with the I2C sensor models (about 10 minutes) and the QNX image (a few minutes).

```
bench/bench up                  # boot QNX in QEMU, deploy, start the driver, scan the bus
bench/bench ssh /tmp/smbctl temp 0x48
bench/bench heat 0x48 91        # from the host: the sensor now reads 91 C
bench/bench ssh /tmp/smbctl temp 0x48
bench/bench test                # the on-target tests, against the running guest
bench/bench down
```

The bench boots QEMU's q35 machine, which has the ICH9 SMBus controller, with TMP105 temperature sensors at 0x48 and 0x49 and eight EEPROMs at 0x50 to 0x57. Settings are in `bench/config.env`; `MACHINE=pc` gives the PIIX4 controller instead. The driver runs as `i2c-ichsmb -I 16` there: see `docs/DESIGN.md` for why the interrupt number has to be given.

The real-time demo and the recovery demo are described in `demo/README.md`. `bench/measure.sh` and `bench/recovery.sh` produce the numbers in `docs/RESULTS.md`.

## Running the tests

On-target tests (QNX kit, QEMU from `bench/qemu/build-qemu.sh`, an image made as above):

```
source ~/qnx800/qnxsdp-env.sh
make test        # q35, ICH9
make test-pc     # pc, PIIX4
```

The runner boots QNX, copies the binaries over, restarts the driver in each mode, sets the sensors through QMP and checks every result. A few failure cases reboot QEMU without a sensor. How the driver maps requests, handles errors, uses interrupts and drops privileges is in `docs/DESIGN.md`; what isn't covered is in `docs/LIMITS.md`.

Host tests (any C99 compiler):

```
cd tests/host
make
```

Bare-metal test in QEMU (Linux or WSL):

```
sudo apt install gcc-mingw-w64-x86-64 qemu-system-x86 ovmf
bash tests/baremetal/run.sh
```

Ubuntu's QEMU has no I2C temperature sensor model for x86. `bench/qemu/build-qemu.sh` builds one that does. With it, `tests/baremetal/sensor_qmp.sh` sets a sensor's temperature from the host over QMP and checks that the driver core reads the new value.

CI (`.github/workflows/ci.yml`) runs the host tests and the bare-metal test on every push.

## A QEMU bug found along the way

QEMU's SMBus controller model (`hw/i2c/pm_smbus.c`) never finishes a 32-byte block write in byte-by-byte mode. It wraps its buffer index to 0 before comparing it with the byte count, so it keeps asking for more bytes. Real hardware handles 32 bytes. The driver now stops with an error instead of waiting forever. Details are in `DEVLOG.md` (19 Nov).

## Two bugs found by the demo

Building the real-time demo turned up two problems that every earlier test had missed, because QEMU's controller never makes the driver wait:

- In interrupt mode the driver attached the interrupt number the PCI server reported (10), which never fires on q35 (it's 16). Transfers still worked, so nothing noticed. The driver now logs whether interrupts arrive, and `-I` sets the number.
- Once interrupts worked, a request from a priority-30 client finished at priority 10, because receiving the interrupt pulse sets the thread to the pulse's priority. Under load that froze a real-time client for seconds. One flag on the driver's channel fixed it.

Both are written up in `docs/DESIGN.md` with the measurements.

## Layout

```
driver/core/     portable engine and request mapping
driver/qnx/      QNX resource-manager glue (libi2c-master)
tests/host/      software controller model and unit tests
tests/baremetal/ UEFI test program for QEMU's q35 machine
tests/target/    on-target test client (t_xfer)
tests/cases/     on-target test cases, run by tests/run_tests.py
tools/smbctl/    command-line client for /dev/i2cN
tools/lib/       small client helpers shared by smbctl and thermd
demo/            thermd, hog, smb-supervisor
bench/           bench script, QMP control, scenarios, measurement scripts
bench/qemu/      QEMU build with I2C sensor models, QNX boot script
docs/            design notes, limits, results
```

## License

Apache-2.0. See `LICENSE`.
