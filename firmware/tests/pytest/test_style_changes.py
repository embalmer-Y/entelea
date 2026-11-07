# SPDX-License-Identifier: Apache-2.0
"""Only unchanged renames may bypass changed-file style checks."""

import importlib.util
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

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


def test_style_command_keeps_kernel_format_checks(monkeypatch):
    checker = ROOT / ".deps/zephyr/scripts/checkpatch.pl"
    if not checker.is_file():
        pytest.skip("Pinned Zephyr source not installed")
    commands = []
    monkeypatch.setattr(style.sys, "argv", ["check_style.py", "baseline"])
    monkeypatch.setattr(
        style.subprocess, "check_output",
        lambda command, **kwargs: b"M\0sample.c\0" if "diff" in command else b"",
    )

    def run(command, **kwargs):
        commands.append(command)
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(style.subprocess, "run", run)
    assert style.main() == 0
    assert "--dry-run" in commands[0] and "--Werror" in commands[0]
    assert "--strict" in commands[1] and "--max-line-length=100" in commands[1]
    ignored = commands[1][commands[1].index("--ignore") + 1]
    assert set(ignored.split(",")) == {"PREFER_KERNEL_TYPES", "PRINTK_WITHOUT_KERN_LEVEL"}


@pytest.mark.parametrize("bad_format", [False, True])
def test_zephyr_api_exception_does_not_accept_bad_formatting(tmp_path, bad_format):
    checker = ROOT / ".deps/zephyr/scripts/checkpatch.pl"
    if not checker.is_file():
        pytest.skip("Pinned Zephyr source not installed")
    source = tmp_path / "sample.c"
    source.write_text(
        "// SPDX-License-Identifier: Apache-2.0\n"
        "#include <stdint.h>\n\n"
        "void show_value(uint32_t value)\n"
        "{\n"
        + ('    printk("%u\\n", value);\n' if bad_format else '\tprintk("%u\\n", value);\n')
        + "}\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        ["perl", str(checker), "--no-tree", "--no-signoff", "--strict",
         "--max-line-length=100", "--ignore", ",".join(style.ZEPHYR_CHECKPATCH_EXCEPTIONS),
         "--file", str(source)],
        capture_output=True, text=True, timeout=10, cwd=ROOT,
    )
    assert (result.returncode != 0) == bad_format, result.stdout + result.stderr
