# Walkthrough

A guided reading of the code, about an hour, then the questions a QNX engineer is likely to ask about it, then five exercises. Have the bench running (`bench/bench up`) for the exercises.

## Reading order

1. The big picture (5 min). `README.md` and `docs/architecture.svg`. Keep the diagram open while you read the rest.

2. What a client sends (5 min). `tools/lib/i2cdev.c`. Three functions, one `devctl()` each. Note the shape of the message: a header (`i2c_send_t`, `i2c_recv_t` or `i2c_sendrecv_t`) with the address and the lengths, then the data in the same buffer. On error, `i2cdev_error()` asks `DCMD_I2C_STATUS` for the reason.

3. The driver's QNX side (15 min). `driver/qnx/i2c-ichsmb.c`, in this order:
   - `i2c_master_getfuncs()` at the bottom: the table `libi2c-master` calls into.
   - `ichsmb_init()`: options, `ThreadCtl()` for I/O privilege, `locate()`, `setup_ich()`, `mmap_device_io()`, the callbacks for the core, the interrupt channel, then `drop_privileges()` if `-U` was given.
   - `ichsmb_xfer()` and `to_status()`: every call into the core comes back as `I2C_STATUS_DONE`, alone or with a cause.
   - `q_wait()` and `q_irq_done()`: how the driver waits for the controller in each mode, and when it unmasks the interrupt.
   - `ichsmb_driver_info()`: why `I2C_CAPS_FORCE_STOP` is set.

4. Request mapping (10 min). `driver/core/smb_map.c`. Read the table in the comment at the top first, then `smb_bus_set_addr()`, `smb_bus_xfer()`, `write_only()` and `write_then_read()`. Each request either becomes exactly one SMBus transaction or is refused. `smb_bus_send()` and `smb_bus_recv()` hold a send without a stop until the matching receive arrives.

5. The controller engine (10 min). `driver/core/smb_core.c`. `run()` is the whole state machine: start the transaction, wait, read the status, feed or collect block bytes one at a time, finish or kill on error or timeout. The public functions below it (`smb_read_byte_data()` and the rest) only fill in a `txn_t` and call `run()`. `driver/core/ichsmb_regs.h` has the register map from the Intel datasheets.

6. How it's tested (10 min). `tests/host/sim.c` is a software model of the controller, with slow devices, NACKs, bus errors and a controller that never finishes; `tests/host/test_core.c` runs the core against it. Then `tests/run_tests.py` and one or two files in `tests/cases/`: a case sets sensors over QMP, runs `tests/target/t_xfer` on the guest and matches its output.

7. The demo (10 min). `demo/thermd/thermd.c` (`main()`: timer, priority, the loop), then `demo/supervisor/smb-supervisor.c` (`main()`: the channel with `_NTO_CHF_COID_DISCONNECT`, waiting for `_PULSE_CODE_COIDDEATH`).

8. Why things are the way they are. `docs/DESIGN.md`, especially Interrupt mode and Privileges, and `docs/LIMITS.md`.

## The path of one read

`smbctl get 0x50 0`, start to finish:

1. `cmd_get()` in `smbctl` calls `i2cdev_write_read(fd, 0x50, {0x00}, 1, buf, 1)`.
2. That fills an `i2c_sendrecv_t` (address 0x50, 7-bit, send 1 byte, receive 1, stop) followed by the byte 0x00, and calls `devctl(fd, DCMD_I2C_SENDRECV, ...)`. `devctl()` is a message to the process that owns `/dev/i2c0`. `smbctl` is blocked until the reply.
3. In the driver process, `libi2c-master` receives the message on a thread that now runs at `smbctl`'s priority. It calls `ichsmb_set_slave_addr(0x50)`, then, because the driver reported `I2C_CAPS_FORCE_STOP`, `ichsmb_xfer()` with the one byte to send and the one byte to receive.
4. `ichsmb_xfer()` calls `smb_bus_xfer()` in `driver/core/smb_map.c`. One byte out, one byte in: that's Read Byte Data with command byte 0x00. It calls `smb_read_byte_data()`.
5. `run()` in `smb_core.c` writes the address, the command and the control register through `out8()`, which starts the transaction, then reads the status register through `in8()`. In polling mode `q_wait()` sleeps between status reads; in interrupt mode it blocks in `MsgReceivePulse()` until the interrupt pulse or a timeout.
6. When the status says done, `run()` reads the data register, clears the status, and calls `q_irq_done()`, which unmasks the interrupt if the kernel had masked it.
7. Back up: `SMB_OK` becomes `I2C_STATUS_DONE`, the library copies the byte into the reply and replies to `smbctl`, `devctl()` returns `EOK`, and `smbctl` prints `0x41`.

If no chip answers, step 5 sees the device-error bit, the core returns `SMB_E_NACK`, the driver returns `I2C_STATUS_DONE | I2C_STATUS_NACK`, the library fails the `devctl()` with `EIO`, and `smbctl` asks `DCMD_I2C_STATUS` and prints "no acknowledge".

## Ten questions, with answers

**1. Why is the controller logic in a separate core with no QNX calls?**
So it can be tested where QNX can't help. The same `driver/core` runs against a software model on a PC (`tests/host`), on bare metal inside QEMU with no QNX licence (`tests/baremetal`), and in the QNX driver. The model produces failures QEMU can't, such as a bus collision or a controller that never finishes, so those paths are tested at all.

**2. What does the request mapping table do, and why refuse instead of approximate?**
The chipset controller can only run fixed SMBus transactions, while an I2C client asks for raw bytes. For each request the mapper finds the one transaction that puts exactly the same bytes on the wire. If there isn't one, sending something close would talk to the chip in a way the client didn't ask for, possibly writing the wrong register, so the request fails with an error instead.

**3. Why `I2C_CAPS_FORCE_STOP` and `xfer()`?**
The controller can't keep the bus between two separate commands; "write, repeated start, read" only exists as one transaction. With the flag set, `libi2c-master` passes a write-then-read request to `xfer()` in one call, and refuses a send without a stop. The core also handles frameworks that call `send(no stop)` then `recv()` by holding the send.

**4. Why do errors come back as `I2C_STATUS_DONE` plus a cause?**
`libi2c-master` counts a call as successful only when the driver returns exactly `I2C_STATUS_DONE`. Anything else fails the client's `devctl()` with `EIO`. "Done" means the transfer is over, successful or not; the extra bits say why it failed, and the client reads them with `DCMD_I2C_STATUS`.

**5. Why `-I 16` on q35, and how was 16 found?**
The PCI server takes the interrupt from its routing table, which has no entry for the SMBus function on QEMU's q35 machine, so it reports 10, the legacy line in configuration space. QNX uses the I/O APIC, where QEMU delivers this controller's interrupt on input 16. A probe build attached each candidate in turn and ran one transaction; only 16 fired. The driver now logs "first interrupt received", or a warning after 20 transactions without one, because the wrong number failed silently before.

**6. Can an interrupt be lost between reading the status and waiting?**
No. The kernel masks the line each time it delivers the event. The driver reads the status, clears the status bits (which lowers the line), and only then unmasks. If the controller raises the line again before the driver waits, the pulse is already queued on the channel, so `MsgReceivePulse()` returns at once. Because the line stays masked until the unmask, at most one pulse can be pending.

**7. What is `_NTO_CHF_FIXED_PRIORITY` for?**
A thread that receives a pulse normally takes the pulse's priority. The interrupt pulse comes at the priority of the thread that attached the interrupt, 10. Without the flag, a request from a priority-30 client finished at 10, and a priority-20 program could stall it for seconds (`docs/results/thermd-inversion-before-fix.txt`). With it, the driver thread keeps the priority it inherited from the client's message.

**8. What does the driver keep after `-U`, and why each?**
It needs root at start for I/O privilege, the PCI attach and the interrupt attach. Then it allows and locks, for a non-root process, only `PATHSPACE` (the library registers `/dev/i2cN` after `init()` returns), `PUBLIC_CHANNEL` (the library's channel) and, in interrupt mode, `INTERRUPT`. It switches user with `set_ids_from_arg()` and denies everything else. I/O privilege stays with the thread `ThreadCtl()` gave it to.

**9. What happens when the driver dies?**
Clients' next request fails (the connection is gone). `smb-supervisor` keeps `/dev/i2c0` open on a channel created with `_NTO_CHF_COID_DISCONNECT`, so the kernel sends it a `_PULSE_CODE_COIDDEATH` pulse when the driver's channel disappears, with no polling. It starts the driver again and waits for `/dev/i2c0`. `thermd` reopens the device and logs how long it went without data. Recorded: 20 kills, 20 restarts in 4 to 8 ms, 20 ms without data each, no missed deadline (`docs/results/recovery.txt`).

**10. What can't the bench tell you?**
Timing on real hardware: QEMU finishes transactions instantly and every time is emulated. Failures QEMU can't produce: bus collisions, a chip holding the clock low, firmware owning the controller, a chip unplugged while running. Those are covered by the host tests against the software model, or not at all, as listed in `docs/LIMITS.md`. And nothing has run on a physical board.

## Five exercises

1. Add a subcommand. Add `smbctl limits ADDR`, which prints a TMP105's low and high limits (registers 2 and 3) in degrees C. Start from `cmd_temp()` in `tools/smbctl/smbctl.c`, then `make qnx`, `bench/bench deploy` and try it. Read the registers the way `cmd_temp()` does, not with `getw`: SMBus Read Word sends the low byte first, and the TMP105 sends the high byte first. Compare with `smbctl getw 0x48 2` to see the swap.

2. Change the timeout and see which test fails. In `driver/core/smb_core.h`, set `SMB_DEFAULT_TIMEOUT_US` to `1000u` (1 ms) and run `make host-test`. Two of the matrix tests fail. Find in `tests/host/sim.c` what the model does that 1 ms isn't enough for, and why the SMBus specification's 35 ms limit is the number that matters. Put it back.

3. Remove the priority fix. Take `_NTO_CHF_FIXED_PRIORITY` out of the `ChannelCreate()` call in `ichsmb_init()`, then run `bench/bench test --only realtime`. Read the output of the failing case, then explain it using question 7. Put the flag back and run it again.

4. Add a chip to the bus. Stop the bench and start it with a third sensor: `bench/bench down`, then `SENSORS="0x48 0x49 0x4a" bench/bench up`. Run `bench/bench test` and see which test fails and why. Then write a case in a new file `tests/cases/91-third.json` that sets the third sensor to 55 C (`"qmp": [ { "sensor": "temp2", "temp": 55000 } ]`) and reads it back, and make it pass. Restart the bench with the default sensors afterwards.

5. Run without root. Start the driver with `bench/bench driver -I 16 -U qnxuser` and check `bench/bench ssh "pidin -p i2c-ichsmb user"`. Then remove `PROCMGR_AID_INTERRUPT` from the list in `drop_privileges()`, deploy, start it the same way, and run `bench/bench test --only user:`. Before you run it, write down what you expect to fail and why; then compare.
