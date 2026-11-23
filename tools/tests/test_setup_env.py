"""Govern the environment setup entry and the hashed interpreter lock."""

import importlib.util
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "build-support/setup-env.sh"
LOCK = ROOT / "build-support/requirements-zephyr.lock"
PIN = re.compile(r"^([A-Za-z0-9_.-]+)(?:\[[^\]]*\])?==")


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SetupEnvTests(unittest.TestCase):
    def test_script_parses_and_is_executable(self):
        self.assertTrue(SCRIPT.stat().st_mode & 0o111, "setup-env.sh not executable")
        self.assertEqual(
            subprocess.run(["bash", "-n", str(SCRIPT)], capture_output=True).returncode,
            0,
        )

    def test_help_lists_every_option_the_parser_accepts(self):
        help_text = subprocess.run(
            [str(SCRIPT), "--help"], capture_output=True, text=True
        ).stdout
        parser = SCRIPT.read_text()
        accepted = set(re.findall(r"^\t(--[a-z-]+)", parser, re.MULTILINE))
        self.assertIn("--system", accepted)
        self.assertIn("--proxy", accepted)
        for option in accepted:
            self.assertIn(option, help_text, f"--help does not mention {option}")

    def test_unknown_option_is_refused(self):
        self.assertEqual(
            subprocess.run([str(SCRIPT), "--nonsense"], capture_output=True).returncode,
            2,
        )

    def test_pins_come_from_dependencies_json_not_from_the_script(self):
        # The script names upstream locations only; commits and patches are
        # read from the single source the dependency checker cross-checks.
        script = SCRIPT.read_text()
        self.assertNotRegex(script, r"\b[0-9a-f]{40}\b", "a commit is hard-coded")
        self.assertIn("dep zephyr commit", script)
        self.assertIn("dep zephyr patches", script)
        self.assertIn('check "$id" "$DEPS/$dir"', script)
        # west must never be installed into the build interpreter.
        self.assertIn("find_spec('west')", script)
        self.assertIn("--require-hashes", script)


class LockTests(unittest.TestCase):
    def setUp(self):
        self.lines = LOCK.read_text().split("\n")

    def test_west_is_not_locked_into_the_build_interpreter(self):
        self.assertFalse([line for line in self.lines if line.startswith("west==")])

    def test_every_pin_is_hashed(self):
        pins = 0
        for index, line in enumerate(self.lines):
            match = PIN.match(line)
            if not match:
                continue
            pins += 1
            following = []
            for later in self.lines[index + 1 :]:
                if later.startswith("    ") or later.startswith("\t"):
                    following.append(later)
                else:
                    break
                if "    # via" in later:
                    break
            self.assertTrue(
                any("--hash=sha256:" in item for item in following),
                f"{match.group(1)} is pinned without a hash",
            )
        self.assertGreater(pins, 100)

    def test_header_names_the_reproduction_command(self):
        header = "\n".join(self.lines[:3])
        self.assertIn("uv pip compile", header)
        self.assertIn("--generate-hashes", header)
        self.assertIn("--no-emit-package west", header)
        self.assertIn("build-support/requirements-zephyr.lock", header)

    def test_style_tool_version_is_the_locked_clang_format(self):
        # The style gate refuses any other clang-format, so the lock must
        # install exactly the version it demands.
        locked = [
            line.split("==", 1)[1].strip(" \\")
            for line in self.lines
            if line.startswith("clang-format==")
        ]
        self.assertEqual(locked, [load("check_style").CLANG_FORMAT_VERSION])


if __name__ == "__main__":
    unittest.main()
