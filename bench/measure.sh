#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# thermd timing runs on the bench, saved to docs/results/thermd-*.txt.
#
#   bench/measure.sh [SECONDS]      (default 60)
#
# Each run: the driver in the given mode, thermd sampling both sensors every
# 10 ms, and from the host the 0x48 sensor jumps to 91 C a third of the way in
# and back to 40 C at two thirds. With hog, hog spins on every processor at
# priority 20 for the whole run, starting a second after thermd.
#
#   baseline      thermd 30, no load, interrupt mode
#   hog-irq       thermd 30 over hog 20, interrupt mode
#   hog-poll      thermd 30 over hog 20, polling mode
#   hog-low       thermd 10 under hog 20, interrupt mode
#
# While hog runs, nothing at priority 20 or below gets the processor, sshd
# included, so the run is started in the background and collected after.
# All times are measured inside QEMU (KVM under WSL2): they are emulated.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
source "$HERE/config.env"
B="$HERE/bench"
T=${1:-60}
OUT="$ROOT/docs/results"
mkdir -p "$OUT"

run() {  # name thermd_priority driver_options with_hog
    local name=$1 prio=$2 opts=$3 hog=$4 load=""
    bash "$B" driver "$opts" >/dev/null
    bash "$B" heat 0x48 40 >/dev/null
    bash "$B" heat 0x49 40 >/dev/null
    [ "$hog" = yes ] && load="sleep 1; /tmp/hog -p 20 -t $((T - 2)) >/tmp/hog.out 2>&1"
    bash "$B" ssh "rm -f /tmp/th.out /tmp/hog.out
        nohup sh -c '/tmp/thermd -P $prio -t $T >/tmp/th.out 2>&1 & $load' </dev/null >/dev/null 2>&1 &"
    sleep $((T / 3)); bash "$B" heat 0x48 91 >/dev/null
    sleep $((T / 3)); bash "$B" heat 0x48 40 >/dev/null
    sleep $((T - 2 * (T / 3) + 5))
    {
        echo "thermd run '$name': thermd priority $prio, driver options '$opts', hog at 20: $hog"
        echo "QEMU $MACHINE (emulated time), $(date '+%Y-%m-%d %H:%M'); 0x48 set to 91 C at $((T / 3)) s, 40 C at $((2 * (T / 3))) s"
        echo
        bash "$B" ssh 'cat /tmp/th.out; [ -f /tmp/hog.out ] && cat /tmp/hog.out; true'
    } > "$OUT/thermd-$name.txt"
    grep RESULT "$OUT/thermd-$name.txt" | sed "s/^/$name: /"
}

run baseline 30 "${DRIVER_OPTS}" no
run hog-irq  30 "${DRIVER_OPTS}" yes
run hog-poll 30 ""               yes
run hog-low  10 "${DRIVER_OPTS}" yes
bash "$B" driver >/dev/null
