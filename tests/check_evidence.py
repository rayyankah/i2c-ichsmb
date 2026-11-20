#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check docs/QNX-EVIDENCE.md against the code.

1. Every reference written as `NAME` (`file:line`) must point at a line
   that contains NAME.
2. Every QNX-specific call found in the C sources must be named somewhere
   in the document.

Exit status 0 when both hold. Needs only Python 3.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "QNX-EVIDENCE.md")
SOURCES = ["driver/qnx", "tools", "demo", "tests/target"]
# Names that only exist on QNX (kernel calls, QNX libraries, QNX constants).
QNX_NAMES = re.compile(
    r"\b(i2c_master_getfuncs|I2C_ADD_FUNC|I2C_CAPS_\w+|i2c_slogf|slog2_\w+|pci_device_\w+|pci_strerror"
    r"|Interrupt(?:AttachEvent|Unmask|Detach|Mask)|Channel(?:Create|Destroy)|Connect(?:Attach|Detach)"
    r"|MsgReceivePulse|MsgReceive|MsgSend\w*|TimerTimeout|ThreadCtl|mmap_device_io|procmgr_\w+"
    r"|set_ids_from_arg|DCMD_I2C_\w+|_NTO_\w+|_PULSE_CODE_COIDDEATH|SIGEV_PULSE_INIT|_syspage_ptr)\b")

def main():
    doc = open(DOC, encoding="utf-8").read()
    bad = 0
    refs = re.findall(r"`([^`]+?)(?:\(\))?` \(`([^`:]+):(\d+)`\)", doc)
    for name, path, line in refs:
        try:
            lines = open(os.path.join(ROOT, path), encoding="utf-8").read().split("\n")
        except OSError:
            print(f"missing file: {path}")
            bad += 1
            continue
        n = int(line)
        if n < 1 or n > len(lines) or name not in lines[n - 1]:
            near = [i + 1 for i, l in enumerate(lines) if name in l]
            print(f"{path}:{line} does not contain {name} (found on lines {near[:5]})")
            bad += 1

    used = {}
    for top in SOURCES:
        for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                path = os.path.join(dirpath, f)
                for i, l in enumerate(open(path, encoding="utf-8")):
                    code = l.split("//")[0]
                    if code.lstrip().startswith(("*", "/*")):
                        continue        # comment lines
                    for m in QNX_NAMES.finditer(code):
                        if m.group(1).endswith("_t"):
                            continue    # type names
                        used.setdefault(m.group(1), f"{os.path.relpath(path, ROOT)}:{i + 1}")
    for name, where in sorted(used.items()):
        if name not in doc:
            print(f"{name} is used ({where}) but not in docs/QNX-EVIDENCE.md")
            bad += 1

    print(f"{len(refs)} references, {len(used)} QNX names in the code, {bad} problems")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
