-- NG64: applies Mario's punches/kicks/ground pounds to this vehicle.
-- Called from GE: ng64Hit.hit(px, py, pz, dx, dy, dz, strength) with world-space hit point and direction.
local M = {}

local HIT_TIME = 0.06        -- seconds each hit's force is spread over
local SHOVE_DV = 1.1         -- m/s whole-vehicle velocity change per unit strength
local DENT_IMPULSE = 3000    -- N*s concentrated on nodes near the hit, per unit strength (calibrated on the Gavril D-Series)
local DENT_RADIUS = 0.45

-- live tuning from the console: ng64Hit.tune(dentImpulse, shoveDv, dentRadius)
local function tune(dent, shove, radius)
  DENT_IMPULSE = dent or DENT_IMPULSE
  SHOVE_DV = shove or SHOVE_DV
  DENT_RADIUS = radius or DENT_RADIUS
end

local pending = {}

local function hit(px, py, pz, dx, dy, dz, strength)
  local d = vec3(dx, dy, dz)
  if d:length() < 1e-4 then return end
  d:normalize()
  pending[#pending + 1] = { p = vec3(px, py, pz), d = d, s = strength or 1, t = HIT_TIME }
end

local function updateGFX(dt)
  if #pending == 0 then return end
  local base = obj:getPosition()
  local nodeCount = obj:getNodeCount()
  for i = #pending, 1, -1 do
    local h = pending[i]
    local step = math.min(dt, h.t)
    local radius = DENT_RADIUS + 0.1 * h.s

    -- find nodes near the hit and their falloff weights
    local near, wsum = {}, 0
    for cid = 0, nodeCount - 1 do
      local mass = obj:getNodeMass(cid)
      if mass and mass > 0 then
        local dist = (base + obj:getNodePosition(cid)):distance(h.p)
        -- whole-vehicle shove: same acceleration on every node
        obj:applyForceVectorTime(cid, h.d * (mass * SHOVE_DV * h.s / HIT_TIME), step)
        if dist < radius then
          local w = 1 - dist / radius
          near[#near + 1] = { cid, w }
          wsum = wsum + w
        end
      end
    end
    if wsum > 0 then
      local total = DENT_IMPULSE * h.s / HIT_TIME
      for _, n in ipairs(near) do
        obj:applyForceVectorTime(n[1], h.d * (total * n[2] / wsum), step)
      end
    end

    h.t = h.t - step
    if h.t <= 1e-4 then table.remove(pending, i) end
  end
end

local function onReset()
  pending = {}
end

M.hit = hit
M.tune = tune
M.updateGFX = updateGFX
M.onReset = onReset
return M
