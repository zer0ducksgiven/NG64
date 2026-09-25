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

local function hit(px, py, pz, dx, dy, dz, strength)
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
  pending[#pending + 1] = { d = d, s = strength or 1, t = HIT_TIME, near = near, wsum = wsum }
  local info = string.format("hit strength %.1f at (%.2f %.2f %.2f) dir (%.2f %.2f %.2f): %d nodes within %.1f m, nearest %.2f m",
    strength or 1, px, py, pz, d.x, d.y, d.z, #near, DENT_RADIUS, cand[1] and cand[1][2] or -1)
  log("I", "ng64Hit", info)
end

local function updateGFX(dt)
  if #pending == 0 then return end
  local nodeCount = obj:getNodeCount()
  for i = #pending, 1, -1 do
    local h = pending[i]
    local step = math.min(dt, h.t)
    -- whole-vehicle shove: same acceleration on every node
    local shoveAccel = SHOVE_DV * h.s / HIT_TIME
    for cid = 0, nodeCount - 1 do
      local mass = obj:getNodeMass(cid)
      if mass and mass > 0 then obj:applyForceVectorTime(cid, h.d * (mass * shoveAccel), step) end
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

-- height grid of the vehicle's nodes in its own frame (x right, y forward, z up, origin = obj:getPosition())
local function sendHull()
  local fwd = obj:getDirectionVector()
  local up = obj:getDirectionVectorUp()
  fwd:normalize()
  up:normalize()
  local right = fwd:cross(up)
  right:normalize()

  local pts = {}
  local minX, maxX, minY, maxY, minZ = math.huge, -math.huge, math.huge, -math.huge, math.huge
  for cid = 0, obj:getNodeCount() - 1 do
    local d = obj:getNodePosition(cid)
    local x, y, z = d:dot(right), d:dot(fwd), d:dot(up)
    pts[#pts + 1] = { x, y, z }
    if x < minX then minX = x end
    if x > maxX then maxX = x end
    if y < minY then minY = y end
    if y > maxY then maxY = y end
    if z < minZ then minZ = z end
  end
  if #pts == 0 then return end

  local cell = math.max(HULL_CELL, (maxX - minX) / 64, (maxY - minY) / 64)
  local nx = math.max(1, math.ceil((maxX - minX) / cell))
  local ny = math.max(1, math.ceil((maxY - minY) / cell))
  local top = {}
  for _, p in ipairs(pts) do
    local i = math.min(nx - 1, math.floor((p[1] - minX) / cell))
    local j = math.min(ny - 1, math.floor((p[2] - minY) / cell))
    local k = j * nx + i + 1
    if not top[k] or p[3] > top[k] then top[k] = p[3] end
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
    for k, v in pairs(filled) do top[k] = v end
  end

  local parts = {}
  for k = 1, nx * ny do parts[k] = top[k] and string.format("%.3f", top[k]) or "n" end
  obj:queueGameEngineLua(string.format("if ng64 then ng64.onHull(%d, %.4f, %.4f, %.4f, %.4f, %d, %d, %q) end",
    obj:getId(), cell, minX, minY, minZ, nx, ny, table.concat(parts, ",")))
end

local function onReset()
  pending = {}
end

M.hit = hit
M.tune = tune
M.sendHull = sendHull
M.updateGFX = updateGFX
M.onReset = onReset
return M
