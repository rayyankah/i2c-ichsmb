#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the bare-metal test as a UEFI program and run it in QEMU (q35).
# Needs no QNX kit. Works on Linux/WSL and on Windows (Git Bash + MinGW-w64).
#
#   Ubuntu:  sudo apt install gcc-mingw-w64-x86-64 qemu-system-x86 ovmf
#   Windows: MinGW-w64 gcc on PATH; QEMU for Windows; set QEMU and OVMF below.
#
# Environment overrides:
#   CC    UEFI-capable compiler   (default x86_64-w64-mingw32-gcc, else gcc on Windows)
#   QEMU  emulator binary         (default qemu-system-x86_64)
#   OVMF  UEFI firmware image     (default: first of the usual locations)
#   QEMU_EXTRA  extra QEMU options, e.g. "-device tmp105,address=0x48" where the
#               QEMU build has that model (Ubuntu's x86 build does not)
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-$(command -v x86_64-w64-mingw32-gcc || command -v gcc)}
QEMU=${QEMU:-qemu-system-x86_64}
if [ -z "${OVMF:-}" ]; then
    for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd \
             "$(dirname "$(command -v "$QEMU" 2>/dev/null || echo .)")/share/edk2-x86_64-code.fd"; do
        [ -f "$f" ] && OVMF=$f && break
    done
fi
[ -n "${OVMF:-}" ] || { echo "UEFI firmware not found; set OVMF=/path/to/firmware.fd" >&2; exit 2; }

CORE=../../driver/core
OUT=build/esp/EFI/BOOT
mkdir -p "$OUT"
"$CC" -std=gnu99 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -mno-red-zone \
      -nostdlib -I"$CORE" -Wl,--subsystem,10 -Wl,-e,efi_main \
      -o "$OUT/BOOTX64.EFI" smbtest.c "$CORE/smb_core.c" "$CORE/smb_map.c"

set +e
# Serial output goes to a file, never straight to the terminal: the UEFI
# firmware writes terminal control codes (screen clear, resize, mouse mode)
# that would otherwise change the user's terminal. The screen gets a copy
# with every control code removed.
timeout 120 "$QEMU" -machine q35 -m 256 -nodefaults -display none -no-reboot \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive format=raw,file=fat:rw:build/esp \
    ${QEMU_EXTRA:-} \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -serial file:build/serial.raw </dev/null
code=$?
set -e
sed -e 's/\x1b\[[0-9;?=<>]*[ -\/]*[@-~]//g' build/serial.raw | tr -cd '\11\12\40-\176' > build/serial.log
cat build/serial.log
# isa-debug-exit: the program writes 0 on success, QEMU exits with (0 << 1) | 1 = 1
if [ "$code" -eq 1 ] && grep -q '^RESULT pass=[0-9]* fail=0' build/serial.log; then
    echo "bare-metal test: OK"
else
    echo "bare-metal test: FAILED (qemu exit $code)" >&2
    exit 1
fi
