#!/usr/bin/env python3
"""Checks that every declaration fixed in 06-implementatieplan-v1.md par. 3 is present in the RDM headers.

usage: check-headers.py [header_dir]   (default: src/helpers/rdm)
Workers may add private members; any change to the fixed public API shows up as a missing declaration.
"""
import glob
import os
import re
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
hdr_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo, "src/helpers/rdm")
doc = open(os.path.join(repo, "docs/reliable-dm/06-implementatieplan-v1.md"), encoding="utf-8").read()


def norm(text):
    return re.sub(r"\s+", " ", re.sub(r"//.*", "", text)).strip()


headers = norm(" ".join(open(f, encoding="utf-8").read() for f in glob.glob(os.path.join(hdr_dir, "*.h"))))
missing = []
for start, end in (("## 3. Vastgelegde interfaces", "### 3.13"), ("### 3.15", "### 3.16")):
    section = doc[doc.find(start):doc.find(end)]
    for block in re.findall(r"```cpp\n(.*?)```", section, re.S):
        for stmt in re.split(r"(?<=[;{}])", norm(block)):
            stmt = stmt.strip()
            if len(stmt) >= 8 and stmt not in ("};", "}") and stmt not in headers:
                missing.append(stmt)

for stmt in missing:
    print("missing:", stmt)
print(f"check-headers: {len(missing)} fixed declarations missing in {hdr_dir}")
sys.exit(1 if missing else 0)
