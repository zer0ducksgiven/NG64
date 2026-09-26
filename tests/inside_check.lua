-- Test helper (sent over MCP): for each sample point (Mario's chest), is it inside a solid map object? Uses the exact
-- collision triangles (ng64World). A point is inside a closed mesh when vertical rays up AND down from it each cross
-- its surface an odd number of times. Input: _insideSamples = { {x,y,z}, ... }. Returns "i1 i2 ..." (1 = inside).
local world = require("ge/extensions/ng64World")
if world.stats().statics == 0 then world.index() end
local s = _insideSamples
local cx, cy, cz, n = 0, 0, 0, #s
for _, p in ipairs(s) do cx, cy, cz = cx + p[1] / n, cy + p[2] / n, cz + p[3] / n end
local r = 5
for _, p in ipairs(s) do r = math.max(r, math.abs(p[1] - cx) + 2, math.abs(p[2] - cy) + 2) end
local tris, pending = world.region(cx, cy, cz, r, 60)
local guard = 0
while pending > 0 and guard < 400 do world.step(0.25) tris, pending = world.region(cx, cy, cz, r, 60) guard = guard + 1 end

-- bucket triangles by 1 m xy cells so each vertical ray only tests the triangles above/below it
local cells = {}
local function key(i, j) return i * 100000 + j end
for t = 1, #tris, 9 do
  local x0 = math.floor(math.min(tris[t], tris[t + 3], tris[t + 6]))
  local x1 = math.floor(math.max(tris[t], tris[t + 3], tris[t + 6]))
  local y0 = math.floor(math.min(tris[t + 1], tris[t + 4], tris[t + 7]))
  local y1 = math.floor(math.max(tris[t + 1], tris[t + 4], tris[t + 7]))
  if (x1 - x0 + 1) * (y1 - y0 + 1) < 4000 then
    for i = x0, x1 do for j = y0, y1 do
      local k = key(i, j)
      local c = cells[k] if not c then c = {} cells[k] = c end
      c[#c + 1] = t
    end end
  end
end

local out = {}
for si, p in ipairs(s) do
  local px, py, pz = p[1], p[2], p[3]
  local up, down = 0, 0
  for _, t in ipairs(cells[key(math.floor(px), math.floor(py))] or {}) do
    local ax, ay, az, bx, by, bz, qx, qy, qz = tris[t], tris[t + 1], tris[t + 2], tris[t + 3], tris[t + 4], tris[t + 5], tris[t + 6], tris[t + 7], tris[t + 8]
    local det = (by - qy) * (ax - qx) + (qx - bx) * (ay - qy)
    if math.abs(det) > 1e-12 then
      local w1 = ((by - qy) * (px - qx) + (qx - bx) * (py - qy)) / det
      local w2 = ((qy - ay) * (px - qx) + (ax - qx) * (py - qy)) / det
      local w3 = 1 - w1 - w2
      if w1 >= 0 and w2 >= 0 and w3 >= 0 then
        local z = w1 * az + w2 * bz + w3 * qz
        if z > pz then up = up + 1 else down = down + 1 end
      end
    end
  end
  out[si] = (up % 2 == 1 and down % 2 == 1) and "1" or "0"
end
return table.concat(out, " ")
