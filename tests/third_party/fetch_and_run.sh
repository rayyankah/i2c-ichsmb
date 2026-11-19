#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run QNX's own I2C client library against i2c-ichsmb, unchanged.
#
# Fetches gitlab.com/qnx/projects/hardware-component-samples at a fixed
# commit into a temporary folder (nothing from it is committed here),
# builds common/rpi_i2c for x86_64 with its own Makefile, links it with
# sample_main.c, and runs that on the QEMU guest against /dev/i2c0.
#
# Needs: source ~/qnx800/qnxsdp-env.sh, and a running guest
# (bench/qemu/run-qnx.sh start). The output also goes to
# docs/results/qnx-sample.txt.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BENCH="$ROOT/bench/qemu/run-qnx.sh"
REPO=https://gitlab.com/qnx/projects/hardware-component-samples.git
COMMIT=6276447fac3bb2d251e2690ab627037b1990e509
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git clone -q "$REPO" "$TMP/samples"
git -C "$TMP/samples" checkout -q "$COMMIT"
make -s -C "$TMP/samples/common/rpi_i2c" PLATFORM=x86_64 BUILD_PROFILE=release > "$TMP/build.log" 2>&1 \
    || { cat "$TMP/build.log"; exit 1; }
LIB=$(find "$TMP/samples/common/rpi_i2c/build" -name 'librpi_i2c.a' | head -1)
qcc -Vgcc_ntox86_64 -std=gnu99 -O2 -Wall -I"$TMP/samples/common/rpi_i2c/public" \
    -o "$TMP/qnx_sample" "$HERE/sample_main.c" "$LIB"

bash "$BENCH" scp "$TMP/qnx_sample" root@127.0.0.1:/tmp/
bash "$BENCH" ssh 'slay -f i2c-ichsmb >/dev/null 2>&1; sleep 0.3; /tmp/i2c-ichsmb -i </dev/null >/dev/null 2>&1' || true
mkdir -p "$ROOT/docs/results"
{
    echo "QNX hardware-component-samples, common/rpi_i2c at $COMMIT, built unchanged for x86_64"
    echo "Driver: i2c-ichsmb -i on QEMU $(MACHINE=${MACHINE:-q35}; echo "$MACHINE"), $(date '+%Y-%m-%d %H:%M')"
    echo
    bash "$BENCH" ssh /tmp/qnx_sample
} | tee "$ROOT/docs/results/qnx-sample.txt"
