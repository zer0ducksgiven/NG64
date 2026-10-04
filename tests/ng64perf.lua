-- test helper (not part of the mod): frame counter for tests/perf_probe.py
local M = {}
local n, sum, worst, spikes = 0, 0, 0, 0
function M.onUpdate(dtReal)
  n = n + 1
  sum = sum + dtReal
  if dtReal > worst then worst = dtReal end
  if dtReal > 0.05 then spikes = spikes + 1 end
end
function M.take()
  local r = string.format('%d %.4f %.4f %d', n, sum, worst, spikes)
  n, sum, worst, spikes = 0, 0, 0, 0
  return r
end
return M
