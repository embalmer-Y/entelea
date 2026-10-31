# SPDX-License-Identifier: Apache-2.0
"""Only unchanged renames may bypass changed-file style checks."""

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("check_style", ROOT / "scripts/check_style.py")
style = importlib.util.module_from_spec(spec)
spec.loader.exec_module(style)


def test_changed_paths_excludes_only_unmodified_rename():
    data = (
        b"R100\0old.c\0moved.c\0R099\0prior.c\0edited.c\0"
        b"M\0modified.h\0A\0new.c\0C100\0source.c\0copied.c\0"
    )
    assert style.changed_paths(data) == ["edited.c", "modified.h", "new.c", "copied.c"]
