# SPDX-License-Identifier: Apache-2.0
"""Enforce Linux-style formatting on changed, first-party C files."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "import-baseline"
    names = subprocess.check_output(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", base, "--", "*.c", "*.h"],
        cwd=ROOT,
    ).decode().splitlines()
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
        if Path(path) == ROOT / "lib/affect/tests/main.c":
            # Unity mandates setUp/tearDown and its public result field names.
            command += ["--ignore", "CAMELCASE"]
        result |= subprocess.run([*command, path], cwd=ROOT).returncode
    return result


if __name__ == "__main__":
    sys.exit(main())
