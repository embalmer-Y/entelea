# SPDX-License-Identifier: Apache-2.0
"""Host wire, privacy and live controlled DNS/TLS service regressions."""

import contextlib
import runpy
import socket
import ssl
import struct
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
MODULE = runpy.run_path(str(REPO / "tools/board/p4_net_probe.py"))
HOSTNAME = MODULE["HOSTNAME"]


def query(name=HOSTNAME, kind=1):
    result = struct.pack("!6H", 4321, 0x100, 1, 0, 0, 0)
    result += b"".join(bytes([len(label)]) + label.encode() for label in name.split("."))
    return result + b"\x00" + struct.pack("!HH", kind, 1)


def test_dns_exact_authoritative_answer_and_nxdomain():
    answer = MODULE["dns_response"](query(), "192.0.2.1")
    assert struct.unpack("!6H", answer[:12]) == (4321, 0x8500, 1, 1, 0, 0)
    assert answer[-4:] == bytes([192, 0, 2, 1])
    missing = MODULE["dns_response"](query("missing." + HOSTNAME), "192.0.2.1")
    assert struct.unpack("!6H", missing[:12]) == (4321, 0x8503, 1, 0, 0, 0)
    unsupported = MODULE["dns_response"](query(kind=28), "192.0.2.1")
    assert struct.unpack("!6H", unsupported[:12])[1] & 15 == 4


@pytest.mark.parametrize(
    "packet", [b"", bytes(11), query()[:-1], query() + bytes(1), bytes(513),
                struct.pack("!6H", 1, 0x8000, 1, 0, 0, 0) + query()[12:],
                struct.pack("!6H", 1, 0, 1, 0, 0, 0) + b"\xc0\x0c\x00\x01\x00\x01"],
)
def test_invalid_dns_shape_is_rejected(packet):
    with pytest.raises(ValueError):
        MODULE["dns_response"](packet, "192.0.2.1")


def test_endpoint_frame_matches_actual_firmware_header_layout():
    ca = bytes(range(128))
    ports = [50001, 50002, 50003, 50004]
    result = MODULE["endpoint_frame"]("192.0.2.1", ports, ca, 1700000000)
    assert len(result) == 27 + len(HOSTNAME) + len(ca)
    assert result[:8] == b"P4T1\xc0\x00\x02\x01"
    assert struct.unpack("!4HQHB", result[8:27]) == (*ports, 1700000000, 128, len(HOSTNAME))
    assert result[27:] == HOSTNAME.encode() + ca


@pytest.mark.parametrize(
    ("address", "ports", "ca", "seconds"),
    [
        ("127.0.0.1", [1, 2, 3, 4], b"ca", 1700000000),
        ("0.0.0.0", [1, 2, 3, 4], b"ca", 1700000000),
        ("224.0.0.1", [1, 2, 3, 4], b"ca", 1700000000),
        ("192.0.2.1", [1, 2, 3], b"ca", 1700000000),
        ("192.0.2.1", [1, 2, 3, 0], b"ca", 1700000000),
        ("192.0.2.1", [1, 2, 3, 65536], b"ca", 1700000000),
        ("192.0.2.1", [1, 2, 3, 4], b"", 1700000000),
        ("192.0.2.1", [1, 2, 3, 4], bytes(1537), 1700000000),
        ("192.0.2.1", [1, 2, 3, 4], b"ca", 0),
    ],
)
def test_invalid_endpoint_contract_is_rejected(address, ports, ca, seconds):
    with pytest.raises(ValueError):
        MODULE["endpoint_frame"](address, ports, ca, seconds)


def test_credentials_and_addresses_are_redacted_in_supported_log_encodings():
    ssid, psk = "\u6d4b\u8bd5 SSID".encode(), b'quoted"password'
    import json

    raw = b" ".join((ssid, psk, ssid.hex().encode(), psk.hex().encode(),
                     json.dumps(ssid.decode())[1:-1].encode(),
                     json.dumps(psk.decode())[1:-1].encode(), b"192.168.123.234"))
    output = MODULE["redact"](raw, ssid, psk)
    assert ssid not in output and psk not in output
    assert ssid.hex().encode() not in output and psk.hex().encode() not in output
    assert b"192.168.123.234" not in output


def test_live_controlled_services_verify_real_certificates_and_terminate(tmp_path):
    MODULE["certificates"](tmp_path)
    servers = []
    dns = MODULE["DNSServer"]("127.0.0.1", {"127.0.0.1"})
    with contextlib.ExitStack() as stack:
        MODULE["start_server"](dns, stack)
        for kind in ("valid", "untrusted", "expired"):
            server = MODULE["TLSServer"](
                "127.0.0.1", {"127.0.0.1"}, MODULE["server_context"](tmp_path, kind), kind,
            )
            MODULE["start_server"](server, stack)
            servers.append(server)
        MODULE["preflight"]("127.0.0.1", dns, servers, tmp_path)
    assert dns.errors == []
    assert all(server.errors == [] for server in servers)
    assert dns.socket.fileno() == -1
    assert all(server.socket.fileno() == -1 for server in servers)
    assert all(path.stat().st_mode & 0o077 == 0 for path in tmp_path.glob("*.key"))


def test_live_tls_faults_are_one_shot_and_normal_echo_recovers(tmp_path):
    MODULE["certificates"](tmp_path)
    server = MODULE["TLSServer"](
        "127.0.0.1", {"127.0.0.1"}, MODULE["server_context"](tmp_path, "valid"), "valid",
    )
    context = ssl.create_default_context(cafile=str(tmp_path / "ca.crt"))
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    with contextlib.ExitStack() as stack:
        MODULE["start_server"](server, stack)
        server.prepare_stall()
        with pytest.raises(RuntimeError, match="not consumed"):
            server.prepare_stall()
        outgoing = ssl.MemoryBIO()
        client = context.wrap_bio(ssl.MemoryBIO(), outgoing, server_hostname=HOSTNAME)
        with pytest.raises(ssl.SSLWantReadError):
            client.do_handshake()
        hello = outgoing.read()
        assert hello[0] == 22 and hello[5] == 1
        with socket.create_connection(server.server_address, timeout=3) as raw:
            raw.sendall(hello[:3])
            raw.sendall(hello[3:])
            raw.shutdown(socket.SHUT_WR)
            assert raw.recv(1) == b""
        assert server.stalls == 1
        assert not server.consume_stall()
        with socket.create_connection(server.server_address, timeout=3) as raw:
            with context.wrap_socket(raw, server_hostname=HOSTNAME) as client:
                client.sendall(b"P4")
                client.sendall(b"FD")
                assert client.recv(1) == b""
                plain = client.unwrap()
                plain.close()
        assert server.drops == 1
        with socket.create_connection(server.server_address, timeout=3) as raw:
            with context.wrap_socket(raw, server_hostname=HOSTNAME) as client:
                payload = bytes(range(256))
                client.sendall(payload)
                reply = bytearray()
                while len(reply) < len(payload):
                    chunk = client.recv(len(payload) - len(reply))
                    assert chunk
                    reply += chunk
                assert reply == payload
    assert server.errors == []
    assert server.socket.fileno() == -1


@pytest.mark.parametrize(
    "payload", [b"", b"not-a-tls-client-hello", b"\x16\x03\x03\x00\x01\x01",
                b"\x16\x03\x03\x00\x04"],
)
def test_injected_stall_does_not_count_absent_or_invalid_client_hello(tmp_path, payload):
    MODULE["certificates"](tmp_path)
    server = MODULE["TLSServer"](
        "127.0.0.1", {"127.0.0.1"}, MODULE["server_context"](tmp_path, "valid"), "valid",
    )
    with contextlib.ExitStack() as stack:
        MODULE["start_server"](server, stack)
        server.prepare_stall()
        with socket.create_connection(server.server_address, timeout=3) as raw:
            raw.sendall(payload)
            raw.shutdown(socket.SHUT_WR)
            assert raw.recv(1) == b""
    assert server.stalls == 0
    assert len(server.errors) == 1 and "ClientHello" in server.errors[0]
