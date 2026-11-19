#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Boot the QNX 8.0 image made by mkqnximage on QEMU's q35 machine, which has
# the ICH9 SMBus controller (mkqnximage's own runner uses the default "pc"
# machine, which only has the older PIIX4 one; MACHINE=pc selects that).
#
#   bench/qemu/run-qnx.sh start   boot in the background
#   bench/qemu/run-qnx.sh stop    power off
#   bench/qemu/run-qnx.sh ssh     log in (root, key installed by mkqnximage)
#
# On the SMBus: tmp105 temperature sensors at the addresses in SENSORS
# (QMP ids temp0, temp1, ...), plus the eight memory-module EEPROMs QEMU
# always puts at 0x50..0x57.
#
# Networking is QEMU user mode: no bridge and no root needed. Guest SSH is
# forwarded to 127.0.0.1:$SSH_PORT. Serial console goes to a log file, never
# to this terminal (the guest writes terminal control codes).
#
# Environment:
#   IMAGE     mkqnximage directory      (default ~/work/qnx-image)
#   QEMU      QEMU with sensor models   (default ~/work/qemu/bin/qemu-system-x86_64,
#                                        built by bench/qemu/build-qemu.sh)
#   SSH_PORT  host port for guest SSH   (default 2222)
#   RAM, CPUS                           (default 1G, 2)
#   MACHINE   q35 (ICH9 SMBus, default) or pc (PIIX4 SMBus)
#   SENSORS   tmp105 addresses          (default "0x48 0x49"; "" for none)
set -u
MACHINE=${MACHINE:-q35}
IMAGE=${IMAGE:-$HOME/work/qnx-image}
QEMU=${QEMU:-$HOME/work/qemu/bin/qemu-system-x86_64}
SSH_PORT=${SSH_PORT:-2222}
RAM=${RAM:-1G}
CPUS=${CPUS:-2}
SENSORS=${SENSORS-0x48 0x49}
RUN=$IMAGE/output/bench
QMP=$RUN/qmp.sock
SSH_KEY=${SSH_KEY:-$HOME/.ssh/id_ed25519}
SSH_OPTS=(-p "$SSH_PORT" -i "$SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5)

start() {
    [ -f "$IMAGE/output/ifs.bin" ] || { echo "no image in $IMAGE (run mkqnximage first)" >&2; exit 2; }
    mkdir -p "$RUN"
    if [ -f "$RUN/qemu.pid" ] && kill -0 "$(cat "$RUN/qemu.pid")" 2>/dev/null; then
        echo "already running (pid $(cat "$RUN/qemu.pid"))"; return 0
    fi
    accel=(-cpu max)
    [ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
    if [ "$MACHINE" = q35 ]; then
        # q35 puts if=ide disks on AHCI, which devb-eide can't drive: add a PIIX4 IDE controller
        disk=(-device piix4-ide,id=pide
              -drive file="$IMAGE/output/disk-qemu.vmdk",if=none,id=drv0
              -device ide-hd,drive=drv0,bus=pide.0)
    else
        disk=(-drive file="$IMAGE/output/disk-qemu.vmdk",if=ide)
    fi
    sensors=() i=0
    for a in $SENSORS; do sensors+=(-device "tmp105,address=$a,id=temp$i"); i=$((i + 1)); done
    rm -f "$QMP" "$RUN/serial.raw"
    "$QEMU" -machine "$MACHINE" "${accel[@]}" -smp "$CPUS" -m "$RAM" \
        -kernel "$IMAGE/output/ifs.bin" \
        "${disk[@]}" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" \
        -device virtio-net-pci,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
        "${sensors[@]}" \
        -qmp "unix:$QMP,server=on,wait=off" \
        -serial "file:$RUN/serial.raw" -display none \
        -pidfile "$RUN/qemu.pid" -daemonize
    echo "started; console log: $RUN/serial.log"
}

log() {
    [ -f "$RUN/serial.raw" ] || return 0
    sed -e 's/\x1b\[[0-9;?=<>]*[ -\/]*[@-~]//g' "$RUN/serial.raw" | tr -cd '\11\12\40-\176' > "$RUN/serial.log"
    cat "$RUN/serial.log"
}

case "${1:-}" in
    start) start ;;
    stop)
        if [ -f "$RUN/qemu.pid" ]; then kill "$(cat "$RUN/qemu.pid")" 2>/dev/null; rm -f "$RUN/qemu.pid"; fi ;;
    log)   log ;;
    ssh)   shift; ssh "${SSH_OPTS[@]}" root@127.0.0.1 "$@" ;;
    scp)   shift; scp -P "$SSH_PORT" -i "$SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$@" ;;
    *)     echo "usage: $0 start|stop|log|ssh [cmd]|scp src root@127.0.0.1:dst" >&2; exit 2 ;;
esac
