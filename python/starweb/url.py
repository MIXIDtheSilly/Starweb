from dataclasses import dataclass

from .errors import URLError

DEFAULT_PORTS = {"moon": 8090, "star": 8490}


@dataclass
class ParsedURL:
    scheme: str
    host: str
    port: int
    path: str

    @property
    def is_default_port(self) -> bool:
        return self.port == DEFAULT_PORTS[self.scheme]

    @property
    def host_header(self) -> str:
        h = format_host(self.host)
        return h if self.is_default_port else f"{h}:{self.port}"

    def __str__(self) -> str:
        return f"{self.scheme}://{self.host_header}{self.path}"


def format_host(host: str) -> str:
    return f"[{host}]" if ":" in host else host


def parse_url(url: str) -> ParsedURL:
    scheme_pos = url.find("://")
    if scheme_pos == -1:
        raise URLError(f"no scheme in URL: {url!r}")

    scheme = url[:scheme_pos].lower()
    if scheme not in DEFAULT_PORTS:
        raise URLError(f"not a StarWeb scheme: {scheme!r}")

    port = DEFAULT_PORTS[scheme]
    rest = url[scheme_pos + 3:]
    if not rest:
        raise URLError(f"no host in URL: {url!r}")

    slash = rest.find("/")
    if slash == -1:
        host_port, path = rest, "/"
    else:
        host_port, path = rest[:slash], rest[slash:]

    if not host_port:
        raise URLError(f"no host in URL: {url!r}")

    port_part = ""
    if host_port[0] == "[":
        close = host_port.find("]")
        if close in (-1, 1):
            raise URLError(f"malformed IPv6 literal: {url!r}")
        host = host_port[1:close]
        after = host_port[close + 1:]
        if after:
            if after[0] != ":":
                raise URLError(f"malformed IPv6 literal: {url!r}")
            port_part = after[1:]
    else:
        colon = host_port.find(":")
        if colon == -1:
            host = host_port
        else:
            host = host_port[:colon]
            port_part = host_port[colon + 1:]
            if not port_part:
                raise URLError(f"trailing ':' with no port: {url!r}")

    if port_part:
        try:
            port = int(port_part)
        except ValueError:
            raise URLError(f"bad port: {url!r}") from None

    if not host:
        raise URLError(f"no host in URL: {url!r}")

    return ParsedURL(scheme=scheme, host=host, port=port, path=path)
