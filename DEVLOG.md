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
