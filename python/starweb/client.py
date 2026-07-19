import socket
import ssl
from dataclasses import dataclass

from . import tls
from .errors import (ConnectionFailed, MixedContentError, ProtocolError,
                     TLSVerificationError)
from .message import VERSION, Request, Response, parse_response
from .url import ParsedURL, parse_url

USER_AGENT = "starweb-py/0.1"
DEFAULT_TIMEOUT = 10.0
_CHUNK = 8192


@dataclass
class TLSInfo:
    version: str
    cipher: str
    alpn: str
    resumed: bool


def _connect(parsed: ParsedURL, timeout: float) -> socket.socket:
    try:
        infos = socket.getaddrinfo(parsed.host, parsed.port,
                                   type=socket.SOCK_STREAM)
    except socket.gaierror as e:
        raise ConnectionFailed(f"host resolution failed: {parsed.host}: {e}") from None

    last = None
    for family, socktype, proto, _, addr in infos:
        sock = socket.socket(family, socktype, proto)
        sock.settimeout(timeout)
        try:
            sock.connect(addr)
            return sock
        except OSError as e:
            last = e
            sock.close()
    raise ConnectionFailed(
        f"connection failed to {parsed.host}:{parsed.port}: {last}"
    )


class Session:
    def __init__(self, cafile: str | None = None, timeout: float = DEFAULT_TIMEOUT,
                 user_agent: str = USER_AGENT, allow_mixed: bool = False):
        self.timeout = timeout
        self.user_agent = user_agent
        self.allow_mixed = allow_mixed
        self._cafile = cafile
        self._ctx: ssl.SSLContext | None = None
        self._sessions = tls.SessionCache()
        self._secure_origin = False

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self) -> None:
        self._sessions.clear()

    def _context(self) -> ssl.SSLContext:
        if self._ctx is None:
            self._ctx = tls.client_context(self._cafile)
        return self._ctx

    def request(self, method: str, url: str, *, body: bytes | str | None = None,
                headers: dict[str, str] | None = None,
                json=None) -> Response:
        parsed = parse_url(url)

        if parsed.scheme == "moon" and self._secure_origin and not self.allow_mixed:
            raise MixedContentError(
                f"refusing moon:// request from a star:// session: {url}"
            )
        if parsed.scheme == "star":
            self._secure_origin = True

        if json is not None:
            import json as _json
            body = _json.dumps(json).encode()

        payload = body.encode() if isinstance(body, str) else (body or b"")

        req = Request(method=method.upper(), path=parsed.path, body=payload)
        req.headers["Host"] = parsed.host_header
        req.headers["User-Agent"] = self.user_agent
        req.headers["Connection"] = "close"
        if json is not None:
            req.headers["Content-Type"] = "application/json"
        if payload:
            req.headers["Content-Length"] = str(len(payload))
        for name, value in (headers or {}).items():
            req.headers[name] = value

        return self._send(parsed, req)

    def _send(self, parsed: ParsedURL, req: Request) -> Response:
        sock = _connect(parsed, self.timeout)
        info = None
        try:
            if parsed.scheme == "star":
                sock, info = self._wrap(sock, parsed)
            sock.sendall(req.serialize())

            buf = b""
            res = None
            while True:
                done = parse_response(buf)
                if done is not None:
                    res, _ = done
                    break
                chunk = sock.recv(_CHUNK)
                if not chunk:
                    break
                buf += chunk
            if res is None:
                raise ProtocolError(
                    "incomplete STWP response" if buf else "empty response"
                )
        finally:
            self._remember(sock, parsed)
            tls.close_gracefully(sock)

        if res.version != VERSION:
            raise ProtocolError(f"not an STWP response: {res.version!r}")
        res.tls = info
        return res

    def _wrap(self, sock: socket.socket, parsed: ParsedURL):
        key = f"{parsed.host}:{parsed.port}"
        try:
            ssock = self._context().wrap_socket(
                sock, server_hostname=parsed.host,
                session=self._sessions.get(key),
            )
        except ssl.SSLCertVerificationError as e:
            sock.close()
            raise TLSVerificationError(
                f"certificate verification failed for {parsed.host}: {e.verify_message or e}"
            ) from None
        except ssl.SSLError as e:
            sock.close()
            raise TLSVerificationError(f"TLS handshake failed: {e}") from None

        try:
            tls.enforce_alpn(ssock, "server")
        except Exception:
            ssock.close()
            raise

        cipher = ssock.cipher() or ("", "", 0)
        return ssock, TLSInfo(version=ssock.version() or "",
                              cipher=cipher[0],
                              alpn=ssock.selected_alpn_protocol() or "",
                              resumed=bool(getattr(ssock, "session_reused", False)))

    def _remember(self, sock, parsed: ParsedURL) -> None:
        if parsed.scheme == "star" and isinstance(sock, ssl.SSLSocket):
            self._sessions.put(f"{parsed.host}:{parsed.port}", sock.session)

    def get(self, url: str, **kw) -> Response:
        return self.request("GET", url, **kw)

    def post(self, url: str, **kw) -> Response:
        return self.request("POST", url, **kw)

    def put(self, url: str, **kw) -> Response:
        return self.request("PUT", url, **kw)

    def delete(self, url: str, **kw) -> Response:
        return self.request("DELETE", url, **kw)


def request(method: str, url: str, **kw) -> Response:
    with Session(cafile=kw.pop("cafile", None),
                 timeout=kw.pop("timeout", DEFAULT_TIMEOUT),
                 allow_mixed=kw.pop("allow_mixed", False)) as s:
        return s.request(method, url, **kw)


def get(url: str, **kw) -> Response:
    return request("GET", url, **kw)


def post(url: str, **kw) -> Response:
    return request("POST", url, **kw)


def put(url: str, **kw) -> Response:
    return request("PUT", url, **kw)


def delete(url: str, **kw) -> Response:
    return request("DELETE", url, **kw)
