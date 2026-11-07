# SPDX-License-Identifier: Apache-2.0
"""Explicitly authorized UART/DNS/TLS probe; never flashes or calls a model."""

import argparse
import contextlib
import datetime
import json
import os
import re
import socket
import socketserver
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HOSTNAME = "p03.entelea.test"
REPO = Path(__file__).resolve().parents[2]


def dns_response(query: bytes, address: str) -> bytes:
    if len(query) < 12 or len(query) > 512:
        raise ValueError("Truncated DNS header")
    identifier, flags, questions, answers, authority, additional = struct.unpack("!6H", query[:12])
    if flags & 0xF800 or questions != 1 or answers or authority or additional:
        raise ValueError("Unsupported DNS query shape")
    offset = 12
    labels = []
    while True:
        if offset >= len(query):
            raise ValueError("Truncated DNS name")
        size = query[offset]
        offset += 1
        if not size:
            break
        if size > 63 or offset + size > len(query):
            raise ValueError("Invalid DNS label")
        labels.append(query[offset:offset + size].decode("ascii"))
        offset += size
        if offset - 12 > 255:
            raise ValueError("DNS name exceeds its protocol limit")
    if offset + 4 != len(query):
        raise ValueError("Invalid DNS question length")
    kind, dns_class = struct.unpack("!HH", query[offset:])
    name = ".".join(labels).lower()
    code = 3 if name != HOSTNAME else (0 if kind == 1 and dns_class == 1 else 4)
    answer = (
        b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 30, 4) + socket.inet_aton(address)
        if code == 0 else b""
    )
    return struct.pack("!6H", identifier, 0x8400 | (flags & 0x100) | code,
                       1, bool(answer), 0, 0) + query[12:] + answer


def endpoint_frame(address: str, ports: list[int], ca: bytes, seconds: int) -> bytes:
    if len(ports) != 4 or any(not 1 <= port <= 65535 for port in ports):
        raise ValueError("Four nonzero DNS/TLS ports are required")
    if not 1 <= len(ca) <= 1536 or not 1577836800 <= seconds <= 4102444800:
        raise ValueError("CA or trusted UTC is outside the diagnostic contract")
    packed = socket.inet_aton(address)
    if packed[0] in (0, 127) or packed[0] >= 224:
        raise ValueError("Controlled endpoint must be reachable unicast IPv4")
    hostname = HOSTNAME.encode("ascii")
    return b"P4T1" + packed + struct.pack("!4HQHB", *ports, seconds, len(ca), len(hostname)) \
        + hostname + ca


def openssl(directory: Path, *arguments: str) -> None:
    result = subprocess.run(
        ["openssl", *arguments], cwd=directory, capture_output=True, timeout=30,
    )
    if result.returncode:
        raise RuntimeError("OpenSSL failed: " + result.stderr.decode(errors="replace"))


def certificates(directory: Path) -> None:
    for name in ("ca", "other-ca", "server"):
        openssl(directory, "ecparam", "-name", "prime256v1", "-genkey", "-noout",
                "-out", f"{name}.key")
    for name in ("ca", "other-ca"):
        openssl(directory, "req", "-x509", "-new", "-key", f"{name}.key",
                "-sha256", "-days", "30", "-subj", f"/CN=Entelea P03 Test {name}",
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-out", f"{name}.crt")
    openssl(directory, "req", "-new", "-key", "server.key", "-subj", "/CN=Entelea P03 Test",
            "-out", "server.csr")
    extensions = (
        "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
        f"extendedKeyUsage=serverAuth\nsubjectAltName=DNS:{HOSTNAME}\n"
    )
    (directory / "server.ext").write_text(extensions)
    for name, issuer in (("valid", "ca"), ("untrusted", "other-ca")):
        openssl(directory, "x509", "-req", "-in", "server.csr", "-CA", f"{issuer}.crt",
                "-CAkey", f"{issuer}.key", "-set_serial", "1001", "-sha256", "-days", "2",
                "-extfile", "server.ext", "-out", f"{name}.crt")
    (directory / "index").touch()
    (directory / "serial").write_text("1002\n")
    (directory / "issued").mkdir()
    (directory / "ca.conf").write_text(
        "[ca]\ndefault_ca=p03\n[p03]\ndatabase=index\nserial=serial\n"
        "new_certs_dir=issued\ncertificate=ca.crt\nprivate_key=ca.key\n"
        "default_md=sha256\npolicy=policy\nunique_subject=no\n"
        "[policy]\ncommonName=supplied\n[server]\n" + extensions
    )
    now = datetime.datetime.now(datetime.UTC)
    openssl(directory, "ca", "-batch", "-config", "ca.conf", "-extensions", "server",
            "-startdate", (now - datetime.timedelta(days=2)).strftime("%Y%m%d%H%M%SZ"),
            "-enddate", (now - datetime.timedelta(days=1)).strftime("%Y%m%d%H%M%SZ"),
            "-in", "server.csr", "-out", "expired.crt")
    openssl(directory, "x509", "-in", "ca.crt", "-outform", "DER", "-out", "ca.der")
    for path in directory.iterdir():
        if path.is_file():
            path.chmod(0o600)


def server_context(directory: Path, kind: str) -> ssl.SSLContext:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")
    context.set_ecdh_curve("prime256v1")
    context.load_cert_chain(directory / f"{kind}.crt", directory / "server.key")
    return context


class DNSHandler(socketserver.BaseRequestHandler):
    def handle(self):
        server = self.server
        assert isinstance(server, DNSServer)
        if self.client_address[0] not in server.allowed:
            print("Controlled DNS refused an unexpected peer", flush=True)
            return
        try:
            answer = dns_response(self.request[0], server.address)
        except (ValueError, UnicodeDecodeError) as error:
            server.errors.append(f"Invalid controlled DNS query: {error}")
            print(server.errors[-1], file=sys.stderr, flush=True)
            return
        if self.client_address[0] != server.address:
            code = struct.unpack_from("!H", answer, 2)[0] & 15
            if code == 0:
                server.device_answers += 1
            elif code == 3:
                server.device_nxdomain += 1
        self.request[1].sendto(answer, self.client_address)


class DNSServer(socketserver.UDPServer):
    def __init__(self, address: str, allowed: set[str]):
        self.address = address
        self.allowed = allowed
        self.errors: list[str] = []
        self.device_answers = 0
        self.device_nxdomain = 0
        super().__init__((address, 0), DNSHandler)


class TLSHandler(socketserver.BaseRequestHandler):
    def handle(self):
        server = self.server
        assert isinstance(server, TLSServer)
        if self.client_address[0] not in server.allowed:
            print("Controlled TLS refused an unexpected peer", flush=True)
            return
        self.request.settimeout(5)
        try:
            if server.consume_stall():
                deadline = time.monotonic() + 15
                total = 0
                hello = bytearray()
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("Injected TLS stall did not end within its deadline")
                    self.request.settimeout(remaining)
                    data = self.request.recv(256)
                    if not data:
                        break
                    total += len(data)
                    if total > 4096:
                        raise RuntimeError("Injected TLS stall exceeded its input limit")
                    if len(hello) < 6:
                        hello += data[:6 - len(hello)]
                        if len(hello) == 6:
                            record_size = int.from_bytes(hello[3:5], "big")
                            if hello[0] != 22 or hello[1] != 3 or hello[2] not in (1, 3) \
                                    or hello[5] != 1 or not 4 <= record_size <= 4091:
                                raise RuntimeError("Injected TLS stall did not receive ClientHello")
                if len(hello) != 6:
                    raise RuntimeError("Injected TLS stall ended without ClientHello")
                server.stalls += 1
                return
            with server.context.wrap_socket(self.request, server_side=True) as connection:
                deadline = time.monotonic() + 20
                first = bytearray()
                while len(first) < 4:
                    chunk = connection.recv(4 - len(first))
                    if not chunk:
                        return
                    first += chunk
                if server.kind != "valid":
                    raise RuntimeError("Invalid-certificate endpoint accepted application data")
                if first == b"P4FD":
                    server.drops += 1
                    plain = connection.unwrap()
                    plain.close()
                    return
                total = len(first)
                connection.sendall(first)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("Controlled TLS connection deadline exceeded")
                    connection.settimeout(min(5, remaining))
                    data = connection.recv(256)
                    if not data:
                        break
                    total += len(data)
                    if total > 4096:
                        raise RuntimeError("Controlled TLS payload limit exceeded")
                    if server.kind != "valid":
                        raise RuntimeError("Invalid-certificate endpoint accepted application data")
                    connection.sendall(data)
        except ssl.SSLError as error:
            if error.reason in {
                "SSLV3_ALERT_BAD_CERTIFICATE", "SSLV3_ALERT_CERTIFICATE_UNKNOWN",
                "SSLV3_ALERT_CERTIFICATE_EXPIRED", "TLSV1_ALERT_UNKNOWN_CA",
                "TLSV1_ALERT_CERTIFICATE_EXPIRED",
            }:
                print(f"Controlled TLS peer rejected certificate: {server.kind}", flush=True)
            else:
                server.errors.append(f"{server.kind}: unexpected TLS error {error.reason}")
                print(server.errors[-1], file=sys.stderr, flush=True)
        except (OSError, RuntimeError) as error:
            server.errors.append(f"{server.kind}: TLS service failure {error}")
            print(server.errors[-1], file=sys.stderr, flush=True)


class TLSServer(socketserver.ThreadingTCPServer):
    def __init__(self, address: str, allowed: set[str], context: ssl.SSLContext, kind: str):
        self.allowed = allowed
        self.context = context
        self.kind = kind
        self.errors: list[str] = []
        self.control = threading.Lock()
        self.stall_next = False
        self.stalls = 0
        self.drops = 0
        super().__init__((address, 0), TLSHandler)

    def prepare_stall(self) -> None:
        with self.control:
            if self.stall_next:
                raise RuntimeError("Previous injected TLS stall was not consumed")
            self.stall_next = True

    def consume_stall(self) -> bool:
        with self.control:
            if not self.stall_next:
                return False
            self.stall_next = False
            return True


def start_server(server, stack: contextlib.ExitStack) -> None:
    stack.callback(server.server_close)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
    thread.start()
    stack.callback(thread.join, 10)
    stack.callback(server.shutdown)


def preflight(address: str, dns: DNSServer, servers: list[TLSServer], directory: Path) -> None:
    query = struct.pack("!6H", 1234, 0x100, 1, 0, 0, 0)
    query += b"".join(bytes([len(label)]) + label.encode() for label in HOSTNAME.split("."))
    query += b"\x00\x00\x01\x00\x01"
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(3)
        client.sendto(query, dns.server_address)
        answer, _ = client.recvfrom(512)
        if answer != dns_response(query, address):
            raise RuntimeError("Controlled DNS readiness failed")
    context = ssl.create_default_context(cafile=str(directory / "ca.crt"))
    context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")
    cases = [(servers[0], HOSTNAME, 0), (servers[0], "wrong.p03.entelea.test", 62),
             (servers[1], HOSTNAME, 20), (servers[2], HOSTNAME, 10)]
    for server, hostname, expected in cases:
        try:
            with socket.create_connection(server.server_address, timeout=3) as raw:
                with context.wrap_socket(raw, server_hostname=hostname) as client:
                    if expected:
                        raise RuntimeError("Host preflight accepted an invalid certificate")
                    payload = b"entelea-p03-controlled-preflight"
                    client.sendall(payload)
                    reply = bytearray()
                    while len(reply) < len(payload):
                        chunk = client.recv(len(payload) - len(reply))
                        if not chunk:
                            raise RuntimeError("Controlled TLS echo closed early")
                        reply += chunk
                    if reply != payload:
                        raise RuntimeError("Controlled TLS echo mismatch")
        except ssl.SSLCertVerificationError as error:
            if error.verify_code != expected:
                raise RuntimeError("Host certificate preflight rejected for wrong reason") \
                    from error
    print("Controlled DNS/TLS readiness: exact echo and three certificate rejections passed",
          flush=True)


def redact(data: bytes, ssid: bytes, psk: bytes) -> bytes:
    for secret in (ssid, psk):
        text = secret.decode("utf-8")
        variants = (secret, secret.hex().encode(), json.dumps(text)[1:-1].encode(),
                    json.dumps(text, ensure_ascii=False)[1:-1].encode())
        for variant in variants:
            data = data.replace(variant, b"<credential-redacted>")
    return re.sub(rb"\b(?:\d{1,3}\.){3}\d{1,3}\b", b"<address-redacted>", data)


def private_credentials(path: Path) -> tuple[bytes, bytes]:
    info = path.stat()
    if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600:
        raise ValueError("Wi-Fi configuration requires current-owner mode 0600")
    config = json.loads(path.read_text())
    if not isinstance(config, dict) or not isinstance(config.get("ssid"), str) \
            or not isinstance(config.get("psk"), str):
        raise ValueError("Private Wi-Fi configuration requires string ssid and psk")
    if "security" in config and config["security"] != "wpa2-psk":
        raise ValueError("Configuration contradicts the WPA2-PSK contract")
    ssid, psk = config["ssid"].encode(), config["psk"].encode()
    if not 1 <= len(ssid) <= 32 or not 8 <= len(psk) <= 63:
        raise ValueError("Invalid credential byte lengths")
    return ssid, psk


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run", action="store_true", help="Authorize reset and runtime configuration",
    )
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--wifi-config", type=Path, required=True)
    parser.add_argument("--security", choices=["wpa2-psk"], required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    if not args.run:
        parser.error("--run is required; this resets and configures the selected hardware")
    if args.device.parent != Path("/dev/serial/by-id") or not args.device.exists():
        parser.error("Explicit existing stable by-id device is required")
    occupied = subprocess.run(["fuser", "-s", str(args.device)], capture_output=True)
    if occupied.returncode != 1:
        parser.error("Device is occupied or occupancy check failed")
    ssid, psk = private_credentials(args.wifi_config)
    evidence = args.evidence_dir.resolve()
    if evidence.is_relative_to(REPO):
        parser.error("Private evidence and generated keys must stay outside the repository")
    evidence.mkdir(mode=0o700)

    import serial
    from esptool.reset import HardReset

    chunks = bytearray()
    credential_sent = False
    endpoint_sent = 0
    stall_configured = 0
    all_servers = []
    with tempfile.TemporaryDirectory(prefix="keys-", dir=evidence) as temporary:
        directory = Path(temporary)
        certificates(directory)
        ca = (directory / "ca.der").read_bytes()
        with contextlib.ExitStack() as stack:
            port = serial.Serial(port=None, baudrate=115200, timeout=0.2, exclusive=True)
            port.dtr = False
            port.rts = False
            port.port = str(args.device)
            port.open()
            stack.callback(port.close)
            port.reset_input_buffer()
            HardReset(port)()
            deadline = time.monotonic() + 210
            try:
                while time.monotonic() < deadline:
                    chunks += port.read(4096)
                    if not credential_sent and b"P4L CONFIG_WAIT" in chunks:
                        frame = b"P4C1" + bytes([len(ssid), len(psk)]) + ssid + psk
                        if port.write(frame) != len(frame):
                            raise RuntimeError("Incomplete credential UART write")
                        port.flush()
                        credential_sent = True
                    if chunks.count(b"P4L TLS_CONFIG_WAIT") > endpoint_sent:
                        matches = re.findall(rb"P4L IPV4_READY address=([\d.]+)", chunks)
                        if not matches:
                            raise RuntimeError("Missing actual DHCP interface address")
                        device_address = socket.inet_ntoa(socket.inet_aton(matches[-1].decode()))
                        if not all_servers:
                            route = json.loads(subprocess.check_output(
                                ["ip", "-j", "-4", "route", "get", device_address]
                            ))
                            address = route[0]["prefsrc"]
                            allowed = {address, device_address}
                            dns = DNSServer(address, allowed)
                            start_server(dns, stack)
                            servers = []
                            for kind in ("valid", "untrusted", "expired"):
                                server = TLSServer(
                                    address, allowed, server_context(directory, kind), kind,
                                )
                                start_server(server, stack)
                                servers.append(server)
                            all_servers = [dns, *servers]
                            preflight(address, dns, servers, directory)
                            ports = [server.server_address[1] for server in all_servers]
                        frame = endpoint_frame(address, ports, ca, int(time.time()))
                        if port.write(frame) != len(frame):
                            raise RuntimeError("Incomplete endpoint UART write")
                        port.flush()
                        endpoint_sent += 1
                    if chunks.count(b"P4L TLS_STALL_CONFIG_WAIT") > stall_configured:
                        if not all_servers:
                            raise RuntimeError("TLS fault requested before endpoint configuration")
                        servers[0].prepare_stall()
                        if port.write(b"P4F1") != 4:
                            raise RuntimeError("Incomplete TLS fault control UART write")
                        port.flush()
                        stall_configured += 1
                    if re.search(rb"P4L (?:PASS|FAIL) association_dhcp_disconnect=", chunks) \
                            or b"Error parsing segments" in chunks or b"FATAL" in chunks:
                        break
            finally:
                with os.fdopen(os.open(evidence / "device.log",
                                      os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as output:
                    output.write(redact(bytes(chunks), ssid, psk))
        errors = [error for server in all_servers for error in server.errors]
    required = {
        b"P4L CONNECT_OK attempt=": 2,
        b"P4L DHCP_BOUND attempt=": 2,
        b"P4L DISCONNECT_OK": 2,
        b"P4L DNS_OK exact_address=1 negative_rc=-4": 2,
        b"P4L TLS_ECHO_OK frames=4 bytes=1024": 6,
        b"P4L TLS_DROP_OK authenticated_eof=1": 2,
        b"P4L TLS_TIMEOUT_OK elapsed_ms=": 2,
        b"P4L TLS_REJECT_OK flags=4": 2,
        b"P4L TLS_REJECT_OK flags=8": 2,
        b"P4L TLS_REJECT_OK flags=1": 2,
        b"P4L PASS dns_tls_echo_certificates": 2,
    }
    counts = {}
    for marker in required:
        if marker.endswith(b"attempt="):
            ending = rb"[12]\r?$"
        elif marker.endswith(b"elapsed_ms="):
            ending = rb"\d+\r?$"
        else:
            ending = rb"\r?$"
        counts[marker.decode()] = len(re.findall(rb"^" + re.escape(marker) + ending, chunks, re.M))
    versions = re.findall(rb"coprocessor firmware v([0-9]+\.[0-9]+\.[0-9]+)", chunks)
    passed = all(counts[marker.decode()] == count for marker, count in required.items()) \
        and b"P4L PASS association_dhcp_disconnect=2/2" in chunks and not errors \
        and not any(marker in chunks for marker in
                    (b"P4L FAIL", b"FATAL", b"Error parsing segments")) \
        and bool(all_servers) and dns.device_answers >= 2 and dns.device_nxdomain >= 2 \
        and bool(versions) and servers[0].stalls == 2 and servers[0].drops == 2
    with os.fdopen(os.open(evidence / "result.json",
                          os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as output:
        result = {"passed": passed, "counts": counts, "service_errors": errors}
        if all_servers:
            result["controlled_dns_answers"] = dns.device_answers
            result["controlled_dns_nxdomain"] = dns.device_nxdomain
            result["controlled_tls_stalls"] = servers[0].stalls
            result["controlled_tls_drops"] = servers[0].drops
        if versions:
            result["coprocessor_firmware"] = versions[-1].decode()
        json.dump(result, output, indent=2)
        output.write("\n")
    if not passed:
        raise RuntimeError(f"Actual DNS/TLS acceptance failed; private evidence: {evidence}")
    print("Target DNS/TLS passed: two cycles, exact echo, certificate rejection and fault recovery",
          flush=True)


if __name__ == "__main__":
    main()
