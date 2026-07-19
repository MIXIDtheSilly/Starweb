"""Wire compatibility with the C++ implementation, in both directions. These
are what prove the package belongs on StarWeb rather than merely near it."""

import subprocess

import pytest
import starweb

from conftest import REPO

STWP_CLIENT = REPO / "stwp_client"


def _cpp_server_running() -> bool:
    """The Python server now defaults to the same ports, so "something is
    listening on 8090" no longer means the C++ implementation is there — these
    tests would pass against Python and prove nothing. Check who answers."""
    try:
        res = starweb.get("moon://localhost/test.txt", timeout=1.0)
    except starweb.StarWebError:
        return False
    return res.headers.get("server") == "StarWeb/1.0"


requires_cpp_server = pytest.mark.skipif(
    not _cpp_server_running(), reason="start ./stwp_server first (8090/8490)"
)
requires_cpp_client = pytest.mark.skipif(
    not STWP_CLIENT.is_file(), reason="build stwp_client first"
)


@requires_cpp_server
@pytest.mark.parametrize("path", ["/test.txt", "/style.css", "/index.html", "/cat.jpg"])
def test_python_client_reads_cpp_server(path):
    expected = (REPO / "www" / path.lstrip("/")).read_bytes()
    assert starweb.get(f"moon://localhost{path}").body == expected
    assert starweb.get(f"star://localhost{path}").body == expected


@requires_cpp_server
def test_tls_profile_matches_cpp_server():
    res = starweb.get("star://localhost/test.txt")
    assert res.tls.version == "TLSv1.3"
    assert res.tls.alpn == "stwp/1.0"


@requires_cpp_server
def test_session_resumption_against_cpp_server():
    with starweb.Session() as s:
        assert not s.get("star://localhost/test.txt").tls.resumed
        assert s.get("star://localhost/style.css").tls.resumed


@requires_cpp_server
def test_cpp_server_rejects_non_get():
    assert starweb.post("moon://localhost/test.txt", body=b"x").status_code == 405


def _run_cpp_client(url: str) -> str:
    out = subprocess.run([str(STWP_CLIENT), url], capture_output=True,
                         timeout=15, cwd=REPO)
    return (out.stdout + out.stderr).decode(errors="replace")


@requires_cpp_client
def test_cpp_client_reads_python_server_over_moon(server, moon):
    out = _run_cpp_client(f"{moon}/api/ping")
    assert "Status Code:      200" in out
    assert '{"pong": true}' in out


@requires_cpp_client
def test_cpp_client_reads_python_server_over_star(server, star):
    out = _run_cpp_client(f"{star}/api/ping")
    assert "ALPN stwp/1.0" in out
    assert "Status Code:      200" in out
    assert '{"pong": true}' in out


@requires_cpp_client
def test_cpp_client_gets_python_static_file(server, moon):
    out = _run_cpp_client(f"{moon}/static/test.txt")
    assert "Status Code:      200" in out
    assert "Starmap txt test" in out


@requires_cpp_client
def test_cpp_client_sees_python_404(server, moon):
    assert "Status Code:      404" in _run_cpp_client(f"{moon}/nope")
