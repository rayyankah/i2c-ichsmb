#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build QEMU for x86_64 with I2C sensor and EEPROM models on the PC SMBus.
# Distribution QEMU builds for x86 leave them out (Ubuntu 26.04's has no
# tmp105), but the bench needs a sensor whose value the host can change.
#
# Ubuntu packages needed:
#   build-essential ninja-build meson pkg-config libglib2.0-dev libpixman-1-dev
#   libslirp-dev python3-venv flex bison libfdt-dev zlib1g-dev curl xz-utils gpg
#
# Result: $PREFIX/bin/qemu-system-x86_64 (default PREFIX=~/work/qemu).
# Takes 20-40 minutes on a 4-core laptop. Re-running resumes the build.
set -euo pipefail

VER=${VER:-10.2.1}
SRC=${SRC:-$HOME/work/qemu-src}
PREFIX=${PREFIX:-$HOME/work/qemu}
# QEMU release signing key (Michael Roth), as published on qemu.org
KEY=CEACC9E15534EBABB82D3FA03353C9CEF108B584

mkdir -p "$SRC"
cd "$SRC"
if [ ! -d "qemu-$VER" ]; then
    curl -sSfLO "https://download.qemu.org/qemu-$VER.tar.xz"
    curl -sSfLO "https://download.qemu.org/qemu-$VER.tar.xz.sig"
    gpg --keyserver hkps://keys.openpgp.org --recv-keys "$KEY"
    gpg --verify "qemu-$VER.tar.xz.sig" "qemu-$VER.tar.xz"
    tar xf "qemu-$VER.tar.xz"
    cat >> "qemu-$VER/configs/devices/x86_64-softmmu/default.mak" <<'EOF'

# i2c-ichsmb bench: I2C sensor and EEPROM models for the PC SMBus
CONFIG_I2C_DEVICES=y
CONFIG_TMP105=y
CONFIG_TMP421=y
CONFIG_EMC141X=y
CONFIG_AT24C=y
EOF
fi

cd "qemu-$VER"
if [ ! -f build/build.ninja ]; then
    ./configure --target-list=x86_64-softmmu --prefix="$PREFIX" \
        --enable-slirp --enable-kvm --disable-docs --disable-werror \
        --disable-gtk --disable-sdl --disable-vnc --disable-opengl > ../configure.log 2>&1
fi
make -j"$(nproc)" > ../make.log 2>&1 || { tail -20 ../make.log; exit 1; }
make install > ../install.log 2>&1
"$PREFIX/bin/qemu-system-x86_64" -device help | grep -E '"(tmp105|tmp421|emc1413|at24c-eeprom)"'
echo "built: $PREFIX/bin/qemu-system-x86_64"
