"""DNS message coding, on the wire as RFC 1035 defines it.

Only what an authoritative server for one TLD has to speak: A, AAAA, CNAME,
TXT, NS, SOA, plus enough EDNS(0) to keep dig happy.
"""
import ipaddress
import struct
from dataclasses import dataclass, field

A, NS, CNAME, SOA, PTR, MX, TXT, AAAA, OPT, ANY = 1, 2, 5, 6, 12, 15, 16, 28, 41, 255
IN = 1

TYPE_NAMES = {A: "A", NS: "NS", CNAME: "CNAME", SOA: "SOA", PTR: "PTR",
              MX: "MX", TXT: "TXT", AAAA: "AAAA", ANY: "ANY"}
TYPE_CODES = {v: k for k, v in TYPE_NAMES.items()}

NOERROR, FORMERR, SERVFAIL, NXDOMAIN, NOTIMP, REFUSED = 0, 1, 2, 3, 4, 5

MAX_UDP = 512


class FormatError(Exception):
    pass


@dataclass
class RR:
    name: str
    rtype: int
    ttl: int
    data: object  # str | tuple, shaped per rtype
    rclass: int = IN


@dataclass
class Query:
    id: int
    name: str
    qtype: int
    qclass: int
    rd: bool
    opcode: int
    udp_size: int = MAX_UDP
    has_opt: bool = False
    questions: list = field(default_factory=list)


def _encode_name(name: str, out: bytearray, offsets: dict[str, int]) -> None:
    name = name.rstrip(".").lower()
    while name:
        pointer = offsets.get(name)
        if pointer is not None:
            out += struct.pack("!H", 0xC000 | pointer)
            return
        if len(out) < 0x4000:
            offsets[name] = len(out)
        label, _, name = name.partition(".")
        raw = label.encode("idna" if any(ord(c) > 127 for c in label) else "ascii")
        if len(raw) > 63:
            raise FormatError(f"label too long: {label!r}")
        out.append(len(raw))
        out += raw
    out.append(0)


def _decode_name(data: bytes, pos: int) -> tuple[str, int]:
    labels: list[str] = []
    jumped = False
    end = pos
    hops = 0
    while True:
        if pos >= len(data):
            raise FormatError("name runs past the end of the message")
        length = data[pos]
        if length & 0xC0 == 0xC0:
            if pos + 1 >= len(data):
                raise FormatError("truncated compression pointer")
            target = struct.unpack("!H", data[pos:pos + 2])[0] & 0x3FFF
            if not jumped:
                end = pos + 2
                jumped = True
            hops += 1
            if hops > 32 or target >= pos:
                # Only backward pointers are legal; anything else is a loop.
                raise FormatError("bad compression pointer")
            pos = target
            continue
        pos += 1
        if length == 0:
            break
        if length > 63 or pos + length > len(data):
            raise FormatError("bad label")
        labels.append(data[pos:pos + length].decode("ascii", "replace").lower())
        pos += length
    return ".".join(labels), (end if jumped else pos)


def _encode_txt(value: str) -> bytes:
    raw = value.encode()
    out = bytearray()
    # A character-string caps at 255 bytes, so a longer value goes out as
    # several strings in one record — which is what resolvers concatenate.
    for i in range(0, max(len(raw), 1), 255):
        chunk = raw[i:i + 255]
        out.append(len(chunk))
        out += chunk
    return bytes(out)


def _encode_rdata(rr: RR, out: bytearray, offsets: dict[str, int]) -> None:
    start = len(out)
    out += b"\x00\x00"  # rdlength, backfilled once the body is known

    if rr.rtype == A:
        out += ipaddress.IPv4Address(rr.data).packed
    elif rr.rtype == AAAA:
        out += ipaddress.IPv6Address(rr.data).packed
    elif rr.rtype in (CNAME, NS, PTR):
        # Not compressed: some resolvers mishandle pointers into rdata.
        _encode_name(str(rr.data), out, {})
    elif rr.rtype == TXT:
        out += _encode_txt(str(rr.data))
    elif rr.rtype == SOA:
        mname, rname, serial, refresh, retry, expire, minimum = rr.data
        _encode_name(mname, out, {})
        _encode_name(rname, out, {})
        out += struct.pack("!IIIII", serial, refresh, retry, expire, minimum)
    else:
        raise FormatError(f"cannot encode type {rr.rtype}")

    struct.pack_into("!H", out, start, len(out) - start - 2)


def _encode_rr(rr: RR, out: bytearray, offsets: dict[str, int]) -> None:
    _encode_name(rr.name, out, offsets)
    out += struct.pack("!HHI", rr.rtype, rr.rclass, max(0, int(rr.ttl)))
    _encode_rdata(rr, out, offsets)


def parse_query(data: bytes) -> Query:
    if len(data) < 12:
        raise FormatError("message shorter than a header")
    ident, flags, qdcount, ancount, nscount, arcount = struct.unpack("!HHHHHH", data[:12])
    if flags & 0x8000:
        raise FormatError("that is a response, not a query")
    if qdcount < 1:
        raise FormatError("no question")

    pos = 12
    questions = []
    for _ in range(qdcount):
        name, pos = _decode_name(data, pos)
        if pos + 4 > len(data):
            raise FormatError("truncated question")
        qtype, qclass = struct.unpack("!HH", data[pos:pos + 4])
        pos += 4
        questions.append((name, qtype, qclass))

    q = Query(id=ident, name=questions[0][0], qtype=questions[0][1],
              qclass=questions[0][2], rd=bool(flags & 0x0100),
              opcode=(flags >> 11) & 0xF, questions=questions)

    # Skip answer/authority to reach additional, where OPT lives.
    try:
        for _ in range(ancount + nscount):
            pos = _skip_rr(data, pos)
        for _ in range(arcount):
            rname, after = _decode_name(data, pos)
            rtype, rclass, _ttl, rdlen = struct.unpack("!HHIH", data[after:after + 10])
            if rtype == OPT:
                q.has_opt = True
                q.udp_size = max(MAX_UDP, min(rclass, 4096))
            pos = after + 10 + rdlen
    except (FormatError, struct.error):
        pass  # a malformed tail costs us EDNS, not the answer

    return q


def _skip_rr(data: bytes, pos: int) -> int:
    _name, pos = _decode_name(data, pos)
    rdlen = struct.unpack("!H", data[pos + 8:pos + 10])[0]
    return pos + 10 + rdlen


def build_response(query: Query, answers: list[RR] | None = None,
                   authority: list[RR] | None = None,
                   additional: list[RR] | None = None,
                   rcode: int = NOERROR, aa: bool = True,
                   max_size: int | None = None) -> bytes:
    answers = answers or []
    authority = authority or []
    additional = additional or []

    flags = 0x8000 | ((query.opcode & 0xF) << 11) | (rcode & 0xF)
    if aa:
        flags |= 0x0400
    if query.rd:
        flags |= 0x0100

    out = bytearray(struct.pack("!HHHHHH", query.id, flags, len(query.questions),
                                len(answers), len(authority), len(additional)))
    offsets: dict[str, int] = {}
    for name, qtype, qclass in query.questions:
        _encode_name(name, out, offsets)
        out += struct.pack("!HH", qtype, qclass)

    for section in (answers, authority, additional):
        for rr in section:
            _encode_rr(rr, out, offsets)

    if query.has_opt:
        out += b"\x00" + struct.pack("!HHIH", OPT, query.udp_size, 0, 0)
        struct.pack_into("!H", out, 10, len(additional) + 1)

    limit = max_size if max_size is not None else len(out)
    if len(out) > limit:
        # Drop every section and set TC; the client retries over TCP.
        head = bytearray(struct.pack("!HHHHHH", query.id, flags | 0x0200,
                                     len(query.questions), 0, 0, 0))
        offsets = {}
        for name, qtype, qclass in query.questions:
            _encode_name(name, head, offsets)
            head += struct.pack("!HH", qtype, qclass)
        return bytes(head)

    return bytes(out)


def error_response(data: bytes, rcode: int) -> bytes:
    """A reply to a message we could not fully parse: header and rcode only."""
    if len(data) < 4:
        return b""
    ident = struct.unpack("!H", data[:2])[0]
    flags = 0x8000 | (rcode & 0xF)
    return struct.pack("!HHHHHH", ident, flags, 0, 0, 0, 0)
