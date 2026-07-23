import struct

import pytest

from stardns import wire
from stardns.wire import RR


def query_bytes(name: str, qtype: int, ident: int = 0x1234, rd: bool = True,
                edns: bool = False) -> bytes:
    out = bytearray(struct.pack("!HHHHHH", ident, 0x0100 if rd else 0,
                                1, 0, 0, 1 if edns else 0))
    for label in name.split("."):
        out.append(len(label))
        out += label.encode()
    out.append(0)
    out += struct.pack("!HH", qtype, wire.IN)
    if edns:
        out += b"\x00" + struct.pack("!HHIH", wire.OPT, 4096, 0, 0)
    return bytes(out)


def parse_answers(data: bytes) -> list[tuple[str, int, bytes]]:
    _id, _flags, qd, an, _ns, _ar = struct.unpack("!HHHHHH", data[:12])
    pos = 12
    for _ in range(qd):
        _n, pos = wire._decode_name(data, pos)
        pos += 4
    out = []
    for _ in range(an):
        name, pos = wire._decode_name(data, pos)
        rtype, _cls, _ttl, rdlen = struct.unpack("!HHIH", data[pos:pos + 10])
        pos += 10
        out.append((name, rtype, data[pos:pos + rdlen]))
        pos += rdlen
    return out


def test_parse_query_reads_question():
    q = wire.parse_query(query_bytes("www.example.star", wire.A))
    assert (q.name, q.qtype, q.rd, q.id) == ("www.example.star", wire.A, True, 0x1234)
    assert q.udp_size == wire.MAX_UDP


def test_parse_query_reads_edns_size():
    q = wire.parse_query(query_bytes("example.star", wire.A, edns=True))
    assert q.has_opt and q.udp_size == 4096


def test_parse_query_rejects_a_response():
    data = bytearray(query_bytes("example.star", wire.A))
    data[2] |= 0x80
    with pytest.raises(wire.FormatError):
        wire.parse_query(bytes(data))


def test_a_record_roundtrip():
    q = wire.parse_query(query_bytes("example.star", wire.A))
    out = wire.build_response(q, [RR("example.star", wire.A, 300, "127.0.0.1")])
    assert struct.unpack("!H", out[:2])[0] == 0x1234
    assert out[2] & 0x84 == 0x84  # QR and AA
    name, rtype, rdata = parse_answers(out)[0]
    assert (name, rtype, rdata) == ("example.star", wire.A, b"\x7f\x00\x00\x01")


def test_aaaa_record_roundtrip():
    q = wire.parse_query(query_bytes("example.star", wire.AAAA))
    out = wire.build_response(q, [RR("example.star", wire.AAAA, 300, "::1")])
    _name, rtype, rdata = parse_answers(out)[0]
    assert rtype == wire.AAAA
    assert rdata == b"\x00" * 15 + b"\x01"


def test_cname_rdata_is_a_name():
    q = wire.parse_query(query_bytes("www.example.star", wire.CNAME))
    out = wire.build_response(q, [RR("www.example.star", wire.CNAME, 300,
                                     "example.star")])
    _name, rtype, rdata = parse_answers(out)[0]
    assert rtype == wire.CNAME
    assert rdata == b"\x07example\x04star\x00"


def test_txt_splits_into_255_byte_strings():
    value = "x" * 300
    q = wire.parse_query(query_bytes("example.star", wire.TXT))
    out = wire.build_response(q, [RR("example.star", wire.TXT, 300, value)])
    _name, rtype, rdata = parse_answers(out)[0]
    assert rtype == wire.TXT
    assert rdata[0] == 255 and rdata[256] == 45
    assert rdata[1:256] + rdata[257:] == value.encode()


def test_long_response_sets_tc_over_udp():
    q = wire.parse_query(query_bytes("example.star", wire.TXT))
    big = [RR("example.star", wire.TXT, 300, "y" * 250) for _ in range(20)]
    out = wire.build_response(q, big, max_size=512)
    assert out[2] & 0x02  # TC
    assert struct.unpack("!H", out[6:8])[0] == 0  # no answers carried


def test_compression_pointer_loop_is_rejected():
    data = bytearray(struct.pack("!HHHHHH", 1, 0x0100, 1, 0, 0, 0))
    data += struct.pack("!H", 0xC00C)  # points at itself
    data += struct.pack("!HH", wire.A, wire.IN)
    with pytest.raises(wire.FormatError):
        wire.parse_query(bytes(data))


def test_error_response_keeps_the_id():
    out = wire.error_response(query_bytes("example.star", wire.A), wire.FORMERR)
    assert struct.unpack("!H", out[:2])[0] == 0x1234
    assert out[3] & 0xF == wire.FORMERR
