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

from pathlib import Path

from . import config, shapes

# The halftone renderer the login page runs per frame. Kept as Lua on disk
# rather than a string in here so it stays readable and editable on its own.
ART = (Path(__file__).resolve().parent / "nebula_art.lua").read_text()

ACCENT = "#ba8cf5"

BRAND = "Starweb DNS"

# A near-black panel: #000000 page, #121216 for the masthead, cards and tiles,
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
# `.stage` uses `justify-content: space-between` rather than `center` so the
# two `.side` canvases sit flush against the viewport edges; since both are
# the same width, `.auth` still lands centred between them. `align-items`
# centres it vertically down a 100vh box, since there is no `margin: auto`.
# A fixed width only reaches a card through a flex parent, which is the other
# reason sign-in is staged. `.c-*` and `.r*` share column widths so the record list
# lines up with the add-record form. `text-align` is sticky once inherited, so
# it sits on leaf paragraphs, never on a container.
CSS = """
body { background: #000000; color: #ffffff; margin: 0; padding: 0; }

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
  justify-content: space-between; align-items: center;
  height: 100vh;
}
.side { width: 28vw; height: 100vh; }

/* 90vh inside the centred stage parks the column in the upper half. */
.auth {
  display: flex; flex-direction: column; align-items: center;
  width: 31vw; height: 90vh;
}

/* Wider than .auth on purpose: it overhangs the form on both sides. Both axes
   are vw so the 2.83:1 banner keeps its aspect on any window shape. */
.logo { width: 58vw; height: 20.5vw; margin-bottom: 6; }

.lbl {
  color: #8f8a9e; font-family: Inter SemiBold; font-size: 15px;
  width: 31vw; margin-top: 0; margin-bottom: 8;
}
.fld {
  background: #000000; color: #ffffff;
  border-width: 1; border-color: #7c5cff; border-radius: 8;
  padding-left: 14; padding-top: 11;
  width: 31vw; height: 38; margin-bottom: 22;
}
.cta {
  background: #8b5cf6; color: #ffffff; font-family: Inter SemiBold;
  border-width: 1; border-color: #8b5cf6; border-radius: 8;
  width: 31vw; height: 44; margin-top: 30;
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

/* The signed-in shell. The sidebar, its hairline and the artwork are all out
   of the flow, pinned to the viewport, so only the content column scrolls --
   which also keeps `vh` off anything that has to grow with the page. Paint
   order is document order, so the artwork is written first and the sidebar
   last. The sidebar paints no background of its own: the page is already black
   and the artwork is meant to run under it. */
.art-bl { position: fixed; left: 0; bottom: 0; width: 300; height: 354; }
.art-tr { position: fixed; right: 0; top: 0; width: 184; height: 204; }

.nav {
  position: fixed; left: 0; top: 0; bottom: 0;
  width: 300;
  padding-left: 24; padding-right: 24; padding-top: 26;
}
.navmark { width: 196; height: 71; margin-left: -4; margin-bottom: 16; }

.rule { background: #17171c; position: fixed; left: 300; top: 0; bottom: 0; width: 1; }

.qbox {
  background: #000000;
  border-width: 1; border-color: #7c5cff; border-radius: 8;
  width: 252; height: 38; margin-bottom: 20;
  padding-left: 12;
  display: flex; flex-direction: row; align-items: center; gap: 10;
}
.qico { width: 15; height: 15; }
.qfld {
  background: #000000; color: #ffffff;
  border-width: 0; font-size: 14px;
  width: 200; padding-left: 0; padding-top: 4;
}

/* The current tab is the only row with a fill; the rest repeat the page colour
   so both kinds of row measure and indent identically. */
.navon {
  background: #17171c; border-radius: 8;
  width: 240; height: 40; margin-bottom: 4;
  padding-left: 12;
  display: flex; flex-direction: row; align-items: center; gap: 12;
}
.navoff {
  background: #000000; border-radius: 8;
  width: 240; height: 40; margin-bottom: 4;
  padding-left: 12;
  display: flex; flex-direction: row; align-items: center; gap: 12;
}
.navico { width: 18; height: 18; }
.navtxt { color: #ffffff; font-family: Inter SemiBold; font-size: 15px; margin: 0; }
.navmut { color: #b6b6c2; font-size: 15px; margin: 0; }

.main {
  display: flex; flex-direction: column; align-items: center;
  margin-left: 301; margin-bottom: 40;
}

.topbar {
  display: flex; flex-direction: row; justify-content: flex-end;
  align-self: stretch;
  padding-right: 30; padding-top: 18;
}
.avatar {
  background: #14141a;
  border-width: 1; border-color: #2f2f3a; border-radius: 22;
  width: 44; height: 44;
  display: flex; flex-direction: row; justify-content: center; align-items: center;
}
.avico { width: 24; height: 24; }

.hub { display: flex; flex-direction: column; align-items: center; align-self: stretch; }
.page { align-self: stretch; padding-left: 40; padding-right: 40; padding-top: 6; }

.hero {
  color: #ffffff; font-family: Inter SemiBold; font-size: 32px;
  margin-top: 56; margin-bottom: 26;
}

.sbox {
  background: #000000;
  border-width: 1; border-color: #6b4ef0; border-radius: 10;
  width: 52vw; height: 52; margin-bottom: 28;
  padding-left: 18;
  display: flex; flex-direction: row; align-items: center; gap: 12;
}
.sico { width: 19; height: 19; }
.sfld {
  background: #000000; color: #ffffff;
  border-width: 0; font-size: 15px;
  width: 46vw; padding-left: 0; padding-top: 7;
}

.hubcols {
  width: 52vw;
  display: flex; flex-direction: row; justify-content: space-between;
}
.hubcol { width: 15vw; display: flex; flex-direction: column; }

.colhead { display: flex; flex-direction: row; align-items: center; gap: 6;
           margin-bottom: 10; }
.colttl { color: #8b8b96; font-size: 14px; margin: 0; }
.colchev { width: 13; height: 13; }

.lrow { display: flex; flex-direction: row; align-items: center; gap: 10;
        height: 34; }
.lico { width: 16; height: 16; }
.lname { color: #ffffff; font-family: Inter SemiBold; font-size: 14px;
         flex-grow: 1; margin: 0; }
.lchev { width: 16; height: 16; }
.hair { background: #1c1c22; height: 1; margin-bottom: 4; }
.empty { color: #6f6f7c; font-size: 13px; margin-top: 2; }
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


# The decoration is drawn, not laid out: the renderer has no absolute
# positioning, so the artwork lives in two full-height canvases flanking the
# form, and each shape is placed to bleed off its own canvas edge.
SCENE = """
-- Every size below is a multiple of S. sqrt(W*H) grows with both axes, so a
-- shape keeps its footprint whichever edge the window is dragged from; sizing
-- off W alone made everything track the width and ignore the height.
local function unit(W, H) return math.sqrt(W * H) end

-- makeLine's ox/oy are the polyline's own origin, so shapes always grew away
-- from the top-left. anchor() pins a corner of the shape's bounding box
-- instead: ax/ay of 0 is left/top, 1 is right/bottom.
local function anchor(L, scale, ax, ay, x, y)
    local lox, hix = L.x[1], L.x[1]
    local loy, hiy = L.y[1], L.y[1]
    for i = 2, #L.x do
        if L.x[i] < lox then lox = L.x[i] elseif L.x[i] > hix then hix = L.x[i] end
        if L.y[i] < loy then loy = L.y[i] elseif L.y[i] > hiy then hiy = L.y[i] end
    end
    return x - (lox + (hix - lox) * ax) * scale,
           y - (loy + (hiy - loy) * ay) * scale
end

makeScene("lft", function(W, H)
    local S = unit(W, H)
    -- Snake hugs the left edge, entering just under the top.
    local ox, oy = anchor(LINE_L, S * 1.01, 0, 0, -S * 0.354, S * 0.057)
    -- Cog hugs the bottom-left. The radius is capped at the distance to the
    -- right edge because the canvas clips its own drawing at W.
    local cx = math.min(S * 0.19, W * 0.30)
    return {
        makeLine(LINE_L, ox, oy, S * 1.01, 0.4),
        makeCog(cx, H - S * 0.158, math.min(S * 0.392, W - cx), 0.00022, 0.0, -2.2),
    }
end)

makeScene("rgt", function(W, H)
    local S = unit(W, H)
    -- Line hugs the right edge and runs off the bottom-right corner.
    local ox, oy = anchor(LINE_R, S * 1.11, 1, 1, W + S * 0.138, H + S * 0.035)
    return {
        makeCog(W - S * 0.155, S * 0.127, S * 0.373, -0.00026, 1.7, -0.9),
        makeLine(LINE_R, ox, oy, S * 1.11, 2.6),
    }
end)
"""


# The icon layer. Geometry comes from the lucide SVGs in assets/icons, flattened
# to polylines by shapes.lua_icons(); this is only the drawing. `s` is pixels per
# grid unit, so one table serves every size a page asks for, and `paint` draws
# once the renderer has sized the canvas -- ops persist until cleared, so a
# static icon costs nothing per frame. Round caps and joins are what lucide is
# drawn with, and without them the corners come out as mitre spikes.
ICONS = """
local function stroke(ctx, s, p)
    ctx:beginPath()
    ctx:moveTo(p[1] * s, p[2] * s)
    for i = 3, #p, 2 do ctx:lineTo(p[i] * s, p[i + 1] * s) end
    ctx:stroke()
end

function paint(id, name, colour, weight)
    local cv = document.getElementById(id)
    local paths = ICON_PATHS[name]
    if not cv or not paths then return end
    local ctx = cv:getContext("2d")
    local w, h = -1, -1

    local function draw()
        if cv.width ~= w or cv.height ~= h then
            w, h = cv.width, cv.height
            if w > 0 and h > 0 then
                ctx:clearRect(0, 0, w, h)
                ctx.strokeStyle = colour
                ctx.lineWidth = weight
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                for i = 1, #paths do stroke(ctx, w / 24, paths[i]) end
            end
        end
        requestAnimationFrame(draw)
    end
    requestAnimationFrame(draw)
end

-- A whole row is the click target, so nothing inside it has to be an anchor.
function link(id, url)
    local el = document.getElementById(id)
    if el then el:addEventListener("click", function() location.assign(url) end) end
end
"""

# Both decorations are sized from their viewBox rather than from the path's own
# extent, since the paths deliberately reach outside it. The canvases are a fixed
# number of pixels, so the artwork keeps one size and hugs its corner however the
# window is dragged, rather than growing with the viewport.
SHELL_ART = """
makeScene("art-bl", function(W, H)
    local scale = W * 1.7
    return { makeLine(LOOP_B, -0.06 * scale, 0.04 * scale, scale, 1.1, 1.3) }
end)

makeScene("art-tr", function(W, H)
    return { makeLine(PFP_T, 0, 0, W, 2.4, 1.3) }
end)
"""

# tab key, label, icon, path
TABS = (
    ("home", "Account home", "house", "/panel"),
    ("domains", "Domains", "globe", "/domains"),
    ("analytics", "Analytics", "chart-pie", "/analytics"),
)


def shell(active: str, token: str, content: str, art: bool = False) -> tuple[str, str]:
    """The signed-in frame. Returns the markup and the script that goes with it;
    a page appends its own icons and row links to the latter. The artwork is the
    account home's alone -- behind a list of cards it is just noise."""
    rows, script = [], []
    for key, label, icon, path in TABS:
        on = key == active
        rows.append(f"""
    <div class="{'navon' if on else 'navoff'}" id="nav-{key}">
      <canvas class="navico" id="i-{key}"></canvas>
      <p class="{'navtxt' if on else 'navmut'}">{esc(label)}</p>
    </div>""")
        script.append(f'paint("i-{key}", "{icon}", '
                      f'"{"#ffffff" if on else "#b6b6c2"}", 1.55)')
        script.append(f'link("nav-{key}", {lua_str(path + "?t=" + token)})')

    decoration = ('<canvas class="art-bl" id="art-bl"></canvas>\n'
                  '<canvas class="art-tr" id="art-tr"></canvas>') if art else ""
    body = f"""
{decoration}

<div class="main">
  <div class="topbar">
    <div class="avatar" id="avatar">
      <canvas class="avico" id="i-user"></canvas>
    </div>
  </div>
{content}
</div>

<div class="rule"></div>

<div class="nav">
  <img class="navmark" src="/banner_trans.png">

  <div class="qbox">
    <canvas class="qico" id="i-quick"></canvas>
    <input type="text" class="qfld" placeholder="Quick search...">
  </div>
{''.join(rows)}
</div>"""

    script.append('paint("i-quick", "search", "#6f6f7c", 1.45)')
    script.append('paint("i-user", "user-round", "#8b8b96", 1.55)')
    prelude = shapes.lua_icons() + ((shapes.lua_shapes() + ART) if art else "")
    return body, (prelude + ICONS + "\n" + "\n".join(script)
                  + ("\n" + SHELL_ART if art else ""))


def login_page() -> str:
    # Markup mirrors www/index.html one for one: the two flanking canvases, a
    # banner, the two labelled fields and a single "Log In" button. The only
    # change is the button carries the login wiring index.html never had.
    body = """
<div class="stage">
  <canvas class="side" id="lft"></canvas>

  <div class="auth">
    <img class="logo" src="/banner.png">
    <p class="lbl">Username</p>
    <input type="text" class="fld" id="u">
    <p class="lbl">Password</p>
    <input type="password" class="fld" id="p">
    <button class="cta" id="go">Log In</button>
  </div>

  <canvas class="side" id="rgt"></canvas>
</div>"""
    # index.html has no message element, so feedback rides on the button label:
    # it shows progress and any error there and restores "Log In" so a second
    # try is possible. /api/access logs in, or creates the account if the
    # username is new, then lands on the panel.
    script = shapes.lua_shapes() + ART + SCENE + """
local go = document.getElementById("go")

go:addEventListener("click", function()
    local u = document.getElementById("u").value
    local p = document.getElementById("p").value
    if u == "" or p == "" then go.textContent = "Fill in both fields"; return end
    go.textContent = "Working..."
    fetch("/api/access", { method = "POST", json = { username = u, password = p } },
        function(err, res)
            if err then go.textContent = err; return end
            local data = res:json()
            if not res.ok then go.textContent = data.error or "Log In"; return end
            location.assign("/panel?t=" .. data.token)
        end)
end)
"""
    return page(BRAND, body, script)


def _hub_column(title: str, rows: str, key: str) -> str:
    """One column of the account home: a heading with a chevron, then rows."""
    return f"""
      <div class="hubcol">
        <div class="colhead" id="head-{key}">
          <p class="colttl">{esc(title)}</p>
          <canvas class="colchev" id="hc-{key}"></canvas>
        </div>
{rows}
      </div>"""


# The number of domains the home tab lists before it stops and defers to the
# Domains tab. Five is what fits above the fold next to the artwork.
HUB_ROWS = 5


def home_page(username: str, token: str, domains: list[dict]) -> str:
    listed = domains[:HUB_ROWS]

    rows, script = [], []
    for i, d in enumerate(listed, 1):
        rows.append(f"""
        <div class="lrow" id="row-{i}">
          <canvas class="lico" id="rg-{i}"></canvas>
          <p class="lname">{esc(d['name'])}</p>
          <canvas class="lchev" id="rc-{i}"></canvas>
        </div>
        <div class="hair"></div>""")
        script.append(f'paint("rg-{i}", "globe", "#8b8b96", 1.45)')
        script.append(f'paint("rc-{i}", "chevron-right", "#6f6f7c", 1.45)')
        target = f"/domain/{d['name']}?t={token}"
        script.append(f'link("row-{i}", {lua_str(target)})')

    if not listed:
        rows.append('        <p class="empty">No domains yet. Register one from '
                    'the Domains tab.</p>')

    columns = [
        _hub_column("Domains", "".join(rows), "dom"),
        _hub_column("Analytics",
                    '        <p class="empty">Nothing measured yet.</p>', "ana"),
        _hub_column("Recent", '        <p class="empty">Nothing yet.</p>', "rec"),
    ]
    for key, path in (("dom", "/domains"), ("ana", "/analytics")):
        script.append(f'paint("hc-{key}", "chevron-right", "#8b8b96", 1.5)')
        script.append(f'link("head-{key}", {lua_str(path + "?t=" + token)})')
    script.append('paint("hc-rec", "chevron-right", "#8b8b96", 1.5)')

    # The search field is not wired to anything yet: a text field keeps the
    # keyboard to itself while it has focus, so there is no Enter to act on, and
    # the design has no button. The columns are the way through for now.
    content = f"""
    <div class="hub">
      <p class="hero">What are we doing today?</p>

      <div class="sbox">
        <canvas class="sico" id="i-search"></canvas>
        <input type="text" class="sfld" placeholder="Search">
      </div>

      <div class="hubcols">
{''.join(columns)}
      </div>
    </div>"""

    body, shell_script = shell("home", token, content, art=True)
    script.append('paint("i-search", "search", "#8b8b96", 1.6)')
    return page(f"{BRAND} - {esc(username)}", body,
                shell_script + "\n" + "\n".join(script))


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

    content = f"""
    <div class="page">

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
    body, shell_script = shell("domains", token, content)

    drops = "\n".join(f'bind({lua_str(d["name"])})' for d in domains)
    script = shell_script + f"""
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
            location.assign("/domains?t=" .. token)
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
        location.assign("/domains?t=" .. token)
    end)
end)

document.getElementById("signout"):addEventListener("click", function()
    fetch("/api/logout", {{ method = "POST", json = {{ token = token }} }},
        function() location.assign("/") end)
end)
"""
    return page(f"{BRAND} - domains", body, script)


def analytics_page(token: str) -> str:
    content = """
    <div class="page">
    <h3>ANALYTICS</h3>
    <div class="card">
      <h2>Nothing measured yet</h2>
      <p class="body">Query counts per domain land here once the resolver keeps
      them. Records and certificates are on the Domains tab.</p>
    </div>
    </div>"""
    body, script = shell("analytics", token, content)
    return page(f"{BRAND} - analytics", body, script)


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
  <a href="/domains?t={esc(token)}">&lt; All domains</a>
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
