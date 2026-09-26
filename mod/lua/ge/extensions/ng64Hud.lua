-- NG64: SM64's HUD for Mario - lives, coins, stars and the power meter - and the UI layout it lives in.
--
-- While the player controls Mario, the "ng64Mario" UI layout (just the NG64 HUD app, see
-- settings/ui_apps/originalLayouts/default/ng64Mario.uilayout.json) replaces the vehicle layout, the way BeamNG's own
-- walking mode swaps in its "unicycle" layout; switching away puts the game's layout back. The HUD app draws with
-- SM64's own HUD graphics, which the helper takes from the player's ROM (ng64_cache/hud/*.png).
--
-- Lives work like SM64: 4 to start, one lost when Mario's health runs out, and after losing the last one ("game
-- over") the next respawn starts again at 4. Coins reset when a life is lost. Nothing in BeamNG gives coins or stars
-- yet; a map or mod can, through collectCoin / collectStar (a coin also heals a wedge, as in SM64).
local M = {}

local LAYOUT_TYPE = "ng64Mario"
local LIVES_START = 4
local SEND_INTERVAL = 0.5    -- resent this often even when unchanged, so an app that just loaded catches up

local lives, coins, stars = LIVES_START, 0, 0
local dead, gameOver = false, false
local layoutOn, reapplyLayout = false, false
local hudImages = false      -- the helper wrote SM64's HUD graphics
local last, lastSentAt = nil, -10
local deaths = 0             -- tests
local heal                   -- function(healCounter) set by ng64.lua: heals Mario through the helper

local function setLayout(on)
  if on == layoutOn then return end
  layoutOn = on
  if on then
    guihooks.trigger("appContainer:loadLayoutByType", LAYOUT_TYPE)
  else
    -- back to the layout the game is using (freeroam, a scenario's...). Re-sending the game state isn't enough: the
    -- UI ignores a layout it thinks it already has
    local gs = core_gamestate and core_gamestate.getGameState and core_gamestate.getGameState() or {}
    guihooks.trigger("appContainer:loadLayoutByType", gs.appLayout or "freeroam")
  end
end

-- every frame from ng64.onUpdate. frame: the newest local pose (health = SM64 health, 0x880 = full), or nil
function M.update(simTime, frame, active, controlled)
  setLayout(active and controlled)
  if reapplyLayout and layoutOn then
    reapplyLayout = false
    guihooks.trigger("appContainer:loadLayoutByType", LAYOUT_TYPE)
  end

  local health = frame and frame.health or 0x880
  local wedges = math.max(0, math.min(8, math.floor(health / 256)))
  if active and frame then
    if not dead and wedges == 0 then
      dead = true
      deaths = deaths + 1
      if lives > 0 then lives = lives - 1 else gameOver = true end
      coins = 0
    elseif dead and wedges > 0 then
      dead = false                           -- respawned
      if gameOver then gameOver, lives = false, LIVES_START end
    end
  end

  local visible = active and controlled and frame ~= nil
  local changed = not last or last.visible ~= visible or last.health ~= health or last.lives ~= lives
    or last.coins ~= coins or last.stars ~= stars or last.images ~= hudImages or last.gameOver ~= gameOver
  if changed or simTime - lastSentAt > SEND_INTERVAL then
    last = { visible = visible, health = health, wedges = wedges, lives = lives, coins = coins, stars = stars,
             gameOver = gameOver, images = hudImages }
    lastSentAt = simTime
    guihooks.trigger("NG64Hud", last)
  end
end

-- the game re-sends its layout on every game state change (menus, level loads): put Mario's back a frame later
function M.onGameStateUpdate()
  if layoutOn then reapplyLayout = true end
end

-- Mario is gone (despawned, level change): give the screen back to the game's layout
function M.deactivate()
  setLayout(false)
  dead = false
end

-- the HUD app just loaded: send the state on the next update instead of waiting for the periodic one
function M.resend() lastSentAt = -10 end

function M.setImages(ok) hudImages = ok and true or false end
function M.setHealer(fn) heal = fn end

function M.collectCoin(value)
  value = value or 1
  coins = math.min(999, coins + value)
  if heal then heal(4 * value) end          -- SM64: each coin's worth restores one wedge
end

function M.collectStar(count)
  stars = math.min(999, stars + (count or 1))
end

function M.addLife(count)
  lives = math.min(99, lives + (count or 1))
end

function M.getState()
  return { lives = lives, coins = coins, stars = stars, dead = dead, gameOver = gameOver, deaths = deaths,
           layout = layoutOn, images = hudImages }
end

-- tests: back to a new game
function M.reset()
  lives, coins, stars, dead, gameOver, deaths = LIVES_START, 0, 0, false, false, 0
end

return M
