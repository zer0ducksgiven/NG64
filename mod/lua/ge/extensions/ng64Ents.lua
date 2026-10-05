-- NG64: SM64's pickups and enemies, drawn in BeamNG. The helper simulates them (ents.c) and sends the list ~30 times a
-- second, each as the rigid pieces of its model, read from the player's own ROM (objrom.c: geo layout, display lists,
-- textures, animation): this builds one mesh per piece the helper describes (asked for when first needed) and moves them.
-- A mesh that has been drawn costs for good when rebuilt, so every piece has a pool of instances, built once and reused.
-- It also applies what the cars do to the enemies and what the enemies do to the cars, and turns the helper's events
-- into the HUD's coin / star counts, toasts and effects. And the settings the NG64 settings app edits.
local M = {}

pcall(ffi.cdef, [[
#pragma pack(push, 1)
typedef struct { uint8_t type; uint8_t kind; uint16_t id; float a, b, c, d; } ng64v13_EntEvent;
typedef struct { uint16_t id; uint8_t type; uint8_t state; uint8_t nparts; uint8_t pad; float pos[3]; } ng64v14_EntHead;
typedef struct { uint16_t piece; uint8_t flags; uint8_t pad; float pos[3]; int16_t q[4]; float scale; } ng64v14_EntPart;
typedef struct { int16_t p[3]; int8_t n[3]; int8_t pad; uint16_t uv[2]; } ng64v14_ObjVert;
typedef struct { uint16_t piece; uint8_t alpha; uint8_t pad; uint16_t nv; uint16_t ni; } ng64v14_PieceHead;
#pragma pack(pop)
]])
local HEAD_SIZE, PART_SIZE = ffi.sizeof("ng64v14_EntHead"), ffi.sizeof("ng64v14_EntPart")
local OVERT_SIZE, PIECE_HEAD = ffi.sizeof("ng64v14_ObjVert"), ffi.sizeof("ng64v14_PieceHead")
local headBuf = ffi.new("ng64v14_EntHead")
local partBuf = ffi.new("ng64v14_EntPart")
local pieceHdr = ffi.new("ng64v14_PieceHead")
local overts = ffi.new("ng64v14_ObjVert[?]", 520)
local oidx = ffi.new("uint16_t[?]", 2000)
local evBuf = ffi.new("ng64v13_EntEvent")

local logTag = "ng64Ents"
local S = 0.0085     -- metres per SM64 unit

-- entity types, as in ents.h
local COIN_Y, COIN_R, COIN_B, POWER_STAR, CAP_METAL, CAP_WING, STAR_POWER = 1, 2, 3, 4, 5, 6, 7
local GOOMBA, BOBOMB, KOOPA, SHELL = 20, 21, 22, 23
local DEAD = 9   -- an enemy's state when squashed

local api            -- { sendRaw, hud, stubId, toast }
local active = {}    -- id -> { type, state, tx, ty, tz (base position), parts = { {piece, flags, tp, tq, ts, cp, cq, cs, slot} }, seen }
local pieces = {}    -- piece id -> { verts, normals, uvs, faces, alpha } once the helper has sent it
local pieceAsked = {}
local pools = {}     -- piece id -> { free = {}, count = n }
local matName, flatName, matPath
local frameNo, simTime = 0, 0
local createdThisFrame = 0
local meshSerial = 0
local opts = { pickups = true, enemies = true, song = 0, volume = 100 }
local POOL_MAX = 24
local carCheckAt = 0

-- ----------------------------------------------------------------------------------------------------------------
-- material and meshes

local function makeMaterial(name, flat)
  local mat = createObject("Material")
  mat:setField("mapTo", 0, name)
  mat:setField("colorMap", 0, matPath)
  -- BeamNG's light is dimmer than SM64's flat white: brighten the textures (the unlit ones, coins and sprites, most)
  local b = flat and "1.7 1.7 1.7 1" or "1.0 1.0 1.0 1"
  mat:setField("diffuseColor", 0, b)
  mat:setField("specular", 0, "0 0 0 1")      -- SM64 has no reflections: they only tint it with the sky
  mat:setField("alphaTest", 0, "1")
  mat:setField("alphaRef", 0, "127")
  mat.canSave = false
  mat:registerObject(name)
  mat:setField("doubleSided", 0, "1")
  mat:flush()
  mat:reload()
end

local function ensureMaterial()
  if not matPath then return end
  if matName and scenetree.findObject(matName) and scenetree.findObject(flatName) then return end
  meshSerial = meshSerial + 1
  matName = "ng64_obj_mat_" .. meshSerial
  flatName = "ng64_obj_flat_" .. meshSerial
  makeMaterial(matName, false)
  makeMaterial(flatName, true)
end

function M.onAtlas(data)
  local path = string.match(data, "^([^%z]*)", 2)
  if not path or path == "" then return end
  if path ~= matPath then
    matPath = path
    matName = nil
    -- instances made with the old material are dropped with it
    for _, pool in pairs(pools) do pool.free = {} pool.count = 0 end
    for _, e in pairs(active) do for _, p in ipairs(e.parts) do p.slot = nil end end
  end
  ensureMaterial()
end

function M.onPiece(data)
  if #data < 1 + PIECE_HEAD then return end
  ffi.copy(pieceHdr, string.sub(data, 2), PIECE_HEAD)
  local id, nv, ni = tonumber(pieceHdr.piece), tonumber(pieceHdr.nv), tonumber(pieceHdr.ni)
  if nv > 520 or ni > 2000 or #data < 1 + PIECE_HEAD + nv * OVERT_SIZE + ni * 2 then return end
  ffi.copy(overts, string.sub(data, 2 + PIECE_HEAD), nv * OVERT_SIZE)
  ffi.copy(oidx, string.sub(data, 2 + PIECE_HEAD + nv * OVERT_SIZE), ni * 2)
  local verts, normals, uvs, faces = {}, {}, {}, {}
  for i = 0, nv - 1 do
    local v = overts[i]
    -- SM64 (x, y up, z) -> BeamNG (x, -z, y), in metres
    verts[i + 1] = { x = v.p[0] * S, y = -v.p[2] * S, z = v.p[1] * S }
    -- lit surfaces carry their light's colour in the atlas and are shaded by BeamNG on their normals, exactly as Mario is;
    -- unlit ones (coins, sprites, vertex-coloured parts) face the sky, taking the light the ground under them does
    if v.pad ~= 0 then
      -- a lit vertex: its real normal (SM64 -> BeamNG axes), lit by BeamNG as Mario is
      local nx, ny, nz = v.n[0] / 127, -v.n[2] / 127, v.n[1] / 127
      local nl = math.sqrt(nx * nx + ny * ny + nz * nz)
      if nl < 1e-6 then nl = 1 end
      normals[i + 1] = { x = nx / nl, y = ny / nl, z = nz / nl }
    else
      normals[i + 1] = { x = 0, y = 0, z = 1 }
    end
    uvs[i + 1] = { u = v.uv[0] / 65535, v = v.uv[1] / 65535 }
  end
  for i = 0, ni - 1 do local k = oidx[i] faces[i + 1] = { v = k, n = k, u = k } end
  pieces[id] = { verts = verts, normals = normals, uvs = uvs, faces = faces, alpha = pieceHdr.alpha ~= 0, lit = pieceHdr.pad ~= 0 }
end

local function acquire(pieceId)
  local pc = pieces[pieceId]
  if not pc then
    local last = pieceAsked[pieceId]
    if not last or simTime - last > 0.5 then
      pieceAsked[pieceId] = simTime
      api.sendRaw("y" .. string.char(pieceId % 256, math.floor(pieceId / 256)))
    end
    return nil
  end
  if not matName or not scenetree.findObject(matName) then ensureMaterial() if not matName then return nil end end
  local pool = pools[pieceId]
  if not pool then pool = { free = {}, count = 0 } pools[pieceId] = pool end
  local slot = table.remove(pool.free)
  if slot and slot.obj and scenetree.findObject(slot.obj:getName()) then return slot end
  if pool.count >= POOL_MAX or createdThisFrame >= 2 then return nil end
  createdThisFrame = createdThisFrame + 1
  meshSerial = meshSerial + 1
  local obj = createObject("ProceduralMesh")
  obj:registerObject("ng64_obj_" .. pieceId .. "_" .. meshSerial)
  obj.canSave = false
  scenetree.MissionGroup:addObject(obj.obj)
  obj:setHidden(true)
  obj:createMesh({ { { verts = pc.verts, normals = pc.normals, uvs = pc.uvs, faces = pc.faces, material = pc.lit and matName or flatName } } })
  pool.count = pool.count + 1
  return { obj = obj, builtFrame = frameNo, hidden = true, piece = pieceId }
end

local function releasePart(p)
  local slot = p.slot
  if not slot then return end
  if not slot.hidden then slot.obj:setHidden(true) slot.hidden = true end
  local pool = pools[slot.piece]
  if pool then table.insert(pool.free, slot) end
  p.slot = nil
end

local function releaseEnt(e)
  for _, p in ipairs(e.parts) do releasePart(p) end
end

-- a level change deletes the scene's objects with the level: forget the pools, they are rebuilt on demand
function M.reset()
  for _, e in pairs(active) do for _, p in ipairs(e.parts) do p.slot = nil end end
  active = {}
  pools = {}
  matName = nil
end

-- ----------------------------------------------------------------------------------------------------------------
-- from the helper

function M.init(a)
  api = a
end

function M.onEntities(data)
  local count = string.byte(data, 2) + string.byte(data, 3) * 256
  local off = 4   -- 1-based position of the first record
  local seen = {}
  for _ = 1, count do
    if #data < off + HEAD_SIZE - 1 then break end
    ffi.copy(headBuf, string.sub(data, off, off + HEAD_SIZE - 1), HEAD_SIZE)
    local id, np = tonumber(headBuf.id), tonumber(headBuf.nparts)
    off = off + HEAD_SIZE
    if #data < off + np * PART_SIZE - 1 then break end
    local e = active[id]
    if not e then e = { parts = {}, first = true } active[id] = e end
    e.type, e.state = headBuf.type, headBuf.state
    e.tx, e.ty, e.tz = headBuf.pos[0], headBuf.pos[1], headBuf.pos[2]
    e.seen = simTime
    local parts = e.parts
    for k = 1, np do
      ffi.copy(partBuf, string.sub(data, off, off + PART_SIZE - 1), PART_SIZE)
      off = off + PART_SIZE
      local p = parts[k]
      if not p then p = {} parts[k] = p end
      local piece = tonumber(partBuf.piece)
      if p.piece ~= piece then releasePart(p) p.piece = piece p.snap = true end
      p.flags = partBuf.flags
      p.tp = { partBuf.pos[0], partBuf.pos[1], partBuf.pos[2] }
      p.tq = { partBuf.q[0] / 32767, partBuf.q[1] / 32767, partBuf.q[2] / 32767, partBuf.q[3] / 32767 }
      p.ts = partBuf.scale
    end
    for k = #parts, np + 1, -1 do releasePart(parts[k]) parts[k] = nil end
    seen[id] = true
  end
  for id, e in pairs(active) do
    if not seen[id] then releaseEnt(e) active[id] = nil end
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
    opts.song = idx < 0 and -1 or idx
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

local carHit = {}   -- "enemy id:vehicle id" -> simTime of the last hit

local function checkCars(marioPos)
  local stub = api.stubId() or -1
  local anyEnemy = false
  for _, e in pairs(active) do if e.type >= GOOMBA and e.type <= KOOPA and e.state ~= DEAD then anyEnemy = true break end end
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
          if e.type >= GOOMBA and e.type <= KOOPA and e.state ~= DEAD and e.tx then
            local p = vec3(e.tx, e.ty, e.tz + 0.3) - ctr
            local inside = true
            for k = 1, 3 do
              local len = ax[k]:length()
              if len > 1e-3 and math.abs(p:dot(ax[k]) / len) > len + 0.35 then inside = false break end
            end
            -- a bob-omb goes off at any touch; the others need the car to be moving to be knocked out
            if inside and (speed > 2.5 or e.type == BOBOMB) and simTime - (carHit[eid .. ":" .. id] or -9) > 1.0 then
              carHit[eid .. ":" .. id] = simTime
              -- the helper treats it as a fast attack, knocked the way the car was going
              api.sendRaw("c" .. string.char(eid % 256, math.floor(eid / 256), 0) .. ffi.string(ffi.new("float[2]", vel.x, vel.y), 8))
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

local function nlerp(a, b, t)
  local ax, ay, az, aw = a[1], a[2], a[3], a[4]
  local bx, by, bz, bw = b[1], b[2], b[3], b[4]
  if ax * bx + ay * by + az * bz + aw * bw < 0 then bx, by, bz, bw = -bx, -by, -bz, -bw end
  local x, y, z, w = ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t, aw + (bw - aw) * t
  local l = math.sqrt(x * x + y * y + z * z + w * w)
  if l < 1e-9 then return { ax, ay, az, aw } end
  return { x / l, y / l, z / l, w / l }
end

function M.update(dt, marioPos)
  frameNo = frameNo + 1
  simTime = simTime + dt
  createdThisFrame = 0
  local k = 1 - math.exp(-30 * dt)
  local camPos = core_camera and core_camera.getPosition and core_camera.getPosition() or nil

  for id, e in pairs(active) do
    if simTime - e.seen > 0.4 then
      releaseEnt(e)
      active[id] = nil
    else
      for _, p in ipairs(e.parts) do
        if p.tp then
          if p.snap or not p.cp then
            p.cp, p.cq, p.cs = { p.tp[1], p.tp[2], p.tp[3] }, { p.tq[1], p.tq[2], p.tq[3], p.tq[4] }, p.ts
            p.snap = false
          else
            local d2 = (p.tp[1] - p.cp[1]) ^ 2 + (p.tp[2] - p.cp[2]) ^ 2 + (p.tp[3] - p.cp[3]) ^ 2
            local kk = d2 > 25 and 1 or k
            p.cp[1], p.cp[2], p.cp[3] = p.cp[1] + (p.tp[1] - p.cp[1]) * kk, p.cp[2] + (p.tp[2] - p.cp[2]) * kk, p.cp[3] + (p.tp[3] - p.cp[3]) * kk
            p.cq = nlerp(p.cq, p.tq, kk)
            p.cs = p.cs + (p.ts - p.cs) * kk
          end
          if not p.slot then p.slot = acquire(p.piece) end
          local slot = p.slot
          if slot then
            local q = p.cq
            if p.flags == 1 and camPos then
              -- a billboard faces the camera, as SM64's mtxf_billboard: turned toward it, and leant back as far as the camera
              -- is above it (so a sprite seen from above shows its whole face). In BeamNG's conjugate quaternions the turn
              -- R = Rz(yaw) * Rx(lean) is conj(Rx) * conj(Rz)
              local dx, dy, dz = camPos.x - p.cp[1], camPos.y - p.cp[2], camPos.z - p.cp[3]
              local th = math.atan2(dx, -dy)
              local a = -math.atan2(dz, math.sqrt(dx * dx + dy * dy))
              local zs, zc = -math.sin(th / 2), math.cos(th / 2)
              local xs, xc = -math.sin(a / 2), math.cos(a / 2)
              -- (xs,0,0,xc) * (0,0,zs,zc)
              q = { xs * zc, -xs * zs, xc * zs, xc * zc }
            end
            slot.obj:setPosRot(p.cp[1], p.cp[2], p.cp[3], q[1], q[2], q[3], q[4])
            if math.abs(p.cs - (slot.scale or 1)) > 1e-4 then
              slot.obj:setScale(vec3(p.cs, p.cs, p.cs))
              slot.scale = p.cs
            end
            -- a mesh built this frame is blank until it has been drawn once: show it the next frame
            if slot.hidden and slot.builtFrame < frameNo then
              slot.obj:setHidden(false)
              slot.hidden = false
            end
          end
        end
      end
    end
  end

  carCheckAt = carCheckAt - dt
  if carCheckAt <= 0 and marioPos then
    carCheckAt = 0.1
    checkCars(marioPos)
  end
end

function M.dbg()
  local out = {}
  for id, e in pairs(active) do
    for k, p in ipairs(e.parts) do
      out[#out + 1] = string.format("%d/%d piece %s flags %s pos %.2f %.2f %.2f scale %.3f slot %s hidden %s lit %s", id, k, tostring(p.piece), tostring(p.flags), p.cp and p.cp[1] or 0, p.cp and p.cp[2] or 0, p.cp and p.cp[3] or 0, p.cs or 0, tostring(p.slot ~= nil), tostring(p.slot and p.slot.hidden), tostring(pieces[p.piece] and pieces[p.piece].alpha))
    end
  end
  return out
end

function M.list()
  local out = {}
  for id, e in pairs(active) do out[#out + 1] = { id = id, type = e.type, state = e.state, x = e.tx, y = e.ty, z = e.tz, parts = #e.parts, flags = e.parts[1] and e.parts[1].flags, piece = e.parts[1] and e.parts[1].piece } end
  return out
end

function M.getStatus()
  local n, e = 0, 0
  for _, v in pairs(active) do if v.type <= STAR_POWER then n = n + 1 else e = e + 1 end end
  local slots, loaded = 0, 0
  for _, p in pairs(pools) do slots = slots + p.count end
  for _ in pairs(pieces) do loaded = loaded + 1 end
  return { pickups = n, enemies = e, meshes = slots, piecesLoaded = loaded, material = matName, options = opts }
end

return M
