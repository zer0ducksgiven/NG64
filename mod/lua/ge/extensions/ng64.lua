-- NG64: Super Mario 64's Mario (via libsm64 in the NG64 helper process) as a BeamNG "vehicle".
--
-- Spawning the "Mario (NG64)" vehicle turns it into an invisible anchor and hands control to Mario. The helper runs
-- the physics and renders the mesh; this side feeds it collision (terrain raycasts + vehicle boxes), draws what it
-- sends back, drives the camera, and relays hits/damage between Mario and vehicles.
local M = {}
local logTag = "ng64"

local ffi = require("ffi")
local world = require("ge/extensions/ng64World")
local hud = require("ge/extensions/ng64Hud")

local HELPER_HOST, HELPER_PORT = "127.0.0.1", 47064
local PROTO_VERSION = 13
local STUB_MODEL = "ng64_mario"

local GRID_N, GRID_SP = 49, 0.5         -- terrain sample grid around Mario (24 m square)
local GRID_REBUILD_DIST = 5             -- metres Mario may stray from the grid centre before resampling
local GRID_SAMPLES_PER_FRAME = 700
local VEHICLE_RANGE = 80
local MP_SEND_INTERVAL = 1 / 15

-- the struct names carry the protocol version: ffi types outlive a Lua reload, and an old definition with the same
-- name would silently stay in place
pcall(ffi.cdef, [[
#pragma pack(push, 1)
typedef struct {
  uint8_t type; uint32_t key; uint32_t seq;
  float pos[3]; float vel[3]; float faceAngle;
  int16_t health; uint32_t action; int16_t animId; int16_t animFrame; uint32_t flags;
  float camPos[3]; float camTarget[3];
  uint16_t numVerts; uint16_t numParts; uint32_t tick;
} ng64v9_FrameHeader;
typedef struct { uint32_t hash; float pos[3]; float axes[3][3]; } ng64v9_PartPose;
typedef struct { uint8_t type; uint32_t key; uint8_t part; uint32_t hash; uint16_t nv; uint16_t ni; } ng64v9_PartHeader;
typedef struct { int16_t p[3]; int8_t n[3]; uint16_t uv[2]; } ng64v9_PackedVert;
typedef struct { uint8_t type; uint32_t vehId; float point[3]; float dir[3]; float strength; } ng64v9_Hit;
typedef struct { uint8_t type; uint8_t kind; uint32_t vehId; uint8_t piece; uint8_t heavy; float point[3]; float yaw; float vel[3]; } ng64v9_Carry;
#pragma pack(pop)
]])
local FRAME_HEADER_SIZE = ffi.sizeof("ng64v9_FrameHeader")
local PART_POSE_SIZE = ffi.sizeof("ng64v9_PartPose")
local PART_HEADER_SIZE = ffi.sizeof("ng64v9_PartHeader")
local VERT_SIZE = ffi.sizeof("ng64v9_PackedVert")

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

local meshes = {}             -- key -> { parts = { [part] = { looks = { [hash] = look }, shown = look } }, pos, vel }
local pendingFrames = {}      -- key -> { hist = recent poses, cur = newest }
local goneKeys = {}           -- remote players who left: frames still in flight for them are dropped
local remoteNames = {}

local grid                    -- in-progress terrain sample job
local gridCenter              -- vec3 of the last sent grid

local mpAccum = 0
local hitCount, hurtCount, hullCount, carDentCount, meshBuilds = 0, 0, 0, 0, 0   -- for UAT
local hullPieces = {}   -- vehicle id -> pieces in its last hull
local profCreate = 0   -- seconds spent in createMesh, for tuning
local materialChanged = false   -- the material was recreated: meshes built with the old one must be rebuilt
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
  materialChanged = true
  log("I", logTag, "mario material " .. name .. " -> " .. path)
end

-- ------------------------------------------------------------------------------------------------------------
-- rendering


-- BeamNG's sandboxed ffi refuses pointer casts/arithmetic, so packets are ffi.copy'd into typed scratch buffers
local hdrBuf = ffi.new("ng64v9_FrameHeader")
local partHdrBuf = ffi.new("ng64v9_PartHeader")
local hitBuf = ffi.new("ng64v9_Hit")
local carryBuf = ffi.new("ng64v9_Carry")
local chunkVerts = ffi.new("ng64v9_PackedVert[?]", 700)
local chunkIdx = ffi.new("uint16_t[?]", 4000)
local partPoses = ffi.new("ng64v9_PartPose[?]", 64)

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

local poseGaps, lastPoseArrival, lastPoseTick = {}, nil, nil   -- local poses arriving late (helper stalls), tests

local function addPose(key, pose)
  local pf = pendingFrames[key]
  if not pf then pf = { hist = {} } pendingFrames[key] = pf end
  local h = pf.hist
  if h[#h] and pose.t <= h[#h].t then h = {} pf.hist = h end   -- ticks went backwards: helper restarted
  h[#h + 1] = pose
  if #h > HISTORY then table.remove(h, 1) end
  pf.cur = pose
end

-- A part's axes (rotation * scale, bng) -> rotation and per-axis scale for setPosRot / setScale
local function partTransform(ax, ay, az)
  local sx, sy, sz = ax:length(), ay:length(), az:length()
  if sy < 1e-6 or sz < 1e-6 then return quat(0, 0, 0, 1), vec3(math.max(sx, 1e-4), math.max(sy, 1e-4), math.max(sz, 1e-4)) end
  local y, z = ay / sy, az / sz
  local x = y:cross(z)
  if ax:dot(x) < 0 then sx = -sx end   -- mirrored part
  return quatFromDir(y, z), vec3(sx, sy, sz)
end

-- Mario is drawn as his rigid body parts. Each look a part has (geometry hash) is built once into its own mesh and
-- kept; every rendered frame only moves the parts and shows the right look. Rebuilding the whole mesh every frame
-- (as NG64 used to) made BeamNG slower and slower the longer the game ran.
local MAX_LOOKS_PER_PART = 8
local frameNo = 0
local lookSerial = 0
local partRequests = {}   -- "key:part:hash" -> simTime last asked for
local poseUpdates = 0     -- parts moved (tests)

local function meshEntry(key)
  local e = meshes[key]
  if not e then e = { parts = {} } meshes[key] = e end
  return e
end

local function deleteLook(look)
  if look.obj then look.obj:delete() look.obj = nil end
end

local function dropLooks()
  for _, e in pairs(meshes) do
    for _, ps in pairs(e.parts) do
      for _, look in pairs(ps.looks) do deleteLook(look) end
      ps.looks, ps.shown = {}, nil
    end
  end
end

-- Loading another level deletes the material: recreate it, and rebuild whatever was drawn with the old one
local function checkMaterial()
  if materialName and not scenetree.findObject(materialName) then ensureMaterial(atlasPath) end
  if materialChanged then materialChanged = false dropLooks() end
end

local function requestPart(key, part, hash)
  local id = key .. ":" .. part .. ":" .. hash
  local last = partRequests[id]
  if last and simTime - last < 0.25 then return end
  partRequests[id] = simTime
  sendRaw("B" .. packU32(key) .. string.char(part - 1) .. packU32(hash))
end

local function onPartGeometry(data)
  if #data < PART_HEADER_SIZE then return end
  ffi.copy(partHdrBuf, data, PART_HEADER_SIZE)
  local key, part, hash = tonumber(partHdrBuf.key), tonumber(partHdrBuf.part) + 1, tonumber(partHdrBuf.hash)
  local nv, ni = tonumber(partHdrBuf.nv), tonumber(partHdrBuf.ni)
  if (key == 0 and not active) or goneKeys[key] or not materialName then return end
  if nv > 700 or ni > 4000 or #data < PART_HEADER_SIZE + nv * VERT_SIZE + ni * 2 then return end
  local e = meshEntry(key)
  local ps = e.parts[part]
  if not ps then ps = { looks = {} } e.parts[part] = ps end
  if ps.looks[hash] then return end
  checkMaterial()
  ffi.copy(chunkVerts, string.sub(data, PART_HEADER_SIZE + 1), nv * VERT_SIZE)
  ffi.copy(chunkIdx, string.sub(data, PART_HEADER_SIZE + 1 + nv * VERT_SIZE), ni * 2)
  local verts, normals, uvs, faces = {}, {}, {}, {}
  for i = 0, nv - 1 do
    local pv = chunkVerts[i]
    verts[i + 1] = { x = pv.p[0] * 0.001, y = pv.p[1] * 0.001, z = pv.p[2] * 0.001 }
    normals[i + 1] = { x = pv.n[0] / 127, y = pv.n[1] / 127, z = pv.n[2] / 127 }
    uvs[i + 1] = { u = pv.uv[0] / 65535, v = pv.uv[1] / 65535 }
  end
  for i = 0, ni - 1 do local k = chunkIdx[i] faces[i + 1] = { v = k, n = k, u = k } end

  lookSerial = lookSerial + 1
  local obj = createObject("ProceduralMesh")
  obj:registerObject("ng64_mario_" .. key .. "_" .. part .. "_" .. lookSerial)
  obj.canSave = false
  scenetree.MissionGroup:addObject(obj.obj)
  obj:setHidden(true)
  local t0 = os.clock()
  obj:createMesh({ { { verts = verts, normals = normals, uvs = uvs, faces = faces, material = materialName } } })
  profCreate = profCreate + (os.clock() - t0)
  meshBuilds = meshBuilds + 1
  ps.looks[hash] = { obj = obj, builtFrame = frameNo, used = simTime }

  -- keep the looks this part uses most recently
  local n, oldest, oldestHash = 0, math.huge, nil
  for h, look in pairs(ps.looks) do
    n = n + 1
    if look ~= ps.shown and h ~= hash and look.used < oldest then oldest, oldestHash = look.used, h end
  end
  if n > MAX_LOOKS_PER_PART and oldestHash then deleteLook(ps.looks[oldestHash]) ps.looks[oldestHash] = nil end
end

local function nlerpQuat(a, b, t)
  local bx, by, bz, bw = b.x, b.y, b.z, b.w
  if a.x * bx + a.y * by + a.z * bz + a.w * bw < 0 then bx, by, bz, bw = -bx, -by, -bz, -bw end
  local x, y, z, w = a.x + (bx - a.x) * t, a.y + (by - a.y) * t, a.z + (bz - a.z) * t, a.w + (bw - a.w) * t
  local l = math.sqrt(x * x + y * y + z * z + w * w)
  if l < 1e-9 then return a.x, a.y, a.z, a.w end
  return x / l, y / l, z / l, w / l
end

-- this rendered frame's pose: blend the two poses either side of the render time, part by part
local function drawMario(key, pf)
  local p0, p1, a = pairFor(pf)
  if not p0 then return end
  local e = meshEntry(key)
  e.pos = p0.pos + (p1.pos - p0.pos) * a
  e.vel = p1.vel
  local near = a < 0.5 and p0 or p1
  for i, q1 in pairs(p1.parts) do
    local q0 = p0.parts[i] or q1
    local nq = near.parts[i] or q1
    local ps = e.parts[i]
    if not ps then ps = { looks = {} } e.parts[i] = ps end
    local look = nq.hash ~= 0 and ps.looks[nq.hash] or nil
    if nq.hash ~= 0 and not look then requestPart(key, i, nq.hash) end
    -- createMesh leaves an object blank until it has been drawn once: a look built this frame waits a frame
    if look and look.builtFrame == frameNo then look = nil end
    local show = look or (nq.hash ~= 0 and ps.shown) or nil
    if show ~= ps.shown then
      if ps.shown and ps.shown.obj then ps.shown.obj:setHidden(true) end
      ps.shown = show
      if show then show.obj:setHidden(false) end
    end
    if show and show.obj then
      show.used = simTime
      local pos = q0.pos + (q1.pos - q0.pos) * a
      local rx, ry, rz, rw = nlerpQuat(q0.rot, q1.rot, a)
      show.obj:setPosRot(pos.x, pos.y, pos.z, rx, ry, rz, rw)
      local sc = q0.scale + (q1.scale - q0.scale) * a
      if not show.scale or (show.scale - sc):squaredLength() > 1e-8 then show.obj:setScale(sc) show.scale = sc end
      poseUpdates = poseUpdates + 1
    end
  end
  for i, ps in pairs(e.parts) do
    if not p1.parts[i] and ps.shown then
      if ps.shown.obj then ps.shown.obj:setHidden(true) end
      ps.shown = nil
    end
  end
end

local function deleteMesh(key)
  local e = meshes[key]
  if e then
    for _, ps in pairs(e.parts) do
      for _, look in pairs(ps.looks) do deleteLook(look) end
    end
  end
  meshes[key] = nil
  pendingFrames[key] = nil
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

-- Whether the level has a terrain at all, checked once per level. On a level without one (a map built entirely from
-- meshes, like an SM64 castle grounds port) every getTerrainHeight call still costs ~0.16 ms and returns nothing:
-- 700 of them a frame froze the game for ~100 ms, four frames in a row, every 5 m Mario moved.
-- A "no terrain" answer is only trusted for a couple of seconds: asked while a big level is still streaming in, it
-- would otherwise leave Mario with no ground for the whole session.
local terrainLevel, levelHasTerrain, terrainCheckedAt
local function hasTerrain()
  local level = getMissionFilename and getMissionFilename() or ""
  local now = os.clock()
  if terrainLevel ~= level or (not levelHasTerrain and now - (terrainCheckedAt or 0) > 2) then
    terrainLevel, terrainCheckedAt = level, now
    levelHasTerrain = #(scenetree.findClassObjects("TerrainBlock") or {}) > 0
  end
  return levelHasTerrain
end

local function sampleHeight(x, y, refZ)
  local h = hasTerrain() and core_terrain and core_terrain.getTerrainHeight and core_terrain.getTerrainHeight(vec3(x, y, refZ))
  if not h or h ~= h or h < -1e5 or h > 1e5 then
    return groundPlane() or 0 / 0
  end
  return h
end

local gridStarts = 0
local function startGrid(center)
  gridStarts = gridStarts + 1
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

-- The map's objects around Mario, streamed as fixed 16 m cells (ng64World.CELL): each cell is sent once when it
-- comes within CELL_RANGE of his cell and removed once he's CELL_DROP away, so running around only sends the new
-- edge. (Re-sending the whole 64 m region every 8 m was up to 128k triangles at a time on Gridmap v2: a stutter
-- every second or so.) Cells are sent nearest first, a few milliseconds' worth per frame.
local CELL_RANGE, CELL_DROP = 1, 2         -- cells: Mario always has at least 16 m of map around him
local CELL_ZR, CELL_REZ = 30, 12           -- a cell covers +- CELL_ZR m of Mario's height, redone if he moves CELL_REZ
local CELL_BUDGET = 0.003                  -- seconds of cell work per frame
local cells = {}                           -- key -> { ix, iy, z, dirty, pending, tris }
local cellSends, meshTris, meshLevel = 0, 0, nil

local function cellId(ix, iy) return (ix + 32768) % 65536 + ((iy + 32768) % 65536) * 65536 end

local function sendCell(c)
  local bufs, nTris, pending = world.cellChunks(c.ix, c.iy, c.z, CELL_ZR)
  local per = world.CHUNK_TRIS
  local chunks = math.max(1, math.ceil(nTris / per))
  local id = packU32(cellId(c.ix, c.iy))
  for k = 0, chunks - 1 do
    local count = math.max(0, math.min(per, nTris - k * per))
    sendRaw("O" .. id .. packU16(k) .. packU16(chunks) .. packU16(count) .. ffi.string(bufs[k + 1], count * 36))
  end
  meshTris = meshTris - (c.tris or 0) + nTris
  c.tris, c.pending, c.dirty = nTris, pending, false
  cellSends = cellSends + 1
end

local function clearCells()
  cells, meshTris = {}, 0
  sendRaw("Y" .. packU32(0xFFFFFFFF))
end

-- A level built from Super Mario 64 (the SM64 map port) ships sm64_surfaces.json: its original collision with SM64
-- surface types (slippery roof, currents...) and water boxes. Those go to the helper as they are, and the shapes
-- they replace are left out of the generic mesh streaming.
local levelSurfaces, levelSurfacesSent

local function loadLevelSurfaces(level)
  levelSurfaces, levelSurfacesSent = nil, false
  local dir = string.match(level, "^(.*)/[^/]*$")
  local data = dir and FS:fileExists(dir .. "/sm64_surfaces.json") and jsonReadFile(dir .. "/sm64_surfaces.json")
  if data and data.format == 1 and data.triangles then
    levelSurfaces = data
    log("I", logTag, string.format("level has SM64 surfaces: %d triangles, %d water boxes", #data.triangles, #(data.water or {})))
  end
end

local SURF_CHUNK = 190                       -- 40 bytes each: a chunk stays under LuaSocket's 8 KB
local function sendLevelSurfaces()
  if not connected then return end
  levelSurfacesSent = true
  local tris = levelSurfaces and levelSurfaces.triangles or {}
  if #tris == 0 then
    sendRaw("G" .. packU16(0) .. packU16(0) .. packU16(0))
  else
    local chunks = math.ceil(#tris / SURF_CHUNK)
    for k = 0, chunks - 1 do
      local parts = {}
      local first, last = k * SURF_CHUNK + 1, math.min(#tris, (k + 1) * SURF_CHUNK)
      for i = first, last do
        local t = tris[i]
        parts[#parts + 1] = packU16(t[1]) .. packI16(t[2]) .. packF(t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11])
      end
      sendRaw("G" .. packU16(k) .. packU16(chunks) .. packU16(last - first + 1) .. table.concat(parts))
    end
  end
  local water = levelSurfaces and levelSurfaces.water or {}
  local w = { string.char(#water) }
  for _, b in ipairs(water) do w[#w + 1] = packF(b.min[1], b.min[2], b.max[1], b.max[2], b.z) end
  sendRaw("J" .. table.concat(w))
  if not levelSurfaces then   -- a map that isn't an SM64 port: the water BeamNG draws
    local beam = world.water()
    local parts = { string.char(#beam) }
    for _, b in ipairs(beam) do parts[#parts + 1] = packF(b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]) end
    sendRaw("j" .. table.concat(parts))
    log("I", logTag, string.format("water: %d volumes", #beam))
  end
end

local function checkLevel()
  local level = getMissionFilename and getMissionFilename() or ""
  if meshLevel ~= level then
    meshLevel = level
    local t0 = os.clock()
    loadLevelSurfaces(level)
    local skip
    if levelSurfaces and levelSurfaces.replacesShapes then
      skip = {}
      for _, sh in ipairs(levelSurfaces.replacesShapes) do skip[sh] = true end
    end
    local n = world.index(skip)
    clearCells()
    log("I", logTag, string.format("indexed %d colliding map objects in %.2f s", n, os.clock() - t0))
  end
  if not levelSurfacesSent then sendLevelSurfaces() end
end

local PREFETCH_RADIUS, prefetchAt = 120, 0
local parseLeft = math.huge

-- keep the cells around pos: add new ones, drop far ones, (re)send whatever needs it within budget seconds
local function updateCells(pos, budget)
  budget = budget or CELL_BUDGET
  checkLevel()
  if simTime >= prefetchAt then
    prefetchAt = simTime + 2
    world.prefetch(pos.x, pos.y, PREFETCH_RADIUS)
  end
  local left = world.step(0.004)
  local parsedMore = left < parseLeft
  parseLeft = left
  local cs = world.CELL
  local cx, cy = math.floor(pos.x / cs), math.floor(pos.y / cs)
  for ix = cx - CELL_RANGE, cx + CELL_RANGE do
    for iy = cy - CELL_RANGE, cy + CELL_RANGE do
      local key = cellId(ix, iy)
      local c = cells[key]
      if not c then
        cells[key] = { ix = ix, iy = iy, z = pos.z, dirty = true }
      elseif math.abs(c.z - pos.z) > CELL_REZ then
        c.z, c.dirty = pos.z, true
      elseif parsedMore and (c.pending or 0) > 0 then
        c.dirty = true                       -- more of the shapes it needed are ready now
      end
    end
  end
  local todo
  for key, c in pairs(cells) do
    if math.abs(c.ix - cx) > CELL_DROP or math.abs(c.iy - cy) > CELL_DROP then
      sendRaw("Y" .. packU32(key))
      meshTris = meshTris - (c.tris or 0)
      cells[key] = nil
    elseif c.dirty then
      todo = todo or {}
      todo[#todo + 1] = c
    end
  end
  if not todo then return end
  table.sort(todo, function(a, b)
    return math.abs(a.ix - cx) + math.abs(a.iy - cy) < math.abs(b.ix - cx) + math.abs(b.iy - cy)
  end)
  local t0 = os.clock()
  for _, c in ipairs(todo) do
    sendCell(c)
    if os.clock() - t0 > budget then break end
  end
end

-- all cells around p at once (spawn, teleport), giving shape parsing a few seconds first so he arrives on solid
-- ground; anything still parsing after that follows in updateCells
local function loadCellsNow(p)
  checkLevel()
  local deadline = os.clock() + 3
  local cs = world.CELL
  local cx, cy = math.floor(p.x / cs), math.floor(p.y / cs)
  while os.clock() < deadline do
    local pending = 0
    for ix = cx - CELL_RANGE, cx + CELL_RANGE do
      for iy = cy - CELL_RANGE, cy + CELL_RANGE do
        local _, _, pd = world.cellChunks(ix, iy, p.z, CELL_ZR)
        pending = pending + pd
      end
    end
    if pending == 0 then break end
    world.step(0.25)
  end
  for _, c in pairs(cells) do c.dirty = true end
  updateCells(p, math.huge)
end

local function finishActivate(veh)
  local p = veh:getPosition()
  -- the map objects around the spawn point first (blocking, once), so he has walls and floors from frame one
  loadCellsNow(p)
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

local restoreCameraUntil = -1   -- after Mario goes, keep handing the camera back to the game for a moment

-- BeamNG's own bindings on the keys and buttons Mario uses are switched off while the player is Mario
-- (setControlled / deactivate): its big map is on Back / M (the music toggle), J pauses, E opens the radial menu,
-- WASD moves the free camera Mario's camera runs in, Space / arrows are the parking brake, throttle and steering.
-- (One table, "fixes", for these helpers: this file is at Lua's limit of 200 top-level locals.)
local fixes = {}

-- Fire: every car reports its own burning nodes (ng64Hit, from BeamNG's fire module); the ones near Mario go to the helper
-- ~10 times a second as flame spheres, and it sets him alight when one touches him. Each report is kept for 0.6 s.
fixes.fires = {}      -- vehicle id -> { t = simTime, pts = { {x, y, z, intensity}, ... } }
fixes.fireAt, fixes.fireSentN = 0, 0
function M.onFire(id, ...)
  if select("#", ...) == 0 then fixes.fires[id] = nil return end
  local a, pts = { ... }, {}
  for i = 1, #a - 3, 4 do pts[#pts + 1] = { a[i], a[i + 1], a[i + 2], a[i + 3] } end
  fixes.fires[id] = { t = simTime, pts = pts }
end

function fixes.sendFire(marioPos, dt)
  fixes.fireAt = fixes.fireAt - dt
  if fixes.fireAt > 0 then return end
  fixes.fireAt = 0.1
  local out, n = {}, 0
  for id, f in pairs(fixes.fires) do
    if simTime - f.t > 0.6 then
      fixes.fires[id] = nil
    else
      for _, p in ipairs(f.pts) do
        if n < 32 and (p[1] - marioPos.x) ^ 2 + (p[2] - marioPos.y) ^ 2 + (p[3] - marioPos.z) ^ 2 < 36 then
          -- a hotter node burns bigger: 0.4 m for a flicker up to a metre for a blaze (nodes sit inside the bodywork: flames lick out well past them)
          out[#out + 1] = packF(p[1], p[2], p[3], 0.4 + 0.6 * math.min(1, p[4] / 0.4))
          n = n + 1
        end
      end
    end
  end
  if n > 0 or fixes.fireSentN > 0 then sendRaw("k" .. string.char(n) .. table.concat(out)) end
  fixes.fireSentN = n
end

-- while he burns, flames come off him (BeamNG's own fire particles, from the anchor vehicle's nodes)
fixes.burning = { [0x00020449] = true, [0x010208B4] = true, [0x010208B5] = true }
function fixes.burnEffect(action, dt)
  fixes.burnAt = (fixes.burnAt or 0) - dt
  if not fixes.burning[action or 0] or fixes.burnAt > 0 then return end
  fixes.burnAt = 0.03   -- about every frame: he moves fast, and the flames are left where they were made
  local stub = stubId and be:getObjectByID(stubId)
  if stub then
    stub:queueLuaCommand("for _, c in ipairs({0, 3, 8}) do obj:addParticleByNodesRelative(c, 3, -1, 27, 0, 1) obj:addParticleByNodesRelative(c, 3, -1, 25, 0, 1) end obj:addParticleByNodesRelative(3, 8, -1, 29, 0, 1)")
  end
end
fixes.marioBlockedActions = {
  "toggleBigMap", "pause", "toggleRadialMenuMulti", "parkingbrake", "parkingbrake_toggle", "steadycamJump",
  "moveforward", "movebackward", "moveleft", "moveright",
  "accelerate", "brake", "steer_left", "steer_right",
  -- BeamNG's own camera turning: it fights Mario's camera, and each turn wakes its vehicle-trigger crosshair
  "rotate_camera_horizontal", "rotate_camera_vertical", "rotate_camera_hz_mouse", "rotate_camera_vt_mouse",
  "rotate_camera_left", "rotate_camera_right", "rotate_camera_up", "rotate_camera_down",
}
function fixes.blockBigMap(block)
  if not core_input_actionFilter then return end
  core_input_actionFilter.setGroup("ng64Mario", fixes.marioBlockedActions)
  core_input_actionFilter.addAction(0, "ng64Mario", block)
end

local function deactivate()
  pendingActivateId = nil
  fixes.blockBigMap(false)
  if not active then return end
  active = false
  sendRaw("D")
  deleteMesh(0)
  lastLocalFrame = nil
  lastPoseArrival = nil
  hud.deactivate()
  if commands.isFreeCamera() then commands.setGameCamera() end
  -- a vehicle replace puts the previous camera mode (Mario's free camera) back after spawning: retry a while
  restoreCameraUntil = simTime + 1.5
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
  elseif kind == 4 then
    -- Bowser-style grab: he's about to spin it. point = where he stands (the car's vehicle side picks the end to hold)
    carryingId = id
    carryCount = carryCount + 1
    hitGrace[id] = math.huge
    veh:queueLuaCommand(string.format("if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.spinStart(%f,%f,%f,%d)",
      c.point[0], c.point[1], c.point[2], stubId or 0))
    log("I", logTag, string.format("mario grabbed vehicle %d to spin it", id))
  elseif kind == 5 then
    -- spinning: point = his feet, yaw = the direction (bng, radians) the car points out from him, vel = { the rate it
    -- turns round him (rad/s), how far along that direction his gloves are (m), how high the car's centre rides (m) }
    veh:queueLuaCommand(string.format("if ng64Hit then ng64Hit.spinTarget(%f,%f,%f,%f,%f,%f,%f) end",
      c.point[0], c.point[1], c.point[2], c.yaw, c.vel[0], c.vel[1], c.vel[2]))
  elseif kind == 3 then
    carryingId = nil
    hitGrace[id] = simTime     -- the usual grace after it leaves his hands
    if c.vel[0] ~= 0 or c.vel[1] ~= 0 then throwCount = throwCount + 1 end
    -- point is the tumble (angular velocity, rad/s) of a spin throw; zero for the ordinary throw
    veh:queueLuaCommand(string.format("if ng64Hit then ng64Hit.carryRelease(%f,%f,%f,%f,%f,%f) end",
      c.vel[0], c.vel[1], c.vel[2], c.point[0], c.point[1], c.point[2]))
    log("I", logTag, string.format("mario released vehicle %d (%.1f, %.1f, %.1f m/s, tumble %.1f %.1f %.1f rad/s)", id,
      c.vel[0], c.vel[1], c.vel[2], c.point[0], c.point[1], c.point[2]))
  end
end

-- the spinning car hit something (reported by the car's own Lua): Mario lets go, and whatever it struck is dented and
-- shoved the way the car was moving
function M.onSpinHit(selfId, otherId, px, py, pz, dx, dy, dz, frac)
  sendRaw("b" .. packU32(selfId))
  hitGrace[selfId] = simTime
  local other = otherId ~= 0 and be:getObjectByID(otherId)
  if other and otherId ~= stubId then
    other:queueLuaCommand(string.format(
      "if not ng64Hit then extensions.load('ng64Hit') end ng64Hit.hit(%f,%f,%f,%f,%f,%f,%f)",
      px, py, pz, dx, dy, dz, 2 + 6 * frac))
  end
  log("I", logTag, string.format("spinning vehicle %d hit %s at %.1f%% of full spin", selfId,
    other and ("vehicle " .. otherId) or "the world", frac * 100))
end

local floorReply   -- last MSG_FLOOR_REPLY (tests)
local lastToast    -- last message the helper asked to show (tests)

local function handlePacket(data)
  local t = string.sub(data, 1, 1)
  if t == "F" then
    if #data < FRAME_HEADER_SIZE then return end
    ffi.copy(hdrBuf, data, FRAME_HEADER_SIZE)
    local h = hdrBuf
    local key = tonumber(h.key)
    if (key == 0 and not active) or goneKeys[key] then return end
    local np = math.min(tonumber(h.numParts), 64)
    if #data < FRAME_HEADER_SIZE + np * PART_POSE_SIZE then return end
    ffi.copy(partPoses, string.sub(data, FRAME_HEADER_SIZE + 1), np * PART_POSE_SIZE)
    local pos, vel = vec3(h.pos[0], h.pos[1], h.pos[2]), vec3(h.vel[0], h.vel[1], h.vel[2])
    local tick = tonumber(h.tick)
    if key == 0 then
      local now = wallTime()
      if lastPoseArrival and now - lastPoseArrival > 0.1 then
        poseGaps[#poseGaps + 1] = { t = simTime, gap = now - lastPoseArrival, ticks = tick - (lastPoseTick or tick) }
        if now - lastPoseArrival > 0.15 then
          log("W", logTag, string.format("mario stalled: no pose from the helper for %.0f ms (%d ticks)", (now - lastPoseArrival) * 1000, tick - (lastPoseTick or tick)))
        end
        if #poseGaps > 40 then table.remove(poseGaps, 1) end
      end
      lastPoseArrival, lastPoseTick = now, tick
    end
    noteTick(tick)
    local parts = {}
    for i = 0, np - 1 do
      local pp = partPoses[i]
      local ax = pp.axes
      local rot, scale = partTransform(vec3(ax[0][0], ax[0][1], ax[0][2]), vec3(ax[1][0], ax[1][1], ax[1][2]), vec3(ax[2][0], ax[2][1], ax[2][2]))
      parts[i + 1] = { hash = tonumber(pp.hash), pos = vec3(pp.pos[0], pp.pos[1], pp.pos[2]), rot = rot, scale = scale }
    end
    local pose = { t = tick * TICK, pos = pos, vel = vel, parts = parts }
    if key == 0 then
      pose.camPos = vec3(h.camPos[0], h.camPos[1], h.camPos[2])
      pose.camTarget = vec3(h.camTarget[0], h.camTarget[1], h.camTarget[2])
      framesStarted = framesStarted + 1
      framesCompleted = framesCompleted + 1
    end
    addPose(key, pose)
    if key == 0 then
      lastLocalFrame = {
        pos = pos, vel = vel,
        faceAngle = h.faceAngle, health = h.health, action = tonumber(h.action),
        animId = h.animId, animFrame = h.animFrame, flags = tonumber(h.flags), numVerts = tonumber(h.numVerts),
      }
      localFrameTime = simTime
    end
  elseif t == "E" then
    onPartGeometry(data)
  elseif t == "W" then
    local ok = string.byte(data, 2) == 1
    local msg, path = string.match(string.sub(data, 3), "^([^%z]*)%z([^%z]*)")
    if ok then
      -- the reply to our hello says whether the helper wrote the HUD graphics (a later "atlas" resend doesn't)
      if msg and msg:sub(1, 2) == "ok" then hud.setImages(msg:find("hud") ~= nil) end
      if not connected then log("I", logTag, "connected to NG64 helper") end
      connected = true
      levelSurfacesSent = false          -- a (re)started helper has none of them
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
  elseif t == "n" then
    fixes.ents.onEntities(data)
  elseif t == "v" then
    if #data >= 20 then fixes.ents.onEvent(data) end
  elseif t == "L" then
    local msg = string.sub(data, 2, -2)
    local longToast = string.match(msg, "^toastl:(.*)")
    if longToast then
      -- a notice that stays up (a new version is out): with a close button, 20 s
      guihooks.trigger("toastrMsg", { type = "info", title = "NG64 update", msg = longToast,
        config = { closeButton = true, timeOut = 20000, extendedTimeOut = 5000 } })
      lastToast = longToast
      return
    end
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

-- Frame-time profile: how long each part of onUpdate took. A frame that took long (the next update's dt) is kept
-- with the previous update's section times, so a stutter can be traced to whatever NG64 was doing (tests, tuning).
local profT, profCur, profLast = 0, {}, {}
local hitches = {}
local function prof(name)
  local now = os.clock()
  profCur[name] = (profCur[name] or 0) + (now - profT)
  profT = now
end

local lastUpdateWall, lastLuaKB
-- start of every update: note the last frame if it was long, and start timing this one
local function noteFrame()
  -- measured on the wall clock: the dt BeamNG passes in doesn't show a long stall (a 1.5 s freeze came in as ~16 ms)
  local wallNow = wallTime()
  local frameDt = lastUpdateWall and (wallNow - lastUpdateWall) or 0
  lastUpdateWall = wallNow
  local luaKB = collectgarbage("count")
  local luaDropMB = lastLuaKB and (lastLuaKB - luaKB) / 1024 or 0   -- memory freed since last frame: a GC cycle ran
  lastLuaKB = luaKB
  if frameDt > 0.03 then
    local rec = { t = simTime, dt = frameDt, luaMB = luaKB / 1024, gcFreedMB = luaDropMB, sections = profCur, builds = meshBuilds, worldTris = meshTris, world = world.stats(), grids = gridStarts }
    hitches[#hitches + 1] = rec
    if #hitches > 40 then table.remove(hitches, 1) end
    if frameDt > 0.05 then
      local parts = {}
      for k, v in pairs(profCur) do if v > 0.002 then parts[#parts + 1] = string.format("%s %.0f ms", k, v * 1000) end end
      log("W", logTag, string.format("stutter: frame took %.0f ms (NG64: %s; map cells sent %s, part builds %d; Lua %.0f MB, %.0f MB freed)", frameDt * 1000,
        #parts > 0 and table.concat(parts, ", ") or "under 2 ms", tostring(cellSends), meshBuilds, luaKB / 1024, luaDropMB))
    end
  end
  profLast, profCur = profCur, {}
  profT = os.clock()
end

-- The helper only reads the controller while the game has focus, and it can't tell for itself (under Wine/Proton it
-- can't see the game's window): tell it on every change, and once a second in case a packet was lost.
local focusSent, focusSentAt = nil, -10
local function sendFocus()
  local ok, f = pcall(Engine.isProgramFocused)
  if not ok or f == nil then ok, f = pcall(isWindowFocused) end
  if not ok or f == nil then f = true end   -- no way to ask: never block input
  if f ~= focusSent or simTime - focusSentAt > 1 then
    focusSent, focusSentAt = f, simTime
    sendRaw("Z" .. string.char(f and 1 or 0))
  end
end

-- AI traffic only avoids what's in BeamNG's object list, and Mario's anchor isn't in it (it has no wheels, so the
-- game never tracks it): traffic drove straight through him. Every frame he's put in the list as the anchor,
-- where he actually is and the way he's facing - moving, or stopped (so traffic goes round him rather than
-- queueing behind him for ever).
fixes.trafficStates = { ignitionLevel = 2 }
function fixes.tellTraffic(pos, f)
  if not (map and map.tempObjectData and stubId) then return end
  local vel = f.vel or vec3(0, 0, 0)
  local fa = f.faceAngle or 0
  local dir = vec3(math.sin(fa), -math.cos(fa), 0)   -- sm64 facing -> bng heading
  map.tempObjectData(stubId, true, vec3(pos.x, pos.y, pos.z), vec3(vel.x, vel.y, vel.z), dir, vec3(0, 0, 1), 0)
  local o = map.objects and map.objects[stubId]
  if o then
    fixes.trafficStates.ignitionLevel = vel:squaredLength() > 1 and 2 or 0
    o.states = fixes.trafficStates
  end
end

-- Last resort: Mario more than 2 m under the terrain surface (fell through before the ground under him was loaded)
-- goes back on top of it. Checked once a second.
function fixes.checkUnderTerrain(pos)
  if simTime - (fixes.terrainCheckAt or -10) < 1 then return end
  fixes.terrainCheckAt = simTime
  if not (hasTerrain() and core_terrain and core_terrain.getTerrainHeight) then return end
  local h = core_terrain.getTerrainHeight(vec3(pos.x, pos.y, pos.z))
  if h and h == h and h > -1e5 and pos.z < h - 2 then
    log("W", logTag, string.format("mario was %.1f m under the terrain at %.1f %.1f: put back on top", h - pos.z, pos.x, pos.y))
    M.teleport(pos.x, pos.y, h + 0.5)
  end
end

local lastMaterialCheck
local function onUpdate(dtReal, dtSim, dtRaw)
  noteFrame()
  local dt = dtReal or 0.016
  simTime = simTime + dt
  if not sock then return end
  pump()
  prof("pump")

  if connected and simTime - lastRecvTime > 3 then
    connected = false
    log("W", logTag, "lost connection to NG64 helper")
  end
  if not connected and simTime - lastHelloTime > 1 then
    sendHello()
    if active and not warnedNoHelper and simTime - lastHelloTime > 0.5 then
      warnedNoHelper = true
      guihooks.trigger("toastrMsg", { type = "warning", title = "NG64", msg = "The NG64 helper isn't running. Start NG64 from the Start menu (or run the NG64 installer again) to play as Mario." })
    end
  end

  -- draw every Mario: move his parts to this rendered frame's pose
  frameNo = frameNo + 1
  if materialChanged or simTime - (lastMaterialCheck or -1) > 1 then
    lastMaterialCheck = simTime
    checkMaterial()
  end
  for key, pf in pairs(pendingFrames) do drawMario(key, pf) end
  prof("draw")
  for key in pairs(meshes) do
    if key ~= 0 and remoteNames[key] and simTime - remoteNames[key] > 3 then
      deleteMesh(key)
      remoteNames[key] = nil
    end
  end

  -- motion trace for measuring smoothness (what was actually drawn this frame)
  if traceOn then
    local e = meshes[0]
    local cp = commands.isFreeCamera() and core_camera.getPosition() or nil
    local mp = e and e.pos
    local pf = pendingFrames[0]
    local _, _, al = pairFor(pf or {})
    al = al or -1
    local ahead = (pf and pf.cur) and (renderTime() - pf.cur.t) or 0
    trace[#trace + 1] = { simTime, dt, mp and mp.x or 0, mp and mp.y or 0, mp and mp.z or 0, cp and cp.x or 0, cp and cp.y or 0, cp and cp.z or 0,
      al, ahead, pf and pf.cur and pf.cur.t or 0 }
    if #trace > 600 then table.remove(trace, 1) end
  end

  sendRaw("P")
  sendFocus()
  hud.update(simTime, lastLocalFrame, active, controlled)
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
  if not active then
    if simTime < restoreCameraUntil and commands.isFreeCamera() then commands.setGameCamera() end
    return
  end
  local stub = stubId and be:getObjectByID(stubId)
  if not stub or not isStub(stub) then deactivate() return end

  if lastLocalFrame then
    local pos = lastLocalFrame.pos
    if not grid and (not gridCenter or (vec3(pos.x, pos.y, 0) - vec3(gridCenter.x, gridCenter.y, 0)):length() > GRID_REBUILD_DIST
        or math.abs(pos.z - gridCenter.z) > 4) then
      startGrid(pos)
    end
    prof("misc")
    stepGrid(GRID_SAMPLES_PER_FRAME)
    prof("grid")
    updateCells(pos)
    prof("world")
    sendVehicles(pos, dt)
    prof("vehicles")
    checkVehicleHurt(pos, lastLocalFrame.vel, dt)
    fixes.tellTraffic(pos, lastLocalFrame)
    fixes.sendFire(pos, dt)
    fixes.ents.update(dt, pos)
    fixes.burnEffect(lastLocalFrame.action, dt)
    fixes.checkUnderTerrain(pos)
    sendMpState(dt)
    if controlled then applyCamera() end
    prof("camera")
    followStub(stub, pos)
    prof("stub")
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
  -- the vehicle spawner's "replace" swaps the vehicle but keeps its id: Mario's anchor is now an ordinary car.
  -- Treating it as the anchor kept pulling the car onto Mario (and Mario back to it) with the camera stuck on him.
  if active and vid == stubId and not isStub(veh) then
    log("I", logTag, "mario's anchor was replaced by another vehicle")
    deactivate()
    return
  end
  if isStub(veh) and veh:getID() == be:getPlayerVehicleID(0) then
    activate(veh)
  end
end

-- Switching to another vehicle (TAB) leaves Mario in the world, standing where he is: he just stops taking input
-- and the camera goes back to the game's. Switching back to him hands control back.
setControlled = function(on)
  controlled = on
  fixes.blockBigMap(on)
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
  meshLevel = nil
  cells, meshTris = {}, 0
  world.reset()
  fixes.ents.reset()
end

-- UAT / console helpers
-- flags (tests): y = press Y, music = press the music toggle
local function scriptInput(stickX, stickY, a, b, z, frames, dirX, dirY, y, music, song, hold)
  local flags = (y and 1 or 0) + (music and 2 or 0) + (song == 1 and 4 or 0) + (song == -1 and 8 or 0) + (hold and 16 or 0)
  local extra = dirX and (packF(dirX, dirY) .. (flags > 0 and string.char(flags) or "")) or ""
  sendRaw("I" .. packF(stickX or 0, stickY or 0) .. string.char(a and 1 or 0, b and 1 or 0, z and 1 or 0) .. packU16(frames or 1) .. extra)
end

local function teleport(x, y, z, reset)
  -- collision around the destination first, so he doesn't arrive over nothing
  startGrid(vec3(x, y, z))
  stepGrid(GRID_N * GRID_N)
  loadCellsNow(vec3(x, y, z))
  sendRaw("M" .. packF(x, y, z) .. (reset and string.char(1) or ""))
end

local function getStatus()
  local f = lastLocalFrame
  return {
    connected = connected, active = active, controlled = controlled, stubId = stubId, material = materialName,
    pos = f and { f.pos.x, f.pos.y, f.pos.z }, health = f and f.health, action = f and f.action,
    numVerts = f and f.numVerts, frameAge = f and (simTime - localFrameTime), hits = hitCount, hurts = hurtCount, hulls = hullCount, carDents = carDentCount, meshBuilds = meshBuilds, poseUpdates = poseUpdates, hullPieces = hullPieces, carrying = carryingId, carries = carryCount, throws = throwCount, worldTris = meshTris, world = world.stats(), lastToast = lastToast, fires = (function() local n = 0 for _, f in pairs(fixes.fires) do n = n + #f.pts end return n end)(), profCreate = profCreate, framesStarted = framesStarted, framesCompleted = framesCompleted,
    meshes = (function() local n = 0 for _ in pairs(meshes) do n = n + 1 end return n end)(),
    hud = hud.getState(),
  }
end

M.onUpdate = onUpdate
M.getHitches = function() return { frames = hitches, poseGaps = poseGaps } end
M.clearHitches = function() hitches, poseGaps = {}, {} end
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
-- SM64 HUD: lives, coins, stars (ng64.hud.collectCoin / collectStar / addLife, for maps and other mods)
M.hud = hud
-- pickups / enemies / music settings (the NG64 settings app calls these)
fixes.ents = require("ge/extensions/ng64Ents")
fixes.ents.init({
  sendRaw = sendRaw, hud = hud, stubId = function() return stubId end,
  toast = function(msg) guihooks.trigger("toastrMsg", { type = "info", title = "NG64", msg = msg }) lastToast = msg end,
})
M.ents = fixes.ents
M.setOption = fixes.ents.setOption
M.resendSettings = fixes.ents.resendSettings
M.onGameStateUpdate = hud.onGameStateUpdate
hud.setHealer(function(healCounter) sendRaw("h" .. string.char(math.max(0, math.min(255, healCounter)))) end)
-- tests: hurt Mario by this many wedges, from a point beside him
M.testHurt = function(wedges)
  local f = lastLocalFrame
  if not f then return end
  sendRaw("K" .. packF(f.pos.x + 1, f.pos.y, f.pos.z) .. string.char(wedges or 1, 0))
end
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
