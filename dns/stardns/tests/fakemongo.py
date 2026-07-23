"""An in-memory stand-in for the slice of pymongo that StarDNS uses.

Enough to run the resolver and the panel against real logic without a mongod,
which is what the tests here do.
"""
import re
from bson import ObjectId


def _matches(doc: dict, query: dict) -> bool:
    for key, want in query.items():
        got = doc.get(key)
        if isinstance(want, dict):
            if "$regex" in want and (got is None or
                                     not re.search(want["$regex"], str(got))):
                return False
            if "$in" in want and got not in want["$in"]:
                return False
            if "$ne" in want and got == want["$ne"]:
                return False
        elif got != want:
            return False
    return True


class _Cursor:
    def __init__(self, docs):
        self._docs = docs

    def sort(self, key, direction=1):
        if isinstance(key, list):
            for field, d in reversed(key):
                self._docs.sort(key=lambda x: x.get(field), reverse=d < 0)
        else:
            self._docs.sort(key=lambda x: x.get(key), reverse=direction < 0)
        return self

    def __iter__(self):
        return iter(self._docs)


class FakeCollection:
    def __init__(self):
        self.docs: list[dict] = []

    def create_index(self, *a, **kw):
        return "index"

    def insert_one(self, doc):
        doc.setdefault("_id", ObjectId())
        self.docs.append(dict(doc))
        return type("R", (), {"inserted_id": doc["_id"]})()

    def find(self, query=None):
        return _Cursor([dict(d) for d in self.docs if _matches(d, query or {})])

    def find_one(self, query=None, sort=None):
        found = [dict(d) for d in self.docs if _matches(d, query or {})]
        if sort:
            for field, d in reversed(sort):
                found.sort(key=lambda x: x.get(field), reverse=d < 0)
        return found[0] if found else None

    def count_documents(self, query=None):
        return sum(1 for d in self.docs if _matches(d, query or {}))

    def delete_one(self, query):
        for i, d in enumerate(self.docs):
            if _matches(d, query):
                del self.docs[i]
                return type("R", (), {"deleted_count": 1})()
        return type("R", (), {"deleted_count": 0})()

    def delete_many(self, query):
        keep = [d for d in self.docs if not _matches(d, query)]
        removed = len(self.docs) - len(keep)
        self.docs = keep
        return type("R", (), {"deleted_count": removed})()

    def update_one(self, query, update):
        for d in self.docs:
            if _matches(d, query):
                d.update(update.get("$set", {}))
                return type("R", (), {"modified_count": 1})()
        return type("R", (), {"modified_count": 0})()


class FakeDB:
    def __init__(self):
        self._collections: dict[str, FakeCollection] = {}

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return self._collections.setdefault(name, FakeCollection())

    def __getitem__(self, name):
        return getattr(self, name)

    def command(self, *a, **kw):
        return {"ok": 1}
