class PanelError(Exception):
    """Raised by the model layer; the panel turns it into a status code."""

    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.message = message
        self.status = status
