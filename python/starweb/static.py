from pathlib import Path

from . import content_types
from .message import FileBody, Response


def sanitize_path(path: str) -> str | None:
    if ".." in path:
        return None
    path = path.split("?")[0].split("#")[0]
    if path in ("", "/"):
        return "/index.html"
    return path


def parse_byte_range(header: str, file_size: int) -> tuple[int, int] | None:
    """Parses a single `bytes=a-b` against a known size, or None if unusable.

    Mirrors parse_byte_range in src/server/server.cpp. Multi-range is refused
    rather than answered with a wrong slice. The suffix form `bytes=-N` is what
    media players use to read a trailing moov atom, so it has to work.
    """
    if file_size <= 0:
        return None

    prefix = "bytes="
    if not header.startswith(prefix):
        return None
    spec = header[len(prefix):].strip()
    if "," in spec:
        return None

    dash = spec.find("-")
    if dash == -1:
        return None
    first, last = spec[:dash].strip(), spec[dash + 1:].strip()
    if not first and not last:
        return None

    try:
        if not first:
            n = int(last)
            if n <= 0:
                return None
            start = 0 if n >= file_size else file_size - n
            end = file_size - 1
        else:
            start = int(first)
            end = file_size - 1 if not last else int(last)
    except ValueError:
        return None

    if start < 0 or end < 0:
        return None
    if end >= file_size:
        end = file_size - 1
    if start > end or start >= file_size:
        return None
    return start, end


def serve_file(root: str | Path, path: str,
               range_header: str | None = None) -> Response:
    safe = sanitize_path(path)
    if safe is None:
        return Response(403, "Forbidden", body=b"Access Denied.",
                        headers={"Content-Type": "text/plain"})

    base = Path(root).resolve()
    target = (base / safe.lstrip("/")).resolve()
    if not target.is_relative_to(base):
        return Response(403, "Forbidden", body=b"Access Denied.",
                        headers={"Content-Type": "text/plain"})

    try:
        stat = target.stat()
        if not target.is_file():
            raise IsADirectoryError
    except (FileNotFoundError, IsADirectoryError, NotADirectoryError):
        return Response(404, "Not Found", body=b"File not found.",
                        headers={"Content-Type": "text/plain"})
    except PermissionError:
        return Response(403, "Forbidden", body=b"Access Denied.",
                        headers={"Content-Type": "text/plain"})

    size = stat.st_size
    headers = {
        "Content-Type": content_types.guess(safe),
        "Accept-Ranges": "bytes",
    }

    if range_header is None:
        return Response(200, "OK", headers=headers,
                        file=FileBody(target, 0, size))

    span = parse_byte_range(range_header, size)
    if span is None:
        # Matches the C++ server: a Range header present but unusable is refused
        # rather than quietly answered with the whole file.
        return Response(416, "Range Not Satisfiable",
                        headers={"Content-Type": "text/plain",
                                 "Accept-Ranges": "bytes",
                                 "Content-Range": f"bytes */{size}"})

    start, end = span
    headers["Content-Range"] = f"bytes {start}-{end}/{size}"
    return Response(206, "Partial Content", headers=headers,
                    file=FileBody(target, start, end - start + 1))
