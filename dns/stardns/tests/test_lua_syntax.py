"""Every page ships a Lua program; a typo in one is invisible until the page
is opened in the browser, so the pages are rendered here and parsed."""
import json
import re
import subprocess
import tempfile
from pathlib import Path

import pytest
from starweb import Request

from stardns import panel, ui

PAGES = ("/panel", "/domains", "/analytics", "/analytics/a.web", "/domain/a.web",
         "/search")


def _parses(name: str, src: str) -> None:
    with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
        f.write(src)
        path = Path(f.name)
    result = subprocess.run(["luac", "-p", str(path)], capture_output=True, text=True)
    path.unlink()
    assert result.returncode == 0, f"{name}: {result.stderr}"


@pytest.fixture
def token(fake_db):
    body = json.dumps({"username": "tester", "password": "hunter2hunter2"}).encode()
    res = panel.app.handle(Request(method="POST", path="/api/register", body=body,
                                   headers={"content-length": str(len(body))}))
    tok = res.json()["token"]
    add = json.dumps({"token": tok, "domain": "a.web"}).encode()
    panel.app.handle(Request(method="POST", path="/api/domain/add", body=add,
                             headers={"content-length": str(len(add))}))
    return tok


def test_page_scripts_parse(token):
    pages = {"login": ui.login_page()}
    for path in PAGES:
        res = panel.app.handle(Request(method="GET", path=f"{path}?t={token}"))
        assert res.status_code == 200, f"{path} -> {res.status_code}"
        pages[path] = res.text
    for name, html in pages.items():
        blocks = re.findall(r"<script>\n(.*?)\n</script>", html, re.S)
        assert blocks, f"{name} carries no script"
        for i, src in enumerate(blocks):
            _parses(f"{name}[{i}]", src)
