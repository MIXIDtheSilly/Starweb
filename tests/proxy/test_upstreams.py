import socket
import subprocess
import threading
import time

import pytest

from conftest import REPO, Client, free_port, get

SERVER = REPO / "stwp_server"


class RawUpstream:
    def __init__(self, mode: str):
        self.mode = mode
        self.connections = 0
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        with conn:
            conn.settimeout(5)
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                data += chunk
            if self.mode == "keepalive-then-close":
                conn.sendall(b"STWP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok")
                time.sleep(0.05)

    def close(self):
        self.sock.close()


def upstream_conf(servers: str, extra: str = "") -> str:
    return """
error_log @dir@/error.log;
stwp {
    access_log off;
    upstream pool {
        %s
        %s
    }
    server {
        listen @port@;
        location / { proxy_pass moon://pool; }
    }
}
""" % (servers, extra)


def test_weighted_round_robin(proxy, backend, tmp_path):
    a, b = backend("a"), backend("b")
    port = free_port()
    proxy(upstream_conf(f"server 127.0.0.1:{a.port} weight=3;\n server 127.0.0.1:{b.port};"),
          [port], dir=tmp_path, port=port)
    order = [get(port, "/").json()["backend"] for _ in range(8)]
    assert order == ["a", "a", "b", "a"] * 2


def test_failover_and_passive_health(proxy, backend, tmp_path):
    live = backend("live")
    dead = free_port()
    port = free_port()
    proxy(upstream_conf(f"server 127.0.0.1:{dead} max_fails=1 fail_timeout=1500ms;\n"
                        f"server 127.0.0.1:{live.port};"), [port], dir=tmp_path, port=port)

    def dead_attempts():
        return (tmp_path / "error.log").read_text().count(f"upstream 127.0.0.1:{dead}: connect failed")

    for _ in range(6):
        assert get(port, "/").json()["backend"] == "live"
    assert dead_attempts() == 1

    time.sleep(1.7)
    for _ in range(3):
        assert get(port, "/").json()["backend"] == "live"
    assert dead_attempts() == 2
    assert live.hits == 9


def test_all_servers_down_is_502(proxy, tmp_path):
    d1, d2, port = free_port(), free_port(), free_port()
    proxy(upstream_conf(f"server 127.0.0.1:{d1};\n server 127.0.0.1:{d2};"), [port], dir=tmp_path, port=port)
    assert get(port, "/").status == 502
    log = (tmp_path / "error.log").read_text()
    assert f"127.0.0.1:{d1}: connect failed" in log
    assert f"127.0.0.1:{d2}: connect failed" in log


def test_no_retry_once_the_request_was_sent(proxy, backend, tmp_path):
    closer = RawUpstream("close")
    live = backend("live")
    port = free_port()
    try:
        proxy(upstream_conf(f"server 127.0.0.1:{closer.port} max_fails=0;\n server 127.0.0.1:{live.port};"),
              [port], dir=tmp_path, port=port)
        assert get(port, "/", method="POST", body=b"charge the card").status == 502
        assert live.hits == 0
        assert "closed without responding" in (tmp_path / "error.log").read_text()
        assert get(port, "/").json()["backend"] == "live"
    finally:
        closer.close()


@pytest.fixture
def cpp_server(tmp_path):
    if not SERVER.is_file():
        pytest.skip("run make stwp_server first")
    port = free_port()
    log = tmp_path / "stwp_server.log"
    proc = subprocess.Popen([str(SERVER), str(port), "--no-tls"], cwd=REPO,
                            stdout=log.open("w"), stderr=subprocess.STDOUT)
    deadline = time.time() + 5
    while True:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            if time.time() > deadline:
                proc.kill()
                raise
            time.sleep(0.02)
    yield port, log
    proc.kill()
    proc.wait()


@pytest.mark.parametrize("keepalive, fresh_connections", [(4, 1), (0, 5)])
def test_keepalive_pool(proxy, cpp_server, tmp_path, keepalive, fresh_connections):
    upstream_port, log = cpp_server
    port = free_port()
    proxy(upstream_conf(f"server 127.0.0.1:{upstream_port};", f"keepalive {keepalive};"),
          [port], dir=tmp_path, port=port)
    index = (REPO / "www" / "index.html").read_bytes()
    for _ in range(5):
        r = get(port, "/index.html")
        assert r.status == 200 and r.body == index
    lines = [l for l in log.read_text().splitlines() if "Request: GET /index.html" in l]
    assert len(lines) == 5
    assert sum("[moon/plain]" in l for l in lines) == fresh_connections


def test_pooled_connection_closed_by_upstream_is_replaced(proxy, tmp_path):
    flaky = RawUpstream("keepalive-then-close")
    port = free_port()
    try:
        proxy(upstream_conf(f"server 127.0.0.1:{flaky.port};", "keepalive 4;"), [port], dir=tmp_path, port=port)
        for _ in range(3):
            r = get(port, "/")
            assert (r.status, r.body) == (200, b"ok")
            time.sleep(0.2)
        assert flaky.connections == 3
        assert "[error]" not in (tmp_path / "error.log").read_text()
    finally:
        flaky.close()


def test_client_keep_alive_across_different_upstreams(proxy, backend, tmp_path):
    a, b = backend("a"), backend("b")
    port = free_port()
    proxy(upstream_conf(f"server 127.0.0.1:{a.port};\n server 127.0.0.1:{b.port};"), [port], dir=tmp_path, port=port)
    with Client(port) as c:
        seen = [c.request(keep_alive=True).json()["backend"] for _ in range(4)]
    assert seen == ["a", "b", "a", "b"]
