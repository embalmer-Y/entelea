"""R02 build-entry preflight: explicit failures, no silent fallback."""

import importlib.util
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]

spec = importlib.util.spec_from_file_location(
    "build_zephyr", ROOT / "tools" / "build_zephyr.py"
)
BZ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(BZ)


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "entelea"
        (self.repo / "tools").mkdir(parents=True)
        (self.repo / "tools" / "check_dependencies.py").write_text("# stub\n")
        (self.repo / "build").mkdir()
        self.app = self.repo / "platform" / "zephyr" / "apps" / "demo"
        self.app.mkdir(parents=True)
        (self.app / "CMakeLists.txt").write_text("# app\n")
        self.zephyr = self.root / "zephyr"
        self.zephyr.mkdir()
        (self.zephyr / "VERSION").write_text("VERSION_MAJOR = 4\n")
        self.base = dict(
            app=str(self.app),
            board="native_sim",
            zephyr_base=str(self.zephyr),
            build_dir=None,
            sdk=None,
            sdk_version=None,
            toolchain=None,
            gcc_version=None,
            host_cc="cc",
            cmake_arg=None,
        )

    def ok_run(self, command, env=None, cwd=None):
        """Execute fixture commands for real (fake SDK scripts included)."""
        cmd = [str(part) for part in command]
        result = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=cwd)
        if result.returncode != 0:
            raise BZ.BuildError("fixture command failed: " + " ".join(cmd))
        return result.stdout.strip()

    def fails(self, message):
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            with self.assertRaisesRegex(BZ.BuildError, message):
                BZ.preflight(self.repo, type("Args", (), self.base)())

    def test_missing_app_cmake(self):
        (self.app / "CMakeLists.txt").unlink()
        self.fails("no CMakeLists.txt")

    def test_app_outside_repo(self):
        outside = self.root / "elsewhere"
        outside.mkdir()
        (outside / "CMakeLists.txt").write_text("# outside\n")
        self.base["app"] = str(outside)
        self.fails("outside repository")

    def test_not_a_zephyr_base(self):
        (self.zephyr / "VERSION").unlink()
        self.fails("no VERSION")

    def test_source_verification_failure_propagates(self):
        def reject(command, env=None, cwd=None):
            raise BZ.BuildError("Dependency Git operation failed")

        with mock.patch.object(BZ, "run", side_effect=reject):
            with self.assertRaisesRegex(BZ.BuildError, "Dependency"):
                BZ.preflight(self.repo, type("Args", (), self.base)())

    def test_native_rejects_sdk_arguments(self):
        self.base["sdk"] = str(self.root / "sdk")
        self.fails("SDK arguments are")

    def test_native_requires_host_compiler(self):
        with mock.patch.object(BZ.shutil, "which", return_value=None):
            with mock.patch.object(BZ, "run", side_effect=self.ok_run):
                with self.assertRaisesRegex(BZ.BuildError, "Host toolchain not found"):
                    BZ.preflight(self.repo, type("Args", (), self.base)())

    def test_board_requires_explicit_sdk_arguments(self):
        self.base["board"] = "esp32p4c5_core_board/esp32p4/hpcore"
        self.fails("requires explicit --sdk")

    def make_sdk(self, version="1.0.1", gcc_version="14.3.0", compiler=True):
        sdk = self.root / "sdk"
        sdk.mkdir(exist_ok=True)
        (sdk / "sdk_version").write_text(version + "\n")
        bindir = sdk / "gnu" / "riscv64-zephyr-elf" / "bin"
        bindir.mkdir(parents=True, exist_ok=True)
        gcc = bindir / "riscv64-zephyr-elf-gcc"
        if compiler:
            gcc.write_text("#!/bin/sh\necho 'riscv64-zephyr-elf-gcc "
                           f"({gcc_version}) 1'\n")
            gcc.chmod(gcc.stat().st_mode | stat.S_IEXEC)
        return sdk

    def board_base(self):
        args = dict(self.base)
        args["board"] = "esp32p4c5_core_board/esp32p4/hpcore"
        args["sdk"] = str(self.root / "sdk")
        args["sdk_version"] = "1.0.1"
        args["toolchain"] = "riscv64-zephyr-elf"
        args["gcc_version"] = "14.3.0"
        return args

    def test_sdk_version_mismatch(self):
        self.make_sdk(version="0.9.0")
        self.base = self.board_base()
        self.fails("SDK version mismatch")

    def test_sdk_version_file_missing(self):
        sdk = self.root / "sdk"
        sdk.mkdir(exist_ok=True)
        self.base = self.board_base()
        self.fails("no sdk_version")

    def test_target_compiler_missing(self):
        self.make_sdk(compiler=False)
        self.base = self.board_base()
        self.fails("Target compiler missing")

    def test_compiler_version_mismatch(self):
        self.make_sdk(gcc_version="13.2.0")
        self.base = self.board_base()
        self.fails("Compiler version mismatch")

    def test_build_dir_outside_build_root(self):
        self.base["build_dir"] = str(self.root / "elsewhere" / "b")
        self.fails("must live under")

    def test_cache_board_conflict_rejected(self):
        target = self.repo / "build" / "r02-native_sim"
        target.mkdir(parents=True)
        (target / "CMakeCache.txt").write_text("BOARD:STRING=qemu_cortex_m3\n")
        self.fails("caches BOARD=qemu_cortex_m3")

    def test_cache_app_conflict_rejected(self):
        target = self.repo / "build" / "r02-native_sim"
        target.mkdir(parents=True)
        (target / "CMakeCache.txt").write_text(
            "BOARD:STRING=native_sim\n"
            "CMAKE_HOME_DIRECTORY:INTERNAL=/tmp/other-app\n"
        )
        self.fails("another app")

    def test_native_preflight_plan(self):
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            with mock.patch.object(BZ.shutil, "which", return_value="/usr/bin/cc"):
                env, cmake_args, build_dir = BZ.preflight(
                    self.repo, type("Args", (), self.base)()
                )
        self.assertEqual(env["ZEPHYR_TOOLCHAIN_VARIANT"], "host")
        self.assertEqual(env["ZEPHYR_MODULES"], "")
        self.assertEqual(env["ZEPHYR_BASE"], str(self.zephyr))
        self.assertIn("-DBOARD=native_sim", cmake_args)
        self.assertTrue(build_dir.is_relative_to(self.repo / "build"))

    def test_board_preflight_plan(self):
        self.make_sdk()
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            env, cmake_args, build_dir = BZ.preflight(
                self.repo, type("Args", (), self.board_base())()
            )
        self.assertEqual(env["ZEPHYR_TOOLCHAIN_VARIANT"], "zephyr")
        self.assertEqual(env["ZEPHYR_SDK_INSTALL_DIR"], str(self.root / "sdk"))
        self.assertEqual(env["ZEPHYR_MODULES"], "")
        self.assertTrue(build_dir.is_relative_to(self.repo / "build"))

    def test_check_dependencies_invoked_read_only(self):
        seen = []

        def spy(command, env=None, cwd=None):
            cmd = [str(part) for part in command]
            seen.append(cmd)
            return self.ok_run(command, env, cwd)

        with mock.patch.object(BZ, "run", side_effect=spy):
            with mock.patch.object(BZ.shutil, "which", return_value="/usr/bin/cc"):
                BZ.preflight(self.repo, type("Args", (), self.base)())
        verify = [c for c in seen if any("check_dependencies.py" in part for part in c)]
        self.assertEqual(len(verify), 1)
        self.assertIn("--dependency", verify[0])
        self.assertIn("zephyr", verify[0])
        self.assertIn("--checkout", verify[0])


if __name__ == "__main__":
    unittest.main()
