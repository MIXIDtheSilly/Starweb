"""Range support, mirroring parse_byte_range in src/server/server.cpp."""
import pytest

import starweb
from starweb.cli import _parse_headers, main
from starweb.static import parse_byte_range

SIZE = 1000


def test_plain_range():
    assert parse_byte_range("bytes=0-99", SIZE) == (0, 99)
    assert parse_byte_range("bytes=100-199", SIZE) == (100, 199)


def test_open_ended_range():
    assert parse_byte_range("bytes=900-", SIZE) == (900, 999)


def test_suffix_range():
    # What a media player uses to read a trailing moov atom.
    assert parse_byte_range("bytes=-100", SIZE) == (900, 999)
    # A suffix larger than the file means the whole file, not a negative start.
    assert parse_byte_range("bytes=-5000", SIZE) == (0, 999)


def test_end_is_clamped_not_rejected():
    assert parse_byte_range("bytes=990-99999", SIZE) == (990, 999)


def test_unusable_ranges():
    assert parse_byte_range("bytes=1000-1001", SIZE) is None  # start past EOF
    assert parse_byte_range("bytes=500-400", SIZE) is None    # inverted
    assert parse_byte_range("bytes=-0", SIZE) is None         # empty suffix
    assert parse_byte_range("bytes=0-99", 0) is None          # empty file
    assert parse_byte_range("bytes=abc", SIZE) is None
    assert parse_byte_range("items=0-99", SIZE) is None
    assert parse_byte_range("bytes=0-9,20-29", SIZE) is None  # multi-range refused


def test_static_range_over_the_wire(moon):
    whole = starweb.get(f"{moon}/static/cat.jpg")
    assert whole.status_code == 200
    assert whole.headers["accept-ranges"] == "bytes"
    assert len(whole.body) == int(whole.headers["content-length"])

    part = starweb.get(f"{moon}/static/cat.jpg",
                       headers={"Range": "bytes=0-99"})
    assert part.status_code == 206
    assert part.headers["content-range"] == f"bytes 0-99/{len(whole.body)}"
    assert part.body == whole.body[:100]


def test_suffix_range_over_the_wire(moon):
    whole = starweb.get(f"{moon}/static/cat.jpg")
    tail = starweb.get(f"{moon}/static/cat.jpg",
                       headers={"Range": "bytes=-64"})
    assert tail.status_code == 206
    assert tail.body == whole.body[-64:]


def test_unsatisfiable_range_over_the_wire(moon):
    res = starweb.get(f"{moon}/static/cat.jpg",
                      headers={"Range": "bytes=99999999-"})
    assert res.status_code == 416
    assert res.headers["content-range"].startswith("bytes */")
    assert res.body == b""


def test_cli_header_parsing():
    assert _parse_headers(["Range: bytes=0-9"]) == {"Range": "bytes=0-9"}
    # A value containing a colon must survive.
    assert _parse_headers(["X: a:b"]) == {"X": "a:b"}
    assert _parse_headers(None) == {}
    with pytest.raises(ValueError):
        _parse_headers(["no colon"])
    with pytest.raises(ValueError):
        _parse_headers([": empty name"])


def test_cli_sends_range_header(moon, capsysbinary):
    rc = main(["get", f"{moon}/static/cat.jpg", "-H", "Range: bytes=0-15"])
    assert rc == 0
    assert capsysbinary.readouterr().out.startswith(b"\xff\xd8\xff")


def test_ranges_reassemble_the_whole_file(moon):
    """The property the browser's MediaSource actually depends on."""
    whole = starweb.get(f"{moon}/static/cat.jpg").body

    out = b""
    step = 4096
    while out != whole:
        end = min(len(out) + step, len(whole)) - 1
        res = starweb.get(f"{moon}/static/cat.jpg",
                          headers={"Range": f"bytes={len(out)}-{end}"})
        assert res.status_code == 206
        assert res.body
        out += res.body
    assert out == whole
