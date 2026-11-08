"""Compare migrated primitives with fixed legacy C sources and independent vectors."""

import argparse
import ctypes
import hashlib
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

BASE = "e54047b51519fa449dfe4a338af8d126bed8fac4"
CBOR_NAMES = [
    "rd_init",
    "map_open",
    "array_open",
    "tstr",
    "bstr_ref",
    "uint",
    "int",
    "bool",
    "skip",
    "put_map",
    "put_array",
    "put_tstr",
    "put_tstrn",
    "put_bstr",
    "put_uint",
    "put_int",
    "put_bool",
]
SHA_NAMES = ["init", "update", "final"]


class Reader(ctypes.Structure):
    _fields_ = [("p", ctypes.POINTER(ctypes.c_uint8)), ("rem", ctypes.c_size_t)]


class Digest(ctypes.Structure):
    _fields_ = [
        ("h", ctypes.c_uint32 * 8),
        ("len", ctypes.c_uint64),
        ("buf", ctypes.c_uint8 * 64),
        ("buf_len", ctypes.c_uint32),
    ]


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, check=False, **kwargs)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors="replace"))
    return result.stdout


def function(lib, name, arguments, result):
    fn = getattr(lib, name)
    fn.argtypes = arguments
    fn.restype = result
    return fn


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(lib, prefix, data):
    raw = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    out = (ctypes.c_uint8 * 32)()
    fn = function(
        lib,
        prefix + "ts_sha256",
        [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p],
        None,
    )
    fn(raw, len(data), out)
    ctx = Digest()
    init = function(lib, prefix + "ts_sha256_init", [ctypes.POINTER(Digest)], None)
    update = function(
        lib,
        prefix + "ts_sha256_update",
        [ctypes.POINTER(Digest), ctypes.c_void_p, ctypes.c_uint32],
        None,
    )
    final = function(
        lib, prefix + "ts_sha256_final", [ctypes.POINTER(Digest), ctypes.c_void_p], None
    )
    init(ctypes.byref(ctx))
    for offset in range(0, len(data), 17):
        update(
            ctypes.byref(ctx), ctypes.byref(raw, offset), min(17, len(data) - offset)
        )
    stream = (ctypes.c_uint8 * 32)()
    final(ctypes.byref(ctx), stream)
    require(bytes(out) == bytes(stream), "Streaming/one-shot mismatch")
    return bytes(out)


def encode(lib, prefix, kind, value):
    out = (ctypes.c_uint8 * 128)()
    pos = ctypes.c_size_t(0)
    argtype = {
        "uint": ctypes.c_uint64,
        "int": ctypes.c_int64,
        "bool": ctypes.c_bool,
        "map": ctypes.c_uint32,
        "array": ctypes.c_uint32,
    }[kind]
    fn = function(
        lib,
        prefix + "ts_cbor_put_" + kind,
        [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t), argtype],
        ctypes.c_bool,
    )
    require(
        fn(out, len(out), ctypes.byref(pos), value), "Encoding rejected valid input"
    )
    return bytes(out[: pos.value])


def decode(lib, prefix, kind, raw):
    buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
    reader = Reader()
    init = function(
        lib,
        prefix + "ts_cbor_rd_init",
        [ctypes.POINTER(Reader), ctypes.c_void_p, ctypes.c_size_t],
        None,
    )
    init(ctypes.byref(reader), buf, len(raw))
    ctype = {
        "uint": ctypes.c_uint64,
        "int": ctypes.c_int64,
        "bool": ctypes.c_bool,
        "map": ctypes.c_uint32,
        "array": ctypes.c_uint32,
    }[kind]
    value = ctype()
    name = kind + "_open" if kind in {"map", "array"} else kind
    fn = function(
        lib,
        prefix + "ts_cbor_" + name,
        [ctypes.POINTER(Reader), ctypes.POINTER(ctype)],
        ctypes.c_bool,
    )
    require(
        fn(ctypes.byref(reader), ctypes.byref(value)) and reader.rem == 0,
        "Decoding rejected valid input",
    )
    return value.value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        with tempfile.TemporaryDirectory(prefix="entelea-oracle-") as directory:
            temp = Path(directory)
            files = []
            for name, source in (
                ("cbor.c", "firmware/module/tessera/src/net/cbor_min.c"),
                ("sha.c", "firmware/module/tessera/src/store/sha256.c"),
            ):
                data = run(["git", "-C", str(root), "show", f"{BASE}:{source}"])
                require(
                    data.count(b'#include "internal.h"') == 1,
                    "Legacy extraction mismatch",
                )
                file = temp / name
                file.write_bytes(
                    data.replace(b'#include "internal.h"', b"#include <ts/common.h>")
                )
                files.append(str(file))
            flags = [
                args.cc,
                "-shared",
                "-fPIC",
                "-std=c11",
                "-I" + str(root / "framework/include"),
                "-I" + str(root / "framework/compat/include"),
            ]
            renames = [f"-Dts_cbor_{name}=old_ts_cbor_{name}" for name in CBOR_NAMES]
            renames += [
                f"-Dts_sha256_{name}=old_ts_sha256_{name}" for name in SHA_NAMES
            ]
            renames += ["-Dts_sha256=old_ts_sha256"]
            run(flags + renames + files + ["-o", str(temp / "old.so")])
            run(
                flags
                + [
                    str(root / p)
                    for p in (
                        "framework/common/sha256.c",
                        "framework/common/cbor.c",
                        "framework/compat/common.c",
                    )
                ]
                + ["-o", str(temp / "new.so")]
            )
            old = ctypes.CDLL(str(temp / "old.so"))
            new = ctypes.CDLL(str(temp / "new.so"))
            invalid = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    (
                        "import ctypes, resource; "
                        "resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); "
                        f"lib = ctypes.CDLL({str(temp / 'new.so')!r}); "
                        "lib.ts_sha256_init(None)"
                    ),
                ],
                capture_output=True,
                check=False,
            )
            require(
                invalid.returncode == -signal.SIGABRT
                and b"legacy void API precondition violated" in invalid.stderr,
                "Legacy void API invalid input did not fail explicitly",
            )
            vectors = [b"", b"abc", bytes(range(256)), b"a" * 1000000]
            vectors += [
                bytes((i * 13) % 256 for i in range(n))
                for n in (55, 56, 63, 64, 65, 127, 128)
            ]
            for data in vectors:
                require(
                    sha(old, "old_", data)
                    == sha(new, "", data)
                    == hashlib.sha256(data).digest(),
                    f"SHA legacy/hashlib mismatch at {len(data)} bytes",
                )
            cases = {
                "uint": [
                    0,
                    23,
                    24,
                    255,
                    256,
                    65535,
                    65536,
                    2**32 - 1,
                    2**32,
                    2**64 - 1,
                ],
                "int": [-(2**63), -65536, -25, -24, -1, 0, 24, 2**63 - 1],
                "bool": [False, True],
                "map": [0, 1, 16],
                "array": [0, 1, 64],
            }
            for kind, values in cases.items():
                for value in values:
                    raw = encode(old, "old_", kind, value)
                    require(raw == encode(new, "", kind, value), "CBOR byte mismatch")
                    require(
                        decode(old, "old_", kind, raw)
                        == decode(new, "", kind, raw)
                        == value,
                        "CBOR value mismatch",
                    )
            for prefix, lib in (("old_", old), ("", new)):
                out = (ctypes.c_uint8 * 128)()
                pos = ctypes.c_size_t()
                put = function(
                    lib,
                    prefix + "ts_cbor_put_tstrn",
                    [
                        ctypes.c_void_p,
                        ctypes.c_size_t,
                        ctypes.POINTER(ctypes.c_size_t),
                        ctypes.c_char_p,
                        ctypes.c_size_t,
                    ],
                    ctypes.c_bool,
                )
                require(
                    put(out, len(out), ctypes.byref(pos), b"hello", 5), "tstr rejected"
                )
                require(bytes(out[: pos.value]) == b"\x65hello", "tstr byte mismatch")
                put_bytes = function(
                    lib,
                    prefix + "ts_cbor_put_bstr",
                    [
                        ctypes.c_void_p,
                        ctypes.c_size_t,
                        ctypes.POINTER(ctypes.c_size_t),
                        ctypes.c_void_p,
                        ctypes.c_uint32,
                    ],
                    ctypes.c_bool,
                )
                pos.value = 0
                require(
                    put_bytes(out, len(out), ctypes.byref(pos), b"\x00\xff", 2),
                    "bstr rejected",
                )
                require(
                    bytes(out[: pos.value]) == b"\x42\x00\xff", "bstr byte mismatch"
                )
    except (OSError, ValueError, AttributeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print("PASS: fixed legacy C / migrated ts symbols / hashlib and CBOR byte vectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
