# i2c-ichsmb

An I2C master driver for QNX 8.0 that talks to the SMBus controller in Intel PC chipsets (ICH family, and PIIX4 for basic transfers). It plugs into the QNX I2C framework, so clients use `/dev/i2cN` as with any other I2C driver. Through it they reach what sits on a PC's SMBus: memory-module EEPROMs, temperature sensors and similar chips.

Work in progress for the QNX Global Hackathon 2026.

## Status

| Part | State |
|---|---|
| Transaction engine (`driver/core`) | Done. Passes the host tests and runs against QEMU's ICH9 SMBus controller. |
| QNX glue (`driver/qnx`) | Drafted from the QNX docs, not built yet. Lines marked `CHECK` still need comparing with the SDP headers. |
| Tools, demo, docs | Not started. |

Nothing has run on QNX or on real hardware yet.

## How it is built

The controller logic is plain C99 in `driver/core`, with no OS calls. It reaches the hardware through a few callbacks (read a register, write a register, wait, read the clock). Because of that, the same code runs in three places:

- inside the QNX driver, on real I/O ports;
- on a PC, against a software model of the controller (`tests/host`);
- on bare metal inside QEMU, against QEMU's own controller model (`tests/baremetal`). QEMU's model was written by other people from the same Intel datasheet, so this is an independent check. It also needs no QNX licence.

`driver/core/smb_map.c` turns generic I2C requests into SMBus transactions. The chipset controller can only run a fixed set of transaction shapes, so for each request the mapper picks the one that puts exactly the same bytes on the wire. If no shape fits, it refuses the request rather than sending something close. The table is at the top of the file.

## Running the tests

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

## Layout

```
driver/core/     portable engine and request mapping
driver/qnx/      QNX resource-manager glue (libi2c-master)
tests/host/      software controller model and unit tests
tests/baremetal/ UEFI test program for QEMU's q35 machine
bench/qemu/      QEMU build with I2C sensor models
```

## License

Apache-2.0. See `LICENSE`.
