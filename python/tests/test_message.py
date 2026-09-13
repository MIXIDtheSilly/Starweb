import pytest
from starweb.message import Request, Response, parse_request, parse_response


def test_request_roundtrip():
    req = Request("GET", "/index.html",
                  headers={"Host": "localhost", "Connection": "close"})
    parsed, consumed = parse_request(req.serialize())
    assert consumed == len(req.serialize())
    assert parsed.method == "GET"
    assert parsed.path == "/index.html"
    assert parsed.version == "STWP/1.0"
    assert parsed.headers["host"] == "localhost"


def test_header_names_lowercased_and_values_trimmed():
    raw = b"GET / STWP/1.0\r\nHOST:  localhost  \r\nX-Odd:\tvalue\r\n\r\n"
    req, _ = parse_request(raw)
    assert req.headers["host"] == "localhost"
    assert req.headers["x-odd"] == "value"


def test_bare_lf_separator_accepted():
    raw = b"GET / STWP/1.0\nHost: localhost\n\nbody"
    req, consumed = parse_request(raw)
    assert req.headers["host"] == "localhost"
    assert consumed == len(raw) - 4


def test_body_framed_by_content_length():
    raw = b"POST /x STWP/1.0\r\nContent-Length: 5\r\n\r\nhelloEXTRA"
    req, consumed = parse_request(raw)
    assert req.body == b"hello"
    assert consumed == len(raw) - 5


def test_incomplete_returns_none():
    assert parse_request(b"GET / STWP/1.0\r\nHost: local") is None
    assert parse_request(b"GET / STWP/1.0\r\nContent-Length: 10\r\n\r\nshort") is None


def test_bad_content_length_ignored():
    raw = b"GET / STWP/1.0\r\nContent-Length: abc\r\n\r\n"
    req, _ = parse_request(raw)
    assert req.body == b""


def test_response_roundtrip():
    res = Response(404, "Not Found", body=b"nope",
                   headers={"Content-Length": "4"})
    parsed, _ = parse_response(res.serialize())
    assert parsed.status_code == 404
    assert parsed.status_text == "Not Found"
    assert parsed.body == b"nope"
    assert not parsed.ok


def test_response_without_status_text():
    res, _ = parse_response(b"STWP/1.0 200\r\n\r\n")
    assert res.status_code == 200
    assert res.status_text == ""


def test_response_non_numeric_status_rejected():
    assert parse_response(b"STWP/1.0 OK OK\r\n\r\n") is None


def test_query_parsing():
    req = Request("GET", "/api?a=1&b=two#frag")
    assert req.query == {"a": "1", "b": "two"}


def test_binary_body_survives():
    payload = bytes(range(256))
    res = Response(body=payload, headers={"Content-Length": str(len(payload))})
    parsed, _ = parse_response(res.serialize())
    assert parsed.body == payload


def test_set_cookie_defaults_to_a_stwp_only_session_cookie():
    res = Response()
    res.set_cookie("sid", "abc123")
    assert res.headers["Set-Cookie"] == "sid=abc123; StwpOnly"


def test_set_cookie_with_max_age():
    res = Response().set_cookie("sid", "abc", max_age=604800)
    assert res.headers["Set-Cookie"] == "sid=abc; Max-Age=604800; StwpOnly"


def test_two_cookies_share_one_header():
    res = Response()
    res.set_cookie("sid", "abc")
    res.set_cookie("theme", "dark", stwp_only=False)
    assert res.headers["Set-Cookie"] == "sid=abc; StwpOnly | theme=dark"


def test_delete_cookie_is_a_zero_max_age():
    assert Response().delete_cookie("sid").headers["Set-Cookie"] == \
        "sid=; Max-Age=0; StwpOnly"


@pytest.mark.parametrize("name,value", [
    ("a;b", "x"), ("a=b", "x"), ("a|b", "x"), ("a,b", "x"), ("", "x"),
    ("a b", "x"), ("sid", "a;b"), ("sid", "a|b"), ("sid", "a\nb"),
])
def test_cookie_delimiters_are_rejected(name, value):
    with pytest.raises(ValueError):
        Response().set_cookie(name, value)


def test_cookie_header_parsing():
    req = Request(headers={"cookie": "sid=abc123; theme=dark"})
    assert req.cookies == {"sid": "abc123", "theme": "dark"}


def test_no_cookie_header_is_empty():
    assert Request().cookies == {}


def test_cookie_survives_serialization():
    res = Response().set_cookie("sid", "abc", max_age=60)
    parsed, _ = parse_response(res.serialize())
    assert parsed.headers["set-cookie"] == "sid=abc; Max-Age=60; StwpOnly"
