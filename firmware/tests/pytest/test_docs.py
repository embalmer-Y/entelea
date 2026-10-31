# SPDX-License-Identifier: Apache-2.0
"""Check relative links in maintained Entelea documentation."""

import re
from pathlib import Path
from urllib.parse import unquote, urlsplit

import pytest

ROOT = Path(__file__).resolve().parents[3]
LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


def local_links():
    files = [
        ROOT / "README.md", ROOT / "firmware/README.md", ROOT / "tools/board/README.md",
        *sorted((ROOT / "docs").glob("*.md")),
    ]
    for path in files:
        for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for target in LINK.findall(line):
                parsed = urlsplit(target.split()[0].strip("<>"))
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                dest = (path.parent / unquote(parsed.path)).resolve()
                yield path, line_no, target, dest


def test_documentation_relative_links():
    errors = [
        f"{path.relative_to(ROOT)}:{line_no}: {target}"
        for path, line_no, target, dest in local_links()
        if not dest.is_relative_to(ROOT / "agent") and not dest.exists()
    ]
    assert not errors, "\n".join(errors)


def test_forge_submodule_documentation_links():
    if not (ROOT / "agent/README.md").is_file():
        pytest.skip("Forge submodule is not initialized")
    errors = [
        f"{path.relative_to(ROOT)}:{line_no}: {target}"
        for path, line_no, target, dest in local_links()
        if dest.is_relative_to(ROOT / "agent") and not dest.exists()
    ]
    assert not errors, "\n".join(errors)
