import struct

import pytest

from stardns import config, resolver, wire, zones
from stardns.tests.test_wire import query_bytes


def ask(name: str, qtype: int = wire.A, edns: bool = False) -> dict:
    q = wire.parse_query(query_bytes(name, qtype, edns=edns))
    return decode(resolver.answer(q))


def decode(data: bytes) -> dict:
    _id, flags, qd, an, ns, ar = struct.unpack("!HHHHHH", data[:12])
    pos = 12
    for _ in range(qd):
        _n, pos = wire._decode_name(data, pos)
        pos += 4

    sections = {"answers": [], "authority": [], "additional": []}
    for key, count in (("answers", an), ("authority", ns), ("additional", ar)):
        for _ in range(count):
            name, pos = wire._decode_name(data, pos)
            rtype, _cls, ttl, rdlen = struct.unpack("!HHIH", data[pos:pos + 10])
            pos += 10
            raw = data[pos:pos + rdlen]
            pos += rdlen
            sections[key].append({"name": name, "type": rtype, "ttl": ttl,
                                  "rdata": raw, "value": _value(rtype, raw, data)})
    sections["rcode"] = flags & 0xF
    sections["aa"] = bool(flags & 0x0400)
    return sections


def _value(rtype, raw, whole):
    import ipaddress
    if rtype == wire.A:
        return str(ipaddress.IPv4Address(raw))
    if rtype == wire.AAAA:
        return str(ipaddress.IPv6Address(raw))
    if rtype in (wire.CNAME, wire.NS):
        return wire._decode_name(raw, 0)[0]
    if rtype == wire.TXT:
        out, i = b"", 0
        while i < len(raw):
            out += raw[i + 1:i + 1 + raw[i]]
            i += 1 + raw[i]
        return out.decode()
    return raw


ZONE = config.ZONE


@pytest.fixture
def zone(account):
    domain = zones.add_domain("tester", "example")["name"]
    zones.add_record("tester", domain, "@", "A", "10.0.0.1")
    zones.add_record("tester", domain, "www", "A", "10.0.0.2")
    zones.add_record("tester", domain, "www", "A", "10.0.0.3")
    zones.add_record("tester", domain, "v6", "AAAA", "2001:db8::1")
    zones.add_record("tester", domain, "blog", "CNAME", "www")
    zones.add_record("tester", domain, "@", "TXT", "hello from starweb")
    zones.add_record("tester", domain, "*.dev", "A", "10.0.0.9")
    return domain


def test_apex_a_record(zone):
    res = ask(zone)
    assert res["rcode"] == wire.NOERROR and res["aa"]
    assert [a["value"] for a in res["answers"]] == ["10.0.0.1"]


def test_subdomain_returns_every_address(zone):
    res = ask(f"www.{zone}")
    assert sorted(a["value"] for a in res["answers"]) == ["10.0.0.2", "10.0.0.3"]


def test_aaaa(zone):
    res = ask(f"v6.{zone}", wire.AAAA)
    assert [a["value"] for a in res["answers"]] == ["2001:db8::1"]


def test_txt(zone):
    res = ask(zone, wire.TXT)
    assert [a["value"] for a in res["answers"]] == ["hello from starweb"]


def test_cname_is_chased_inside_the_zone(zone):
    res = ask(f"blog.{zone}")
    kinds = [a["type"] for a in res["answers"]]
    assert kinds[0] == wire.CNAME
    assert res["answers"][0]["value"] == f"www.{zone}"
    assert sorted(a["value"] for a in res["answers"][1:]) == ["10.0.0.2", "10.0.0.3"]


def test_cname_query_returns_the_cname_itself(zone):
    res = ask(f"blog.{zone}", wire.CNAME)
    assert len(res["answers"]) == 1
    assert res["answers"][0]["type"] == wire.CNAME


def test_cname_out_of_zone_is_not_chased(account):
    domain = zones.add_domain("tester", "example")["name"]
    zones.add_record("tester", domain, "away", "CNAME", f"other.{ZONE}")
    res = ask(f"away.{domain}")
    assert len(res["answers"]) == 1
    assert res["answers"][0]["value"] == f"other.{ZONE}"


def test_wildcard_matches_any_label(zone):
    for host in ("a", "b-2"):
        res = ask(f"{host}.dev.{zone}")
        assert [a["value"] for a in res["answers"]] == ["10.0.0.9"]
        assert res["answers"][0]["name"] == f"{host}.dev.{zone}"


def test_unknown_name_is_nxdomain(zone):
    res = ask(f"nope.{zone}")
    assert res["rcode"] == wire.NXDOMAIN
    assert res["answers"] == []
    assert res["authority"][0]["type"] == wire.SOA


def test_known_name_wrong_type_is_nodata(zone):
    res = ask(f"www.{zone}", wire.AAAA)
    assert res["rcode"] == wire.NOERROR
    assert res["answers"] == []
    assert res["authority"][0]["type"] == wire.SOA


def test_empty_non_terminal_is_nodata(zone):
    # dev.example.star holds no records, but *.dev.example.star lives under it.
    res = ask(f"dev.{zone}")
    assert res["rcode"] == wire.NOERROR and res["answers"] == []


def test_unregistered_domain_is_nxdomain(account):
    res = ask(f"nobody.{ZONE}")
    assert res["rcode"] == wire.NXDOMAIN


def test_names_outside_the_zone_are_refused(zone):
    for name in ("www.google.com", "localhost", "example.moon"):
        res = ask(name)
        assert res["rcode"] == wire.REFUSED
        assert not res["aa"]


def test_zone_apex_serves_ns_and_soa(zone):
    ns = ask(ZONE, wire.NS)
    assert [a["value"] for a in ns["answers"]] == [config.NS_NAME]
    assert ns["additional"][0]["value"] == config.NS_ADDR
    soa = ask(ZONE, wire.SOA)
    assert soa["answers"][0]["type"] == wire.SOA


def test_domain_apex_serves_its_own_soa(zone):
    res = ask(zone, wire.SOA)
    assert res["answers"][0]["type"] == wire.SOA
    assert res["answers"][0]["name"] == zone


def test_any_returns_everything_at_the_name(zone):
    res = ask(f"www.{zone}", wire.ANY)
    assert len(res["answers"]) == 2


def test_ttl_is_what_was_configured(zone):
    zones.add_record("tester", zone, "slow", "A", "10.0.0.7", 3600)
    res = ask(f"slow.{zone}")
    assert res["answers"][0]["ttl"] == 3600


def test_records_die_with_their_domain(zone):
    zones.delete_domain("tester", zone)
    assert ask(f"www.{zone}")["rcode"] == wire.NXDOMAIN


def test_big_txt_answer_truncates_over_udp(account):
    domain = zones.add_domain("tester", "example")["name"]
    for i in range(12):
        zones.add_record("tester", domain, "@", "TXT", f"{i}" + "z" * 240)
    q = wire.parse_query(query_bytes(domain, wire.TXT))
    out = resolver.answer(q)
    assert out[2] & 0x02 and len(out) <= 512

    q = wire.parse_query(query_bytes(domain, wire.TXT, edns=True))
    out = resolver.answer(q)
    assert not out[2] & 0x02 and len(out) > 512
