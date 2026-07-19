import pytest

from starweb.errors import URLError
from starweb.url import parse_url


def test_defaults():
    u = parse_url("moon://localhost/index.html")
    assert (u.scheme, u.host, u.port, u.path) == ("moon", "localhost", 8090, "/index.html")
    assert parse_url("star://localhost/").port == 8490


def test_path_defaults_to_slash():
    assert parse_url("star://localhost").path == "/"


def test_scheme_lowercased():
    assert parse_url("STAR://localhost/").scheme == "star"


def test_explicit_port():
    u = parse_url("star://localhost:9999/x")
    assert u.port == 9999
    assert u.host_header == "localhost:9999"


def test_default_port_elided_from_host_header():
    assert parse_url("star://localhost:8490/x").host_header == "localhost"
    assert parse_url("moon://localhost:8090/x").host_header == "localhost"


def test_ipv6():
    u = parse_url("star://[::1]:8490/x")
    assert u.host == "::1"
    assert u.port == 8490
    assert u.host_header == "[::1]"


@pytest.mark.parametrize("url", [
    "https://example.com/",
    "http://localhost/",
    "file:///etc/passwd",
    "example.com/x",
    "star://",
    "star://localhost:/x",
    "star://localhost:abc/x",
    "star://[::1/x",
])
def test_rejected(url):
    with pytest.raises(URLError):
        parse_url(url)
