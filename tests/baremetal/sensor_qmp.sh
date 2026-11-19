#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The host changes a sensor, the driver core sees the change.
# Starts QEMU paused with a tmp105 at 0x48, sets its temperature to 42.5 C
# through QMP (the QEMU control socket), resumes, and checks that the
# bare-metal test read raw 0x802A (42 degrees in the first byte, .5 in the second).
#
# Needs a QEMU with the tmp105 model: bench/qemu/build-qemu.sh.
# Run run.sh first (it builds build/esp/EFI/BOOT/BOOTX64.EFI).
set -u
cd "$(dirname "$0")"
QEMU=${QEMU:-$HOME/work/qemu/bin/qemu-system-x86_64}
OVMF=${OVMF:-/usr/share/OVMF/OVMF_CODE_4M.fd}
SOCK=$(mktemp -u /tmp/i2c-qmp.XXXXXX)
[ -f build/esp/EFI/BOOT/BOOTX64.EFI ] || { echo "run run.sh first" >&2; exit 2; }

rm -f build/serial.raw
timeout 120 "$QEMU" -machine q35 -m 256 -nodefaults -display none -no-reboot -S \
    -qmp "unix:$SOCK,server=on,wait=off" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive format=raw,file=fat:rw:build/esp \
    -device tmp105,address=0x48,id=temp0 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -serial file:build/serial.raw </dev/null &
qpid=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

python3 -I - "$SOCK" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a} if a else {"execute": c}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "event" not in r:
            return r
f.readline()                                    # greeting
cmd("qmp_capabilities")
cmd("qom-set", path="/machine/peripheral/temp0", property="temperature", value=42500)
print("QMP temperature set to", cmd("qom-get", path="/machine/peripheral/temp0", property="temperature")["return"], "mC")
cmd("cont")
EOF
wait $qpid
code=$?
rm -f "$SOCK"
sed -e 's/\x1b\[[0-9;?=<>]*[ -\/]*[@-~]//g' build/serial.raw | tr -cd '\11\12\40-\176' > build/serial.log
grep -E 'tmp105|RESULT' build/serial.log
if [ "$code" -eq 1 ] && grep -q 'tmp105 raw=0x802A' build/serial.log && grep -q '^RESULT pass=[0-9]* fail=0' build/serial.log; then
    echo "sensor via QMP: OK"
else
    echo "sensor via QMP: FAILED (qemu exit $code)" >&2
    exit 1
fi
