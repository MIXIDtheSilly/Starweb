import os
import ssl
import threading
from pathlib import Path

from .errors import ALPNError, TLSVerificationError

ALPN = "stwp/1.0"


def find_ca() -> str:
    env = os.environ.get("STARWEB_CA")
    if env:
        return env
    candidates = [
        Path.cwd() / "certs" / "starweb_root.pem",
        Path(__file__).resolve().parents[2] / "certs" / "starweb_root.pem",
    ]
    for path in candidates:
        if path.is_file():
            return str(path)
    raise TLSVerificationError(
        "no StarWeb root CA found; set STARWEB_CA or run tools/make_certs.sh"
    )


def client_context(cafile: str | None = None) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = True
    ctx.set_alpn_protocols([ALPN])
    ctx.load_verify_locations(cafile or find_ca())
    return ctx


def server_context(certfile: str, keyfile: str) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.set_alpn_protocols([ALPN])
    ctx.load_cert_chain(certfile, keyfile)
    return ctx


def enforce_alpn(sock: ssl.SSLSocket, peer: str) -> None:
    """A peer offering no ALPN never reaches the selection callback, so this
    check has to happen after the handshake rather than during it."""
    if sock.selected_alpn_protocol() != ALPN:
        raise ALPNError(f"{peer} did not negotiate ALPN {ALPN}")


def close_gracefully(sock, timeout: float = 1.0) -> None:
    """OpenSSL 3 reports a bare FIN as an error, so a TLS peer has to be sent
    close_notify or its read fails instead of ending cleanly."""
    try:
        if isinstance(sock, ssl.SSLSocket):
            sock.settimeout(timeout)
            sock = sock.unwrap()
    except (OSError, ValueError):
        pass
    try:
        sock.close()
    except OSError:
        pass


class SessionCache:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._sessions: dict[str, ssl.SSLSession] = {}

    def get(self, key: str) -> ssl.SSLSession | None:
        with self._lock:
            return self._sessions.get(key)

    def put(self, key: str, session: ssl.SSLSession | None) -> None:
        if session is None:
            return
        with self._lock:
            self._sessions[key] = session

    def clear(self) -> None:
        with self._lock:
            self._sessions.clear()
