#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Change the bench's emulated sensors through QEMU's control socket (QMP).

  qmp.py set SENSOR CELSIUS       e.g. qmp.py set 0x48 92.5
  qmp.py get [SENSOR]             print one or all sensor temperatures
  qmp.py scenario FILE            play a temperature script, see below

SENSOR is an I2C address from $SENSORS (0x48) or a QMP id (temp0).

Scenario files have one step per line: seconds from the start, sensor,
temperature, and optionally a ramp time. With a ramp, the temperature moves
from where it is to the target in 0.5 s steps over that many seconds.

  # seconds  sensor  celsius  [ramp seconds]
  0          0x48    40
  5          0x48    95       10

Standard library only.
"""
import json
import os
import socket
import sys
import time

IMAGE = os.environ.get("IMAGE", os.path.expanduser("~/work/qnx-image"))
QMP = os.path.join(IMAGE, "output", "bench", "qmp.sock")
SENSORS = os.environ.get("SENSORS", "0x48 0x49").split()


class Qmp:
    def __init__(self, path=QMP):
        self.s = socket.socket(socket.AF_UNIX)
        try:
            self.s.connect(path)
        except OSError as e:
            sys.exit(f"qmp.py: can't reach QEMU at {path} ({e.strerror}). Is the bench up?")
        self.f = self.s.makefile("rw")
        self.f.readline()                       # greeting
        self.call("qmp_capabilities")

    def call(self, name, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "event" in r:
                continue
            if "error" in r:
                raise RuntimeError(r["error"].get("desc", str(r["error"])))
            return r.get("return")

    def get(self, qid):
        return self.call("qom-get", path=f"/machine/peripheral/{qid}", property="temperature") / 1000.0

    def set(self, qid, celsius):
        self.call("qom-set", path=f"/machine/peripheral/{qid}", property="temperature",
                  value=int(round(celsius * 1000)))


def sensor_id(name):
    if name.startswith("temp"):
        return name
    try:
        addr = int(name, 0)
    except ValueError:
        sys.exit(f"qmp.py: unknown sensor {name!r}")
    for i, a in enumerate(SENSORS):
        if int(a, 0) == addr:
            return f"temp{i}"
    sys.exit(f"qmp.py: no sensor at {name} (bench sensors: {' '.join(SENSORS) or 'none'})")


def label(qid):
    i = int(qid[4:])
    return SENSORS[i] if i < len(SENSORS) else qid


def scenario(q, path):
    steps = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            if len(line) not in (3, 4):
                sys.exit(f"{path}:{n}: want: seconds sensor celsius [ramp]")
            steps.append((float(line[0]), sensor_id(line[1]), float(line[2]),
                          float(line[3]) if len(line) == 4 else 0.0))
    t0 = time.monotonic()

    def at(t):
        d = t0 + t - time.monotonic()
        if d > 0:
            time.sleep(d)

    for t, qid, target, ramp in steps:
        at(t)
        if ramp > 0:
            start, n = q.get(qid), max(1, int(ramp / 0.5))
            for k in range(1, n + 1):
                at(t + k * ramp / n)
                q.set(qid, start + (target - start) * k / n)
        else:
            q.set(qid, target)
        print(f"{time.monotonic() - t0:7.1f} s  {label(qid)}  {target:.1f} C", flush=True)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    q = Qmp()
    cmd = argv[1]
    if cmd == "set" and len(argv) == 4:
        q.set(sensor_id(argv[2]), float(argv[3]))
    elif cmd == "get" and len(argv) in (2, 3):
        ids = [sensor_id(argv[2])] if len(argv) == 3 else [f"temp{i}" for i in range(len(SENSORS))]
        for qid in ids:
            print(f"{label(qid)}  {q.get(qid):.3f} C")
    elif cmd == "scenario" and len(argv) == 3:
        scenario(q, argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
