import pytest

from stardns import config, zones
from stardns.errors import PanelError


@pytest.fixture
def domain(account):
    return zones.add_domain("tester", "example")["name"]


def test_domain_normalizes_with_or_without_the_tld(account):
    assert zones.normalize_domain("Mysite") == f"mysite.{config.ZONE}"
    assert zones.normalize_domain(f"mysite.{config.ZONE}.") == f"mysite.{config.ZONE}"


def test_subdomains_are_not_registerable(account):
    with pytest.raises(PanelError):
        zones.normalize_domain(f"a.b.{config.ZONE}")


def test_reserved_names_are_refused(account):
    with pytest.raises(PanelError):
        zones.add_domain("tester", "registry")


@pytest.mark.parametrize("bad", ["-lead", "trail-", "sp ace", "under_score", ""])
def test_bad_domain_labels(account, bad):
    with pytest.raises(PanelError):
        zones.normalize_domain(bad)


def test_add_a_record(domain):
    rec = zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    assert (rec["name"], rec["type"], rec["value"]) == ("www", "A", "10.0.0.5")
    assert rec["ttl"] == config.DEFAULT_TTL


def test_apex_name_forms_are_equivalent(domain):
    zones.add_record("tester", domain, "@", "A", "10.0.0.5")
    rec = zones.list_records(domain)[0]
    assert rec["name"] == "@"
    assert zones.normalize_record_name(domain, domain) == "@"
    assert zones.normalize_record_name("", domain) == "@"


def test_record_name_is_taken_relative_to_the_domain(domain):
    rec = zones.add_record("tester", domain, f"www.{domain}", "A", "10.0.0.5")
    assert rec["name"] == "www"


@pytest.mark.parametrize("rtype,value", [
    ("A", "not-an-ip"), ("A", "::1"), ("AAAA", "10.0.0.1"), ("AAAA", "zz::"),
    ("CNAME", "not a host"), ("TXT", ""),
])
def test_bad_values_are_rejected(domain, rtype, value):
    with pytest.raises(PanelError):
        zones.add_record("tester", domain, "x", rtype, value)


def test_unknown_type_is_rejected(domain):
    with pytest.raises(PanelError):
        zones.add_record("tester", domain, "x", "MX", "mail.example.star")


def test_ipv6_is_normalized(domain):
    rec = zones.add_record("tester", domain, "v6", "AAAA", "2001:0db8:0000::1")
    assert rec["value"] == "2001:db8::1"


def test_cname_target_at_is_the_domain(domain):
    rec = zones.add_record("tester", domain, "www", "CNAME", "@")
    assert rec["value"] == domain


def test_cname_cannot_sit_at_the_apex(domain):
    with pytest.raises(PanelError):
        zones.add_record("tester", domain, "@", "CNAME", "other.star")


def test_cname_cannot_share_a_name(domain):
    zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    with pytest.raises(PanelError):
        zones.add_record("tester", domain, "www", "CNAME", "example.star")


def test_nothing_can_share_a_cname(domain):
    zones.add_record("tester", domain, "blog", "CNAME", "example.star")
    with pytest.raises(PanelError):
        zones.add_record("tester", domain, "blog", "A", "10.0.0.5")


def test_duplicate_record_is_refused(domain):
    zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    with pytest.raises(PanelError) as e:
        zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    assert e.value.status == 409


def test_two_addresses_on_one_name_are_fine(domain):
    zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    zones.add_record("tester", domain, "www", "A", "10.0.0.6")
    assert len(zones.list_records(domain)) == 2


@pytest.mark.parametrize("ttl", [1, 59, 86401, "soon"])
def test_ttl_bounds(domain, ttl):
    with pytest.raises(PanelError):
        zones.add_record("tester", domain, "x", "A", "10.0.0.5", ttl)


def test_record_limit(domain):
    for i in range(config.MAX_RECORDS):
        zones.add_record("tester", domain, f"h{i}", "A", "10.0.0.5")
    with pytest.raises(PanelError) as e:
        zones.add_record("tester", domain, "spill", "A", "10.0.0.5")
    assert e.value.status == 403


def test_delete_record(domain):
    rec = zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    zones.delete_record("tester", domain, str(rec["_id"]))
    assert zones.list_records(domain) == []


def test_delete_record_needs_a_valid_id(domain):
    with pytest.raises(PanelError) as e:
        zones.delete_record("tester", domain, "not-an-objectid")
    assert e.value.status == 404


def test_deleting_a_domain_takes_its_records(domain):
    zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    zones.delete_domain("tester", domain)
    assert zones.list_records(domain) == []


def test_serial_moves_when_records_change(domain, fake_db, monkeypatch):
    before = fake_db.domains.find_one({"name": domain})["serial"]
    monkeypatch.setattr(zones, "_serial", lambda: before + 10)
    zones.add_record("tester", domain, "www", "A", "10.0.0.5")
    assert fake_db.domains.find_one({"name": domain})["serial"] > before


def test_wildcard_names_only_on_the_left(domain):
    assert zones.normalize_record_name("*.dev", domain) == "*.dev"
    with pytest.raises(PanelError):
        zones.normalize_record_name("dev.*", domain)
