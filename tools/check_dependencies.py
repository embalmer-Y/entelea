"""Check pinned dependency metadata and optionally a read-only local checkout."""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

PINS = {
    "zephyr": "41d10b59730406bbab4ca9f1fbbe9e96aafb4966",
    "wamr": "25bd7eb63e828e4bd242cc9b38d260b4b31c6605",
    "hal_espressif": "cc81db00233af6020f2fea9fbb0d0098fb3d0d86",
    # F 批真网/真云模块（esp_hosted RPC 编解码；host TLS 库——
    # Zephyr 模块树同源）；新增模块须同步此处+json。
    "nanopb": "0f6d088e777a33dd7b54297f137ef10d745c5da8",
    "mbedtls": "c43f34b93797e81fd0257c30004dcd0ae332ae51",
    "tf_psa_crypto": "765af96a20a2b408474dcd950372ba9af4d26b62",
}
APPROVED = [
    "patches/zephyr-p4-dsi-frame-size.patch",
    "patches/zephyr-esp32-gpio-base.patch",
    "patches/zephyr-cst8xx-report.patch",
    "patches/zephyr-c5-scan.patch",
    "patches/zephyr-p4-ram-digest.patch",
    "patches/zephyr-task-wdt-setup.patch",
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def git(repo, *args):
    result = subprocess.run(
        ["git", "-C", str(repo), *args], capture_output=True, check=False
    )
    require(
        result.returncode == 0, f"Dependency Git operation failed: {' '.join(args)}"
    )
    return result.stdout


def normalize_diff(data):
    return b"\n".join(
        line for line in data.splitlines() if not line.startswith(b"index ")
    ).strip()


def verify_checkout(repo, commit, expected_diff):
    require(
        git(repo, "rev-parse", "HEAD").decode().strip() == commit,
        "Dependency revision mismatch",
    )
    require(
        not git(repo, "diff", "--cached", "--name-only"), "Staged dependency changes"
    )
    require(
        not git(repo, "ls-files", "--others", "--exclude-standard"),
        "Untracked dependency changes",
    )
    actual = git(
        repo,
        "diff",
        "--no-ext-diff",
        "--no-color",
        "--no-textconv",
        "--unified=3",
        "--binary",
        "HEAD",
    )
    require(
        normalize_diff(actual) == normalize_diff(expected_diff),
        "Dependency differs from complete approved patch set",
    )
    require(
        not git(repo, "diff", "--ignore-submodules=none", "--summary"),
        "Dependency file modes changed",
    )


def validate(root, data):
    require(data["schema_version"] == 1, "Unsupported dependency schema")
    deps = data["dependencies"]
    require(
        len(deps) == len(PINS) and {d["id"] for d in deps} == set(PINS),
        "Unknown/missing dependency",
    )
    inventory = json.loads((root / "docs/migration-assets.json").read_text())
    sources = {
        f["path"]: f for f in inventory["source_files"] if f["repository"] == "legacy"
    }
    for dep in deps:
        require(
            dep["commit"] == PINS[dep["id"]], f"Dependency pin changed: {dep['id']}"
        )
        paths = [p["path"] for p in dep["patches"]]
        require(
            paths == (APPROVED if dep["id"] == "zephyr" else []),
            "Unapproved patch selection/order",
        )
        for patch in dep["patches"]:
            path = patch["path"]
            require(
                patch["sha256"] == sources[path]["sha256"],
                f"Patch baseline identity changed: {path}",
            )
            require(
                hashlib.sha256((root / path).read_bytes()).hexdigest()
                == patch["sha256"],
                f"Patch content changed: {path}",
            )
    return {d["id"]: d for d in deps}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency", choices=sorted(PINS))
    parser.add_argument("--checkout", type=Path)
    args = parser.parse_args()
    if bool(args.dependency) != bool(args.checkout):
        parser.error("--dependency and --checkout must be supplied together")
    root = Path(__file__).resolve().parents[1]
    try:
        deps = validate(
            root, json.loads((root / "build-support/dependencies.json").read_text())
        )
        if args.checkout:
            dep = deps[args.dependency]
            expected = b"".join((root / p["path"]).read_bytes() for p in dep["patches"])
            verify_checkout(args.checkout, dep["commit"], expected)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        "PASS: pinned metadata/patches"
        + (
            " and exact read-only checkout"
            if args.checkout
            else "; no dependency checkout/build claimed"
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
