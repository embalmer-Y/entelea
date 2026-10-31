# SPDX-License-Identifier: Apache-2.0
#!/usr/bin/env python3
"""补齐 hal_espressif blobs：只下缺失文件，经 ghfast.top 镜像，sha256 校验。"""
import hashlib
import os
import subprocess
import sys

BLOBS_ROOT = "/home/emb/project/zephyrproject/modules/hal/espressif/zephyr/blobs"
MODULE_YML = "/home/emb/project/zephyrproject/modules/hal/espressif/zephyr/module.yml"
MIRROR = "https://ghfast.top/"

import yaml

with open(MODULE_YML) as f:
    entries = yaml.safe_load(f)["blobs"]

missing = []
for e in entries:
    dest = os.path.join(BLOBS_ROOT, e["path"])
    ok = False
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        h = hashlib.sha256(open(dest, "rb").read()).hexdigest()
        ok = h == e["sha256"]
    if not ok:
        missing.append(e)

print(f"共 {len(entries)} 项，缺失/校验不符 {len(missing)} 项")
for e in missing:
    dest = os.path.join(BLOBS_ROOT, e["path"])
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    url = MIRROR + e["url"]
    tmp = dest + ".tmp"
    print(f"下载 {e['path']} ...", flush=True)
    r = subprocess.run(["wget", "-q", "--tries=8", "--timeout=60", "-O", tmp, url])
    if r.returncode != 0:
        print(f"  失败：wget rc={r.returncode}")
        sys.exit(1)
    h = hashlib.sha256(open(tmp, "rb").read()).hexdigest()
    if h != e["sha256"]:
        print(f"  sha256 不符：{h} != {e['sha256']}")
        os.unlink(tmp)
        sys.exit(1)
    os.rename(tmp, dest)
    print(f"  OK（sha256 验证通过）")
print("全部补齐")
