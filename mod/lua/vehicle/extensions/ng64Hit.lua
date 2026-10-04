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
local carryUpdate, throwUpdate, spinUpdate, spinThrowUpdate   -- defined with the carry code further down

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
  spinUpdate(dt)
  spinThrowUpdate(dt)
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

-- ------------------------------------------------------------------------------------------------------------------
-- Spun by Mario, like Bowser by his tail: he holds one end of the car at his hands and turns on the spot, the car
-- swinging round him. Every node is pulled to where the car would be if it were a rigid body rotating with Mario (a
-- spring on position and velocity, plus the centripetal acceleration that circling takes), so the car stays a soft
-- body that dents and collides: when it can't get where it should be, or touches another vehicle, the spin is over.
local SPIN_K = 450            -- 1/s^2
local SPIN_C = 34             -- 1/s
local SPIN_GRIP = 0.55        -- from his axis to the end of the car he holds: just past his gloves, which swing out ~0.3 m
local SPIN_HAND_Z = 1.0       -- his hands above his feet
local SPIN_TILT = math.rad(10)   -- the far end lifts this much at full spin (low enough to take other cars with it)
local SPIN_OMEGA_MAX = 11.8   -- rad/s, SM64's fastest spin
local SPIN_BLOCKED = 0.8      -- m: mean distance of the car from where the spin wants it that means it's stuck
local SPIN_BLOCKED_MAX = 2.2  -- m: same, for its worst node

local spin     -- { nodes = { {cid, mass, lx, ly, lz} }, grip, ext, stubId, t, reached, bad, vehBad, P, phi, omega, age }
local spinThrow   -- { t, nodes = { {cid, mass, dvx, dvy, dvz} } }

local function spinStart(mx, my, mz, stubId)
  if spin then return end
  local nodeCount = obj:getNodeCount()
  local base = obj:getPosition()
  local fwd, up = obj:getDirectionVector(), obj:getDirectionVectorUp()
  local right = fwd:cross(up)
  local com, mass = vec3(0, 0, 0), 0
  for cid = 0, nodeCount - 1 do
    local m = obj:getNodeMass(cid)
    com = com + (base + obj:getNodePosition(cid)) * m
    mass = mass + m
  end
  if mass <= 0 then return end
  com = com / mass
  local nodes, minF, maxF = {}, math.huge, -math.huge
  for cid = 0, nodeCount - 1 do
    local rel = base + obj:getNodePosition(cid) - com
    local ly = rel:dot(fwd)
    nodes[#nodes + 1] = { cid, obj:getNodeMass(cid), rel:dot(right), ly, rel:dot(up) }
    if ly < minF then minF = ly end
    if ly > maxF then maxF = ly end
  end
  -- he takes the end nearer to him
  local grip = (vec3(mx, my, mz) - com):dot(fwd) >= 0 and 1 or -1
  spin = { nodes = nodes, grip = grip, ext = grip > 0 and maxF or -minF, mass = mass, stubId = stubId or 0, t = 0, bad = 0, vehBad = 0 }
end

local function spinTarget(px, py, pz, phi, omega, grip, centreZ)
  if not spin then return end
  spin.P = { px, py, pz }
  spin.phi, spin.omega, spin.age = phi, omega, 0
  spin.gripD, spin.centreZ = grip, centreZ
end

-- the car lets go of the spin and keeps a quarter of the speed it had
local function spinDrop()
  local s = spin
  spin = nil
  if not s then return end
  local nodes = {}
  for _, n in ipairs(s.nodes) do
    local nv = obj:getNodeVelocityVector(n[1])
    nodes[#nodes + 1] = { n[1], n[2], -0.75 * nv.x, -0.75 * nv.y, -0.75 * nv.z }
  end
  spinThrow = { t = THROW_TIME, nodes = nodes }
end

spinUpdate = function(dt)
  if not spin or not spin.P then return end
  local s = spin
  -- a hitch (a long frame) must not turn into one huge push, or into a false "it's stuck": push for at most a frame's
  -- worth and ignore how far it lags behind for a moment
  if dt > 0.04 then s.grace = 0.6 end
  s.grace = math.max(0, (s.grace or 0) - dt)
  local fdt = math.min(dt, 0.04)
  s.t = s.t + dt
  s.age = s.age + dt
  local px, py, pz = s.P[1], s.P[2], s.P[3]
  local omega = s.omega
  local phi = s.phi + omega * math.min(s.age, 0.07)   -- between his updates, keep turning
  local frac = math.min(1, math.abs(omega) / SPIN_OMEGA_MAX)
  local tilt = SPIN_TILT * frac
  local ux, uy = math.cos(phi), math.sin(phi)
  local ct, sn = math.cos(tilt), math.sin(tilt)
  local Ox, Oy, Oz = ux * ct, uy * ct, sn                  -- outward from him, tilted up
  local g = s.grip
  local Fx, Fy, Fz = -g * Ox, -g * Oy, -g * Oz             -- the car's own forward: towards him if he holds its front
  local Ux, Uy, Uz = -Fz * Fx, -Fz * Fy, 1 - Fz * Fz
  local ul = math.sqrt(Ux * Ux + Uy * Uy + Uz * Uz)
  Ux, Uy, Uz = Ux / ul, Uy / ul, Uz / ul
  local Rx, Ry, Rz = Fy * Uz - Fz * Uy, Fz * Ux - Fx * Uz, Fx * Uy - Fy * Ux
  local dist = (s.gripD or SPIN_GRIP) + s.ext         -- his gloves' reach along the car, plus the car's own half length
  local Cx, Cy, Cz = px + Ox * dist, py + Oy * dist, pz + (s.centreZ or SPIN_HAND_Z) + Oz * dist
  local gain = math.min(1, 0.25 + 0.75 * s.t / 0.7)        -- the heave from the ground ramps in
  local K, C = SPIN_K * gain, SPIN_C * (0.6 + 0.4 * gain)
  local w2 = omega * omega
  local base = obj:getPosition()
  local err2, errMax, lead, leadSpeed2 = 0, 0, nil, -1
  for _, n in ipairs(s.nodes) do
    local cid, m, lx, ly, lz = n[1], n[2], n[3], n[4], n[5]
    local wx, wy, wz = Cx + Rx * lx + Fx * ly + Ux * lz, Cy + Ry * lx + Fy * ly + Uy * lz, Cz + Rz * lx + Fz * ly + Uz * lz
    local rx, ry = wx - px, wy - py
    local p = obj:getNodePosition(cid)
    local nv = obj:getNodeVelocityVector(cid)
    local ex, ey, ez = wx - (base.x + p.x), wy - (base.y + p.y), wz - (base.z + p.z)
    local ax = K * ex + C * (-omega * ry - nv.x) - w2 * rx
    local ay = K * ey + C * (omega * rx - nv.y) - w2 * ry
    local az = K * ez + C * (-nv.z) + 9.81
    obj:applyForceVectorTime(cid, vec3(ax * m, ay * m, az * m), fdt)
    local e2 = ex * ex + ey * ey + ez * ez
    err2 = err2 + e2 * m
    if e2 > errMax then errMax = e2 end
    local sp2 = nv.x * nv.x + nv.y * nv.y + nv.z * nv.z
    if sp2 > leadSpeed2 then leadSpeed2, lead = sp2, { base.x + p.x, base.y + p.y, base.z + p.z, nv.x, nv.y, nv.z } end
  end
  local err, worst = math.sqrt(err2 / s.mass), math.sqrt(errMax)
  if err < 0.35 then s.reached = true end

  -- is it hitting something? only once it has reached the hold: before that it's still being heaved off the ground
  local hit, otherId, why = false, 0, ""
  if s.reached and s.grace <= 0 then
    if err > SPIN_BLOCKED or worst > SPIN_BLOCKED_MAX then s.bad = s.bad + dt else s.bad = 0 end
    if s.bad >= 0.1 then hit, why = true, "off its path" end
    local cols = mapmgr and mapmgr.objectCollisionIds
    if cols then
      local found
      for k, cv in pairs(cols) do
        local id = type(cv) == "number" and cv or k   -- an array of ids, or a set keyed by id
        if type(id) == "number" and id ~= s.stubId and id ~= obj:getId() then found = id break end
      end
      if found then s.vehBad = s.vehBad + dt otherId = found s.lastVeh, s.lastVehT = found, s.t else s.vehBad = 0 end
      if s.vehBad >= 0.04 then hit, why = true, "vehicle" end
    end
  elseif not s.reached and s.t > 3 then
    hit, why = true, "never reached the hold"     -- jammed against something: give up
  end
  if hit and otherId == 0 and s.lastVeh and s.t - s.lastVehT < 0.5 then otherId = s.lastVeh end   -- it was just touching one
  if hit and lead then
    local speed = math.sqrt(leadSpeed2)
    local dx, dy, dz = lead[4] / math.max(speed, 1e-3), lead[5] / math.max(speed, 1e-3), lead[6] / math.max(speed, 1e-3)
    M.lastSpin = string.format("spin ended (%s): err %.2f m, worst %.2f m, t %.2f s, other vehicle %s", why, err, worst, s.t, tostring(otherId))   -- for tests
    log("I", "ng64Hit", string.format("spin ended (%s): err %.2f m, worst %.2f m, t %.2f s, other vehicle %s", why, err, worst, s.t, tostring(otherId)))
    spinDrop()
    obj:queueGameEngineLua(string.format("ng64.onSpinHit(%d, %d, %f,%f,%f, %f,%f,%f, %f)", obj:getId(), otherId or 0,
      lead[1], lead[2], lead[3], dx, dy, dz, frac))
  end
end

-- thrown: every node gets the throw velocity plus the tumble (w x r about the car's centre), over a few physics steps
local function spinRelease(vx, vy, vz, wx, wy, wz)
  local s = spin
  spin = nil
  if not s then return end
  local base = obj:getPosition()
  local com, mass = vec3(0, 0, 0), 0
  for _, n in ipairs(s.nodes) do
    com = com + (base + obj:getNodePosition(n[1])) * n[2]
    mass = mass + n[2]
  end
  com = com / mass
  local nodes = {}
  for _, n in ipairs(s.nodes) do
    local r = base + obj:getNodePosition(n[1]) - com
    local nv = obj:getNodeVelocityVector(n[1])
    local tx, ty, tz = vx + wy * r.z - wz * r.y, vy + wz * r.x - wx * r.z, vz + wx * r.y - wy * r.x
    nodes[#nodes + 1] = { n[1], n[2], tx - nv.x, ty - nv.y, tz - nv.z }
  end
  spinThrow = { t = THROW_TIME, nodes = nodes }
end

spinThrowUpdate = function(dt)
  if not spinThrow then return end
  local th = spinThrow
  local step = math.min(dt, th.t)
  for _, n in ipairs(th.nodes) do
    local f = n[2] / THROW_TIME
    obj:applyForceVectorTime(n[1], vec3(n[3] * f, n[4] * f, n[5] * f), step)
  end
  th.t = th.t - step
  if th.t <= 1e-4 then spinThrow = nil end
end


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
  -- rest it on its floor right above his hands, not on its lowest point (the tyres): the underside is the lowest
  -- node near the middle; fall back to the lowest anywhere for small pieces
  local under = math.huge
  for _, cid in ipairs(nodes) do
    local p = base + obj:getNodePosition(cid)
    if (p.x - com.x) ^ 2 + (p.y - com.y) ^ 2 < 0.7 ^ 2 and p.z < under then under = p.z end
  end
  if under == math.huge then under = minZ end
  carry = { nodes = {}, lift = com.z - under }
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

local function carryRelease(vx, vy, vz, wx, wy, wz)
  if spin then return spinRelease(vx, vy, vz, wx or 0, wy or 0, wz or 0) end
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
  spin, spinThrow = nil, nil
end

M.hit = hit
M.tune = tune
M.sendHull = sendHull
M.carryStart = carryStart
M.carryTarget = carryTarget
M.carryRelease = carryRelease
M.spinStart = spinStart
M.spinTarget = spinTarget
M.updateGFX = updateGFX
M.onReset = onReset
return M
