#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
On-target tests for i2c-ichsmb.

Boots QNX in QEMU (bench/qemu/run-qnx.sh), copies the driver and the test
client to the guest, then runs every case in tests/cases/*.json:

  - restarts the driver with the options of the case's mode when the mode
    changes,
  - applies the case's sensor settings over QMP,
  - runs tests/target/t_xfer (or a shell command) over SSH,
  - compares each output line with the expected regular expression.

Prints a pass/fail table and exits 1 if any case failed.

  tests/run_tests.py [--machine q35|pc] [--keep] [--only SUBSTRING]

Needs the QNX environment (qnxsdp-env.sh) for building, and the QEMU from
bench/qemu/build-qemu.sh.
"""
import argparse
import glob
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench", "qemu", "run-qnx.sh")
IMAGE = os.environ.get("IMAGE", os.path.expanduser("~/work/qnx-image"))
QMP = os.path.join(IMAGE, "output", "bench", "qmp.sock")

# Driver options for each mode a case can ask for.
MODES = {
    "poll":  "",
    "irq":   "-i",
    "smbus": "-s",
    "user":  "-i -U qnxuser",
    "none":  None,                      # driver not running
}


def bench(*args, check=True, capture=True, timeout=120):
    env = dict(os.environ, MACHINE=ARGS.machine)
    r = subprocess.run(["bash", BENCH, *args], env=env, timeout=timeout,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.STDOUT if capture else None, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"run-qnx.sh {' '.join(args)} failed:\n{r.stdout}")
    return r


def guest(cmd, timeout=600):
    r = bench("ssh", cmd, check=False, timeout=timeout)
    return r.returncode, r.stdout


def qmp(commands):
    s = socket.socket(socket.AF_UNIX)
    s.connect(QMP)
    f = s.makefile("rw")

    def call(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a} if a else {"execute": name}) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "event" not in r:
                return r

    f.readline()
    call("qmp_capabilities")
    for c in commands:
        if "temp" in c:
            call("qom-set", path="/machine/peripheral/temp0", property="temperature", value=c["temp"])
    s.close()


def build():
    for d in ("driver/qnx", "tests/target", "tools/smbctl"):
        r = subprocess.run(["make", "-s", "-C", os.path.join(ROOT, d)], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            sys.exit(f"build of {d} failed:\n{r.stdout}")


def boot():
    bench("stop", check=False)
    bench("start")
    for _ in range(60):
        if guest("true")[0] == 0:
            return
        time.sleep(1)
    sys.exit("guest did not come up:\n" + bench("log", check=False).stdout[-2000:])


def deploy():
    files = [os.path.join(ROOT, p) for p in
             ("driver/qnx/i2c-ichsmb", "tests/target/t_xfer", "tools/smbctl/smbctl")]
    guest("mkdir -p /tmp/new")
    bench("scp", *files, "root@127.0.0.1:/tmp/new/")
    guest("slay -f i2c-ichsmb >/dev/null 2>&1; for f in /tmp/new/*; do mv -f $f /tmp/; done")


def start_driver(mode):
    opts = MODES[mode]
    # Stop any running copy and wait until /dev/i2c0 has gone.
    guest("slay -f i2c-ichsmb >/dev/null 2>&1; "
          "n=0; while [ -e /dev/i2c0 ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done")
    if opts is None:
        return True, ""
    rc, out = guest(f"/tmp/i2c-ichsmb {opts} </dev/null >/tmp/drv.out 2>&1; echo rc=$?; cat /tmp/drv.out; "
                    "n=0; while [ ! -e /dev/i2c0 ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done")
    return "rc=0" in out, out.strip()


def load_cases():
    cases = []
    for path in sorted(glob.glob(os.path.join(ROOT, "tests", "cases", "*.json"))):
        with open(path) as f:
            for c in json.load(f):
                c["file"] = os.path.basename(path)
                cases.append(c)
    return cases


def expected_for(case):
    return case.get("expect_" + ARGS.machine, case.get("expect"))


def run_case(case):
    if "qmp" in case:
        qmp(case["qmp"])
    if "shell" in case:
        rc, out = guest(case["shell"])
    else:
        rc, out = guest("/tmp/t_xfer " + " ".join(case["ops"]))
    lines = [l for l in out.splitlines() if l.strip()]
    want = expected_for(case)
    ok = len(lines) == len(want) and all(re.fullmatch(w, l) for w, l in zip(want, lines))
    return ok, lines, want


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument("--machine", default="q35", choices=["q35", "pc"])
    p.add_argument("--keep", action="store_true", help="leave the guest running")
    p.add_argument("--only", default="", help="run cases whose name contains this")
    p.add_argument("--no-boot", action="store_true", help="use the running guest")
    ARGS = p.parse_args()

    cases = [c for c in load_cases()
             if ARGS.only in c["name"] and ARGS.machine in c.get("machines", ["q35", "pc"])]
    build()
    if not ARGS.no_boot:
        boot()
    deploy()

    results, mode = [], None
    t0 = time.time()
    for c in cases:
        if c.get("mode", "poll") != mode or c.get("restart"):
            mode = c.get("mode", "poll")
            started, msg = start_driver(mode)
            if not started:
                results.append((c, False, [msg], ["driver start"]))
                mode = None
                continue
        ok, got, want = run_case(c)
        results.append((c, ok, got, want))

    width = max(len(c["name"]) for c, *_ in results)
    print(f"i2c-ichsmb on-target tests, QEMU machine {ARGS.machine}, {time.strftime('%Y-%m-%d %H:%M')}")
    print()
    for c, ok, got, want in results:
        print(f"{'PASS' if ok else 'FAIL'}  {c['name']:<{width}}  [{c.get('mode', 'poll')}]")
        if not ok:
            for i in range(max(len(got), len(want))):
                g = got[i] if i < len(got) else "(nothing)"
                w = want[i] if i < len(want) else "(nothing)"
                print(f"        got  {g}\n        want {w}")
        elif c.get("show"):
            for line in got:
                print(f"        {line}")
    failed = sum(1 for _, ok, *_ in results if not ok)
    print()
    print(f"{len(results)} cases, {len(results) - failed} passed, {failed} failed, {time.time() - t0:.0f} s")
    if not ARGS.keep:
        bench("stop", check=False)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
