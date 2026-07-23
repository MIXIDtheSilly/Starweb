import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from stardns import db as db_module  # noqa: E402
from stardns.tests.fakemongo import FakeDB  # noqa: E402


@pytest.fixture(autouse=True)
def fake_db(monkeypatch):
    fake = FakeDB()
    monkeypatch.setattr(db_module, "_db", fake)
    monkeypatch.setattr(db_module, "db", lambda: fake)
    for module in ("auth", "zones", "ca", "resolver", "panel"):
        mod = __import__(f"stardns.{module}", fromlist=["db"])
        if hasattr(mod, "db"):
            monkeypatch.setattr(mod, "db", lambda: fake)
    return fake


@pytest.fixture
def account(fake_db):
    from stardns import auth
    return auth.register("tester", "hunter2hunter2")
