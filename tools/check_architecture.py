"""Validate exact build source classification and applicable R01 constraints."""

import argparse
import json
import re
import sys
from pathlib import Path, PurePosixPath

OUTPUT = re.compile(
    r"\b(?:gpio_pin_set(?:_raw|_dt)?|gpio_port_set_bits_raw|gpio_port_clear_bits_raw"
    r"|pwm_set(?:_(?:cycles|duty|pulse|ticks))?(?:_dt)?)\s*\("
)
PRIVATE_INCLUDE = re.compile(
    r'#\s*include\s*[<"](?:zephyr/|pthread|sys/|net/|internal\.h|ts/)', re.IGNORECASE
)
DIRECT_CLOCK = re.compile(
    r"\b(?:k_uptime_\w+|clock_gettime|time|localtime|gmtime|srand|rand|random)\s*\("
)
# The driver ops table is the safety module's private write path; a public
# reference outside the module is an output-protection bypass (R02 defect 1).
DRIVER_TABLE = re.compile(r"\bentelea_drivers\b")


def fail(condition, message):
    if not condition:
        raise ValueError(message)


def checked_path(root, path):
    fail(isinstance(path, str) and path, "Empty source path")
    rel = PurePosixPath(path)
    fail(
        not rel.is_absolute() and ".." not in rel.parts and "\\" not in path,
        f"Unsafe source path: {path}",
    )
    file = root / path
    fail(
        file.resolve().is_relative_to(root.resolve()) and file.is_file(),
        f"Missing/escaping source: {path}",
    )
    return file


def validate(root, policy):
    fail(
        set(policy)
        == {
            "common",
            "compat",
            "contract_tests",
            "zephyr_framework",
            "platform_zephyr",
            "diagnostics",
            "protected_output_sources",
        },
        "Unknown/missing source classification",
    )
    groups = {
        name: policy[name]
        for name in ("common", "compat", "contract_tests", "zephyr_framework",
                     "platform_zephyr")
    }
    fail(isinstance(policy["diagnostics"], dict), "Diagnostics must name exact targets")
    for name, paths in policy["diagnostics"].items():
        fail(
            re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            "Invalid diagnostic target",
        )
        groups["diagnostic:" + name] = paths
    seen = set()
    group_of = {}
    for group, paths in groups.items():
        fail(isinstance(paths, list), f"Source group is not a list: {group}")
        if (
            group in {"common", "contract_tests", "zephyr_framework", "platform_zephyr"}
            or group.startswith("diagnostic:")
        ):
            fail(paths, f"Empty required source group: {group}")
        for path in paths:
            fail(path not in seen, f"Source classified twice: {path}")
            seen.add(path)
            group_of[path] = group
            file = checked_path(root, path)
            fail(file.suffix == ".c", f"Non-C build source: {path}")
            expected = (
                "tests/diagnostics/"
                if group.startswith("diagnostic:")
                else "tests/contracts/"
                if group == "contract_tests"
                else "framework/compat/"
                if group == "compat"
                else "framework/common/"
                if group == "common"
                else "framework/runtime/ or framework/execution/"
                if group == "zephyr_framework"
                else "platform/zephyr/"
            )
            ok = (
                path.startswith("framework/runtime/")
                or path.startswith("framework/execution/")
                if group == "zephyr_framework"
                else path.startswith(expected)
            )
            fail(ok, f"Source crosses target boundary: {path}")
    tracked_sources = set()
    for directory in ("framework", "platform", "products", "tests"):
        base = root / directory
        tracked_sources.update(
            p.relative_to(root).as_posix() for p in base.rglob("*.c")
        )
    fail(
        tracked_sources == seen,
        f"Unclassified/extra source paths: {sorted(tracked_sources ^ seen)}",
    )
    gates = policy["protected_output_sources"]
    fail(
        isinstance(gates, list) and len(gates) == len(set(gates)),
        "Invalid protection source list",
    )
    # R02: the only approved write path is the reviewed driver dispatch in the
    # Zephyr-built safety group. Gates must never cover host-built sources.
    for gate in gates:
        fail(
            gate in group_of
            and group_of[gate] in {"zephyr_framework", "platform_zephyr"},
            f"Protection gate outside reviewed Zephyr sources: {gate}",
        )
    for group, paths in groups.items():
        for path in paths:
            text = checked_path(root, path).read_text()
            if not group.startswith("diagnostic:") and path not in set(gates):
                fail(
                    OUTPUT.search(text) is None,
                    f"Output bypass outside protection: {path}",
                )
            if group == "common":
                fail(
                    PRIVATE_INCLUDE.search(text) is None, f"Private dependency: {path}"
                )
                for include in re.findall(r'#\s*include\s*[<"]([^>"]+)', text):
                    fail(
                        include in {"stdbool.h", "stddef.h", "stdint.h", "string.h"}
                        or include.startswith("entelea/"),
                        f"Unapproved common dependency: {path}:{include}",
                    )
                fail(
                    DIRECT_CLOCK.search(text) is None,
                    f"Direct clock/random source: {path}",
                )
            if group == "zephyr_framework":
                # Framework mechanisms consume the injected instance clock;
                # only the platform adapter may read the Zephyr uptime.
                fail(
                    DIRECT_CLOCK.search(text) is None,
                    f"Direct clock/random source: {path}",
                )
            if not path.startswith("framework/execution/safety/"):
                fail(
                    DRIVER_TABLE.search(text) is None,
                    f"Driver table bypass outside safety module: {path}",
                )
    headers = root / "framework/include/entelea"
    for file in headers.rglob("*.h"):
        for include in re.findall(r'#\s*include\s*[<"]([^>"]+)', file.read_text()):
            fail(
                include in {"stdbool.h", "stddef.h", "stdint.h"}
                or include.startswith("entelea/"),
                f"Non-public dependency in header {file.name}: {include}",
            )
            if include.startswith("entelea/"):
                checked_path(root, "framework/include/" + include)
    return len(seen)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    args = parser.parse_args()
    try:
        policy = json.loads(
            (args.root / "build-support/source-policy.json").read_text()
        )
        count = validate(args.root, policy)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        f"PASS: {count} exactly classified C sources; mechanical classification "
        "and output-gate constraints only, not physical safety"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
