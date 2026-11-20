id: i2c-driver-without-hardware
title: Write and test a QNX 8.0 I2C driver without hardware
summary: Run a QNX 8.0 I2C driver against emulated sensors in QEMU, read a sensor from your own program, change its value from the host, and add a tested error path.
categories: Storage & Hardware, Camera & Sensors
tags: intermediate
platforms: QEMU, Ubuntu, Windows (WSL)
published: 2026-11-20
featured: false
status: published
authors: Rayyan Khalil
feedback_link: https://github.com/rayyankah/i2c-ichsmb/issues

# Write and test a QNX 8.0 I2C driver without hardware

## Overview
Duration: 2:00

I2C code usually can't be tested until the board is on your desk, and even then it's hard to make a chip go missing or a sensor read 95 C on purpose. This codelab shows another way: QEMU emulates the SMBus controller found in Intel PC chipsets, and it can put emulated temperature sensors and EEPROMs on that bus. A QNX 8.0 driver running in QEMU talks to them as it would to real chips, and you can change what the sensors read from outside the guest while QNX runs.

The worked example is [i2c-ichsmb](https://github.com/rayyankah/i2c-ichsmb), a QNX 8.0 driver for the Intel chipset SMBus controller, built on the QNX I2C Driver Development Kit (DDK). What you learn applies to any QNX I2C driver.

### What you'll do

- Start QNX 8.0 in QEMU with two temperature sensors and eight EEPROMs on the SMBus.
- See how a QNX I2C driver is put together: the function table the DDK asks for, the status values it expects, and how the driver finds its hardware.
- Write a small client that reads a sensor through `/dev/i2c0`, and change the temperature it reads from the host.
- Add a test for an error path, watch it fail when the driver check is removed, and pass again when it's back.

### What you'll need

- A [QNX Everywhere](https://www.qnx.com/products/everywhere/) licence and QNX SDP 8.0, installed with the QNX Software Center under `~/qnx800`. The base install has everything used here.
- Ubuntu on x86_64, native or in WSL2 on Windows. This codelab was written on Ubuntu 26.04 in WSL2 on Windows 11.
- Disk space and time for one QEMU build (20 to 40 minutes the first time on a 4-core laptop) and one QNX image (a few minutes).
- Some C, and a rough idea of what I2C is: a two-wire bus where a master sends an address, then reads or writes bytes.

<aside>
Everything you measure in this codelab runs inside an emulator. It's good for logic and failure handling and poor for timing. The last step says more about what changes on real hardware.
</aside>

## Set up the bench
Duration: 5:00

Install the packages QEMU needs to build, plus git:

```bash
sudo apt install git build-essential ninja-build meson pkg-config libglib2.0-dev \
    libpixman-1-dev libslirp-dev python3-venv flex bison libfdt-dev zlib1g-dev \
    curl xz-utils gpg
```

Get the code and load the QNX environment:

```bash
git clone https://github.com/rayyankah/i2c-ichsmb.git
cd i2c-ichsmb
source ~/qnx800/qnxsdp-env.sh
```

Start the bench:

```bash
bench/bench up
```

The first time, `up` does three slow things before it boots:

1. Builds QEMU 10.2.1 into `~/work/qemu` with the I2C sensor and EEPROM models switched on (`bench/qemu/build-qemu.sh`). Ubuntu's own QEMU leaves them out for x86. The script checks the release signature before building.
2. Makes an SSH key for the guest if you don't have `~/.ssh/id_ed25519`.
3. Builds a QNX 8.0 image in `~/work/qnx-image` with `mkqnximage --type=qemu`.

Then it boots QNX on QEMU's `q35` machine, builds the driver and tools with `qcc`, copies them to the guest's `/tmp`, starts the driver and scans the bus. The end of the output should look like this:

```
==> starting the driver
i2c-ichsmb -I 16: /dev/i2c0 ready
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                         -- -- -- -- -- -- -- --
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
30: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
40: -- -- -- -- -- -- -- -- 48 49 -- -- -- -- -- --
50: 50 51 52 53 54 55 56 57 -- -- -- -- -- -- -- --
60: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
70: -- -- -- -- -- -- -- --
10 device(s)
```

That's two TMP105 temperature sensors at 0x48 and 0x49 and the eight EEPROMs that QEMU's `q35` machine always puts at 0x50 to 0x57 (they hold memory-module data on a real PC).

<aside>
If <code>/dev/kvm</code> is writable for your user, the bench runs QEMU with KVM. Without it QEMU emulates the processor in software, which works but is much slower.
</aside>

`bench/bench ssh COMMAND` runs a command on the guest as root, and `bench/bench ssh` alone gives you a shell. Check that you're on QNX:

```bash
bench/bench ssh uname -a
```

```
QNX noname 8.0.0 2026/10/30-09:10:05EDT x86pc x86_64
```

The date is the build date of the kernel in your SDP, so yours may differ.

## How a QNX I2C driver is shaped
Duration: 7:00

On QNX a driver is an ordinary process. Clients reach it through a path name, here `/dev/i2c0`, and every request is a message from the client to the driver. Writing the code that receives those messages (a resource manager) is a lot of work, so QNX ships it for I2C as a library: `libi2c-master`, part of the DDK. It supplies `main()`, parses the standard options, creates `/dev/i2cN` and decodes each request. You supply the part that touches the hardware, as a table of functions.

The library finds the table by calling `i2c_master_getfuncs()`, which your driver must define. From `driver/qnx/i2c-ichsmb.c`:

```c
int i2c_master_getfuncs(i2c_master_funcs_t *funcs, int tabsize)
{
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, init, ichsmb_init, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, fini, ichsmb_fini, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, send, ichsmb_send, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, recv, ichsmb_recv, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, xfer, ichsmb_xfer, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, abort, ichsmb_abort, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, set_slave_addr, ichsmb_set_slave_addr, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, set_bus_speed, ichsmb_set_bus_speed, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, version_info, ichsmb_version_info, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, driver_info, ichsmb_driver_info, tabsize);
    I2C_ADD_FUNC(i2c_master_funcs_t, funcs, bus_reset, ichsmb_bus_reset, tabsize);
    return 0;
}
```

What each entry is for:

| Function | Called when |
|---|---|
| `init(argc, argv)` | once at start, with the options the library didn't take; returns your state, or NULL to refuse to start |
| `fini` | at an orderly exit |
| `set_slave_addr` | before a transfer, with the client's chip address |
| `send`, `recv` | a client writes or reads bytes |
| `xfer` | a client asks for "write, then read without releasing the bus" (only if your driver says it needs this, see below) |
| `driver_info` | a client asks what the driver can do |
| `set_bus_speed`, `bus_reset`, `abort` | the client asks for them, or a request is cancelled |

### The status contract

`send`, `recv` and `xfer` return an `i2c_status_t`. The library counts a call as successful only when it returns exactly `I2C_STATUS_DONE`. A failed transfer is still "done", so errors come back as `DONE` plus a cause:

```c
case SMB_OK:        return I2C_STATUS_DONE;
case SMB_E_NACK:    return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_NACK);
case SMB_E_BUS:     return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_ARBL);
case SMB_E_TIMEOUT:
case SMB_E_BUSY:    return (i2c_status_t)(I2C_STATUS_DONE | I2C_STATUS_BUSY);
```

The client sees `EIO` from its request, and can then ask for the cause with `DCMD_I2C_STATUS`. You'll use that in your own client shortly.

### When the hardware can't do what I2C can

A plain I2C master can put any bytes on the wire. The chipset controller can't: it runs a fixed set of SMBus transactions (Quick, Send Byte, Read Byte Data, Read Word Data, block transfers). The driver's job is to take each I2C request and pick the one transaction that puts exactly the same bytes on the wire, or refuse it. That table is at the top of `driver/core/smb_map.c`.

The controller also can't hold the bus between two separate requests. So `ichsmb_driver_info()` sets `I2C_CAPS_FORCE_STOP`, and the library then hands "write, then read" to `xfer()` as one call, which the controller can do as one transaction:

```c
info->caps = I2C_CAPS_FORCE_STOP;   /* every message ends with a stop; use xfer() */
```

Ask the running driver what it reported:

```bash
bench/bench ssh /tmp/smbctl info
```

```
speed modes  0x1 (100 kHz only)
addressing   0x2 (7-bit)
caps         0x1 (write-then-read goes out as one combined transfer)
```

<aside>
A request is a message, and the driver thread that receives it runs at the priority of the client that sent it. A high-priority client is therefore served at high priority, without the driver doing anything. Keep that in mind for the last step.
</aside>

## Find and reach the hardware
Duration: 5:00

`init()` has to find the controller, get permission to touch it, and map its registers. The pieces, all from `driver/qnx/i2c-ichsmb.c`:

**Permission for port I/O.** On x86 the controller's registers are I/O ports, and a thread needs I/O privilege to use `in8()` and `out8()`. Only root can get it:

```c
if (ThreadCtl(_NTO_TCTL_IO_LEVEL, (void *)(_NTO_IO_LEVEL_1 | _NTO_TCTL_IO_LEVEL_INHERIT)) == -1) {
    logmsg(h, _SLOG_ERROR, "no I/O privilege (%s): start as root, then use -U to drop to a user", ...);
```

**Finding the device through the PCI server.** QNX's PCI server owns PCI configuration space. The driver asks it for the device by vendor and device ID, attaches to it (which also stops a second driver from claiming the same controller), and reads its I/O base address:

```c
h->bdf = pci_device_find(0, PCI_VID_INTEL, hw_table[i].did, PCI_CCODE_ANY);
...
h->pci = pci_device_attach(h->bdf, pci_attachFlags_DEFAULT, &err);
pci_device_read_ba(h->pci, &nba, ba, pci_reqType_e_UNSPECIFIED);
```

**Mapping the ports.** `mmap_device_io()` turns the base address into a handle for `in8()` and `out8()`:

```c
h->iobase = mmap_device_io(SMB_IO_LEN, h->iobase);
...
static uint8_t q_in8(void *ctx, unsigned reg)             { return in8(((ichsmb_t *)ctx)->iobase + reg); }
static void    q_out8(void *ctx, unsigned reg, uint8_t v) { out8(((ichsmb_t *)ctx)->iobase + reg, v); }
```

Those two functions are handed to the controller logic in `driver/core` as callbacks, together with "wait" and "read the clock". `driver/core` has no QNX calls at all. That's a choice worth copying: the same controller code also runs on a PC against a software model of the controller (`make -C tests/host`), which is how its timeout and bus-error paths are tested, since QEMU never produces those.

See what the driver found. Restart it with more logging and read its log:

```bash
bench/bench driver -I 16 -vv
bench/bench ssh "slog2info -b i2c_ichsmb | grep 'SMBus at'"
```

```
... i2c-ichsmb: ICH9 SMBus at 0:1f.3, I/O 0x700, interrupt 16 (-I), status 0x00
```

`-I 16` sets the interrupt number by hand. On QEMU's `q35` machine the PCI server reports a different one that never fires; the last step explains how that was found. Put the driver back to its normal logging:

```bash
bench/bench driver
```

## A first transfer
Duration: 4:00

`smbctl` is a small command-line client in `tools/smbctl`. It only uses the standard QNX I2C requests, so it works with any QNX I2C driver.

The scan you saw earlier sends a Receive Byte to every address from 0x08 to 0x77. A chip that's there answers with an acknowledge; an empty address gets no acknowledge (NACK), which the driver reports and `smbctl` prints as `--`.

Write a byte to the first EEPROM and read it back:

```bash
bench/bench ssh /tmp/smbctl set 0x50 0 0x41
bench/bench ssh /tmp/smbctl get 0x50 0
```

```
0x41
```

Each `smbctl` command is one or two `devctl()` calls on `/dev/i2c0`. `get` is `DCMD_I2C_SENDRECV`: write the register number, then read one byte, without releasing the bus in between. The library passes it to the driver's `xfer()`, and the driver turns it into an SMBus Read Byte Data.

Ask for an address with no chip on it:

```bash
bench/bench ssh /tmp/smbctl get 0x30 0
```

```
smbctl: get: no acknowledge
```

## Read the sensor from your own program
Duration: 7:00

The sensors come from QEMU's command line. `bench/qemu/run-qnx.sh` adds one device per address in `SENSORS` (set in `bench/config.env`):

```bash
-device tmp105,address=0x48,id=temp0 -device tmp105,address=0x49,id=temp1
```

QEMU attaches them to the SMBus of the `q35` machine. To QNX they're simply chips that answer at 0x48 and 0x49.

Now write a client. A TMP105 keeps the temperature in register 0, two bytes, most significant first, in steps of 1/256 C. Reading it is "write 0, then read 2 bytes", which is one `DCMD_I2C_SENDRECV` request. Save this as `mytemp.c` in an empty directory (it is also in the repository as `codelab/files/mytemp.c`):

```c
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hw/i2c.h>

int main(int argc, char *argv[])
{
    unsigned addr = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 0x48;
    struct {
        i2c_sendrecv_t hdr;         /* who to talk to, how many bytes each way */
        uint8_t        buf[2];      /* bytes to send, then the bytes received */
    } msg;
    int16_t raw;
    int fd, rc;

    fd = open("/dev/i2c0", O_RDWR);
    if (fd == -1) {
        perror("mytemp: /dev/i2c0");
        return 1;
    }

    /* Write the register number (0, temperature), repeated start, read 2 bytes. */
    memset(&msg, 0, sizeof(msg));
    msg.hdr.slave.addr = addr;
    msg.hdr.slave.fmt = I2C_ADDRFMT_7BIT;
    msg.hdr.send_len = 1;
    msg.hdr.recv_len = 2;
    msg.hdr.stop = 1;
    msg.buf[0] = 0x00;

    rc = devctl(fd, DCMD_I2C_SENDRECV, &msg, sizeof(msg), NULL);
    if (rc != EOK) {
        i2c_status_t st = 0;

        /* EIO means the transfer failed on the bus; the driver says why. */
        if (rc == EIO && devctl(fd, DCMD_I2C_STATUS, &st, sizeof(st), NULL) == EOK
            && (st & I2C_STATUS_NACK)) {
            fprintf(stderr, "mytemp: no chip answers at 0x%02x\n", addr);
        } else {
            fprintf(stderr, "mytemp: 0x%02x: %s\n", addr, strerror(rc));
        }
        return 1;
    }

    /* Most significant byte first, two's complement, 1/256 C per step. */
    raw = (int16_t)((msg.buf[0] << 8) | msg.buf[1]);
    printf("0x%02x: %.2f C\n", addr, raw / 256.0);
    return 0;
}
```

The request and the reply share one buffer: the header says how many bytes to send and how many to receive, the bytes to send follow the header, and the received bytes come back in the same place.

Build it with the QNX compiler for x86_64 and copy it to the guest. Run these from the `i2c-ichsmb` directory, with the path to your `mytemp.c`:

```bash
qcc -Vgcc_ntox86_64 -Wall -o /tmp/mytemp path/to/mytemp.c
bench/qemu/run-qnx.sh scp /tmp/mytemp root@127.0.0.1:/tmp/
bench/bench ssh /tmp/mytemp
```

```
0x48: 0.00 C
```

QEMU's TMP105 starts at 0 C. The program you just wrote doesn't know which driver is behind `/dev/i2c0`; it would run unchanged against any QNX I2C driver on any board with a TMP105.

## Change the sensor from the host
Duration: 4:00

QEMU has a control socket (QMP) that can set properties of devices while the guest runs. The TMP105 model has a `temperature` property in thousandths of a degree. `bench/bench heat` sets it through `bench/qmp.py`:

```bash
bench/bench heat 0x48 25
bench/bench ssh /tmp/mytemp
```

```
0x48  25.000 C
0x48: 25.00 C
```

Negative values work too:

```bash
bench/bench heat 0x48 -12.5
bench/bench ssh /tmp/mytemp
```

```
0x48: -12.50 C
```

Under the hood that's one QMP command, `qom-set` on `/machine/peripheral/temp0` with `"property": "temperature", "value": -12500`.

To watch a change arrive, set the sensor back to 25 C and open a second terminal in the same directory. In the first terminal, watch register 0 of the sensor every 100 ms:

```bash
bench/bench heat 0x48 25
bench/bench ssh /tmp/smbctl watch 0x48 0 100
```

In the second, change the temperature twice:

```bash
bench/bench heat 0x48 30
bench/bench heat 0x48 31.5
```

The first terminal prints a line each time the value changes (the time, then the register's value; a one-byte read only gets the whole degrees):

```
        0 ms  0x19  25
     1936 ms  0x1e  30
     3159 ms  0x1f  31
```

Stop it with Ctrl-C. For longer sequences, `bench/bench scenario bench/scenarios/overheat.txt` plays a script of temperature steps and ramps. That's how you can test what your own code does when a sensor overheats, with no heat gun involved.

## An error path, test first
Duration: 7:00

QEMU makes some failures easy to produce. A chip that isn't there is one:

```bash
bench/bench ssh /tmp/mytemp 0x4a
```

```
mytemp: no chip answers at 0x4a
```

That message has come a long way. The controller set its "device error" bit; `driver/core` turned it into `SMB_E_NACK`; the driver returned `I2C_STATUS_DONE | I2C_STATUS_NACK`; the library failed the client's `devctl()` with `EIO`; and `mytemp` asked `DCMD_I2C_STATUS` for the reason.

Now a failure that's less obvious. Address 0x00 is the I2C *general call*: every chip on the bus takes a write sent to it. A client that sends a register number to 0x00 by mistake moves the register pointer of every chip on the bus. So the driver refuses the reserved addresses (0x00 to 0x07 and 0x78 to 0x7F) in `smb_bus_set_addr()` in `driver/core/smb_map.c`:

```c
if (addr < 0x08 || addr > 0x77) {
    return SMB_E_INVAL;
}
```

You'll write a test for that check, then make sure the test can actually fail.

### Write the test

The on-target tests are JSON files in `tests/cases/`. Each case can set sensors over QMP, then runs operations with the test client `t_xfer` on the guest and compares its output, line by line, with regular expressions. `sendrecv:ADDR:HEX:N` writes bytes and reads N; `send:ADDR:HEX` writes; `recv:ADDR:N` reads N bytes with no register byte.

Create `tests/cases/90-mine.json`:

```json
[
 {
  "name": "mine: a write to 0x00 is refused and the sensor is untouched",
  "qmp": [ { "temp": 25000 } ],
  "ops": [ "sendrecv:0x48:00:1", "send:0x00:01", "recv:0x48:1" ],
  "expect": [ "OK 19", "ERR EIO", "OK 19" ]
 }
]
```

It sets the sensor to 25 C (0x19), points the sensor at register 0 and reads it, tries to send 0x01 to address 0x00, and reads the sensor again with no register byte, which returns whatever register its pointer is on. If the general call got through, the pointer has moved to register 1 and the read returns something else.

Run only your case:

```bash
bench/bench test --only mine:
```

```
PASS  mine: a write to 0x00 is refused and the sensor is untouched  [poll]

1 cases, 1 passed, 0 failed, 1 s
```

### Make sure it can fail

A test you've never seen fail doesn't prove much. Switch the check off:

```bash
sed -i 's/    if (addr < 0x08 || addr > 0x77) {/    if (0 \&\& (addr < 0x08 || addr > 0x77)) {/' driver/core/smb_map.c
bench/bench test --only mine:
```

The test runner rebuilds the driver and copies it to the guest before it runs, so this tests your change:

```
FAIL  mine: a write to 0x00 is refused and the sensor is untouched  [poll]
        got  OK 19
        want OK 19
        got  OK
        want ERR EIO
        got  OK 00
        want OK 19
```

The write to 0x00 went out, and afterwards the sensor answered with register 1 (its configuration, 0x00) instead of the temperature. The EEPROMs' address pointers move too. Put the check back and run the test again:

```bash
git checkout driver/core/smb_map.c
bench/bench test --only mine:
```

```
PASS  mine: a write to 0x00 is refused and the sensor is untouched  [poll]
```

`bench/bench test` with no options runs every case, in polling mode, interrupt mode, as a non-root user, and with sensors missing from the boot. Run it before you change anything bigger.

## Interrupts and priority
Duration: 3:00

This step is reading only. Two lessons from this driver are worth knowing before you write your own.

**Check that interrupts actually arrive.** In interrupt mode the driver attaches its interrupt with `InterruptAttachEvent()`, which delivers a pulse to a private channel, and waits for that pulse with `MsgReceivePulse()` and a timeout. On `q35` the PCI server reported interrupt 10, and the driver attached it. It never fired, and nothing noticed, because the driver still worked through its timeouts and QEMU's controller finishes so fast that the waits didn't show. The fix was to find the real line (16, by trying each candidate), add `-I` to set it, and make the driver log "first interrupt received" once, or a warning when 20 transactions pass without one.

**A pulse can change your thread's priority.** Receiving a pulse normally sets the receiving thread to the pulse's priority. The interrupt pulse arrives at the priority of the thread that attached it, 10. So a request from a priority-30 client finished at priority 10, and with a priority-20 program keeping the processors busy, the client stalled for 15 seconds. One flag on the channel keeps the priority the thread inherited from the client:

```c
h->chid = ChannelCreate(_NTO_CHF_PRIVATE | _NTO_CHF_FIXED_PRIORITY);
```

The repository's `demo/` has the programs that showed this (a monitor with a 10 ms deadline and a load generator), and `bench/measure.sh` repeats the measurement.

## Clean up and next steps
Duration: 1:00

Stop the guest:

```bash
bench/bench down
```

You have run a QNX 8.0 I2C driver against emulated chips, read a sensor from your own program, changed it from the host, and added a test that you watched fail and pass.

To take this further:

- Read `docs/DESIGN.md` in the repository for the full request mapping, error handling and privilege dropping (`-U`), and `docs/LIMITS.md` for what QEMU can't show you. QEMU doesn't model bus timing, a chip holding the clock low, or another master on the bus.
- Point the bench at your own I2C code: anything that uses `/dev/i2c0` and the `DCMD_I2C_*` requests runs against it unchanged. QEMU also has `tmp421`, `emc1413` and `at24c-eeprom` models in the bench's build.
- On real hardware, the same driver binary should find the controller through the PCI server, but nothing here has been tested on a physical board. Check that the interrupt number the PCI server reports is the one that fires: run with `-vv` and look for "first interrupt received".
