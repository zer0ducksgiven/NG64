# NG64: Super Mario 64's Mario in BeamNG.drive

Play as Mario in BeamNG.drive. Spawn **NG64 → Mario** from the vehicle selector and you get Super Mario 64's own
movement, powered by [libsm64](https://github.com/libsm64/libsm64): running, jumping, long jumps, dives, punches,
kicks and ground pounds, on any BeamNG map, against real soft-body vehicles.

- Punch, kick, dive and ground-pound cars and watch them dent; land on a roof and it buckles.
- Pick up a car (or a piece that's fallen off one) and throw it, like King Bob-omb.
- Grab a whole car, spin it round you like Bowser's tail and hurl it: it flies far, tumbling, and anything the swing
  hits is dented and knocked away.
- Cars hurt Mario: get run over and he tumbles away, SM64-style.
- Fire hurts Mario too: touch the flames of a burning vehicle and he catches fire, SM64-style (jump into water to put it out).
- Walks on the map's real collision (buildings, ramps, rails, rocks), on any level, and swims in the water BeamNG draws.
- Super Mario 64's camera, its HUD (power meter, lives, coins, stars) and its music.
- Multiplayer through BeamMP.

> **Testing phase.** NG64 is being tested and changes often. Expect rough edges, and please report what you find.

**You need your own copy of Super Mario 64 (US version) as a ROM file.** NG64 contains nothing from the game:
Mario's model, animations, textures, HUD and music are all read from your ROM on your PC.

## Requirements

- Windows 10 or 11 (Linux through Wine/Proton works too, see below)
- BeamNG.drive (tested on 0.39)
- A Super Mario 64 **US** ROM. Any file name; `.z64`, `.v64` and `.n64` dumps all work
- A controller is recommended (Xbox, PlayStation, Switch Pro and most generic pads); keyboard works too

## Installation

NG64 comes as a zip containing an installer, plus the files for installing by hand. Close BeamNG.drive first.

### With the installer (recommended)

1. Unzip, and run **`NG64-Setup.exe`**. It doesn't need admin rights.
2. Choose your Super Mario 64 ROM. The installer checks it really is Super Mario 64, US version.
3. Check the BeamNG mods folder. It's found for you, so this is almost always right as it is.
4. Install, then start BeamNG.drive and spawn **NG64 → Mario** from the vehicle selector.

That's all. NG64 has a small helper program that runs Mario's physics next to the game (BeamNG mods can't run it
inside the game). The installer sets it up to run by itself:

- A hidden standby process starts with Windows and uses next to nothing while it waits.
- When BeamNG.drive starts, it starts the helper, with no window.
- The helper closes when the game does.

If the game ever says the helper isn't running, start **NG64** from the Start menu. To remove NG64, uninstall it
from Windows' **Installed apps**; that removes the helper, its copy of your ROM, the startup entry and the mod.

The installer puts the helper and a copy of your ROM in `%LOCALAPPDATA%\NG64`, and the mod (`ng64.zip`) in
BeamNG's mods folder, normally `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods`. If you've moved BeamNG's user
folder, it follows the game's `startup.ini`.

### By hand

1. Copy **`ng64.zip`** into BeamNG's mods folder (normally `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\mods`).
2. Put **`ng64helper.exe`** in a folder of its own, and your ROM next to it (any file name). Or write the ROM's
   full path into a `rom.txt` next to the helper.
3. Start `ng64helper.exe`, before or after the game. It has no window; it runs until the game closes, so start it
   again each time you play. For it to start by itself instead, run `ng64helper.exe --watch` (or use the
   installer): that waits in the background and starts the helper whenever BeamNG does.
4. In BeamNG, spawn **NG64 → Mario**.

### Multiplayer (BeamMP)

On the server, put `ng64.zip` in `Resources/Client/` and the `beammp_server_plugin` folder in
`Resources/Server/` as `NG64`. Every player needs NG64 installed (the installer, or the helper by hand).

### Linux (Wine / Proton)

The game runs under Proton; run the helper in the game's own Wine prefix, or Mario has no textures:

```bash
WINEPREFIX=~/.steam/steam/steamapps/compatdata/284160/pfx wine ng64helper.exe
```

`protontricks-launch --appid 284160 ng64helper.exe` does the same with Proton's own Wine. If the controller
doesn't respond, check Wine sees it (`wine control joy.cpl` with the same `WINEPREFIX`), and try turning Steam
Input off for BeamNG.

## Playing

| N64 | Controller (Xbox / PlayStation / Switch Pro) | Keyboard |
|---|---|---|
| Stick | Left stick | W A S D |
| A (jump) | A / Cross / Switch B (bottom button) | Space |
| B (punch / kick) | B or X / Circle or Square / Switch A or Y | J |
| Z (crouch / ground pound) | RT or LT / R2 or L2 / ZR or ZL | K |
| C buttons (camera) | Right stick | Arrow keys |
| Camera zoom | RB / LB / R1 / L1 / R / L | — |
| Pick up / throw | Y / Triangle / Switch X (top button) | E |
| Spin throw | Hold Y (Triangle / Switch X), circle the left stick, let go to throw | Hold E (it winds up by itself), let go |
| Music on / off | Back (View) / Share or Create / Minus, tap | M |
| Next / previous song | Hold Back + RB / LB | ] / [ |

Buttons are matched by position, so the bottom button always jumps whichever controller you have.

- Controls only work while the BeamNG window is in front.
- **Controllers:** Xbox pads work through XInput; PlayStation (DualShock 4 / DualSense), Switch Pro and generic
  DirectInput pads are read through DirectInput as well, and both can be plugged in at once. If a button of yours
  lands in the wrong place, put a `controller.ini` next to `ng64helper.exe` (`%LOCALAPPDATA%\NG64`):

  ```ini
  # button numbers start at 1, as in Windows' "Set up USB game controllers" dialog; 0 turns a button off
  layout = switch        # or playstation (the default for anything that isn't a Nintendo pad)
  a = 2
  b = 3
  x = 1
  y = 4
  lb = 5
  rb = 6
  lt = 7
  rt = 8
  back = 9
  # sticks: X Y Z RX RY RZ (a leading - flips the direction)
  rx = Z
  ry = -RZ
  ```

  The helper's log names each DirectInput pad it finds and the layout it chose.
- **Water:** Mario swims in lakes and the sea on any map (WaterBlock and WaterPlane water; BeamNG's rivers aren't
  covered yet).
- **The helper's tray icon:** while the helper is running, Mario's head from the lives counter sits in the
  notification area (Windows may keep it in the `^` overflow); hover it for "NG64 helper - running".
- While you're Mario, BeamNG's big map (normally Back / M) is switched off, so the music keys don't open it.
- **Music:** SM64's songs, from your ROM. All 34 can be cycled through; the current one shows on screen.
- **Spin throw:** tap Y for the ordinary lift; *hold* it next to a car and Mario takes it by one end like Bowser's
  tail. Circle the stick (any direction) to wind up the spin, up to SM64's fastest. He turns on the spot with the
  car swinging round him. Let go of Y to throw: the car goes the way it was moving, up at an angle and tumbling, and
  the faster the spin the further it goes (a ground-level throw is about 15 m; a full spin sends it 60 m or more). If
  the swinging car hits another vehicle, a wall or the ground, the spin ends and it drops; whatever it struck is
  dented and shoved. Wrecks' loose pieces can only be lifted, not spun. Holding Y without spinning for a few
  seconds puts the car down.
- **Traffic** sees Mario: AI cars slow down or steer round him instead of running him over.
- **Switching vehicles:** TAB to another vehicle leaves Mario standing where he is; switch back to play him again.
  Replacing him in the vehicle selector removes him.
- **HUD:** while you're Mario, the screen shows SM64's HUD. The power meter appears when he's hurt. You have 4
  lives; when his health runs out, respawn him with the vehicle reset (**R**). After the last life, it's game over
  and the next respawn starts at 4 again. Nothing in BeamNG hands out coins or stars yet (maps and mods can).
- **Reset (R)** puts Mario back on his feet at full health, where BeamNG's reset puts vehicles.

## Updates

The helper asks GitHub (api.github.com, a single request for this project's latest release) whether a newer NG64 has been
published, when it starts and every 12 hours, and shows a notice in the game if there is one. Nothing is sent but that
request. To turn it off, create an empty file called `no-update-check.txt` next to `ng64helper.exe`
(`%LOCALAPPDATA%\NG64`), or start the helper with `--no-update-check`.

## Troubleshooting

- **"The NG64 helper isn't running"**: start **NG64** from the Start menu (or `ng64helper.exe` by hand), or run
  the installer again.
- **Mario has no textures** (Linux/Wine): run the helper in the game's Wine prefix, as above.
- **The installer refuses the ROM**: it must be the 8 MB US version of Super Mario 64. European and Japanese
  versions aren't supported.
- **Logs**: `ng64helper.log` and `ng64watch.log` next to the helper (`%LOCALAPPDATA%\NG64` when installed), and
  BeamNG's own `beamng.log` in its user folder. Please include them when reporting a problem.

## Credits

- **[libsm64](https://github.com/libsm64/libsm64)** (CC0) by its contributors: Super Mario 64 as a library.
  NG64 is built on it; Mario's movement, animation, audio and everything else from SM64 runs through libsm64.
  NG64 adds a few small patches of its own (in `helper/patches/`).
- **[The SM64 decompilation project](https://github.com/n64decomp/sm64)**, whose reverse-engineered game code
  libsm64 is built from.
- **[sm64-san-andreas](https://github.com/headshot2017/sm64-san-andreas)** by headshot2017, which put libsm64's
  Mario into GTA San Andreas and was the inspiration and reference for this project.
- **[BeamNG.drive](https://www.beamng.com/)** by BeamNG GmbH, and **[BeamMP](https://beammp.com/)** for
  multiplayer.

## Legal

NG64 is a fan project. It isn't affiliated with, endorsed or sponsored by Nintendo or BeamNG GmbH. Super Mario 64
and Mario are trademarks of Nintendo.

No part of Super Mario 64 is included in NG64 or in this repository: no code, models, textures, animations or
audio. Everything Nintendo-made is read at runtime from the ROM you supply, which must be your own. Don't ask for
ROMs, and don't share them.

NG64's own code (the BeamNG mod, the helper, the installer and the BeamMP plugin) is released under the
[MIT License](LICENSE). libsm64 is CC0.

## For developers

How NG64 works, how to build it, and how it's tested: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
