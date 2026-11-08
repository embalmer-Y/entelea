"""Reject polluted dependencies and architecture/source classification violations."""

import copy
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ARCH = load("check_architecture")
DEP = load("check_dependencies")
POLICY = json.loads((ROOT / "build-support/source-policy.json").read_text())
LOCK = json.loads((ROOT / "build-support/dependencies.json").read_text())


class ArchitectureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.policy = {
            "common": ["framework/common/a.c"],
            "compat": [],
            "contract_tests": ["tests/contracts/a.c"],
            "zephyr_framework": ["framework/runtime/supervision/fw.c"],
            "platform_zephyr": ["platform/zephyr/pf.c"],
            "diagnostics": {},
            "protected_output_sources": [],
        }
        self.write("framework/common/a.c", "int a(void) { return 0; }\n")
        self.write("tests/contracts/a.c", "int main(void) { return 0; }\n")
        self.write("framework/runtime/supervision/fw.c", "int fw(void) { return 0; }\n")
        self.write("platform/zephyr/pf.c", "int pf(void) { return 0; }\n")

    def write(self, path, text):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)

    def rejected(self, message):
        with self.assertRaisesRegex(ValueError, message):
            ARCH.validate(self.root, self.policy)

    def test_current_sources(self):
        self.assertEqual(ARCH.validate(ROOT, POLICY), 49)

    def test_unclassified_source(self):
        self.write("framework/common/extra.c", "")
        self.rejected("Unclassified")

    def test_missing_source(self):
        self.policy["common"].append("framework/common/missing.c")
        self.rejected("Missing/escaping")

    def test_output_bypass(self):
        self.write("framework/common/a.c", "gpio_pin_set(0, 1);")
        self.rejected("Output bypass")

    def test_gate_cannot_cover_host_sources(self):
        self.policy["protected_output_sources"] = ["framework/common/a.c"]
        self.rejected("Protection gate outside reviewed Zephyr sources")

    def test_gate_must_be_classified(self):
        self.policy["protected_output_sources"] = ["platform/zephyr/unlisted.c"]
        self.rejected("Protection gate outside reviewed Zephyr sources")

    def test_gate_allows_reviewed_driver_output(self):
        self.write(
            "framework/runtime/supervision/fw.c", "gpio_pin_set_dt(&s, 1);\n"
        )
        self.policy["protected_output_sources"] = [
            "framework/runtime/supervision/fw.c"
        ]
        self.assertEqual(ARCH.validate(self.root, self.policy), 4)

    def test_zephyr_framework_output_bypass(self):
        self.write(
            "framework/execution/safety/bad.c", "pwm_set(dev, 1, 2, 3, 0);\n"
        )
        self.policy["zephyr_framework"].append("framework/execution/safety/bad.c")
        self.rejected("Output bypass")

    def test_zephyr_framework_direct_clock(self):
        self.write("platform/zephyr/pf.c", "k_uptime_get();\n")
        self.write("framework/runtime/supervision/fw.c", "k_uptime_get();\n")
        self.rejected("Direct clock")

    def test_driver_table_bypass_rejected(self):
        self.write(
            "platform/zephyr/pf.c",
            "extern char entelea_drivers; char f(void) { return entelea_drivers; }\n",
        )
        self.rejected("Driver table bypass")

    def test_driver_table_inside_safety_allowed(self):
        self.policy["zephyr_framework"].append(
            "framework/execution/safety/extra.c"
        )
        self.write(
            "framework/execution/safety/extra.c",
            "extern const char entelea_drivers;\n",
        )
        self.assertEqual(ARCH.validate(self.root, self.policy), 5)

    def test_platform_clock_adapter_allowed(self):
        self.write("platform/zephyr/pf.c", "k_uptime_get();\n")
        self.assertEqual(ARCH.validate(self.root, self.policy), 4)

    def test_public_header_os_dependency(self):
        self.write("framework/include/entelea/a.h", "#include <zephyr/kernel.h>")
        self.rejected("Non-public dependency")

    def test_direct_clock(self):
        self.write("framework/common/a.c", "k_uptime_get();")
        self.rejected("Direct clock")

    def test_private_source_dependency(self):
        self.write("framework/common/a.c", '#include "internal.h"')
        self.rejected("Private dependency")

    def test_relative_private_dependency(self):
        self.write("framework/common/a.c", '#include "../net/internal.h"')
        self.rejected("Unapproved common dependency")

    def test_explicit_diagnostic(self):
        self.write("tests/diagnostics/probe.c", "gpio_pin_set(0, 1);")
        self.policy["diagnostics"] = {"probe": ["tests/diagnostics/probe.c"]}
        self.assertEqual(ARCH.validate(self.root, self.policy), 5)

    def test_diagnostic_cannot_enter_product(self):
        self.write("tests/diagnostics/probe.c", "gpio_pin_set(0, 1);")
        self.policy["common"].append("tests/diagnostics/probe.c")
        self.rejected("crosses target boundary")

    def test_diagnostic_duplicate_linkage(self):
        self.policy["diagnostics"] = {"probe": ["framework/common/a.c"]}
        self.rejected("classified twice")

    def test_diagnostic_no_directory_exemption(self):
        self.write("tests/diagnostics/unlisted.c", "gpio_pin_set(0, 1);")
        self.rejected("Unclassified")


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "-q")
        (self.repo / "a.txt").write_text("base\n")
        self.git("add", "a.txt")
        self.git(
            "-c",
            "user.name=R01 test",
            "-c",
            "user.email=test@example.invalid",
            "commit",
            "-q",
            "-m",
            "fixture",
        )
        self.commit = self.git("rev-parse", "HEAD").decode().strip()

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args], stderr=subprocess.DEVNULL
        )

    def test_clean_checkout(self):
        DEP.verify_checkout(self.repo, self.commit, b"")

    def test_wrong_revision(self):
        with self.assertRaisesRegex(ValueError, "revision mismatch"):
            DEP.verify_checkout(self.repo, "0" * 40, b"")

    def test_unapproved_diff(self):
        (self.repo / "a.txt").write_text("changed\n")
        with self.assertRaisesRegex(ValueError, "complete approved"):
            DEP.verify_checkout(self.repo, self.commit, b"")

    def test_complete_approved_diff(self):
        (self.repo / "a.txt").write_text("approved\n")
        expected = self.git("diff", "--no-color", "--binary", "--unified=3", "HEAD")
        DEP.verify_checkout(self.repo, self.commit, expected)
        (self.repo / "a.txt").write_text("approved\nextra\n")
        with self.assertRaisesRegex(ValueError, "complete approved"):
            DEP.verify_checkout(self.repo, self.commit, expected)

    def test_staged_change(self):
        (self.repo / "a.txt").write_text("staged\n")
        self.git("add", "a.txt")
        with self.assertRaisesRegex(ValueError, "Staged"):
            DEP.verify_checkout(self.repo, self.commit, b"")

    def test_untracked_change(self):
        (self.repo / "extra.txt").write_text("extra")
        with self.assertRaisesRegex(ValueError, "Untracked"):
            DEP.verify_checkout(self.repo, self.commit, b"")

    def test_current_pins(self):
        self.assertEqual(set(DEP.validate(ROOT, LOCK)), set(DEP.PINS))

    def test_pin_drift(self):
        lock = copy.deepcopy(LOCK)
        lock["dependencies"][0]["commit"] = "0" * 40
        with self.assertRaisesRegex(ValueError, "pin changed"):
            DEP.validate(ROOT, lock)

    def test_candidate_patch_rejected(self):
        lock = copy.deepcopy(LOCK)
        lock["dependencies"][0]["patches"].append(
            {"path": "patches/zephyr-esp32-gdma.patch"}
        )
        with self.assertRaisesRegex(ValueError, "Unapproved patch"):
            DEP.validate(ROOT, lock)

    def test_patch_digest_drift(self):
        lock = copy.deepcopy(LOCK)
        lock["dependencies"][0]["patches"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "baseline identity"):
            DEP.validate(ROOT, lock)


if __name__ == "__main__":
    unittest.main()
