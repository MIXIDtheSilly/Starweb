import signal
import socket
import threading
import time

from conftest import Client, free_port, get

CONF = """
error_log @dir@/error.log;
stwp {
    access_log off;
    server {
        listen @port@;
        location / { return 200 "one"; }
        location /slow { proxy_pass moon://127.0.0.1:@up@; }
    }
}
"""


def wait_until(check, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise AssertionError("condition not met in time")


def error_log(tmp_path) -> str:
    path = tmp_path / "error.log"
    return path.read_text() if path.exists() else ""


def refused(port) -> bool:
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return False
    except OSError:
        return True


def start(proxy, backend, tmp_path):
    up = backend("slow")
    port = free_port()
    p = proxy(CONF, [port], dir=tmp_path, port=port, up=up.port)
    return p, port


def rewrite(p, old, new):
    text = p.conf_path.read_text()
    assert old in text
    p.conf_path.write_text(text.replace(old, new))


def slow_request(port, out):
    out["reply"] = get(port, "/slow", keep_alive=True)


def test_reload_applies_the_new_config(proxy, backend, tmp_path):
    p, port = start(proxy, backend, tmp_path)
    idle = Client(port)
    assert idle.request(keep_alive=True).body == b"one"

    busy = {}
    t = threading.Thread(target=slow_request, args=(port, busy))
    t.start()
    time.sleep(0.3)

    rewrite(p, '"one"', '"two"')
    p.proc.send_signal(signal.SIGHUP)
    wait_until(lambda: get(port).body == b"two")
    assert "[notice] configuration reloaded" in error_log(tmp_path)

    assert idle.is_closed()
    t.join()
    assert busy["reply"].status == 200
    assert busy["reply"].headers["connection"] == "close"


def test_bad_reload_keeps_serving(proxy, backend, tmp_path):
    p, port = start(proxy, backend, tmp_path)
    rewrite(p, 'return 200 "one";', "bogus;")
    p.proc.send_signal(signal.SIGHUP)
    wait_until(lambda: "reload failed" in error_log(tmp_path))
    assert 'unknown directive "bogus"' in error_log(tmp_path)
    assert get(port).body == b"one"


def test_reload_cannot_change_listen_ports(proxy, backend, tmp_path):
    p, port = start(proxy, backend, tmp_path)
    other = free_port()
    rewrite(p, f"listen {port};", f"listen {other};")
    p.proc.send_signal(signal.SIGHUP)
    wait_until(lambda: "listen ports changed" in error_log(tmp_path))
    assert get(port).body == b"one"
    assert refused(other)


def test_graceful_shutdown(proxy, backend, tmp_path):
    p, port = start(proxy, backend, tmp_path)
    idle = Client(port)
    idle.request(keep_alive=True)

    busy = {}
    t = threading.Thread(target=slow_request, args=(port, busy))
    t.start()
    time.sleep(0.3)

    p.proc.send_signal(signal.SIGTERM)
    assert idle.is_closed()
    wait_until(lambda: refused(port), timeout=2)
    assert p.proc.poll() is None

    t.join()
    assert busy["reply"].status == 200
    assert busy["reply"].headers["connection"] == "close"
    assert p.proc.wait(5) == 0
    assert "[notice] shutting down" in error_log(tmp_path)


def test_shutdown_is_prompt_when_only_idle_connections_remain(proxy, backend, tmp_path):
    p, port = start(proxy, backend, tmp_path)
    clients = [Client(port) for _ in range(5)]
    for c in clients:
        c.request(keep_alive=True)
    started = time.time()
    p.proc.send_signal(signal.SIGINT)
    assert p.proc.wait(5) == 0
    assert time.time() - started < 1.5
    assert all(c.is_closed() for c in clients)
