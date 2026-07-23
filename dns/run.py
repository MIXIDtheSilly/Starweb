#!/usr/bin/env python3
"""Start StarDNS from anywhere: python3 dns/run.py --log

Equivalent to `python3 -m stardns` with dns/ on the path, which is all this
does — the package is deliberately not at the checkout root.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stardns.__main__ import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
