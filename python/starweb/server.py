import json
import socket
import ssl
import threading
import traceback
from collections.abc import Callable
from dataclasses import dataclass

from . import static, tls
from .errors import ALPNError
from .message import VERSION, Request, Response, parse_request
from .url import DEFAULT_PORTS, format_host

SERVER = "starweb-py/0.1"
RECV_TIMEOUT = 5.0
_CHUNK = 4096
_FILE_CHUNK = 64 * 1024


@dataclass
class Route:
    segments: list[str]
    methods: dict[str, object]
    cors: str | None = None

    def match(self, path: str) -> dict[str, str] | None:
        parts = path.split("?")[0].split("#")[0].strip("/").split("/")
        if len(parts) != len(self.segments):
            return None
        params: dict[str, str] = {}
        for pattern, actual in zip(self.segments, parts):
            if pattern.startswith("<") and pattern.endswith(">"):
                params[pattern[1:-1]] = actual
            elif pattern != actual:
                return None
        return params


def _coerce(value) -> Response:
    if isinstance(value, Response):
        return value
    if isinstance(value, dict) or isinstance(value, list):
        return Response(body=json.dumps(value).encode(),
                        headers={"Content-Type": "application/json"})
    if isinstance(value, str):
        return Response(body=value.encode(),
                        headers={"Content-Type": "text/html"})
    if isinstance(value, bytes):
        return Response(body=value,
                        headers={"Content-Type": "application/octet-stream"})
    if value is None:
        return Response(204, "No Content")
    raise TypeError(f"handler returned unsupported type: {type(value).__name__}")


def _reachable_url(scheme: str, host: str, port: int) -> str:
    # A wildcard bind address is not a destination, and it is not in any cert's
    # SAN either, so never print one as though it were a URL.
    if host in ("0.0.0.0", "::", ""):
        host = "localhost"
    suffix = "" if port == DEFAULT_PORTS[scheme] else f":{port}"
    return f"{scheme}://{format_host(host)}{suffix}/"


def _error(code: int, text: str, message: str) -> Response:
    return Response(code, text, body=message.encode(),
                    headers={"Content-Type": "text/plain"})


class App:
    def __init__(self, cors: str | None = None):
        self._routes: list[Route] = []
        self._static: list[tuple[str, str]] = []
        self._cors = cors

    def route(self, path: str, methods: list[str] | None = None,
              cors: str | None = None):
        """cors names the origin allowed to read this route from a page script
        ("*" for any). Without it a cross-origin fetch is refused by the browser."""
        segments = path.strip("/").split("/")

        def decorator(fn):
            for existing in self._routes:
                if existing.segments == segments:
                    for m in (methods or ["GET"]):
                        existing.methods[m.upper()] = fn
                    if cors is not None:
                        existing.cors = cors
                    return fn
            self._routes.append(
                Route(segments, {m.upper(): fn for m in (methods or ["GET"])},
                      cors if cors is not None else self._cors)
            )
            return fn

        return decorator

    def mount_static(self, prefix: str, root: str) -> None:
        self._static.append((prefix.rstrip("/"), root))

    def handle(self, req: Request) -> Response:
        # An HTTP/1.1 request line parses fine as STWP, so this is what keeps a
        # web client from being served content.
        if req.version != VERSION:
            return _error(505, "Version Not Supported",
                          "This server speaks STWP/1.0 only.")

        path = req.path.split("?")[0].split("#")[0]

        for route in self._routes:
            params = route.match(path)
            if params is None:
                continue
            handler = route.methods.get(req.method)
            if handler is None:
                res = _error(405, "Method Not Allowed",
                             "Method not allowed for this route.")
                res.headers["Allow"] = ", ".join(sorted(route.methods))
                return res
            try:
                res = _coerce(handler(req, **params))
            except Exception:
                traceback.print_exc()
                return _error(500, "Internal Server Error", "Handler failed.")
            if route.cors and "Access-Control-Allow-Origin" not in res.headers:
                res.headers["Access-Control-Allow-Origin"] = route.cors
            return res

        for prefix, root in self._static:
            if path.startswith(prefix or "/"):
                if req.method != "GET":
                    return _error(405, "Method Not Allowed",
                                  "Only GET method is supported.")
                return static.serve_file(root, path[len(prefix):] or "/",
                                         req.headers.get("range"))

        return _error(404, "Not Found", "Not found.")

    def run(self, host: str = "0.0.0.0", scheme: str = "both",
            port: int | None = 8090, tls_port: int | None = 8490,
            cert: str | None = None, key: str | None = None,
            log: bool | Callable[[str], None] = False) -> None:
        Server(self, host, scheme, port, tls_port, cert, key, log).serve_forever()


SCHEMES = ("moon", "star", "both")


class Server:
    def __init__(self, app: App, host: str = "0.0.0.0", scheme: str = "both",
                 port: int | None = 8090, tls_port: int | None = 8490,
                 cert: str | None = None, key: str | None = None,
                 log: bool | Callable[[str], None] = False):
        if scheme not in SCHEMES:
            raise ValueError(f"scheme must be one of {SCHEMES}, got {scheme!r}")

        self.app = app
        self.host = host
        self.scheme = scheme
        self.log = log
        self._listeners: list[tuple[socket.socket, str]] = []
        self._ctx: ssl.SSLContext | None = None
        self._stop = threading.Event()

        want_moon = scheme in ("moon", "both") and port is not None
        want_star = scheme in ("star", "both") and tls_port is not None

        self.tls_port = None
        if want_star:
            if not cert or not key:
                self._no_star("no cert/key given", fatal=scheme == "star")
            else:
                try:
                    self._ctx = tls.server_context(cert, key)
                except (OSError, ssl.SSLError) as e:
                    # A cert that was named but won't load is a typo, not a
                    # request for plaintext. Degrading here hides the mistake
                    # and serves moon:// to someone expecting star://.
                    self._no_star(f"cannot load {cert}: {e}", fatal=True)

        self.port = None
        if want_moon:
            self._listeners.append((self._listen(port), "moon"))
            self.port = self._listeners[-1][0].getsockname()[1]

        if self._ctx is not None:
            self._listeners.append((self._listen(tls_port), "star"))
            self.tls_port = self._listeners[-1][0].getsockname()[1]

        if not self._listeners:
            raise RuntimeError(f"scheme={scheme!r} leaves nothing to serve")

    def _emit(self, message: str) -> None:
        if callable(self.log):
            self.log(message)
        else:
            print(f"[Server] {message}", flush=True)

    def _no_star(self, reason: str, fatal: bool) -> None:
        if fatal:
            raise RuntimeError(f"star:// requested but {reason}")
        print(f"[Server] star:// disabled: {reason}", flush=True)

    def _listen(self, port: int) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.host, port))
        sock.listen(64)
        return sock

    def start(self) -> None:
        for listener, scheme in self._listeners:
            threading.Thread(target=self._accept_loop,
                             args=(listener, scheme), daemon=True).start()
            port = listener.getsockname()[1]
            print(f"[Server] {scheme}:// on {self.host}:{port}"
                  f". ->. {_reachable_url(scheme, self.host, port)}", flush=True)

    def serve_forever(self) -> None:
        self.start()
        try:
            while not self._stop.wait(0.5):
                pass
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def stop(self) -> None:
        self._stop.set()
        for listener, _ in self._listeners:
            try:
                listener.close()
            except OSError:
                pass

    def _accept_loop(self, listener: socket.socket, scheme: str) -> None:
        while not self._stop.is_set():
            try:
                conn, _ = listener.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn, scheme),
                             daemon=True).start()

    def _handle(self, conn: socket.socket, scheme: str) -> None:
        conn.settimeout(RECV_TIMEOUT)
        try:
            if scheme == "star":
                try:
                    conn = self._ctx.wrap_socket(conn, server_side=True)
                    tls.enforce_alpn(conn, "client")
                except (ssl.SSLError, OSError, ALPNError):
                    return

            buf = b""
            req = None
            while True:
                done = parse_request(buf)
                if done is not None:
                    req, _ = done
                    break
                try:
                    chunk = conn.recv(_CHUNK)
                except (socket.timeout, OSError):
                    break
                if not chunk:
                    break
                buf += chunk

            if req is None:
                res = _error(400, "Bad Request", "Failed to parse STWP request.")
            else:
                res = self.app.handle(req)

            if self.log and req is not None:
                transport = "star/TLS" if scheme == "star" else "moon/TCP"
                self._emit(f"[{transport}] {req.method} {req.path} "
                           f"{req.version} -> {res.status_code}")

            res.headers.setdefault("Server", SERVER)
            res.headers["Content-Length"] = str(res.content_length)
            res.headers["Connection"] = "close"
            try:
                # serialize() emits only the head when the body is file-backed.
                conn.sendall(res.serialize())
                if res.file is not None:
                    for chunk in res.file.chunks(_FILE_CHUNK):
                        conn.sendall(chunk)
            except OSError:
                pass  # peer went away mid-transfer
        finally:
            tls.close_gracefully(conn)
