# SPDX-License-Identifier: Apache-2.0
"""仓库级检查（M0 CI 骨架起步；docs/std/testing.md L5 的雏形位）。

军规 4：全库文本 UTF-8 无 BOM——此处在 CI 中机械执行（本地构建不依赖 Zephyr 环境）。
"""
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]  # firmware/tests/pytest -> 仓库根

TEXT_SUFFIXES = {".md", ".c", ".h", ".py", ".yml", ".yaml", ".txt", ".conf", ".cmake"}
def _text_files():
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=REPO,
    ).decode().split("\0")
    return [
        p for name in sorted(set(names) - {""})
        if (p := REPO / name).is_file()
        if p.suffix.lower() in TEXT_SUFFIXES
    ]


def test_no_bom():
    """全库文本不得带 UTF-8 BOM（军规 4：旧项目曾因编码事故损毁文档）。"""
    files = _text_files()
    assert files, "扫描器必须找到文件，否则检查失真"
    bad = [str(p) for p in files if p.open("rb").read(3) == b"\xef\xbb\xbf"]
    assert not bad, f"发现 BOM：{bad}"


def test_utf8_decodable():
    """全库文本必须可按 UTF-8 解码。"""
    for p in _text_files():
        p.read_text(encoding="utf-8")
