"""Validate source classification, the R01 constraints and the restored L5 rules."""

import argparse
import json
import re
import sys
from pathlib import Path, PurePosixPath

OUTPUT = re.compile(
    # gpio_port_(set|clear|toggle)\w* covers every port-level write variant in
    # fixed Zephyr 4.5 (set_bits[_raw]/set_masked[_raw]/set_clr_bits[_raw]/
    # clear_bits[_raw]/toggle_bits); read APIs all start with "get".
    r"\b(?:gpio_pin_set(?:_raw|_dt)?|gpio_port_(?:set|clear|toggle)\w*"
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
# Restored mechanical safety rules of the legacy L5 checker that the rules
# below did not cover. They scan production sources only; the test trees
# carry the negative cases on purpose.
DESCRIPTOR_INIT = re.compile(r"\bentelea_out_channel_t\s+\w+\s*=\s*\{")
SAFE_STATE_FIELDS = (".poweron", ".linkloss", ".fault")
ESTOP_ENTRY_FILE = "framework/execution/safety/force.c"
ESTOP_DEFINITION = re.compile(r"\bentelea_safety_force_all_fault\s*\([^;{]*\)\s*\{")
# The estop path runs in interrupt context: its call graph has no allocation,
# blocking, queueing, logging or protocol-stack call.
ESTOP_FORBIDDEN = re.compile(
    r"\b(?:k_(?:malloc|calloc|free|mutex\w*|sem\w*|msgq\w*|fifo\w*|queue\w*"
    r"|work\w*|sleep)|net_\w+|zenoh\w*|printk)\b"
)
PROVISIONING_SUFFIX = "/config/prov.c"
PROVISIONING_WRITE = re.compile(r"\bentelea_storage_(?:write|erase\w*)\s*\(")
# App package slots are written only through the package store's stage chain,
# so their partition identifiers belong to the storage port and that store.
SLOT_PARTITION = re.compile(r"\bENTELEA_PART_SLOT_[AB]\b")
SLOT_PARTITION_FILES = {
    "framework/include/entelea/storage.h",
    "framework/services/storage/part.c",
    "framework/services/package/store.c",
}


def fail(condition, message):
    if not condition:
        raise ValueError(message)


def strip_comments(text):
    """Blank out // and /* */ comments, keeping every newline and column."""
    out = list(text)
    state = "code"
    i = 0
    while i < len(text):
        char = text[i]
        pair = text[i : i + 2]
        if state == "code":
            if pair in ("//", "/*"):
                state = "line" if pair == "//" else "block"
                out[i] = out[i + 1] = " "
                i += 2
                continue
            if char in "\"'":
                state = char
            i += 1
        elif state == "block":
            out[i] = "\n" if char == "\n" else " "
            if pair == "*/":
                out[i + 1] = " "
                state = "code"
                i += 2
                continue
            i += 1
        elif state == "line":
            if char == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
        else:  # inside a string or character literal
            if char == "\\":
                i += 1
            elif char == state:
                state = "code"
            i += 1
    return "".join(out)


def brace_block(text, start):
    """Text inside the braces that open at ``start`` (index just after ``{``)."""
    depth = 1
    for index in range(start, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            return text[start:index]
    return text[start:]


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def restored_safety_errors(texts, gates):
    """Violations of the restored L5 rules; ``texts`` maps paths to source."""
    code = {path: strip_comments(text) for path, text in texts.items()}
    errors = []
    for path, text in sorted(code.items()):
        for match in DESCRIPTOR_INIT.finditer(text):
            block = brace_block(text, match.end())
            missing = [field for field in SAFE_STATE_FIELDS if field not in block]
            if missing:
                errors.append(
                    f"Descriptor misses {', '.join(missing)}: "
                    f"{path}:{line_of(text, match.start())}"
                )
        if path.endswith(PROVISIONING_SUFFIX):
            for number, line in enumerate(text.split("\n"), 1):
                if PROVISIONING_WRITE.search(line):
                    errors.append(f"Provisioning write path: {path}:{number}")
        if path not in SLOT_PARTITION_FILES:
            for number, line in enumerate(text.split("\n"), 1):
                if SLOT_PARTITION.search(line):
                    errors.append(
                        f"App slot partition outside the package store: {path}:{number}"
                    )
    if ESTOP_ENTRY_FILE in code:
        text = code[ESTOP_ENTRY_FILE]
        definitions = list(ESTOP_DEFINITION.finditer(text))
        if not definitions:
            errors.append(
                f"Estop entry point not found (check ineffective): {ESTOP_ENTRY_FILE}"
            )
        for match in definitions:
            for forbidden in ESTOP_FORBIDDEN.finditer(brace_block(text, match.end())):
                errors.append(
                    f"Estop path uses {forbidden.group(0)}: {ESTOP_ENTRY_FILE}"
                )
    for path in sorted(set(gates)):
        for forbidden in ESTOP_FORBIDDEN.finditer(code.get(path, "")):
            errors.append(f"Estop path uses {forbidden.group(0)}: {path}")
    return errors


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
        for name in (
            "common",
            "compat",
            "contract_tests",
            "zephyr_framework",
            "platform_zephyr",
        )
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
        if group in {
            "common",
            "contract_tests",
            "zephyr_framework",
            "platform_zephyr",
        } or group.startswith("diagnostic:"):
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
                else "framework/runtime|execution|services/"
                if group == "zephyr_framework"
                else "platform/zephyr/"
            )
            if group == "zephyr_framework":
                ok = (
                    path.startswith("framework/runtime/")
                    or path.startswith("framework/execution/")
                    or path.startswith("framework/services/")
                )
            elif group == "platform_zephyr":
                ok = path.startswith("platform/zephyr/") or path.startswith(
                    "platform/linux/"
                )
            else:
                ok = path.startswith(expected)
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
    production = {}
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
            if "/tests/" not in path and group in {
                "zephyr_framework",
                "platform_zephyr",
            }:
                production[path] = text
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
    headers = {}
    for directory in (root / "framework/include/entelea", root / "platform"):
        for file in directory.rglob("*.h"):
            path = file.relative_to(root).as_posix()
            if "/tests/" not in path:
                headers[path] = file.read_text()
    for file in sorted(headers):
        if not file.startswith("framework/include/entelea/"):
            continue
        for include in re.findall(r'#\s*include\s*[<"]([^>"]+)', headers[file]):
            fail(
                include in {"stdbool.h", "stddef.h", "stdint.h"}
                or include.startswith("entelea/"),
                f"Non-public dependency in header {Path(file).name}: {include}",
            )
            if include.startswith("entelea/"):
                checked_path(root, "framework/include/" + include)
    errors = restored_safety_errors({**production, **headers}, gates)
    fail(not errors, errors[0] if errors else "")
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
        f"PASS: {count} exactly classified C sources; mechanical classification, "
        "output-gate and restored L5 constraints only, not physical safety"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
