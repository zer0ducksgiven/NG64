-- NG64: SM64's pickups and enemies, drawn in BeamNG. The helper simulates them (ents.c) and sends the list ~30 times a second;
-- this draws each as a small procedural model (built once per slot and reused: a mesh that has been drawn costs for good, so
-- they're pooled, not created and deleted), applies what the cars do to them, and what they do to the cars, and turns the
-- helper's events into the HUD's coin / star counts, toasts and effects. Also the settings the NG64 settings app edits.
local M = {}

pcall(ffi.cdef, [[
#pragma pack(push, 1)
typedef struct { uint16_t id; uint8_t type; uint8_t state; float pos[3]; float yaw; float anim; float scale; } ng64v13_Ent;
typedef struct { uint8_t type; uint8_t kind; uint16_t id; float a, b, c, d; } ng64v13_EntEvent;
#pragma pack(pop)
]])
local ENT_SIZE = ffi.sizeof("ng64v13_Ent")
local entBuf = ffi.new("ng64v13_Ent")
local evBuf = ffi.new("ng64v13_EntEvent")

local logTag = "ng64Ents"

-- entity types, as in ents.h
local COIN_Y, COIN_R, COIN_B, POWER_STAR, CAP_METAL, CAP_WING, STAR_POWER = 1, 2, 3, 4, 5, 6, 7
local GOOMBA, BOBOMB, KOOPA, SHELL = 20, 21, 22, 23
local DEAD = 9   -- an enemy's state when squashed

local api            -- { sendRaw, hud, stubId, toast }
local active = {}    -- id -> { type, state, tx, ty, tz (target), x, y, z, yaw, anim, scale, seen, slot }
local pools = {}     -- type -> { free = {}, all = {}, max }
local frameNo, simTime = 0, 0
local createdThisFrame = 0
local opts = { pickups = true, enemies = true, song = 0, volume = 100 }

-- ----------------------------------------------------------------------------------------------------------------
-- materials and meshes

local function ensureMaterial(r, g, b, shiny)
  local name = string.format("ng64_ent_%02x%02x%02x%s", math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5), shiny and "s" or "")
  if not scenetree.findObject(name) then
    local mat = createObject("Material")
    mat:setField("mapTo", 0, name)
    mat:setField("diffuseColor", 0, string.format("%f %f %f 1", r, g, b))
    if shiny then
      mat:setField("specular", 0, "1 1 0.9 1")
      mat:setField("specularPower", 0, "60")
    end
    mat.canSave = false
    mat:registerObject(name)
    mat:setField("doubleSided", 0, "1")
    mat:flush()
    mat:reload()
  end
  return name
end

-- a model is built into one submesh per colour
local function newBuilder() return { subs = {}, order = {} } end

local function sub(b, r, g, b2, shiny)
  local key = string.format("%.3f,%.3f,%.3f,%s", r, g, b2, tostring(shiny))
  local s = b.subs[key]
  if not s then
    s = { verts = {}, normals = {}, uvs = {}, faces = {}, material = ensureMaterial(r, g, b2, shiny) }
    b.subs[key] = s
    b.order[#b.order + 1] = s
  end
  return s
end

local function vtx(s, x, y, z, nx, ny, nz)
  local n = #s.verts
  s.verts[n + 1] = { x = x, y = y, z = z }
  local l = math.sqrt(nx * nx + ny * ny + nz * nz)
  if l < 1e-9 then l = 1 end
  s.normals[n + 1] = { x = nx / l, y = ny / l, z = nz / l }
  s.uvs[n + 1] = { u = 0.5, v = 0.5 }
  return n
end

local function tri(s, a, b, c)
  local f = s.faces
  f[#f + 1] = { v = a, n = a, u = a }
  f[#f + 1] = { v = b, n = b, u = b }
  f[#f + 1] = { v = c, n = c, u = c }
end

local function quad(s, a, b, c, d) tri(s, a, b, c) tri(s, a, c, d) end

-- an ellipsoid (or a cut of one) centred at cx, cy, cz with radii rx, ry, rz; theta is the angle from straight up
local function sphere(b, col, cx, cy, cz, rx, ry, rz, thetaMin, thetaMax, segs, rings)
  local s = sub(b, col[1], col[2], col[3], col[4])
  segs, rings = segs or 12, rings or 8
  thetaMin, thetaMax = thetaMin or 0, thetaMax or math.pi
  local base = #s.verts
  for i = 0, rings do
    local th = thetaMin + (thetaMax - thetaMin) * i / rings
    local st, ct = math.sin(th), math.cos(th)
    for j = 0, segs do
      local ph = 2 * math.pi * j / segs
      local cp, sp = math.cos(ph), math.sin(ph)
      vtx(s, cx + rx * st * cp, cy + ry * st * sp, cz + rz * ct, st * cp / rx, st * sp / ry, ct / rz)
    end
  end
  for i = 0, rings - 1 do
    for j = 0, segs - 1 do
      local a = base + i * (segs + 1) + j
      quad(s, a, a + 1, a + segs + 2, a + segs + 1)
    end
  end
end

-- a cylinder with its axis along 'x', 'y' or 'z', length h centred on cx, cy, cz
local function cylinder(b, col, cx, cy, cz, r, h, axis, segs)
  local s = sub(b, col[1], col[2], col[3], col[4])
  segs = segs or 14
  local function P(u, v, w) -- u, v across the disc, w along the axis
    if axis == "x" then return cx + w, cy + u, cz + v end
    if axis == "y" then return cx + u, cy + w, cz + v end
    return cx + u, cy + v, cz + w
  end
  local function N(u, v, w)
    if axis == "x" then return w, u, v end
    if axis == "y" then return u, w, v end
    return u, v, w
  end
  local base = #s.verts
  for j = 0, segs do
    local a = 2 * math.pi * j / segs
    local c, sn = math.cos(a), math.sin(a)
    local x1, y1, z1 = P(r * c, r * sn, -h / 2)
    local nx, ny, nz = N(c, sn, 0)
    vtx(s, x1, y1, z1, nx, ny, nz)
    local x2, y2, z2 = P(r * c, r * sn, h / 2)
    vtx(s, x2, y2, z2, nx, ny, nz)
  end
  for j = 0, segs - 1 do
    local a = base + j * 2
    quad(s, a, a + 2, a + 3, a + 1)
  end
  for _, side in ipairs({ -1, 1 }) do
    local x0, y0, z0 = P(0, 0, side * h / 2)
    local nx, ny, nz = N(0, 0, side)
    local centre = vtx(s, x0, y0, z0, nx, ny, nz)
    local ring = #s.verts
    for j = 0, segs do
      local a = 2 * math.pi * j / segs
      local x1, y1, z1 = P(r * math.cos(a), r * math.sin(a), side * h / 2)
      vtx(s, x1, y1, z1, nx, ny, nz)
    end
    for j = 0, segs - 1 do
      if side > 0 then tri(s, centre, ring + j, ring + j + 1) else tri(s, centre, ring + j + 1, ring + j) end
    end
  end
end

-- a box centred at cx, cy, cz with half sizes hx, hy, hz, optionally turned about z by 'yaw' then about x by 'roll'
local function box(b, col, cx, cy, cz, hx, hy, hz, yaw, roll)
  local s = sub(b, col[1], col[2], col[3], col[4])
  local cyw, syw = math.cos(yaw or 0), math.sin(yaw or 0)
  local crl, srl = math.cos(roll or 0), math.sin(roll or 0)
  local function T(x, y, z)
    local y2, z2 = y * crl - z * srl, y * srl + z * crl
    return cx + x * cyw - y2 * syw, cy + x * syw + y2 * cyw, cz + z2
  end
  local function D(x, y, z)
    local y2, z2 = y * crl - z * srl, y * srl + z * crl
    return x * cyw - y2 * syw, x * syw + y2 * cyw, z2
  end
  local faces = {
    { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 }, { 0, -1, 0 } },
    { { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 }, { 1, 0, 0 } },
    { { 1, 1, -1 }, { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 0, 1, 0 } },
    { { -1, 1, -1 }, { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 0, 0 } },
    { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }, { 0, 0, 1 } },
    { { -1, 1, -1 }, { 1, 1, -1 }, { 1, -1, -1 }, { -1, -1, -1 }, { 0, 0, -1 } },
  }
  for _, f in ipairs(faces) do
    local nx, ny, nz = D(f[5][1], f[5][2], f[5][3])
    local ids = {}
    for k = 1, 4 do
      local x, y, z = T(f[k][1] * hx, f[k][2] * hy, f[k][3] * hz)
      ids[k] = vtx(s, x, y, z, nx, ny, nz)
    end
    quad(s, ids[1], ids[2], ids[3], ids[4])
  end
end

-- a five-pointed star prism standing in the x-z plane (points up), facing along y
local function starPrism(b, col, cx, cy, cz, rOut, rIn, thick)
  local s = sub(b, col[1], col[2], col[3], col[4])
  local pts = {}
  for k = 0, 9 do
    local a = k * math.pi / 5
    local r = k % 2 == 0 and rOut or rIn
    pts[#pts + 1] = { r * math.sin(a), r * math.cos(a) }
  end
  for _, side in ipairs({ -1, 1 }) do
    local centre = vtx(s, cx, cy + side * thick / 2, cz, 0, side, 0)
    local ring = #s.verts
    for k = 1, 10 do vtx(s, cx + pts[k][1], cy + side * thick / 2, cz + pts[k][2], 0, side, 0) end
    for k = 0, 9 do
      local a, c = ring + k, ring + (k + 1) % 10
      if side > 0 then tri(s, centre, c, a) else tri(s, centre, a, c) end
    end
  end
  for k = 1, 10 do
    local p, q = pts[k], pts[k % 10 + 1]
    local nx, nz = q[2] - p[2], -(q[1] - p[1])
    local a = vtx(s, cx + p[1], cy - thick / 2, cz + p[2], nx, 0, nz)
    local bb = vtx(s, cx + q[1], cy - thick / 2, cz + q[2], nx, 0, nz)
    local c = vtx(s, cx + q[1], cy + thick / 2, cz + q[2], nx, 0, nz)
    local d = vtx(s, cx + p[1], cy + thick / 2, cz + p[2], nx, 0, nz)
    quad(s, a, bb, c, d)
  end
end

-- ----------------------------------------------------------------------------------------------------------------
-- the models (metres, +x is forward, z up, origin at the feet for enemies and the centre for pickups)

local WHITE, BLACK = { 1, 1, 1 }, { 0.04, 0.04, 0.05 }

local function eyes(b, x, spread, z, size)
  sphere(b, WHITE, x, -spread, z, size * 0.6, size, size * 1.25, nil, nil, 8, 6)
  sphere(b, WHITE, x, spread, z, size * 0.6, size, size * 1.25, nil, nil, 8, 6)
  sphere(b, BLACK, x + size * 0.45, -spread, z, size * 0.3, size * 0.5, size * 0.7, nil, nil, 6, 4)
  sphere(b, BLACK, x + size * 0.45, spread, z, size * 0.3, size * 0.5, size * 0.7, nil, nil, 6, 4)
end

local MODELS = {
  [COIN_Y] = function(b)
    cylinder(b, { 1, 0.78, 0.08, true }, 0, 0, 0, 0.3, 0.07, "y", 16)
    cylinder(b, { 1, 0.9, 0.35, true }, 0, 0, 0, 0.2, 0.085, "y", 16)
  end,
  [COIN_R] = function(b)
    cylinder(b, { 0.85, 0.08, 0.08, true }, 0, 0, 0, 0.3, 0.07, "y", 16)
    cylinder(b, { 1, 0.45, 0.45, true }, 0, 0, 0, 0.2, 0.085, "y", 16)
  end,
  [COIN_B] = function(b)
    cylinder(b, { 0.12, 0.3, 0.95, true }, 0, 0, 0, 0.3, 0.07, "y", 16)
    cylinder(b, { 0.5, 0.7, 1, true }, 0, 0, 0, 0.2, 0.085, "y", 16)
  end,
  [POWER_STAR] = function(b)
    starPrism(b, { 1, 0.88, 0.08, true }, 0, 0, 0, 0.5, 0.22, 0.2)
    for _, side in ipairs({ -1, 1 }) do
      box(b, BLACK, 0.0, side * 0.105, 0.05, 0.035, 0.012, 0.09)
      box(b, BLACK, 0.0, side * 0.105, 0.05, 0.035, 0.012, 0.09)
    end
    for _, ex in ipairs({ -0.09, 0.09 }) do
      box(b, BLACK, ex, -0.106, 0.04, 0.03, 0.012, 0.085)
      box(b, BLACK, ex, 0.106, 0.04, 0.03, 0.012, 0.085)
    end
  end,
  [STAR_POWER] = function(b)
    starPrism(b, { 1, 0.4, 0.05, true }, 0, 0, 0, 0.58, 0.26, 0.2)
    starPrism(b, { 1, 0.95, 0.5, true }, 0, 0, 0, 0.34, 0.15, 0.24)
    for _, ex in ipairs({ -0.07, 0.07 }) do
      box(b, BLACK, ex, -0.125, 0.04, 0.025, 0.012, 0.07)
      box(b, BLACK, ex, 0.125, 0.04, 0.025, 0.012, 0.07)
    end
  end,
  [CAP_METAL] = function(b)
    sphere(b, { 0.72, 0.76, 0.84, true }, 0, 0, 0, 0.3, 0.3, 0.26, 0, math.pi / 2, 14, 7)
    cylinder(b, { 0.72, 0.76, 0.84, true }, 0, 0, 0, 0.3, 0.03, "z", 14)
    box(b, { 0.62, 0.66, 0.75, true }, 0.3, 0, 0.0, 0.17, 0.2, 0.025)
  end,
  [CAP_WING] = function(b)
    sphere(b, { 0.88, 0.08, 0.08 }, 0, 0, 0, 0.3, 0.3, 0.26, 0, math.pi / 2, 14, 7)
    cylinder(b, { 0.88, 0.08, 0.08 }, 0, 0, 0, 0.3, 0.03, "z", 14)
    box(b, { 0.8, 0.05, 0.05 }, 0.3, 0, 0.0, 0.17, 0.2, 0.025)
    for _, side in ipairs({ -1, 1 }) do
      box(b, WHITE, -0.05, side * 0.36, 0.2, 0.2, 0.2, 0.012, side * 0.25, side * 0.5)
      box(b, { 0.95, 0.85, 0.5 }, -0.05, side * 0.36, 0.2, 0.15, 0.15, 0.016, side * 0.25, side * 0.5)
    end
  end,
  [GOOMBA] = function(b)
    local brown, tan, dark = { 0.5, 0.29, 0.12 }, { 0.93, 0.78, 0.55 }, { 0.26, 0.15, 0.07 }
    sphere(b, brown, 0, 0, 0.5, 0.46, 0.46, 0.4, 0, math.pi / 2 + 0.25, 14, 8)
    cylinder(b, tan, 0, 0, 0.3, 0.27, 0.34, "z", 12)
    sphere(b, dark, 0.06, -0.2, 0.1, 0.22, 0.14, 0.1, nil, nil, 8, 6)
    sphere(b, dark, 0.06, 0.2, 0.1, 0.22, 0.14, 0.1, nil, nil, 8, 6)
    eyes(b, 0.32, 0.13, 0.5, 0.1)
    box(b, BLACK, 0.4, -0.13, 0.66, 0.02, 0.1, 0.02, 0, 0.5)
    box(b, BLACK, 0.4, 0.13, 0.66, 0.02, 0.1, 0.02, 0, -0.5)
  end,
  [BOBOMB] = function(b)
    local body, yellow = { 0.1, 0.1, 0.13 }, { 1, 0.72, 0.08 }
    sphere(b, body, 0, 0, 0.45, 0.4, 0.4, 0.4, nil, nil, 14, 10)
    cylinder(b, { 0.55, 0.55, 0.6 }, 0, 0, 0.87, 0.17, 0.1, "z", 10)
    cylinder(b, { 0.85, 0.75, 0.5 }, 0, 0, 1.0, 0.025, 0.2, "z", 6)
    sphere(b, { 1, 0.2, 0.05 }, 0, 0, 1.12, 0.05, 0.05, 0.05, nil, nil, 6, 4)
    sphere(b, yellow, 0.05, -0.18, 0.09, 0.15, 0.1, 0.09, nil, nil, 8, 6)
    sphere(b, yellow, 0.05, 0.18, 0.09, 0.15, 0.1, 0.09, nil, nil, 8, 6)
    eyes(b, 0.34, 0.12, 0.55, 0.1)
  end,
  [KOOPA] = function(b)
    local green, cream, skin = { 0.1, 0.55, 0.15 }, { 0.95, 0.9, 0.65 }, { 0.98, 0.82, 0.3 }
    sphere(b, green, -0.1, 0, 0.62, 0.42, 0.38, 0.32, 0, math.pi / 2 + 0.2, 12, 7)
    cylinder(b, cream, -0.1, 0, 0.55, 0.36, 0.06, "z", 12)
    sphere(b, skin, 0.3, 0, 0.92, 0.17, 0.15, 0.2, nil, nil, 10, 8)
    sphere(b, skin, 0.4, 0, 0.88, 0.1, 0.09, 0.08, nil, nil, 8, 6)
    eyes(b, 0.4, 0.07, 0.98, 0.05)
    sphere(b, skin, 0.05, -0.13, 0.3, 0.1, 0.09, 0.3, nil, nil, 8, 6)
    sphere(b, skin, 0.05, 0.13, 0.3, 0.1, 0.09, 0.3, nil, nil, 8, 6)
    sphere(b, green, 0.12, -0.13, 0.07, 0.17, 0.1, 0.07, nil, nil, 8, 6)
    sphere(b, green, 0.12, 0.13, 0.07, 0.17, 0.1, 0.07, nil, nil, 8, 6)
  end,
  [SHELL] = function(b)
    sphere(b, { 0.1, 0.55, 0.15 }, 0, 0, 0.1, 0.4, 0.36, 0.32, 0, math.pi / 2 + 0.1, 12, 7)
    cylinder(b, { 0.95, 0.9, 0.65 }, 0, 0, 0.09, 0.34, 0.06, "z", 12)
  end,
}

local POOL_MAX = {
  [COIN_Y] = 12, [COIN_R] = 4, [COIN_B] = 3, [POWER_STAR] = 2, [STAR_POWER] = 2, [CAP_METAL] = 2, [CAP_WING] = 2,
  [GOOMBA] = 8, [BOBOMB] = 8, [KOOPA] = 4, [SHELL] = 6,
}

local serial = 0
local function buildMesh(typ)
  local b = newBuilder()
  MODELS[typ](b)
  serial = serial + 1
  local obj = createObject("ProceduralMesh")
  obj:registerObject("ng64_ent_" .. typ .. "_" .. serial)
  obj.canSave = false
  scenetree.MissionGroup:addObject(obj.obj)
  obj:setHidden(true)
  obj:createMesh({ b.order })
  return { obj = obj, builtFrame = frameNo, hidden = true, typ = typ }
end

local function acquire(typ)
  local pool = pools[typ]
  if not pool then pool = { free = {}, all = {} } pools[typ] = pool end
  local slot = table.remove(pool.free)
  if slot and slot.obj and scenetree.findObject(slot.obj:getName()) then return slot end
  if #pool.all >= (POOL_MAX[typ] or 4) or createdThisFrame >= 2 then return nil end
  createdThisFrame = createdThisFrame + 1
  slot = buildMesh(typ)
  pool.all[#pool.all + 1] = slot
  return slot
end

local function release(e)
  local slot = e.slot
  if not slot then return end
  if not slot.hidden then slot.obj:setHidden(true) slot.hidden = true end
  table.insert(pools[slot.typ].free, slot)
  e.slot = nil
end

-- a level change deletes the scene's objects with the level: forget the pool, it is rebuilt on demand
function M.reset()
  for id, e in pairs(active) do e.slot = nil end
  active = {}
  pools = {}
end

-- ----------------------------------------------------------------------------------------------------------------
-- from the helper

function M.init(a)
  api = a
end

function M.onEntities(data)
  local n = ffi.new("uint16_t[1]")
  ffi.copy(n, string.sub(data, 2, 3), 2)
  local count = n[0]
  local seen = {}
  for i = 0, count - 1 do
    ffi.copy(entBuf, string.sub(data, 4 + i * ENT_SIZE, 3 + (i + 1) * ENT_SIZE), ENT_SIZE)
    local id = entBuf.id
    local e = active[id]
    if not e then
      e = { type = entBuf.type, x = entBuf.pos[0], y = entBuf.pos[1], z = entBuf.pos[2], yaw = entBuf.yaw, first = true }
      active[id] = e
    end
    e.type, e.state = entBuf.type, entBuf.state
    e.tx, e.ty, e.tz = entBuf.pos[0], entBuf.pos[1], entBuf.pos[2]
    e.tyaw, e.anim, e.scale = entBuf.yaw, entBuf.anim, entBuf.scale
    e.seen = simTime
    seen[id] = true
  end
  for id, e in pairs(active) do
    if not seen[id] then release(e) active[id] = nil end
  end
end

local function explosionAt(x, y, z, radius)
  local pos = vec3(x, y, z + 0.4)
  -- particles from a pooled emitter node
  M.fx = M.fx or { next = 1 }
  local fx = M.fx
  if not fx.nodes then
    fx.nodes = {}
    for i, emitter in ipairs({ "BNGP_31", "BNGP_32", "BNGP_31", "BNGP_32" }) do
      local o = createObject("ParticleEmitterNode")
      o:setPosition(vec3(0, 0, -1000))
      o.scale = vec3(1, 1, 1)
      o:setField("rotation", 0, "1 0 0 0")
      o:setField("emitter", 0, emitter)
      o:setField("dataBlock", 0, "lightExampleEmitterNodeData1")
      o.canSave = false
      o:registerObject("ng64_fx_" .. i .. "_" .. serial)
      serial = serial + 1
      scenetree.MissionGroup:addObject(o.obj)
      fx.nodes[i] = { obj = o, until_ = 0 }
    end
  end
  for i = 0, 1 do
    local n = fx.nodes[(fx.next - 1) % #fx.nodes + 1]
    fx.next = fx.next + 1
    n.obj:setPosition(pos)
    n.obj:setHidden(false)
    n.until_ = simTime + 0.5
  end
end

-- cars within the blast are dented and shoved away from it
local function blastCars(x, y, z, radius)
  local c = vec3(x, y, z)
  for i = 0, be:getObjectCount() - 1 do
    local veh = be:getObject(i)
    if veh and veh:getID() ~= (api.stubId() or -1) then
      local id = veh:getID()
      local cx, cy, cz = be:getObjectOOBBCenterXYZ(id)
      if cx then
        local ctr = vec3(cx, cy, cz)
        local d = c - ctr
        -- distance to the car's box, roughly: from the centre minus its half size along the line to the blast
        local dir = d:length() > 1e-3 and (ctr - c) / d:length() or vec3(0, 0, 1)
        local extent = 0
        for a = 0, 2 do
          local ax = vec3(be:getObjectOOBBHalfAxisXYZ(id, a))
          extent = math.max(extent, math.abs(ax:dot(dir)))
        end
        local dist = math.max(0, d:length() - extent)
        if dist < radius then
          local f = 1 - dist / radius
          local hitPoint = ctr - dir * extent
          veh:queueLuaCommand(string.format(
            "if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,%f,%f,%f,%f)",
            hitPoint.x, hitPoint.y, hitPoint.z, dir.x, dir.y, math.max(dir.z, 0.15), 2 + 9 * f))
        end
      end
    end
  end
end

function M.onEvent(data)
  ffi.copy(evBuf, data, ffi.sizeof("ng64v13_EntEvent"))
  local kind = evBuf.kind
  local a, b, c, d = evBuf.a, evBuf.b, evBuf.c, evBuf.d
  if kind == 1 then
    api.hud.collectCoin(math.floor(d + 0.5))
  elseif kind == 2 then
    api.hud.collectStar(1)
    api.toast("Power Star!")
  elseif kind == 3 then
    api.toast("Metal Cap! " .. math.floor(d) .. " seconds")
  elseif kind == 4 then
    api.toast("Wing Cap! " .. math.floor(d) .. " seconds")
  elseif kind == 5 then
    M.starUntil = simTime + d
    api.toast("Invincible! " .. math.floor(d) .. " seconds")
  elseif kind == 6 then
    if d == 7 then M.starUntil = nil end
    api.toast(d == 5 and "The Metal Cap wore off" or d == 6 and "The Wing Cap wore off" or "Invincibility wore off")
  elseif kind == 7 then
    explosionAt(a, b, c, d)
    blastCars(a, b, c, d + 1.0)
  elseif kind == 9 then
    opts.pickups, opts.enemies = a ~= 0, b ~= 0
    opts.song = c >= 0 and math.floor(c + 0.5) or -1
    opts.volume = math.floor(d + 0.5)
    guihooks.trigger("NG64Settings", opts)
  end
end

-- ----------------------------------------------------------------------------------------------------------------
-- settings (the settings app, through ng64.setOption)

local function sendOptions(song, volume)
  api.sendRaw("o" .. string.char(opts.pickups and 1 or 0, opts.enemies and 1 or 0, song or 255, volume or 255))
end

function M.setOption(key, value)
  if key == "pickups" then opts.pickups = value and true or false sendOptions()
  elseif key == "enemies" then opts.enemies = value and true or false sendOptions()
  elseif key == "song" then
    local idx = tonumber(value) or -1
    sendOptions(idx < 0 and 254 or math.min(253, idx), nil)
  elseif key == "volume" then
    opts.volume = math.max(0, math.min(100, math.floor((tonumber(value) or 100) + 0.5)))
    sendOptions(nil, opts.volume)
  end
  guihooks.trigger("NG64Settings", opts)
end

function M.resendSettings() guihooks.trigger("NG64Settings", opts) end
function M.getOptions() return opts end

-- ----------------------------------------------------------------------------------------------------------------
-- cars and enemies

local carCheckAt = 0
local carHit = {}   -- "enemy id:vehicle id" -> simTime of the last hit

local function checkCars(marioPos)
  local stub = api.stubId() or -1
  local anyEnemy = false
  for _, e in pairs(active) do if e.type >= GOOMBA and e.state ~= DEAD then anyEnemy = true break end end
  if not anyEnemy then return end
  for i = 0, be:getObjectCount() - 1 do
    local veh = be:getObject(i)
    if veh and veh:getID() ~= stub then
      local id = veh:getID()
      local cx, cy, cz = be:getObjectOOBBCenterXYZ(id)
      if cx and (vec3(cx, cy, cz) - marioPos):length() < 70 then
        local ctr = vec3(cx, cy, cz)
        local ax = { vec3(be:getObjectOOBBHalfAxisXYZ(id, 0)), vec3(be:getObjectOOBBHalfAxisXYZ(id, 1)), vec3(be:getObjectOOBBHalfAxisXYZ(id, 2)) }
        local vel = veh:getVelocity()
        local speed = vel:length()
        for eid, e in pairs(active) do
          if e.type >= GOOMBA and e.state ~= DEAD and e.tx then
            local p = vec3(e.tx, e.ty, e.tz + 0.3) - ctr
            local inside = true
            for k = 1, 3 do
              local len = ax[k]:length()
              if len > 1e-3 and math.abs(p:dot(ax[k]) / len) > len + 0.35 then inside = false break end
            end
            local slideShell = e.type == SHELL and e.state == 1
            if inside and (speed > 2.5 or slideShell) and simTime - (carHit[eid .. ":" .. id] or -9) > 1.0 then
              carHit[eid .. ":" .. id] = simTime
              api.sendRaw("c" .. string.char(eid % 256, math.floor(eid / 256), slideShell and 1 or 0))
              -- the car takes a small dent where it met the enemy, scaled to what it hit
              local strength = e.type == GOOMBA and 0.7 or e.type == KOOPA and 0.9 or e.type == SHELL and 1.4 or 0.4
              local dir = speed > 0.5 and (-vel / speed) or vec3(0, 0, 1)
              local at = vec3(e.tx, e.ty, e.tz + 0.3)
              veh:queueLuaCommand(string.format(
                "if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,%f,%f,%f,%f,0)",
                at.x, at.y, at.z, dir.x, dir.y, dir.z, strength))
            end
          end
        end
      end
    end
  end
end

-- ----------------------------------------------------------------------------------------------------------------
-- drawing

local function quatYaw(a)
  local h = a / 2
  return 0, 0, math.sin(h), math.cos(h)
end

function M.update(dt, marioPos)
  frameNo = frameNo + 1
  simTime = simTime + dt
  createdThisFrame = 0
  local k = 1 - math.exp(-25 * dt)

  for id, e in pairs(active) do
    if simTime - e.seen > 0.4 then
      release(e)
      active[id] = nil
    elseif e.tx then
      if e.first or (e.tx - e.x) ^ 2 + (e.ty - e.y) ^ 2 > 25 then
        e.x, e.y, e.z = e.tx, e.ty, e.tz
      else
        e.x, e.y, e.z = e.x + (e.tx - e.x) * k, e.y + (e.ty - e.y) * k, e.z + (e.tz - e.z) * k
      end
      -- turn the short way
      local dy = (e.tyaw - e.yaw + math.pi) % (2 * math.pi) - math.pi
      e.yaw = e.first and e.tyaw or (e.yaw + dy * math.min(1, k * 1.5))

      if not e.slot then e.slot = acquire(e.type) end
      local slot = e.slot
      if slot then
        local z, sx, sy, sz, yaw = e.z, 1, 1, 1, e.yaw
        local t = e.type
        if t <= STAR_POWER then
          z = z + 0.08 * math.sin(e.anim)
        elseif e.state == DEAD then
          sz = e.scale
          sx, sy = 1 + (1 - e.scale) * 0.7, 1 + (1 - e.scale) * 0.7
        elseif t == GOOMBA or t == KOOPA then
          local moving = e.state == 1 or e.state == 0
          if moving then z = z + 0.05 * math.abs(math.sin(e.anim)) end
        elseif t == BOBOMB then
          z = z + 0.04 * math.abs(math.sin(e.anim))
          sx, sy, sz = e.scale, e.scale, e.scale
        elseif t == SHELL and e.state == 1 then
          yaw = e.anim * 3
        end
        local qx, qy, qz, qw = quatYaw(yaw)
        slot.obj:setPosRot(e.x, e.y, z, qx, qy, qz, qw)
        if sx ~= 1 or sy ~= 1 or sz ~= 1 or slot.scaled then
          slot.obj:setScale(vec3(sx, sy, sz))
          slot.scaled = not (sx == 1 and sy == 1 and sz == 1)
        end
        -- a mesh built this frame is blank until it has been drawn once: show it the next frame
        if slot.hidden and slot.builtFrame < frameNo then
          slot.obj:setHidden(false)
          slot.hidden = false
        end
      end
      e.first = false
    end
  end

  if M.fx and M.fx.nodes then
    for _, n in ipairs(M.fx.nodes) do
      if n.until_ > 0 and simTime > n.until_ then n.obj:setHidden(true) n.obj:setPosition(vec3(0, 0, -1000)) n.until_ = 0 end
    end
  end

  carCheckAt = carCheckAt - dt
  if carCheckAt <= 0 and marioPos then
    carCheckAt = 0.1
    checkCars(marioPos)
  end
end

function M.getStatus()
  local n, e = 0, 0
  for _, v in pairs(active) do if v.type <= STAR_POWER then n = n + 1 else e = e + 1 end end
  local slots = 0
  for _, p in pairs(pools) do slots = slots + #p.all end
  return { pickups = n, enemies = e, meshes = slots, options = opts }
end

return M
