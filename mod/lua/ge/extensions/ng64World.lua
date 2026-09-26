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

local statics = {}          -- { shape, visible, box = {x0,y0,z0,x1,y1,z1}, pos, c0, c1, c2, scale }
local shapes = {}           -- shape path -> parsed { tris } | false (unreadable) | "pending"
local queue = {}            -- shape paths waiting to be parsed
local parser                -- coroutine parsing queue[1]
local instanceCache = {}    -- key -> world triangles for one placed object

local function vec(v) return { v.x, v.y, v.z } end

local function daePath(shape)
  return (shape:gsub("%.cdae$", ".dae"))
end

function M.reset()
  statics, instanceCache, queue, parser = {}, {}, {}, nil
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
  return s
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
      shapes[id] = res
      parser = nil
    end
  end
  return #queue + (parser and 1 or 0)
end

-- place a shape's local triangles in the world (position, rotation columns, scale); a mirrored placement swaps two
-- corners so the triangles still face out
local function place(inst, tris)
  local out, n = {}, 0
  local p, c0, c1, c2, s = inst.pos, inst.c0, inst.c1, inst.c2, inst.scale
  local sx, sy, sz = s[1], s[2], s[3]
  local mirrored = sx * sy * sz < 0
  for i = 1, #tris, 3 do
    local x, y, z = tris[i] * sx, tris[i + 1] * sy, tris[i + 2] * sz
    out[n + 1] = p[1] + c0[1] * x + c1[1] * y + c2[1] * z
    out[n + 2] = p[2] + c0[2] * x + c1[2] * y + c2[2] * z
    out[n + 3] = p[3] + c0[3] * x + c1[3] * y + c2[3] * z
    n = n + 3
  end
  if mirrored then
    for t = 0, n - 9, 9 do
      for k = 1, 3 do out[t + 3 + k], out[t + 6 + k] = out[t + 6 + k], out[t + 3 + k] end
    end
  end
  return out
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

-- all triangles overlapping the square (cx +- r, cy +- r), within zr metres of cz vertically, from every shape that's
-- parsed; also how many it needs are still being parsed. Ready shapes are never held back by slow ones (West Coast
-- USA's island backdrop is a 164 MB file) - ask again once step() has caught up and send the fuller region.
function M.region(cx, cy, cz, r, zr)
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

  local out, n = {}, 0
  for _, inst in ipairs(insts) do
    local tris = shapeTris(inst.shape, inst.visible)
    if tris and #tris.tris > 0 then
      local w = instanceCache[inst.key]
      if not w then w = place(inst, tris.tris) instanceCache[inst.key] = w end
      for t = 1, #w, 9 do
        local ax, ay, az, bx, by, bz, qx, qy, qz = w[t], w[t + 1], w[t + 2], w[t + 3], w[t + 4], w[t + 5], w[t + 6], w[t + 7], w[t + 8]
        if math.max(ax, bx, qx) >= x0 and math.min(ax, bx, qx) <= x1 and math.max(ay, by, qy) >= y0 and math.min(ay, by, qy) <= y1
          and math.max(az, bz, qz) >= z0 and math.min(az, bz, qz) <= z1 then
          for k = 0, 8 do out[n + 1 + k] = w[t + k] end
          n = n + 9
        end
      end
    end
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
