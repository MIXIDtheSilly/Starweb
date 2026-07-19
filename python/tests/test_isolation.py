"""StarWeb's separation from the public web is enforced, not assumed. These
tests are the enforcement — none of them should ever be skipped or relaxed."""

import socket
import ssl

import pytest

import starweb
from starweb import tls
from starweb.errors import MixedContentError, URLError
from starweb.message import parse_response

from conftest import CERTS


def _raw_send(port: int, payload: bytes) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(payload)
        buf = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                return buf
            buf += chunk


def test_http_request_line_gets_505(server):
    raw = _raw_send(server.port,
                    b"GET /api/ping HTTP/1.1\r\nHost: localhost\r\n\r\n")
    res, _ = parse_response(raw)
    assert res.status_code == 505
    assert b"pong" not in res.body


def test_http_10_also_rejected(server):
    raw = _raw_send(server.port, b"GET / HTTP/1.0\r\n\r\n")
    res, _ = parse_response(raw)
    assert res.status_code == 505


def test_client_context_trusts_only_starweb_root():
    ctx = tls.client_context()
    subjects = [dict(x for x in c["subject"][0]) for c in ctx.get_ca_certs()]
    assert len(subjects) == 1, "system roots must never be loaded"


def test_client_pins_tls13_only():
    ctx = tls.client_context()
    assert ctx.minimum_version is ssl.TLSVersion.TLSv1_3
    assert ctx.maximum_version is ssl.TLSVersion.TLSv1_3


@pytest.mark.parametrize("url", [
    "https://example.com/", "http://example.com/", "file:///etc/passwd",
    "ftp://example.com/", "javascript:alert(1)",
])
def test_only_starweb_schemes_are_addressable(url):
    with pytest.raises(URLError):
        starweb.get(url)


def _tls_connect(port: int, alpn: list[str] | None):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_verify_locations(str(CERTS / "starweb_root.pem"))
    if alpn is not None:
        ctx.set_alpn_protocols(alpn)
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    return ctx.wrap_socket(sock, server_hostname="localhost")


def test_client_offering_no_alpn_is_dropped(server):
    if server.tls_port is None:
        pytest.fail("star:// must be enabled for isolation tests")
    with _tls_connect(server.tls_port, None) as s:
        assert s.selected_alpn_protocol() is None
        try:
            s.sendall(b"GET /api/ping STWP/1.0\r\nHost: localhost\r\n\r\n")
            assert s.recv(4096) == b""
        except OSError:
            pass


def test_browser_alpn_is_dropped(server):
    if server.tls_port is None:
        pytest.fail("star:// must be enabled for isolation tests")
    with _tls_connect(server.tls_port, ["h2", "http/1.1"]) as s:
        assert s.selected_alpn_protocol() != "stwp/1.0"
        try:
            s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert s.recv(4096) == b""
        except OSError:
            pass


def test_star_session_refuses_moon(server, star, moon):
    with starweb.Session() as s:
        s.get(f"{star}/api/ping")
        with pytest.raises(MixedContentError):
            s.get(f"{moon}/api/ping")


def test_mixed_content_opt_in_is_explicit(server, star, moon):
    with starweb.Session(allow_mixed=True) as s:
        s.get(f"{star}/api/ping")
        assert s.get(f"{moon}/api/ping").ok
