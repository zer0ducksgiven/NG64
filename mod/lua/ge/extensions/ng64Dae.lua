-- NG64: reads a shape's collision geometry out of its COLLADA (.dae) file, so Mario collides with the world's real
-- triangles instead of a raycast approximation. BeamNG ships a readable .dae next to each compiled .cdae.
--
-- parse(text) -> { tris = { x1,y1,z1, x2,y2,z2, x3,y3,z3, ... } } in the shape's own space, metres, Z up.
-- Collision geometry is what sits under a node named "collision*" / "colmesh*" (BeamNG's convention, the same meshes
-- its physics uses) for "Collision Mesh" objects; "Visible Mesh (Final)" objects collide with their visible mesh
-- instead - checked against BeamNG's own raycasts on West Coast USA's island, whose collision mesh differs by 13 m.
local M = {}

local tonumber, find, sub, match, gmatch = tonumber, string.find, string.sub, string.match, string.gmatch
local sin, cos, rad = math.sin, math.cos, math.rad

local yield = coroutine.yield
local function numbers(s, out)
  out = out or {}
  local n = #out
  local cooperative = M.cooperative
  for v in gmatch(s, "[^%s]+") do
    n = n + 1
    out[n] = tonumber(v)
    -- big collision meshes take seconds: let the caller spread them over frames
    if cooperative and n % 20000 == 0 then yield() end
  end
  return out
end

-- 4x4 matrices as flat row-major tables (Collada's column-vector convention: p' = M * p)
local function ident() return { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } end
local function mul(a, b)
  local r = {}
  for i = 0, 3 do
    for j = 0, 3 do
      r[i * 4 + j + 1] = a[i * 4 + 1] * b[j + 1] + a[i * 4 + 2] * b[4 + j + 1] + a[i * 4 + 3] * b[8 + j + 1] + a[i * 4 + 4] * b[12 + j + 1]
    end
  end
  return r
end
local function translate(x, y, z) return { 1,0,0,x, 0,1,0,y, 0,0,1,z, 0,0,0,1 } end
local function scale(x, y, z) return { x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1 } end
local function rotate(x, y, z, deg)
  local l = math.sqrt(x * x + y * y + z * z)
  if l < 1e-12 then return ident() end
  x, y, z = x / l, y / l, z / l
  local c, s = cos(rad(deg)), sin(rad(deg))
  local t = 1 - c
  return { t*x*x + c, t*x*y - s*z, t*x*z + s*y, 0,
           t*x*y + s*z, t*y*y + c, t*y*z - s*x, 0,
           t*x*z - s*y, t*y*z + s*x, t*z*z + c, 0,
           0, 0, 0, 1 }
end

-- one <geometry> body -> triangle list (local to the geometry)
local function parseGeometry(body)
  local sources = {}
  for sid, arr in gmatch(body, '<source[^>]-id="([^"]+)"[^>]*>.-<float_array[^>]*>(.-)</float_array>') do
    sources[sid] = arr
  end
  local vertSource = {}
  for vid, inner in gmatch(body, '<vertices[^>]-id="([^"]+)"[^>]*>(.-)</vertices>') do
    local src = match(inner, '<input[^>]-semantic="POSITION"[^>]-source="#([^"]+)"')
    vertSource[vid] = src
  end
  local tris = {}
  local nt = 0
  local cache = {}
  local function positions(srcId)
    local p = cache[srcId]
    if not p and sources[srcId] then p = numbers(sources[srcId]) cache[srcId] = p end
    return p
  end
  for kind, pbody in gmatch(body, "<(%a+)[^>]-count=\"%d+\"[^>]*>(.-)</%1>") do
    if kind == "triangles" or kind == "polylist" or kind == "polygons" then
      local stride, vOff, vSrc = 0, 0, nil
      for inputTag in gmatch(pbody, "<input[^>]*>") do
        local sem = match(inputTag, 'semantic="([^"]+)"')
        local off = tonumber(match(inputTag, 'offset="(%d+)"') or "0")
        if off + 1 > stride then stride = off + 1 end
        if sem == "VERTEX" then
          vOff = off
          local s = match(inputTag, 'source="#([^"]+)"')
          vSrc = vertSource[s] or s
        end
      end
      local pos = vSrc and positions(vSrc)
      if pos and stride > 0 then
        local function corner(idx)
          local v = idx[1 + vOff] * 3
          return pos[v + 1], pos[v + 2], pos[v + 3]
        end
        if kind == "triangles" then
          local p = numbers(match(pbody, "<p>(.-)</p>") or "")
          local per = stride * 3
          for i = 0, #p / per - 1 do
            local b = i * per
            for c = 0, 2 do
              local v = p[b + c * stride + vOff + 1] * 3
              tris[nt + 1], tris[nt + 2], tris[nt + 3] = pos[v + 1], pos[v + 2], pos[v + 3]
              nt = nt + 3
            end
          end
        else
          -- polygons: fan-triangulate each (vcount per polygon, or one <p> per polygon)
          local vcount = match(pbody, "<vcount>(.-)</vcount>")
          local polys = {}
          if vcount then
            polys[1] = { numbers(vcount), numbers(match(pbody, "<p>(.-)</p>") or "") }
          else
            for ptxt in gmatch(pbody, "<p>(.-)</p>") do
              local p = numbers(ptxt)
              polys[#polys + 1] = { { #p / stride }, p }
            end
          end
          for _, pl in ipairs(polys) do
            local counts, p = pl[1], pl[2]
            local base = 0
            for _, n in ipairs(counts) do
              local v0 = p[base + vOff + 1] * 3
              for k = 1, n - 2 do
                local v1 = p[base + k * stride + vOff + 1] * 3
                local v2 = p[base + (k + 1) * stride + vOff + 1] * 3
                tris[nt + 1], tris[nt + 2], tris[nt + 3] = pos[v0 + 1], pos[v0 + 2], pos[v0 + 3]
                tris[nt + 4], tris[nt + 5], tris[nt + 6] = pos[v1 + 1], pos[v1 + 2], pos[v1 + 3]
                tris[nt + 7], tris[nt + 8], tris[nt + 9] = pos[v2 + 1], pos[v2 + 2], pos[v2 + 3]
                nt = nt + 9
              end
              base = base + n * stride
            end
          end
        end
      end
    end
  end
  return tris
end

local function isCollisionName(name)
  local n = string.lower(name or "")
  return sub(n, 1, 9) == "collision" or sub(n, 1, 7) == "colmesh"
end

-- walks <library_visual_scenes>: every instance_geometry with its accumulated node transform, and whether it sits
-- under a collision node; also the trailing detail number of visible LOD nodes (e.g. "roof_a100" -> 100)
local function sceneInstances(text)
  local s0 = find(text, "<library_visual_scenes", 1, true)
  if not s0 then return {} end
  local s1 = find(text, "</library_visual_scenes>", s0, true) or #text
  local scene = sub(text, s0, s1)
  local out = {}
  local stack = { { m = ident(), col = false, lod = nil } }
  local pos = 1
  while true do
    local a, b, close, tag, attrs, selfClose = find(scene, "<(/?)([%w_]+)([^>]-)(/?)>", pos)
    if not a then break end
    pos = b + 1
    local top = stack[#stack]
    if tag == "node" then
      if close == "/" then
        if #stack > 1 then stack[#stack] = nil end
      else
        local name = match(attrs, 'name="([^"]*)"') or match(attrs, 'id="([^"]*)"') or ""
        local lod = tonumber(match(name, "(%d+)$"))
        local entry = { m = top.m, col = top.col or isCollisionName(name), lod = lod or top.lod, name = name }
        if selfClose ~= "/" then stack[#stack + 1] = entry end
      end
    elseif close ~= "/" and (tag == "matrix" or tag == "translate" or tag == "rotate" or tag == "scale") then
      local e = find(scene, "</" .. tag .. ">", pos, true)
      local v = numbers(sub(scene, pos, (e or pos) - 1))
      local t
      if tag == "matrix" and #v >= 16 then t = v
      elseif tag == "translate" and #v >= 3 then t = translate(v[1], v[2], v[3])
      elseif tag == "rotate" and #v >= 4 then t = rotate(v[1], v[2], v[3], v[4])
      elseif tag == "scale" and #v >= 3 then t = scale(v[1], v[2], v[3]) end
      if t then top.m = mul(top.m, t) end
      if e then pos = e end
    elseif tag == "instance_geometry" and close ~= "/" then
      local url = match(attrs, 'url="#([^"]+)"')
      if url then out[#out + 1] = { geom = url, m = top.m, col = top.col, lod = top.lod } end
    end
  end
  return out
end

local function geometryBody(text, id)
  local a = find(text, '<geometry[^>]-id="' .. id:gsub("%p", "%%%0") .. '"')
  if not a then return nil end
  local e = find(text, "</geometry>", a, true)
  return e and sub(text, a, e) or nil
end

-- mode: "collision" (the collision mesh; nothing if there is none), "visibleHigh" / "visibleLow" (the most / least
-- detailed visible LOD, i.e. the highest / lowest trailing number; everything visible if none are numbered)
function M.parse(text, mode)
  mode = mode or "collision"
  local up = match(text, "<up_axis>%s*([%w_]+)%s*</up_axis>") or "Z_UP"
  local unit = tonumber(match(text, '<unit[^>]-meter="([%d%.eE%-]+)"') or "1") or 1
  local inst = sceneInstances(text)
  local pick = {}
  local usedVisible = mode ~= "collision"
  if mode == "collision" then
    for _, it in ipairs(inst) do if it.col then pick[#pick + 1] = it end end
  else
    local best
    for _, it in ipairs(inst) do
      if not it.col and it.lod and (not best or (mode == "visibleLow" and it.lod < best) or (mode ~= "visibleLow" and it.lod > best)) then best = it.lod end
    end
    for _, it in ipairs(inst) do if not it.col and (not best or it.lod == best) then pick[#pick + 1] = it end end
  end
  local geomCache = {}
  local out, n = {}, 0
  for _, it in ipairs(pick) do
    local g = geomCache[it.geom]
    if g == nil then
      local body = geometryBody(text, it.geom)
      g = body and parseGeometry(body) or false
      geomCache[it.geom] = g
    end
    if g then
      local m = it.m
      local det = m[1] * (m[6] * m[11] - m[7] * m[10]) - m[2] * (m[5] * m[11] - m[7] * m[9]) + m[3] * (m[5] * m[10] - m[6] * m[9])
      local base = n
      for i = 1, #g, 3 do
        local x, y, z = g[i], g[i + 1], g[i + 2]
        local wx = (m[1] * x + m[2] * y + m[3] * z + m[4]) * unit
        local wy = (m[5] * x + m[6] * y + m[7] * z + m[8]) * unit
        local wz = (m[9] * x + m[10] * y + m[11] * z + m[12]) * unit
        if up == "Y_UP" then wy, wz = -wz, wy end
        out[n + 1], out[n + 2], out[n + 3] = wx, wy, wz
        n = n + 3
      end
      if det < 0 then
        -- mirrored node: swap two corners so every triangle still faces out
        for t = base, n - 9, 9 do
          for k = 1, 3 do out[t + 3 + k], out[t + 6 + k] = out[t + 6 + k], out[t + 3 + k] end
        end
      end
    end
  end
  return { tris = out, visible = usedVisible }
end

return M
