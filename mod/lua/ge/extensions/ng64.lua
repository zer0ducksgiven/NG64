-- NG64: Super Mario 64's Mario (via libsm64 in the NG64 helper process) as a BeamNG "vehicle".
--
-- Spawning the "Mario (NG64)" vehicle turns it into an invisible anchor and hands control to Mario. The helper runs
-- the physics and renders the mesh; this side feeds it collision (terrain raycasts + vehicle boxes), draws what it
-- sends back, drives the camera, and relays hits/damage between Mario and vehicles.
local M = {}
local logTag = "ng64"

local ffi = require("ffi")

local HELPER_HOST, HELPER_PORT = "127.0.0.1", 47064
local PROTO_VERSION = 2
local STUB_MODEL = "ng64_mario"

local GRID_N, GRID_SP = 49, 0.5         -- terrain sample grid around Mario (24 m square)
local GRID_REBUILD_DIST = 5             -- metres Mario may stray from the grid centre before resampling
local GRID_SAMPLES_PER_FRAME = 700
local VEHICLE_RANGE = 80
local MP_SEND_INTERVAL = 1 / 15

pcall(ffi.cdef, [[
#pragma pack(push, 1)
typedef struct {
  uint8_t type; uint32_t key; uint32_t seq;
  float pos[3]; float vel[3]; float faceAngle;
  int16_t health; uint32_t action; int16_t animId; int16_t animFrame; uint32_t flags;
  float camPos[3]; float camTarget[3];
  uint16_t numVerts;
} ng64_FrameHeader;
typedef struct { uint8_t type; uint32_t key; uint32_t seq; uint16_t start; uint16_t count; } ng64_ChunkHeader;
typedef struct { int16_t p[3]; int8_t n[3]; uint16_t uv[2]; } ng64_PackedVert;
typedef struct { uint8_t type; uint32_t vehId; float point[3]; float dir[3]; float strength; } ng64_Hit;
#pragma pack(pop)
]])
local CHUNK_HEADER_SIZE = ffi.sizeof("ng64_ChunkHeader")
local VERT_SIZE = ffi.sizeof("ng64_PackedVert")

local sock
local connected = false
local lastHelloTime = -10
local lastRecvTime = -10
local atlasPath
local materialName
local materialSerial = 0
local warnedNoHelper = false

local active = false          -- local Mario is being played
local stubId                  -- vehicle id of the anchor vehicle
local lastLocalFrame          -- decoded header fields of the latest local frame
local localFrameTime = 0
local prevCam, curCam         -- {pos, target, t}

local meshes = {}             -- key -> { obj, lastFrame }
local pendingFrames = {}      -- key -> frame being reassembled from chunks
local remoteNames = {}

local grid                    -- in-progress terrain sample job
local gridCenter              -- vec3 of the last sent grid

local mpAccum = 0
local hitCount, hurtCount, hullCount = 0, 0, 0   -- for UAT
local simTime = 0

-- ------------------------------------------------------------------------------------------------------------
-- helper link

local function sendRaw(data)
  if sock then sock:send(data) end
end

local function packF(...) return ffi.string(ffi.new("float[?]", select("#", ...), ...), 4 * select("#", ...)) end
local function packU16(v) return ffi.string(ffi.new("uint16_t[1]", v), 2) end
local function packU32(v) return ffi.string(ffi.new("uint32_t[1]", v), 4) end
local function packI16(v) return ffi.string(ffi.new("int16_t[1]", v), 2) end

local function openSocket()
  if sock then return end
  sock = socket.udp()
  sock:settimeout(0)
  sock:setpeername(HELPER_HOST, HELPER_PORT)
end

local function sendHello()
  lastHelloTime = simTime
  sendRaw("H" .. packU16(PROTO_VERSION) .. FS:getUserPath())
end

local function ensureMaterial(path)
  if not path or path == "" or path == atlasPath then return end
  atlasPath = path
  materialSerial = materialSerial + 1
  local name = "ng64_mario_mat_" .. materialSerial
  local mat = createObject("Material")
  mat:setField("mapTo", 0, name)
  mat:setField("colorMap", 0, path)
  mat:setField("diffuseColor", 0, "1 1 1 1")
  mat.canSave = false
  mat:registerObject(name)
  -- sm64's winding is back-facing for BeamNG; doubleSided only takes effect after a flush/reload
  mat:setField("doubleSided", 0, "1")
  mat:flush()
  mat:reload()
  materialName = name
  log("I", logTag, "mario material " .. name .. " -> " .. path)
end

-- ------------------------------------------------------------------------------------------------------------
-- rendering


-- BeamNG's sandboxed ffi refuses pointer casts/arithmetic, so packets are ffi.copy'd into typed scratch buffers
local hdrBuf = ffi.new("ng64_FrameHeader")
local chunkHdrBuf = ffi.new("ng64_ChunkHeader")
local hitBuf = ffi.new("ng64_Hit")
local chunkVerts = ffi.new("ng64_PackedVert[?]", 600)

local function fillVerts(pool, start, count)
  for i = 0, count - 1 do
    local pv = chunkVerts[i]
    local j = start + i + 1
    local vt = pool.verts[j]; if not vt then vt = {}; pool.verts[j] = vt end
    vt.x, vt.y, vt.z = pv.p[0] * 0.001, pv.p[1] * 0.001, pv.p[2] * 0.001
    local nt = pool.normals[j]; if not nt then nt = {}; pool.normals[j] = nt end
    nt.x, nt.y, nt.z = pv.n[0] / 127, pv.n[1] / 127, pv.n[2] / 127
    local ut = pool.uvs[j]; if not ut then ut = {}; pool.uvs[j] = ut end
    ut.u, ut.v = pv.uv[0] / 65535, pv.uv[1] / 65535
    if not pool.faces[j] then pool.faces[j] = { v = j - 1, n = j - 1, u = j - 1 } end
  end
end

local function newPool() return { verts = {}, normals = {}, uvs = {}, faces = {} } end

local NUM_MESH_BUFFERS = 3

-- Only ever called once per onUpdate, with the newest complete frame. createMesh leaves an object blank until the
-- next rendered frame, so each pose goes into a mesh that isn't on screen, is shown, and the previous one is only
-- hidden a few frames later. Three meshes means the one being rebuilt is never the one still waiting to be hidden.
local function buildMesh(key, r)
  local pool, nv = r.pool, r.nv
  for j = nv + 1, pool.lastNv or 0 do pool.verts[j], pool.normals[j], pool.uvs[j], pool.faces[j] = nil, nil, nil, nil end
  pool.lastNv = nv
  local entry = meshes[key]
  if not entry then
    entry = { objs = {}, front = 1, hiding = {} }
    for i = 1, NUM_MESH_BUFFERS do
      local obj = createObject("ProceduralMesh")
      obj:registerObject("ng64_mario_mesh_" .. tostring(key) .. "_" .. i)
      obj.canSave = false
      scenetree.MissionGroup:addObject(obj.obj)
      obj:setHidden(true)
      entry.objs[i] = obj
    end
    meshes[key] = entry
  end
  if nv > 0 and materialName then
    -- next mesh that is neither on screen nor waiting to be hidden
    local back
    for k = 1, NUM_MESH_BUFFERS - 1 do
      local i = (entry.front + k - 1) % NUM_MESH_BUFFERS + 1
      if not entry.hiding[i] then back = i break end
    end
    if not back then return end
    local obj = entry.objs[back]
    obj:createMesh({ { { verts = pool.verts, normals = pool.normals, uvs = pool.uvs, faces = pool.faces, material = materialName } } })
    obj:setPosition(r.pos)
    obj:setHidden(false)
    if entry.obj then entry.hiding[entry.front] = 3 end
    entry.front = back
    entry.obj = obj
  end
  entry.pos = r.pos
  entry.vel = r.vel
  entry.frameTime = simTime
end

local function deleteMesh(key)
  local e = meshes[key]
  if e then for _, o in ipairs(e.objs) do o:delete() end end
  meshes[key] = nil
end

-- ------------------------------------------------------------------------------------------------------------
-- terrain sampling (raycasts) -> heightfield for the helper

local down = vec3(0, 0, -1)
local up = vec3(0, 0, 1)

local function sampleHeight(x, y, refZ)
  -- top-down from well above Mario; if that lands on an overhang above him (bridge, tunnel roof) use the lower ground
  local topStart = vec3(x, y, refZ + 25)
  local d = castRayStatic(topStart, down, 80)
  if d >= 80 then return 0 / 0 end
  local top = topStart.z - d
  if top > refZ + 2.5 then
    local lowStart = vec3(x, y, refZ + 1.0)
    local d2 = castRayStatic(lowStart, down, 60)
    if d2 < 60 then
      local low = lowStart.z - d2
      local ceil = castRayStatic(vec3(x, y, low + 0.1), up, 40)
      if ceil < 40 and ceil > 1.8 then return low end
    end
  end
  return top
end

local function startGrid(center)
  grid = { cx = center.x, cy = center.y, refZ = center.z, i = 0, h = ffi.new("float[?]", GRID_N * GRID_N) }
end

local function stepGrid(maxSamples)
  if not grid then return end
  local total = GRID_N * GRID_N
  local half = (GRID_N - 1) * GRID_SP * 0.5
  local stop = math.min(total, grid.i + maxSamples)
  for k = grid.i, stop - 1 do
    local i, j = k % GRID_N, math.floor(k / GRID_N)
    grid.h[k] = sampleHeight(grid.cx - half + i * GRID_SP, grid.cy - half + j * GRID_SP, grid.refZ)
  end
  grid.i = stop
  if grid.i >= total then
    sendRaw("T" .. packF(grid.cx, grid.cy, GRID_SP) .. packU16(GRID_N) .. ffi.string(grid.h, total * 4))
    gridCenter = vec3(grid.cx, grid.cy, grid.refZ)
    grid = nil
  end
end

-- ------------------------------------------------------------------------------------------------------------
-- vehicles

local function isStub(veh)
  return veh and veh.JBeam == STUB_MODEL
end

local HULL_REFRESH = 2.0            -- seconds between re-reading a vehicle's node shape
local hullAge = {}                  -- vehicle id -> seconds since its hull was last requested

local function requestHull(veh)
  veh:queueLuaCommand("if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.sendHull()")
  hullAge[veh:getID()] = 0
end

local function sendVehicles(marioPos, dt)
  local parts, count = {}, 0
  for i = 0, be:getObjectCount() - 1 do
    local veh = be:getObject(i)
    if veh and not isStub(veh) then
      local id = veh:getID()
      local cx, cy, cz = be:getObjectOOBBCenterXYZ(id)
      if cx and (not marioPos or (vec3(cx, cy, cz) - marioPos):length() < VEHICLE_RANGE) then
        local a0x, a0y, a0z = be:getObjectOOBBHalfAxisXYZ(id, 0)
        local a1x, a1y, a1z = be:getObjectOOBBHalfAxisXYZ(id, 1)
        local a2x, a2y, a2z = be:getObjectOOBBHalfAxisXYZ(id, 2)
        local o, f, u = veh:getPosition(), veh:getDirectionVector(), veh:getDirectionVectorUp()
        parts[#parts + 1] = packU32(id) .. packF(cx, cy, cz, a0x, a0y, a0z, a1x, a1y, a1z, a2x, a2y, a2z,
          o.x, o.y, o.z, f.x, f.y, f.z, u.x, u.y, u.z)
        count = count + 1
        local age = (hullAge[id] or HULL_REFRESH) + (dt or 0)
        hullAge[id] = age
        if age >= HULL_REFRESH then requestHull(veh) end
      end
    end
  end
  sendRaw("V" .. packU16(count) .. table.concat(parts))
end

-- vehicles driving into Mario hurt him
local hurtCooldown = 0
local hitGrace = {}   -- vehicle id -> simTime Mario last hit it
local function checkVehicleHurt(marioPos, marioVel, dt)
  hurtCooldown = math.max(0, hurtCooldown - dt)
  if hurtCooldown > 0 then return end
  for i = 0, be:getObjectCount() - 1 do
    local veh = be:getObject(i)
    if veh and not isStub(veh) then
      local id = veh:getID()
      local vel = veh:getVelocity()
      if vel:length() > 4 and simTime - (hitGrace[id] or -99) > 1.5 then
        local c = vec3(be:getObjectOOBBCenterXYZ(id))
        local d = marioPos + vec3(0, 0, 0.6) - c
        local inside = true
        for a = 0, 2 do
          local ax = vec3(be:getObjectOOBBHalfAxisXYZ(id, a))
          local len = ax:length()
          if len > 1e-3 then
            local along = d:dot(ax) / len
            if math.abs(along) > len + 0.35 then inside = false break end
            -- standing on the roof is riding, not being run over
            if math.abs(ax.z) / len > 0.7 and along * (ax.z > 0 and 1 or -1) > len * 0.6 then inside = false break end
          end
        end
        -- the car's own speed toward Mario (d points car -> Mario). Relative speed would count Mario running or
        -- sliding into a parked car, and a car he just shoved away, as him being hit.
        local flat = vec3(d.x, d.y, 0)
        local speed = flat:length() > 1e-3 and vel:dot(flat / flat:length()) or 0
        if inside and speed > 4 then
          local dmg = math.min(4, math.max(1, math.floor(speed / 6)))
          sendRaw("K" .. packF(c.x, c.y, c.z) .. string.char(dmg, speed > 15 and 1 or 0) .. packF(vel.x, vel.y, vel.z))
          hurtCooldown = 1.0
          hurtCount = hurtCount + 1
          log("I", logTag, string.format("vehicle %d hit mario at %.1f m/s (damage %d)", id, speed, dmg))
          return
        end
      end
    end
  end
end

local function applyHit(data)
  if #data < ffi.sizeof(hitBuf) then return end
  ffi.copy(hitBuf, data, ffi.sizeof(hitBuf))
  local h = hitBuf
  local veh = be:getObjectByID(h.vehId)
  if not veh then return end
  veh:queueLuaCommand(string.format(
    "if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,%f,%f,%f,%f)",
    h.point[0], h.point[1], h.point[2], h.dir[0], h.dir[1], h.dir[2], h.strength))
  hitCount = hitCount + 1
  log("I", logTag, string.format("mario hit vehicle %d (strength %.1f)", h.vehId, h.strength))
  hullAge[tonumber(h.vehId)] = HULL_REFRESH - 0.4   -- re-read the dented shape shortly
  hitGrace[tonumber(h.vehId)] = simTime             -- its dented parts flying about aren't the car hitting him
end

-- ------------------------------------------------------------------------------------------------------------
-- camera

local function applyCamera()
  if not curCam then return end
  local a = 1
  if prevCam then a = math.min(1, (simTime - curCam.t) / (1 / 30)) end
  local pos, target = curCam.pos, curCam.target
  if prevCam then
    pos = prevCam.pos + (curCam.pos - prevCam.pos) * a
    target = prevCam.target + (curCam.target - prevCam.target) * a
  end
  -- pull in when something is between Mario and the camera
  local dir = pos - target
  local len = dir:length()
  if len > 0.01 then
    local n = dir / len
    local hit = castRayStatic(target, n, len)
    if hit < len then pos = target + n * math.max(0.5, hit - 0.3) end
  end
  if not commands.isFreeCamera() then commands.setFreeCamera() end
  core_camera.setPosition(0, pos)
  core_camera.setRotation(0, quatFromDir(target - pos, vec3(0, 0, 1)))
end

-- ------------------------------------------------------------------------------------------------------------
-- local Mario lifecycle

local function activate(veh)
  openSocket()
  if not connected then sendHello() end
  stubId = veh:getID()
  local p = veh:getPosition()
  -- synchronous grid so Mario has a floor the moment he spawns
  startGrid(p)
  stepGrid(GRID_N * GRID_N)
  sendVehicles(p)
  sendRaw("S" .. packF(p.x, p.y, p.z + 0.2))
  active = true
  prevCam, curCam = nil, nil
  log("I", logTag, string.format("mario activated at %.2f %.2f %.2f", p.x, p.y, p.z))
  if not connected then
    guihooks.trigger("toastrMsg", { type = "warning", title = "NG64", msg = "Waiting for the NG64 helper (ng64helper.exe)..." })
  end
end

local function deactivate()
  if not active then return end
  active = false
  sendRaw("D")
  deleteMesh(0)
  lastLocalFrame = nil
  if commands.isFreeCamera() then commands.setGameCamera() end
  if TriggerServerEvent then pcall(TriggerServerEvent, "ng64Gone", "") end
  log("I", logTag, "mario deactivated")
end

-- ------------------------------------------------------------------------------------------------------------
-- multiplayer (BeamMP): state goes out through the NG64 server plugin, remote Marios come back through it

local function onRemote(data)
  local pid, rest = string.match(data or "", "^(%d+)|(.*)$")
  if not pid then return end
  local v = {}
  for num in string.gmatch(rest, "[^,]+") do v[#v + 1] = tonumber(num) end
  if #v < 8 then return end
  local key = tonumber(pid) + 1
  sendRaw("R" .. packU32(key) .. packF(v[1], v[2], v[3], v[4]) .. packU32(v[5]) .. packI16(v[6]) .. packI16(v[7]) .. packU32(v[8]))
  remoteNames[key] = simTime
end

local function onRemoteGone(data)
  local key = (tonumber(data) or -1) + 1
  if key <= 0 then return end
  sendRaw("X" .. packU32(key))
  deleteMesh(key)
  remoteNames[key] = nil
end

if AddEventHandler then
  AddEventHandler("ng64Remote", onRemote)
  AddEventHandler("ng64RemoteGone", onRemoteGone)
end

local function sendMpState(dt)
  if not TriggerServerEvent or not lastLocalFrame then return end
  mpAccum = mpAccum + dt
  if mpAccum < MP_SEND_INTERVAL then return end
  mpAccum = 0
  local f = lastLocalFrame
  pcall(TriggerServerEvent, "ng64State", string.format("%.3f,%.3f,%.3f,%.4f,%d,%d,%d,%d",
    f.pos.x, f.pos.y, f.pos.z, f.faceAngle, f.action, f.animId, f.animFrame, f.flags))
end

-- ------------------------------------------------------------------------------------------------------------
-- packets

local function handlePacket(data)
  local t = string.sub(data, 1, 1)
  if t == "F" then
    if #data < ffi.sizeof(hdrBuf) then return end
    ffi.copy(hdrBuf, data, ffi.sizeof(hdrBuf))
    local h = hdrBuf
    local key = tonumber(h.key)
    if key == 0 and not active then return end
    local nv = math.min(tonumber(h.numVerts), 3072)
    local pos, vel = vec3(h.pos[0], h.pos[1], h.pos[2]), vec3(h.vel[0], h.vel[1], h.vel[2])
    local pf = pendingFrames[key]
    if not pf then pf = { pools = { newPool(), newPool() }, fill = 1 }; pendingFrames[key] = pf end
    pf.seq, pf.nv, pf.got, pf.pos, pf.vel = tonumber(h.seq), nv, 0, pos, vel
    if nv == 0 then pf.ready = { pool = pf.pools[pf.fill], nv = 0, pos = pos, vel = vel } end
    if key == 0 then
      lastLocalFrame = {
        pos = pos, vel = vel,
        faceAngle = h.faceAngle, health = h.health, action = tonumber(h.action),
        animId = h.animId, animFrame = h.animFrame, flags = tonumber(h.flags), numVerts = nv,
      }
      localFrameTime = simTime
      prevCam = curCam
      curCam = { pos = vec3(h.camPos[0], h.camPos[1], h.camPos[2]), target = vec3(h.camTarget[0], h.camTarget[1], h.camTarget[2]), t = simTime }
    end
  elseif t == "G" then
    if #data < CHUNK_HEADER_SIZE then return end
    ffi.copy(chunkHdrBuf, data, CHUNK_HEADER_SIZE)
    local key = tonumber(chunkHdrBuf.key)
    local pf = pendingFrames[key]
    if not pf or pf.seq ~= tonumber(chunkHdrBuf.seq) then return end
    local start, count = tonumber(chunkHdrBuf.start), tonumber(chunkHdrBuf.count)
    if count > 600 or start + count > pf.nv or #data < CHUNK_HEADER_SIZE + count * VERT_SIZE then return end
    ffi.copy(chunkVerts, string.sub(data, CHUNK_HEADER_SIZE + 1), count * VERT_SIZE)
    fillVerts(pf.pools[pf.fill], start, count)
    pf.got = pf.got + count
    if pf.got == pf.nv then
      -- complete: hand this pool to onUpdate and fill the other one from now on
      pf.ready = { pool = pf.pools[pf.fill], nv = pf.nv, pos = pf.pos, vel = pf.vel }
      pf.fill = 3 - pf.fill
    end
  elseif t == "W" then
    local ok = string.byte(data, 2) == 1
    local msg, path = string.match(string.sub(data, 3), "^([^%z]*)%z([^%z]*)")
    if ok then
      if not connected then log("I", logTag, "connected to NG64 helper") end
      connected = true
      warnedNoHelper = false
      ensureMaterial(path)
    else
      log("E", logTag, "helper refused connection: " .. tostring(msg))
      guihooks.trigger("toastrMsg", { type = "error", title = "NG64", msg = tostring(msg) })
    end
  elseif t == "A" then
    applyHit(data)
  elseif t == "P" then
    -- keepalive reply
  elseif t == "L" then
    log("W", logTag, "helper: " .. string.sub(data, 2, -2))
  end
end

local function pump()
  if not sock then return end
  for _ = 1, 64 do
    local data = sock:receive(65536)
    if not data then break end
    lastRecvTime = simTime
    handlePacket(data)
  end
end

-- ------------------------------------------------------------------------------------------------------------
-- hooks

local function onUpdate(dtReal, dtSim, dtRaw)
  local dt = dtReal or 0.016
  simTime = simTime + dt
  if not sock then return end
  pump()

  if connected and simTime - lastRecvTime > 3 then
    connected = false
    log("W", logTag, "lost connection to NG64 helper")
  end
  if not connected and simTime - lastHelloTime > 1 then
    sendHello()
    if active and not warnedNoHelper and simTime - lastHelloTime > 0.5 then
      warnedNoHelper = true
      guihooks.trigger("toastrMsg", { type = "warning", title = "NG64", msg = "Start ng64helper.exe to play as Mario." })
    end
  end

  -- at most one rebuild per mesh per rendered frame, always the newest complete pose
  for key, pf in pairs(pendingFrames) do
    if pf.ready then buildMesh(key, pf.ready) pf.ready = nil end
  end

  -- smooth the meshes between 30 Hz frames
  for key, e in pairs(meshes) do
    if e.obj and e.pos and e.frameTime then
      local ahead = math.min(simTime - e.frameTime, 0.05)
      e.obj:setPosition(e.pos + e.vel * ahead)
    end
    for i, left in pairs(e.hiding) do
      if left <= 1 then e.objs[i]:setHidden(true) e.hiding[i] = nil else e.hiding[i] = left - 1 end
    end
    if key ~= 0 and remoteNames[key] and simTime - remoteNames[key] > 3 then
      deleteMesh(key)
      remoteNames[key] = nil
    end
  end

  sendRaw("P")
  if not active then return end
  local stub = stubId and be:getObjectByID(stubId)
  if not stub then deactivate() return end

  if lastLocalFrame then
    local pos = lastLocalFrame.pos
    if not grid and (not gridCenter or (vec3(pos.x, pos.y, 0) - vec3(gridCenter.x, gridCenter.y, 0)):length() > GRID_REBUILD_DIST
        or math.abs(pos.z - gridCenter.z) > 4) then
      startGrid(pos)
    end
    stepGrid(GRID_SAMPLES_PER_FRAME)
    sendVehicles(pos, dt)
    checkVehicleHurt(pos, lastLocalFrame.vel, dt)
    sendMpState(dt)
    applyCamera()
  elseif connected then
    sendVehicles(nil)
  end
end

-- called from the vehicle VM (ng64Hit.sendHull) with its node height grid
local function onHull(id, cell, x0, y0, bottom, nx, ny, csv)
  local vals = {}
  for tok in string.gmatch(csv, "[^,]+") do vals[#vals + 1] = (tok == "n") and (0 / 0) or tonumber(tok) end
  if #vals ~= nx * ny then return end
  hullCount = hullCount + 1
  sendRaw("U" .. packU32(id) .. packF(cell, x0, y0, bottom) .. packU16(nx) .. packU16(ny) .. packF(unpack(vals)))
end

local function onVehicleSpawned(vid)
  local veh = be:getObjectByID(vid)
  if isStub(veh) and veh:getID() == be:getPlayerVehicleID(0) then
    activate(veh)
  end
end

local function onVehicleSwitched(oldId, newId)
  local veh = newId and be:getObjectByID(newId)
  if isStub(veh) then
    if not active or stubId ~= newId then activate(veh) end
  elseif active then
    deactivate()
  end
end

local function onVehicleDestroyed(vid)
  if active and vid == stubId then deactivate() end
end

local function onExtensionLoaded()
  openSocket()
  sendHello()
  log("I", logTag, "NG64 loaded")
  local veh = getPlayerVehicle(0)
  if isStub(veh) then activate(veh) end
end

local function onExtensionUnloaded()
  deactivate()
  for key in pairs(meshes) do deleteMesh(key) end
  if sock then sock:close() sock = nil end
end

local function onClientEndMission()
  deactivate()
  for key in pairs(meshes) do deleteMesh(key) end
  gridCenter = nil
end

-- UAT / console helpers
local function scriptInput(stickX, stickY, a, b, z, frames, dirX, dirY)
  local extra = dirX and packF(dirX, dirY) or ""
  sendRaw("I" .. packF(stickX or 0, stickY or 0) .. string.char(a and 1 or 0, b and 1 or 0, z and 1 or 0) .. packU16(frames or 1) .. extra)
end

local function teleport(x, y, z)
  -- collision around the destination first, so he doesn't arrive over nothing
  startGrid(vec3(x, y, z))
  stepGrid(GRID_N * GRID_N)
  sendRaw("M" .. packF(x, y, z))
end

local function getStatus()
  local f = lastLocalFrame
  return {
    connected = connected, active = active, stubId = stubId, material = materialName,
    pos = f and { f.pos.x, f.pos.y, f.pos.z }, health = f and f.health, action = f and f.action,
    numVerts = f and f.numVerts, frameAge = f and (simTime - localFrameTime), hits = hitCount, hurts = hurtCount, hulls = hullCount,
    meshes = (function() local n = 0 for _ in pairs(meshes) do n = n + 1 end return n end)(),
  }
end

M.onUpdate = onUpdate
M.onVehicleSpawned = onVehicleSpawned
M.onVehicleSwitched = onVehicleSwitched
M.onVehicleDestroyed = onVehicleDestroyed
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onClientEndMission = onClientEndMission
M.scriptInput = scriptInput
M.teleport = teleport
M.getStatus = getStatus
M.onRemote = onRemote
M.onHull = onHull
M.onRemoteGone = onRemoteGone
return M
