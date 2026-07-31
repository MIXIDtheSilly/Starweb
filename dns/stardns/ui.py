"""HTML for the panel.

Written to what the StarWeb renderer supports, which shapes everything here:
selectors are a bare tag or a single class name (no descendant or id
selectors, and no element may carry two classes), `display:none` is not
honoured, so views are separate pages rather than one page with hidden
sections, and there is no cookie or local storage, so the session token
travels in the URL.

Two more renderer facts drive the layout below. A `<table>` paints its cells
with the table's own colour, so per-cell styling is impossible; record lists
are therefore flex rows with fixed column widths, which also lets the type
column carry a colour. And the main font is loaded with the default glyph
range, so text stays inside ASCII and Latin-1: no em dashes, arrows or
bullets.

Links put the domain in the path and keep the token as the only query
parameter. That started as a workaround: the renderer did not decode entities
in attribute values, so `href="?a=1&amp;b=2"` arrived with `b` lost, and the
parser was fixed on 2026-07-22, but the shape is the nicer one, so it stayed.
"""

from pathlib import Path

from . import analytics, config, shapes

# Kept as a separate .lua file rather than a string here so it stays readable and editable.
ART = (Path(__file__).resolve().parent / "nebula_art.lua").read_text()

ACCENT = "#ba8cf5"

BRAND = "Nebula"

# Flat black everywhere (#000000), white body text, purple for links, headings
# and primary buttons. Everything else is demarcated only by border colour
# (grey normally, purple on the flagship trend/analytics cards). Deliberately
# darker than src/browser/theme.hpp's chrome (#1e1e1e/#282828), which this no
# longer tracks.
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
/* Set once here so every element inherits Inter via merge_node_style()
   instead of the platform default. Only the SemiBold weight is bundled, so
   that's the family used everywhere. */
body { background: #000000; color: #ffffff; margin: 0; padding: 0;
       font-family: Inter SemiBold; }

h1 { color: #ba8cf5; font-size: 26px; margin-bottom: 4; }
h2 { color: #ffffff; font-size: 17px; margin-top: 0; margin-bottom: 12; }
h3 { color: #8b8b96; font-size: 13px; margin-top: 0; margin-bottom: 8; }
a { color: #ba8cf5; }
p { color: #ffffff; }
ul { margin-top: 4; margin-bottom: 4; }
li { color: #d4d4dc; font-size: 14px; }

.band {
  background: #000000;
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
  background: #000000;
  border-width: 1; border-color: #24242b; border-radius: 8;
  width: 168; height: 72;
  padding-left: 14; padding-top: 12;
}
.tnum { color: #ffffff; font-size: 21px; margin: 0; }
.tlab { color: #8b8b96; font-size: 12px; margin-top: 4; margin-bottom: 0; }

.card {
  background: #000000;
  border-width: 1; border-color: #24242b; border-radius: 8;
  padding: 18; margin-bottom: 16;
}
.card-warn {
  background: #000000;
  border-width: 1; border-color: #52292e; border-radius: 8;
  padding: 18; margin-bottom: 16;
}
/* Bolder purple outline (matching .trend) marks this as the primary action,
   not just another list card. */
.addcard {
  background: #000000;
  border-width: 2; border-color: #6b4ef0; border-radius: 8;
  padding: 18; margin-bottom: 16;
}

.row { display: flex; flex-direction: row; align-items: center; gap: 12;
       margin-bottom: 8; }

/* flex-wrap: fields overlap instead of shrinking when the row can't fit
   their declared widths, so wrapping onto a second line is the fix. */
.addrow { display: flex; flex-direction: row; align-items: center; flex-wrap: wrap;
          gap: 20; margin-top: 4; margin-bottom: 4; }

.backrow { display: flex; flex-direction: row; align-items: center; gap: 6;
           margin-bottom: 16; }
.backico { width: 14; height: 14; }
/* Nudges text down to match a same-height canvas icon; <p> boxes get extra
   intrinsic-height padding that isn't reflected in the glyph position. */
.backlbl { color: #8b8b96; font-size: 13px; margin: 0; margin-top: 3; }

.section { margin-bottom: 24; }

/* Hairline-separated like the Home hub and Analytics list, not a bordered card per domain. */
.drow { display: flex; flex-direction: row; align-items: center; gap: 12;
        height: 44; }
/* Single class since an element can't carry two, so this one also carries flex-grow. */
.dname { color: #ba8cf5; font-family: Inter SemiBold; font-size: 16px;
         margin: 0; flex-grow: 1; }
.dico { width: 18; height: 18; }
.dspark { width: 100; height: 28; }
.dcert { width: 18; height: 18; }

.label { color: #8b8b96; font-size: 13px; margin-bottom: 5; margin-top: 10; }
.hint { color: #74747f; font-size: 12px; margin-top: 8; margin-bottom: 0; }
.body { color: #d4d4dc; font-size: 14px; margin-top: 0; }
.certdesc { color: #d4d4dc; font-size: 14px; margin-top: 0; margin-bottom: 10; }
.mono { color: #d4d4dc; font-size: 13px; }

.kv { display: flex; flex-direction: row; align-items: center; gap: 10;
      margin-bottom: 10; }
.k { color: #8b8b96; font-size: 13px; width: 96; margin: 0; }
.v { color: #d4d4dc; font-size: 13px; flex-grow: 1; margin: 0; }

.certhead { display: flex; flex-direction: row; align-items: center; gap: 10;
            margin-bottom: 14; }
.certicon { width: 22; height: 22; }
/* See .backlbl above for why text next to a canvas icon needs this nudge. */
.certttl { color: #ffffff; font-family: Inter SemiBold; font-size: 15px; margin: 0; margin-top: 3; }
.certlinks { display: flex; flex-direction: row; align-items: center; gap: 12;
             margin-top: 10; margin-bottom: 4; }

.cols { display: flex; flex-direction: row; align-items: center; gap: 12;
        margin-bottom: 6; }
.colsr { display: flex; flex-direction: row; align-items: center; gap: 12;
         margin-bottom: 6; padding-left: 10; padding-right: 10; }
.crow {
  background: #000000;
  border-width: 1; border-color: #24242b; border-radius: 6;
  padding: 10; margin-bottom: 6;
  display: flex; flex-direction: row; align-items: center; gap: 12;
}
.c-name { color: #74747f; font-size: 12px; width: 10vw; margin: 0; }
.c-type { color: #74747f; font-size: 12px; width: 7vw; margin: 0; }
.c-val { color: #74747f; font-size: 12px; flex-grow: 1; margin: 0; }
.c-ttl { color: #74747f; font-size: 12px; width: 6vw; margin: 0; }
.c-act { color: #74747f; font-size: 12px; width: 74; margin: 0; }

/* Same vw units as .in-name/.sel/.in-val/.in-ttl below, so the add-record
   row's columns track the records table's columns as the window resizes. */
.ac-name { color: #74747f; font-size: 12px; width: 10vw; margin: 0; }
.ac-type { color: #74747f; font-size: 12px; width: 7vw; margin: 0; }
.ac-val { color: #74747f; font-size: 12px; margin: 0; flex-grow: 1; }
.ac-ttl { color: #74747f; font-size: 12px; width: 6vw; margin: 0; }

/* margin-top matches text to .btn-del's vertical center; buttons don't get
   the same extra intrinsic-height padding text tags do. */
.rname { color: #ffffff; font-size: 14px; width: 10vw; margin: 0; margin-top: 3; }
.rval { color: #d4d4dc; font-size: 14px; flex-grow: 1; margin: 0; margin-top: 3; }
.rttl { color: #8b8b96; font-size: 14px; width: 6vw; margin: 0; margin-top: 3; }
.rt-a { color: #79c0ff; font-size: 14px; width: 7vw; margin: 0; margin-top: 3; }
.rt-aaaa { color: #6fd0c0; font-size: 14px; width: 7vw; margin: 0; margin-top: 3; }
.rt-cname { color: #ba8cf5; font-size: 14px; width: 7vw; margin: 0; margin-top: 3; }
.rt-txt { color: #d9a94e; font-size: 14px; width: 7vw; margin: 0; margin-top: 3; }

.in {
  background: #000000; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 300; margin-bottom: 4;
}
/* vw-based, not px, so these actually shrink with the window; .ac-name/etc
   above are matching caption widths, kept separate from the records table's
   fixed .c-name/etc. */
.in-name {
  background: #000000; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 10vw;
}
/* No width: fills whatever NAME/TYPE/TTL leave in the row via
   widget_fill_width() in renderer.cpp, since flex-grow alone doesn't repaint
   widgets at their grown size. */
.in-val {
  background: #000000; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; flex-grow: 1;
}
.in-ttl {
  background: #000000; color: #ffffff;
  border-width: 1; border-color: #2e2e37; border-radius: 5;
  padding: 7; width: 6vw;
}
.sel {
  background: #000000; color: #ffffff;
  border-width: 0; border-radius: 5;
  padding: 7; width: 7vw;
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
.btn-del {
  background: #000000; color: #f87171;
  border-width: 1; border-color: #f87171; border-radius: 5;
  width: 74; height: 26;
}

.msg { color: #8b8b96; font-size: 14px; margin-top: 12; }
.bad { color: #f87171; font-size: 14px; }

/* The signed-in shell. The sidebar, its hairline and the artwork are all out
   of the flow, pinned to the viewport, so only the content column scrolls,
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

/* position:fixed keeps the closed state out of flow; toggled via className.
   The idle state sets no background at all, since even a matching black
   still paints a fill. */
.qdropoff {
  position: fixed; left: 24; top: 161; width: 252;
  border-width: 0; border-radius: 8;
}
.qdropon {
  position: fixed; left: 24; top: 161; width: 252;
  background: #000000; border-width: 1; border-color: #7c5cff; border-radius: 8;
  padding-top: 6; padding-bottom: 6;
}
.qheadoff { color: #6f6f7c; font-size: 11px; margin: 0; }
.qheadon { color: #6f6f7c; font-size: 11px; margin: 0;
           padding-left: 12; padding-top: 8; padding-bottom: 4; }
/* The dropdown is a plain block, not a flex container, so an unset width here
   would fall back to the full window rather than 252. */
.qrow { display: flex; flex-direction: row; align-items: center; gap: 10;
        width: 228; height: 36; padding-left: 12; padding-right: 12; }
.qricon { width: 15; height: 15; }
.qrname { color: #d4d4dc; font-size: 13px; margin: 0; margin-top: 2; }
.qrchev { width: 13; height: 13; }

/* left tracks .sbox's centring but is computed in Lua (heroPosition, home_page)
   since this engine has no calc() to combine px and vw. */
.hdropoff {
  position: fixed; top: 248; width: 52vw;
  border-width: 0; border-radius: 8;
}
.hdropon {
  position: fixed; top: 248; width: 52vw;
  background: #000000; border-width: 1; border-color: #7c5cff; border-radius: 8;
  padding-top: 6; padding-bottom: 6;
}
.hheadoff { color: #6f6f7c; font-size: 11px; margin: 0; }
.hheadon { color: #6f6f7c; font-size: 11px; margin: 0;
           padding-left: 12; padding-top: 8; padding-bottom: 4; }
.hrow { display: flex; flex-direction: row; align-items: center; gap: 10;
        width: 52vw; height: 36; padding-left: 12; padding-right: 12; }
.hricon { width: 15; height: 15; }
.hrname { color: #d4d4dc; font-size: 13px; margin: 0; margin-top: 2; }
.hrchev { width: 13; height: 13; }

/* 1px sliver whose .width in Lua is the only way to read live window width. */
.vpmeter { position: fixed; left: 0; top: 0; width: 100vw; height: 1; }

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
  background: #000000;
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
/* See .backlbl: a <p> measures taller than its glyphs, so centring the box in
   the row leaves the text sitting above the icon beside it. */
.lname { color: #ffffff; font-family: Inter SemiBold; font-size: 14px;
         flex-grow: 1; margin: 0; margin-top: 3; }
.lchev { width: 16; height: 16; }
.hair { background: #1c1c22; height: 1; margin-bottom: 4; }
.empty { color: #6f6f7c; font-size: 13px; margin-top: 2; }

/* .trend fills .page's width like other cards there. .trendhome instead
   pins to 52vw to line up with .hubcols in the vw-based .hub. Both get a
   purple outline, like qbox/sbox, since this is the flagship feature. */
.trend {
  background: #000000;
  border-width: 1; border-color: #6b4ef0; border-radius: 8;
  padding: 18; margin-bottom: 16;
}
.trendhome {
  background: #000000;
  border-width: 1; border-color: #6b4ef0; border-radius: 8;
  padding: 18; margin-top: 22;
  width: 52vw;
}
.trendhead { display: flex; flex-direction: row; align-items: center;
             justify-content: space-between; }
.trendttl { color: #ffffff; font-family: Inter SemiBold; font-size: 15px; margin: 0; }
.trendtot { color: #ba8cf5; font-size: 24px; margin: 0; }
/* .trendcv/.trendlabs take no width, so they auto-match .trend's own unsized
   content width. .trendhome sets an explicit 52vw though, so its children
   don't narrow to match; .trendcvh/.trendlabsh are pinned a bit inside that
   52vw instead, the same margin .sbox leaves .sfld. */
.trendcv { height: 130; margin-top: 16; margin-bottom: 6; }
/* Same unsized width as .trendcv, inside a plain .card rather than .trend. */
.barcv { height: 108; margin-top: 2; margin-bottom: 6; }
.trendcvh { width: 47vw; height: 108; margin-top: 14; margin-bottom: 6; }
.trendlabs { display: flex; flex-direction: row; justify-content: space-between; }
.trendlabsh { display: flex; flex-direction: row; justify-content: space-between;
              width: 47vw; }
.trendlab { color: #6f6f7c; font-size: 11px; margin: 0; }

/* Two classes rather than one plus a modifier, since an element can only
   carry a single class here; same split as .navon/.navoff. */
.ranges { display: flex; flex-direction: row; align-items: center; gap: 8;
          margin-bottom: 18; }
.rgon {
  background: #17171c;
  border-width: 1; border-color: #6b4ef0; border-radius: 14;
  height: 28; padding-left: 13; padding-right: 13;
  display: flex; flex-direction: row; align-items: center;
}
.rgoff {
  background: #000000;
  border-width: 1; border-color: #24242b; border-radius: 14;
  height: 28; padding-left: 13; padding-right: 13;
  display: flex; flex-direction: row; align-items: center;
}
.rgtxt { color: #ffffff; font-size: 12px; margin: 0; margin-top: 3; }
.rgmut { color: #8b8b96; font-size: 12px; margin: 0; margin-top: 3; }

/* Wider than .k: these labels carry the window's own noun, so they run to
   "Busiest 5 minutes" rather than the cert card's one-word keys. */
.ak { color: #8b8b96; font-size: 13px; width: 132; margin: 0; }

.spark { display: flex; flex-direction: row; align-items: center; gap: 14;
         height: 40; }
.sparkname { color: #ffffff; font-family: Inter SemiBold; font-size: 14px;
             width: 160; margin: 0; }
.sparkcv { width: 160; height: 30; }
.sparktot { color: #8b8b96; font-size: 13px; width: 90; margin: 0; }
.sparkchev { width: 15; height: 15; }
"""

MUTED = "#8b8b96"
BAD = "#f87171"


def icon_src(name: str, color: str) -> str:
    """URL for a recoloured lucide icon, served by shapes.icon_svg() via panel.py."""
    return f"/assets/icon/{name}/{color.lstrip('#')}"


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


def lua_arr(values) -> str:
    """A Lua literal for a flat array of integers, e.g. a query-count series."""
    return "{" + ",".join(str(int(v)) for v in values) + "}"


def lua_strs(values) -> str:
    """A Lua literal for a flat array of strings, e.g. a chart's axis labels."""
    return "{" + ",".join(lua_str(v) for v in values) + "}"


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


def _queries(n: int) -> str:
    return "query" if n == 1 else "queries"


def _fit(name: str, limit: int) -> str:
    """Domain labels run up to 63 characters; the fixed-width tile and spark
    row that show one don't wrap or clip on overflow, so anything past what
    the box can hold needs shortening before it lands there."""
    return name if len(name) <= limit else name[: limit - 3] + "..."


def range_pills(active: str, path: str, token: str) -> tuple[str, list[str]]:
    """The window picker. Returns the markup and the links that drive it; the
    selected window rides in `r` alongside the token."""
    pills, script = [], []
    for i, (key, short, *_) in enumerate(analytics.RANGES, 1):
        on = key == active
        pills.append(f"""
      <div class="{'rgon' if on else 'rgoff'}" id="rp-{i}">
        <p class="{'rgtxt' if on else 'rgmut'}">{esc(short)}</p>
      </div>""")
        script.append(f'link("rp-{i}", {lua_str(f"{path}?t={token}&r={key}")})')
    return f'<div class="ranges">{"".join(pills)}</div>', script


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


# stroke() backs drawIcon() (the two live search dropdowns); link() and
# urlenc() are used everywhere.
ICONS = """
local function stroke(ctx, s, p)
    ctx:beginPath()
    ctx:moveTo(p[1] * s, p[2] * s)
    for i = 3, #p, 2 do ctx:lineTo(p[i] * s, p[i + 1] * s) end
    ctx:stroke()
end

-- A whole row is the click target, so nothing inside it has to be an anchor.
function link(id, url)
    local el = document.getElementById(id)
    if el then el:addEventListener("click", function() location.assign(url) end) end
end

-- Percent-encodes a search query for the URL: the wire format splits the
-- request line on the first two spaces (see stwp_msg.hpp), so a raw space in
-- the query would truncate the path there. There is no JS-style
-- encodeURIComponent in this runtime, hence this.
function urlenc(s)
    local out = {}
    for i = 1, #s do
        local b = string.byte(s, i)
        local c = string.sub(s, i, i)
        if (b >= 48 and b <= 57) or (b >= 65 and b <= 90) or (b >= 97 and b <= 122)
           or c == "-" or c == "_" or c == "." or c == "~" then
            out[#out + 1] = c
        else
            out[#out + 1] = string.format("%%%02X", b)
        end
    end
    return table.concat(out)
end
"""

# The canvas API is a small hand-rolled 2D context (no closePath, curves, or
# fillText alignment), so charts are just fillRect bars or a stroked
# polyline; axis labels are plain <p> elements laid out around the canvas.
CHARTS = """
local function chartMax(values)
    local m = 0
    for i = 1, #values do if values[i] > m then m = values[i] end end
    return m > 0 and m or 1
end

local PILLPAD = 14
local PILLH = 26

-- measureText and the corner radius on fillRect are both newer than some builds
-- out there, and a missing method is a nil call rather than a no-op, so the
-- guess stays as the fallback. An older build draws the readout square.
local function textWidth(ctx, text)
    if ctx.measureText then
        local m = ctx:measureText(text)
        if m and m.width and m.width > 0 then return m.width end
    end
    return #text * 8
end

local function readout(ctx, w, text, centre)
    local tw = textWidth(ctx, text) + PILLPAD * 2
    local x = centre - tw / 2
    -- A pixel off each edge, since the outline straddles the rect it traces.
    if x < 1 then x = 1 elseif x + tw > w - 1 then x = w - 1 - tw end
    local r = PILLH / 2
    ctx.fillStyle = "#17171c"
    ctx:fillRect(x, 2, tw, PILLH, r)
    ctx.strokeStyle = "#6b4ef0"
    ctx.lineWidth = 1
    ctx:strokeRect(x, 2, tw, PILLH, r)
    ctx.fillStyle = "#ffffff"
    ctx:fillText(text, x + PILLPAD, 7)
end

-- cv.hoverX is the pointer's x within the canvas, or -1 when it is elsewhere.
-- Read from the rAF loop rather than delivered as an event, so the redraw the
-- highlight needs is the one already running. A build that predates it reports
-- nil, and comparing that would kill the callback before it re-registers,
-- taking the whole chart with it, so the fallback is not optional.
function chartBars(id, values, labels, colour)
    local cv = document.getElementById(id)
    if not cv then return end
    local ctx = cv:getContext("2d")
    local w, h, hot = -1, -1, -2
    local n = #values

    local function draw()
        local cw, ch = cv.width, cv.height
        local gap, bw = 0, 0
        if cw > 0 and n > 0 then
            gap = math.min(4, cw / (n * 6))
            bw = (cw - gap * (n - 1)) / n
        end

        local hx = cv.hoverX or -1
        local over = -1
        if bw > 0 and hx >= 0 then
            local i = math.floor(hx / (bw + gap)) + 1
            if i >= 1 and i <= n then over = i end
        end

        if cw ~= w or ch ~= h or over ~= hot then
            w, h, hot = cw, ch, over
            if w > 0 and h > 0 and n > 0 then
                ctx:clearRect(0, 0, w, h)
                local maxv = chartMax(values)
                for i = 1, n do
                    -- A zero bar draws nothing, so a quiet series does not
                    -- smear into a solid floor, but the hovered one keeps a
                    -- stub to show the readout belongs to it.
                    if values[i] > 0 or i == hot then
                        local bh = 2
                        if values[i] > 0 then
                            bh = math.max(2, (values[i] / maxv) * (h - 2))
                        end
                        ctx.fillStyle = (i == hot) and "#cbb2ff" or colour
                        ctx:fillRect((i - 1) * (bw + gap), h - bh, bw, bh)
                    end
                end
                if hot > 0 then
                    readout(ctx, w, labels[hot] .. ": " .. values[hot],
                            (hot - 1) * (bw + gap) + bw / 2)
                end
            end
        end
        requestAnimationFrame(draw)
    end
    requestAnimationFrame(draw)
end

function chartLine(id, values, colour)
    local cv = document.getElementById(id)
    if not cv then return end
    local ctx = cv:getContext("2d")
    local w, h = -1, -1
    local n = #values

    local function draw()
        if cv.width ~= w or cv.height ~= h then
            w, h = cv.width, cv.height
            if w > 0 and h > 0 and n > 1 then
                ctx:clearRect(0, 0, w, h)
                local maxv = chartMax(values)
                local pad = 3
                ctx.strokeStyle = colour
                ctx.lineWidth = 2
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx:beginPath()
                for i = 1, n do
                    local x = (i - 1) / (n - 1) * (w - pad * 2) + pad
                    local y = h - pad - (values[i] / maxv) * (h - pad * 2)
                    if i == 1 then ctx:moveTo(x, y) else ctx:lineTo(x, y) end
                end
                ctx:stroke()
            end
        end
        requestAnimationFrame(draw)
    end
    requestAnimationFrame(draw)
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

# How many rows a search widget reserves. Fixed, since there is no way to
# create DOM nodes at runtime here: every page renders all of them up front,
# collapsed, and JS only ever fills in or clears what's already there.
SEARCH_SLOTS = 6

# drawIcon draws once rather than looping on requestAnimationFrame, since a
# repeat call per keystroke would pile up one redraw loop per keystroke. Still
# canvas+Lua rather than an <img>, since which icon a row needs is only known
# once the user has typed something, and an <img>'s src can't swap live.
DRAW_ICON_ONCE = """
function drawIcon(id, name, colour, weight)
    local cv = document.getElementById(id)
    if not cv then return end
    local ctx = cv:getContext("2d")
    local w, h = cv.width, cv.height
    if w <= 0 or h <= 0 then return end
    ctx:clearRect(0, 0, w, h)
    local paths = name and ICON_PATHS[name]
    if not paths then return end
    ctx.strokeStyle = colour
    ctx.lineWidth = weight
    ctx.lineCap = "round"
    ctx.lineJoin = "round"
    for i = 1, #paths do stroke(ctx, w / 24, paths[i]) end
end
"""


def _search_widget(prefix: str, field_id: str, token: str,
                   domains: list[dict]) -> tuple[str, str]:
    """A self-contained live search box: a header and SEARCH_SLOTS rows.
    `prefix` names everything (classes, ids, the Lua index/target/search
    function) so two calls with different prefixes share no state."""
    items = [(label, icon, f"{path}?t={token}") for _, label, icon, path in TABS]
    for d in domains:
        name = d["name"]
        items.append((name, "globe", f"/domain/{name}?t={token}"))
        items.append((f"Analytics for {name}", "chart-pie", f"/analytics/{name}?t={token}"))
    index = ",\n  ".join("{" + lua_str(label) + ", " + lua_str(icon) + ", " + lua_str(url) + "}"
                         for label, icon, url in items)

    rows, binds = [f'<p class="{prefix}headoff" id="{prefix}head"></p>'], []
    for i in range(1, SEARCH_SLOTS + 1):
        rows.append(f"""
    <div class="{prefix}row" id="{prefix}r-{i}">
      <canvas class="{prefix}ricon" id="{prefix}ri-{i}"></canvas>
      <p class="{prefix}rname" id="{prefix}rn-{i}"></p>
      <canvas class="{prefix}rchev" id="{prefix}rc-{i}"></canvas>
    </div>""")
        binds.append(f"""
document.getElementById({lua_str(f"{prefix}r-{i}")}):addEventListener("click", function()
    if {prefix}Target[{i}] then location.assign({prefix}Target[{i}]) end
end)""")
    markup = f'<div class="{prefix}dropoff" id="{prefix}drop">{"".join(rows)}</div>'

    script = f"""
local {prefix}Index = {{
  {index}
}}
local {prefix}Target = {{}}

local function {prefix}Search(q)
    local qlow = string.lower(q)
    local n = 0
    if q ~= "" then
        for i = 1, #{prefix}Index do
            if n >= {SEARCH_SLOTS} then break end
            local item = {prefix}Index[i]
            if string.find(string.lower(item[1]), qlow, 1, true) then
                n = n + 1
                {prefix}Target[n] = item[3]
                document.getElementById({lua_str(prefix + "rn-")} .. n).textContent = item[1]
                drawIcon({lua_str(prefix + "ri-")} .. n, item[2], "#d4d4dc", 1.4)
                drawIcon({lua_str(prefix + "rc-")} .. n, "chevron-right", "#6f6f7c", 1.3)
            end
        end
    end
    for i = n + 1, {SEARCH_SLOTS} do
        {prefix}Target[i] = nil
        document.getElementById({lua_str(prefix + "rn-")} .. i).textContent = ""
        drawIcon({lua_str(prefix + "ri-")} .. i)
        drawIcon({lua_str(prefix + "rc-")} .. i)
    end
    document.getElementById({lua_str(prefix + "head")}).className =
        (n > 0) and {lua_str(prefix + "headon")} or {lua_str(prefix + "headoff")}
    document.getElementById({lua_str(prefix + "head")}).textContent = (n > 0) and "GO TO" or ""
    document.getElementById({lua_str(prefix + "drop")}).className =
        (n > 0) and {lua_str(prefix + "dropon")} or {lua_str(prefix + "dropoff")}
end

document.getElementById({lua_str(field_id)}):addEventListener("input", function()
    {prefix}Search(document.getElementById({lua_str(field_id)}).value)
end)
""" + "\n".join(binds)

    return markup, script


def shell(active: str, token: str, domains: list[dict], content: str,
         art: bool = False) -> tuple[str, str]:
    """The signed-in frame. Returns the markup and the script that goes with it;
    a page appends its own icons and row links to the latter. The artwork is the
    account home's alone; behind a list of cards it is just noise."""
    rows, script = [], []
    for key, label, icon, path in TABS:
        on = key == active
        rows.append(f"""
    <div class="{'navon' if on else 'navoff'}" id="nav-{key}">
      <img class="navico" src="{icon_src(icon, '#ffffff' if on else '#b6b6c2')}">
      <p class="{'navtxt' if on else 'navmut'}">{esc(label)}</p>
    </div>""")
        script.append(f'link("nav-{key}", {lua_str(path + "?t=" + token)})')

    qdrop_markup, qdrop_script = _search_widget("q", "qfld", token, domains)
    decoration = ('<canvas class="art-bl" id="art-bl"></canvas>\n'
                  '<canvas class="art-tr" id="art-tr"></canvas>') if art else ""
    body = f"""
{decoration}

<div class="main">
  <div class="topbar">
    <div class="avatar" id="avatar">
      <img class="avico" src="{icon_src('user-round', '#8b8b96')}">
    </div>
  </div>
{content}
</div>

<div class="rule"></div>

<div class="nav">
  <img class="navmark" src="/banner_trans.png">

  <div class="qbox">
    <img class="qico" src="{icon_src('search', '#6f6f7c')}">
    <input type="text" class="qfld" id="qfld" placeholder="Quick search...">
  </div>
{''.join(rows)}
{qdrop_markup}
</div>"""

    script.append(DRAW_ICON_ONCE)
    script.append(qdrop_script)
    # Sign out lives on the shared avatar now, not a Domains-only button, so
    # every tab can reach it.
    script.append(f"""
document.getElementById("avatar"):addEventListener("click", function()
    fetch("/api/logout", {{ method = "POST", json = {{ token = {lua_str(token)} }} }},
        function() location.assign("/") end)
end)""")
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
          <img class="colchev" src="{icon_src('chevron-right', '#8b8b96')}">
        </div>
{rows}
      </div>"""


# The number of domains the home tab lists before it stops and defers to the
# Domains tab. Five is what fits above the fold next to the artwork.
HUB_ROWS = 5


def home_page(username: str, token: str, domains: list[dict],
             series: list[int], labels: list[str],
             recent: list[dict] = ()) -> str:
    listed = domains[:HUB_ROWS]

    rows, script = [], []
    for i, d in enumerate(listed, 1):
        rows.append(f"""
        <div class="lrow" id="row-{i}">
          <img class="lico" src="{icon_src('globe', '#8b8b96')}">
          <p class="lname">{esc(d['name'])}</p>
          <img class="lchev" src="{icon_src('chevron-right', '#6f6f7c')}">
        </div>
        <div class="hair"></div>""")
        target = f"/domain/{d['name']}?t={token}"
        script.append(f'link("row-{i}", {lua_str(target)})')

    if not listed:
        rows.append('        <p class="empty">No domains yet. Register one from '
                    'the Domains tab.</p>')

    total = sum(series)
    if domains:
        ana_body = (f'        <p class="tnum">{total}</p>\n'
                   '        <p class="tlab">Queries, last 14 days</p>')
    else:
        ana_body = ('        <p class="empty">Register a domain to start '
                   'counting queries.</p>')

    rec_rows = []
    for i, r in enumerate(recent, 1):
        rec_rows.append(f"""
        <div class="lrow" id="rrow-{i}">
          <img class="lico" src="{icon_src(r['icon'], '#8b8b96')}">
          <p class="lname">{esc(r['label'])}</p>
          <img class="lchev" src="{icon_src('chevron-right', '#6f6f7c')}">
        </div>
        <div class="hair"></div>""")
        script.append(f'link("rrow-{i}", {lua_str(r["path"] + "?t=" + token)})')
    if not rec_rows:
        rec_rows.append('        <p class="empty">Places you visit will show up '
                        'here.</p>')

    columns = [
        _hub_column("Domains", "".join(rows), "dom"),
        _hub_column("Analytics", ana_body, "ana"),
        _hub_column("Recent", "".join(rec_rows), "rec"),
    ]
    for key, path in (("dom", "/domains"), ("ana", "/analytics")):
        script.append(f'link("head-{key}", {lua_str(path + "?t=" + token)})')

    content = f"""
    <div class="hub">
      <p class="hero">What are we doing today?</p>

      <div class="sbox">
        <img class="sico" id="i-search" src="{icon_src('search', '#8b8b96')}">
        <input type="text" class="sfld" id="sfld" placeholder="Search">
      </div>

      <div class="hubcols">
{''.join(columns)}
      </div>

      <div class="trendhome">
        <div class="trendhead">
          <p class="trendttl">Queries across your domains</p>
          <p class="trendtot">{total}</p>
        </div>
        <canvas class="trendcvh" id="chart-home"></canvas>
        {axis(labels, analytics.DEFAULT_RANGE, "trendlabsh")}
      </div>
    </div>"""

    body, shell_script = shell("home", token, domains, content, art=True)
    hdrop_markup, hdrop_script = _search_widget("h", "sfld", token, domains)
    script.append(f"""
document.getElementById("i-search"):addEventListener("click", function()
    local q = document.getElementById("sfld").value
    if q ~= "" then
        location.assign({lua_str(f"/search?t={token}&q=")} .. urlenc(q))
    end
end)""")
    # Retries until #vpmeter reports a width (unsized on the first frame),
    # capped so it can't spin requestAnimationFrame forever.
    script.append("""
local heroTries = 0
local function heroPosition()
    local vpw = document.getElementById("vpmeter").width
    if vpw > 0 then
        local hubw = vpw - 301
        local sboxw = 0.52 * vpw
        local left = 301 + (hubw - sboxw) / 2
        document.getElementById("hdrop").style.left = tostring(left) .. "px"
        return
    end
    heroTries = heroTries + 1
    if heroTries < 60 then requestAnimationFrame(heroPosition) end
end
heroPosition()""")
    script.append(f'chartLine("chart-home", {lua_arr(series)}, "#ba8cf5")')
    # Must come after the sidebar in document order to paint on top of it.
    vpmeter_markup = '<canvas class="vpmeter" id="vpmeter"></canvas>'
    return page(f"{BRAND} - {esc(username)}", body + vpmeter_markup + hdrop_markup,
                shell_script + "\n" + CHARTS + "\n" + "\n".join(script) +
                "\n" + hdrop_script)


def panel_page(username: str, token: str, domains: list[dict],
               counts: dict[str, int], certs: dict[str, bool] | None = None,
               queries: dict[str, int] | None = None,
               series: dict[str, list[int]] | None = None) -> str:
    certs = certs or {}
    queries = queries or {}
    series = series or {}
    used = len(domains)
    total_records = sum(counts.get(d["name"], 0) for d in domains)
    total_queries = sum(queries.get(d["name"], 0) for d in domains)

    # One card, hairline-separated rows (icon, name, sparkline, cert icon),
    # matching Home/Analytics rather than a bordered card per domain. Line,
    # not bars: bars packed into ~100px read as a smear at this size.
    rows, icon_script = [], []
    for i, d in enumerate(domains, 1):
        name = d["name"]
        has_cert = certs.get(name, False)
        cert_icon = (icon_src("shield-check", "#4ade80") if has_cert
                    else icon_src("shield-question-mark", "#74747f"))
        rows.append(f"""
<div class="drow" id="drow-{i}">
  <img class="dico" src="{icon_src('globe', '#8b8b96')}">
  <a class="dname" href="/domain/{esc(name)}?t={esc(token)}">{esc(name)}</a>
  <canvas class="dspark" id="dchart-{i}"></canvas>
  <img class="dcert" src="{cert_icon}">
  <button class="btn-del" id="drop-{esc(name)}">Delete</button>
</div>""")
        if i < len(domains):
            rows.append('<div class="hair"></div>')
        icon_script.append(f'chartLine("dchart-{i}", '
                           f'{lua_arr(series.get(name) or [0] * analytics.DEFAULT_DAYS)}, '
                           f'"#8b6ef0")')

    if domains:
        list_card = f'<div class="card">{"".join(rows)}</div>'
    else:
        list_card = ('<div class="card">'
                     '<h2>No domains yet</h2>'
                     '<p class="body">Register your first name below. It becomes '
                     f'resolvable on the .{config.ZONE} network straight away.</p>'
                     '</div>')

    at_limit = used >= config.MAX_DOMAINS
    add = f"""
<div class="addcard">
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
    <h3>DOMAINS</h3>

    <div class="tiles">
      {tile(f"{used}/{config.MAX_DOMAINS}", "domains")}
      {tile(total_records, plural(total_records, "record"))}
      {tile(total_queries, "queries (14d)")}
    </div>

    <h3>YOUR DOMAINS</h3>
    {list_card}
    {add}
    <p class="msg" id="msg"></p>

    </div>"""
    body, shell_script = shell("domains", token, domains, content, art=True)

    drops = "\n".join(f'bind({lua_str(d["name"])})' for d in domains)
    script = shell_script + "\n" + CHARTS + "\n" + "\n".join(icon_script) + f"""
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
"""
    return page(f"{BRAND} - domains", body, script)


def axis(labels: list[str], rng: str, cls: str = "trendlabs") -> str:
    """A chart's x axis: first bucket, middle bucket, and the one running now."""
    latest = "Now" if analytics.spec(rng)[3] < analytics.DAY else "Today"
    return f"""<div class="{cls}">
        <p class="trendlab">{esc(labels[0])}</p>
        <p class="trendlab">{esc(labels[len(labels) // 2])}</p>
        <p class="trendlab">{latest}</p>
      </div>"""


def analytics_page(token: str, domains: list[dict], series: list[int],
                   labels: list[str], per_domain: list[dict],
                   rng: str = analytics.DEFAULT_RANGE) -> str:
    """per_domain is one dict per domain: {"name", "series", "total"}, all over
    the selected window, in the same order as domains."""
    if not domains:
        content = """
    <div class="page">
    <h3>ANALYTICS</h3>
    <div class="card">
      <h2>No domains yet</h2>
      <p class="body">Query counts land here once you have registered a domain
      and something has looked it up. Register one from the Domains tab.</p>
    </div>
    </div>"""
        body, script = shell("analytics", token, domains, content, art=True)
        return page(f"{BRAND} - analytics", body, script)

    _, short, window, _, buckets, unit = analytics.spec(rng)
    total = sum(series)
    avg = round(total / buckets)
    busiest = max(per_domain, key=lambda d: d["total"])
    busiest_name = _fit(busiest["name"], 13) if busiest["total"] > 0 else "-"

    picker, script = range_pills(rng, "/analytics", token)
    rows = []
    for i, d in enumerate(per_domain, 1):
        rows.append(f"""
    <div class="spark" id="spk-{i}">
      <p class="sparkname">{esc(_fit(d['name'], 20))}</p>
      <canvas class="sparkcv" id="chart-dom-{i}"></canvas>
      <p class="sparktot">{d['total']} / {esc(short)}</p>
      <img class="sparkchev" src="{icon_src('chevron-right', '#6f6f7c')}">
    </div>""")
        if i < len(per_domain):
            rows.append('    <div class="hair"></div>')
        script.append(f'chartLine("chart-dom-{i}", {lua_arr(d["series"])}, "#8b6ef0")')
        target = f"/analytics/{d['name']}?t={token}&r={rng}"
        script.append(f'link("spk-{i}", {lua_str(target)})')

    content = f"""
    <div class="page">
    <h3>ANALYTICS</h3>

    {picker}

    <div class="tiles">
      {tile(total, "queries")}
      {tile(busiest_name, "busiest domain")}
      {tile(avg, f"avg per {unit}")}
    </div>

    <div class="trend">
      <div class="trendhead">
        <p class="trendttl">{esc(window)} across your domains</p>
        <p class="trendtot">{total}</p>
      </div>
      <canvas class="trendcv" id="chart-all"></canvas>
      {axis(labels, rng)}
    </div>

    <h3>BY DOMAIN</h3>
    <div class="card">
{''.join(rows)}
    </div>
    </div>"""

    body, shell_script = shell("analytics", token, domains, content, art=True)
    script.insert(0, f'chartLine("chart-all", {lua_arr(series)}, "#ba8cf5")')
    return page(f"{BRAND} - analytics", body,
                shell_script + "\n" + CHARTS + "\n" + "\n".join(script))


def domain_analytics_page(token: str, domain: str, domains: list[dict], series: list[int],
                          labels: list[str], rng: str, all_time: int) -> str:
    """One domain's own analytics over the selected window."""
    _, _, window, _, buckets, unit = analytics.spec(rng)
    picker, script = range_pills(rng, f"/analytics/{domain}", token)

    total = sum(series)
    avg = round(total / buckets)
    peak = max(series)
    peak_text = (f"{labels[series.index(peak)]}, {peak} {_queries(peak)}"
                 if peak else "Nothing yet")
    active = sum(1 for v in series if v)

    content = f"""
    <div class="page">
    <h3>{esc(domain)}</h3>

    <div class="backrow" id="back-ana">
      <img class="backico" src="{icon_src('chevron-left', '#8b8b96')}">
      <p class="backlbl">All analytics</p>
    </div>

    {picker}

    <div class="tiles">
      {tile(total, "queries")}
      {tile(avg, f"avg per {unit}")}
    </div>

    <div class="trend">
      <div class="trendhead">
        <p class="trendttl">{esc(window)} for {esc(_fit(domain, 26))}</p>
        <p class="trendtot">{total}</p>
      </div>
      <canvas class="trendcv" id="chart-dom"></canvas>
      {axis(labels, rng)}
    </div>

    <h3>BY {unit.upper()}</h3>
    <div class="card">
      <canvas class="barcv" id="chart-days"></canvas>
      {axis(labels, rng)}
    </div>

    <h3>ACTIVITY</h3>
    <div class="card">
      <div class="kv"><p class="ak">Busiest {esc(unit)}</p><p class="v">{esc(peak_text)}</p></div>
      <div class="kv"><p class="ak">Coverage</p><p class="v">{active} of {buckets} with any traffic</p></div>
      <div class="kv"><p class="ak">All time</p><p class="v">{all_time} {_queries(all_time)}</p></div>
    </div>

    <div class="card">
      <div class="lrow" id="manage">
        <img class="lico" src="{icon_src('globe', '#8b8b96')}">
        <p class="lname">Manage records and certificate</p>
        <img class="lchev" src="{icon_src('chevron-right', '#6f6f7c')}">
      </div>
    </div>
    </div>"""

    body, shell_script = shell("analytics", token, domains, content, art=True)
    script += [
        f'link("back-ana", {lua_str(f"/analytics?t={token}&r={rng}")})',
        f'link("manage", {lua_str(f"/domain/{domain}?t={token}")})',
        f'chartLine("chart-dom", {lua_arr(series)}, "#ba8cf5")',
        f'chartBars("chart-days", {lua_arr(series)}, {lua_strs(labels)}, "#8b6ef0")',
    ]
    return page(f"{BRAND} - {domain} analytics", body,
                shell_script + "\n" + CHARTS + "\n" + "\n".join(script))


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


def domain_page(token: str, domain: str, domains: list[dict], records: list[dict],
                cert: dict | None, ca_note: str | None) -> str:
    if ca_note:
        cert_body = f'<p class="bad">Certificates are unavailable: {esc(ca_note)}.</p>'
    elif cert:
        cert_body = f"""
<div class="certhead">
  <img class="certicon" src="{icon_src('shield-check', '#4ade80')}">
  <p class="certttl">Certificate issued</p>
</div>
<div class="kv"><p class="k">Issued</p><p class="v">{esc(cert['issued_at'].strftime('%Y-%m-%d %H:%M UTC'))}</p></div>
<div class="kv"><p class="k">Serial</p><p class="v">{esc(cert['serial'][:16])}</p></div>
<div class="kv"><p class="k">Names</p><p class="v">{esc(cert['sans'])}</p></div>
<div class="kv"><p class="k">Expires</p><p class="v">{esc(cert['not_after'])}</p></div>
<div class="certlinks">
  <a href="/cert/{esc(domain)}/cert?t={esc(token)}">Download certificate</a>
  <a href="/cert/{esc(domain)}/key?t={esc(token)}">Download private key</a>
</div>
<button class="btn" id="issue">Re-issue</button>"""
    else:
        cert_body = f"""
<div class="certhead">
  <img class="certicon" src="{icon_src('shield-question-mark', '#74747f')}">
  <p class="certttl">No certificate yet</p>
</div>
<p class="certdesc">Issuing one gives you a leaf for {esc(domain)} and *.{esc(domain)},
signed by the StarWeb root CA, which is enough to serve star:// to any StarWeb
client on the network.</p>
<button class="btn" id="issue">Issue certificate</button>"""

    content = f"""
    <div class="page">
    <h3>{esc(domain)}</h3>

    <div class="backrow" id="back-all">
      <img class="backico" src="{icon_src('chevron-left', '#8b8b96')}">
      <p class="backlbl">All domains</p>
    </div>

    <div class="tiles">
      {tile(f"{len(records)}/{config.MAX_RECORDS}", "records")}
    </div>

    <div class="section">
      <h2>Records</h2>
      {_record_rows(records)}
      <p class="hint">"@" means the domain itself, as a name or as a CNAME target;
      a CNAME target with no dot in it is relative to {esc(domain)}.</p>
    </div>

    <div class="addcard">
      <h2>Add a record</h2>
      <div class="cols">
        <p class="ac-name">NAME</p>
        <p class="ac-type">TYPE</p>
        <p class="ac-val">VALUE</p>
        <p class="ac-ttl">TTL</p>
      </div>
      <div class="addrow">
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
    body, shell_script = shell("domains", token, domains, content, art=True)

    binds = "\n".join(f'bind({lua_str(str(r["_id"]))})' for r in records)
    back_target = f"/domains?t={token}"
    script = shell_script + f"""
link("back-all", {lua_str(back_target)})

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


def _search_rows(items: list[tuple[str, str, str]], token: str, prefix: str) -> tuple[str, list[str]]:
    """items: (label, icon, path). Same lrow/hair idiom as the account home
    columns, since these are the same thing: a navigable, icon-led list."""
    rows, script = [], []
    for i, (label, icon, path) in enumerate(items, 1):
        rid = f"{prefix}-{i}"
        rows.append(f"""
<div class="lrow" id="{rid}">
  <img class="lico" src="{icon_src(icon, '#8b8b96')}">
  <p class="lname">{esc(label)}</p>
  <img class="lchev" src="{icon_src('chevron-right', '#6f6f7c')}">
</div>""")
        if i < len(items):
            rows.append('<div class="hair"></div>')
        script.append(f'link("{rid}", {lua_str(f"{path}?t={token}")})')
    return "".join(rows), script


def search_page(token: str, domains: list[dict], q: str) -> str:
    """Quick search results: pages, domains and per-domain analytics, the same
    three things the sidebar tabs reach. There is no live-as-you-type result
    list here (see the click handlers in shell()/home_page() for why), so this
    is a real results page, reached by clicking the search icon."""
    query = q.strip()
    qlow = query.lower()

    page_items = [(label, icon, path) for _, label, icon, path in TABS
                 if qlow in label.lower()] if query else []
    matched = [d["name"] for d in domains if qlow in d["name"].lower()] if query else []
    domain_items = [(name, "globe", f"/domain/{name}") for name in matched]
    analytics_items = [(f"Analytics for {name}", "chart-pie", f"/analytics/{name}")
                       for name in matched]

    sections, script = [], []
    for title, items, prefix in (("PAGES", page_items, "sp"),
                                 ("DOMAINS", domain_items, "sd"),
                                 ("ANALYTICS", analytics_items, "sa")):
        if not items:
            continue
        rows, rscript = _search_rows(items, token, prefix)
        script += rscript
        sections.append(f'<h3>{title}</h3>\n<div class="card">{rows}</div>')

    if not query:
        status = "Type something, then press the search icon."
    elif not sections:
        status = f'No results for "{esc(query)}".'
    else:
        status = f'Results for "{esc(query)}"'

    content = f"""
    <div class="page">
    <h3>SEARCH</h3>
    <p class="body">{status}</p>
    {''.join(sections)}
    </div>"""

    body, shell_script = shell("", token, domains, content, art=True)
    return page(f"{BRAND} - search", body, shell_script + "\n" + "\n".join(script))


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
