# Changelog

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
