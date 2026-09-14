from conftest import Client, free_port, get, pattern

ROUTES = """
stwp {
    access_log off;

    server {
        listen @p1@;
        server_name a.web;
        location / { return 200 "a"; }
    }
    server {
        listen @p1@ default_server;
        server_name b.web www.b.web;
        location / { return 200 "b"; }
    }
    server {
        listen @p1@;
        server_name *.c.web;
        location / { return 200 "wild-c"; }
    }
    server {
        listen @p1@;
        server_name *.deep.c.web;
        location / { return 200 "wild-deep"; }
    }
    server {
        listen @p1@;
        listen @p2@;
        server_name exact.deep.c.web;
        location / { return 200 "exact-deep"; }
    }
    server {
        listen @p2@;
        server_name only-p2.web;
        location / { return 200 "p2"; }
    }
}
"""


def body(port, host=None, path="/"):
    with Client(port) as c:
        if host is None:
            c.send(f"GET {path} STWP/1.0\r\nConnection: close\r\n\r\n".encode())
            return c.read_reply().body.decode()
        return c.request(path=path, host=host).body.decode()


def test_server_selection_by_name(proxy):
    p1, p2 = free_port(), free_port()
    proxy(ROUTES, [p1, p2], p1=p1, p2=p2)

    assert body(p1, "a.web") == "a"
    assert body(p1, "A.WEB:9090") == "a"
    assert body(p1, "a.web.") == "a"
    assert body(p1, "www.b.web") == "b"
    assert body(p1, "unknown.web") == "b"
    assert body(p1) == "b"

    assert body(p1, "x.c.web") == "wild-c"
    assert body(p1, "x.y.c.web") == "wild-c"
    assert body(p1, "c.web") == "b"
    assert body(p1, "x.deep.c.web") == "wild-deep"
    assert body(p1, "exact.deep.c.web") == "exact-deep"

    assert body(p2, "only-p2.web") == "p2"
    assert body(p2, "a.web") == "exact-deep"


LOCATIONS = """
stwp {
    access_log off;
    server {
        listen @port@;
        location / { return 200 "root"; }
        location /a/ { return 200 "a"; }
        location /a/b/ { return 200 "ab"; }
        location = /a/b/c { return 200 "exact"; }
        location ^~ /a/bx { return 200 "abx"; }
        location /files/ { root @root@; }
        location /docs/ { root @root@; index start.html; }
    }
}
"""


def test_location_matching(proxy, tmp_path):
    port = free_port()
    proxy(LOCATIONS, [port], port=port, root=tmp_path)
    assert body(port, path="/") == "root"
    assert body(port, path="/zzz") == "root"
    assert body(port, path="/a") == "root"
    assert body(port, path="/a/") == "a"
    assert body(port, path="/a/x") == "a"
    assert body(port, path="/a/b/c") == "exact"
    assert body(port, path="/a/b/c?q=1") == "exact"
    assert body(port, path="/a/b/cd") == "ab"
    assert body(port, path="/a/bxy") == "abx"


def test_root_serves_files_with_ranges(proxy, tmp_path):
    files = tmp_path / "files"
    files.mkdir()
    data = pattern(300_000)
    (files / "blob.bin").write_bytes(data)
    (files / "hello.txt").write_text("hello")
    (files / "index.html").write_text("<p>index</p>")
    docs = tmp_path / "docs"
    docs.mkdir()
    (docs / "start.html").write_text("<p>start</p>")
    (tmp_path / "secret.txt").write_text("nope")

    port = free_port()
    proxy(LOCATIONS, [port], port=port, root=tmp_path)

    with Client(port) as c:
        r = c.request(path="/files/hello.txt", keep_alive=True)
        assert (r.status, r.body, r.headers["content-type"]) == (200, b"hello", "text/plain")
        assert r.headers["accept-ranges"] == "bytes"

        r = c.request(path="/files/blob.bin", keep_alive=True)
        assert r.status == 200 and r.body == data

        r = c.request(path="/files/blob.bin", headers={"Range": "bytes=100000-199999"}, keep_alive=True)
        assert r.status == 206
        assert r.headers["content-range"] == "bytes 100000-199999/300000"
        assert r.body == data[100000:200000]

        r = c.request(path="/files/blob.bin", headers={"Range": "bytes=-10"}, keep_alive=True)
        assert r.body == data[-10:]

        r = c.request(path="/files/blob.bin", headers={"Range": "bytes=999999-"}, keep_alive=True)
        assert r.status == 416
        assert r.headers["content-range"] == "bytes */300000"

        assert c.request(path="/files/", keep_alive=True).body == b"<p>index</p>"
        assert c.request(path="/docs/", keep_alive=True).body == b"<p>start</p>"
        assert c.request(path="/files/missing", keep_alive=True).status == 404
        assert c.request(path="/files/../secret.txt", keep_alive=True).status == 403
        assert c.request(path="/files", keep_alive=True).status == 200
        assert c.request(method="POST", path="/files/hello.txt", body=b"x", keep_alive=True).status == 405
        assert c.request(path="/files/hello.txt").status == 200
        assert c.is_closed()
