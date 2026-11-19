# Dev log

Dated notes: what I tried, what I found, and the commands that worked.

## 14 Nov 2026

### Is there already a driver?

I searched Devpost projects for `QNX smbus` (nothing), `QNX i2c driver` (three unrelated projects) and `smbus driver` (two unrelated). I still need to check the QNX SDP package list once the kit is installed.

### QNX API notes

Read from the public QNX docs:

- I2C DDK: https://www.qnx.com/developers/docs/qnxeverywhere/com.qnx.doc.ddk/topic/i2c/driver_I2C.html, plus `driver_I2C_implementation.html` and `I2C_example.html` in the same folder.
- Function table: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.custom_bsp/topic/i2c/i2c_framework_Function_table.html. Besides send and recv it has `xfer` and `bus_reset`.
- PCI library: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.pci_server/topic/library.html
- `ThreadCtl`, `mmap_device_io`, `InterruptAttachEvent` and `procmgr_ability` in the 8.0 library reference.

These pages don't settle a few things that I'll check in the installed headers: `DCMD_I2C_SENDRECV` and how the library turns it into send/recv/xfer calls, the fields of `pci_ba_t`, the signatures of `pci_device_cfg_wr8`, `pci_device_read_irq` and `pci_device_detach`, the numeric `i2c_status_t` values, and whether error codes are OR-ed with `I2C_STATUS_DONE`.

### Design: a portable core

All the controller logic is in `driver/core`, with no OS calls. It reaches the hardware through five callbacks (`smb_io_t`). The same source can then be tested on a PC, run on bare metal in QEMU without a QNX licence, and built into the QNX driver.

### Design: block transfers go byte by byte

I don't use the controller's 32-byte buffer mode. The byte-done handshake works on every ICH generation. Controllers announce the last byte of a block read in one of two ways (BYTE_DONE and then INTR, or INTR alone), and the engine accepts both.

### Design: no block commands on PIIX4

The real PIIX4 moves block data through a FIFO, but QEMU's model uses the ICH handshake for both chips. I could only test PIIX4 block code against behaviour the real chip doesn't have, so PIIX4 gets quick, byte, byte data and word data only.

### Host tests

`tests/host` has a software model of the controller that records a wire-level trace. There are 31 cases (252 checks), and all pass:

```
cd tests/host
make            # Windows: mingw32-make
```

To check that the tests can fail, I broke the engine on purpose four ways: setting LAST_BYTE one byte early, leaving the KILL bit set, removing the final-byte-by-INTR path, and swapping the word byte order. Each one made between one and five cases fail.

### Bare-metal test and QNX glue

`tests/baremetal/smbtest.c` builds as a 17 KB UEFI program (`gcc ... -Wl,--subsystem,10 -Wl,-e,efi_main`). It hasn't run yet.

`driver/qnx/i2c-ichsmb.c` is drafted from the docs above but not compiled. Lines marked `CHECK` need comparing with the real headers.

## 19 Nov 2026

### First run against QEMU's controller

Ubuntu 26.04 in WSL2, QEMU 10.2.1 from the Ubuntu packages:

```
sudo apt install gcc-mingw-w64-x86-64 qemu-system-x86 ovmf build-essential
bash tests/baremetal/run.sh
```

1. `-device tmp105,address=0x48` fails because the Ubuntu x86 build has no `tmp105`. The only I2C models it has are `i2c-ddc`, `i2c-echo` and `smbus-ipmi`. The sensor part of the test was already optional, so `run.sh` now takes extra QEMU options from `QEMU_EXTRA`.
2. The eight memory-module EEPROMs on q35 (0x50 to 0x57) read as all zeros. A broken I2C block read would then still match the reference, so the test now compares against the non-zero bytes its own block writes leave at 0x90.
3. The test hung on the 32-byte SMBus block write. The cause is a bug in QEMU, described in the next section.
4. That hang showed a bug in my engine. When a byte event made progress, the loop went round again without checking the deadline. A controller that kept raising BYTE_DONE could therefore hold the driver forever. It is fixed two ways. More than `SMB_BLOCK_MAX + 1` byte events in one transaction now counts as a protocol error (kill, `SMB_E_PROTO`). The loop also checks the deadline after progress, not only while waiting.

Result: 30 checks, 0 failures, 220 transactions, no timeouts. The checks cover:

- bus scan, and NACK from an empty address;
- byte and word reads and writes;
- send byte and receive byte;
- SMBus block write and read for 1 to 31 bytes;
- I2C block read for 1 to 32 bytes;
- block write with I2C_EN set;
- send (no stop) followed by recv, through the mapping layer.

Two open questions from 14 Nov are now answered for QEMU (not for real hardware):

- QEMU announces the last byte of a block read with INTR alone, with the byte waiting in `SMB_BLOCK_DB`. That is the `CTL_LAST_BYTE` branch in `pm_smbus.c`.
- With INTREN set, bit 3 of the PCI status register reads 1 after completion, so QEMU raises the PCI interrupt (IRQ 10, pin A under OVMF). Interrupt mode should be testable on QNX under QEMU.

### QEMU bug: 32-byte block writes never finish

In `hw/i2c/pm_smbus.c` (v10.2.0, lines 265 to 269, and the same on master on 19 Nov), byte-by-byte mode increments `smb_index` after each BYTE_DONE clear. It wraps the index to 0 at `PM_SMBUS_MAX_MSG_SIZE` (32) before comparing it with the count in `smb_data0`. With a count of 32 the comparison never matches, so the model raises BYTE_DONE forever and never writes the block. Real ICH hardware accepts 32-byte block writes. The 32-byte buffer mode (`AUX_BLK`) doesn't have the problem. Comparing before wrapping would fix it. I plan to report it upstream.

On QEMU, a 32-byte write now ends with `SMB_E_PROTO` (-8), and the test expects that.

### Host tests for the hang

The software model got a `byte_storm` fault: it raises BYTE_DONE forever and never moves on, as QEMU does above. A new test case uses it. That makes 32 cases and 261 checks, all passing. With the event limit removed, the new case fails: it gets `SMB_E_TIMEOUT` after the full timeout instead of `SMB_E_PROTO` straight away. The deadline check alone still prevents the hang.

### Keeping firmware output off the terminal

OVMF writes terminal control codes to the serial port (clear screen, resize, mouse mode). Passed straight to a terminal, they change its settings; in my case the terminal started typing mouse reports. `run.sh` now writes serial output to `build/serial.raw` and prints `build/serial.log`, a copy with control codes and non-printable bytes stripped.

### QEMU with sensor models

`bench/qemu/build-qemu.sh` downloads QEMU 10.2.1 from qemu.org and checks the signature (key CEACC9E15534EBABB82D3FA03353C9CEF108B584). It then adds `CONFIG_I2C_DEVICES`, `TMP105`, `TMP421`, `EMC141X` and `AT24C` to the x86_64 device config and installs to `~/work/qemu`. The result lists `tmp105`, `tmp421`, `emc1413` and `at24c-eeprom` on `i2c-bus`.

My first build was interrupted and left truncated object files that broke the link. Deleting them fixed it.

### The host changes a sensor, the driver sees it

With `-device tmp105,address=0x48,id=temp0` on q35, the sensor shows up in the scan next to the EEPROMs, and 31 of 31 checks pass.

`tests/baremetal/sensor_qmp.sh` starts QEMU paused and uses QMP `qom-set` to set the `temperature` property of `/machine/peripheral/temp0` to 42500 (millidegrees). It then resumes the machine. The core reads the raw word `0x802A`, which is 42.5 C: the whole degrees come first on the wire, so they land in the low byte of the SMBus word.

## 19 Nov 2026, later: first run on QNX

### Kit

QNX SDP 8.0 installed through the QNX Software Center on Ubuntu 26.04 (WSL2). The base install already has everything the driver needs: `qcc` for x86_64, `<hw/i2c.h>`, `libi2c-master.a`, `<pci/pci.h>`, `libpci` and `mkqnximage`. No extra package was needed.

The installed kit has no `i2c-*` driver at all and nothing that drives an ICH or PIIX4 SMBus. "SMBus" only appears in the PCI device-name table.

### Headers against the draft

All the PCI calls and types I had guessed from the docs match `<pci/pci.h>`. Two things in `<hw/i2c.h>` changed the driver:

- `i2c_driver_info_t` has a `caps` field, and `I2C_CAPS_FORCE_STOP` describes this controller exactly: it can't hold the bus between two messages. The library refuses that flag unless the driver has `xfer()` ("xfer() is needed for I2C_CAPS_FORCE_STOP set" is one of its strings). The driver now sets the flag, and the library hands write-then-read requests over as one call.
- The library's `check_mstatus()` treats only an exact `I2C_STATUS_DONE` as success. For anything else it logs each set bit and fails the devctl with EIO. I read that from its disassembly, since the docs only say "returns one of". Errors are now returned as `DONE | cause`.

The driver builds with `-Wall -Wextra` and no warnings.

### Booting on q35

`mkqnximage --type=qemu` builds a working image, but its runner starts QEMU with no `-machine`, so the guest gets the default `pc` machine and the PIIX4 SMBus. It also wants a network bridge set up as root. `bench/qemu/run-qnx.sh` starts the same image on `q35` instead, with user-mode networking (SSH on 127.0.0.1:2222) and a tmp105 at 0x48.

One catch: the image uses `devb-eide`, and on q35 a plain `-drive if=ide` lands on the AHCI controller. Adding a PIIX4 IDE controller (`-device piix4-ide`) and putting the disk on it fixed that. QNX boots in about 6 seconds with KVM.

`pci-tool -v` in the guest shows `8086/2930 Intel 82801I (ICH9 Family) SMBus Controller` at B0:D31:F3, and nothing claims it.

### End to end

```
# /tmp/i2c-ichsmb -v
i2c-ichsmb: ICH9 SMBus at I/O 0x700, polling, status 0x00
```

`/dev/i2c0` appears. `tools/smbctl` (new) uses only the standard I2C devctls:

- `scan` finds 0x48 and 0x50 to 0x57, nothing else.
- `write 0x50 0x10 0xde 0xad 0xbe 0xef`, then `read 0x50 0x10 4`, gives `de ad be ef`. One- and two-byte reads of the same place agree.
- `recv 0x20` (empty address) fails with "no acknowledge", exit status 1.
- `temp 0x48` reads 0.0 C. After `qom-set` of 30250 millidegrees from the host it reads 30.0 C, raw 0x1e00. That's right: the TMP105 powers up in 9-bit mode, which has 0.5 C steps.

Interrupt mode (`-i`) works as well: `pidin irqs` shows the driver attached to IRQ 10 with a pulse event, and all of the above pass, including a 32-byte I2C block read.

One thing to fix: started from an SSH session with output going to the session, the driver keeps the session's output open after it turns into a daemon, so the SSH command never returns. For now I start it with output redirected. Logging should move to slog2.

## 19 Nov 2026, evening: complete driver (M2)

### What libi2c-master does around the driver

I disassembled parts of `libi2c-master.a` where the docs were silent:

- `_i2c_options()` takes `--u N` (unit, default 0) and `--b N` (speed, default 100000), removes them, and leaves everything else for the driver's `init()`. Single-dash options pass straight through.
- `main()` runs the options, then `init()`, then `procmgr_daemon(0, NOCLOSE | NODEVNULL)`. Standard input and output stay open, which is why starting the driver over SSH without redirection kept the session open. QNX's other I2C drivers behave the same way.
- `i2c_slogf(verbosity, level, ...)` only logs when `verbosity >= level`, and the library uses the verbosity from our `driver_info` to decide whether to log every failed transfer at error level.
- A requested bus speed is stored per open connection and applied at the next transfer. If the driver refuses it, that transfer fails, while `DCMD_I2C_SET_BUS_SPEED` itself returns OK.
- A zero-length send never reaches the driver, so an SMBus Quick command can't be issued through the devctls.

### Driver changes

- slog2 logging with levels (default warnings and errors, more with `-v`). Start-up errors also go to stderr.
- `-U user[:group]`: after start-up keep only PATHSPACE, PUBLIC_CHANNEL and (with `-i`) INTERRUPT, lock them, switch user with `set_ids_from_arg()`, then deny and lock everything else. I/O port access stays with the thread.
- A small hardware table (ICH9, PIIX4) and `-p bus:dev.func` for naming a controller. With `-p`, an untested Intel SMBus-class function is tried as ICH-compatible, with a warning.
- `set_bus_speed` refuses anything but 100 kHz. The chipset runs the SMBus at a fixed clock.
- `abort()` implemented, and usage text embedded with `usemsg` (`use i2c-ichsmb`).
- Private channel and `_NTO_INTR_FLAGS_TRK_MSK` for the interrupt. The lost-interrupt reasoning is in `docs/DESIGN.md`.

### On-target tests

`tests/target/t_xfer` runs raw devctl operations in order and prints one result line each. `tests/run_tests.py` boots the guest, copies the binaries, restarts the driver in each mode (polling, `-i`, `-s`, `-i -U qnxuser`), sets the sensor over QMP and compares every line with the case files in `tests/cases/`. Run with `make test` (q35) or `make test-pc` (pc).

My first version of `t_xfer` put the data after a union of the three message headers. `i2c_sendrecv_t` is 4 bytes longer than `i2c_send_t`, so plain sends had their data 4 bytes too late, and the driver dutifully wrote zeros. Each call now builds its own header-plus-data buffer.

Results, saved in `docs/results/`:

- q35 (ICH9): 46 of 46 pass.
- pc (PIIX4): 42 of 42 pass. The 4 q35-only cases need block transfers or `-s`. PIIX4 refuses block requests, as designed, and `-i` falls back to polling.

My first pc run had one failure, and the expectation was the bug: the 31-byte write that the q35 case relies on is refused on PIIX4, so the bytes read back later are 00.

The suite covers:

- every row of the mapping table and every refusal;
- NACK on write, read and register read;
- bus reset;
- both bus-speed cases;
- 1000 times (failed read, good read) in each mode;
- five sensor values set from the host, including -10.5 C and the 0.5 C rounding of the TMP105's 9-bit mode;
- the non-root checks;
- 10,000-read soaks.

Soak, 10,000 word reads, QEMU with KVM:

| Machine | Mode | us per read | driver CPU |
|---|---|---|---|
| q35 | polling | 1498 | 4.2 s |
| q35 | interrupt | 204 | 2.0 s |
| pc | polling | 1350 | 3.6 s |
| pc | `-i` (falls back to polling) | 1403 | 3.7 s |

No transfer failed in any soak.

### QNX's own sample client

`tests/third_party/fetch_and_run.sh` clones QNX's hardware-component-samples at a fixed commit into a temporary folder, builds `common/rpi_i2c` for x86_64 with its own Makefile, links it with `sample_main.c` and runs it against `/dev/i2c0`. I read the library first: its read and write calls are plain SENDRECV and SEND devctls that map onto Read Byte Data, I2C Block Read, Write Byte Data and block write. The run itself is still to do.
