import socket
import ssl
import subprocess
import threading

import pytest

from stardns import ca, config, zones
from stardns.errors import PanelError

pytestmark = pytest.mark.skipif(not ca.ca_ready()[0],
                                reason="no StarWeb root CA in certs/")

ZONE = config.ZONE


@pytest.fixture
def issued(account, tmp_path, monkeypatch):
    monkeypatch.setattr(config, "ISSUED", tmp_path)
    domain = zones.add_domain("tester", "example")["name"]
    return domain, ca.issue(domain)


def openssl_text(path) -> str:
    return subprocess.run(["openssl", "x509", "-in", str(path), "-noout", "-text"],
                          capture_output=True, text=True).stdout


def test_leaf_chains_to_the_starweb_root(issued):
    _domain, cert = issued
    proc = subprocess.run(["openssl", "verify", "-CAfile", str(config.CA_CERT),
                           cert["cert_path"]], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr


def test_leaf_covers_the_domain_and_its_subdomains(issued):
    domain, cert = issued
    assert cert["sans"] == f"DNS:{domain},DNS:*.{domain}"
    text = openssl_text(cert["cert_path"])
    assert f"DNS:{domain}" in text and f"DNS:*.{domain}" in text
    assert "TLS Web Server Authentication" in text


def test_key_is_on_disk_and_not_in_mongo(issued, fake_db):
    _domain, cert = issued
    from pathlib import Path
    assert Path(cert["key_path"]).is_file()
    assert Path(cert["key_path"]).stat().st_mode & 0o077 == 0
    stored = fake_db.certs.find_one({"serial": cert["serial"]})
    assert "PRIVATE KEY" not in stored["cert_pem"]
    assert "key_pem" not in stored


def test_reissue_gets_a_new_serial(issued):
    domain, first = issued
    second = ca.issue(domain)
    assert first["serial"] != second["serial"]
    assert ca.latest(domain)["serial"] == second["serial"]


def test_download_material(issued):
    domain, _cert = issued
    name, pem = ca.read_material(domain, "cert")
    assert name == f"{domain}.pem" and "BEGIN CERTIFICATE" in pem
    name, pem = ca.read_material(domain, "key")
    assert name == f"{domain}.key" and "PRIVATE KEY" in pem


def test_no_certificate_yet(account):
    with pytest.raises(PanelError) as e:
        ca.read_material(f"nothing.{ZONE}", "cert")
    assert e.value.status == 404


def test_the_root_cannot_reach_outside_the_zone(account, tmp_path, monkeypatch):
    monkeypatch.setattr(config, "ISSUED", tmp_path)
    with pytest.raises(PanelError) as e:
        ca.issue("www.google.com")
    assert "verify" in e.value.message


def _serve_once(cert_path: str, key_path: str) -> tuple[int, threading.Thread]:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(cert_path, key_path)
    ctx.set_alpn_protocols(["stwp/1.0"])

    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)

    def run():
        try:
            conn, _ = listener.accept()
            with ctx.wrap_socket(conn, server_side=True) as tls:
                tls.recv(64)
                tls.sendall(b"ok")
        except OSError:
            pass
        finally:
            listener.close()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return listener.getsockname()[1], thread


def _client_context() -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_verify_locations(cafile=str(config.CA_CERT))  # StarWeb root only
    ctx.set_alpn_protocols(["stwp/1.0"])
    return ctx


def _handshake(port: int, server_hostname: str) -> str:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
        with _client_context().wrap_socket(raw, server_hostname=server_hostname) as tls:
            tls.sendall(b"hi")
            tls.recv(8)
            return tls.version()


def test_a_starweb_client_accepts_the_issued_certificate(issued):
    """The point of the whole thing: a client trusting only the StarWeb root
    completes a TLS 1.3 handshake with a host serving a panel-issued cert."""
    domain, cert = issued
    port, thread = _serve_once(cert["cert_path"], cert["key_path"])
    assert _handshake(port, domain) == "TLSv1.3"
    thread.join(timeout=5)


def test_the_wildcard_covers_subdomains(issued):
    domain, cert = issued
    port, thread = _serve_once(cert["cert_path"], cert["key_path"])
    assert _handshake(port, f"www.{domain}") == "TLSv1.3"
    thread.join(timeout=5)


def test_a_different_name_is_refused(issued):
    domain, cert = issued
    port, thread = _serve_once(cert["cert_path"], cert["key_path"])
    with pytest.raises(ssl.SSLCertVerificationError):
        _handshake(port, f"other.{ZONE}")
    thread.join(timeout=5)
