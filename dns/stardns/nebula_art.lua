-- Rasterises the shapes in nebula_shapes.lua as an animated halftone.
-- Every frame each grid cell asks the shapes for an intensity, that intensity
-- is thresholded against an ordered (Bayer) matrix, and surviving cells are
-- drawn as a small square. Spinning the cog, sliding the shine along a shape
-- and crawling the threshold all happen inside that per-frame pass.

local TAU = math.pi * 2

local PAL = {
  "#1a1040", "#22164f", "#2a1c5e", "#33236d", "#3d2b7c", "#48358b", "#54409a",
  "#604ca9", "#6c59b8", "#7867c6", "#8376d4", "#8f85e0", "#9b94ea", "#a7a3f2",
  "#b5b2f8", "#c4c1fd",
}
local NPAL = 16

local BAYER = {
   0,  8,  2, 10,
  12,  4, 14,  6,
   3, 11,  1,  9,
  15,  7, 13,  5,
}

local CELL = 5
local DOT = 4
local BUCKET = 24

local function falloff(d, k)
    return 1.0 / (1.0 + k * d * d)
end

local function segDist2(px, py, x1, y1, x2, y2)
    local vx, vy = x2 - x1, y2 - y1
    local wx, wy = px - x1, py - y1
    local dd = vx * vx + vy * vy
    local tt = 0.0
    if dd > 0 then
        tt = (wx * vx + wy * vy) / dd
        if tt < 0 then tt = 0 elseif tt > 1 then tt = 1 end
    end
    local qx = x1 + vx * tt - px
    local qy = y1 + vy * tt - py
    return qx * qx + qy * qy
end

-- Brightness ramps along a direction across the shape rather than out from its
-- centre, which is what gives the artwork its lit-from-one-side look. The
-- shine is a soft band riding that same axis, swept back and forth by sin.
local function cogInten(px, py, s, t)
    local dx = px - s.cx
    local dy = py - s.cy
    local r2 = dx * dx + dy * dy
    if r2 > s.rad2 then return 0 end

    local r = math.sqrt(r2)
    local ang = (math.atan(dy, dx) + t * s.spin) % TAU
    local edge = COG[math.floor(ang / TAU * COG_N) + 1] * s.rad
    if r > edge then return 0 end

    local g = (dx * s.gdx + dy * s.gdy) / s.rad * 0.5 + 0.5
    local shine = 0.5 + 0.5 * math.sin(t * 0.00075 + s.phase)
    local inten = 0.02 + 0.88 * g + 0.38 * falloff(g - shine, 6.0)

    local rim = (edge - r) / 20.0
    if rim < 1 then inten = inten * (0.15 + 0.85 * rim) end
    return inten
end

local function lineInten(px, py, s, t)
    local list = s.buckets[math.floor(py / BUCKET)]
    if not list then return 0 end

    local best = 1e18
    local along = 0
    for i = 1, #list do
        local g = list[i]
        local d2 = segDist2(px, py, g[1], g[2], g[3], g[4])
        if d2 < best then best = d2; along = g[5] end
    end
    if best > s.half2 then return 0 end

    local u = math.sqrt(best) / s.half
    local shine = 0.5 + 0.5 * math.sin(t * 0.0009 + s.phase)
    local inten = 0.04 + 0.80 * along + 0.38 * falloff(along - shine, 7.0)

    -- Thin the dots out across the stroke so the edges stay soft.
    local rim = (1.0 - u) * 3.0
    if rim < 1 then inten = inten * (0.12 + 0.88 * rim) end
    return inten
end

function makeCog(cx, cy, rad, spin, phase, gang)
    gang = gang or -2.2
    return { f = cogInten, cx = cx, cy = cy, rad = rad, rad2 = rad * rad,
             spin = spin, phase = phase,
             gdx = math.cos(gang), gdy = math.sin(gang) }
end

-- Segments are bucketed by row so a cell only tests the few that can reach it.
-- `weight` scales the stroke off the width the shape was drawn with, for a
-- placement that wants the line heavier than the artwork specifies.
function makeLine(L, ox, oy, scale, phase, weight)
    local n = #L.x
    local half = L.half * scale * (weight or 1.0)
    local buckets = {}

    for i = 1, n - 1 do
        local g = { ox + L.x[i] * scale,     oy + L.y[i] * scale,
                    ox + L.x[i + 1] * scale, oy + L.y[i + 1] * scale,
                    (i - 1) / (n - 2) }
        local lo = math.floor(((g[2] < g[4] and g[2] or g[4]) - half) / BUCKET)
        local hi = math.floor(((g[2] > g[4] and g[2] or g[4]) + half) / BUCKET)
        for b = lo, hi do
            local t = buckets[b]
            if not t then t = {}; buckets[b] = t end
            t[#t + 1] = g
        end
    end

    return { f = lineInten, buckets = buckets, half = half,
             half2 = half * half, phase = phase }
end

function makeScene(id, build)
    local cv = document.getElementById(id)
    local ctx = cv:getContext("2d")
    local shapes, sw, sh = nil, 0, 0

    local function draw(t)
        local W, H = cv.width, cv.height
        if W < 1 or H < 1 then requestAnimationFrame(draw); return end
        if W ~= sw or H ~= sh then shapes = build(W, H); sw, sh = W, H end

        ctx:clearRect(0, 0, W, H)

        local n = #shapes
        local shift = math.floor(t * 0.004)
        local cols = math.floor(W / CELL)
        local rows = math.floor(H / CELL)

        for gy = 0, rows do
            local py = gy * CELL + 3
            local brow = (gy % 4) * 4
            for gx = 0, cols do
                local px = gx * CELL + 3
                local inten = 0
                for k = 1, n do
                    local s = shapes[k]
                    local v = s.f(px, py, s, t)
                    if v > inten then inten = v end
                end
                if inten > 0.04 then
                    local th = (BAYER[brow + ((gx + shift) % 4) + 1] + 0.5) / 16.0
                    if inten > th then
                        local lvl = math.floor(inten * NPAL) + 1
                        if lvl > NPAL then lvl = NPAL end
                        ctx.fillStyle = PAL[lvl]
                        ctx:fillRect(gx * CELL, gy * CELL, DOT, DOT)
                    end
                end
            end
        end

        requestAnimationFrame(draw)
    end
    requestAnimationFrame(draw)
end
