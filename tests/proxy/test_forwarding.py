import hashlib
import os
import threading
import time

from conftest import Client, free_port, get, pattern

BASIC = """
stwp {
    access_log @dir@/access.log;
    error_log_placeholder;
    client_max_body_size 2m;
    proxy_read_timeout 1s;

    server {
        listen @port@;

        location = /exact {
            proxy_pass moon://127.0.0.1:@up@/replaced;
        }
        location /api/ {
            proxy_pass moon://127.0.0.1:@up@/v1/;
            proxy_set_header Star-Real-IP $remote_addr;
            proxy_set_header X-Seen "$scheme $host $server_port $request_uri $proxy_host";
            proxy_set_header User-Agent "";
        }
        location /dead/ {
            proxy_pass moon://127.0.0.1:@dead@;
        }
        location = /health {
            return 200 "ok";
        }
        location / {
            proxy_pass moon://127.0.0.1:@up@;
        }
    }
}
"""


def start_basic(proxy, backend, tmp_path, **extra):
    up = backend("a")
    port = free_port()
    conf = BASIC.replace("error_log_placeholder;", "")
    conf = f"error_log {tmp_path}/error.log;\n" + conf
    p = proxy(conf, [port], dir=tmp_path, port=port, up=up.port, dead=free_port(), **extra)
    return p, port, up


def test_forwards_request_and_adds_forwarded_headers(proxy, backend, tmp_path):
    _, port, _ = start_basic(proxy, backend, tmp_path)
    r = get(port, "/page?x=1", host="site.web:9090", headers={"Star-Forwarded-For": "10.0.0.9"})
    assert r.status == 200
    seen = r.json()
    assert seen["path"] == "/page?x=1"
    h = seen["headers"]
    assert h["host"] == "site.web:9090"
    assert h["star-forwarded-for"] == "10.0.0.9, 127.0.0.1"
    assert h["star-forwarded-proto"] == "moon"
    assert h["star-forwarded-host"] == "site.web:9090"
    assert h["connection"] == "close"


def test_set_header_variables_removal_and_uri_rewrite(proxy, backend, tmp_path):
    _, port, up = start_basic(proxy, backend, tmp_path)
    r = get(port, "/api/items/7?q=2", host="Site.WEB", headers={"User-Agent": "tester"})
    seen = r.json()
    assert seen["path"] == "/v1/items/7?q=2"
    h = seen["headers"]
    assert h["star-real-ip"] == "127.0.0.1"
    assert h["x-seen"] == f"moon site.web {port} /api/items/7?q=2 127.0.0.1:{up.port}"
    assert "user-agent" not in h

    assert get(port, "/exact?z=9").json()["path"] == "/replaced?z=9"


def test_return_location(proxy, backend, tmp_path):
    _, port, up = start_basic(proxy, backend, tmp_path)
    r = get(port, "/health")
    assert (r.status, r.body) == (200, b"ok")
    assert up.hits == 0


def test_post_body_is_forwarded_intact(proxy, backend, tmp_path):
    _, port, _ = start_basic(proxy, backend, tmp_path)
    payload = os.urandom(1_500_000)
    r = get(port, "/echo", body=payload, method="POST")
    assert r.status == 200
    assert r.body == payload


def test_request_limits_and_malformed_requests(proxy, backend, tmp_path):
    _, port, up = start_basic(proxy, backend, tmp_path)

    for raw, status in [
        (b"POST /echo STWP/1.0\r\nContent-Length: 2097153\r\n\r\n" + b"x" * 1000, 413),
        (b"GET / STWP/1.0\r\nHost: a\r\nContent-Length: 5x\r\n\r\nhello", 400),
        (b"GET / STWP/1.0\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 400),
        (b"GET / HTTP/1.1\r\nHost: a\r\n\r\n", 505),
        (b"GET\r\n\r\n", 400),
        (b"GET example.web STWP/1.0\r\n\r\n", 400),
        (b"GET / STWP/1.0\r\nnot a header\r\n\r\n", 400),
        (b"GET / STWP/1.0\r\nX-Big: " + b"a" * 20000 + b"\r\n\r\n", 431),
    ]:
        with Client(port) as c:
            c.send(raw)
            reply = c.read_reply()
            assert reply.status == status, raw[:40]
            assert reply.headers["connection"] == "close"
            assert c.is_closed()
    assert up.hits == 0


def test_client_keep_alive(proxy, backend, tmp_path):
    _, port, up = start_basic(proxy, backend, tmp_path)
    with Client(port) as c:
        for i in range(3):
            r = c.request(path=f"/n{i}", keep_alive=True)
            assert r.status == 200
            assert r.headers["connection"] == "keep-alive"
            assert r.json()["path"] == f"/n{i}"
        r = c.request(path="/last")
        assert r.headers["connection"] == "close"
        assert c.is_closed()
    assert up.hits == 4


def test_upstream_down_is_502_and_keeps_the_client(proxy, backend, tmp_path):
    p, port, _ = start_basic(proxy, backend, tmp_path)
    with Client(port) as c:
        r = c.request(path="/dead/x", keep_alive=True)
        assert r.status == 502
        assert c.request(path="/health").status == 200
    log = (tmp_path / "error.log").read_text()
    assert "[error] upstream 127.0.0.1:" in log
    assert "connect failed" in log
    assert 'request "GET /dead/x STWP/1.0"' in log


def test_slow_upstream_is_504(proxy, backend, tmp_path):
    _, port, _ = start_basic(proxy, backend, tmp_path)
    started = time.time()
    r = get(port, "/slow")
    assert r.status == 504
    assert time.time() - started < 1.9


def test_large_response_streams_with_flat_memory(proxy, backend, tmp_path):
    p, port, _ = start_basic(proxy, backend, tmp_path)
    size = 96 * 1024 * 1024
    peak = [0]
    done = threading.Event()

    def sample():
        while not done.is_set():
            peak[0] = max(peak[0], p.rss_kb())
            time.sleep(0.05)

    sampler = threading.Thread(target=sample)
    sampler.start()
    try:
        with Client(port, timeout=60) as c:
            c.send(f"GET /big/{size} STWP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
            digest = hashlib.sha256()
            received = 0
            head_done = False
            while True:
                chunk = c.sock.recv(1 << 16)
                if not chunk:
                    break
                if not head_done:
                    c.buf += chunk
                    if b"\r\n\r\n" not in c.buf:
                        continue
                    end = c.buf.index(b"\r\n\r\n")
                    assert b"content-length: %d" % size in bytes(c.buf[:end]).lower()
                    chunk = bytes(c.buf[end + 4:])
                    head_done = True
                digest.update(chunk)
                received += len(chunk)
    finally:
        done.set()
        sampler.join()
    assert received == size
    assert digest.hexdigest() == hashlib.sha256(pattern(size)).hexdigest()
    assert peak[0] < 24 * 1024, f"proxy peaked at {peak[0]} KB"


def test_set_cookie_passes_through(proxy, backend, tmp_path):
    _, port, _ = start_basic(proxy, backend, tmp_path)
    r = get(port, "/cookie")
    assert r.headers["set-cookie"] == "sid=abc; Max-Age=60; StwpOnly"


def test_access_log_line(proxy, backend, tmp_path):
    _, port, up = start_basic(proxy, backend, tmp_path)
    get(port, "/logged?a=1", host="log.web")
    get(port, "/health")
    lines = (tmp_path / "access.log").read_text().splitlines()
    assert lines[0].startswith("127.0.0.1 [")
    assert '"GET /logged?a=1 STWP/1.0" 200 ' in lines[0]
    assert f'"log.web" upstream=127.0.0.1:{up.port} ' in lines[0]
    assert '"GET /health STWP/1.0" 200 2 "localhost" upstream=- ' in lines[1]


def test_no_matching_location_is_404(proxy, tmp_path):
    port = free_port()
    proxy("""
    stwp {
        access_log off;
        server { listen @port@; location /only/ { return 204; } }
    }
    """, [port], port=port)
    assert get(port, "/elsewhere").status == 404
    assert get(port, "/only/x").status == 204
