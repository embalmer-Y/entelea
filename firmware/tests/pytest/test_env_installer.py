# SPDX-License-Identifier: Apache-2.0
"""tools/env 安装程序套件守恒测试（DEC-79 · V0.16.0）。

覆盖：① 全部 shell 语法；② 步骤编号有序且 install.sh 引用齐全；③ 钉版常量与
预期字面量一致（改版本须三处同步：env_common/README 依赖表/本测试）；④ zenoh-pico
TLS 补丁统一 diff 计数自洽（2026-10-24 曾有两处计数/头行潜伏缺陷——本检查防复发）；
⑤ --help 契约与命名规范合规。
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
ENV_DIR = REPO / "tools" / "env"

EXPECTED_PINS = {
    "PIN_ZEPHYR_REF": "v4.5.0",
    "PIN_SDK_VER": "1.0.1",
    "PIN_WAMR_REF": "WAMR-2.4.5",
    "PIN_ZENOH_PICO_REF": "1.10.1",
    "PIN_ESPTOOL_VER": "5.4.0",
    "PIN_ZENOHD_VER": "1.10.1",
    "PIN_LLVM_VER": "18.1.8",
    "PIN_CMAKE_VER": "3.31.6",
    "PIN_PY_VENV": "3.12",
    "PIN_DEFAULT_MODEL": "anthropic:MiniMax-M3",
}


def _sh_files() -> list[Path]:
    return sorted(ENV_DIR.rglob("*.sh"))


def test_shell_syntax_all():
    files = _sh_files()
    assert len(files) >= 12, "安装套件应 ≥12 个 shell 文件"
    for f in files:
        r = subprocess.run(["bash", "-n", str(f)], capture_output=True, text=True)
        assert r.returncode == 0, f"{f}: {r.stderr}"


def test_steps_ordered_and_referenced():
    steps = sorted(p.name for p in (ENV_DIR / "steps").glob("*.sh"))
    nums = [int(s.split("-")[0]) for s in steps]
    assert nums == sorted(nums) and len(set(nums)) == len(nums), "步骤编号须有序唯一"
    installer = (ENV_DIR / "install.sh").read_text(encoding="utf-8")
    for s in steps:
        assert s[:-3] in installer, f"install.sh 未引用步骤 {s}"


def test_pins_match_expected():
    src = (ENV_DIR / "env_common.sh").read_text(encoding="utf-8")
    pins = dict(re.findall(r'^readonly (PIN_[A-Z_]+)="([^"]+)"', src, re.M))
    assert pins == EXPECTED_PINS, "钉版漂移：改版本须 env_common/README 依赖表/本测试三处同步"
    # README 依赖表对齐抽查
    readme = (REPO / "README.md").read_text(encoding="utf-8")
    for pin in ("v4.5.0", "1.0.1", "2.4.5", "1.10.1"):
        assert pin in readme, f"README 依赖与钉版表缺少 {pin}"


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
    assert len(files) == len(set(files)) and len(files) >= 9, f"TLS 补丁应 ≥9 文件（现行 {len(files)}）"
    # 每个文件段须有 ---/+++ 头（git apply 严格域——缺 --- 行 = 拒收）
    body = "\n".join(patch)
    for f in files:
        assert f"--- a/{f}\n" in body, f"{f} 段缺 --- a/ 头行"


def test_installer_help_contract():
    r = subprocess.run(
        ["bash", str(ENV_DIR / "install.sh"), "--help"],
        capture_output=True, text=True, timeout=30,
        env={"PATH": "/usr/bin:/bin", "HOME": str(REPO)},  # 隔离环境——防副作用
    )
    assert r.returncode == 0 and "--dev" in r.stdout and "--config" in r.stdout


def test_naming_convention_compliance():
    for p in ENV_DIR.rglob("*"):
        if p.is_file():
            stem = p.stem
            assert not re.fullmatch(r"[a-zA-Z]\d+(\.\d+)?", stem), f"命名违例：{p.name}"
    for d in ENV_DIR.iterdir():
        if d.is_dir():
            assert not re.fullmatch(r"[a-zA-Z]\d+", d.name), f"目录命名违例：{d.name}"
