"""Verified Zephyr build entry for Entelea (R02).

Explicit preflight before any CMake invocation (build-support/README.md
"R02 构建入口" contract):

1. Verify the pinned Zephyr checkout (commit + full approved patch set +
   staged/untracked/mode diffs) by reusing the read-only dependency checker.
2. native_sim targets select the host toolchain explicitly; board targets
   require an explicit SDK, sdk_version, toolchain and compiler version.
3. SDK sdk_version and the target compiler's real --version are verified;
   missing/mismatched pieces fail explicitly — no auto-download, no fallback.
4. Build directories are per-target and never reuse another target's cache.
5. The resolved plan (paths, versions, command) is printed for evidence.

West is not used; ZEPHYR_MODULES is pinned empty so module selection is
explicit, not inherited from any workspace.
"""

import argparse
import os
import re
import shutil
import subprocess
import json
import sys
from pathlib import Path

NATIVE_BOARDS = ("native_sim", "native_sim/native/64", "native_sim/native/32")
# 模块白名单=dependencies.json 单一来源（F 批起含 nanopb/mbedtls/
# tf_psa_crypto——真网/真云面模块；commit pin 由 check_dependencies.py
# 交叉校验，防 json 自改）。
PINNED_MODULES = {dep["id"] for dep in json.loads(
    (Path(__file__).resolve().parents[1]
     / "build-support/dependencies.json").read_text())["dependencies"]}
# Managed CMake variables: set only by this tool after verification. Extra
# --cmake-arg values must not override them (review defect 6: a later
# -DBOARD=... silently repointed the build while the SDK stayed verified
# for the original board).
MANAGED_CMAKE_VARS = (
    "BOARD", "BOARD_ROOT", "ZEPHYR_BASE", "ZEPHYR_MODULES",
    "ZEPHYR_TOOLCHAIN_VARIANT", "ZEPHYR_SDK_INSTALL_DIR",
    "Python3_EXECUTABLE", "CMAKE_C_COMPILER",
    "CMAKE_PROJECT_INCLUDE", "CMAKE_PROJECT_TOP_LEVEL_INCLUDES",
    "CMAKE_HOME_DIRECTORY", "DTC_OVERLAY_FILE", "DTC_ROOT",
    "CMAKE_TOOLCHAIN_FILE",
)


class BuildError(Exception):
    """Explicit preflight/build failure (never fall back silently)."""


def is_native(board):
    return board in NATIVE_BOARDS


def run(command, env=None, cwd=None):
    result = subprocess.run(command, capture_output=True, text=True, env=env, cwd=cwd)
    if result.returncode != 0:
        raise BuildError(
            f"Command failed ({result.returncode}): {' '.join(map(str, command))}\n"
            f"{result.stdout.strip()}\n{result.stderr.strip()}"
        )
    return result.stdout.strip()


def preflight(root, args):
    """Return (env, cmake_args) or raise BuildError."""
    app = Path(args.app).resolve()
    zephyr_base = Path(args.zephyr_base).resolve()
    if not app.is_dir() or (app / "CMakeLists.txt").is_file() is False:
        raise BuildError(f"App directory has no CMakeLists.txt: {app}")
    if not root.resolve() in app.parents:
        raise BuildError(f"App outside repository: {app}")
    if not (zephyr_base / "VERSION").is_file():
        raise BuildError(f"Not a Zephyr base (no VERSION): {zephyr_base}")

    # 1. Pinned source verification (read-only; reuses the R01 checker).
    checker = str(root / "tools" / "check_dependencies.py")

    run([sys.executable, checker, "--dependency", "zephyr",
         "--checkout", str(zephyr_base)])

    # Explicit module selection: only pinned dependency ids, each verified
    # read-only at its pinned commit/patch state. No implicit workspace.
    modules = []
    for spec in args.module or []:
        name, sep, path = spec.partition("=")
        if not sep or not name or not path:
            raise BuildError(f"Invalid --module spec (expected name=path): {spec}")
        if name not in PINNED_MODULES:
            raise BuildError(
                f"Unpinned module not allowed: {name} "
                f"(allowed: {', '.join(sorted(PINNED_MODULES))})"
            )
        module_path = Path(path).resolve()
        run([sys.executable, checker, "--dependency", name,
             "--checkout", str(module_path)])
        modules.append(str(module_path))

    # Managed esptool shim: the frozen venv's console script carries a stale
    # pre-relocation shebang; do not modify the venv. Instead verify the given
    # interpreter can import esptool, generate a shim in the build dir and
    # prepend it to PATH for configure+build (explicit, no auto-install).
    if args.esptool_python:
        esppy = Path(args.esptool_python).absolute()
        if not esppy.is_file():
            raise BuildError(f"--esptool-python not found: {esppy}")
        run([str(esppy), "-c", "import esptool"], env=dict(os.environ))
        toolbin = root / "build" / ".toolbin"
        toolbin.mkdir(parents=True, exist_ok=True)
        shim = toolbin / "esptool"
        shim.write_text(f"#!/bin/sh\nexec {esppy} -m esptool \"$@\"\n")
        shim.chmod(0o755)
        env_path = str(toolbin) + os.pathsep + os.environ.get("PATH", "")
        print(f"[preflight] esptool shim: {shim} -> {esppy} -m esptool")
    else:
        env_path = None

    env = dict(os.environ)
    if env_path is not None:
        env["PATH"] = env_path
    env["ZEPHYR_BASE"] = str(zephyr_base)
    env["ZEPHYR_MODULES"] = ";".join(modules)  # explicit set; no west workspace

    managed_args = []
    if args.python:
        # absolute() (not resolve()): venv pythons are symlinks into an
        # interpreter store; following the link escapes the venv and loses
        # its site-packages. Keep the path exactly as provided.
        python = Path(args.python).absolute()
        if not python.is_file():
            raise BuildError(f"--python interpreter not found: {python}")
        run([str(python), "--version"])
        managed_args.append(f"-DPython3_EXECUTABLE={python}")
        print(f"[preflight] python: {run([str(python), '--version'])}")

    if is_native(args.board):
        if args.sdk or args.sdk_version or args.toolchain or args.gcc_version:
            raise BuildError(
                "native_sim targets use the host toolchain; SDK arguments are "
                "not applicable (board targets need them)"
            )
        cc = shutil.which(args.host_cc)
        if cc is None:
            raise BuildError(f"Host toolchain not found: {args.host_cc}")
        version = run([cc, "--version"]).splitlines()[0]
        env["ZEPHYR_TOOLCHAIN_VARIANT"] = "host"
        # Bind the verified compiler to the actual build (defect 6: previously
        # --host-cc was only checked, never passed; Zephyr picked its own).
        managed_args.append(f"-DCMAKE_C_COMPILER={cc}")
        print(f"[preflight] host toolchain: {cc}: {version}")
    else:
        missing = [
            name
            for name, value in (
                ("--sdk", args.sdk),
                ("--sdk-version", args.sdk_version),
                ("--toolchain", args.toolchain),
                ("--gcc-version", args.gcc_version),
            )
            if not value
        ]
        if missing:
            raise BuildError(
                f"Board target {args.board} requires explicit {', '.join(missing)}"
            )
        sdk = Path(args.sdk).resolve()
        version_file = sdk / "sdk_version"
        if not version_file.is_file():
            raise BuildError(f"SDK has no sdk_version file: {sdk}")
        actual = version_file.read_text().strip()
        if actual != args.sdk_version.strip():
            raise BuildError(
                f"SDK version mismatch: expected {args.sdk_version}, got {actual}"
            )
        compiler = sdk / "gnu" / args.toolchain / "bin" / f"{args.toolchain}-gcc"
        if not compiler.is_file():
            raise BuildError(f"Target compiler missing: {compiler}")
        first = run([str(compiler), "--version"]).splitlines()[0]
        if args.gcc_version.strip() not in first:
            raise BuildError(
                f"Compiler version mismatch: expected {args.gcc_version}, got: {first}"
            )
        env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
        env["ZEPHYR_SDK_INSTALL_DIR"] = str(sdk)
        print(f"[preflight] SDK {sdk}: sdk_version={actual}")
        print(f"[preflight] target toolchain: {first}")

    build_dir = Path(args.build_dir) if args.build_dir else None
    if build_dir is None:
        slug = re.sub(r"[^a-z0-9._-]+", "-", args.board.lower()).strip("-")
        build_dir = root / "build" / f"r02-{slug}"
    build_dir = (root / build_dir).resolve() if not build_dir.is_absolute() else build_dir
    allowed_root = (root / "build").resolve()
    if allowed_root not in build_dir.parents:
        raise BuildError(f"Build directory must live under {allowed_root}: {build_dir}")

    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        text = cache.read_text(errors="replace")
        cached_board = re.search(r"^BOARD:STRING=(.+)$", text, re.MULTILINE)
        cached_home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", text, re.MULTILINE)
        if cached_board and cached_board.group(1).strip() != args.board:
            raise BuildError(
                f"Build directory {build_dir} caches BOARD={cached_board.group(1)}; "
                "pick a per-target directory instead of reusing another target's cache"
            )
        if cached_home and Path(cached_home.group(1).strip()).resolve() != app:
            raise BuildError(
                f"Build directory {build_dir} caches another app ({cached_home.group(1)}); "
                "pick a per-target directory"
            )

    for extra in args.cmake_arg or []:
        flag = extra.strip()
        if flag in {"-S", "-B"} or flag.startswith("-S") or flag.startswith("-B"):
            raise BuildError(
                f"--cmake-arg must not redirect {flag.split(' ')[0]} "
                "(source/build directories are managed by the entry)"
            )
        if flag.startswith("-C") or flag.startswith("--preset") or \
                flag.startswith("--toolchain") or flag.startswith("-U"):
            raise BuildError(
                f"--cmake-arg must not inject {flag.split(' ')[0]} "
                "(init scripts/presets/toolchain/unset are entry-managed)"
            )
        name = flag[2:] if flag.startswith("-D") else flag
        # Strip cache-type suffixes too: -DBOARD:STRING=... must not bypass
        # the managed-variable blacklist (second-review finding 4).
        var = name.split("=", 1)[0].split(":", 1)[0].strip()
        if var in MANAGED_CMAKE_VARS:
            raise BuildError(
                f"--cmake-arg must not override managed variable {var} "
                "(use the dedicated entry options)"
            )
    cmake_args = ["cmake", "-S", str(app), "-B", str(build_dir), "-G", "Ninja",
                  f"-DBOARD={args.board}"]
    if args.board_root:
        board_root = Path(args.board_root).resolve()
        if not board_root.is_dir():
            raise BuildError(f"Board root is not a directory: {board_root}")
        if root.resolve() not in board_root.parents:
            raise BuildError(f"Board root outside repository: {board_root}")
        cmake_args.append(f"-DBOARD_ROOT={board_root}")
    cmake_args += managed_args
    cmake_args += list(args.cmake_arg or [])
    print(f"[preflight] app={app}")
    print(f"[preflight] board={args.board} build={build_dir}")
    print(f"[preflight] zephyr={zephyr_base} modules={env['ZEPHYR_MODULES'] or '(none)'}")
    return env, cmake_args, build_dir


def verify_effective_config(build_dir, args, modules):
    """Check what CMake actually adopted, not just the command we printed.

    A verified command line does not prove an effective configuration:
    read back the generated cache, the resolved module list and the source
    directory, and compare them with the preflight-verified values
    (third-review finding 5: source-dir hijack via a second -S, module
    identity vs count, board-target compiler, pinned interpreter).
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise BuildError(f"No CMakeCache.txt after configure: {cache}")
    text = cache.read_text(errors="replace")

    def cached(var):
        found = re.search(rf"^{var}:(?:[A-Z]+)=(.*)$", text, re.MULTILINE)
        return found.group(1).strip() if found else None

    checks = [
        ("BOARD", cached("BOARD"), args.board),
        ("ZEPHYR_BASE", cached("ZEPHYR_BASE"), str(Path(args.zephyr_base).resolve())),
        # Actual source directory (a second -S would land here).
        ("CMAKE_HOME_DIRECTORY", cached("CMAKE_HOME_DIRECTORY"),
         str(Path(args.app).resolve())),
    ]
    if is_native(args.board):
        cc = shutil.which(args.host_cc)
        checks.append(("CMAKE_C_COMPILER", cached("CMAKE_C_COMPILER"), cc))
    else:
        # Board builds compile with the SDK cross compiler: the cache must
        # point at the verified toolchain inside the verified SDK.
        sdk = Path(args.sdk).resolve()
        expected_cc = sdk / "gnu" / args.toolchain / "bin" / f"{args.toolchain}-gcc"
        checks.append(("CMAKE_C_COMPILER", cached("CMAKE_C_COMPILER"),
                       str(expected_cc)))
    if args.python:
        checks.append(("Python3_EXECUTABLE", cached("Python3_EXECUTABLE"),
                       str(Path(args.python).absolute())))
    for name, actual, expected in checks:
        if actual is None:
            raise BuildError(f"Effective-config check: {name} missing from cache")
        if Path(actual).resolve() != Path(expected).resolve() and actual != expected:
            raise BuildError(
                f"Effective-config mismatch for {name}: "
                f"cache={actual}, verified={expected}"
            )
    # Modules actually consumed by the build: identity, not just count.
    # Format per line: "name":"<module-root>":"<module-root>/zephyr"
    module_file = build_dir / "zephyr_modules.txt"
    effective = []
    if module_file.is_file():
        effective = [
            line.strip()
            for line in module_file.read_text().splitlines()
            if line.strip()
        ]
    expected_roots = {str(Path(m).resolve()) for m in modules}
    effective_roots = set()
    for entry in effective:
        parts = [seg.strip('"') for seg in entry.split('":')]
        root = parts[1] if len(parts) > 1 else parts[0]
        effective_roots.add(str(Path(root).resolve()))
    if effective_roots != expected_roots:
        raise BuildError(
            "Effective module set differs from the verified set: "
            f"build={sorted(effective_roots)}, verified={sorted(expected_roots)}"
        )
    print("[effective] source-dir/board/compiler/python and module identity verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, help="App/test directory inside the repo")
    parser.add_argument("--board", required=True, help="e.g. native_sim or a board name")
    parser.add_argument("--zephyr-base", required=True, help="Verified Zephyr checkout")
    parser.add_argument("--build-dir", help="Default build/r02-<board>")
    parser.add_argument("--sdk", help="Zephyr SDK root (board targets)")
    parser.add_argument("--sdk-version", help="Expected sdk_version content")
    parser.add_argument("--toolchain", help="e.g. riscv64-zephyr-elf (board targets)")
    parser.add_argument("--gcc-version", help="Expected target gcc version substring")
    parser.add_argument("--host-cc", default="cc", help="Host compiler (native targets)")
    parser.add_argument(
        "--python",
        help="Pinned build interpreter (e.g. the frozen venv python with pyelftools)",
    )
    parser.add_argument(
        "--esptool-python",
        help="Interpreter providing the esptool module (espressif targets); "
        "a managed shim is generated - the source venv is never modified",
    )
    parser.add_argument("--board-root", help="Out-of-tree board directory inside the repo")
    parser.add_argument(
        "--module",
        action="append",
        help="Explicit pinned module as name=path (repeatable; verified read-only)",
    )
    parser.add_argument("--cmake-arg", action="append", help="Extra -D args (repeatable)")
    parser.add_argument("--build", action="store_true", help="Also compile after configure")
    parser.add_argument("--dry-run", action="store_true", help="Preflight only")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    try:
        env, cmake_args, build_dir = preflight(root, args)
        if args.dry_run:
            print("[dry-run] " + " ".join(cmake_args))
            return 0
        run(cmake_args, env=env)
        print("[configure] " + " ".join(cmake_args))
        modules_arg = env["ZEPHYR_MODULES"].split(";") if env["ZEPHYR_MODULES"] else []
        verify_effective_config(build_dir, args, modules_arg)
        if args.build:
            run(["cmake", "--build", str(build_dir)], env=env)
            print(f"[build] {build_dir}")
    except BuildError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
