# SPDX-License-Identifier: Apache-2.0
"""Entelea setup regressions and retained zenoh TLS patch integrity."""
from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
ENV_DIR = REPO / "scripts"


def _sh_files() -> list[Path]:
    return sorted(ENV_DIR.rglob("*.sh"))


def test_shell_syntax_all():
    files = _sh_files()
    assert {p.name for p in files} == {"setup-firmware.sh", "firmware.sh"}
    for f in files:
        r = subprocess.run(["bash", "-n", str(f)], capture_output=True, text=True)
        assert r.returncode == 0, f"{f}: {r.stderr}"


def test_setup_uses_immutable_revisions():
    src = (ENV_DIR / "setup-firmware.sh").read_text(encoding="utf-8")
    pins = re.findall(r'^\s+([0-9a-f]{40}) "\$ROOT/\.deps/[^"]+"', src, re.M)
    assert len(pins) == 2
    assert "sudo" not in src
    assert "reset --hard" not in src


def test_missing_sources_fail_explicitly(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    shutil.copyfile(ENV_DIR / "firmware.sh", scripts / "firmware.sh")
    result = subprocess.run(
        ["bash", str(scripts / "firmware.sh"), "build"],
        capture_output=True, text=True, timeout=10,
        env={"PATH": "/usr/bin:/bin", "HOME": str(tmp_path)},
    )
    assert result.returncode != 0
    assert "Missing source:" in result.stderr


def test_zenoh_pico_patch_diff_counts_selfconsistent():
    """统一 diff 逐 hunk 计数自检（计数/头行缺陷 = git apply 全拒——2026-10-24 实发并修复）。"""
    patch = (REPO / "firmware/module/tessera/ext/zenoh-pico-tls.patch").read_text(
        encoding="utf-8").splitlines()
    files: list[str] = []
    hunk = None
    old_n = new_n = 0
    # 文件段起点两种形态：diff --git 头（git 风格）或裸 --- a/ 头（本补丁首段 config.h 即此形态）

    def _close_hunk():
        assert hunk is not None and old_n == hunk[0] and new_n == hunk[1], (
            f"hunk 计数不实：头 @{hunk} 实际 old={old_n} new={new_n}")

    for ln in patch:
        if ln.startswith("diff --git "):
            if hunk:
                _close_hunk()
                hunk = None
            files.append(ln.split(" b/", 1)[0].split(" a/", 1)[1])
        elif ln.startswith("--- a/") and hunk is None and not files:
            files.append(ln[6:])  # 首段裸 --- 形态（config.h）——其余段 --- 跟随 diff --git 不重复计
        elif ln.startswith("@@ "):
            if hunk:
                _close_hunk()
            m = re.match(r"@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@", ln)
            assert m, f"hunk 头格式异常：{ln}"
            hunk = (int(m.group(1) or 1), int(m.group(2) or 1))
            old_n = new_n = 0
        elif hunk is not None:
            if ln.startswith("+++") or ln.startswith("---"):
                continue
            if ln.startswith("+"):
                new_n += 1
            elif ln.startswith("-"):
                old_n += 1
            elif ln.startswith(" ") or ln == "":
                old_n += 1
                new_n += 1
            else:
                raise AssertionError(f"补丁体异常行：{ln!r}")
    if hunk:
        _close_hunk()
    assert len(files) == len(set(files)) and len(files) >= 9, (
        f"TLS 补丁应 ≥9 文件（现行 {len(files)}）")
    # 每个文件段须有 ---/+++ 头（git apply 严格域——缺 --- 行 = 拒收）
    body = "\n".join(patch)
    for f in files:
        assert f"--- a/{f}\n" in body, f"{f} 段缺 --- a/ 头行"


def test_naming_convention_compliance():
    for p in ENV_DIR.rglob("*"):
        if p.is_file():
            stem = p.stem
            assert not re.fullmatch(r"[a-zA-Z]\d+(\.\d+)?", stem), f"命名违例：{p.name}"
    for d in ENV_DIR.iterdir():
        if d.is_dir():
            assert not re.fullmatch(r"[a-zA-Z]\d+", d.name), f"目录命名违例：{d.name}"
