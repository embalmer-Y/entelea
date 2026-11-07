import base64
import runpy
import sys
import types
import zlib
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "board/p4cameraidf/capture.py"
TARGET = "/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B79068007-if00"
FRAME = bytes((0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF))
CHECKSUM = zlib.adler32(FRAME) & 0xFFFFFFFF


def lines(frame=FRAME, trailer=None, cleanup=b"P4CAM END"):
    header = f"P4CAM FRAME_BEGIN width=2 height=2 bytes=8 adler32={CHECKSUM:08x} format=RGB565"
    if trailer is None:
        trailer = f"P4CAM FRAME_END adler32={CHECKSUM:08x}".encode()
    return [header.encode(), base64.b64encode(frame), trailer, cleanup]


def run_capture(monkeypatch, output, incoming, *, fuser_result=1):
    observed = {}

    class Port:
        def __init__(self, **kwargs):
            self.lines = iter(incoming)
            self.closed = False
            observed["port"] = self
            observed["kwargs"] = kwargs

        def open(self):
            pass

        def reset_input_buffer(self):
            pass

        def readline(self):
            return next(self.lines) + b"\n"

        def close(self):
            self.closed = True

    class Preview:
        def save(self, file, format):
            assert format == "PNG"
            file.write(b"PNG fixture")

        def getextrema(self):
            return ((0, 255),) * 3

    def frombytes(mode, size, pixels):
        observed["pixels"] = (mode, size, pixels)
        return Preview()

    fake_pil = types.ModuleType("PIL")
    fake_pil.Image = types.SimpleNamespace(frombytes=frombytes)
    fake_pil.ImageStat = types.SimpleNamespace(
        Stat=lambda image: types.SimpleNamespace(stddev=[1] * 3)
    )
    monkeypatch.setitem(sys.modules, "PIL", fake_pil)
    monkeypatch.setitem(sys.modules, "serial", types.SimpleNamespace(Serial=Port))
    monkeypatch.setattr(sys, "argv", [str(SCRIPT), "--output", str(output)])
    monkeypatch.setattr("time.sleep", lambda _: None)
    monkeypatch.setattr("os.umask", lambda _: 0)
    monkeypatch.setattr(
        "subprocess.run", lambda *args, **kwargs: types.SimpleNamespace(returncode=fuser_result)
    )
    real_exists = Path.exists
    real_resolve = Path.resolve
    monkeypatch.setattr(
        Path, "exists", lambda path: True if str(path) == TARGET else real_exists(path)
    )
    monkeypatch.setattr(
        Path,
        "resolve",
        lambda path, *args, **kwargs: (
            Path("/dev/ttyACM0") if str(path) == TARGET else real_resolve(path, *args, **kwargs)
        ),
    )
    runpy.run_path(str(SCRIPT), run_name="__main__")
    return observed


def test_capture_verifies_frame_and_cleanup_before_creating_png(monkeypatch, tmp_path):
    output = tmp_path / "preview.png"
    observed = run_capture(monkeypatch, output, lines())

    assert output.read_bytes() == b"PNG fixture"
    assert observed["kwargs"]["exclusive"] is True
    assert observed["port"].closed
    assert observed["pixels"] == (
        "RGB",
        (2, 2),
        bytes((255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255)),
    )


@pytest.mark.parametrize(
    "incoming, message",
    [
        (lines(frame=bytes(8)), "Frame integrity failure"),
        (lines(trailer=b"P4CAM FRAME_END adler32=00000000"), "Unexpected frame trailer"),
        (lines(cleanup=b"P4CAM FAIL cleanup_stream"), "acquisition cleanup error"),
    ],
)
def test_capture_rejects_bad_frame_or_cleanup(monkeypatch, tmp_path, incoming, message):
    output = tmp_path / "preview.png"
    with pytest.raises(RuntimeError, match=message):
        run_capture(monkeypatch, output, incoming)
    assert not output.exists()


def test_capture_refuses_occupied_port_and_existing_output(monkeypatch, tmp_path):
    output = tmp_path / "preview.png"
    with pytest.raises(RuntimeError, match="occupied"):
        run_capture(monkeypatch, output, lines(), fuser_result=0)
    output.write_bytes(b"existing")
    with pytest.raises(RuntimeError, match="overwrite"):
        run_capture(monkeypatch, output, lines())
    assert output.read_bytes() == b"existing"


def test_capture_refuses_repository_output(monkeypatch):
    with pytest.raises(ValueError, match="outside the repository"):
        run_capture(monkeypatch, SCRIPT.parent / "private.png", lines())
