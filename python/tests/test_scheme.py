import pytest
import starweb
from starweb import Server

from conftest import CERTS, certs_available

CERT = str(CERTS / "localhost.pem")
KEY = str(CERTS / "localhost.key")


def _server(**kw):
    srv = Server(kw.pop("app"), host="127.0.0.1", port=0, tls_port=0, **kw)
    srv.start()
    return srv


def test_moon_only(app):
    srv = _server(app=app, scheme="moon", cert=CERT, key=KEY)
    try:
        assert srv.port is not None
        assert srv.tls_port is None
        assert starweb.get(f"moon://localhost:{srv.port}/api/ping").ok
    finally:
        srv.stop()


@pytest.mark.skipif(not certs_available(), reason="run tools/make_certs.sh first")
def test_star_only(app):
    srv = _server(app=app, scheme="star", cert=CERT, key=KEY)
    try:
        assert srv.port is None
        assert srv.tls_port is not None
        assert starweb.get(f"star://localhost:{srv.tls_port}/api/ping").ok
    finally:
        srv.stop()


@pytest.mark.skipif(not certs_available(), reason="run tools/make_certs.sh first")
def test_both(app):
    srv = _server(app=app, scheme="both", cert=CERT, key=KEY)
    try:
        assert srv.port is not None and srv.tls_port is not None
    finally:
        srv.stop()


def test_star_only_without_cert_refuses_rather_than_downgrading(app):
    with pytest.raises(RuntimeError, match="star:// requested"):
        Server(app, host="127.0.0.1", scheme="star", port=0, tls_port=0)


@pytest.mark.parametrize("scheme", ["star", "both"])
def test_unloadable_cert_is_fatal_even_when_moon_could_serve(app, scheme):
    # The silent-downgrade trap: a mistyped cert path must not quietly leave a
    # plaintext-only origin behind.
    with pytest.raises(RuntimeError, match="cannot load"):
        Server(app, host="127.0.0.1", scheme=scheme, port=0, tls_port=0,
               cert="/no/such/cert.pem", key="/no/such/key.pem")


def test_both_without_cert_degrades_to_moon(app, capsys):
    srv = Server(app, host="127.0.0.1", scheme="both", port=0, tls_port=0)
    try:
        assert srv.port is not None
        assert srv.tls_port is None
        assert "star:// disabled" in capsys.readouterr().out
    finally:
        srv.stop()


def test_invalid_scheme_rejected(app):
    with pytest.raises(ValueError, match="scheme must be one of"):
        Server(app, scheme="https", port=0, tls_port=0)


def test_no_listeners_is_an_error(app):
    with pytest.raises(RuntimeError, match="nothing to serve"):
        Server(app, scheme="moon", port=None, tls_port=0)
