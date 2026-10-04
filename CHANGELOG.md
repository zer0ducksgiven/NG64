# Changelog

## 0.1.4

**Fire**
- Mario catches fire from the flames of burning vehicles (a fuel leak that ignites, a wreck on fire), on the bodywork or low
  down near the ground. He plays SM64's burning pain animation, runs about in flames, and loses health as in SM64 (about
  three wedges per burn, and a burn can cost a life). Water puts him out.

**Water**
- Fixed Mario floating several metres above BeamNG water (Gridmap's pool, the lakes on West Coast USA): a water block's
  surface is at its own height, not the top of its box. Mario also no longer swims in the air under a lake's bed.

**Spin throw**
- Hold Y (Triangle, Switch X, or E on the keyboard) next to a car and Mario grabs it like Bowser's tail, holding it out in
  front of him at his gloves. Circle the stick to spin it round him (the keyboard winds up by itself), then let go to
  throw: the car flies far at an angle, tumbling, and the faster the spin the further it goes. Tapping Y still lifts and
  throws as before.
- The spinning car collides: if it hits another vehicle, a wall or the ground the spin ends and the car drops, and
  whatever it hit is dented and knocked away.
- On a wrecked car, holding Y spins its main body even when a loose wheel or panel is nearer (a tap still lifts the
  nearest part); the loose parts stay where they lie.

## 0.1.3

- DualSense (and other pads Windows lists as a "first person" device) weren't found by the DirectInput reader; now they are.
- The helper's log notes the first controller button presses with their DirectInput numbers, for `controller.ini`.

## 0.1.2

**Controllers**
- PlayStation (DualShock 4 / DualSense), Nintendo Switch Pro and generic DirectInput controllers now work, alongside
  Xbox pads. Buttons follow the Xbox layout by position (the bottom button jumps on every pad). A `controller.ini`
  next to the helper can remap any button or stick; see the README.

**Water**
- Mario swims in the water BeamNG draws (lakes, the sea) on every map, instead of walking through it as if it
  weren't there. BeamNG's rivers aren't covered yet.

**Music**
- Fixed the hitching and stuttering, which was worst on demanding maps. The audio thread now keeps a buffer ahead
  and runs at audio priority, so a busy game no longer starves it (63 dropouts in 40 s with every core busy, none now).
- Fixed the music getting stuck on one song after a few changes with the song selector.

**Vehicle selector and tray**
- Mario now has a picture in the vehicle selector, drawn from your ROM when you install (nothing is shipped).
- While the helper runs, a small Mario-head icon sits in the notification area. It's taken from your ROM too.

**Fixes**
- The uninstaller now removes everything it installed (it couldn't find BeamNG's mods folder before) and the picture
  and cache NG64 made.
- Looking for controllers no longer stalls the helper for ~150 ms every few seconds.

## 0.1.1

- The camera's up/down matches BeamNG's: push the right stick up to look up.
- BeamNG's interaction crosshair no longer appears when you move the camera as Mario.

## 0.1.0

First release: Mario in BeamNG.drive on libsm64, with SM64's camera, HUD and music read from your own ROM; the
installer; car pick-up and throw; traffic that sees Mario; BeamMP support.
