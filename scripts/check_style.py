# SPDX-License-Identifier: Apache-2.0
"""Enforce Linux-style formatting on changed, first-party C files."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZEPHYR_CHECKPATCH_EXCEPTIONS = ("PREFER_KERNEL_TYPES", "PRINTK_WITHOUT_KERN_LEVEL")


def changed_paths(data):
    fields = iter(data.decode().split("\0"))
    paths = []
    for status in fields:
        if not status:
            break
        path = next(fields)
        if status.startswith(("R", "C")):
            path = next(fields)
            if status == "R100":
                continue
        paths.append(path)
    return paths


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "import-baseline"
    names = changed_paths(subprocess.check_output(
        ["git", "diff", "--name-status", "-z", "--find-renames",
         "--diff-filter=ACMR", base, "--", "*.c", "*.h"],
        cwd=ROOT,
    ))
    names += subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "--", "*.c", "*.h"], cwd=ROOT
    ).decode().splitlines()
    paths = [
        str(ROOT / name) for name in sorted(set(names))
        if "/appmgr/crypto/" not in name and not name.endswith("_wasm.h")
    ]
    if not paths:
        print("No changed first-party C/H files")
        return 0
    result = subprocess.run(
        [str(ROOT / ".venv/bin/clang-format"), "--dry-run", "--Werror", *paths], cwd=ROOT
    ).returncode
    if result:
        return result
    checker = ROOT / ".deps/zephyr/scripts/checkpatch.pl"
    if not checker.is_file():
        print("Run bash scripts/setup-firmware.sh before kernel style checks", file=sys.stderr)
        return 1
    for path in paths:
        command = [
            "perl", str(checker), "--no-tree", "--no-signoff", "--strict",
            "--max-line-length=100", "--file",
        ]
        exceptions = list(ZEPHYR_CHECKPATCH_EXCEPTIONS)
        if Path(path) == ROOT / "lib/affect/tests/main.c":
            # Unity mandates setUp/tearDown and its public result field names.
            exceptions.append("CAMELCASE")
        command += ["--ignore", ",".join(exceptions)]
        result |= subprocess.run([*command, path], cwd=ROOT).returncode
    return result


if __name__ == "__main__":
    sys.exit(main())
