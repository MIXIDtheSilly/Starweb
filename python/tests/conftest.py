import time
from pathlib import Path

import pytest

from starweb import App, Server

REPO = Path(__file__).resolve().parents[2]
CERTS = REPO / "certs"


def certs_available() -> bool:
    return (CERTS / "localhost.pem").is_file() and (CERTS / "localhost.key").is_file()


requires_certs = pytest.mark.skipif(
    not certs_available(),
    reason="run tools/make_certs.sh first",
)


@pytest.fixture
def app():
    app = App()

    @app.route("/api/ping")
    def ping(req):
        return {"pong": True}

    @app.route("/api/echo", methods=["POST"])
    def echo(req):
        return req.body

    @app.route("/api/item/<name>", methods=["GET", "DELETE"])
    def item(req, name):
        return {"name": name, "method": req.method}

    @app.route("/api/boom")
    def boom(req):
        raise RuntimeError("intentional")

    @app.route("/hello")
    def hello(req):
        return "<h1>hi</h1>"

    app.mount_static("/static", str(REPO / "www"))
    return app


@pytest.fixture
def server(app):
    srv = Server(app, host="127.0.0.1", port=0,
                 tls_port=0 if certs_available() else None,
                 cert=str(CERTS / "localhost.pem") if certs_available() else None,
                 key=str(CERTS / "localhost.key") if certs_available() else None)
    srv.start()
    time.sleep(0.05)
    yield srv
    srv.stop()


@pytest.fixture
def moon(server):
    return f"moon://localhost:{server.port}"


@pytest.fixture
def star(server):
    if server.tls_port is None:
        pytest.skip("star:// not enabled")
    return f"star://localhost:{server.tls_port}"
