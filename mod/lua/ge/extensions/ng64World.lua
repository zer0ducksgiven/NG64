-- NG64: the world's real collision triangles around Mario.
--
-- Every colliding TSStatic (and forest item: trees, rocks) is placed from its shape's collision mesh (ng64Dae),
-- instead of guessing the world from downward raycasts - which can't see walls, overhangs, thin parts or anything
-- with more than one level, and was why Mario clipped through things he climbed.
--
-- index()            once per level: every colliding static's world box
-- region(c, r)       triangles (flat bng world xyz * 3 per triangle) of everything overlapping a square around c;
--                    nil while shapes it needs are still being parsed (see step())
-- step(budgetSec)    parses queued shapes, spread over frames (a few big collision meshes take seconds)
local M = {}
local dae = require("ge/extensions/ng64Dae")
local ffi = require("ffi")

-- Triangles live in ffi float arrays, not Lua tables: Lua's garbage collector scans every table it keeps, and the
-- millions of numbers in the parsed and placed shapes (and a fresh region table every few metres) made it stall the
-- game for tens of milliseconds every few seconds while Mario ran around a detailed map.
local CHUNK_TRIS = 200                  -- one chunk = one 'O' packet (200 * 36 bytes < LuaSocket's 8 KB)
local CACHE_FLOATS = 24 * 1000 * 1000   -- placed triangles kept for reuse (~96 MB); least recently used go first

local statics = {}          -- { shape, visible, box = {x0,y0,z0,x1,y1,z1}, pos, c0, c1, c2, scale }
local shapes = {}           -- shape path -> parsed { tris } | false (unreadable) | "pending"
local queue = {}            -- shape paths waiting to be parsed
local parser                -- coroutine parsing queue[1]
local instanceCache = {}    -- key -> { w = float[n], n = floats, used = clock } world triangles for one placed object
local cachedFloats = 0

local function vec(v) return { v.x, v.y, v.z } end

local function daePath(shape)
  return (shape:gsub("%.cdae$", ".dae"))
end

function M.reset()
  statics, instanceCache, queue, parser, cachedFloats = {}, {}, {}, nil, 0
end

function M.index()
  M.reset()
  local names = scenetree.findClassObjects("TSStatic") or {}
  for _, n in ipairs(names) do
    local o = scenetree.findObject(n)
    if o then
      local ct = o:getField("collisionType", 0)
      local shape = o.shapeName
      if ct ~= "None" and shape and shape ~= "" then
        local b = o:getWorldBox()
        local m = o:getTransform()
        local mn, mx = b.minExtents, b.maxExtents
        statics[#statics + 1] = {
          key = "s" .. o:getId(), shape = daePath(shape), visible = (ct ~= "Collision Mesh"),
          box = { mn.x, mn.y, mn.z, mx.x, mx.y, mx.z },
          pos = vec(m:getColumn(3)), c0 = vec(m:getColumn(0)), c1 = vec(m:getColumn(1)), c2 = vec(m:getColumn(2)),
          scale = vec(o:getScale()),
        }
      end
    end
  end
  return #statics
end

local function shapeTris(path, visible)
  local id = path .. (visible and "|v" or "")
  local s = shapes[id]
  if s == nil then
    shapes[id] = "pending"
    queue[#queue + 1] = { id = id, path = path, visible = visible }
    return nil
  end
  if s == "pending" then return nil end
  return s    -- { tris = float[n], n } or false
end

function M.step(budget)
  local t0 = os.clock()
  while (#queue > 0 or parser) and os.clock() - t0 < budget do
    if not parser then
      local job = table.remove(queue, 1)
      parser = coroutine.create(function()
        local text = readFile(job.path)
        if not text then return job.id, false end
        dae.cooperative = true
        local ok, res = pcall(dae.parse, text, job.visible and "visibleHigh" or "collision")
        dae.cooperative = false
        return job.id, ok and res or false
      end)
    end
    local ok, id, res = coroutine.resume(parser)
    if not ok then
      log("W", "ng64", "collision mesh parse failed: " .. tostring(id))
      parser = nil
    elseif coroutine.status(parser) == "dead" then
      if res then
        local t = res.tris
        local n = #t
        local a = ffi.new("float[?]", math.max(n, 1))
        for i = 1, n do a[i - 1] = t[i] end
        res = { tris = a, n = n }
      end
      shapes[id] = res
      parser = nil
    end
  end
  return #queue + (parser and 1 or 0)
end

-- place a shape's local triangles in the world (position, rotation columns, scale); a mirrored placement swaps two
-- corners so the triangles still face out
local function place(inst, shape)
  local tris, n = shape.tris, shape.n
  local out = ffi.new("float[?]", math.max(n, 1))
  local p, c0, c1, c2, s = inst.pos, inst.c0, inst.c1, inst.c2, inst.scale
  local sx, sy, sz = s[1], s[2], s[3]
  local mirrored = sx * sy * sz < 0
  for i = 0, n - 3, 3 do
    local x, y, z = tris[i] * sx, tris[i + 1] * sy, tris[i + 2] * sz
    out[i] = p[1] + c0[1] * x + c1[1] * y + c2[1] * z
    out[i + 1] = p[2] + c0[2] * x + c1[2] * y + c2[2] * z
    out[i + 2] = p[3] + c0[3] * x + c1[3] * y + c2[3] * z
  end
  if mirrored then
    for t = 0, n - 9, 9 do
      for k = 0, 2 do out[t + 3 + k], out[t + 6 + k] = out[t + 6 + k], out[t + 3 + k] end
    end
  end
  return out
end

local function placed(inst, shape)
  local c = instanceCache[inst.key]
  if not c then
    c = { w = place(inst, shape), n = shape.n }
    instanceCache[inst.key] = c
    cachedFloats = cachedFloats + c.n
    if cachedFloats > CACHE_FLOATS then
      -- forget the least recently used placements (not this one) until back under the limit
      local list = {}
      for k, v in pairs(instanceCache) do if v ~= c then list[#list + 1] = { k, v.used or 0 } end end
      table.sort(list, function(x, y) return x[2] < y[2] end)
      for _, e in ipairs(list) do
        if cachedFloats <= CACHE_FLOATS * 0.75 then break end
        cachedFloats = cachedFloats - instanceCache[e[1]].n
        instanceCache[e[1]] = nil
      end
    end
  end
  c.used = os.clock()
  return c
end

local function forestItems(cx, cy, cz, r)
  local out = {}
  local forests = scenetree.findClassObjects("Forest") or {}
  for _, fn in ipairs(forests) do
    local f = scenetree.findObject(fn)
    local data = f and f.getData and f:getData()
    if data then
      local items = data:getItemsCircle(vec3(cx, cy, cz), r * 1.42) or {}
      for _, it in ipairs(items) do
        local d = it:getData()
        local shape = d and d.shapeFile
        if shape and shape ~= "" then
          local m = it:getTransform()
          local sc = it:getScale()
          local s = type(sc) == "number" and { sc, sc, sc } or vec(sc)
          out[#out + 1] = {
            key = "f" .. tostring(it:getKey()), shape = daePath(shape), visible = false,
            pos = vec(m:getColumn(3)), c0 = vec(m:getColumn(0)), c1 = vec(m:getColumn(1)), c2 = vec(m:getColumn(2)),
            scale = s,
          }
        end
      end
    end
  end
  return out
end

-- Reused output: chunks[i] = float[CHUNK_TRIS * 9], filled front to back by regionChunks
local chunks = {}

-- all triangles overlapping the box (cx +- r, cy +- r, cz +- zr), from every shape that's parsed, written into reused
-- chunk buffers: returns chunks, triangle count, and how many shapes it needs are still being parsed. Ready shapes
-- are never held back by slow ones (West Coast USA's island backdrop is a 164 MB file) - ask again once step() has
-- caught up and send the fuller region.
function M.regionChunks(cx, cy, cz, r, zr)
  local x0, y0, x1, y1, z0, z1 = cx - r, cy - r, cx + r, cy + r, cz - zr, cz + zr
  local insts = {}
  for _, st in ipairs(statics) do
    local b = st.box
    if b[1] <= x1 and b[4] >= x0 and b[2] <= y1 and b[5] >= y0 and b[3] <= z1 and b[6] >= z0 then insts[#insts + 1] = st end
  end
  for _, it in ipairs(forestItems(cx, cy, cz, r)) do insts[#insts + 1] = it end

  local pending = 0
  for _, inst in ipairs(insts) do
    if not shapeTris(inst.shape, inst.visible) then pending = pending + 1 end
  end

  local nt, ci, fill = 0, 1, 0
  local cur = chunks[1]
  if not cur then cur = ffi.new("float[?]", CHUNK_TRIS * 9) chunks[1] = cur end
  local mn, mx = math.min, math.max
  for _, inst in ipairs(insts) do
    local shape = shapeTris(inst.shape, inst.visible)
    if shape and shape.n > 0 then
      local w, n = placed(inst, shape).w, shape.n
      for t = 0, n - 9, 9 do
        local ax, ay, az, bx, by, bz, qx, qy, qz = w[t], w[t + 1], w[t + 2], w[t + 3], w[t + 4], w[t + 5], w[t + 6], w[t + 7], w[t + 8]
        if mx(ax, bx, qx) >= x0 and mn(ax, bx, qx) <= x1 and mx(ay, by, qy) >= y0 and mn(ay, by, qy) <= y1
          and mx(az, bz, qz) >= z0 and mn(az, bz, qz) <= z1 then
          if fill == CHUNK_TRIS then
            ci, fill = ci + 1, 0
            cur = chunks[ci]
            if not cur then cur = ffi.new("float[?]", CHUNK_TRIS * 9) chunks[ci] = cur end
          end
          local o = fill * 9
          cur[o], cur[o + 1], cur[o + 2], cur[o + 3], cur[o + 4], cur[o + 5], cur[o + 6], cur[o + 7], cur[o + 8] = ax, ay, az, bx, by, bz, qx, qy, qz
          fill = fill + 1
          nt = nt + 1
        end
      end
    end
  end
  return chunks, nt, pending
end

M.CHUNK_TRIS = CHUNK_TRIS

-- Map cells: the world around Mario is streamed as fixed CELL x CELL metre squares, each sent once while he's near
-- (see ng64.lua). A big placed object's triangles are bucketed by cell once, so a cell only visits its own.
local CELL = 16
M.CELL = CELL
local BUCKET_MIN_TRIS = 400    -- smaller objects are just scanned
local BUCKET_MAX_SPAN = 8      -- a triangle wider than this many cells (a huge ground plate) goes in every cell's scan

local function cellKey(ix, iy) return ix * 65536 + iy end

local function buckets(c)
  if c.buckets then return c.buckets end
  local w, n = c.w, c.n
  local counts, order, big = {}, {}, {}
  local floor, mn, mx = math.floor, math.min, math.max
  for pass = 1, 2 do
    local fillAt = {}
    if pass == 2 then
      local total = 0
      for _, k in ipairs(order) do c.start[k] = total fillAt[k] = total total = total + counts[k] end
      c.refs = ffi.new("int32_t[?]", math.max(total, 1))
    else
      c.start = {}
    end
    for t = 0, n - 9, 9 do
      local ix0 = floor(mn(w[t], w[t + 3], w[t + 6]) / CELL)
      local ix1 = floor(mx(w[t], w[t + 3], w[t + 6]) / CELL)
      local iy0 = floor(mn(w[t + 1], w[t + 4], w[t + 7]) / CELL)
      local iy1 = floor(mx(w[t + 1], w[t + 4], w[t + 7]) / CELL)
      if not (ix1 - ix0 <= BUCKET_MAX_SPAN and iy1 - iy0 <= BUCKET_MAX_SPAN) then   -- also NaN
        if pass == 1 then big[#big + 1] = t end
        ix1 = ix0 - 1
      end
      for ix = ix0, ix1 do
        for iy = iy0, iy1 do
          local k = cellKey(ix, iy)
          if pass == 1 then
            if not counts[k] then counts[k] = 0 order[#order + 1] = k end
            counts[k] = counts[k] + 1
          else
            c.refs[fillAt[k]] = t
            fillAt[k] = fillAt[k] + 1
          end
        end
      end
    end
  end
  c.counts = counts
  c.big = ffi.new("int32_t[?]", math.max(#big, 1))
  c.nBig = #big
  for i, t in ipairs(big) do c.big[i - 1] = t end
  c.buckets = true
  return true
end

-- every triangle overlapping cell (ix, iy) within zr metres of zc, into the reused chunk buffers: returns chunks,
-- triangle count, and how many shapes it needs are still being parsed. A triangle spanning several cells is in each
-- of them (the helper drops the duplicates).
function M.cellChunks(ix, iy, zc, zr)
  local x0, y0 = ix * CELL, iy * CELL
  local x1, y1, z0, z1 = x0 + CELL, y0 + CELL, zc - zr, zc + zr
  local cx, cy = x0 + CELL * 0.5, y0 + CELL * 0.5
  local insts = {}
  for _, st in ipairs(statics) do
    local b = st.box
    if b[1] <= x1 and b[4] >= x0 and b[2] <= y1 and b[5] >= y0 and b[3] <= z1 and b[6] >= z0 then insts[#insts + 1] = st end
  end
  for _, it in ipairs(forestItems(cx, cy, zc, CELL * 0.5)) do insts[#insts + 1] = it end

  local pending = 0
  local nt, ci, fill = 0, 1, 0
  local cur = chunks[1]
  if not cur then cur = ffi.new("float[?]", CHUNK_TRIS * 9) chunks[1] = cur end
  local mn, mx = math.min, math.max
  local key = cellKey(ix, iy)
  local function take(w, t)
    local ax, ay, az, bx, by, bz, qx, qy, qz = w[t], w[t + 1], w[t + 2], w[t + 3], w[t + 4], w[t + 5], w[t + 6], w[t + 7], w[t + 8]
    if mx(ax, bx, qx) >= x0 and mn(ax, bx, qx) <= x1 and mx(ay, by, qy) >= y0 and mn(ay, by, qy) <= y1
      and mx(az, bz, qz) >= z0 and mn(az, bz, qz) <= z1 then
      if fill == CHUNK_TRIS then
        ci, fill = ci + 1, 0
        cur = chunks[ci]
        if not cur then cur = ffi.new("float[?]", CHUNK_TRIS * 9) chunks[ci] = cur end
      end
      local o = fill * 9
      cur[o], cur[o + 1], cur[o + 2], cur[o + 3], cur[o + 4], cur[o + 5], cur[o + 6], cur[o + 7], cur[o + 8] = ax, ay, az, bx, by, bz, qx, qy, qz
      fill = fill + 1
      nt = nt + 1
    end
  end
  for _, inst in ipairs(insts) do
    local shape = shapeTris(inst.shape, inst.visible)
    if not shape then
      if shape == nil then pending = pending + 1 end
    elseif shape.n > 0 then
      local c = placed(inst, shape)
      local w = c.w
      if shape.n >= BUCKET_MIN_TRIS * 9 then
        buckets(c)
        local st, cnt = c.start[key], c.counts[key]
        if st then
          local refs = c.refs
          for i = st, st + cnt - 1 do take(w, refs[i]) end
        end
        local big = c.big
        for i = 0, c.nBig - 1 do take(w, big[i]) end
      else
        for t = 0, shape.n - 9, 9 do take(w, t) end
      end
    end
  end
  return chunks, nt, pending
end

-- the same as a flat Lua table { x1,y1,z1, ... } (tests)
function M.region(cx, cy, cz, r, zr)
  local cs, nt, pending = M.regionChunks(cx, cy, cz, r, zr)
  local out = {}
  for i = 0, nt - 1 do
    local c = cs[math.floor(i / CHUNK_TRIS) + 1]
    local o = (i % CHUNK_TRIS) * 9
    for k = 0, 8 do out[i * 9 + k + 1] = c[o + k] end
  end
  return out, pending
end

-- queue (nearest first) the shapes of everything within r of (cx, cy) that hasn't been parsed yet, so it's ready
-- before Mario gets there
function M.prefetch(cx, cy, r)
  local want = {}
  for _, st in ipairs(statics) do
    local b = st.box
    local dx = math.max(b[1] - cx, 0, cx - b[4])
    local dy = math.max(b[2] - cy, 0, cy - b[5])
    local d = math.sqrt(dx * dx + dy * dy)
    if d < r then
      local id = st.shape .. (st.visible and "|v" or "")
      if shapes[id] == nil then want[#want + 1] = { d, st } end
    end
  end
  table.sort(want, function(a, b) return a[1] < b[1] end)
  for _, w in ipairs(want) do shapeTris(w[2].shape, w[2].visible) end
  return #want
end

function M.stats()
  local parsed, pending = 0, 0
  for _, s in pairs(shapes) do if s == "pending" then pending = pending + 1 elseif s then parsed = parsed + 1 end end
  return { statics = #statics, shapesParsed = parsed, shapesPending = pending + #queue }
end

return M
