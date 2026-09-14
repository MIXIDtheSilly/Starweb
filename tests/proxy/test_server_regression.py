import subprocess
import time
from pathlib import Path

import pytest

from conftest import REPO, Client, free_port

SERVER = REPO / "stwp_server"


@pytest.fixture
def server(tmp_path):
    if not SERVER.is_file():
        pytest.skip("run make stwp_server first")
    port = free_port()
    log = tmp_path / "server.log"
    proc = subprocess.Popen([str(SERVER), str(port), "--no-tls"], cwd=REPO,
                            stdout=log.open("w"), stderr=subprocess.STDOUT)
    deadline = time.time() + 5
    while True:
        try:
            Client(port).close()
            break
        except OSError:
            if time.time() > deadline:
                proc.kill()
                raise
            time.sleep(0.02)
    yield port, log
    proc.kill()
    proc.wait()


def test_static_serving_unchanged(server):
    port, log = server
    index = (REPO / "www" / "index.html").read_bytes()
    size = len(index)

    with Client(port) as c:
        r = c.request(path="/", keep_alive=True)
        assert r.status == 200 and r.body == index
        assert r.headers["content-type"] == "text/html"
        assert r.headers["server"] == "StarWeb/1.0"

        r = c.request(path="/index.html", headers={"Range": "bytes=0-9"}, keep_alive=True)
        assert r.status == 206 and r.body == index[:10]
        assert r.headers["content-range"] == f"bytes 0-9/{size}"

        r = c.request(path="/index.html", headers={"Range": "bytes=a-b"}, keep_alive=True)
        assert r.status == 416

        assert c.request(path="/nope.html", keep_alive=True).status == 404
        assert c.request(path="/../PROTOCOL.md", keep_alive=True).status == 403
        assert c.request(method="POST", path="/", keep_alive=True).status == 405
        assert c.request(path="/index.html").status == 200

    with Client(port) as c:
        c.send(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        assert c.read_reply().status == 505

    big = next(p for p in sorted((REPO / "www").iterdir(), key=lambda p: p.stat().st_size)
               if p.is_file() and p.stat().st_size > 1_000_000)
    with Client(port, timeout=60) as c:
        r = c.request(path="/" + big.name)
        assert r.status == 200 and r.body == big.read_bytes()

    assert "[kept alive]" in log.read_text()
