# SPDX-License-Identifier: Apache-2.0
"""G3 测试债：L5 机械检查器自测（check_1/2 合成夹具 + 仓级烟测）。

夹具须位于仓内（检查器报文用 relative_to(REPO)——仓外路径即崩，
其本身即隐含契约：被检面 = 仓内源码树）。
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CHECK = REPO / "firmware" / "tests" / "safety_check" / "check_safety_contract.py"

sys.path.insert(0, str(CHECK.parent))
import check_safety_contract  # noqa: E402


def _fixture(tmp: Path, name: str, text: str) -> Path:
    f = tmp / name
    f.write_text(text, encoding="utf-8")
    return f


def test_check1_output_driver_outside_dispatch(tmp_path: Path) -> None:
    # tmp_path 在仓外——relative_to(REPO) 崩；在仓内建夹具目录
    root = REPO / ".tmp-l5-self"
    root.mkdir(exist_ok=True)
    try:
        bad_f = _fixture(root, "violate.c", "int f(void) { gpio_pin_set_raw(d, p, 1); }\n")
        violations = check_safety_contract.check_1_write_path([bad_f])
        assert len(violations) == 1 and "[1]" in violations[0]
        assert "gpio_pin_set_raw" in violations[0]
        # 干净文件零违规
        clean = _fixture(root, "clean.c", "int g(void) { return 0; }\n")
        assert check_safety_contract.check_1_write_path([clean]) == []
        # 白名单文件（dispatch 本体）零违规
        assert check_safety_contract.check_1_write_path([check_safety_contract.DISPATCH_FILE]) == []
    finally:
        for f in root.iterdir():
            f.unlink()
        root.rmdir()


def test_check2_three_states_complete(tmp_path: Path) -> None:
    root = REPO / ".tmp-l5-self"
    root.mkdir(exist_ok=True)
    try:
        bad = _fixture(root, "ch.c",
                       "static const ts_out_ch_t x = {\n\t.uid=\"u\",\n};\n")
        v = check_safety_contract.check_2_three_states([bad])
        assert len(v) == 3  # poweron/linkloss/fault 各一
        good = _fixture(root, "ch2.c",
                        "static const ts_out_ch_t y = {\n\t.uid=\"u\","
                        "\n\t.poweron={0}, .linkloss={0}, .fault={0},\n};\n")
        assert check_safety_contract.check_2_three_states([good]) == []
    finally:
        for f in root.iterdir():
            f.unlink()
        root.rmdir()


def test_repo_smoke_green() -> None:
    """仓级烟测：主检查流在当前源码树恒绿（CI 同面）。"""
    r = subprocess.run([sys.executable, str(CHECK)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "L5 OK" in r.stdout
