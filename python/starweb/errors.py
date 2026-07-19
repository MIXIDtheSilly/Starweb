class StarWebError(Exception):
    pass


class URLError(StarWebError):
    pass


class ProtocolError(StarWebError):
    pass


class ConnectionFailed(StarWebError):
    pass


class ALPNError(StarWebError):
    pass


class TLSVerificationError(StarWebError):
    pass


class MixedContentError(StarWebError):
    pass
