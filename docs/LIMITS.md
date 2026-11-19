# Limits

## Not tested on hardware

Everything here ran in QEMU 10.2.1 (q35 and pc machines, KVM under WSL2 on Windows 11). No physical Intel board was available. QEMU's SMBus controller model was written from the same Intel datasheets, and it differs from real chipsets in ways that matter:

- It finishes a whole transaction inside the register write that starts it (with interrupts enabled), or at the first status read (without). Real controllers take about half a millisecond for a word read at 100 kHz, so on hardware the driver actually waits, in both modes. The waiting code is covered by the host tests against a software model of the controller (`tests/host`), not by QEMU.
- It never finishes a 32-byte block write in byte-by-byte mode (a QEMU bug, DEVLOG 19 Nov). The driver stops that transfer with an error; real hardware would complete it.
- On q35 the interrupt arrives on I/O APIC input 16, but the image's PCI server reports 10, hence `-I 16` on the bench. A real board's PCI server configuration may or may not have the right entry.

Chipsets other than ICH9 and PIIX4 are only reachable with `-p` and untested. Their register layout is the same family, but newer ones add features (host notify, SMBus 2.0 PEC, a separate I2C block mode) that the driver doesn't use.

## Timing

Every time in `docs/results/` is measured inside the emulator: register accesses are traps into QEMU, and the virtual processors are threads on a Windows host. The numbers compare modes and priorities on the same bench; they are not what a board would show. `thermd`'s worst wake-up lateness without any load is already a few milliseconds, which is the host, not QNX.

## Failures QEMU can't produce

| Failure | Real cause | In QEMU | Covered by |
|---|---|---|---|
| no acknowledge | chip absent or busy | yes | tests (NACK cases, sensor left out of the boot) |
| bus collision, lost arbitration | another master on the bus | no | host tests (software model) |
| transaction never finishes | chip holds the clock low, controller hangs | no | host tests (timeout path) |
| bus stuck low after a reset | chip mid-transfer when the host reset | no | not covered; `bus_reset()` only resets the controller |
| firmware owns the controller (HOST_BUSY at start) | BIOS or ME using the SMBus | no | host tests (busy path) |
| SMI routing left on by firmware | BIOS setting | no | the driver clears SMI_EN at start; untested |
| chip removed while running | hot unplug | no | QEMU's I2C devices can't be unplugged; a boot without the chip is tested instead |
| controller disabled by firmware (no I/O base) | BIOS setting | no | error message only, untested |
| corrupted data on the wire | noise | no | not detected; the driver doesn't use packet error checking (PEC) |

## Not implemented

- SMBus packet error checking (PEC), address resolution protocol, host notify, acting as a bus slave.
- 10-bit addresses (the controller can't).
- Bus speeds other than 100 kHz (the chipset fixes the clock); other speeds are refused.
- Interrupt mode on PIIX4.
- More than one controller per driver process (start one driver per controller, with `-p` and `--u`).
