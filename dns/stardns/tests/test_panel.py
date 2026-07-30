import json

import pytest
from starweb import Request

from stardns import config, panel, ui, zones

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
    assert "Log In" in res.text and "/panel.css" in res.text


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


def test_access_creates_an_unknown_account(fake_db):
    res = post("/api/access", {"username": "newbie", "password": "hunter2hunter2"})
    assert res.status_code == 200 and res.json()["token"]
    # A second call is now a plain login, not a re-registration.
    again = post("/api/access", {"username": "newbie", "password": "hunter2hunter2"})
    assert again.status_code == 200 and again.json()["token"]


def test_access_rejects_wrong_password_for_existing_account(session):
    res = post("/api/access", {"username": "tester", "password": "nope"})
    assert res.status_code == 401 and "error" in res.json()


def test_panel_needs_a_session(fake_db):
    res = get("/panel")
    assert res.status_code == 401
    assert "Sign in" in res.text


def test_home_lists_domains(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/panel?t={session}")
    assert res.status_code == 200
    assert f"mysite.{ZONE}" in res.text
    assert "What are we doing today?" in res.text


def test_domains_tab_lists_domains(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domains?t={session}")
    assert res.status_code == 200
    assert f"mysite.{ZONE}" in res.text
    assert f"0 of {config.MAX_DOMAINS} used" not in res.text


def test_tabs_need_a_session(fake_db):
    for path in ("/domains", "/analytics"):
        assert get(path).status_code == 401


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
    """Clicking anything the panel renders must not land on an error page.

    Rows and sidebar tabs are clickable boxes rather than anchors, so their
    destinations are Lua string literals passed to location.assign, and both
    forms are collected here."""
    import re
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/record/add", {"token": session, "domain": "mysite",
                             "name": "@", "type": "A", "value": "10.0.0.1"})
    for path in (f"/panel?t={session}", f"/domains?t={session}",
                 f"/analytics?t={session}", f"/domain/mysite.{ZONE}?t={session}"):
        text = get(path).text
        targets = (re.findall(r'href="([^"]*)"', text)
                   + re.findall(r'link\("[^"]*", "([^"]*)"\)', text))
        assert targets
        for target in targets:
            assert get(target).status_code == 200, f"{target} (linked from {path})"


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


def test_home_shows_query_total(session):
    from stardns import analytics
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    analytics.record_query(f"mysite.{ZONE}")
    analytics.record_query(f"mysite.{ZONE}")
    res = get(f"/panel?t={session}")
    assert 'id="chart-home"' in res.text
    assert "Queries across your domains" in res.text
    assert ">2<" in res.text


def test_home_recent_column_starts_empty(session):
    res = get(f"/panel?t={session}")
    assert "Places you visit will show up here." in res.text


def test_home_recent_column_tracks_visited_pages(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    get(f"/domain/mysite.{ZONE}?t={session}")
    get(f"/analytics/mysite.{ZONE}?t={session}")

    res = get(f"/panel?t={session}")
    assert f"mysite.{ZONE}" in res.text
    assert f'link("rrow-1", "/analytics/mysite.{ZONE}?t={session}")' in res.text
    assert f'link("rrow-2", "/domain/mysite.{ZONE}?t={session}")' in res.text
    # Slice past the Domains column first, since it also uses globe.
    recent_col = res.text[res.text.index('id="head-rec"'):]
    chart_pie_src = f'src="{ui.icon_src("chart-pie", "#8b8b96")}"'
    globe_src = f'src="{ui.icon_src("globe", "#8b8b96")}"'
    assert chart_pie_src in recent_col
    assert globe_src in recent_col
    assert recent_col.index(chart_pie_src) < recent_col.index(globe_src)


def test_home_recent_column_caps_at_three_and_dedupes_repeat_visits(session):
    for name in ("one", "two", "three"):
        post("/api/domain/add", {"token": session, "domain": name})
    # Repeat visit to /domain/one should jump back to the front, not duplicate.
    for path in ("/domain/one", "/domain/two", "/domain/three",
                 "/analytics/one", "/domain/one"):
        get(f"{path}.{ZONE}?t={session}")

    res = get(f"/panel?t={session}")
    assert res.text.count('class="lrow" id="rrow-') == 3
    assert f'link("rrow-1", "/domain/one.{ZONE}?t={session}")' in res.text
    assert f'link("rrow-2", "/analytics/one.{ZONE}?t={session}")' in res.text
    assert f'link("rrow-3", "/domain/three.{ZONE}?t={session}")' in res.text
    assert 'link("rrow-4"' not in res.text


def test_domains_tab_shows_a_queries_tile(session):
    from stardns import analytics
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    analytics.record_query(f"mysite.{ZONE}")
    res = get(f"/domains?t={session}")
    assert "queries (14d)" in res.text


def test_domains_tab_has_no_separate_certificates_tile(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domains?t={session}")
    assert '<p class="tlab">certificate</p>' not in res.text
    assert '<p class="tlab">certificates</p>' not in res.text


def test_domains_tab_shows_certificate_status_as_icons_not_text(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domains?t={session}")
    assert "Certificate issued" not in res.text and "No certificate" not in res.text
    assert "shield-question-mark" in res.text


def test_domains_and_analytics_tabs_carry_the_shell_artwork(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    for path in (f"/domains?t={session}", f"/analytics?t={session}"):
        text = get(path).text
        assert 'id="art-bl"' in text and 'id="art-tr"' in text


def test_domains_tab_has_no_giant_page_title(session):
    res = get(f"/domains?t={session}")
    assert "<h1>" not in res.text


def test_analytics_tab_with_no_domains_is_still_an_empty_state(session):
    res = get(f"/analytics?t={session}")
    assert "No domains yet" in res.text


def test_analytics_tab_shows_real_per_domain_breakdown(session):
    from stardns import analytics
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/domain/add", {"token": session, "domain": "other"})
    analytics.record_query(f"mysite.{ZONE}")
    analytics.record_query(f"mysite.{ZONE}")
    analytics.record_query(f"other.{ZONE}")
    res = get(f"/analytics?t={session}")
    assert res.status_code == 200
    assert f"mysite.{ZONE}" in res.text and f"other.{ZONE}" in res.text
    assert f'<p class="tnum">mysite.{ZONE}</p>' in res.text
    assert '<p class="tlab">busiest domain</p>' in res.text


def test_analytics_rows_link_to_the_per_domain_page(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/analytics?t={session}")
    assert f"/analytics/mysite.{ZONE}?t={session}" in res.text


def test_domain_analytics_page_charts_that_domains_series(session):
    from stardns import analytics
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/domain/add", {"token": session, "domain": "other"})
    for _ in range(3):
        analytics.record_query(f"mysite.{ZONE}")
    analytics.record_query(f"other.{ZONE}")

    res = get(f"/analytics/mysite.{ZONE}?t={session}")
    assert res.status_code == 200
    assert f"Last 14 days for mysite.{ZONE}" in res.text
    assert 'id="chart-dom"' in res.text and 'id="chart-days"' in res.text
    # The 14-day series ends on today's three queries, and the totals read off
    # this domain alone, never the account-wide figure of four.
    assert ",3}" in res.text
    assert '<p class="tnum">3</p>' in res.text


def test_domain_analytics_page_with_no_queries_yet(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/analytics/mysite.{ZONE}?t={session}")
    assert res.status_code == 200
    assert "Nothing yet" in res.text


def test_bar_chart_survives_a_browser_without_canvas_hover(session):
    """cv.hoverX is newer than some builds out there. Reading it bare threw
    inside the rAF callback, which then never re-registered, so the bars never
    drew at all; the chart has to fall back rather than die."""
    from stardns import ui
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/analytics/mysite.{ZONE}?t={session}")
    assert "cv.hoverX or -1" in res.text
    assert "cv.hoverX >=" not in ui.CHARTS


def test_hover_readout_falls_back_without_measure_text(session):
    """Same lesson as cv.hoverX: a method an older build lacks reads as nil, and
    calling it throws inside the rAF callback, which takes the whole chart with
    it. The corner radius degrades on its own, since an extra argument to
    fillRect is simply ignored, but measureText has to be guarded."""
    from stardns import ui
    body = ui.CHARTS[ui.CHARTS.index("local function textWidth"):]
    body = body[:body.index("\nend\n")]
    assert "if ctx.measureText then" in body
    assert "return #text * 8" in body


def test_analytics_range_switches_the_window(session):
    from stardns import analytics
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    analytics.record_query(f"mysite.{ZONE}")

    res = get(f"/analytics/mysite.{ZONE}?t={session}&r=24h")
    assert res.status_code == 200
    assert "<h3>BY HOUR</h3>" in res.text
    assert "Last 24 hours for" in res.text
    # Sub-day windows end on a bucket still filling, not on a whole day.
    assert ">Now</p>" in res.text
    assert ">Today</p>" not in res.text


def test_analytics_range_pills_carry_the_token_and_key(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/analytics?t={session}&r=90d")
    assert f'"/analytics?t={session}&r=1h"' in res.text
    assert f'"/analytics?t={session}&r=365d"' in res.text
    # The selected pill is the only filled one.
    assert res.text.count('class="rgon"') == 1
    assert "<h3>ANALYTICS</h3>" in res.text and "Last 3 months" in res.text


def test_analytics_range_falls_back_when_the_key_is_junk(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/analytics/mysite.{ZONE}?t={session}&r=../etc")
    assert res.status_code == 200
    assert "Last 14 days for" in res.text


def test_domain_analytics_drops_what_the_domain_page_already_shows(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/record/add", {"token": session, "domain": "mysite",
                             "name": "www", "type": "A", "value": "10.0.0.1"})
    res = get(f"/analytics/mysite.{ZONE}?t={session}")
    assert "Certificate" not in res.text
    assert f"1 of {config.MAX_RECORDS}" not in res.text


def test_domain_analytics_page_rejects_someone_elses_domain(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    other = post("/api/register", {"username": "mallory",
                                   "password": "hunter2hunter2"}).json()["token"]
    res = get(f"/analytics/mysite.{ZONE}?t={other}")
    assert res.status_code == 404


def test_domain_page_has_no_analytics_on_it(session):
    from stardns import analytics
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    analytics.record_query(f"mysite.{ZONE}")
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert 'id="chart-domain"' not in res.text
    assert "queries (14d)" not in res.text


def test_domain_page_records_tile_is_used_of_max(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/record/add", {"token": session, "domain": "mysite",
                             "name": "www", "type": "A", "value": "10.0.0.1"})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert f"1/{config.MAX_RECORDS}" in res.text
    assert '<p class="tlab">slots left</p>' not in res.text


def test_domain_page_back_link_is_an_icon_row_not_literal_arrow(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert "&lt;" not in res.text
    assert "All domains" in res.text
    assert f'src="{ui.icon_src("chevron-left", "#8b8b96")}"' in res.text


def test_domain_page_certificate_card_has_no_written_to_line(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert "Also written to" not in res.text
    assert "shield-question-mark" in res.text


def test_domain_page_add_record_card_has_purple_outline(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert 'class="addcard"' in res.text


def test_html_is_escaped(session, fake_db):
    # A value is echoed into the records table, so it must not carry markup out.
    zones.add_domain("tester", "mysite")
    fake_db.records.insert_one({"domain": f"mysite.{ZONE}", "name": "x",
                                "type": "TXT", "value": "<script>bad</script>",
                                "ttl": 300})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert "<script>bad</script>" not in res.text
    assert "&lt;script&gt;" in res.text


def test_search_finds_a_matching_domain(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    post("/api/domain/add", {"token": session, "domain": "other"})
    res = get(f"/search?t={session}&q=mysi")
    assert res.status_code == 200
    assert f"/domain/mysite.{ZONE}?t={session}" in res.text
    assert f"/analytics/mysite.{ZONE}?t={session}" in res.text
    # The sidebar's live dropdown index carries every domain regardless of
    # query, so check the rendered result row, not the whole page.
    assert f'<p class="lname">other.{ZONE}</p>' not in res.text


def test_search_finds_a_matching_page(session):
    res = get(f"/search?t={session}&q=analytic")
    assert res.status_code == 200
    assert f"/analytics?t={session}" in res.text


def test_search_with_no_matches_says_so(session):
    res = get(f"/search?t={session}&q=nothingmatchesthis")
    assert "No results" in res.text


def test_search_with_no_query_prompts_instead_of_matching_everything(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/search?t={session}")
    assert f'<p class="lname">mysite.{ZONE}</p>' not in res.text
    assert "press the search icon" in res.text


def test_search_needs_a_session(fake_db):
    assert get("/search?q=x").status_code == 401


def test_search_result_links_lead_somewhere(session):
    import re
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    text = get(f"/search?t={session}&q=my").text
    targets = re.findall(r'link\("[^"]*", "([^"]*)"\)', text)
    assert targets
    for target in targets:
        assert get(target).status_code == 200, target


def test_quick_search_dropdown_index_covers_pages_and_domains(session):
    # The sidebar's qSearch index must include every domain and its
    # analytics link, not just the static pages.
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/panel?t={session}")
    assert 'id="qdrop"' in res.text
    assert "local function qSearch(q)" in res.text
    assert f'"mysite.{ZONE}", "globe", "/domain/mysite.{ZONE}?t={session}"' in res.text
    assert (f'"Analytics for mysite.{ZONE}", "chart-pie", '
            f'"/analytics/mysite.{ZONE}?t={session}"') in res.text
    assert f'"Domains", "globe", "/domains?t={session}"' in res.text


def test_hero_box_does_not_touch_the_sidebar_widget(session):
    # The hero search box must stay independent of the sidebar's qSearch table.
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/panel?t={session}").text
    assert "local function qSearch(q)" in res
    assert 'id="qdrop"' in res
    assert "qSearch(document.getElementById(\"sfld\").value)" not in res
    assert 'id="sdrop"' not in res


def test_quick_search_dropdown_present_on_the_domain_and_domain_analytics_pages(session):
    post("/api/domain/add", {"token": session, "domain": "mysite"})
    res = get(f"/domain/mysite.{ZONE}?t={session}")
    assert res.status_code == 200 and 'id="qdrop"' in res.text
    res = get(f"/analytics/mysite.{ZONE}?t={session}")
    assert res.status_code == 200 and 'id="qdrop"' in res.text
