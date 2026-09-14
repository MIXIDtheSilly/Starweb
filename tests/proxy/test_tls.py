import ssl
import subprocess

import pytest

from conftest import BINARY, REPO, Client, free_port, get, tls_client

LOCALHOST_CERT = REPO / "certs" / "localhost.pem"
LOCALHOST_KEY = REPO / "certs" / "localhost.key"

CONF = """
error_log @dir@/error.log;
stwp {
    access_log off;

    server {
        listen @sp@ star default_server;
        server_name alpha.web;
        tls_certificate @alpha_crt@;
        tls_certificate_key @alpha_key@;
        location / { return 200 "alpha"; }
        location /up/ { proxy_pass moon://127.0.0.1:@up@; }
    }
    server {
        listen @sp@ star;
        server_name beta.web;
        tls_certificate @beta_crt@;
        tls_certificate_key @beta_key@;
        location / { return 200 "beta"; }
    }
    server {
        listen @pp@;
        location /good/ { proxy_pass star://localhost:@good@/; }
        location /bad/ { proxy_pass star://localhost:@bad@/; }
    }
}
"""


@pytest.fixture
def tls_proxy(proxy, backend, pki, tmp_path):
    if not LOCALHOST_CERT.is_file():
        pytest.skip("run tools/make_certs.sh first")
    alpha = pki("alpha.web")
    beta = pki("beta.web")
    up = backend("plain")
    good = backend("good", scheme="star", cert=str(LOCALHOST_CERT), key=str(LOCALHOST_KEY))
    bad = backend("bad", scheme="star", cert=str(alpha[0]), key=str(alpha[1]))
    sp, pp = free_port(), free_port()
    p = proxy(CONF, [sp, pp], dir=tmp_path, sp=sp, pp=pp, up=up.port, good=good.tls_port,
              bad=bad.tls_port, alpha_crt=alpha[0], alpha_key=alpha[1], beta_crt=beta[0], beta_key=beta[1])
    return p, sp, pp, tmp_path


def san(client: Client):
    return client.sock.getpeercert()["subjectAltName"]


def test_sni_selects_the_certificate(tls_proxy):
    _, sp, _, _ = tls_proxy
    for name in ("alpha.web", "beta.web"):
        with Client(sp, tls=tls_client(), sni=name) as c:
            assert san(c) == (("DNS", name),)
            assert c.request(host=name).body == name.split(".")[0].encode()

    with Client(sp, tls=tls_client(verify_hostname=False), sni="gamma.web") as c:
        assert san(c) == (("DNS", "alpha.web"),)


def test_host_must_agree_with_sni(tls_proxy):
    _, sp, _, _ = tls_proxy
    with Client(sp, tls=tls_client(), sni="alpha.web") as c:
        r = c.request(host="beta.web", keep_alive=True)
        assert r.status == 421
        assert c.is_closed()

    with Client(sp, tls=tls_client(verify_hostname=False), sni=None) as c:
        assert c.request(host="beta.web").body == b"beta"


def test_star_client_keep_alive_and_forwarded_proto(tls_proxy):
    _, sp, _, _ = tls_proxy
    with Client(sp, tls=tls_client(), sni="alpha.web") as c:
        for i in range(2):
            seen = c.request(path=f"/up/{i}", host="alpha.web", keep_alive=True).json()
            assert seen["path"] == f"/up/{i}"
            assert seen["headers"]["star-forwarded-proto"] == "star"
        assert c.request(path="/up/last", host="alpha.web").status == 200
        assert c.is_closed()


def test_handshake_requires_alpn_and_tls13(tls_proxy):
    _, sp, _, tmp_path = tls_proxy
    with pytest.raises(OSError):
        with Client(sp, tls=tls_client(alpn=False), sni="alpha.web") as c:
            c.request(host="alpha.web")

    old = tls_client()
    old.minimum_version = ssl.TLSVersion.TLSv1_2
    old.maximum_version = ssl.TLSVersion.TLSv1_2
    with pytest.raises(OSError):
        Client(sp, tls=old, sni="alpha.web")

    assert "did not negotiate ALPN stwp/1.0" in (tmp_path / "error.log").read_text()


def test_proxy_pass_star_verifies_the_upstream(tls_proxy):
    _, _, pp, tmp_path = tls_proxy
    r = get(pp, "/good/hello")
    assert r.status == 200
    seen = r.json()
    assert (seen["backend"], seen["path"]) == ("good", "/hello")

    assert get(pp, "/bad/hello").status == 502
    assert "certificate verify failed" in (tmp_path / "error.log").read_text()


def test_check_catches_mismatched_key(pki, tmp_path):
    alpha = pki("alpha.web")
    beta = pki("beta.web")
    conf = tmp_path / "mismatch.conf"
    conf.write_text(f"""stwp {{
    server {{
        listen 9490 star;
        tls_certificate {alpha[0]};
        tls_certificate_key {beta[1]};
        location / {{ return 200; }}
    }}
}}
""")
    result = subprocess.run([str(BINARY), "-t", "-c", str(conf)], cwd=REPO, capture_output=True, text=True)
    assert result.returncode == 1
    assert f"{conf}:2: loading key" in result.stderr
    assert "key values mismatch" in result.stderr
