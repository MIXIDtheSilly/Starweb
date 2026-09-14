import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BINARY = REPO / "stwp_proxy"

pytestmark = pytest.mark.skipif(not BINARY.is_file(), reason="run make stwp_proxy first")


def run(conf: Path, flag: str = "-t") -> subprocess.CompletedProcess:
    return subprocess.run([str(BINARY), flag, "-c", str(conf)], cwd=REPO,
                          capture_output=True, text=True, timeout=10)


def write(tmp_path: Path, text: str, name: str = "proxy.conf") -> Path:
    path = tmp_path / name
    path.write_text(text)
    return path


MINIMAL_SERVER = """
    server {
        listen 9090;
        location / { return 200 "ok"; }
    }
"""


def test_example_config_is_valid():
    result = run(REPO / "conf" / "stwp_proxy.conf")
    assert result.returncode == 0, result.stderr
    assert "test is successful" in result.stderr


def test_dump_resolves_inheritance_and_implicit_upstreams(tmp_path):
    conf = write(tmp_path, """
    stwp {
        proxy_set_header Star-Real-IP $remote_addr;
        proxy_read_timeout 30s;

        upstream api {
            server 127.0.0.1:8091 weight=3 max_fails=2 fail_timeout=5s;
            keepalive 4;
        }

        server {
            listen 9090 default_server;
            server_name Example.WEB. *.example.web;

            location /api/ { proxy_pass moon://api/v1/; }
            location /a/ { proxy_pass moon://127.0.0.1; }
            location /b/ { proxy_pass moon://127.0.0.1:8090; }
            location /s/ {
                proxy_pass star://localhost;
                proxy_set_header X-Only $host;
                proxy_connect_timeout 250ms;
            }
        }
    }
    """)
    result = run(conf, "-T")
    assert result.returncode == 0, result.stderr
    out = result.stdout

    assert "server 127.0.0.1:8091 weight=3 max_fails=2 fail_timeout=5000ms;" in out
    assert "keepalive 4;" in out
    assert "server_name example.web *.example.web;" in out
    assert "proxy_pass moon://api/v1/;  # upstream api" in out
    assert out.count("upstream 127.0.0.1:8090 {  # implicit") == 1
    assert "upstream localhost:8490 {  # implicit" in out

    api_block = out.split("location /api/")[1].split("}")[0]
    assert "proxy_set_header star-real-ip $remote_addr;" in api_block
    assert "proxy_read_timeout 30000ms;" in api_block

    star_block = out.split("location /s/")[1].split("}")[0]
    assert "star-real-ip" not in star_block
    assert "proxy_set_header x-only $host;" in star_block
    assert "proxy_connect_timeout 250ms;" in star_block
    assert "proxy_read_timeout 30000ms;" in star_block


def test_include_with_wildcard(tmp_path):
    sites = tmp_path / "sites"
    sites.mkdir()
    (sites / "a.conf").write_text("""
        server { listen 9090; server_name a.web; location / { return 200 "a"; } }
    """)
    (sites / "b.conf").write_text("""
        server { listen 9090; server_name b.web; location / { return 200 "b"; } }
    """)
    (sites / "ignored.txt").write_text("this is not config")
    conf = write(tmp_path, "stwp { include sites/*.conf; }")
    result = run(conf, "-T")
    assert result.returncode == 0, result.stderr
    assert "server_name a.web;" in result.stdout
    assert "server_name b.web;" in result.stdout


def test_include_error_names_the_included_file(tmp_path):
    (tmp_path / "bad.conf").write_text("server {\n    listen 9090;\n    bogus on;\n}\n")
    conf = write(tmp_path, "stwp {\n    include bad.conf;\n}\n")
    result = run(conf)
    assert result.returncode == 1
    assert f"{tmp_path / 'bad.conf'}:3: unknown directive \"bogus\"" in result.stderr


@pytest.mark.parametrize("body, expected", [
    ("stwp {\n" + MINIMAL_SERVER + "\n    bogus on;\n}", ':8: unknown directive "bogus"'),
    ("stwp {\n    server {\n        listen 9090\n    }\n}", ':3: directive "listen" is not terminated by ";"'),
    ("stwp {\n" + MINIMAL_SERVER, 'unexpected end of file, expecting "}"'),
    ("stwp { }\n}", ':2: unexpected "}"'),
    ('stwp {\n    access_log "logs/a.log;\n}', ':2: unterminated string'),
    ("error_log a;\n", 'no "stwp" block'),
    ("stwp { }", 'no "server" blocks'),
    ("stwp {\n    listen 9090;\n}", ':2: "listen" directive is not allowed here'),
    ("stwp {\n    server {\n        location / { proxy_pass http://127.0.0.1:80; }\n        listen 9090;\n    }\n}",
     ':3: proxy_pass accepts only moon:// and star:// URLs'),
    ("stwp {\n    server {\n        listen 9090;\n        location / { proxy_pass 127.0.0.1; }\n    }\n}",
     ':4: invalid URL prefix'),
    ("stwp {\n    server {\n        listen 9090;\n        location ~ \\.lua$ { return 404; }\n    }\n}",
     ':4: regex locations are not supported'),
    ("stwp {\n    server {\n        listen 9090;\n        location / { }\n    }\n}",
     ':4: location "/" has no "proxy_pass", "root" or "return"'),
    ("stwp {\n    server {\n        listen 9090;\n        location / { root www; return 200; }\n    }\n}",
     ':4: location "/" has more than one of'),
    ("stwp {\n    server {\n        listen 9090;\n        location / { return 200; }\n        location / { return 204; }\n    }\n}",
     ':5: duplicate location "/"'),
    ("stwp {\n    server {\n        location / { return 200; }\n    }\n}", ':2: server has no "listen"'),
    ("stwp {\n    server {\n        listen 9490 star;\n        location / { return 200; }\n    }\n}",
     ':2: server listens with "star" but has no "tls_certificate"'),
    ("stwp {\n    server {\n        listen 9490 star;\n        tls_certificate missing.pem;\n"
     "        tls_certificate_key missing.key;\n        location / { return 200; }\n    }\n}",
     ':4: certificate "missing.pem" not found'),
    ("stwp {\n    server { listen 9090; location / { return 200; } }\n"
     "    server {\n        listen 9090 star;\n        location / { return 200; }\n    }\n}",
     ':4: port 9090 is used both with and without "star"'),
    ("stwp {\n    server { listen 9090 default_server; location / { return 200; } }\n"
     "    server {\n        listen 9090 default_server;\n        location / { return 200; }\n    }\n}",
     ':4: a duplicate default server for port 9090'),
    ("stwp {\n    server { listen 9090; server_name a.web; location / { return 200; } }\n"
     "    server {\n        listen 9090;\n        server_name A.web;\n        location / { return 200; }\n    }\n}",
     ':3: conflicting server name "a.web" on port 9090'),
    ("stwp {\n    server {\n        listen 9090;\n        server_name a.*.web;\n        location / { return 200; }\n    }\n}",
     ':4: invalid server name "a.*.web"'),
    ("stwp {\n    server {\n        listen 70000;\n        location / { return 200; }\n    }\n}",
     ':3: invalid port "70000"'),
    ("stwp {\n    server {\n        listen 9090 ssl;\n        location / { return 200; }\n    }\n}",
     ':3: invalid parameter "ssl"'),
    ("stwp {\n    upstream u {\n        server 127.0.0.1;\n    }\n" + MINIMAL_SERVER + "}",
     ':3: upstream server "127.0.0.1" has no port'),
    ("stwp {\n    upstream u {\n        server 127.0.0.1:1 weight=0;\n    }\n" + MINIMAL_SERVER + "}",
     ':3: invalid parameter "weight=0"'),
    ("stwp {\n    upstream u { }\n" + MINIMAL_SERVER + "}", ':2: no servers are inside upstream "u"'),
    ("stwp {\n    upstream u { server 127.0.0.1:1; }\n    upstream U { server 127.0.0.1:2; }\n" + MINIMAL_SERVER + "}",
     ':3: duplicate upstream "U"'),
    ("stwp {\n    proxy_set_header Star-Id $request_id;\n" + MINIMAL_SERVER + "}",
     ':2: unknown "request_id" variable'),
    ("stwp {\n    proxy_set_header Content-Length 0;\n" + MINIMAL_SERVER + "}",
     ':2: "content-length" is managed by the proxy'),
    ("stwp {\n    proxy_read_timeout soon;\n" + MINIMAL_SERVER + "}", ':2: invalid time "soon"'),
    ("stwp {\n    client_max_body_size 8x;\n" + MINIMAL_SERVER + "}", ':2: invalid size "8x"'),
    ("stwp {\n    keepalive_timeout 5s;\n    keepalive_timeout 6s;\n" + MINIMAL_SERVER + "}",
     ':3: "keepalive_timeout" directive is duplicate'),
    ("stwp {\n    tls_trusted_certificate nowhere.pem;\n    server {\n        listen 9090;\n"
     "        location / { proxy_pass star://localhost; }\n    }\n}",
     ':2: trusted certificate "nowhere.pem" not found'),
    ("stwp {\n    server {\n        listen 9090;\n        location / { return 99; }\n    }\n}",
     ':4: invalid return code "99"'),
])
def test_errors_point_at_the_offending_line(tmp_path, body, expected):
    conf = write(tmp_path, body)
    result = run(conf)
    assert result.returncode == 1, result.stdout
    assert "test failed" in result.stderr
    assert expected in result.stderr, result.stderr


def test_missing_config_file(tmp_path):
    result = run(tmp_path / "nope.conf")
    assert result.returncode == 1
    assert "cannot open" in result.stderr
