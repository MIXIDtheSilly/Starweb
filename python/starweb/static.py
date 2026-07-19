from pathlib import Path

from . import content_types
from .message import Response


def sanitize_path(path: str) -> str | None:
    if ".." in path:
        return None
    path = path.split("?")[0].split("#")[0]
    if path in ("", "/"):
        return "/index.html"
    return path


def serve_file(root: str | Path, path: str) -> Response:
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
        data = target.read_bytes()
    except (FileNotFoundError, IsADirectoryError, NotADirectoryError):
        return Response(404, "Not Found", body=b"File not found.",
                        headers={"Content-Type": "text/plain"})
    except PermissionError:
        return Response(403, "Forbidden", body=b"Access Denied.",
                        headers={"Content-Type": "text/plain"})

    return Response(200, "OK", body=data,
                    headers={"Content-Type": content_types.guess(safe)})
