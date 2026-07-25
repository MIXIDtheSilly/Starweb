"""Decorative SVG shapes, flattened into numbers the page script can use.

The browser has no SVG support and no way to sample an image from script, so
the artwork in assets/ is reduced here to plain geometry and emitted as Lua
literals. The page then rasterises it per frame as a dithered halftone, which
is what makes the shapes spin and shimmer.

Two forms come out of this:

  * the cog becomes a radius-per-angle table. It is star-convex about its
    centre, so "is this point inside" is one table lookup, and spinning it is
    just an offset into that table.
  * each line becomes a sampled polyline. It is a stroked path, so a point is
    inside when it lies within half the stroke width of the curve.

Figma writes degenerate `V` commands between segments (a vertical lineto to
the y the pen is already on); they are parsed and discarded.
"""
import math
import re
from functools import lru_cache
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"

# Angular resolution of the cog table. 720 bins is a quarter degree apiece,
# finer than the dither grid can resolve at any size we draw it.
COG_BINS = 720

_NUM = re.compile(r"-?\d*\.?\d+(?:e-?\d+)?")


def _numbers(text: str) -> list[float]:
    return [float(n) for n in _NUM.findall(text)]


def _path_d(svg: str) -> str:
    return re.search(r'\sd="([^"]*)"', svg).group(1)


def _attr(svg: str, name: str, default: float = 0.0) -> float:
    m = re.search(rf'\s{name}="([^"]*)"', svg)
    return float(m.group(1)) if m else default


def _cubic(p0, p1, p2, p3, steps):
    """Sample one cubic Bezier, skipping t=0 so segments do not double up."""
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        a, b, c, d = u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


def flatten_path(d: str, steps: int = 24) -> list[tuple[float, float]]:
    """Walk an M/C/V/Z path into a polyline."""
    tokens = re.findall(r"[MCVZmcvz]|-?\d*\.?\d+(?:e-?\d+)?", d)
    pts: list[tuple[float, float]] = []
    cur = (0.0, 0.0)
    i = 0
    while i < len(tokens):
        cmd = tokens[i]
        i += 1
        if cmd in "Mm":
            cur = (float(tokens[i]), float(tokens[i + 1]))
            i += 2
            pts.append(cur)
        elif cmd in "Cc":
            p1 = (float(tokens[i]), float(tokens[i + 1]))
            p2 = (float(tokens[i + 2]), float(tokens[i + 3]))
            p3 = (float(tokens[i + 4]), float(tokens[i + 5]))
            i += 6
            pts.extend(_cubic(cur, p1, p2, p3, steps))
            cur = p3
        elif cmd in "Vv":
            # Figma's no-op separator; consume its argument and move on.
            i += 1
        elif cmd in "Zz":
            pass
    return pts


def cog_table(path: Path | None = None) -> dict:
    """Radius per angle bin for the cog, normalised to a unit outer radius."""
    svg = (path or ASSETS / "cog.svg").read_text()
    pts = flatten_path(_path_d(svg), steps=28)

    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)

    bins: list[float | None] = [None] * COG_BINS
    for x, y in pts:
        ang = math.atan2(y - cy, x - cx) % (2 * math.pi)
        r = math.hypot(x - cx, y - cy)
        b = int(ang / (2 * math.pi) * COG_BINS) % COG_BINS
        if bins[b] is None or r > bins[b]:
            bins[b] = r

    # Sampling leaves gaps between bins; bridge them from the nearest filled
    # neighbour on each side so the outline stays continuous.
    filled = [i for i, v in enumerate(bins) if v is not None]
    if not filled:
        raise ValueError("cog path produced no points")
    for i, v in enumerate(bins):
        if v is not None:
            continue
        lo = max((f for f in filled if f <= i), default=filled[-1] - COG_BINS)
        hi = min((f for f in filled if f >= i), default=filled[0] + COG_BINS)
        a, b = bins[lo % COG_BINS], bins[hi % COG_BINS]
        span = (hi - lo) or 1
        bins[i] = a + (b - a) * ((i - lo) / span)

    peak = max(bins)
    return {"r": [v / peak for v in bins], "peak": peak}


def line_polyline(name: str) -> dict:
    """Sampled centre line plus half its stroke width, in unit coordinates."""
    svg = (ASSETS / name).read_text()
    pts = flatten_path(_path_d(svg), steps=26)
    width = _attr(svg, "stroke-width", 93.0)
    w = _attr(svg, "width", 1.0)
    h = _attr(svg, "height", 1.0)

    scale = max(w, h)
    return {
        "pts": [(x / scale, y / scale) for x, y in pts],
        "half": (width / 2.0) / scale,
        "w": w / scale,
        "h": h / scale,
    }


def _fmt(v: float, places: int = 4) -> str:
    return f"{round(v, places):g}"


# Emitted as globals: the panel inlines this beside the drawing code, but the
# scratch page loads them as two separate <script>s, which do not share locals.
def lua_cog(var: str = "COG") -> str:
    t = cog_table()
    body = ",".join(_fmt(v) for v in t["r"])
    return f"{var} = {{{body}}}\n{var}_N = {len(t['r'])}\n"


def lua_line(var: str, name: str) -> str:
    d = line_polyline(name)
    xs = ",".join(_fmt(p[0]) for p in d["pts"])
    ys = ",".join(_fmt(p[1]) for p in d["pts"])
    return (f"{var} = {{ x = {{{xs}}}, y = {{{ys}}}, "
            f"half = {_fmt(d['half'])}, w = {_fmt(d['w'])}, h = {_fmt(d['h'])} }}\n")


# --- lucide icons -----------------------------------------------------------
#
# The decorative shapes above are single open curves written by Figma, and the
# reader for them only ever had to handle M/C. Icons are drawn by hand against a
# 24x24 grid and use the rest of the path language -- relative commands, H/V,
# elliptical arcs, closepath -- plus <circle> elements, so they get a full
# reader. Each icon flattens to a list of subpaths in grid units, which is what
# the page strokes; a closed subpath repeats its first point at the end so the
# renderer joins the seam.

ICONS = ASSETS / "icons"

# Every arc becomes a chord every ARC_STEP radians, every cubic CUBIC_STEPS
# segments. At the sizes an icon is drawn (16-20px) that is well under a pixel.
ARC_STEP = math.pi / 12.0
CUBIC_STEPS = 10

_PATH_NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
_PATH_TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
_ELEMENT = re.compile(r"<(path|circle|line)\b([^>]*)>")


def _arc_points(p0, rx, ry, rot_deg, large, sweep, p1):
    """SVG endpoint-parameterised elliptical arc, sampled. Follows F.6.5 of the
    spec: recover the centre and the angles, then walk the ellipse."""
    x0, y0 = p0
    x1, y1 = p1
    if rx == 0 or ry == 0 or (abs(x0 - x1) < 1e-12 and abs(y0 - y1) < 1e-12):
        return [p1]

    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rot_deg)
    cos_p, sin_p = math.cos(phi), math.sin(phi)

    dx2, dy2 = (x0 - x1) / 2.0, (y0 - y1) / 2.0
    xp = cos_p * dx2 + sin_p * dy2
    yp = -sin_p * dx2 + cos_p * dy2

    # An ellipse too small to span the endpoints is scaled up until it fits.
    lam = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry)
    if lam > 1.0:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s

    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    den = rx * rx * yp * yp + ry * ry * xp * xp
    factor = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        factor = -factor
    cxp = factor * rx * yp / ry
    cyp = -factor * ry * xp / rx

    cx = cos_p * cxp - sin_p * cyp + (x0 + x1) / 2.0
    cy = sin_p * cxp + cos_p * cyp + (y0 + y1) / 2.0

    theta0 = math.atan2((yp - cyp) / ry, (xp - cxp) / rx)
    theta1 = math.atan2((-yp - cyp) / ry, (-xp - cxp) / rx)
    sweep_angle = theta1 - theta0
    if sweep and sweep_angle < 0:
        sweep_angle += 2 * math.pi
    elif not sweep and sweep_angle > 0:
        sweep_angle -= 2 * math.pi

    steps = max(2, int(math.ceil(abs(sweep_angle) / ARC_STEP)))
    out = []
    for i in range(1, steps + 1):
        t = theta0 + sweep_angle * i / steps
        ct, st = math.cos(t), math.sin(t)
        out.append((cos_p * rx * ct - sin_p * ry * st + cx,
                    sin_p * rx * ct + cos_p * ry * st + cy))
    return out


def flatten_subpaths(d: str) -> list[list[tuple[float, float]]]:
    """Every subpath of a path, as a polyline. Handles the whole command set an
    icon uses; a repeated command letter is implied by extra arguments, and a
    lone M is followed by implicit linetos, both as the spec has it."""
    tokens = _PATH_TOKEN.findall(d)
    subs: list[list[tuple[float, float]]] = []
    cur_sub: list[tuple[float, float]] = []
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    prev_ctrl = None
    cmd = ""
    i = 0

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    while i < len(tokens):
        if _PATH_NUM.fullmatch(tokens[i]):
            # An argument where a command was expected repeats the last command,
            # except that a repeated moveto means lineto.
            if cmd in ("M", "m"):
                cmd = "L" if cmd == "M" else "l"
        else:
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                if cur_sub:
                    cur_sub.append(start)
                    subs.append(cur_sub)
                    cur_sub = []
                cur = start
                continue

        rel = cmd.islower()
        up = cmd.upper()

        if up == "M":
            x, y = num(), num()
            if rel:
                x, y = cur[0] + x, cur[1] + y
            if cur_sub:
                subs.append(cur_sub)
            cur = start = (x, y)
            cur_sub = [cur]
        elif up in ("L", "H", "V"):
            if up == "L":
                x, y = num(), num()
                if rel:
                    x, y = cur[0] + x, cur[1] + y
            elif up == "H":
                x = num()
                x = cur[0] + x if rel else x
                y = cur[1]
            else:
                y = num()
                y = cur[1] + y if rel else y
                x = cur[0]
            cur = (x, y)
            cur_sub.append(cur)
        elif up in ("C", "S"):
            if up == "C":
                c1 = (num(), num())
                c2 = (num(), num())
            else:
                # Smooth cubic: the first control point mirrors the last one.
                c1 = (2 * cur[0] - prev_ctrl[0], 2 * cur[1] - prev_ctrl[1]) if prev_ctrl else cur
                c2 = (num(), num())
                if rel:
                    c2 = (cur[0] + c2[0], cur[1] + c2[1])
            end = (num(), num())
            if rel:
                if up == "C":
                    c1 = (cur[0] + c1[0], cur[1] + c1[1])
                    c2 = (cur[0] + c2[0], cur[1] + c2[1])
                end = (cur[0] + end[0], cur[1] + end[1])
            cur_sub.extend(_cubic(cur, c1, c2, end, CUBIC_STEPS))
            prev_ctrl = c2
            cur = end
        elif up == "A":
            rx, ry, rot = num(), num(), num()
            large, sweep = num(), num()
            x, y = num(), num()
            if rel:
                x, y = cur[0] + x, cur[1] + y
            cur_sub.extend(_arc_points(cur, rx, ry, rot, large >= 0.5, sweep >= 0.5, (x, y)))
            cur = (x, y)
        else:
            raise ValueError(f"unsupported path command {cmd!r}")

        if up not in ("C", "S"):
            prev_ctrl = None

    if cur_sub:
        subs.append(cur_sub)
    return [s for s in subs if len(s) > 1]


def _circle_points(cx: float, cy: float, r: float, steps: int = 48):
    pts = [(cx + r * math.cos(2 * math.pi * i / steps),
            cy + r * math.sin(2 * math.pi * i / steps)) for i in range(steps)]
    pts.append(pts[0])
    return pts


def icon_subpaths(name: str) -> list[list[tuple[float, float]]]:
    """One lucide icon, as polylines on its own 24x24 grid."""
    svg = (ICONS / f"{name}.svg").read_text()
    out: list[list[tuple[float, float]]] = []
    for tag, attrs in _ELEMENT.findall(svg):
        if tag == "path":
            d = re.search(r'\bd="([^"]*)"', attrs)
            if d:
                out.extend(flatten_subpaths(d.group(1)))
        elif tag == "circle":
            def a(n, default=0.0):
                m = re.search(rf'\b{n}="([^"]*)"', attrs)
                return float(m.group(1)) if m else default
            out.append(_circle_points(a("cx"), a("cy"), a("r")))
        elif tag == "line":
            def a(n, default=0.0):
                m = re.search(rf'\b{n}="([^"]*)"', attrs)
                return float(m.group(1)) if m else default
            out.append([(a("x1"), a("y1")), (a("x2"), a("y2"))])
    return out


ICON_NAMES = ("house", "globe", "chart-pie", "chevron-right", "search", "user-round")


@lru_cache(maxsize=1)
def lua_icons(var: str = "ICON_PATHS") -> str:
    """Every icon as one Lua table: name -> list of flat {x,y,x,y,...} subpaths."""
    rows = []
    for name in ICON_NAMES:
        if not (ICONS / f"{name}.svg").exists():
            continue
        subs = []
        for sub in icon_subpaths(name):
            flat = ",".join(_fmt(v, 3) for p in sub for v in p)
            subs.append("{" + flat + "}")
        rows.append(f'  ["{name}"] = {{' + ",".join(subs) + "}")
    return var + " = {\n" + ",\n".join(rows) + "\n}\n"


@lru_cache(maxsize=1)
def lua_shapes() -> str:
    return (lua_cog()
            + lua_line("LINE_L", "line_left_top.svg")
            + lua_line("LINE_R", "line_right_bottom.svg")
            + lua_line("LOOP_B", "loop_bottom.svg")
            + lua_line("PFP_T", "pfp_line_top.svg"))


if __name__ == "__main__":
    print(lua_shapes())
