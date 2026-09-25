-- NG64 vehicle side: applies Mario's attacks to this vehicle, and describes its shape for Mario's collision.
-- GE calls ng64Hit.hit(px, py, pz, dx, dy, dz, strength) with a world-space hit point and direction, and
-- ng64Hit.sendHull() to get a height grid of the vehicle's nodes back through ng64.onHull.
local M = {}

local HIT_TIME = 0.06        -- seconds each hit's force is spread over
local SHOVE_DV = 1.1         -- m/s whole-vehicle velocity change per unit strength
local DENT_IMPULSE = 9000    -- N*s on the nodes nearest the hit, per unit strength (a punch visibly buckles a D-Series door)
local DENT_NODES = 12        -- how many of the nearest nodes share the dent
local DENT_RADIUS = 1.0      -- nodes further than this from the hit point are never dented

local HULL_CELL = 0.35

-- live tuning from the console: ng64Hit.tune(dentImpulse, shoveDv, dentNodes)
local function tune(dent, shove, nodes)
  DENT_IMPULSE = dent or DENT_IMPULSE
  SHOVE_DV = shove or SHOVE_DV
  DENT_NODES = nodes or DENT_NODES
end

local pending = {}
local carryUpdate, throwUpdate   -- defined with the carry code further down

local function hit(px, py, pz, dx, dy, dz, strength, shoveScale)
  local d = vec3(dx, dy, dz)
  if d:length() < 1e-4 then return end
  d:normalize()
  local p = vec3(px, py, pz)
  -- the nodes nearest the hit, found once; the panel can sit well inside the bounding box the hit was aimed at
  local base = obj:getPosition()
  local cand = {}
  for cid = 0, obj:getNodeCount() - 1 do
    local dist = (base + obj:getNodePosition(cid)):distance(p)
    if dist < DENT_RADIUS then cand[#cand + 1] = { cid, dist } end
  end
  table.sort(cand, function(a, b) return a[2] < b[2] end)
  local near, wsum = {}, 0
  for i = 1, math.min(DENT_NODES, #cand) do
    local w = 1 - cand[i][2] / DENT_RADIUS
    near[#near + 1] = { cand[i][1], w }
    wsum = wsum + w
  end
  pending[#pending + 1] = { d = d, s = strength or 1, t = HIT_TIME, near = near, wsum = wsum, shove = shoveScale or 1 }
  local info = string.format("hit strength %.1f at (%.2f %.2f %.2f) dir (%.2f %.2f %.2f): %d nodes within %.1f m, nearest %.2f m",
    strength or 1, px, py, pz, d.x, d.y, d.z, #near, DENT_RADIUS, cand[1] and cand[1][2] or -1)
  log("I", "ng64Hit", info)
end

local function updateGFX(dt)
  carryUpdate(dt)
  throwUpdate(dt)
  if #pending == 0 then return end
  local nodeCount = obj:getNodeCount()
  for i = #pending, 1, -1 do
    local h = pending[i]
    local step = math.min(dt, h.t)
    -- whole-vehicle shove: same acceleration on every node
    local shoveAccel = SHOVE_DV * h.s * h.shove / HIT_TIME
    if shoveAccel > 0 then
      for cid = 0, nodeCount - 1 do
        local mass = obj:getNodeMass(cid)
        if mass and mass > 0 then obj:applyForceVectorTime(cid, h.d * (mass * shoveAccel), step) end
      end
    end
    if h.wsum > 0 then
      local total = DENT_IMPULSE * h.s / HIT_TIME
      for _, n in ipairs(h.near) do
        obj:applyForceVectorTime(n[1], h.d * (total * n[2] / h.wsum), step)
      end
    end
    h.t = h.t - step
    if h.t <= 1e-4 then table.remove(pending, i) end
  end
end

local MAX_HULLS = 6          -- separate pieces (a truck's cab, chassis and bed box once it's torn apart)
local MIN_PIECE_NODES = 6    -- smaller loose bits (a mirror, an exhaust) aren't worth colliding with

-- groups of nodes still held together by unbroken beams: each piece of a wrecked car gets its own hull, instead of
-- one grid stretched over the empty space between the pieces (which showed up as invisible walls and floors)
local function pieces(nodeCount)
  local parent = {}
  for i = 0, nodeCount - 1 do parent[i] = i end
  local function find(i)
    while parent[i] ~= i do parent[i] = parent[parent[i]] i = parent[i] end
    return i
  end
  local beams = v.data.beams
  if beams then
    for b = 0, tableSizeC(beams) - 1 do
      local beam = beams[b]
      if beam and beam.id1 and beam.id2 and not obj:beamIsBroken(beam.cid) then
        local ra, rb = find(beam.id1), find(beam.id2)
        if ra ~= rb then parent[ra] = rb end
      end
    end
  end
  local groups = {}
  for i = 0, nodeCount - 1 do
    local r = find(i)
    groups[r] = groups[r] or {}
    table.insert(groups[r], i)
  end
  local list = {}
  for _, g in pairs(groups) do if #g >= MIN_PIECE_NODES then list[#list + 1] = g end end
  table.sort(list, function(x, y) return #x > #y end)
  return list, find
end

-- height grid of one piece's nodes and skin triangles, in the vehicle frame (x right, y forward, z up, origin
-- obj:getPosition())
local function pieceHull(pts, nodes, root, find, index, count)
  local minX, maxX, minY, maxY, minZ = math.huge, -math.huge, math.huge, -math.huge, math.huge
  for _, cid in ipairs(nodes) do
    local p = pts[cid + 1]
    if p[1] < minX then minX = p[1] end
    if p[1] > maxX then maxX = p[1] end
    if p[2] < minY then minY = p[2] end
    if p[2] > maxY then maxY = p[2] end
    if p[3] < minZ then minZ = p[3] end
  end

  local cell = math.max(HULL_CELL, (maxX - minX) / 64, (maxY - minY) / 64)
  local nx = math.max(1, math.ceil((maxX - minX) / cell))
  local ny = math.max(1, math.ceil((maxY - minY) / cell))
  local top = {}
  local function raise(k, z) if not top[k] or z > top[k] then top[k] = z end end
  -- nodes alone leave holes (a roof is a few nodes half a metre apart, and cells between them only hold seats or
  -- floor), so the car's collision triangles - its actual skin - are rasterized into the grid too
  for _, cid in ipairs(nodes) do
    local p = pts[cid + 1]
    local i = math.min(nx - 1, math.floor((p[1] - minX) / cell))
    local j = math.min(ny - 1, math.floor((p[2] - minY) / cell))
    raise(j * nx + i + 1, p[3])
  end
  local tris = v.data.triangles
  if tris then
    local sub = { { 0.5, 0.5 }, { 0.2, 0.2 }, { 0.8, 0.2 }, { 0.2, 0.8 }, { 0.8, 0.8 } }
    for t = 0, tableSizeC(tris) - 1 do
      local tri = tris[t]
      -- only this piece's triangles; one torn between two pieces would span the gap
      if find(tri.id1) == root and find(tri.id2) == root and find(tri.id3) == root then
        local a, b, c = pts[tri.id1 + 1], pts[tri.id2 + 1], pts[tri.id3 + 1]
        local det = (b[2] - c[2]) * (a[1] - c[1]) + (c[1] - b[1]) * (a[2] - c[2])
        if math.abs(det) > 1e-6 then   -- skip triangles seen edge-on from above (side panels)
          local i0 = math.max(0, math.floor((math.min(a[1], b[1], c[1]) - minX) / cell))
          local i1 = math.min(nx - 1, math.floor((math.max(a[1], b[1], c[1]) - minX) / cell))
          local j0 = math.max(0, math.floor((math.min(a[2], b[2], c[2]) - minY) / cell))
          local j1 = math.min(ny - 1, math.floor((math.max(a[2], b[2], c[2]) - minY) / cell))
          for j = j0, j1 do
            for i = i0, i1 do
              for _, s in ipairs(sub) do
                local x, y = minX + (i + s[1]) * cell, minY + (j + s[2]) * cell
                local w1 = ((b[2] - c[2]) * (x - c[1]) + (c[1] - b[1]) * (y - c[2])) / det
                local w2 = ((c[2] - a[2]) * (x - c[1]) + (a[1] - c[1]) * (y - c[2])) / det
                local w3 = 1 - w1 - w2
                if w1 >= -0.02 and w2 >= -0.02 and w3 >= -0.02 then
                  raise(j * nx + i + 1, w1 * a[3] + w2 * b[3] + w3 * c[3])
                end
              end
            end
          end
        end
      end
    end
  end
  -- fill gaps between nodes with the lowest neighbour so the shell has no holes
  local dirs = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }
  for _ = 1, 2 do
    local filled = {}
    for j = 0, ny - 1 do
      for i = 0, nx - 1 do
        local k = j * nx + i + 1
        if not top[k] then
          local n, lo = 0, math.huge
          for _, o in ipairs(dirs) do
            local ii, jj = i + o[1], j + o[2]
            if ii >= 0 and jj >= 0 and ii < nx and jj < ny then
              local t = top[jj * nx + ii + 1]
              if t then n = n + 1 lo = math.min(lo, t) end
            end
          end
          if n >= 2 then filled[k] = lo end
        end
      end
    end
    for k, val in pairs(filled) do top[k] = val end
  end

  local parts = {}
  for k = 1, nx * ny do parts[k] = top[k] and string.format("%.3f", top[k]) or "n" end
  obj:queueGameEngineLua(string.format("if ng64 then ng64.onHull(%d, %.4f, %.4f, %.4f, %.4f, %d, %d, %q, %d, %d) end",
    obj:getId(), cell, minX, minY, minZ, nx, ny, table.concat(parts, ","), index, count))
end

local function sendHull()
  local fwd = obj:getDirectionVector()
  local up = obj:getDirectionVectorUp()
  fwd:normalize()
  up:normalize()
  local right = fwd:cross(up)
  right:normalize()

  local nodeCount = obj:getNodeCount()
  if nodeCount == 0 then return end
  local pts = {}
  for cid = 0, nodeCount - 1 do
    local d = obj:getNodePosition(cid)
    pts[cid + 1] = { d:dot(right), d:dot(fwd), d:dot(up) }
  end
  local list, find = pieces(nodeCount)
  local count = math.min(MAX_HULLS, #list)
  for i = 1, count do
    pieceHull(pts, list[i], find(list[i][1]), find, i - 1, count)
  end
end

-- ------------------------------------------------------------------------------------------------------------------
-- Being carried by Mario: every node of the carried piece is pulled (damped spring, gravity cancelled) towards where
-- it sat relative to the piece's centre when he picked it up, re-placed at his hold point and turned with him. The
-- piece keeps its shape and stays a normal soft body - it can still dent and hit things while he carries it.
local CARRY_K = 90        -- 1/s^2
local CARRY_C = 19        -- 1/s (about critically damped)
local THROW_TIME = 0.05

local carry            -- { nodes = { {cid, mass, offx, offy, offz} }, lift, yaw0, target, yaw }

local function carryStart(piece)
  local nodeCount = obj:getNodeCount()
  local list = pieces(nodeCount)
  local nodes = list[(piece or 0) + 1] or list[1]
  if not nodes then return end
  local base = obj:getPosition()
  local com, mass, minZ = vec3(0, 0, 0), 0, math.huge
  for _, cid in ipairs(nodes) do
    local m = obj:getNodeMass(cid)
    local p = base + obj:getNodePosition(cid)
    com = com + p * m
    mass = mass + m
    if p.z < minZ then minZ = p.z end
  end
  if mass <= 0 then return end
  com = com / mass
  carry = { nodes = {}, lift = com.z - minZ }
  for _, cid in ipairs(nodes) do
    local p = base + obj:getNodePosition(cid)
    carry.nodes[#carry.nodes + 1] = { cid, obj:getNodeMass(cid), p.x - com.x, p.y - com.y, p.z - com.z }
  end
end

local function carryTarget(x, y, z, yaw)
  if not carry then return end
  if not carry.yaw0 then carry.yaw0 = yaw end
  carry.target = vec3(x, y, z + carry.lift)   -- (x, y, z) is where its underside should be
  carry.yaw = yaw
end

carryUpdate = function(dt)
  if not carry or not carry.target then return end
  local base = obj:getPosition()
  local a = carry.yaw - carry.yaw0
  local ca, sa = math.cos(a), math.sin(a)
  local t = carry.target
  for _, n in ipairs(carry.nodes) do
    local cid, m = n[1], n[2]
    local want = vec3(t.x + n[3] * ca - n[4] * sa, t.y + n[3] * sa + n[4] * ca, t.z + n[5])
    local p = base + obj:getNodePosition(cid)
    local vel = obj:getNodeVelocityVector(cid)
    local acc = (want - p) * CARRY_K - vel * CARRY_C + vec3(0, 0, 9.81)
    obj:applyForceVectorTime(cid, acc * m, dt)
  end
end

local function carryRelease(vx, vy, vz)
  if not carry then return end
  -- give the whole piece the throw velocity (over a few physics steps), then let it fly
  local sum, mass = vec3(0, 0, 0), 0
  for _, n in ipairs(carry.nodes) do
    sum = sum + obj:getNodeVelocityVector(n[1]) * n[2]
    mass = mass + n[2]
  end
  local dv = vec3(vx, vy, vz) - (mass > 0 and sum / mass or vec3(0, 0, 0))
  carry.throw = { dv = dv, t = THROW_TIME }
  carry.target = nil
end

throwUpdate = function(dt)
  if not carry or not carry.throw then return end
  local th = carry.throw
  local step = math.min(dt, th.t)
  for _, n in ipairs(carry.nodes) do obj:applyForceVectorTime(n[1], th.dv * (n[2] / THROW_TIME), step) end
  th.t = th.t - step
  if th.t <= 1e-4 then carry = nil end
end

local function onReset()
  pending = {}
  carry = nil
end

M.hit = hit
M.tune = tune
M.sendHull = sendHull
M.carryStart = carryStart
M.carryTarget = carryTarget
M.carryRelease = carryRelease
M.updateGFX = updateGFX
M.onReset = onReset
return M
