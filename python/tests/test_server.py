import starweb


def test_json_route(moon):
    res = starweb.get(f"{moon}/api/ping")
    assert res.status_code == 200
    assert res.json() == {"pong": True}
    assert res.headers["content-type"] == "application/json"


def test_post_body_echoed(moon):
    res = starweb.post(f"{moon}/api/echo", body=b"round trip")
    assert res.body == b"round trip"


def test_json_request(moon):
    res = starweb.post(f"{moon}/api/echo", json={"a": 1})
    assert res.json() == {"a": 1}


def test_path_params(moon):
    res = starweb.get(f"{moon}/api/item/widget")
    assert res.json() == {"name": "widget", "method": "GET"}


def test_delete_shares_route(moon):
    res = starweb.delete(f"{moon}/api/item/widget")
    assert res.json()["method"] == "DELETE"


def test_method_not_allowed_lists_allow(moon):
    res = starweb.post(f"{moon}/api/ping", body=b"x")
    assert res.status_code == 405
    assert res.headers["allow"] == "GET"


def test_unknown_route_404(moon):
    assert starweb.get(f"{moon}/nope").status_code == 404


def test_handler_exception_is_500_without_traceback(moon):
    res = starweb.get(f"{moon}/api/boom")
    assert res.status_code == 500
    assert b"intentional" not in res.body
    assert b"Traceback" not in res.body


def test_str_return_is_html(moon):
    res = starweb.get(f"{moon}/hello")
    assert res.headers["content-type"] == "text/html"
    assert res.text == "<h1>hi</h1>"


def test_static_file(moon):
    res = starweb.get(f"{moon}/static/test.txt")
    assert res.status_code == 200
    assert res.headers["content-type"] == "text/plain"


def test_static_traversal_blocked(moon):
    res = starweb.get(f"{moon}/static/../../certs/starweb_root.key")
    assert res.status_code == 403


def test_content_length_always_set(moon):
    res = starweb.get(f"{moon}/api/ping")
    assert int(res.headers["content-length"]) == len(res.body)
    assert res.headers["connection"] == "close"
