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
            python=None,
            board_root=None,
            module=None,
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

    def test_board_root_outside_repo_rejected(self):
        outside = self.root / "boards"
        outside.mkdir()
        self.base["board"] = "some_board"
        self.base["sdk"] = str(self.root / "sdk")
        self.base["sdk_version"] = "1.0.1"
        self.base["toolchain"] = "riscv64-zephyr-elf"
        self.base["gcc_version"] = "14.3.0"
        self.make_sdk()
        self.base["board_root"] = str(outside)
        self.fails("Board root outside repository")

    def test_board_root_missing_dir_rejected(self):
        self.base["board"] = "some_board"
        self.base["sdk"] = str(self.root / "sdk")
        self.base["sdk_version"] = "1.0.1"
        self.base["toolchain"] = "riscv64-zephyr-elf"
        self.base["gcc_version"] = "14.3.0"
        self.make_sdk()
        self.base["board_root"] = str(self.root / "nope")
        self.fails("not a directory")

    def test_board_root_passed_explicitly(self):
        boards = self.repo / "platform" / "zephyr" / "boards"
        boards.mkdir(parents=True)
        self.make_sdk()
        base = self.board_base()
        base["board_root"] = str(boards)
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            env, cmake_args, build_dir = BZ.preflight(
                self.repo, type("Args", (), base)()
            )
        self.assertIn(f"-DBOARD_ROOT={boards.resolve()}", cmake_args)

    def test_module_bad_spec_rejected(self):
        self.base["module"] = ["hal_espressif"]
        self.fails("Invalid --module spec")

    def test_module_unpinned_name_rejected(self):
        self.base["module"] = ["something=/tmp/x"]
        self.fails("Unpinned module not allowed")

    def test_module_verification_failure_propagates(self):
        hal = self.root / "hal"
        hal.mkdir()
        self.base["module"] = [f"hal_espressif={hal}"]

        def reject(command, env=None, cwd=None):
            cmd = [str(part) for part in command]
            if "hal_espressif" in cmd:
                raise BZ.BuildError("Dependency revision mismatch")
            return self.ok_run(command, env, cwd)

        with mock.patch.object(BZ, "run", side_effect=reject):
            with self.assertRaisesRegex(BZ.BuildError, "revision mismatch"):
                BZ.preflight(self.repo, type("Args", (), self.base)())

    def test_modules_explicit_and_verified(self):
        hal = self.root / "hal"
        hal.mkdir()
        self.base["module"] = [f"hal_espressif={hal}", f"wamr={hal}"]
        seen = []

        def spy(command, env=None, cwd=None):
            cmd = [str(part) for part in command]
            seen.append(cmd)
            return self.ok_run(command, env, cwd)

        with mock.patch.object(BZ, "run", side_effect=spy):
            with mock.patch.object(BZ.shutil, "which", return_value="/usr/bin/cc"):
                env, cmake_args, build_dir = BZ.preflight(
                    self.repo, type("Args", (), self.base)()
                )
        checks = [c for c in seen if "--checkout" in c]
        self.assertEqual(len(checks), 3)  # zephyr + hal + wamr
        self.assertIn("hal_espressif", checks[1])
        self.assertIn(str(hal.resolve()), checks[1])
        self.assertIn("wamr", checks[2])
        self.assertEqual(
            env["ZEPHYR_MODULES"], f"{hal.resolve()};{hal.resolve()}"
        )

    def test_cmake_arg_cannot_override_board(self):
        self.base["cmake_arg"] = ["-DBOARD=qemu_x86"]
        self.fails("must not override managed variable BOARD")

    def test_cmake_arg_cannot_override_managed_vars(self):
        for var in ("ZEPHYR_BASE", "ZEPHYR_MODULES", "Python3_EXECUTABLE",
                    "CMAKE_C_COMPILER", "BOARD_ROOT", "ZEPHYR_SDK_INSTALL_DIR"):
            self.base["cmake_arg"] = [f"-D{var}=x"]
            self.fails("must not override managed variable " + var)

    def test_cmake_arg_app_options_still_allowed(self):
        self.base["cmake_arg"] = ["-DTEST_WDT_REQUIRED_MISSING=ON"]
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            with mock.patch.object(BZ.shutil, "which", return_value="/usr/bin/cc"):
                env, cmake_args, build_dir = BZ.preflight(
                    self.repo, type("Args", (), self.base)()
                )
        self.assertIn("-DTEST_WDT_REQUIRED_MISSING=ON", cmake_args)

    def test_python_missing_rejected(self):
        self.base["python"] = str(self.root / "no-python")
        self.fails("--python interpreter not found")

    def test_python_verified_and_bound(self):
        fake_py = self.root / "fakepy"
        fake_py.write_text("#!/bin/sh\necho 'Python 3.12.13'\n")
        fake_py.chmod(fake_py.stat().st_mode | stat.S_IEXEC)
        self.base["python"] = str(fake_py)
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            with mock.patch.object(BZ.shutil, "which", return_value="/usr/bin/cc"):
                env, cmake_args, build_dir = BZ.preflight(
                    self.repo, type("Args", (), self.base)()
                )
        self.assertIn(f"-DPython3_EXECUTABLE={fake_py.absolute()}", cmake_args)

    def test_native_binds_verified_compiler(self):
        with mock.patch.object(BZ, "run", side_effect=self.ok_run):
            with mock.patch.object(BZ.shutil, "which", return_value="/usr/bin/cc"):
                env, cmake_args, build_dir = BZ.preflight(
                    self.repo, type("Args", (), self.base)()
                )
        self.assertIn("-DCMAKE_C_COMPILER=/usr/bin/cc", cmake_args)
        board_index = cmake_args.index("-DBOARD=native_sim")
        cc_index = cmake_args.index("-DCMAKE_C_COMPILER=/usr/bin/cc")
        self.assertLess(board_index, cc_index, "managed args precede user args")

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
