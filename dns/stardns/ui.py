"""HTML for the panel.

Written to what the StarWeb renderer supports, which shapes everything here:
selectors are a bare tag or a single class name (no descendant or id
selectors, and no element may carry two classes), `display:none` is not
honoured — so views are separate pages rather than one page with hidden
sections — and there is no cookie or local storage, so the session token
travels in the URL.

Two more renderer facts drive the layout below. A `<table>` paints its cells
with the table's own colour, so per-cell styling is impossible; record lists
are therefore flex rows with fixed column widths, which also lets the type
column carry a colour. And the main font is loaded with the default glyph
range, so text stays inside ASCII and Latin-1 — no em dashes, arrows or
bullets.

Links put the domain in the path and keep the token as the only query
parameter. That started as a workaround — the renderer did not decode entities
in attribute values, so `href="?a=1&amp;b=2"` arrived with `b` lost — and the
parser was fixed on 2026-07-22, but the shape is the nicer one, so it stayed.
"""

from . import config

ACCENT = "#ba8cf5"

BRAND = "Starweb DNS"

# A near-black panel: #0a0a0c page, #121216 for the masthead, cards and tiles,
# #08080a inside fields, and #ffffff body text, with the purple kept for links,
# headings and primary buttons. This no longer tracks src/browser/theme.hpp,
# which is still on the lighter greys (#1e1e1e viewport, #282828 toolbar) — the
# panel is deliberately darker than the chrome around it now.
#
# Anything meant to line up with a field repeats that field's width rather than
# splitting it. Inline siblings are spaced by a hardcoded 8px in the renderer
# and `margin-right` is ignored, so a row of two half-width buttons only lands
# flush by coincidence; the sign-in pair is full width and stacked instead.
#
# Kept free of /* comments */: the renderer splits rules on braces, so a comment
# is swallowed into the next selector and silently kills that rule.
#
# `.stage` centres sign-in on both axes, `justify-content` across and
# `align-items` down a 100vh box, since there is no `margin: auto`. A fixed
# width only reaches a card through a flex parent, which is the other reason
# sign-in is staged. `.c-*` and `.r*` share column widths so the record list
# lines up with the add-record form. `text-align` is sticky once inherited, so
# it sits on leaf paragraphs, never on a container.
CSS = """
body { background: #0a0a0c; color: #ffffff; margin: 0; padding: 0; }

h1 { color: #ba8cf5; font-size: 26px; margin-bottom: 4; }
h2 { color: #ffffff; font-size: 17px; margin-top: 0; margin-bottom: 12; }
h3 { color: #8b8b96; font-size: 13px; margin-top: 0; margin-bottom: 8; }
a { color: #ba8cf5; }
p { color: #ffffff; }
ul { margin-top: 4; margin-bottom: 4; }
li { color: #d4d4dc; font-size: 14px; }

.band {
  background: #121216;
  padding-left: 30; padding-right: 30; padding-top: 20; padding-bottom: 18;
  margin-bottom: 22;
}
.bandrow { display: flex; flex-direction: row; align-items: center;
           justify-content: space-between; }
.brand { color: #ba8cf5; font-size: 24px; margin: 0; }
.tag { color: #8b8b96; font-size: 13px; margin-top: 0; margin-bottom: 0; }
.who { color: #d4d4dc; font-size: 13px; margin: 0; }

.wrap { margin-left: 30; margin-right: 30; margin-bottom: 30; }

.stage {
  display: flex; flex-direction: row;
  justify-content: center; align-items: center;
  height: 100vh;
}
.auth { width: 460; }
.hbrand { color: #ba8cf5; font-size: 30px; text-align: center; }
.htag { color: #8b8b96; font-size: 14px; text-align: center;
        margin-top: 10; margin-bottom: 22; }
.note { color: #8b8b96; font-size: 13px; text-align: center; margin-top: 14; }
.in-auth {
  background: #08080a; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 420; margin-bottom: 4;
}
.btn-auth {
  background: #8b5cf6; color: #ffffff;
  border-width: 1; border-color: #8b5cf6; border-radius: 6;
  display: block; width: 420; height: 34; margin-top: 14;
}
.btn-auth2 {
  background: #17171c; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 6;
  display: block; width: 420; height: 34; margin-top: 8;
}

.tiles { display: flex; flex-direction: row; gap: 12; margin-bottom: 20; }
.tile {
  background: #121216;
  border-width: 1; border-color: #24242b; border-radius: 8;
  width: 168; height: 72;
  padding-left: 14; padding-top: 12;
}
.tnum { color: #ffffff; font-size: 21px; margin: 0; }
.tlab { color: #8b8b96; font-size: 12px; margin-top: 4; margin-bottom: 0; }

.card {
  background: #121216;
  border-width: 1; border-color: #24242b; border-radius: 8;
  padding: 18; margin-bottom: 16;
}
.card-warn {
  background: #1a1214;
  border-width: 1; border-color: #52292e; border-radius: 8;
  padding: 18; margin-bottom: 16;
}

.head { display: flex; flex-direction: row; align-items: center;
        justify-content: space-between; margin-bottom: 6; }
.row { display: flex; flex-direction: row; align-items: center; gap: 12;
       margin-bottom: 8; }
.grow { flex-grow: 1; }

.name { color: #ba8cf5; font-size: 19px; margin: 0; }
.meta { color: #8b8b96; font-size: 13px; margin-top: 0; margin-bottom: 12; }
.on { color: #4ade80; font-size: 13px; margin: 0; }
.off { color: #74747f; font-size: 13px; margin: 0; }

.label { color: #8b8b96; font-size: 13px; margin-bottom: 5; margin-top: 10; }
.hint { color: #74747f; font-size: 12px; margin-top: 8; margin-bottom: 0; }
.body { color: #d4d4dc; font-size: 14px; margin-top: 0; }
.mono { color: #d4d4dc; font-size: 13px; }

.kv { display: flex; flex-direction: row; align-items: center; gap: 10;
      margin-bottom: 5; }
.k { color: #8b8b96; font-size: 13px; width: 96; margin: 0; }
.v { color: #d4d4dc; font-size: 13px; flex-grow: 1; margin: 0; }

.cols { display: flex; flex-direction: row; align-items: center; gap: 12;
        margin-bottom: 6; }
.colsr { display: flex; flex-direction: row; align-items: center; gap: 12;
         margin-bottom: 6; padding-left: 10; padding-right: 10; }
.crow {
  background: #0e0e12;
  border-width: 1; border-color: #17171c; border-radius: 6;
  padding: 10; margin-bottom: 6;
  display: flex; flex-direction: row; align-items: center; gap: 12;
}
.c-name { color: #74747f; font-size: 12px; width: 150; margin: 0; }
.c-type { color: #74747f; font-size: 12px; width: 100; margin: 0; }
.c-val { color: #74747f; font-size: 12px; width: 340; margin: 0; }
.c-ttl { color: #74747f; font-size: 12px; width: 90; margin: 0; }
.c-act { color: #74747f; font-size: 12px; width: 74; margin: 0; }

.rname { color: #ffffff; font-size: 14px; width: 150; margin: 0; }
.rval { color: #d4d4dc; font-size: 14px; width: 340; margin: 0; }
.rttl { color: #8b8b96; font-size: 14px; width: 90; margin: 0; }
.rt-a { color: #79c0ff; font-size: 14px; width: 100; margin: 0; }
.rt-aaaa { color: #6fd0c0; font-size: 14px; width: 100; margin: 0; }
.rt-cname { color: #ba8cf5; font-size: 14px; width: 100; margin: 0; }
.rt-txt { color: #d9a94e; font-size: 14px; width: 100; margin: 0; }

.in {
  background: #08080a; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 300; margin-bottom: 4;
}
.in-name {
  background: #08080a; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 150;
}
.in-val {
  background: #08080a; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 340;
}
.in-ttl {
  background: #08080a; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 90;
}
.sel {
  background: #08080a; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 100;
}

.btn {
  background: #8b5cf6; color: #ffffff;
  border-width: 1; border-color: #8b5cf6; border-radius: 6;
  width: 160; height: 34; margin-top: 12; margin-right: 8;
}
.btn-alt {
  background: #17171c; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 6;
  width: 160; height: 34; margin-top: 12;
}
.btn-row {
  background: #8b5cf6; color: #ffffff;
  border-width: 1; border-color: #8b5cf6; border-radius: 6;
  width: 130; height: 32;
}
.btn-sm {
  background: #17171c; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  width: 88; height: 28;
}
.btn-del {
  background: #1d1215; color: #f5a5a5;
  border-width: 1; border-color: #52292e; border-radius: 5;
  width: 74; height: 26;
}

.msg { color: #8b8b96; font-size: 14px; margin-top: 12; }
.ok { color: #4ade80; font-size: 14px; }
.bad { color: #f87171; font-size: 14px; }
"""

MUTED = "#8b8b96"
BAD = "#f87171"


def esc(value) -> str:
    return (str(value).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def lua_str(value) -> str:
    """A Lua literal for a server-side string. Control characters are dropped
    rather than escaped; nothing that reaches here should carry any."""
    out = []
    for ch in str(value):
        if ch in ('"', "\\"):
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) >= 32:
            out.append(ch)
    return '"' + "".join(out) + '"'


def page(title: str, body: str, script: str = "") -> str:
    tail = f"<script>\n{script}\n</script>" if script else ""
    return f"""<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>{esc(title)}</title>
<link rel="stylesheet" href="/panel.css">
</head>
<body>
{body}
{tail}
</body>
</html>
"""


def band(right: str = "", tagline: str = "") -> str:
    """The masthead. `right` is markup for the far side of the top row."""
    return f"""
<div class="band">
  <div class="bandrow">
    <p class="brand">{BRAND}</p>
    {right or '<p class="who"></p>'}
  </div>
  <p class="tag">{esc(tagline)}</p>
</div>"""


def plural(n: int, word: str) -> str:
    return word if n == 1 else word + "s"


def tile(number, label: str) -> str:
    return (f'<div class="tile"><p class="tnum">{esc(number)}</p>'
            f'<p class="tlab">{esc(label)}</p></div>')


def login_page() -> str:
    body = f"""
<div class="stage">
<div class="auth">

  <p class="hbrand">{BRAND}</p>
  <p class="htag">Name registry and certificate authority for .{config.ZONE}</p>

  <div class="card">
    <h2>Sign in</h2>
    <p class="label">Username</p>
    <input type="text" class="in-auth" id="u" placeholder="yourname">
    <p class="label">Password</p>
    <input type="password" class="in-auth" id="p" placeholder="at least 8 characters">
    <div>
      <button class="btn-auth" id="signin">Sign in</button>
      <button class="btn-auth2" id="signup">Create account</button>
    </div>
    <p class="note" id="msg">New here? Pick a name and a password and the
    account is created on the spot.</p>
  </div>

</div>
</div>"""
    script = """
local msg = document.getElementById("msg")

local function say(text, cls)
    msg.textContent = text
    msg.style.color = cls or "#8b8b96"
end

local function submit(path)
    local u = document.getElementById("u").value
    local p = document.getElementById("p").value
    if u == "" or p == "" then return say("Fill in both fields.", "#f87171") end
    say("Working...")
    fetch(path, { method = "POST", json = { username = u, password = p } },
        function(err, res)
            if err then return say(err, "#f87171") end
            local data = res:json()
            if not res.ok then return say(data.error or "Failed.", "#f87171") end
            location.assign("/panel?t=" .. data.token)
        end)
end

document.getElementById("signin"):addEventListener("click", function() submit("/api/login") end)
document.getElementById("signup"):addEventListener("click", function() submit("/api/register") end)
"""
    return page(BRAND, body, script)


def panel_page(username: str, token: str, domains: list[dict],
               counts: dict[str, int], certs: dict[str, bool] | None = None) -> str:
    certs = certs or {}
    used = len(domains)
    total_records = sum(counts.get(d["name"], 0) for d in domains)
    secured = sum(1 for d in domains if certs.get(d["name"]))

    cards = []
    for d in domains:
        name = d["name"]
        n = counts.get(name, 0)
        status = ('<p class="on">Certificate issued</p>' if certs.get(name)
                  else '<p class="off">No certificate</p>')
        cards.append(f"""
<div class="card">
  <div class="head">
    <p class="name">{esc(name)}</p>
    {status}
  </div>
  <p class="meta">{n} {plural(n, 'record')}</p>
  <div class="row">
    <a class="grow" href="/domain/{esc(name)}?t={esc(token)}">Manage records and certificate</a>
    <button class="btn-del" id="drop-{esc(name)}">Delete</button>
  </div>
</div>""")

    if not domains:
        cards.append('<div class="card">'
                     '<h2>No domains yet</h2>'
                     '<p class="body">Register your first name below. It becomes '
                     f'resolvable on the .{config.ZONE} network straight away.</p>'
                     '</div>')

    at_limit = used >= config.MAX_DOMAINS
    add = f"""
<div class="card">
  <h2>Register a domain</h2>
  <div class="row">
    <input type="text" class="in-name" id="newdomain" placeholder="mysite">
    <p class="mono">.{config.ZONE}</p>
    <button class="btn-row" id="add">Register</button>
  </div>
  <p class="hint">{used} of {config.MAX_DOMAINS} used{
      ', delete one to register another.' if at_limit else '.'}
  Letters, digits and hyphens.</p>
</div>"""

    right = (f'<p class="who">{esc(username)}</p>')
    body = band(right, f"Signed in to the .{config.ZONE} registry") + f"""
<div class="wrap">

<div class="tiles">
  {tile(f"{used}/{config.MAX_DOMAINS}", "domains")}
  {tile(total_records, plural(total_records, "record"))}
  {tile(secured, plural(secured, "certificate"))}
</div>

<div class="row">
  <button class="btn-sm" id="signout">Sign out</button>
</div>

<h3>YOUR DOMAINS</h3>
{''.join(cards)}
{add}
<p class="msg" id="msg"></p>

</div>"""

    drops = "\n".join(f'bind({lua_str(d["name"])})' for d in domains)
    script = f"""
local token = {lua_str(token)}
local msg = document.getElementById("msg")

local function say(text, color)
    msg.textContent = text
    msg.style.color = color or "#8b8b96"
end

local function bind(name)
    document.getElementById("drop-" .. name):addEventListener("click", function()
        say("Deleting " .. name .. "...")
        fetch("/api/domain/delete", {{ method = "POST",
            json = {{ token = token, domain = name }} }}, function(err, res)
            if err then return say(err, "#f87171") end
            local data = res:json()
            if not res.ok then return say(data.error or "Failed.", "#f87171") end
            location.assign("/panel?t=" .. token)
        end)
    end)
end

{drops}

document.getElementById("add"):addEventListener("click", function()
    local name = document.getElementById("newdomain").value
    if name == "" then return say("Enter a name.", "#f87171") end
    say("Registering...")
    fetch("/api/domain/add", {{ method = "POST",
        json = {{ token = token, domain = name }} }}, function(err, res)
        if err then return say(err, "#f87171") end
        local data = res:json()
        if not res.ok then return say(data.error or "Failed.", "#f87171") end
        location.assign("/panel?t=" .. token)
    end)
end)

document.getElementById("signout"):addEventListener("click", function()
    fetch("/api/logout", {{ method = "POST", json = {{ token = token }} }},
        function() location.assign("/") end)
end)
"""
    return page(f"{BRAND} - domains", body, script)


def _record_rows(records: list[dict]) -> str:
    if not records:
        return ('<p class="body">No records yet. A single A record pointing at '
                'your server is enough to make the name resolve.</p>')

    # .colsr repeats the row's padding so the captions sit over the columns
    # rather than 10px to their left. The border does not inset content in this
    # renderer, so padding alone is the offset to match.
    head = """
<div class="colsr">
  <p class="c-name">NAME</p>
  <p class="c-type">TYPE</p>
  <p class="c-val">VALUE</p>
  <p class="c-ttl">TTL</p>
  <p class="c-act"></p>
</div>"""

    rows = []
    for r in records:
        rid = str(r["_id"])
        rtype = str(r["type"])
        rows.append(f"""
<div class="crow">
  <p class="rname">{esc(r['name'])}</p>
  <p class="rt-{esc(rtype.lower())}">{esc(rtype)}</p>
  <p class="rval">{esc(r['value'])}</p>
  <p class="rttl">{esc(r['ttl'])}s</p>
  <button class="btn-del" id="rm-{esc(rid)}">Delete</button>
</div>""")
    return head + "".join(rows)


def domain_page(token: str, domain: str, records: list[dict],
                cert: dict | None, ca_note: str | None) -> str:
    if ca_note:
        cert_body = f'<p class="bad">Certificates are unavailable: {esc(ca_note)}.</p>'
    elif cert:
        cert_body = f"""
<p class="ok">Issued {esc(cert['issued_at'].strftime('%Y-%m-%d %H:%M UTC'))}</p>
<div class="kv"><p class="k">Serial</p><p class="v">{esc(cert['serial'][:16])}</p></div>
<div class="kv"><p class="k">Names</p><p class="v">{esc(cert['sans'])}</p></div>
<div class="kv"><p class="k">Expires</p><p class="v">{esc(cert['not_after'])}</p></div>
<div class="row">
  <a href="/cert/{esc(domain)}/cert?t={esc(token)}">Download certificate</a>
  <a href="/cert/{esc(domain)}/key?t={esc(token)}">Download private key</a>
</div>
<p class="hint">Also written to {esc(config.ISSUED)}/{esc(domain)}.pem and .key</p>
<button class="btn" id="issue">Re-issue</button>"""
    else:
        cert_body = f"""
<p class="body">No certificate yet. Issuing one gives you a leaf for
{esc(domain)} and *.{esc(domain)}, signed by the StarWeb root CA, which is
enough to serve star:// to any StarWeb client on the network.</p>
<button class="btn" id="issue">Issue certificate</button>"""

    left = config.MAX_RECORDS - len(records)
    right = f'<p class="who">{esc(domain)}</p>'
    body = band(right, "Records and certificate for this zone") + f"""
<div class="wrap">

<div class="row">
  <a href="/panel?t={esc(token)}">&lt; All domains</a>
</div>

<div class="tiles">
  {tile(len(records), plural(len(records), "record"))}
  {tile(left, plural(left, "slot") + " left")}
  {tile(config.DEFAULT_TTL, "default TTL")}
</div>

<div class="card">
  <h2>Records</h2>
  {_record_rows(records)}
  <p class="hint">"@" means the domain itself, as a name or as a CNAME target;
  a CNAME target with no dot in it is relative to {esc(domain)}.</p>
</div>

<div class="card">
  <h2>Add a record</h2>
  <div class="cols">
    <p class="c-name">NAME</p>
    <p class="c-type">TYPE</p>
    <p class="c-val">VALUE</p>
    <p class="c-ttl">TTL</p>
  </div>
  <div class="row">
    <input type="text" class="in-name" id="rname" placeholder="@ or www">
    <select class="sel" id="rtype">
      <option value="A">A</option>
      <option value="AAAA">AAAA</option>
      <option value="CNAME">CNAME</option>
      <option value="TXT">TXT</option>
    </select>
    <input type="text" class="in-val" id="rvalue" placeholder="127.0.0.1">
    <input type="number" class="in-ttl" id="rttl" value="{config.DEFAULT_TTL}">
  </div>
  <button class="btn" id="addrec">Add record</button>
</div>

<div class="card">
  <h2>Certificate</h2>
  {cert_body}
</div>

<p class="msg" id="msg"></p>

</div>"""

    binds = "\n".join(f'bind({lua_str(str(r["_id"]))})' for r in records)
    script = f"""
local token = {lua_str(token)}
local domain = {lua_str(domain)}
local msg = document.getElementById("msg")

local function say(text, color)
    msg.textContent = text
    msg.style.color = color or "#8b8b96"
end

local function reload()
    location.assign("/domain/" .. domain .. "?t=" .. token)
end

local function post(path, payload, done)
    fetch(path, {{ method = "POST", json = payload }}, function(err, res)
        if err then return say(err, "#f87171") end
        local data = res:json()
        if not res.ok then return say(data.error or "Failed.", "#f87171") end
        done(data)
    end)
end

local function bind(id)
    document.getElementById("rm-" .. id):addEventListener("click", function()
        say("Deleting...")
        post("/api/record/delete",
            {{ token = token, domain = domain, id = id }}, reload)
    end)
end

{binds}

document.getElementById("addrec"):addEventListener("click", function()
    say("Adding...")
    post("/api/record/add", {{
        token = token, domain = domain,
        name = document.getElementById("rname").value,
        type = document.getElementById("rtype").value,
        value = document.getElementById("rvalue").value,
        ttl = document.getElementById("rttl").value,
    }}, reload)
end)

local issue = document.getElementById("issue")
if issue then
    issue:addEventListener("click", function()
        say("Signing... this takes a moment.")
        post("/api/cert/issue", {{ token = token, domain = domain }}, reload)
    end)
end
"""
    return page(f"{BRAND} - {domain}", body, script)


def error_page(message: str, token: str | None = None) -> str:
    back = (f'<a href="/panel?t={esc(token)}">&lt; Back to your domains</a>'
            if token else '<a href="/">&lt; Sign in</a>')
    body = band(tagline="Something went wrong") + f"""
<div class="wrap">
<div class="card-warn">
  <p class="bad">{esc(message)}</p>
</div>
<div class="row">{back}</div>
</div>"""
    return page(BRAND, body)
