#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Kill the driver repeatedly under smb-supervisor while thermd runs, and save
# what both saw to docs/results/recovery.txt.
#
#   bench/recovery.sh [KILLS]       (default 20, one every 2 s)
#
# Pass: every kill is followed by one restart and one gap in thermd's data,
# and thermd misses no deadline.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
source "$HERE/config.env"
B="$HERE/bench"
N=${1:-20}
OUT="$ROOT/docs/results/recovery.txt"

bash "$B" ssh "slay -f smb-supervisor >/dev/null 2>&1; slay -f thermd >/dev/null 2>&1; slay -f i2c-ichsmb >/dev/null 2>&1; sleep 0.3
    nohup /tmp/smb-supervisor -- /tmp/i2c-ichsmb $DRIVER_OPTS </dev/null >/tmp/sup.out 2>&1 &
    n=0; while [ ! -e /dev/i2c0 ] && [ \$n -lt 50 ]; do sleep 0.1; n=\$((n+1)); done
    nohup /tmp/thermd -q -t $((2 * N + 4)) </dev/null >/tmp/th.out 2>&1 &
    sleep 2
    for i in \$(seq $N); do slay -s KILL -f i2c-ichsmb; sleep 2; done
    sleep 4; slay -f smb-supervisor"
{
    echo "smb-supervisor recovery: $N x kill -9 of i2c-ichsmb $DRIVER_OPTS, one every 2 s, thermd running"
    echo "QEMU $MACHINE (emulated time), $(date '+%Y-%m-%d %H:%M')"
    echo
    bash "$B" ssh 'cat /tmp/sup.out; echo; cat /tmp/th.out'
} > "$OUT"
restarts=$(grep -c "serving again" "$OUT" || true)
gaps=$(grep -o "gaps=[0-9]*" "$OUT" | cut -d= -f2)
missed=$(grep -o "missed=[0-9]*" "$OUT" | cut -d= -f2)
echo "kills $N, restarts $restarts, thermd gaps $gaps, missed deadlines $missed"
grep "serving again" "$OUT" | sed 's/.*again, \([0-9]*\) ms.*/\1/' | sort -n | awk '{a[NR]=$1} END {print "restart time ms: min " a[1] ", median " a[int((NR+1)/2)] ", max " a[NR]}'
grep "ms without data:" "$OUT" | sed 's/.*data:\(.*\))/\1/'| tr ' ' '\n' | grep . | sort -n | awk '{a[NR]=$1} END {print "thermd gap ms:    min " a[1] ", median " a[int((NR+1)/2)] ", max " a[NR]}'
bash "$B" driver >/dev/null
[ "$restarts" = "$N" ] && [ "$gaps" = "$N" ] && [ "$missed" = 0 ]
