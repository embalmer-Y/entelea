import array
import runpy
import sys
import types
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "board/p4audiopc/experiment.py"
TARGET = "/dev/serial/by-id/usb-1a86_USB_Single_Serial_5B79068007-if00"


def run_experiment(monkeypatch, incoming, *, fuser_result=1, playback_result=0, observed=None):
    if observed is None:
        observed = {}

    class Port:
        def __init__(self, **kwargs):
            self.lines = iter(incoming)
            self.closed = False
            observed["port"] = self
            observed["serial_kwargs"] = kwargs

        def open(self):
            pass

        def reset_input_buffer(self):
            pass

        def readline(self):
            return (next(self.lines) + "\n").encode()

        def close(self):
            self.closed = True

    class Player:
        def __init__(self, command, **kwargs):
            self.command = command
            self.returncode = None
            observed["player"] = self

        def communicate(self, pcm, timeout):
            self.pcm = pcm
            self.returncode = playback_result
            return b"", b"fixture playback failure" if playback_result else b""

        def poll(self):
            return self.returncode

        def kill(self):
            self.returncode = -9

        def wait(self):
            return self.returncode

    monkeypatch.setitem(sys.modules, "serial", types.SimpleNamespace(Serial=Port))
    monkeypatch.setattr("time.sleep", lambda _: None)
    monkeypatch.setattr("os.umask", lambda _: 0)
    monkeypatch.setattr(
        "subprocess.run", lambda *args, **kwargs: types.SimpleNamespace(returncode=fuser_result)
    )
    monkeypatch.setattr("subprocess.Popen", Player)
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


def successful_lines():
    return (
        ["P4MIC READY rate=16000 amp=0 dac_muted=1 windows=40"]
        + [
            f"P4MIC WINDOW i={index} ch={ch} n=8192 ac=0 f1000=0 f2000=0"
            for index in range(40)
            for ch in range(2)
        ]
        + ["P4MIC END rc=0 amp_rc=0 drop_rc=0"]
    )


def test_synthesized_tones_and_audio_cleanup(monkeypatch):
    observed = run_experiment(monkeypatch, successful_lines())

    assert observed["port"].closed
    assert observed["serial_kwargs"]["exclusive"] is True
    assert observed["player"].returncode == 0
    pcm = array.array("h")
    pcm.frombytes(observed["player"].pcm)
    if sys.byteorder != "little":
        pcm.byteswap()
    assert len(pcm) == (32768 * 3 + 65536 * 2) * 2
    assert all(value == 0 for value in pcm[: 32768 * 2])
    assert pcm[32768 * 2 + 8] == 2000
    assert pcm[32768 * 2 + 9] == 2000
    assert pcm[(32768 * 2 + 65536) * 2 + 4] == 2000
    assert pcm[(32768 * 2 + 65536) * 2 + 5] == 2000
    assert max(map(abs, pcm)) == 2000


def test_board_failure_closes_port_without_starting_playback(monkeypatch):
    observed = {}
    with pytest.raises(RuntimeError, match="audio diagnostic failure"):
        run_experiment(monkeypatch, ["P4MIC FAIL codec_id=0000 rc=-1"], observed=observed)
    assert observed["port"].closed
    assert "player" not in observed


def test_failed_playback_is_not_success(monkeypatch):
    with pytest.raises(RuntimeError, match="playback failed"):
        run_experiment(monkeypatch, successful_lines(), playback_result=1)


def test_occupied_device_is_rejected(monkeypatch):
    with pytest.raises(RuntimeError, match="occupied"):
        run_experiment(monkeypatch, successful_lines(), fuser_result=0)


def test_missing_audio_window_is_rejected(monkeypatch):
    incoming = successful_lines()
    incoming.pop(2)
    with pytest.raises(RuntimeError, match="Incomplete trial"):
        run_experiment(monkeypatch, incoming)
