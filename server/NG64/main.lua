-- NG64 BeamMP server plugin: relays each player's Mario state to everyone else.
-- Install: <BeamMP server>/Resources/Server/NG64/main.lua (the client mod zip goes in Resources/Client).

function ng64State(pid, data)
  if type(data) ~= "string" or #data > 256 then return end
  local msg = tostring(pid) .. "|" .. data
  for id, _ in pairs(MP.GetPlayers() or {}) do
    if id ~= pid then MP.TriggerClientEvent(id, "ng64Remote", msg) end
  end
end

function ng64Gone(pid)
  MP.TriggerClientEvent(-1, "ng64RemoteGone", tostring(pid))
end

function ng64OnDisconnect(pid)
  ng64Gone(pid)
end

MP.RegisterEvent("ng64State", "ng64State")
MP.RegisterEvent("ng64Gone", "ng64Gone")
MP.RegisterEvent("onPlayerDisconnect", "ng64OnDisconnect")
