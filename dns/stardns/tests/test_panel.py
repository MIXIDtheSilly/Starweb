import json

import pytest
from starweb import Request

from stardns import config, panel, zones

ZONE = config.ZONE


def get(path: str):
    return panel.app.handle(Request(method="GET", path=path))


def post(path: str, payload: dict):
    body = json.dumps(payload).encode()
    return panel.app.handle(Request(method="POST", path=path, body=body,
                                    headers={"content-length": str(len(body))}))


@pytest.fixture
def session(fake_db):
    res = post("/api/register", {"username": "tester", "password": "hunter2hunter2"})
    return res.json()["token"]


def test_http_client_gets_nothing(fake_db):
    res = panel.app.handle(Request(method="GET", path="/", version="HTTP/1.1"))
    assert res.status_code == 505


def test_login_page_renders(fake_db):
    res = get("/")
    assert res.status_code == 200
    assert res.headers["Content-Type"].startswith("text/html")
    assert "Sign in" in res.text and "/panel.css" in res.text


def test_stylesheet_is_served(fake_db):
    res = get("/panel.css")
    assert res.status_code == 200
    assert res.headers["Content-Type"].startswith("text/css")


def test_register_returns_a_token(fake_db):
    res = post("/api/register", {"username": "tester", "password": "hunter2hunter2"})
    assert res.status_code == 200 and res.json()["token"]


def test_register_rejects_a_short_password(fake_db):
    res = post("/api/register", {"username": "tester", "password": "no"})
    assert res.status_code == 400 and "error" in res.json()


def test_login_with_the_wrong_password(session):
    res = post("/api/login", {"username": "tester", "password": "nope"})
    assert res.status_code == 401


def test_panel_needs_a_session(fake_db):
    res = get("/panel")
    assert res.status_code == 401
    assert "Sign in" in res.text


def test_panel_lists_domains(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/panel?t={session}")
    assert res.status_code == 200
    assert f"mysite.{ZONE}" in res.text
    assert f"0 of {config.MAX_DOMAINS} used" not in res.text


def test_domain_limit_through_the_api(session):
    for i in range(config.MAX_DOMAINS):
        assert post("/api/domain/add", {"token": session, "domain": f"s{i}"}).status_code == 200
    res = post("/api/domain/add", {"token": session, "domain": "toomany"})
    assert res.status_code == 403


def test_record_lifecycle_through_the_api(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    add = post("/api/record/add", {"token": session, "domain": "mysite",
                                   "name": "www", "type": "A", "value": "10.0.0.1"})
    assert add.status_code == 200
    rid = add.json()["id"]

    listed = post("/api/records", {"token": session, "domain": "mysite"})
    assert listed.json()["records"][0]["fqdn"] == f"www.mysite.{ZONE}"

    gone = post("/api/record/delete", {"token": session, "domain": "mysite", "id": rid})
    assert gone.status_code == 200
    assert post("/api/records", {"token": session, "domain": "mysite"}).json()["records"] == []


def test_record_page_shows_records(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/record/add", {"token": session, "domain": "mysite",
                             "name": "www", "type": "A", "value": "10.0.0.1"})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert "10.0.0.1" in res.text and "Add a record" in res.text


def test_someone_elses_domain_is_not_found(session, fake_db):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    other = post("/api/register", {"username": "mallory",
                                   "password": "hunter2hunter2"}).json()["token"]
    res = post("/api/record/add", {"token": other, "domain": "mysite",
                                   "name": "evil", "type": "A", "value": "10.0.0.1"})
    assert res.status_code == 404
    assert get(f"/domain/mysite.{ZONE}?t={other}").status_code == 404


def test_expired_or_bogus_token_on_the_api(fake_db):
    res = post("/api/domain/add", {"token": "made-up", "domain": "mysite"})
    assert res.status_code == 401


def test_logout_invalidates(session):
    post("/api/logout", {"token": session})
    assert get(f"/panel?t={session}").status_code == 401


def test_bad_json_body(fake_db):
    res = panel.app.handle(Request(method="POST", path="/api/login", body=b"{oops",
                                   headers={"content-length": "5"}))
    assert res.status_code == 400


def test_every_link_on_a_page_leads_somewhere(session):
    """Clicking anything the panel renders must not land on an error page."""
    import re
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/record/add", {"token": session, "domain": "mysite",
                             "name": "@", "type": "A", "value": "10.0.0.1"})
    for path in (f"/panel?t={session}", f"/domain/mysite.{ZONE}?t={session}"):
        hrefs = re.findall(r'href="([^"]*)"', get(path).text)
        assert hrefs
        for href in hrefs:
            assert get(href).status_code == 200, f"{href} (linked from {path})"


def test_cert_download_route(session, tmp_path, monkeypatch):
    from stardns import ca
    if not ca.ca_ready()[0]:
        pytest.skip("no StarWeb root CA in certs/")
    monkeypatch.setattr(config, "ISSUED", tmp_path)
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    assert post("/api/cert/issue", {"token": session, "domain": "mysite"}).status_code == 200

    res = get(f"/cert/mysite.{ZONE}/cert?t={session}")
    assert res.status_code == 200 and "BEGIN CERTIFICATE" in res.text
    res = get(f"/cert/mysite.{ZONE}/key?t={session}")
    assert res.status_code == 200 and "PRIVATE KEY" in res.text
    assert get(f"/cert/mysite.{ZONE}/cert").status_code == 401


def test_unknown_route(fake_db):
    assert get("/nothing-here").status_code == 404


def test_html_is_escaped(session, fake_db):
    # A value is echoed into the records table, so it must not carry markup out.
    zones.add_domain("tester", "mysite")
    fake_db.records.insert_one({"domain": f"mysite.{ZONE}", "name": "x",
                                "type": "TXT", "value": "<script>bad</script>",
                                "ttl": 300})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert "<script>bad</script>" not in res.text
    assert "&lt;script&gt;" in res.text
