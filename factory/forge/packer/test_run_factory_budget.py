#!/usr/bin/env python3
"""run_factory 预算闸离线单测（RF1 #10；不导入 SDK、不联网）。"""

import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_factory as rf  # noqa: E402

HDR = (
    "| 日期 | 用途 | 模型 | tokens（约） | 估算成本 | 结果 |\n"
    "| --- | --- | --- | --- | --- | --- |\n"
)


def row(purpose: str, tokens: str) -> str:
    return f"| 2026-11-09 | {purpose} | m | {tokens} | ¥0 | r |\n"


class ParseTokens(unittest.TestCase):
    def test_forms(self):
        self.assertEqual(rf.parse_tokens("1025161"), 1025161)
        self.assertEqual(rf.parse_tokens("~1.0M(估)"), 1_000_000)
        self.assertEqual(rf.parse_tokens("~0.5M(估)"), 500_000)
        self.assertEqual(rf.parse_tokens("63+76"), 139)
        self.assertEqual(rf.parse_tokens("1.5K"), 1500)
        self.assertEqual(rf.parse_tokens("≈2,000"), 2000)

    def test_unparseable_fails_closed(self):
        for bad in ("", "未读出", "1.0G", "-5", "12abc", "~", "1++2"):
            with self.assertRaises(rf.BudgetError, msg=bad):
                rf.parse_tokens(bad)


class LedgerSum(unittest.TestCase):
    def test_only_factory_rows(self):
        text = (
            HDR
            + row("工厂 run001", "~1.0M(估)")
            + row("冒烟", "999999")
            + row("工厂 run002", "1025161")
        )
        self.assertEqual(rf.factory_tokens_used(text), 2_025_161)

    def test_bad_factory_row_fails_closed(self):
        with self.assertRaises(rf.BudgetError):
            rf.factory_tokens_used(HDR + row("工厂 run009", "unknown"))
        with self.assertRaises(rf.BudgetError):
            rf.factory_tokens_used(HDR + "| 2026 | 工厂 run009 |\n")

    def test_real_ledger_parses(self):
        if not rf.LEDGER.is_file():
            self.skipTest("ledger absent")
        self.assertGreater(rf.factory_tokens_used(rf.LEDGER.read_text()), 0)

    def test_unread_usage_row_roundtrips_as_cap(self):
        r = rf.ledger_row("run9", "m", None, 2_000_000, "x")
        self.assertEqual(rf.factory_tokens_used(HDR + r), 2_000_000)
        r = rf.ledger_row("run9", "m", 1234, 2_000_000, "x")
        self.assertEqual(rf.factory_tokens_used(HDR + r), 1234)


class Gate(unittest.TestCase):
    CAP = rf.FACTORY_CAP_TOKENS

    def test_under_cap(self):
        self.assertIsNone(rf.budget_gate(0, 2_000_000))

    def test_warn_at_80_percent_projected(self):
        self.assertIsNotNone(rf.budget_gate(int(self.CAP * 0.8) - 1, 1))

    def test_exhausted_or_projected_over_rejects(self):
        for used, run in (
            (self.CAP, 1),
            (self.CAP + 5, 1),
            (self.CAP - 1_000_000, 2_000_000),
        ):
            with self.assertRaises(rf.BudgetError):
                rf.budget_gate(used, run)
        self.assertIsNotNone(rf.budget_gate(self.CAP - 2_000_000, 2_000_000))

    def test_nonpositive_run_cap_rejects(self):
        with self.assertRaises(rf.BudgetError):
            rf.budget_gate(0, 0)


class MainRefusesBeforeSdk(unittest.TestCase):
    def run_main(self, ledger_text, argv):
        with tempfile.TemporaryDirectory() as d:
            led = Path(d) / "ledger.md"
            if ledger_text is not None:
                led.write_text(ledger_text)
            saved = rf.LEDGER
            rf.LEDGER = led
            try:
                with redirect_stderr(io.StringIO()):
                    rc = rf.main(argv)
            finally:
                rf.LEDGER = saved
            return rc, ("openhands.sdk" in sys.modules)

    def test_exhausted_budget_no_sdk_import(self):
        text = HDR + row("工厂 big", f"{rf.FACTORY_CAP_TOKENS}")
        rc, imported = self.run_main(text, [str(rf.FORGE / "artifacts/runX")])
        self.assertEqual(rc, 2)
        self.assertFalse(imported)

    def test_missing_or_bad_ledger_fails_closed(self):
        self.assertEqual(self.run_main(None, ["x"])[0], 2)
        self.assertEqual(self.run_main(HDR + row("工厂 a", "?"), ["x"])[0], 2)

    def test_bad_args_refused(self):
        argv_bad = (
            ["/tmp/elsewhere"],
            [str(rf.FORGE / "artifacts/runX"), "--max-iter", "0"],
            [str(rf.FORGE / "artifacts/runX"), "--run-tokens", "0"],
        )
        for argv in argv_bad:
            rc, imported = self.run_main(HDR, argv)
            self.assertEqual(rc, 2, argv)
            self.assertFalse(imported)

    def test_existing_evidence_not_overwritten(self):
        run004 = rf.FORGE / "artifacts/run004"
        self.assertTrue((run004 / "app.wasm").exists(), "run004 evidence missing")
        rc, imported = self.run_main(HDR, [str(run004)])
        self.assertEqual(rc, 2)
        self.assertFalse(imported)


class ModuleImportIsSdkFree(unittest.TestCase):
    def test_import_does_not_load_sdk(self):
        code = (
            "import sys; sys.path.insert(0, %r); import run_factory; "
            "print('openhands' in ' '.join(sys.modules))"
            % str(Path(__file__).resolve().parent)
        )
        out = subprocess.run(
            [sys.executable, "-c", code], check=True, capture_output=True, text=True
        ).stdout.strip()
        self.assertEqual(out, "False")


if __name__ == "__main__":
    unittest.main()
