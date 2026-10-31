# SPDX-License-Identifier: Apache-2.0
"""Check tracked source, not build caches or the Forge submodule."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXT = {".md", ".c", ".h", ".py", ".yml", ".yaml", ".toml", ".conf", ".sh"}
FORBIDDEN = {".venv", ".deps", "__pycache__", ".pytest_cache", ".ruff_cache"}


def main():
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT
    ).decode().split("\0")
    errors = []
    for name in sorted(set(names) - {""}):
        path = ROOT / name
        if not path.is_file():
            continue
        if FORBIDDEN.intersection(path.relative_to(ROOT).parts):
            errors.append(f"{name}: runtime artifact")
        if path.suffix in TEXT:
            data = path.read_bytes()
            if data.startswith(b"\xef\xbb\xbf"):
                errors.append(f"{name}: UTF-8 BOM")
            try:
                data.decode("utf-8")
            except UnicodeDecodeError:
                errors.append(f"{name}: invalid UTF-8")
    instructions = (ROOT / "AGENTS.md").read_bytes()
    if len(instructions) > 8192 or len(instructions.splitlines()) > 100:
        errors.append("AGENTS.md exceeds the 8 KiB / 100 line budget")
    for error in errors:
        print(error, file=sys.stderr)
    if not errors:
        print("Repository hygiene PASS")
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
