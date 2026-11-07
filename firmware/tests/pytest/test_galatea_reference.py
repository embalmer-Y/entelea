# SPDX-License-Identifier: Apache-2.0
"""Check the pinned standalone oracle and reject stale or missing C fixtures."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
REFERENCE = ROOT / "lib/affect/tests/reference/galatea.py"
HEADER = ROOT / "lib/affect/tests/galatea_reference.h"


def run_reference(*args):
    return subprocess.run(
        [sys.executable, str(REFERENCE), *map(str, args)],
        cwd=ROOT, capture_output=True, text=True, check=False,
    )


def test_reference_generation_is_deterministic():
    first = run_reference()
    second = run_reference()
    assert first.returncode == second.returncode == 0, first.stderr + second.stderr
    assert first.stdout == second.stdout == HEADER.read_text(encoding="utf-8")


def test_checked_in_reference_matches_oracle():
    result = run_reference("--check", HEADER)
    assert result.returncode == 0, result.stderr
    assert "Galatea reference PASS" in result.stdout


def test_modified_reference_is_rejected(tmp_path):
    header = tmp_path / "changed.h"
    original = HEADER.read_text(encoding="utf-8")
    assert "0.578125f" in original
    header.write_text(original.replace("0.578125f", "0.6f"), encoding="utf-8")
    result = run_reference("--check", header)
    assert result.returncode == 1
    assert "MISMATCH" in result.stderr
    assert "PASS" not in result.stdout


def test_missing_reference_is_explicit_failure(tmp_path):
    result = run_reference("--check", tmp_path / "missing.h")
    assert result.returncode == 1
    assert "ERROR" in result.stderr
    assert "PASS" not in result.stdout
