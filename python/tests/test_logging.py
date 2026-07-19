import starweb
from starweb import Server

from conftest import CERTS, certs_available


def _run(app, **kw):
    srv = Server(app, host="127.0.0.1", port=0, tls_port=None, **kw)
    srv.start()
    return srv


def test_silent_by_default(app, capsys):
    srv = _run(app)
    try:
        capsys.readouterr()
        starweb.get(f"moon://localhost:{srv.port}/api/ping")
        assert capsys.readouterr().out == ""
    finally:
        srv.stop()


def test_log_true_prints_one_line_per_request(app, capsys):
    srv = _run(app, log=True)
    try:
        capsys.readouterr()
        starweb.get(f"moon://localhost:{srv.port}/api/ping")
        starweb.get(f"moon://localhost:{srv.port}/nope")
        lines = [l for l in capsys.readouterr().out.splitlines() if l]
        assert len(lines) == 2
        assert "moon/TCP" in lines[0]
        assert "GET /api/ping STWP/1.0 -> 200" in lines[0]
        assert "-> 404" in lines[1]
    finally:
        srv.stop()


def test_log_callable_receives_lines(app):
    seen = []
    srv = _run(app, log=seen.append)
    try:
        starweb.get(f"moon://localhost:{srv.port}/api/ping")
        assert len(seen) == 1
        assert seen[0].startswith("[moon/TCP] GET /api/ping")
        assert "[Server]" not in seen[0]
    finally:
        srv.stop()


def test_star_transport_labelled(app):
    if not certs_available():
        return
    seen = []
    srv = Server(app, host="127.0.0.1", port=None, tls_port=0, log=seen.append,
                 cert=str(CERTS / "localhost.pem"), key=str(CERTS / "localhost.key"))
    srv.start()
    try:
        starweb.get(f"star://localhost:{srv.tls_port}/api/ping")
        assert seen[0].startswith("[star/TLS]")
    finally:
        srv.stop()
