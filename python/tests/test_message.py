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
