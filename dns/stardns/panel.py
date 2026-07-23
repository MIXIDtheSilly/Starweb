"""The control panel: STWP pages plus a small JSON API, served by starweb."""
import json

from starweb import App, Response

from . import auth, ca, config, ui, zones
from .db import db
from .errors import PanelError

STATUS_TEXT = {
    200: "OK", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
    404: "Not Found", 409: "Conflict", 500: "Internal Server Error",
    503: "Service Unavailable",
}

app = App()


def _json(payload: dict, status: int = 200) -> Response:
    return Response(status, STATUS_TEXT.get(status, "Error"),
                    body=json.dumps(payload).encode(),
                    headers={"Content-Type": "application/json"})


def _fail(err: PanelError) -> Response:
    return _json({"error": err.message}, err.status)


def _html(markup: str, status: int = 200) -> Response:
    return Response(status, STATUS_TEXT.get(status, "Error"),
                    body=markup.encode(),
                    headers={"Content-Type": "text/html; charset=utf-8"})


def _body(req) -> dict:
    try:
        payload = json.loads(req.body or b"{}")
    except ValueError:
        raise PanelError("Expected a JSON body.") from None
    if not isinstance(payload, dict):
        raise PanelError("Expected a JSON object.")
    return payload


@app.route("/panel.css")
def stylesheet(req):
    return Response(200, body=ui.CSS.encode(),
                    headers={"Content-Type": "text/css; charset=utf-8"})


@app.route("/")
def index(req):
    return _html(ui.login_page())


@app.route("/panel")
def panel(req):
    token = req.query.get("t", "")
    try:
        username = auth.user_for(token)
        domains = zones.list_domains(username)
    except PanelError as e:
        return _html(ui.error_page(e.message), e.status)

    counts = {d["name"]: db().records.count_documents({"domain": d["name"]})
              for d in domains}
    certs = {d["name"]: ca.latest(d["name"]) is not None for d in domains}
    return _html(ui.panel_page(username, token, domains, counts, certs))


# The domain sits in the path, not the query: the renderer does not decode HTML
# entities in attribute values, so an href carrying a second `&amp;` parameter
# arrives with the entity intact and the parameter lost.
@app.route("/domain/<name>")
def domain_view(req, name):
    token = req.query.get("t", "")
    try:
        username = auth.user_for(token)
        domain = zones.get_domain(username, name)
        records = zones.list_records(domain["name"])
    except PanelError as e:
        return _html(ui.error_page(e.message, token if token else None), e.status)

    ok, why = ca.ca_ready()
    return _html(ui.domain_page(token, domain["name"], records,
                                ca.latest(domain["name"]),
                                None if ok else why))


@app.route("/cert/<name>/<what>")
def cert_download(req, name, what):
    token = req.query.get("t", "")
    try:
        username = auth.user_for(token)
        domain = zones.get_domain(username, name)
        filename, pem = ca.read_material(domain["name"], what)
    except PanelError as e:
        return _html(ui.error_page(e.message, token if token else None), e.status)

    return Response(200, body=pem.encode(), headers={
        "Content-Type": "text/plain; charset=utf-8",
        "Content-Disposition": f'attachment; filename="{filename}"',
    })


@app.route("/api/register", methods=["POST"])
def api_register(req):
    try:
        payload = _body(req)
        token = auth.register(payload.get("username", ""), payload.get("password", ""))
    except PanelError as e:
        return _fail(e)
    return _json({"token": token})


@app.route("/api/login", methods=["POST"])
def api_login(req):
    try:
        payload = _body(req)
        token = auth.login(payload.get("username", ""), payload.get("password", ""))
    except PanelError as e:
        return _fail(e)
    return _json({"token": token})


@app.route("/api/logout", methods=["POST"])
def api_logout(req):
    try:
        auth.logout(_body(req).get("token", ""))
    except PanelError as e:
        return _fail(e)
    return _json({"ok": True})


@app.route("/api/domains", methods=["POST"])
def api_domains(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        domains = zones.list_domains(username)
    except PanelError as e:
        return _fail(e)
    return _json({
        "limit": config.MAX_DOMAINS,
        "domains": [{"name": d["name"],
                     "records": db().records.count_documents({"domain": d["name"]})}
                    for d in domains],
    })


@app.route("/api/domain/add", methods=["POST"])
def api_domain_add(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        doc = zones.add_domain(username, payload.get("domain", ""))
    except PanelError as e:
        return _fail(e)
    return _json({"domain": doc["name"]})


@app.route("/api/domain/delete", methods=["POST"])
def api_domain_delete(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        zones.delete_domain(username, payload.get("domain", ""))
    except PanelError as e:
        return _fail(e)
    return _json({"ok": True})


@app.route("/api/records", methods=["POST"])
def api_records(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        domain = zones.get_domain(username, payload.get("domain", ""))
        records = zones.list_records(domain["name"])
    except PanelError as e:
        return _fail(e)
    return _json({"domain": domain["name"], "records": [
        {"id": str(r["_id"]), "name": r["name"], "type": r["type"],
         "value": r["value"], "ttl": r["ttl"],
         "fqdn": zones.fqdn(r["name"], domain["name"])}
        for r in records
    ]})


@app.route("/api/record/add", methods=["POST"])
def api_record_add(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        doc = zones.add_record(username, payload.get("domain", ""),
                               payload.get("name", "@"), payload.get("type", ""),
                               payload.get("value", ""), payload.get("ttl"))
    except PanelError as e:
        return _fail(e)
    return _json({"id": str(doc["_id"]), "name": doc["name"],
                  "type": doc["type"], "value": doc["value"], "ttl": doc["ttl"]})


@app.route("/api/record/delete", methods=["POST"])
def api_record_delete(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        zones.delete_record(username, payload.get("domain", ""),
                            payload.get("id", ""))
    except PanelError as e:
        return _fail(e)
    return _json({"ok": True})


@app.route("/api/cert/issue", methods=["POST"])
def api_cert_issue(req):
    try:
        payload = _body(req)
        username = auth.user_for(payload.get("token", ""))
        domain = zones.get_domain(username, payload.get("domain", ""))
        doc = ca.issue(domain["name"])
    except PanelError as e:
        return _fail(e)
    return _json({"domain": doc["domain"], "serial": doc["serial"],
                  "sans": doc["sans"], "not_after": doc["not_after"],
                  "cert_path": doc["cert_path"], "key_path": doc["key_path"]})
