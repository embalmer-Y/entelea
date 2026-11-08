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
import sys
from pathlib import Path

NATIVE_BOARDS = ("native_sim", "native_sim/native/64", "native_sim/native/32")


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
    run([
        sys.executable,
        str(root / "tools" / "check_dependencies.py"),
        "--dependency", "zephyr",
        "--checkout", str(zephyr_base),
    ])

    env = dict(os.environ)
    env["ZEPHYR_BASE"] = str(zephyr_base)
    env["ZEPHYR_MODULES"] = ""  # explicit empty module set; no west workspace

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

    cmake_args = ["cmake", "-S", str(app), "-B", str(build_dir), "-G", "Ninja",
                  f"-DBOARD={args.board}"] + list(args.cmake_arg or [])
    print(f"[preflight] app={app}")
    print(f"[preflight] board={args.board} build={build_dir}")
    print(f"[preflight] zephyr={zephyr_base} modules= (explicit empty)")
    return env, cmake_args, build_dir


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
        if args.build:
            run(["cmake", "--build", str(build_dir)], env=env)
            print(f"[build] {build_dir}")
    except BuildError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
