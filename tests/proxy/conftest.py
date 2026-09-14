import json
import random
import socket
import ssl
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BINARY = REPO / "stwp_proxy"
ROOT_CA = REPO / "certs" / "starweb_root.pem"
ROOT_KEY = REPO / "certs" / "starweb_root.key"
OPENSSL = next((p for p in ("/usr/local/opt/openssl@3/bin/openssl", "/opt/homebrew/opt/openssl@3/bin/openssl")
                if Path(p).exists()), "openssl")
sys.path.insert(0, str(REPO / "python"))

from starweb import App, Response, Server  # noqa: E402


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def pattern(n: int) -> bytes:
    block = bytes(range(256))
    return (block * (n // 256 + 1))[:n]


class Backend(App):
    def __init__(self, name: str):
        super().__init__()
        self.name = name
        self.hits = 0
        self.server: Server | None = None

    @property
    def port(self) -> int:
        return self.server.port

    @property
    def tls_port(self) -> int:
        return self.server.tls_port

    def handle(self, req):
        self.hits += 1
        path = req.path.split("?")[0]
        if path == "/slow":
            time.sleep(2)
        if path.startswith("/big/"):
            return Response(200, body=pattern(int(path[5:])),
                            headers={"Content-Type": "application/octet-stream"})
        if path == "/echo":
            return Response(200, body=req.body, headers={"Content-Type": "application/octet-stream"})
        if path == "/cookie":
            return Response(200, body=b"ok").set_cookie("sid", "abc", max_age=60)
        body = json.dumps({"backend": self.name, "method": req.method, "path": req.path,
                           "headers": req.headers, "body_len": len(req.body)}).encode()
        return Response(200, body=body, headers={"Content-Type": "application/json"})


@dataclass
class Reply:
    status: int
    headers: dict
    body: bytes

    def json(self):
        return json.loads(self.body)


class Client:
    def __init__(self, port: int, tls=None, sni: str | None = None, timeout: float = 10):
        sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        if tls is not None:
            sock = tls.wrap_socket(sock, server_hostname=sni)
        self.sock = sock
        self.buf = bytearray()

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, data: bytes):
        self.sock.sendall(data)

    def request(self, method="GET", path="/", host="localhost", headers=None, body=b"",
                keep_alive=False) -> Reply:
        h = {"Host": host, "Connection": "keep-alive" if keep_alive else "close"}
        h.update(headers or {})
        if body:
            h["Content-Length"] = str(len(body))
        head = f"{method} {path} STWP/1.0\r\n" + "".join(f"{k}: {v}\r\n" for k, v in h.items()) + "\r\n"
        self.send(head.encode() + body)
        return self.read_reply(head_only=method == "HEAD")

    def _fill(self):
        chunk = self.sock.recv(1 << 20)
        if not chunk:
            raise ConnectionError("connection closed")
        self.buf += chunk

    def read_reply(self, head_only=False) -> Reply:
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        end = self.buf.index(b"\r\n\r\n")
        lines = bytes(self.buf[:end]).decode("latin-1").split("\r\n")
        del self.buf[:end + 4]
        status = int(lines[0].split(" ")[1])
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        if head_only:
            return Reply(status, headers, b"")
        if "content-length" in headers:
            n = int(headers["content-length"])
            while len(self.buf) < n:
                self._fill()
            body = bytes(self.buf[:n])
            del self.buf[:n]
        else:
            while True:
                try:
                    self._fill()
                except ConnectionError:
                    break
            body = bytes(self.buf)
            self.buf.clear()
        return Reply(status, headers, body)

    def is_closed(self, timeout=2.0) -> bool:
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(1) == b""
        except (ConnectionError, OSError):
            return True


def get(port, path="/", **kw) -> Reply:
    with Client(port) as c:
        return c.request(path=path, **kw)


@pytest.fixture
def backend():
    started = []

    def make(name="a", scheme="moon", cert=None, key=None) -> Backend:
        app = Backend(name)
        srv = Server(app, host="127.0.0.1", scheme=scheme, port=0, tls_port=0, cert=cert, key=key)
        srv.start()
        app.server = srv
        started.append(srv)
        return app

    yield make
    for srv in started:
        srv.stop()


class ProxyProcess:
    def __init__(self, tmp_path: Path, conf: str, index: int):
        self.dir = tmp_path
        self.conf_path = tmp_path / f"proxy{index}.conf"
        self.conf_path.write_text(conf)
        self.stdout_path = tmp_path / f"proxy{index}.stdout"
        self.stderr_path = tmp_path / f"proxy{index}.stderr"
        self.proc = subprocess.Popen([str(BINARY), "-c", str(self.conf_path)], cwd=REPO,
                                     stdout=self.stdout_path.open("w"), stderr=self.stderr_path.open("w"))

    @property
    def pid(self):
        return self.proc.pid

    def stderr(self) -> str:
        return self.stderr_path.read_text()

    def stdout(self) -> str:
        return self.stdout_path.read_text()

    def wait_for(self, ports, timeout=5.0):
        deadline = time.time() + timeout
        for port in ports:
            while True:
                if self.proc.poll() is not None:
                    raise RuntimeError(f"stwp_proxy exited: {self.stderr()}")
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    if time.time() > deadline:
                        raise
                    time.sleep(0.02)

    def rss_kb(self) -> int:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(self.pid)], capture_output=True, text=True)
        return int(out.stdout.strip() or 0)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


@pytest.fixture
def proxy(tmp_path):
    if not BINARY.is_file():
        pytest.skip("run make stwp_proxy first")
    procs = []

    def start(conf: str, ports, **subs) -> ProxyProcess:
        for key, value in subs.items():
            conf = conf.replace(f"@{key}@", str(value))
        p = ProxyProcess(tmp_path, conf, len(procs))
        procs.append(p)
        p.wait_for(ports)
        return p

    yield start
    for p in procs:
        p.stop()


def tls_client(alpn=True, verify_hostname=True) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_verify_locations(ROOT_CA)
    if alpn:
        ctx.set_alpn_protocols(["stwp/1.0"])
    ctx.check_hostname = verify_hostname
    return ctx


@pytest.fixture(scope="session")
def pki(tmp_path_factory):
    if not (ROOT_CA.is_file() and ROOT_KEY.is_file()):
        pytest.skip("run tools/make_certs.sh first")
    d = tmp_path_factory.mktemp("pki")

    def issue(name: str):
        key, csr, crt, ext = (d / f"{name}.{x}" for x in ("key", "csr", "pem", "ext"))
        if crt.exists():
            return crt, key
        ext.write_text(f"subjectAltName=DNS:{name}\nextendedKeyUsage=serverAuth\n"
                       "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n")
        run = lambda *args: subprocess.run([OPENSSL, *map(str, args)], check=True, capture_output=True)
        run("req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
            "-keyout", key, "-out", csr, "-subj", f"/CN={name}")
        run("x509", "-req", "-in", csr, "-CA", ROOT_CA, "-CAkey", ROOT_KEY, "-set_serial",
            random.getrandbits(62), "-days", "7", "-sha256", "-extfile", ext, "-out", crt)
        return crt, key

    return issue
