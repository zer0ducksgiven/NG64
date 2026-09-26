-- NG64: Super Mario 64's Mario (via libsm64 in the NG64 helper process) as a BeamNG "vehicle".
--
-- Spawning the "Mario (NG64)" vehicle turns it into an invisible anchor and hands control to Mario. The helper runs
-- the physics and renders the mesh; this side feeds it collision (terrain raycasts + vehicle boxes), draws what it
-- sends back, drives the camera, and relays hits/damage between Mario and vehicles.
local M = {}
local logTag = "ng64"

local ffi = require("ffi")
local world = require("ge/extensions/ng64World")

local HELPER_HOST, HELPER_PORT = "127.0.0.1", 47064
local PROTO_VERSION = 6
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
  uint16_t numVerts; uint16_t numIndices; uint32_t indexHash; uint32_t tick;
} ng64_FrameHeader;
typedef struct { uint8_t type; uint32_t key; uint32_t seq; uint16_t start; uint16_t count; } ng64_ChunkHeader;
typedef struct { int16_t p[3]; int8_t n[3]; uint16_t uv[2]; } ng64_PackedVert;
typedef struct { uint8_t type; uint32_t vehId; float point[3]; float dir[3]; float strength; } ng64_Hit;
typedef struct { uint8_t type; uint8_t kind; uint32_t vehId; uint8_t piece; uint8_t heavy; float point[3]; float yaw; float vel[3]; } ng64_Carry;
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

local active = false          -- local Mario exists
local controlled = false      -- ...and the player is controlling him (not another vehicle)
local setControlled           -- defined with the vehicle-switch handling below
local stubId                  -- vehicle id of the anchor vehicle
local lastLocalFrame          -- decoded header fields of the latest local frame
local localFrameTime = 0
local prevCam, curCam         -- {pos, target, t}

local meshes = {}             -- key -> { obj, lastFrame }
local pendingFrames = {}      -- key -> frame being reassembled from chunks
local goneKeys = {}           -- remote players who left: frames still in flight for them are dropped
local remoteNames = {}

local grid                    -- in-progress terrain sample job
local gridCenter              -- vec3 of the last sent grid

local mpAccum = 0
local hitCount, hurtCount, hullCount, carDentCount, meshBuilds = 0, 0, 0, 0, 0   -- for UAT
local hullPieces = {}   -- vehicle id -> pieces in its last hull
local profCreate, profBlend = 0, 0   -- seconds spent in createMesh / blending, for tuning
local traceOn, trace = false, {}
local framesStarted, framesCompleted = 0, 0   -- local Mario frames begun vs fully received (dropped packets)
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
  if not path or path == "" then return end
  -- same atlas and the material still exists: nothing to do. Loading another level deletes it, and Mario was then
  -- drawn with a material that no longer existed - invisible on whichever map you went to next.
  if path == atlasPath and materialName and scenetree.findObject(materialName) then return end
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
local carryBuf = ffi.new("ng64_Carry")
local chunkVerts = ffi.new("ng64_PackedVert[?]", 600)

local chunkIdx = ffi.new("uint16_t[?]", 3000)

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
  end
end

local function fillIndices(pool, start, count)
  local idx = pool.idx
  for i = 0, count - 1 do idx[start + i + 1] = chunkIdx[i] end
end

local function newPool() return { verts = {}, normals = {}, uvs = {}, idx = {}, faces = {} } end

local function trim(t, n, upto)
  for j = n + 1, upto do t[j] = nil end
end

local NUM_MESH_BUFFERS = 3

-- Poses are timed by the helper's simulation tick (exactly 1/30 s apart), not by when they arrive: Lua only reads the
-- socket once per rendered frame, so arrival times are lumpy, and blending by them made Mario stall every few
-- frames. clockOffset maps ticks onto simTime (the earliest arrival seen, drifting up slowly to follow the helper);
-- everything is drawn RENDER_DELAY behind that, i.e. between the last two poses.
local TICK = 1 / 30
-- Wall clock for all of this: summed frame deltas drift against the helper's real-time ticks, and
-- getSystemTimeMS only moves in ~16 ms steps.
local wallTimer = hptimer()
local function wallTime() return wallTimer:stop() * 0.001 end

-- Over the last ~2 s of poses: the earliest arrival (relative to its tick) fixes where ticks sit on the wall clock,
-- and the spread of arrivals is how much jitter the delay has to absorb. A window rather than an all-time minimum,
-- because a helper hiccup shifts its ticks against the wall clock for good.
local ARRIVAL_WINDOW = 60
local arrivals, arrivalHead = {}, 0
local clockOffset, arrivalSpread = nil, 0.01

local function noteTick(tick)
  local est = wallTime() - tick * TICK
  arrivalHead = arrivalHead % ARRIVAL_WINDOW + 1
  arrivals[arrivalHead] = est
  local lo, hi = math.huge, -math.huge
  for _, e in pairs(arrivals) do
    if e < lo then lo = e end
    if e > hi then hi = e end
  end
  if clockOffset and lo - clockOffset > 0.5 then arrivals = { est } arrivalHead = 1 lo, hi = est, est end   -- helper restarted
  clockOffset, arrivalSpread = lo, math.min(0.1, hi - lo)
end

local function renderTime()
  local now = wallTime()
  return now - (clockOffset or now) - (TICK + arrivalSpread + 0.005)
end

-- The two poses either side of the render time, and how far between them (0..1). Poses don't arrive evenly (Lua
-- reads the socket once per rendered frame), so a short history is kept and the render time runs a fixed delay
-- behind the newest: with only the last two, an early pose made Mario wait and a late one made him hold.
local HISTORY = 5
local function pairFor(pf)
  local h = pf.hist
  local n = h and #h or 0
  if n == 0 then return nil end
  if n == 1 then return h[1], h[1], 1 end
  local rt = renderTime()
  if rt >= h[n].t then return h[n - 1], h[n], 1 end
  if rt <= h[1].t then return h[1], h[2], 0 end
  for i = n - 1, 1, -1 do
    if h[i].t <= rt then
      local p0, p1 = h[i], h[i + 1]
      return p0, p1, (rt - p0.t) / math.max(1e-6, p1.t - p0.t)
    end
  end
  return h[1], h[2], 0
end

local function blendable(p0, p1)
  return p0 ~= p1 and p0.ihash == p1.ihash and p0.nv == p1.nv
end

local MAX_BUILDS_PER_SECOND = 60   -- createMesh is BeamNG's cost; blending faster than this isn't visible anyway

-- A finished 30 Hz pose becomes "current", the old current becomes "previous", and the next one is filled into
-- whichever pool is neither. Rendering blends previous -> current, so Mario moves smoothly at the game's frame rate
-- (one sm64 frame behind, like the camera) instead of stepping at 30 Hz.
local function completeFrame(pf)
  local filled = pf.pools[pf.fill]
  local nv, ni = pf.nv, pf.ni
  trim(filled.verts, nv, filled.nv or 0)
  trim(filled.normals, nv, filled.nv or 0)
  trim(filled.uvs, nv, filled.nv or 0)
  -- the face list only changes when the topology does (hash), so rebuild it only then
  if filled.facesHash ~= pf.ihash or filled.ni ~= ni then
    local faces, idx = filled.faces, filled.idx
    for j = 1, ni do
      local k = idx[j]
      local f = faces[j]
      if f then f.v, f.n, f.u = k, k, k else faces[j] = { v = k, n = k, u = k } end
    end
    trim(faces, ni, filled.ni or 0)
    filled.facesHash = pf.ihash
  end
  filled.nv, filled.ni = nv, ni
  local frame = { pool = filled, nv = nv, ihash = pf.ihash, pos = pf.pos, vel = pf.vel, t = pf.tick * TICK,
                  camPos = pf.camPos, camTarget = pf.camTarget }
  pf.hist = pf.hist or {}
  local h = pf.hist
  if h[#h] and frame.t <= h[#h].t then h = {} pf.hist = h end   -- ticks went backwards: helper restarted
  h[#h + 1] = frame
  if #h > HISTORY then table.remove(h, 1) end
  pf.cur = frame
  -- fill the next pose into a pool no kept pose is using
  local used = {}
  for _, f in ipairs(h) do used[f.pool] = true end
  pf.fill = nil
  for i, p in ipairs(pf.pools) do if not used[p] then pf.fill = i break end end
  if not pf.fill then pf.pools[#pf.pools + 1] = newPool() pf.fill = #pf.pools end
  pf.dirty = true
end

-- The pose to draw this rendered frame. Returns the full vertex set when a rebuild is due (a new pose, or the blend
-- moved on and the 60/s build cap allows it); otherwise just the blended position, which is applied every frame so
-- Mario's movement through the world stays smooth even between rebuilds.
local function blendedFrame(pf)
  local p0, p1, a = pairFor(pf)
  if not p0 then return nil end
  local mix = blendable(p0, p1)
  if not mix then
    if a < 0.5 then p1 = p0 end   -- topology changed between them: show whichever is nearer
    a = 1
  end
  local pos = mix and (p0.pos * (1 - a) + p1.pos * a) or p1.pos
  local pairChanged = pf.lastP1 ~= p1 or pf.lastP0 ~= p0
  local rebuild = pf.dirty and pairChanged or pairChanged
    or (mix and math.abs(a - (pf.lastAlpha or -1)) > 0.02 and simTime - (pf.lastBuild or -1) >= 1 / MAX_BUILDS_PER_SECOND)
  pf.dirty = false
  if not rebuild then return { pos = pos } end
  pf.lastP0, pf.lastP1, pf.lastAlpha, pf.lastBuild = p0, p1, a, simTime
  if not mix or a >= 1 then
    return { verts = p1.pool.verts, normals = p1.pool.normals, uvs = p1.pool.uvs, faces = p1.pool.faces, nv = p1.nv, pos = pos, vel = p1.vel }
  end
  local out, pp, cp, b = pf.out, p0.pool, p1.pool, 1 - a
  for j = 1, p1.nv do
    local vo = out.verts[j]; if not vo then vo = {}; out.verts[j] = vo end
    local v0, v1 = pp.verts[j], cp.verts[j]
    vo.x, vo.y, vo.z = v0.x * b + v1.x * a, v0.y * b + v1.y * a, v0.z * b + v1.z * a
    local no = out.normals[j]; if not no then no = {}; out.normals[j] = no end
    local n0, n1 = pp.normals[j], cp.normals[j]
    no.x, no.y, no.z = n0.x * b + n1.x * a, n0.y * b + n1.y * a, n0.z * b + n1.z * a
  end
  trim(out.verts, p1.nv, out.nv or 0)
  trim(out.normals, p1.nv, out.nv or 0)
  out.nv = p1.nv
  return { verts = out.verts, normals = out.normals, uvs = cp.uvs, faces = cp.faces, nv = p1.nv, pos = pos, vel = p1.vel }
end

-- Only ever called once per onUpdate, with the newest complete frame. createMesh leaves an object blank until the
-- next rendered frame, so each pose goes into a mesh that isn't on screen, is shown, and the previous one is only
-- hidden on the next update, once the new one has been drawn. Three meshes means the one being rebuilt is never the one still waiting to be hidden.
local function buildMesh(key, r)
  local nv = r.nv
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
  if materialName and not scenetree.findObject(materialName) then ensureMaterial(atlasPath) end
  if nv > 0 and materialName then
    -- next mesh that is neither on screen nor waiting to be hidden
    local back
    for k = 1, NUM_MESH_BUFFERS - 1 do
      local i = (entry.front + k - 1) % NUM_MESH_BUFFERS + 1
      if not entry.hiding[i] then back = i break end
    end
    if not back then return end
    local obj = entry.objs[back]
    meshBuilds = meshBuilds + 1
    local t0 = os.clock()
    obj:createMesh({ { { verts = r.verts, normals = r.normals, uvs = r.uvs, faces = r.faces, material = materialName } } })
    profCreate = profCreate + (os.clock() - t0)
    obj:setPosition(r.pos)
    obj:setHidden(false)
    if entry.obj then entry.hiding[entry.front] = 2 end
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
  pendingFrames[key] = nil   -- or the next update would rebuild it from the last pose
end

-- ------------------------------------------------------------------------------------------------------------
-- terrain sampling (raycasts) -> heightfield for the helper

local down = vec3(0, 0, -1)
local up = vec3(0, 0, 1)

-- Terrain only: the map's objects (buildings, ramps, walls, rocks...) come in as their real collision triangles
-- (ng64World). Sampling them from above is what made Mario clip: raycasts can't see walls, overhangs or anything
-- with more than one level. Terrain is a heightfield, which a grid of samples does capture.
-- Levels without terrain (smallgrid) stand on a GroundPlane instead: an endless flat plane at its own height.
local groundPlaneZ, groundPlaneLevel
local function groundPlane()
  local level = getMissionFilename and getMissionFilename() or ""
  if groundPlaneLevel ~= level then
    groundPlaneLevel, groundPlaneZ = level, nil
    for _, n in ipairs(scenetree.findClassObjects("GroundPlane") or {}) do
      local o = scenetree.findObject(n)
      if o then groundPlaneZ = math.max(groundPlaneZ or -math.huge, o:getPosition().z) end
    end
  end
  return groundPlaneZ
end

local function sampleHeight(x, y, refZ)
  local h = core_terrain and core_terrain.getTerrainHeight and core_terrain.getTerrainHeight(vec3(x, y, refZ))
  if not h or h ~= h or h < -1e5 or h > 1e5 then
    return groundPlane() or 0 / 0
  end
  return h
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
local hullDamage = {}               -- vehicle id -> its damage when the hull was last requested
local HULL_REFRESH_WRECKING = 0.3

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
        -- a car that's still being wrecked changes shape (and falls apart) fast: re-read it far more often then
        local dmg = map.objects[id] and map.objects[id].damage or 0
        local changing = dmg - (hullDamage[id] or dmg) > 50
        local age = (hullAge[id] or HULL_REFRESH) + (dt or 0)
        hullAge[id] = age
        if age >= (changing and HULL_REFRESH_WRECKING or HULL_REFRESH) then
          hullDamage[id] = dmg
          requestHull(veh)
        end
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
          -- the car takes a small dent where it hit him: a light, speed-scaled push into the body, no shove
          local contact = marioPos + vec3(0, 0, 0.6)
          local into = -(flat / flat:length())
          veh:queueLuaCommand(string.format(
            "if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,%f,%f,%f,%f,0)",
            contact.x, contact.y, contact.z, into.x, into.y, into.z, math.min(0.9, math.max(0.5, speed / 15))))   -- bumpers ignore anything under ~0.5
          carDentCount = carDentCount + 1
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
  -- same clock and the same two poses as Mario's mesh, so they can never drift against each other
  local pf = pendingFrames[0]
  local p0, p1, a = pairFor(pf or {})
  if not p0 or not p1.camPos then return end
  local pos, target = p1.camPos, p1.camTarget
  if p0.camPos and p0 ~= p1 then
    pos = p0.camPos + (p1.camPos - p0.camPos) * a
    target = p0.camTarget + (p1.camTarget - p0.camTarget) * a
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

-- The anchor is invisible and pinned, so on its own it would stay where Mario was spawned and a reset would put
-- him back there. Keep it under him (only while he's on the ground, so a reset never drops him mid-air).
local STUB_FOLLOW_INTERVAL = 1.0
local stubMovedAt, stubFollowAt, pendingReset, ownMovePending = -10, 0, nil, false
local function followStub(stub, pos)
  if pendingReset and simTime >= pendingReset then
    pendingReset = nil
    local p = stub:getPosition()
    M.teleport(p.x, p.y, p.z + 0.3, true)   -- defined further down
    log("I", logTag, string.format("anchor reset: mario to %.2f %.2f %.2f", p.x, p.y, p.z))
    return
  end
  if simTime < stubFollowAt or not lastLocalFrame then return end
  stubFollowAt = simTime + STUB_FOLLOW_INTERVAL
  local airborne = bit.band(lastLocalFrame.action or 0, 0x800) ~= 0
  if airborne or (stub:getPosition() - pos):length() < 1.0 then return end
  stubMovedAt, ownMovePending = simTime, true
  stub:setPosRot(pos.x, pos.y, pos.z + 0.05, 0, 0, 0, 1)
end

-- Mario is only brought in once the level has finished loading and there's real ground under the spawn point.
-- When a level loads, BeamNG respawns the last player vehicle - Mario - while the map's objects are still loading;
-- on a big map (West Coast USA takes seconds) the terrain scan then found nothing and he spawned with no collision.
local pendingActivateId, pendingActivateAt, waitingLogged = nil, 0, false

local function groundUnder(p)
  local hits = 0
  for i = -2, 2 do
    for j = -2, 2 do
      if castRayStatic(vec3(p.x + i * 2, p.y + j * 2, p.z + 3), down, 60) < 60 then hits = hits + 1 end
    end
  end
  return hits >= 5
end

local function activate(veh)
  openSocket()
  if not connected then sendHello() end
  stubId = veh:getID()
  pendingActivateId, pendingActivateAt, waitingLogged = stubId, simTime, false
end

-- the map's objects around Mario: re-sent whenever he's moved this far from the last region's centre
local MESH_RADIUS, MESH_HEIGHT, MESH_REFRESH = 32, 25, 8
local MESH_TRIS_PER_PACKET = 200            -- 200 * 36 bytes stays under LuaSocket's 8 KB datagrams
local meshCenter, meshRegionId, meshWanted, meshTris, meshLevel = nil, 0, nil, 0, nil

local function sendMesh(tris)
  meshRegionId = meshRegionId + 1
  local nTris = #tris / 9
  local chunks = math.max(1, math.ceil(nTris / MESH_TRIS_PER_PACKET))
  local buf = ffi.new("float[?]", MESH_TRIS_PER_PACKET * 9)
  for c = 0, chunks - 1 do
    local first = c * MESH_TRIS_PER_PACKET
    local count = math.min(MESH_TRIS_PER_PACKET, nTris - first)
    for i = 0, count * 9 - 1 do buf[i] = tris[first * 9 + i + 1] end
    sendRaw("O" .. packU32(meshRegionId) .. packU16(c) .. packU16(chunks) .. packU16(count) .. ffi.string(buf, count * 36))
  end
  meshTris = nTris
end

-- Send the region around p with every shape that's parsed so far; if some are still being parsed, send it again
-- (updateMesh) each time more of them are ready. Blocking (spawn, teleport) gives parsing a few seconds first.
local meshPending = 0

local function requestMesh(p, blocking)
  local level = getMissionFilename and getMissionFilename() or ""
  if meshLevel ~= level then
    meshLevel = level
    local t0 = os.clock()
    local n = world.index()
    log("I", logTag, string.format("indexed %d colliding map objects in %.2f s", n, os.clock() - t0))
  end
  local deadline = os.clock() + (blocking and 3 or 0)
  local tris, pending = world.region(p.x, p.y, p.z, MESH_RADIUS, MESH_HEIGHT)
  while pending > 0 and os.clock() < deadline do
    world.step(0.25)
    tris, pending = world.region(p.x, p.y, p.z, MESH_RADIUS, MESH_HEIGHT)
  end
  sendMesh(tris)
  meshCenter, meshPending = vec3(p.x, p.y, p.z), pending
  meshWanted = pending > 0 and meshCenter or nil
end

local PREFETCH_RADIUS, prefetchAt = 120, 0

local function updateMesh(pos)
  if simTime >= prefetchAt then
    prefetchAt = simTime + 2
    world.prefetch(pos.x, pos.y, PREFETCH_RADIUS)
  end
  local left = world.step(0.004)
  if meshWanted and left < meshPending then
    requestMesh(meshWanted, false)       -- more shapes finished: resend the fuller region
  elseif not meshCenter or (vec3(pos.x, pos.y, 0) - vec3(meshCenter.x, meshCenter.y, 0)):length() > MESH_REFRESH
      or math.abs(pos.z - meshCenter.z) > MESH_HEIGHT * 0.5 then
    requestMesh(pos, false)
  end
end

local function finishActivate(veh)
  local p = veh:getPosition()
  -- the map objects around the spawn point first (blocking, once), so he has walls and floors from frame one
  requestMesh(p, true)
  -- synchronous grid so Mario has a floor the moment he spawns
  startGrid(p)
  stepGrid(GRID_N * GRID_N)
  sendVehicles(p)
  sendRaw("S" .. packF(p.x, p.y, p.z + 0.2))
  active = true
  setControlled(true)
  prevCam, curCam = nil, nil
  log("I", logTag, string.format("mario activated at %.2f %.2f %.2f", p.x, p.y, p.z))
  if not connected then
    guihooks.trigger("toastrMsg", { type = "warning", title = "NG64", msg = "Waiting for the NG64 helper (ng64helper.exe)..." })
  end
end

local function deactivate()
  pendingActivateId = nil
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
  goneKeys[key] = nil
  sendRaw("R" .. packU32(key) .. packF(v[1], v[2], v[3], v[4]) .. packU32(v[5]) .. packI16(v[6]) .. packI16(v[7]) .. packU32(v[8]))
  remoteNames[key] = simTime
end

local function onRemoteGone(data)
  local key = (tonumber(data) or -1) + 1
  if key <= 0 then return end
  goneKeys[key] = true
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

-- Mario carrying a car / wreck piece: the helper decides (SM64 does the lift, carry, throw), the car's own Lua
-- holds the piece at the point it's given and applies the throw
local carryingId
local carryCount, throwCount = 0, 0   -- for UAT
local function onCarry(c)
  local id = tonumber(c.vehId)
  local veh = be:getObjectByID(id)
  if not veh then return end
  local kind = c.kind
  if kind == 1 then
    carryingId = id
    carryCount = carryCount + 1
    hitGrace[id] = math.huge   -- it's in his hands: never "hitting" him
    veh:queueLuaCommand(string.format("if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.carryStart(%d)", c.piece))
    log("I", logTag, string.format("mario picked up vehicle %d piece %d", id, c.piece))
  elseif kind == 2 then
    -- sm64 facing -> heading in the world: sm64 forward (sin f, cos f) maps to bng (sin f, -cos f)
    local f = c.yaw
    veh:queueLuaCommand(string.format("if ng64Hit then ng64Hit.carryTarget(%f,%f,%f,%f) end",
      c.point[0], c.point[1], c.point[2], math.atan2(-math.cos(f), math.sin(f))))
  elseif kind == 3 then
    carryingId = nil
    hitGrace[id] = simTime     -- the usual grace after it leaves his hands
    if c.vel[0] ~= 0 or c.vel[1] ~= 0 then throwCount = throwCount + 1 end
    veh:queueLuaCommand(string.format("if ng64Hit then ng64Hit.carryRelease(%f,%f,%f) end", c.vel[0], c.vel[1], c.vel[2]))
    log("I", logTag, string.format("mario released vehicle %d (%.1f, %.1f, %.1f m/s)", id, c.vel[0], c.vel[1], c.vel[2]))
  end
end

local floorReply   -- last MSG_FLOOR_REPLY (tests)
local lastToast    -- last message the helper asked to show (tests)

local function handlePacket(data)
  local t = string.sub(data, 1, 1)
  if t == "F" then
    if #data < ffi.sizeof(hdrBuf) then return end
    ffi.copy(hdrBuf, data, ffi.sizeof(hdrBuf))
    local h = hdrBuf
    local key = tonumber(h.key)
    if (key == 0 and not active) or goneKeys[key] then return end
    local nv = math.min(tonumber(h.numVerts), 3072)
    local ni = math.min(tonumber(h.numIndices), 3072)
    local pos, vel = vec3(h.pos[0], h.pos[1], h.pos[2]), vec3(h.vel[0], h.vel[1], h.vel[2])
    local pf = pendingFrames[key]
    if not pf then pf = { pools = { newPool(), newPool(), newPool() }, fill = 1, out = newPool() }; pendingFrames[key] = pf end
    pf.seq, pf.nv, pf.ni, pf.ihash, pf.got, pf.gotIdx, pf.pos, pf.vel = tonumber(h.seq), nv, ni, tonumber(h.indexHash), 0, 0, pos, vel
    pf.tick = tonumber(h.tick)
    noteTick(pf.tick)
    if key == 0 then
      pf.camPos = vec3(h.camPos[0], h.camPos[1], h.camPos[2])
      pf.camTarget = vec3(h.camTarget[0], h.camTarget[1], h.camTarget[2])
    end
    if key == 0 then framesStarted = framesStarted + 1 end
    if nv == 0 then completeFrame(pf) end
    if key == 0 then
      lastLocalFrame = {
        pos = pos, vel = vel,
        faceAngle = h.faceAngle, health = h.health, action = tonumber(h.action),
        animId = h.animId, animFrame = h.animFrame, flags = tonumber(h.flags), numVerts = nv,
      }
      localFrameTime = simTime
    end
  elseif t == "G" or t == "J" then
    if #data < CHUNK_HEADER_SIZE then return end
    ffi.copy(chunkHdrBuf, data, CHUNK_HEADER_SIZE)
    local key = tonumber(chunkHdrBuf.key)
    local pf = pendingFrames[key]
    if not pf or pf.seq ~= tonumber(chunkHdrBuf.seq) then return end
    local start, count = tonumber(chunkHdrBuf.start), tonumber(chunkHdrBuf.count)
    if t == "G" then
      if count > 600 or start + count > pf.nv or #data < CHUNK_HEADER_SIZE + count * VERT_SIZE then return end
      ffi.copy(chunkVerts, string.sub(data, CHUNK_HEADER_SIZE + 1), count * VERT_SIZE)
      fillVerts(pf.pools[pf.fill], start, count)
      pf.got = pf.got + count
    else
      if count > 3000 or start + count > pf.ni or #data < CHUNK_HEADER_SIZE + count * 2 then return end
      ffi.copy(chunkIdx, string.sub(data, CHUNK_HEADER_SIZE + 1), count * 2)
      fillIndices(pf.pools[pf.fill], start, count)
      pf.gotIdx = pf.gotIdx + count
    end
    if pf.got == pf.nv and pf.gotIdx == pf.ni then
      completeFrame(pf)
      if key == 0 then framesCompleted = framesCompleted + 1 end
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
  elseif t == "C" then
    if #data < ffi.sizeof(carryBuf) then return end
    ffi.copy(carryBuf, data, ffi.sizeof(carryBuf))
    onCarry(carryBuf)
  elseif t == "q" then
    -- test reply: SM64 floor heights for a floorQuery
    local n = string.byte(data, 6) + string.byte(data, 7) * 256
    local fb = ffi.new("float[?]", n)
    ffi.copy(fb, string.sub(data, 8), n * 4)
    local out = {}
    for i = 0, n - 1 do out[i + 1] = fb[i] end
    floorReply = out
  elseif t == "P" then
    -- keepalive reply
  elseif t == "L" then
    local msg = string.sub(data, 2, -2)
    local toast = string.match(msg, "^toast:(.*)")
    if toast then
      guihooks.trigger("toastrMsg", { type = "info", title = "NG64", msg = toast })
      lastToast = toast
    else
      log("W", logTag, "helper: " .. msg)
    end
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
    local t0 = os.clock()
    local r = blendedFrame(pf)
    profBlend = profBlend + (os.clock() - t0)
    if r and r.verts then
      buildMesh(key, r)
    elseif r and meshes[key] and meshes[key].obj then
      meshes[key].obj:setPosition(r.pos)
    end
  end

  -- smooth the meshes between 30 Hz frames
  for key, e in pairs(meshes) do
    for i, left in pairs(e.hiding) do
      if left <= 1 then e.objs[i]:setHidden(true) e.hiding[i] = nil else e.hiding[i] = left - 1 end
    end
    if key ~= 0 and remoteNames[key] and simTime - remoteNames[key] > 3 then
      deleteMesh(key)
      remoteNames[key] = nil
    end
  end

  -- motion trace for measuring smoothness (what was actually drawn this frame)
  if traceOn then
    local e = meshes[0]
    local cp = commands.isFreeCamera() and core_camera.getPosition() or nil
    local mp = e and e.obj and e.obj:getPosition()
    local pf = pendingFrames[0]
    local _, _, al = pairFor(pf or {})
    al = al or -1
    local ahead = (pf and pf.cur) and (renderTime() - pf.cur.t) or 0
    trace[#trace + 1] = { simTime, dt, mp and mp.x or 0, mp and mp.y or 0, mp and mp.z or 0, cp and cp.x or 0, cp and cp.y or 0, cp and cp.z or 0,
      al, ahead, pf and pf.cur and pf.cur.t or 0 }
    if #trace > 600 then table.remove(trace, 1) end
  end

  sendRaw("P")
  if pendingActivateId and simTime >= pendingActivateAt then
    local veh = be:getObjectByID(pendingActivateId)
    if not veh then
      pendingActivateId = nil
    elseif (worldReadyState == nil or worldReadyState == 2) and groundUnder(veh:getPosition()) then
      pendingActivateId = nil
      finishActivate(veh)
    else
      if not waitingLogged then log("I", logTag, "waiting for the level to finish loading before spawning Mario") waitingLogged = true end
      pendingActivateAt = simTime + 0.5
    end
  end
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
    updateMesh(pos)
    sendVehicles(pos, dt)
    checkVehicleHurt(pos, lastLocalFrame.vel, dt)
    sendMpState(dt)
    if controlled then applyCamera() end
    followStub(stub, pos)
  elseif connected then
    sendVehicles(nil)
  end
end

-- called from the vehicle VM (ng64Hit.sendHull) with its node height grid
local function onHull(id, cell, x0, y0, bottom, nx, ny, csv, index, count)
  index, count = index or 0, count or 1
  local vals = {}
  for tok in string.gmatch(csv, "[^,]+") do vals[#vals + 1] = (tok == "n") and (0 / 0) or tonumber(tok) end
  if #vals ~= nx * ny then return end
  hullCount = hullCount + 1
  hullPieces[id] = count
  sendRaw("U" .. packU32(id) .. packF(cell, x0, y0, bottom) .. packU16(nx) .. packU16(ny) .. string.char(index, count) .. packF(unpack(vals)))
end

local function onWorldReadyState(state)
  -- nothing to do here: finishActivate polls worldReadyState; kept so a level reload re-checks promptly
  if state == 2 and pendingActivateId then pendingActivateAt = simTime end
end

local function onVehicleSpawned(vid)
  local veh = be:getObjectByID(vid)
  if isStub(veh) and veh:getID() == be:getPlayerVehicleID(0) then
    activate(veh)
  end
end

-- Switching to another vehicle (TAB) leaves Mario in the world, standing where he is: he just stops taking input
-- and the camera goes back to the game's. Switching back to him hands control back.
setControlled = function(on)
  controlled = on
  sendRaw("N" .. string.char(on and 1 or 0))
  if not on and commands.isFreeCamera() then commands.setGameCamera() end
  log("I", logTag, on and "controlling mario" or "mario left standing; controlling another vehicle")
end

local function onVehicleSwitched(oldId, newId)
  local veh = newId and be:getObjectByID(newId)
  if isStub(veh) then
    if active and stubId == newId then setControlled(true)
    elseif not active or stubId ~= newId then activate(veh) end
  elseif active and controlled then
    setControlled(false)
  end
end

-- BeamNG's reset (R), recover and map teleports all act on the anchor vehicle; Mario goes where it went
local function onVehicleResetted(vid)
  if not active or vid ~= stubId then return end
  -- moving the anchor ourselves fires this event too: swallow exactly one, and only right after our own move
  if ownMovePending and simTime - stubMovedAt < 0.5 then ownMovePending = false return end
  pendingReset = simTime + 0.1                       -- position settles a moment after the reset event
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
  meshCenter, meshWanted, meshLevel = nil, nil, nil
  world.reset()
end

-- UAT / console helpers
-- flags (tests): y = press Y, music = press the music toggle
local function scriptInput(stickX, stickY, a, b, z, frames, dirX, dirY, y, music)
  local flags = (y and 1 or 0) + (music and 2 or 0)
  local extra = dirX and (packF(dirX, dirY) .. (flags > 0 and string.char(flags) or "")) or ""
  sendRaw("I" .. packF(stickX or 0, stickY or 0) .. string.char(a and 1 or 0, b and 1 or 0, z and 1 or 0) .. packU16(frames or 1) .. extra)
end

local function teleport(x, y, z, reset)
  -- collision around the destination first, so he doesn't arrive over nothing
  startGrid(vec3(x, y, z))
  stepGrid(GRID_N * GRID_N)
  requestMesh(vec3(x, y, z), true)
  sendRaw("M" .. packF(x, y, z) .. (reset and string.char(1) or ""))
end

local function getStatus()
  local f = lastLocalFrame
  return {
    connected = connected, active = active, controlled = controlled, stubId = stubId, material = materialName,
    pos = f and { f.pos.x, f.pos.y, f.pos.z }, health = f and f.health, action = f and f.action,
    numVerts = f and f.numVerts, frameAge = f and (simTime - localFrameTime), hits = hitCount, hurts = hurtCount, hulls = hullCount, carDents = carDentCount, meshBuilds = meshBuilds, hullPieces = hullPieces, carrying = carryingId, carries = carryCount, throws = throwCount, worldTris = meshTris, world = world.stats(), lastToast = lastToast, profCreate = profCreate, profBlend = profBlend, framesStarted = framesStarted, framesCompleted = framesCompleted,
    meshes = (function() local n = 0 for _ in pairs(meshes) do n = n + 1 end return n end)(),
  }
end

M.onUpdate = onUpdate
M.onVehicleSpawned = onVehicleSpawned
M.onVehicleSwitched = onVehicleSwitched
M.onVehicleDestroyed = onVehicleDestroyed
M.onVehicleResetted = onVehicleResetted
M.onWorldReadyState = onWorldReadyState
M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onClientEndMission = onClientEndMission
M.scriptInput = scriptInput
M.teleport = teleport
M.getStatus = getStatus
-- tests: ask the helper for SM64's floor height under each {x,y,z}; the answer arrives in a later frame (getFloorReply)
M.floorQuery = function(points)
  floorReply = nil
  local fb = ffi.new("float[?]", #points * 3)
  for i, p in ipairs(points) do fb[(i - 1) * 3], fb[(i - 1) * 3 + 1], fb[(i - 1) * 3 + 2] = p[1], p[2], p[3] end
  sendRaw("Q" .. packU32(1) .. packU16(#points) .. ffi.string(fb, #points * 12))
end
M.getFloorReply = function() return floorReply end
M.startTrace = function() trace = {} traceOn = true end
M.getTrace = function() traceOn = false local out = {} for i, r in ipairs(trace) do out[i] = table.concat(r, ' ') end return table.concat(out, string.char(10)) end
M.onRemote = onRemote
M.onHull = onHull
M.onRemoteGone = onRemoteGone
return M
