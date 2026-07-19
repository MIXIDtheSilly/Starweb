TYPES = {
    ".html": "text/html",
    ".htm": "text/html",
    ".css": "text/css",
    ".lua": "application/x-lua",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".txt": "text/plain",
    ".mov": "video/mp4",
    ".mp4": "video/mp4",
    ".mp3": "audio/mpeg",
    ".json": "application/json",
}

DEFAULT = "application/octet-stream"


def guess(path: str) -> str:
    dot = path.rfind(".")
    if dot == -1:
        return DEFAULT
    return TYPES.get(path[dot:].lower(), DEFAULT)
