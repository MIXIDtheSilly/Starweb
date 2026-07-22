from dataclasses import dataclass, field
from pathlib import Path

VERSION = "STWP/1.0"

_TRIM = " \t\r\n"


def _split_headers(data: bytes) -> tuple[int, int] | None:
    end = data.find(b"\r\n\r\n")
    if end == -1:
        end = data.find(b"\n\n")
        if end == -1:
            return None
    return end, 4 if data[end:end + 1] == b"\r" else 2


def _parse_header_lines(raw: str) -> dict[str, str]:
    headers: dict[str, str] = {}
    for line in raw.split("\n")[1:]:
        if not line.strip(_TRIM):
            continue
        colon = line.find(":")
        if colon == -1:
            continue
        name = line[:colon].strip(_TRIM).lower()
        if name:
            headers[name] = line[colon + 1:].strip(_TRIM)
    return headers


def _content_length(headers: dict[str, str]) -> int:
    try:
        return int(headers.get("content-length", "0"))
    except ValueError:
        return 0


def _serialize(start_line: str, headers: dict[str, str], body: bytes) -> bytes:
    out = [start_line.encode("latin-1"), b"\r\n"]
    for name, value in headers.items():
        out.append(f"{name}: {value}".encode("latin-1"))
        out.append(b"\r\n")
    out.append(b"\r\n")
    out.append(body)
    return b"".join(out)


def _decode(body: bytes, headers: dict[str, str]) -> str:
    ctype = headers.get("content-type", "")
    charset = "utf-8"
    if "charset=" in ctype:
        charset = ctype.split("charset=", 1)[1].split(";")[0].strip()
    try:
        return body.decode(charset)
    except (LookupError, UnicodeDecodeError):
        return body.decode("utf-8", "replace")


@dataclass
class Request:
    method: str = "GET"
    path: str = "/"
    version: str = VERSION
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""

    def serialize(self) -> bytes:
        return _serialize(f"{self.method} {self.path} {self.version}",
                          self.headers, self.body)

    @property
    def text(self) -> str:
        return _decode(self.body, self.headers)

    def json(self):
        import json
        return json.loads(self.body)

    @property
    def query(self) -> dict[str, str]:
        from urllib.parse import parse_qsl
        q = self.path.find("?")
        if q == -1:
            return {}
        return dict(parse_qsl(self.path[q + 1:].split("#")[0]))


@dataclass
class FileBody:
    """A body the server sends straight from disk instead of holding in memory.

    A 350 MB video read into `body` costs about three times the file by the time
    serialize() has made its own copy, which is enough to kill the process."""
    path: Path
    offset: int = 0
    length: int = 0

    def chunks(self, size: int = 64 * 1024):
        with open(self.path, "rb") as fh:
            fh.seek(self.offset)
            remaining = self.length
            while remaining > 0:
                data = fh.read(min(size, remaining))
                if not data:
                    return
                remaining -= len(data)
                yield data


@dataclass
class Response:
    status_code: int = 200
    status_text: str = "OK"
    version: str = VERSION
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""
    # Set instead of body to stream from disk; serialize() then emits only the head.
    file: FileBody | None = field(default=None, compare=False, repr=False)
    tls: object | None = field(default=None, compare=False, repr=False)

    def serialize(self) -> bytes:
        return _serialize(f"{self.version} {self.status_code} {self.status_text}",
                          self.headers, self.body)

    @property
    def content_length(self) -> int:
        return self.file.length if self.file is not None else len(self.body)

    @property
    def ok(self) -> bool:
        return 200 <= self.status_code < 300

    @property
    def text(self) -> str:
        return _decode(self.body, self.headers)

    def json(self):
        import json
        return json.loads(self.body)


def parse_request(data: bytes) -> tuple[Request, int] | None:
    """None means the message is incomplete; feed more bytes and retry."""
    split = _split_headers(data)
    if split is None:
        return None
    end, sep_len = split

    raw = data[:end].decode("latin-1")
    line = raw.split("\n", 1)[0].strip(_TRIM)
    parts = line.split(" ")
    if len(parts) < 3:
        return None

    headers = _parse_header_lines(raw)
    total = end + sep_len + _content_length(headers)
    if len(data) < total:
        return None

    req = Request(method=parts[0], path=parts[1], version=" ".join(parts[2:]),
                  headers=headers, body=data[end + sep_len:total])
    return req, total


def parse_response(data: bytes) -> tuple[Response, int] | None:
    split = _split_headers(data)
    if split is None:
        return None
    end, sep_len = split

    raw = data[:end].decode("latin-1")
    line = raw.split("\n", 1)[0].strip(_TRIM)
    parts = line.split(" ", 2)
    if len(parts) < 2:
        return None
    try:
        status = int(parts[1].strip(_TRIM))
    except ValueError:
        return None

    headers = _parse_header_lines(raw)
    total = end + sep_len + _content_length(headers)
    if len(data) < total:
        return None

    res = Response(version=parts[0], status_code=status,
                   status_text=parts[2].strip(_TRIM) if len(parts) > 2 else "",
                   headers=headers, body=data[end + sep_len:total])
    return res, total
