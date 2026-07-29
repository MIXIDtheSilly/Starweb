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

-- Most of what a cell's intensity depends on is geometry, and geometry does not
-- move: where the cell sits in the cog, or how far it is from the nearest run of
-- the line. Only the shine, and the cog's rotation, change between frames. So
-- each shape answers twice: prep() once per canvas size, into a flat array, and
-- lit() per frame off what prep left behind. That takes the square roots, the
-- arctangents and the walk over every line segment out of the per-frame loop
-- entirely, and the picture is the same one either way.

-- Brightness ramps along a direction across the shape rather than out from its
-- centre, which is what gives the artwork its lit-from-one-side look. The
-- shine is a soft band riding that same axis, swept back and forth by sin.
local function cogPrep(px, py, s, c, i)
    local dx = px - s.cx
    local dy = py - s.cy
    local r2 = dx * dx + dy * dy
    if r2 > s.rad2 then c[i] = -1; return end
    c[i]     = math.sqrt(r2)
    c[i + 1] = math.atan(dy, dx)
    c[i + 2] = (dx * s.gdx + dy * s.gdy) / s.rad * 0.5 + 0.5
end

local function cogLit(s, c, i)
    local r = c[i]
    if r < 0 then return 0 end

    local ang = (c[i + 1] + s.rot) % TAU
    local edge = COG[math.floor(ang * s.cogScale) + 1] * s.rad
    if r > edge then return 0 end

    local g = c[i + 2]
    local inten = 0.02 + 0.88 * g + 0.38 * falloff(g - s.shine, 6.0)

    local rim = (edge - r) / 20.0
    if rim < 1 then inten = inten * (0.15 + 0.85 * rim) end
    return inten
end

local function linePrep(px, py, s, c, i)
    local list = s.buckets[math.floor(py / BUCKET)]
    if not list then c[i] = -1; return end

    local best = 1e18
    local along = 0
    for k = 1, #list do
        local g = list[k]
        local d2 = segDist2(px, py, g[1], g[2], g[3], g[4])
        if d2 < best then best = d2; along = g[5] end
    end
    if best > s.half2 then c[i] = -1; return end

    c[i]     = math.sqrt(best) / s.half
    c[i + 1] = along
end

local function lineLit(s, c, i)
    local u = c[i]
    if u < 0 then return 0 end

    local along = c[i + 1]
    local inten = 0.04 + 0.80 * along + 0.38 * falloff(along - s.shine, 7.0)

    -- Thin the dots out across the stroke so the edges stay soft.
    local rim = (1.0 - u) * 3.0
    if rim < 1 then inten = inten * (0.12 + 0.88 * rim) end
    return inten
end

function makeCog(cx, cy, rad, spin, phase, gang)
    gang = gang or -2.2
    return { prep = cogPrep, lit = cogLit, stride = 3,
             cx = cx, cy = cy, rad = rad, rad2 = rad * rad,
             spin = spin, phase = phase, rate = 0.00075,
             cogScale = COG_N / TAU,
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

    return { prep = linePrep, lit = lineLit, stride = 2,
             buckets = buckets, half = half,
             half2 = half * half, phase = phase, rate = 0.0009 }
end

function makeScene(id, build)
    local cv = document.getElementById(id)
    local ctx = cv:getContext("2d")
    local shapes, sw, sh, cols, rows = nil, 0, 0, 0, 0

    local function draw(t)
        local W, H = cv.width, cv.height
        if W < 1 or H < 1 then requestAnimationFrame(draw); return end

        if W ~= sw or H ~= sh then
            shapes = build(W, H)
            sw, sh = W, H
            cols = math.floor(W / CELL)
            rows = math.floor(H / CELL)
            for k = 1, #shapes do
                local s = shapes[k]
                local c, i = {}, 1
                for gy = 0, rows do
                    local py = gy * CELL + 3
                    for gx = 0, cols do
                        s.prep(gx * CELL + 3, py, s, c, i)
                        i = i + s.stride
                    end
                end
                s.cells = c
            end
        end

        ctx:clearRect(0, 0, W, H)

        local n = #shapes
        local shift = math.floor(t * 0.004)
        -- The shine rides the whole shape at once, and the cog turns as a whole,
        -- so both are one value a frame rather than one per cell.
        for k = 1, n do
            local s = shapes[k]
            s.shine = 0.5 + 0.5 * math.sin(t * s.rate + s.phase)
            s.rot = s.spin and t * s.spin or 0
        end

        local i = 1
        for gy = 0, rows do
            local brow = (gy % 4) * 4
            for gx = 0, cols do
                local inten = 0
                for k = 1, n do
                    local s = shapes[k]
                    local v = s.lit(s, s.cells, (i - 1) * s.stride + 1)
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
                i = i + 1
            end
        end

        requestAnimationFrame(draw)
    end
    requestAnimationFrame(draw)
end
