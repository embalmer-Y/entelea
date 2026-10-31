# SPDX-License-Identifier: Apache-2.0
"""Exercise the repository hygiene gate without touching a real checkout."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]


@pytest.fixture
def workspace(tmp_path):
    subprocess.run(["git", "init", "--quiet", str(tmp_path)], check=True)
    (tmp_path / "scripts").mkdir()
    shutil.copyfile(REPO / "scripts/check_repo.py", tmp_path / "scripts/check_repo.py")
    (tmp_path / "AGENTS.md").write_text("# Instructions\n", encoding="utf-8")
    (tmp_path / ".gitignore").write_text(".deps/\n", encoding="utf-8")
    return tmp_path


def check(workspace):
    return subprocess.run(
        [sys.executable, str(workspace / "scripts/check_repo.py")],
        capture_output=True, text=True, timeout=10,
    )


@pytest.mark.parametrize(
    ("contents", "expected"),
    [(b"a" * 8192, 0), (b"a" * 8193, 1), (b"\n" * 100, 0), (b"\n" * 101, 1)],
)
def test_instruction_budget(workspace, contents, expected):
    (workspace / "AGENTS.md").write_bytes(contents)
    result = check(workspace)
    assert result.returncode == expected, result.stderr
    if expected:
        assert "budget" in result.stderr


@pytest.mark.parametrize(
    ("contents", "message"),
    [(b"\xef\xbb\xbftext", "UTF-8 BOM"), (b"\xff", "invalid UTF-8")],
)
def test_invalid_text_rejected(workspace, contents, message):
    (workspace / "source.py").write_bytes(contents)
    result = check(workspace)
    assert result.returncode == 1
    assert message in result.stderr


def test_ignored_dependencies_not_scanned_but_tracked_artifacts_rejected(workspace):
    cache = workspace / ".deps"
    cache.mkdir()
    artifact = cache / "upstream.py"
    artifact.write_bytes(b"\xff")
    result = check(workspace)
    assert result.returncode == 0, result.stderr
    subprocess.run(["git", "add", "--force", str(artifact)], cwd=workspace, check=True)
    result = check(workspace)
    assert result.returncode == 1
    assert "runtime artifact" in result.stderr
